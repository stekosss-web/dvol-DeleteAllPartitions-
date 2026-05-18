/** @file
  UEFI Application 'dvol': Utility for wiping all MBR/GPT partitions.

  This application operates in two phases:
  1. Scan Phase: Collects information about all block devices (type, model, size, partition count).
  2. Apply Phase: Displays a summary, prompts for user confirmation, and physically wipes the partition tables.

  Copyright (c) 2026. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/SafeIntLib.h>
// Protocols for disk and device manipulation
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DevicePathToText.h>
#include <Protocol/DiskInfo.h>
#include <Protocol/Shell.h>
#include <Protocol/ShellParameters.h>
// Standard EFI GUIDs and structures
#include <Guid/Gpt.h>
#include <Uefi/UefiGpt.h>
#include <Guid/FileInfo.h>
#include <Library/DevicePathLib.h>

// ---------------------------------------------------------------------------
// Constants: Limits and offsets for boot records
// ---------------------------------------------------------------------------

#define MBR_SIGNATURE_OFFSET    0x1FE       ///< Offset of the 0xAA55 magic number
#define MBR_PARTITION_OFFSET    0x1BE       ///< Offset of the 4-partition entry table
#define MBR_PARTITION_SIZE      16          ///< Size of a single MBR entry (bytes)
#define MBR_PARTITION_COUNT     4           ///< Maximum primary partitions in MBR
#define MBR_SIGNATURE           0xAA55      ///< Valid MBR signature
#define MAX_GPT_ENTRIES_SIZE    (1 * 1024 * 1024) ///< Maximum allowed size for GPT entry array (1 MB)
#define MAX_TARGET_DISKS        32          ///< Max number of disks selectable via -d flag
#define MAX_LOG_PATH            256         ///< Max length for the log file path

// ---------------------------------------------------------------------------
// MBR Structures: Packed structures for direct sector I/O
// ---------------------------------------------------------------------------

#pragma pack(1)
typedef struct {
  UINT8 BootIndicator;    ///< 0x80 = bootable, 0x00 = non-bootable
  UINT8 StartHead;
  UINT8 StartSector;
  UINT8 StartCylinder;
  UINT8 PartitionType;    ///< 0xEE indicates a protective MBR (GPT disk)
  UINT8 EndHead;
  UINT8 EndSector;
  UINT8 EndCylinder;
  UINT32 StartingSector;  ///< LBA of the first sector
  UINT32 SectorCount;     ///< Number of sectors in the partition
} MBR_PARTITION_ENTRY;

typedef struct {
  UINT8 BootCode[446];
  MBR_PARTITION_ENTRY Partitions[MBR_PARTITION_COUNT];
  UINT16 Signature;
} MBR_TABLE;
#pragma pack()

// ---------------------------------------------------------------------------
// Types / Enums: Internal enumerations and data structures
// ---------------------------------------------------------------------------

/**
  @enum DISK_TYPE
  @brief Detected partition table type on a disk.
*/
typedef enum {
  DiskTypeUnknown,
  DiskTypeMbr,
  DiskTypeGpt
} DISK_TYPE;

/**
  @struct TARGET_LIST
  @brief List of disk indices selected by the user via the -d flag.
*/
typedef struct {
  UINTN   TargetArray[MAX_TARGET_DISKS];
  UINTN   TargetCount;
} TARGET_LIST;

/**
  @struct DISK_SCAN_INFO
  @brief Cached information about a specific block device.
  Used to separate the scanning phase from the modification phase.
*/
typedef struct {
  EFI_BLOCK_IO_PROTOCOL   *BlockIo;
  EFI_DISK_IO_PROTOCOL    *DiskIo;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;
  EFI_HANDLE              Handle;
  DISK_TYPE               DiskType;
  UINTN                   PartitionCount;
  UINTN                   DiskIndex;       ///< Index shown in scan output
  CHAR16                  DiskTypeStr[32]; ///< Human-readable type (e.g., NVMe, SATA)
  CHAR16                  DiskModelStr[64];///< Device model string
  CHAR16                  SizeStr[32];     ///< Capacity string (GB/TB)
  BOOLEAN                 WillProcess;     ///< Flag: Whether this disk should be processed after filtering
} DISK_SCAN_INFO;

// ---------------------------------------------------------------------------
// Global Log State: Logging management
// ---------------------------------------------------------------------------

static SHELL_FILE_HANDLE   mLogFile = NULL; ///< Log file handle (NULL = console output only)
static EFI_SHELL_PROTOCOL  *mShell  = NULL; ///< Shell protocol for path-aware file I/O

/**
  Locates the shell protocol on demand.

  @retval EFI_SUCCESS  The shell protocol is available.
  @retval EFI_ERROR    The shell protocol is unavailable.
**/
STATIC
EFI_STATUS
EnsureShellProtocol (
  VOID
  )
{
  if (mShell != NULL) {
    return EFI_SUCCESS;
  }

  return gBS->LocateProtocol (&gEfiShellProtocolGuid, NULL, (VOID **)&mShell);
}

/**
  Writes a raw buffer to the log file if logging is enabled.

  @param[in] Buffer  Buffer to write.
  @param[in] Size    Buffer size in bytes.
**/
STATIC
VOID
LogFileWrite (
  IN CONST VOID  *Buffer,
  IN UINTN       Size
  )
{
  EFI_STATUS  Status;
  UINTN       WriteSize;

  if ((mLogFile == NULL) || (mShell == NULL) || (Buffer == NULL) || (Size == 0)) {
    return;
  }

  WriteSize = Size;
  Status    = mShell->WriteFile (mLogFile, &WriteSize, (VOID *)Buffer);
  if (EFI_ERROR (Status) || (WriteSize != Size)) {
    mShell->CloseFile (mLogFile);
    mLogFile = NULL;
  }
}

/**
  Prints a string without a newline. Duplicates output to the console and log file.
  @param[in] Str The string to print.
*/
STATIC
VOID
LogWrite (
  IN CONST CHAR16 *Str
  )
{
  Print (L"%s", Str);
  LogFileWrite (Str, StrLen (Str) * sizeof (CHAR16));
}

/**
  Prints a string followed by CRLF. Duplicates output to the console and log file.
  @param[in] Str The string to print.
*/
STATIC
VOID
LogWriteCrLf (
  IN CONST CHAR16 *Str
  )
{
  CONST CHAR16  CrLf[] = L"\r\n";

  Print (L"%s\r\n", Str);
  LogFileWrite (Str, StrLen (Str) * sizeof (CHAR16));
  LogFileWrite (CrLf, StrLen (CrLf) * sizeof (CHAR16));
}

/**
  Formatted printf-like output.
  @param[in] Format The format string.
  @param[in] ...    Variable arguments.
*/
STATIC
VOID
LogPrintf (
  IN CONST CHAR16 *Format,
  ...
  )
{
  CHAR16  Buffer[512];
  VA_LIST Args;

  VA_START (Args, Format);
  UnicodeVSPrint (Buffer, sizeof (Buffer), Format, Args);
  VA_END (Args);

  Print (L"%s", Buffer);
  LogFileWrite (Buffer, StrLen (Buffer) * sizeof (CHAR16));
}

/**
  Opens or creates a log file using shell path resolution.
  Existing files are truncated. A UTF-16 BOM is written to the start.
  @param[in] Path The path to the log file (e.g., L"fs0:\\dvol_log.txt").
  @retval EFI_SUCCESS File opened successfully.
  @retval EFI_ERROR   Failure to open or create.
*/
STATIC
EFI_STATUS
OpenLogFile (
  IN CONST CHAR16 *Path
  )
{
  EFI_STATUS  Status;
  CHAR16      Bom;

  if (StrLen (Path) == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Status = EnsureShellProtocol ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = mShell->CreateFile ((CHAR16 *)Path, 0, &mLogFile);
  if (!EFI_ERROR (Status)) {
    Bom = 0xFEFF;
    LogFileWrite (&Bom, sizeof (Bom));
  }

  return Status;
}

/// Closes the log file handle if it is open.
STATIC
VOID
CloseLogFile (
  VOID
  )
{
  if ((mLogFile != NULL) && (mShell != NULL)) {
    mShell->FlushFile (mLogFile);
    mShell->CloseFile (mLogFile);
    mLogFile = NULL;
  }
}

// ---------------------------------------------------------------------------
// Helper Functions: Disk detection and information retrieval
// ---------------------------------------------------------------------------

/// Returns TRUE if the device is marked as removable media.
STATIC
BOOLEAN
IsRemovableDevice (
  IN EFI_BLOCK_IO_PROTOCOL *BlockIo
  )
{
  return BlockIo->Media->RemovableMedia;
}

/// Returns TRUE for whole-disk block devices only.
STATIC
BOOLEAN
IsWholeDiskDevice (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo
  )
{
  if ((BlockIo == NULL) || (BlockIo->Media == NULL)) {
    return FALSE;
  }

  return (BOOLEAN)(
           !BlockIo->Media->LogicalPartition &&
           BlockIo->Media->MediaPresent &&
           (BlockIo->Media->BlockSize >= sizeof (MBR_TABLE))
           );
}

/**
  Validates a primary GPT header and computes the on-disk layout of the entry arrays.

  @param[in]  BlockIo           Block I/O interface.
  @param[in]  Header            GPT header read from LBA 1.
  @param[out] EntriesSize       Exact size of the GPT entry array in bytes.
  @param[out] EntrySpanSize     Entry array span rounded up to full blocks.
  @param[out] BackupEntryLba    First LBA of the backup GPT entry array.

  @retval TRUE   Header/layout is valid enough for scanning or wiping.
  @retval FALSE  Header is malformed or unsupported.
**/
STATIC
BOOLEAN
GetGptEntryLayout (
  IN  EFI_BLOCK_IO_PROTOCOL            *BlockIo,
  IN  CONST EFI_PARTITION_TABLE_HEADER *Header,
  OUT UINTN                            *EntriesSize,
  OUT UINTN                            *EntrySpanSize,
  OUT EFI_LBA                          *BackupEntryLba
  )
{
  RETURN_STATUS  SafeStatus;
  UINTN          BlockSize;
  UINTN          RoundedSize;
  EFI_LBA        EntryBlocks;

  if ((BlockIo == NULL) || (BlockIo->Media == NULL) || (Header == NULL) ||
      (EntriesSize == NULL) || (EntrySpanSize == NULL) || (BackupEntryLba == NULL))
  {
    return FALSE;
  }

  BlockSize = (UINTN)BlockIo->Media->BlockSize;
  if ((Header->Header.Signature != EFI_PTAB_HEADER_ID) ||
      (Header->MyLBA != PRIMARY_PART_HEADER_LBA) ||
      (Header->Header.HeaderSize < sizeof (EFI_PARTITION_TABLE_HEADER)) ||
      (Header->Header.HeaderSize > BlockSize) ||
      (Header->FirstUsableLBA > Header->LastUsableLBA) ||
      (Header->PartitionEntryLBA < 2) ||
      (Header->PartitionEntryLBA > BlockIo->Media->LastBlock) ||
      (Header->AlternateLBA == 0) ||
      (Header->AlternateLBA > BlockIo->Media->LastBlock) ||
      (Header->NumberOfPartitionEntries == 0) ||
      (Header->SizeOfPartitionEntry < sizeof (EFI_PARTITION_ENTRY)) ||
      ((Header->SizeOfPartitionEntry % sizeof (EFI_PARTITION_ENTRY)) != 0))
  {
    return FALSE;
  }

  SafeStatus = SafeUintnMult (
                 (UINTN)Header->NumberOfPartitionEntries,
                 (UINTN)Header->SizeOfPartitionEntry,
                 EntriesSize
                 );
  if (RETURN_ERROR (SafeStatus) || (*EntriesSize == 0) || (*EntriesSize > MAX_GPT_ENTRIES_SIZE)) {
    return FALSE;
  }

  SafeStatus = SafeUintnAdd (*EntriesSize, BlockSize - 1, &RoundedSize);
  if (RETURN_ERROR (SafeStatus)) {
    return FALSE;
  }

  EntryBlocks = (EFI_LBA)(RoundedSize / BlockSize);
  if ((EntryBlocks == 0) ||
      ((EntryBlocks - 1) > BlockIo->Media->LastBlock) ||
      (Header->PartitionEntryLBA > (BlockIo->Media->LastBlock - (EntryBlocks - 1))) ||
      (Header->AlternateLBA <= EntryBlocks))
  {
    return FALSE;
  }

  SafeStatus = SafeUintnMult ((UINTN)EntryBlocks, BlockSize, EntrySpanSize);
  if (RETURN_ERROR (SafeStatus) || (*EntrySpanSize < *EntriesSize)) {
    return FALSE;
  }

  *BackupEntryLba = Header->AlternateLBA - EntryBlocks;
  return TRUE;
}

/**
  Reads and validates the primary GPT header from LBA 1.

  @param[in]  DiskIo            Disk I/O interface.
  @param[in]  BlockIo           Block I/O interface.
  @param[out] Header            GPT header buffer.
  @param[out] EntriesSize       Exact size of the GPT entry array in bytes.
  @param[out] EntrySpanSize     Entry array span rounded up to full blocks.
  @param[out] BackupEntryLba    First LBA of the backup GPT entry array.

  @retval EFI_SUCCESS           GPT header was read and validated.
  @retval EFI_ERROR             Read failed or the header is malformed.
**/
STATIC
EFI_STATUS
ReadPrimaryGptHeader (
  IN  EFI_DISK_IO_PROTOCOL         *DiskIo,
  IN  EFI_BLOCK_IO_PROTOCOL        *BlockIo,
  OUT EFI_PARTITION_TABLE_HEADER   *Header,
  OUT UINTN                        *EntriesSize,
  OUT UINTN                        *EntrySpanSize,
  OUT EFI_LBA                      *BackupEntryLba
  )
{
  EFI_STATUS  Status;

  if ((DiskIo == NULL) || (BlockIo == NULL) || (Header == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = DiskIo->ReadDisk (
                     DiskIo,
                     BlockIo->Media->MediaId,
                     MultU64x32 (PRIMARY_PART_HEADER_LBA, BlockIo->Media->BlockSize),
                     sizeof (*Header),
                     Header
                     );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (!GetGptEntryLayout (BlockIo, Header, EntriesSize, EntrySpanSize, BackupEntryLba)) {
    return EFI_VOLUME_CORRUPTED;
  }

  return EFI_SUCCESS;
}

/// Counts populated GPT entries while honoring the actual entry stride from the header.
STATIC
UINTN
CountGptPartitions (
  IN CONST EFI_PARTITION_TABLE_HEADER  *Header,
  IN CONST VOID                        *Entries
  )
{
  UINTN  PartitionCount;

  PartitionCount = 0;
  for (UINTN Index = 0; Index < Header->NumberOfPartitionEntries; Index++) {
    CONST EFI_PARTITION_ENTRY  *Entry;

    Entry = (CONST EFI_PARTITION_ENTRY *)((CONST UINT8 *)Entries + (Index * Header->SizeOfPartitionEntry));
    if (!IsZeroGuid (&Entry->PartitionTypeGUID)) {
      PartitionCount++;
    }
  }

  return PartitionCount;
}

/**
  Parses a disk index from the command line and rejects partially valid strings.

  @param[in]  Value      Unicode decimal string.
  @param[out] DiskIndex  Parsed disk index.

  @retval EFI_SUCCESS            The value was parsed successfully.
  @retval EFI_INVALID_PARAMETER  The string is not a pure decimal number.
**/
STATIC
EFI_STATUS
ParseDiskIndexArg (
  IN  CONST CHAR16  *Value,
  OUT UINTN         *DiskIndex
  )
{
  RETURN_STATUS  ParseStatus;
  CHAR16         *EndPointer;

  if ((Value == NULL) || (Value[0] == L'\0') || (DiskIndex == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  EndPointer = NULL;
  ParseStatus = StrDecimalToUintnS (Value, &EndPointer, DiskIndex);
  if (RETURN_ERROR (ParseStatus) || (EndPointer == Value) || (*EndPointer != L'\0')) {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Determines the partition table type (MBR or GPT) by inspecting the first few sectors.
  @param[in] BlockIo Block I/O interface.
  @param[in] DiskIo  Disk I/O interface.
  @retval DiskTypeGpt If protective partition 0xEE is found or a valid GPT header exists.
  @retval DiskTypeMbr If a valid MBR signature exists without 0xEE.
  @retval DiskTypeUnknown If read fails or format is unrecognized.
*/
STATIC
DISK_TYPE
DetectDiskType (
  IN EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN EFI_DISK_IO_PROTOCOL   *DiskIo
  )
{
  EFI_STATUS                  Status;
  MBR_TABLE                   Mbr;
  EFI_PARTITION_TABLE_HEADER  Header;
  UINTN                       EntriesSize;
  UINTN                       EntrySpanSize;
  EFI_LBA                     BackupEntryLba;

  if (!IsWholeDiskDevice (BlockIo)) {
    return DiskTypeUnknown;
  }

  // Read the first sector
  Status = DiskIo->ReadDisk (DiskIo, BlockIo->Media->MediaId, 0, sizeof (Mbr), &Mbr);
  if (EFI_ERROR (Status)) {
    return DiskTypeUnknown;
  }

  // Check for 0xAA55 signature
  if (Mbr.Signature == MBR_SIGNATURE) {
    if (Mbr.Partitions[0].PartitionType == 0xEE) {
      Status = ReadPrimaryGptHeader (DiskIo, BlockIo, &Header, &EntriesSize, &EntrySpanSize, &BackupEntryLba);
      return EFI_ERROR (Status) ? DiskTypeUnknown : DiskTypeGpt;
    }

    return DiskTypeMbr;
  }

  // If LBA 0 is not a valid MBR, still check whether a primary GPT header exists at LBA 1.
  Status = ReadPrimaryGptHeader (DiskIo, BlockIo, &Header, &EntriesSize, &EntrySpanSize, &BackupEntryLba);
  if (!EFI_ERROR (Status)) {
    return DiskTypeGpt;
  }

  return DiskTypeUnknown;
}

/**
  Retrieves the interface type and model string of the device via EFI_DISK_INFO_PROTOCOL.
  Parses ATA IDENTIFY or SCSI INQUIRY data to get human-readable strings.
*/
STATIC
VOID
GetDiskHardwareInfo (
  IN  EFI_HANDLE Handle,
  OUT CHAR16     *OutType,
  OUT CHAR16     *OutModel
  )
{
  EFI_STATUS                Status;
  EFI_DISK_INFO_PROTOCOL    *DiskInfo;
  UINT8                     *Buffer;
  UINT32                    BufSize;
  EFI_DEVICE_PATH_PROTOCOL  *DevPath;

  StrCpyS (OutType, 32, L"Disk");
  StrCpyS (OutModel, 64, L"N/A");

  DiskInfo = NULL;
  Status = gBS->HandleProtocol (Handle, &gEfiDiskInfoProtocolGuid, (VOID **)&DiskInfo);
  if (EFI_ERROR (Status) || DiskInfo == NULL) {
    // Fallback: Print DevicePath if DiskInfo is unavailable
    DevPath = NULL;
    Status = gBS->HandleProtocol (Handle, &gEfiDevicePathProtocolGuid, (VOID **)&DevPath);
    if (!EFI_ERROR (Status) && DevPath != NULL) {
      EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *DevPathToText;
      DevPathToText = NULL;
      Status = gBS->LocateProtocol (&gEfiDevicePathToTextProtocolGuid, NULL, (VOID **)&DevPathToText);
      if (!EFI_ERROR (Status) && DevPathToText != NULL) {
        CHAR16 *TextPath = DevPathToText->ConvertDevicePathToText (DevPath, FALSE, TRUE);
        if (TextPath != NULL) {
          StrnCpyS (OutModel, 64, TextPath, 50);
          OutModel[50] = L'\0';
          if (TextPath[50] != L'\0') { StrCatS (OutModel, 64, L"..."); }
          FreePool (TextPath);
        }
      }
    }
    return;
  }

  Buffer = AllocatePool (4096);
  if (Buffer == NULL) {
    StrCpyS (OutModel, 64, L"Memory Error");
    return;
  }

  // Request data: SCSI INQUIRY or ATA IDENTIFY
  BufSize = 4096;
  Status = DiskInfo->Inquiry (DiskInfo, Buffer, &BufSize);
  if (EFI_ERROR (Status)) {
    BufSize = 4096;
    Status = DiskInfo->Identify (DiskInfo, Buffer, &BufSize);
  }

  // Parse the byte buffer based on interface type
  if (!EFI_ERROR (Status) && BufSize > 0) {
    if (CompareGuid (&DiskInfo->Interface, &gEfiDiskInfoIdeInterfaceGuid) ||
        CompareGuid (&DiskInfo->Interface, &gEfiDiskInfoAhciInterfaceGuid))
    {
      if (BufSize >= 512) {
        UINT16 *IdData = (UINT16 *)Buffer;
        // Word 217: Nominal Media Rotation Rate. 1 = SSD, >1 = HDD RPM
        if (IdData[217] == 1)       { StrCpyS (OutType, 32, L"SATA SSD"); }
        else if (IdData[217] > 1)   { UnicodeSPrint (OutType, 40, L"SATA HDD (%u RPM)", IdData[217]); }
        else                        { StrCpyS (OutType, 32, L"SATA/AHCI Disk"); }

        // ATA model strings are stored in reverse byte order (Little-Endian word-swapped)
        CHAR8 *RawStr = AllocatePool (41);
        if (RawStr != NULL) {
          ZeroMem (RawStr, 41);
          CopyMem (RawStr, IdData + 27, 40); // Words 27-46 = Model
          for (UINTN i = 0; i < 40; i += 2) {
            CHAR8 Tmp = RawStr[i]; RawStr[i] = RawStr[i+1]; RawStr[i+1] = Tmp;
          }
          RawStr[40] = '\0';
          AsciiStrToUnicodeStrS (RawStr, OutModel, 64);
          FreePool (RawStr);
        }
      }
    }
    else if (CompareGuid (&DiskInfo->Interface, &gEfiDiskInfoNvmeInterfaceGuid))
    {
      StrCpyS (OutType, 32, L"NVMe SSD");
      if (BufSize >= 4096) {
        CHAR8 *RawStr = AllocatePool (41);
        if (RawStr != NULL) {
          ZeroMem (RawStr, 41);
          CopyMem (RawStr, Buffer + 4, 40); // NVMe Model starts at offset 4
          RawStr[40] = '\0';
          AsciiStrToUnicodeStrS (RawStr, OutModel, 64);
          FreePool (RawStr);
        }
      }
    }
    else if (CompareGuid (&DiskInfo->Interface, &gEfiDiskInfoScsiInterfaceGuid) ||
             CompareGuid (&DiskInfo->Interface, &gEfiDiskInfoUsbInterfaceGuid))
    {
      StrCpyS (OutType, 32, L"USB/SCSI Device");
      if (BufSize >= 36) {
        CHAR8 *RawStr = AllocatePool (17);
        if (RawStr != NULL) {
          ZeroMem (RawStr, 17);
          CopyMem (RawStr, Buffer + 16, 16); // SCSI Product ID
          RawStr[16] = '\0';
          AsciiStrToUnicodeStrS (RawStr, OutModel, 64);
          FreePool (RawStr);
        }
      }
    }
  }
  FreePool (Buffer);
}

/// Formats the disk size into a human-readable string ("X GB" or "X TB").
STATIC
VOID
GetDiskSizeStr (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  OUT CHAR16                 *SizeBuf,
  IN  UINTN                  SizeBufSize
  )
{
  UINT64 Bytes = (UINT64)(BlockIo->Media->LastBlock + 1) * (UINT64)BlockIo->Media->BlockSize;
  if (Bytes >= 1024ULL * 1024 * 1024 * 1024) {
    UnicodeSPrint (SizeBuf, SizeBufSize, L"%ld TB", (UINT64)(Bytes / (1024ULL * 1024 * 1024 * 1024)));
  } else {
    UnicodeSPrint (SizeBuf, SizeBufSize, L"%ld GB", (UINT64)(Bytes / (1024ULL * 1024 * 1024)));
  }
}

// ---------------------------------------------------------------------------
// Partition Wiping: Physical deletion of partition tables
// ---------------------------------------------------------------------------

STATIC
EFI_STATUS
ClearMBR (
  IN EFI_DISK_IO_PROTOCOL  *DiskIo,
  IN EFI_BLOCK_IO_PROTOCOL *BlockIo
  );

/**
  Wipes GPT structures: full LBA 0, primary header, primary entries, backup entries,
  and backup header.
 */
STATIC
EFI_STATUS
ClearGPT (
  IN EFI_DISK_IO_PROTOCOL        *DiskIo,
  IN EFI_BLOCK_IO_PROTOCOL       *BlockIo,
  IN EFI_PARTITION_TABLE_HEADER  *Header,
  IN UINTN                       EntrySpanSize,
  IN EFI_LBA                     BackupEntryLba
  )
{
  EFI_STATUS  Status;
  UINTN       BlockSize;
  UINT8       *ZeroBuffer;

  BlockSize   = (UINTN)BlockIo->Media->BlockSize;

  ZeroBuffer = AllocateZeroPool (EntrySpanSize);
  if (ZeroBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  // 1. Wipe the entire first block so no MBR signature or protective entry remains.
  Status = ClearMBR (DiskIo, BlockIo);
  if (EFI_ERROR (Status)) { goto Done; }

  // 2. Wipe LBA 1 (primary GPT header)
  Status = DiskIo->WriteDisk (DiskIo, BlockIo->Media->MediaId, 1 * BlockSize, BlockSize, ZeroBuffer);
  if (EFI_ERROR (Status)) { goto Done; }

  // 3. Wipe the primary partition entry array.
  Status = DiskIo->WriteDisk (
                     DiskIo,
                     BlockIo->Media->MediaId,
                     MultU64x32 (Header->PartitionEntryLBA, BlockIo->Media->BlockSize),
                     EntrySpanSize,
                     ZeroBuffer
                     );
  if (EFI_ERROR (Status)) { goto Done; }

  // 4. Wipe the backup partition entry array immediately before the alternate header.
  Status = DiskIo->WriteDisk (
                     DiskIo,
                     BlockIo->Media->MediaId,
                     MultU64x32 (BackupEntryLba, BlockIo->Media->BlockSize),
                     EntrySpanSize,
                     ZeroBuffer
                     );
  if (EFI_ERROR (Status)) { goto Done; }

  // 5. Wipe the backup GPT header at the very last sector.
  Status = DiskIo->WriteDisk (
                     DiskIo,
                     BlockIo->Media->MediaId,
                     MultU64x32 (Header->AlternateLBA, BlockIo->Media->BlockSize),
                     BlockSize,
                     ZeroBuffer
                     );
  if (EFI_ERROR (Status)) { goto Done; }

  Status = BlockIo->FlushBlocks (BlockIo);

Done:
  FreePool (ZeroBuffer);
  return Status;
}

/**
  Wipes the entire LBA 0 so the disk no longer advertises any MBR layout.
 */
STATIC
EFI_STATUS
ClearMBR (
  IN EFI_DISK_IO_PROTOCOL  *DiskIo,
  IN EFI_BLOCK_IO_PROTOCOL *BlockIo
  )
{
  EFI_STATUS  Status;
  UINTN       BlockSize;
  UINT8       *ZeroBuffer;

  if (!IsWholeDiskDevice (BlockIo)) {
    return EFI_INVALID_PARAMETER;
  }

  BlockSize  = (UINTN)BlockIo->Media->BlockSize;
  ZeroBuffer = AllocateZeroPool (BlockSize);
  if (ZeroBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = DiskIo->WriteDisk (DiskIo, BlockIo->Media->MediaId, 0, BlockSize, ZeroBuffer);
  if (EFI_ERROR (Status)) { goto Done; }

  Status = BlockIo->FlushBlocks (BlockIo);

Done:
  FreePool (ZeroBuffer);
  return Status;
}

/// Prints the human-readable text representation of a device's path.
STATIC
VOID
PrintDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL *DevicePath
  )
{
  EFI_STATUS                       Status;
  EFI_DEVICE_PATH_TO_TEXT_PROTOCOL *DevPathToText;

  if (DevicePath == NULL) { return; }

  DevPathToText = NULL;
  Status = gBS->LocateProtocol (&gEfiDevicePathToTextProtocolGuid, NULL, (VOID **)&DevPathToText);
  if (!EFI_ERROR (Status) && DevPathToText != NULL) {
    CHAR16 *TextPath = DevPathToText->ConvertDevicePathToText (DevicePath, FALSE, FALSE);
    if (TextPath != NULL) {
      LogPrintf (L"  Device Path: %s\r\n", TextPath);
      FreePool (TextPath);
    }
  }
}

/// Checks if a disk index matches the target list provided via the -d flag.
STATIC
BOOLEAN
IsTargeted (
  IN UINTN             Index,
  IN CONST TARGET_LIST *Targets
  )
{
  if (Targets->TargetCount == 0) {
    return TRUE; // If list is empty, all disks are targets
  }

  for (UINTN i = 0; i < Targets->TargetCount; i++) {
    if (Targets->TargetArray[i] == Index) {
      return TRUE;
    }
  }
  return FALSE;
}

// ---------------------------------------------------------------------------
// Phase 1: Scan & Cache
// ---------------------------------------------------------------------------

/**
  Scans all available BlockIo devices, caching their state.
  This allows showing the user the full picture before any modifications begin.
  @param[out] DiskCount The number of found disks.
  @return Pointer to the DISK_SCAN_INFO array or NULL on failure.
*/
STATIC
DISK_SCAN_INFO *
ScanDisks (
  OUT UINTN *DiskCount
  )
{
  EFI_STATUS      Status;
  EFI_HANDLE      *Handles;
  UINTN           HandleCount;
  DISK_SCAN_INFO  *Info;
  UINTN           Count;

  Handles = NULL;
  *DiskCount = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  Info = AllocateZeroPool (HandleCount * sizeof (DISK_SCAN_INFO));
  if (Info == NULL) {
    FreePool (Handles);
    return NULL;
  }

  Count = 0;
  for (UINTN i = 0; i < HandleCount; i++) {
    EFI_BLOCK_IO_PROTOCOL   *BlockIo;
    EFI_DISK_IO_PROTOCOL    *DiskIo;
    EFI_DEVICE_PATH_PROTOCOL *DevicePath;

    BlockIo = NULL;
    Status = gBS->HandleProtocol (Handles[i], &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    if (EFI_ERROR (Status) || BlockIo == NULL) { continue; }
    if (!IsWholeDiskDevice (BlockIo)) { continue; }

    DiskIo = NULL;
    Status = gBS->HandleProtocol (Handles[i], &gEfiDiskIoProtocolGuid, (VOID **)&DiskIo);
    if (EFI_ERROR (Status) || DiskIo == NULL) { continue; }

    DevicePath = NULL;
    gBS->HandleProtocol (Handles[i], &gEfiDevicePathProtocolGuid, (VOID **)&DevicePath);

    Info[Count].BlockIo    = BlockIo;
    Info[Count].DiskIo     = DiskIo;
    Info[Count].DevicePath = DevicePath;
    Info[Count].Handle     = Handles[i];
    Info[Count].DiskIndex  = Count;
    Info[Count].DiskType   = DetectDiskType (BlockIo, DiskIo);
    Info[Count].WillProcess = TRUE;

    // Populate human-readable fields
    GetDiskHardwareInfo (Handles[i], Info[Count].DiskTypeStr, Info[Count].DiskModelStr);
    GetDiskSizeStr (BlockIo, Info[Count].SizeStr, sizeof (Info[Count].SizeStr));

    // Count existing partitions
    if (Info[Count].DiskType == DiskTypeMbr) {
      MBR_TABLE Mbr;
      Status = DiskIo->ReadDisk (DiskIo, BlockIo->Media->MediaId, 0, sizeof (Mbr), &Mbr);
      if (!EFI_ERROR (Status)) {
        for (UINTN p = 0; p < MBR_PARTITION_COUNT; p++) {
          if (Mbr.Partitions[p].PartitionType != 0 && Mbr.Partitions[p].SectorCount != 0) {
            Info[Count].PartitionCount++;
          }
        }
      }
    } else if (Info[Count].DiskType == DiskTypeGpt) {
      EFI_PARTITION_TABLE_HEADER  Header;
      UINTN                       EntriesSize;
      UINTN                       EntrySpanSize;
      EFI_LBA                     BackupEntryLba;

      Status = ReadPrimaryGptHeader (
                 DiskIo,
                 BlockIo,
                 &Header,
                 &EntriesSize,
                 &EntrySpanSize,
                 &BackupEntryLba
                 );
      if (!EFI_ERROR (Status)) {
        VOID  *Entries;

        Entries = AllocatePool (EntriesSize);
        if (Entries != NULL) {
          Status = DiskIo->ReadDisk (
                             DiskIo,
                             BlockIo->Media->MediaId,
                             MultU64x32 (Header.PartitionEntryLBA, BlockIo->Media->BlockSize),
                             EntriesSize,
                             Entries
                             );
          if (!EFI_ERROR (Status)) {
            Info[Count].PartitionCount = CountGptPartitions (&Header, Entries);
          }

          FreePool (Entries);
        }
      }
    }
    Count++;
  }

  FreePool (Handles);
  *DiskCount = Count;
  return Info;
}

// ---------------------------------------------------------------------------
// Phase 2: Apply Changes
// ---------------------------------------------------------------------------

/**
  Performs the actual deletion or a Dry-Run simulation on filtered disks.
*/
STATIC
VOID
ApplyDiskChanges (
  IN DISK_SCAN_INFO  *Info,
  IN UINTN           DiskCount,
  IN BOOLEAN         DryRun,
  OUT UINTN          *Processed,
  OUT UINTN          *Skipped,
  OUT UINTN          *MbrDeleted,
  OUT UINTN          *GptDeleted
  )
{
  EFI_STATUS Status;

  *Processed = 0;
  *Skipped   = 0;
  *MbrDeleted = 0;
  *GptDeleted = 0;

  for (UINTN i = 0; i < DiskCount; i++) {
    DISK_SCAN_INFO *Disk = &Info[i];

    if (!Disk->WillProcess) { continue; }
    if (Disk->PartitionCount == 0) { continue; }

    if (DryRun) {
      LogPrintf (L"[DRY RUN] Would delete %d partitions.\r\n", Disk->PartitionCount);
      if (Disk->DevicePath != NULL) { PrintDevicePath (Disk->DevicePath); }
      LogWriteCrLf (L"");
      *Processed += 1;
      if (Disk->DiskType == DiskTypeMbr) { *MbrDeleted += Disk->PartitionCount; }
      else                               { *GptDeleted += Disk->PartitionCount; }
      continue;
    }

    LogWrite (L"  Deleting partitions... ");
    EFI_STATUS ClearStatus = EFI_SUCCESS;
    if (Disk->DiskType == DiskTypeMbr) {
      ClearStatus = ClearMBR (Disk->DiskIo, Disk->BlockIo);
    } else {
      EFI_PARTITION_TABLE_HEADER  Header;
      UINTN                       DummyEntriesSize;
      UINTN                       EntrySpanSize;
      EFI_LBA                     BackupEntryLba;

      Status = ReadPrimaryGptHeader (
                 Disk->DiskIo,
                 Disk->BlockIo,
                 &Header,
                 &DummyEntriesSize,
                 &EntrySpanSize,
                 &BackupEntryLba
                 );
      if (!EFI_ERROR (Status)) {
        ClearStatus = ClearGPT (Disk->DiskIo, Disk->BlockIo, &Header, EntrySpanSize, BackupEntryLba);
      } else {
        ClearStatus = Status;
      }
    }

    if (EFI_ERROR (ClearStatus)) {
      LogPrintf (L"FAILED (%r)\r\n", ClearStatus);
      *Skipped += 1;
    } else {
      LogWriteCrLf (L"SUCCESS");
      *Processed += 1;
      if (Disk->DiskType == DiskTypeMbr) { *MbrDeleted += Disk->PartitionCount; }
      else                               { *GptDeleted += Disk->PartitionCount; }
    }
    LogWriteCrLf (L"");
  }
}

// ---------------------------------------------------------------------------
// UEFI Application Entry Point
// ---------------------------------------------------------------------------

/**
  Main entry point for the UEFI Application.
  Execution flow:
  1. Initialization and CLI argument parsing.
  2. Phase 1: Scan (ScanDisks).
  3. Display information and apply filters (-r, -d).
  4. Prompt for confirmation (unless -y or -n is present).
  5. Phase 2: Apply (ApplyDiskChanges).
  6. Display summary and cleanup resources.
*/
EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                        Status;
  EFI_SHELL_PARAMETERS_PROTOCOL     *ShellParams;
  UINTN                             ProcessedDisks;
  UINTN                             MbrPartitionsDeleted;
  UINTN                             GptPartitionsDeleted;
  UINTN                             SkippedDisks;
  BOOLEAN                           AutoConfirm;
  BOOLEAN                           DryRun;
  BOOLEAN                           IncRemovable;
  CONST CHAR16                      *LogPath;
  TARGET_LIST                       Targets;
  DISK_SCAN_INFO                    *ScanInfo;
  UINTN                             DiskCount;
  UINTN                             ProcessCount;

  // Initialize variables
  DiskCount = 0;
  ProcessedDisks = 0;
  MbrPartitionsDeleted = 0;
  GptPartitionsDeleted = 0;
  SkippedDisks = 0;
  AutoConfirm = FALSE;
  DryRun = FALSE;
  IncRemovable = FALSE;
  Targets.TargetCount = 0;
  LogPath = NULL;

  // Retrieve command line arguments from the UEFI Shell
  Status = gBS->OpenProtocol (
                  ImageHandle,
                  &gEfiShellParametersProtocolGuid,
                  (VOID **)&ShellParams,
                  ImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    ShellParams = NULL;
  }

  // Parse flags manually to keep the application self-contained.
  if (ShellParams != NULL) {
    for (UINTN i = 1; i < ShellParams->Argc; i++) {
      if (StrCmp (ShellParams->Argv[i], L"-y") == 0 || StrCmp (ShellParams->Argv[i], L"--yes") == 0) {
        AutoConfirm = TRUE;
      } else if (StrCmp (ShellParams->Argv[i], L"-n") == 0 || StrCmp (ShellParams->Argv[i], L"--dry-run") == 0) {
        DryRun = TRUE;
      } else if (StrCmp (ShellParams->Argv[i], L"-r") == 0 || StrCmp (ShellParams->Argv[i], L"--removable") == 0) {
        IncRemovable = TRUE;
      } else if (StrCmp (ShellParams->Argv[i], L"-h") == 0 || StrCmp (ShellParams->Argv[i], L"--help") == 0) {
        LogWriteCrLf (L"Usage: DvolApp [OPTIONS]");
        LogWriteCrLf (L"Options:");
        LogWriteCrLf (L"  -y, --yes              Skip confirmation");
        LogWriteCrLf (L"  -n, --dry-run          Scan only");
        LogWriteCrLf (L"  -r, --removable        Include USB/SD cards");
        LogWriteCrLf (L"  -d <N>, --disk <N>     Target specific scan index");
        LogWriteCrLf (L"  -o <PATH>, --output    Export log (for example fs0:\\dvol_log.txt)");
        LogWriteCrLf (L"  -h, --help             Show help");
        return EFI_SUCCESS;
      } else if (StrCmp (ShellParams->Argv[i], L"-d") == 0 || StrCmp (ShellParams->Argv[i], L"--disk") == 0) {
        UINTN  DiskIndex;

        if (i + 1 >= ShellParams->Argc) {
          LogWriteCrLf (L"ERROR: Missing value for -d/--disk.");
          return EFI_INVALID_PARAMETER;
        }

        if (Targets.TargetCount >= MAX_TARGET_DISKS) {
          LogPrintf (L"ERROR: Too many -d arguments (max %u).\r\n", MAX_TARGET_DISKS);
          return EFI_INVALID_PARAMETER;
        }

        Status = ParseDiskIndexArg (ShellParams->Argv[++i], &DiskIndex);
        if (EFI_ERROR (Status)) {
          LogPrintf (L"ERROR: Invalid disk index '%s'.\r\n", ShellParams->Argv[i]);
          return EFI_INVALID_PARAMETER;
        }

        Targets.TargetArray[Targets.TargetCount++] = DiskIndex;
      } else if (StrCmp (ShellParams->Argv[i], L"-o") == 0 || StrCmp (ShellParams->Argv[i], L"--output") == 0) {
        if (i + 1 >= ShellParams->Argc) {
          LogWriteCrLf (L"ERROR: Missing value for -o/--output.");
          return EFI_INVALID_PARAMETER;
        }

        LogPath = ShellParams->Argv[++i];
      } else {
        LogPrintf (L"ERROR: Unknown option '%s'.\r\n", ShellParams->Argv[i]);
        return EFI_INVALID_PARAMETER;
      }
    }
  }

  // Open the log file if specified
  if (LogPath != NULL && StrLen (LogPath) > 0) {
    Status = OpenLogFile (LogPath);
    if (EFI_ERROR (Status)) {
      LogPrintf (L"WARNING: Failed to open log %s (%r)\r\n", LogPath, Status);
    } else {
      LogPrintf (L"Log output enabled: %s\r\n", LogPath);
    }
  }
  // Banner
  LogWriteCrLf (L"=================================");
  LogWriteCrLf (L"UEFI Disk Partitions Eraser v1.0");
  LogWriteCrLf (L"=================================");
  LogWriteCrLf (L"Start with -h for usage help");

  // --- Phase 1: Scanning ---
  ScanInfo = ScanDisks (&DiskCount);
  if (ScanInfo == NULL || DiskCount == 0) {
    LogWriteCrLf (L"No block devices found.");
    CloseLogFile ();
    if (ScanInfo) { FreePool (ScanInfo); }
    return EFI_DEVICE_ERROR;
  }

  LogPrintf (L"\nFound %d block device(s).\r\n\r\n", DiskCount);

  // Filtering and displaying information
  for (UINTN i = 0; i < DiskCount; i++) {
    DISK_SCAN_INFO *Disk = &ScanInfo[i];

    // Skip removable media unless -r is set
    if (IsRemovableDevice (Disk->BlockIo) && !IncRemovable) { Disk->WillProcess = FALSE; }
    // Skip disks that are not in the -d target list
    if (Disk->WillProcess && !IsTargeted (Disk->DiskIndex, &Targets)) { Disk->WillProcess = FALSE; }

    CHAR16 TypeLabel[16];
    if (Disk->DiskType == DiskTypeGpt)      { StrCpyS (TypeLabel, 16, L"GPT"); }
    else if (Disk->DiskType == DiskTypeMbr) { StrCpyS (TypeLabel, 16, L"MBR"); }
    else                                    { StrCpyS (TypeLabel, 16, L"???"); }

    LogPrintf (L"#[%d] %-4s | %s | %s | %s", Disk->DiskIndex, TypeLabel, Disk->DiskTypeStr, Disk->DiskModelStr, Disk->SizeStr);

    if (Disk->PartitionCount > 0) {
      LogPrintf (L" | %d partition(s)", Disk->PartitionCount);
    } else {
      LogPrintf (L" | Empty or no partitions");
      if (Disk->WillProcess) { Disk->WillProcess = FALSE; } // Nothing to wipe
    }

    if (!Disk->WillProcess) { LogWrite (L" (skipped)"); }
    LogWriteCrLf (L"");
    if (Disk->DevicePath != NULL) { PrintDevicePath (Disk->DevicePath); }
    LogWriteCrLf (L"");
  }

  // Count disks ready for processing
  ProcessCount = 0;
  for (UINTN i = 0; i < DiskCount; i++) {
    if (ScanInfo[i].WillProcess) { ProcessCount++; }
  }

  if (ProcessCount == 0) {
    LogWriteCrLf (L"\r\nNo disks to process.");
    CloseLogFile ();
    FreePool (ScanInfo);
    return EFI_SUCCESS;
  }

  // --- Confirmation Prompt ---
  if (!AutoConfirm && !DryRun) {
    LogWriteCrLf (L"\r\nWill permanently delete partition tables on the above disk(s).");
    LogWrite (L"Type 'Y' to proceed, or any other key to cancel: ");
    {
      EFI_EVENT   WaitList[1];
      EFI_INPUT_KEY Key;
      UINTN         EventIndex;

      WaitList[0] = gST->ConIn->WaitForKey;
      EventIndex  = 0;
      Status = gBS->WaitForEvent (1, WaitList, &EventIndex);
      if (EFI_ERROR (Status) || (EventIndex != 0)) {
        LogPrintf (L"\r\nFailed to read confirmation key (%r).\r\n", Status);
        CloseLogFile ();
        FreePool (ScanInfo);
        return Status;
      }

      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      if (EFI_ERROR (Status) || (Key.UnicodeChar != L'Y' && Key.UnicodeChar != L'y')) {
        LogWriteCrLf (L"\r\nOperation cancelled.");
        CloseLogFile ();
        FreePool (ScanInfo);
        return EFI_SUCCESS;
      }
      LogPrintf (L"%c\r\n", Key.UnicodeChar);
    }
  } else if (DryRun) {
    LogWriteCrLf (L"\r\n[DRY RUN MODE]");
  } else {
    LogWriteCrLf (L"\r\nAuto-confirmed. Proceeding...");
  }

  // --- Phase 2: Applying Changes ---
  LogWriteCrLf (L"");
  ApplyDiskChanges (
    ScanInfo, DiskCount,
    DryRun,
    &ProcessedDisks, &SkippedDisks,
    &MbrPartitionsDeleted, &GptPartitionsDeleted
  );

  // Final Statistics
  LogWriteCrLf (L"========================================");
  LogWriteCrLf (L"OPERATION COMPLETE");
  LogWriteCrLf (L"========================================");
  LogPrintf (L"Disks processed: %d\r\n", ProcessedDisks);
  LogPrintf (L"Disks skipped: %d\r\n", SkippedDisks);
  LogPrintf (L"MBR cleared: %d\r\n", MbrPartitionsDeleted);
  LogPrintf (L"GPT cleared: %d\r\n", GptPartitionsDeleted);
  LogPrintf (L"Total cleared: %d\r\n", MbrPartitionsDeleted + GptPartitionsDeleted);

  if (DryRun) { LogWriteCrLf (L"[DRY RUN] No data was actually written."); }
  else        { LogWriteCrLf (L"Please reboot your system to apply changes."); }

  FreePool (ScanInfo);
  CloseLogFile ();
  return EFI_SUCCESS;
}
