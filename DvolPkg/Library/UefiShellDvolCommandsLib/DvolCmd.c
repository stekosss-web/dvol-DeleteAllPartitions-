/** @file
  UEFI Shell command 'dvol': Delete ALL MBR/GPT partitions on disks.

  Copyright (c) 2026. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiShellDvolCommandsLib.h"

#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DevicePathToText.h>
#include <Protocol/DiskInfo.h>
#include <Guid/Gpt.h>
#include <Uefi/UefiGpt.h>
#include <Guid/FileInfo.h>
#include <Library/DevicePathLib.h>
#include <Library/PrintLib.h>
#include <Library/SafeIntLib.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define MBR_SIGNATURE_OFFSET    0x1FE
#define MBR_PARTITION_OFFSET    0x1BE
#define MBR_PARTITION_SIZE      16
#define MBR_PARTITION_COUNT     4
#define MBR_SIGNATURE           0xAA55
#define MAX_GPT_ENTRIES_SIZE    (1 * 1024 * 1024)
#define MAX_TARGET_DISKS        32
#define MAX_LOG_PATH            256

// ---------------------------------------------------------------------------
// MBR structures (packed)
// ---------------------------------------------------------------------------

#pragma pack(1)
typedef struct {
  UINT8 BootIndicator;
  UINT8 StartHead;
  UINT8 StartSector;
  UINT8 StartCylinder;
  UINT8 PartitionType;
  UINT8 EndHead;
  UINT8 EndSector;
  UINT8 EndCylinder;
  UINT32 StartingSector;
  UINT32 SectorCount;
} MBR_PARTITION_ENTRY;

typedef struct {
  UINT8 BootCode[446];
  MBR_PARTITION_ENTRY Partitions[MBR_PARTITION_COUNT];
  UINT16 Signature;
} MBR_TABLE;
#pragma pack()

// ---------------------------------------------------------------------------
// Types / Enums
// ---------------------------------------------------------------------------

typedef enum {
  DiskTypeUnknown,
  DiskTypeMbr,
  DiskTypeGpt
} DISK_TYPE;

typedef struct {
  UINTN   TargetArray[MAX_TARGET_DISKS];
  UINTN   TargetCount;
} TARGET_LIST;

// ---------------------------------------------------------------------------
// Per-disk scan info
// ---------------------------------------------------------------------------

typedef struct {
  EFI_BLOCK_IO_PROTOCOL   *BlockIo;
  EFI_DISK_IO_PROTOCOL    *DiskIo;
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;
  EFI_HANDLE              Handle;
  DISK_TYPE               DiskType;
  UINTN                   PartitionCount;
  UINTN                   DiskIndex;
  CHAR16                  DiskTypeStr[32];
  CHAR16                  DiskModelStr[64];
  CHAR16                  SizeStr[32];
  BOOLEAN                 WillProcess;
} DISK_SCAN_INFO;

// ---------------------------------------------------------------------------
// Global Log State
// ---------------------------------------------------------------------------

static SHELL_FILE_HANDLE  mLogFile = NULL;

STATIC
VOID
LogFileWrite (
  IN CONST VOID  *Buffer,
  IN UINTN       Size
  )
{
  EFI_STATUS  Status;
  UINTN       WriteSize;

  if ((mLogFile == NULL) || (Buffer == NULL) || (Size == 0)) {
    return;
  }

  WriteSize = Size;
  Status    = ShellWriteFile (mLogFile, &WriteSize, (VOID *)Buffer);
  if (EFI_ERROR (Status) || (WriteSize != Size)) {
    ShellCloseFile (&mLogFile);
  }
}

STATIC
VOID
LogWrite (
  IN CONST CHAR16 *Str
  )
{
  ShellPrintEx (-1, -1, L"%s", Str);
  LogFileWrite (Str, StrLen (Str) * sizeof (CHAR16));
}

STATIC
VOID
LogWriteCrLf (
  IN CONST CHAR16 *Str
  )
{
  CONST CHAR16  CrLf[] = L"\r\n";

  ShellPrintEx (-1, -1, L"%s\r\n", Str);
  LogFileWrite (Str, StrLen (Str) * sizeof (CHAR16));
  LogFileWrite (CrLf, StrLen (CrLf) * sizeof (CHAR16));
}

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

  ShellPrintEx (-1, -1, L"%s", Buffer);
  LogFileWrite (Buffer, StrLen (Buffer) * sizeof (CHAR16));
}

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

  Status = ShellOpenFileByName (
             Path,
             &mLogFile,
             EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
             0
             );
  if (!EFI_ERROR (Status)) {
    Bom = 0xFEFF;
    LogFileWrite (&Bom, sizeof (Bom));
  }

  return Status;
}

STATIC
VOID
CloseLogFile (
  VOID
  )
{
  if (mLogFile != NULL) {
    ShellFlushFile (mLogFile);
    ShellCloseFile (&mLogFile);
  }
}

// ---------------------------------------------------------------------------
// Helper Functions
// ---------------------------------------------------------------------------

STATIC
BOOLEAN
IsRemovableDevice (
  IN EFI_BLOCK_IO_PROTOCOL *BlockIo
  )
{
  return BlockIo->Media->RemovableMedia;
}

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

  Status = DiskIo->ReadDisk (DiskIo, BlockIo->Media->MediaId, 0, sizeof (Mbr), &Mbr);
  if (EFI_ERROR (Status)) {
    return DiskTypeUnknown;
  }

  if (Mbr.Signature == MBR_SIGNATURE) {
    if (Mbr.Partitions[0].PartitionType == 0xEE) {
      Status = ReadPrimaryGptHeader (DiskIo, BlockIo, &Header, &EntriesSize, &EntrySpanSize, &BackupEntryLba);
      return EFI_ERROR (Status) ? DiskTypeUnknown : DiskTypeGpt;
    }

    return DiskTypeMbr;
  }

  Status = ReadPrimaryGptHeader (DiskIo, BlockIo, &Header, &EntriesSize, &EntrySpanSize, &BackupEntryLba);
  if (!EFI_ERROR (Status)) {
    return DiskTypeGpt;
  }

  return DiskTypeUnknown;
}

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

  BufSize = 4096;
  Status = DiskInfo->Inquiry (DiskInfo, Buffer, &BufSize);
  if (EFI_ERROR (Status)) {
    BufSize = 4096;
    Status = DiskInfo->Identify (DiskInfo, Buffer, &BufSize);
  }

  if (!EFI_ERROR (Status) && BufSize > 0) {
    if (CompareGuid (&DiskInfo->Interface, &gEfiDiskInfoIdeInterfaceGuid) ||
        CompareGuid (&DiskInfo->Interface, &gEfiDiskInfoAhciInterfaceGuid))
    {
      if (BufSize >= 512) {
        UINT16 *IdData = (UINT16 *)Buffer;
        if (IdData[217] == 1)       { StrCpyS (OutType, 32, L"SATA SSD"); }
        else if (IdData[217] > 1)   { UnicodeSPrint (OutType, 40, L"SATA HDD (%u RPM)", IdData[217]); }
        else                        { StrCpyS (OutType, 32, L"SATA/AHCI Disk"); }

        CHAR8 *RawStr = AllocatePool (41);
        if (RawStr != NULL) {
          ZeroMem (RawStr, 41);
          CopyMem (RawStr, IdData + 27, 40);
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
          CopyMem (RawStr, Buffer + 4, 40);
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
          CopyMem (RawStr, Buffer + 16, 16);
          RawStr[16] = '\0';
          AsciiStrToUnicodeStrS (RawStr, OutModel, 64);
          FreePool (RawStr);
        }
      }
    }
  }

  FreePool (Buffer);
}

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

STATIC
EFI_STATUS
ClearMBR (
  IN EFI_DISK_IO_PROTOCOL  *DiskIo,
  IN EFI_BLOCK_IO_PROTOCOL *BlockIo
  );

// Wipe all GPT metadata so the disk is left fully unpartitioned.
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

  // Wipe the full first block so no MBR signature or protective entry remains.
  Status = ClearMBR (DiskIo, BlockIo);
  if (EFI_ERROR (Status)) { goto Done; }

  Status = DiskIo->WriteDisk (DiskIo, BlockIo->Media->MediaId, 1 * BlockSize, BlockSize, ZeroBuffer);
  if (EFI_ERROR (Status)) { goto Done; }

  Status = DiskIo->WriteDisk (
                     DiskIo,
                     BlockIo->Media->MediaId,
                     MultU64x32 (Header->PartitionEntryLBA, BlockIo->Media->BlockSize),
                     EntrySpanSize,
                     ZeroBuffer
                     );
  if (EFI_ERROR (Status)) { goto Done; }

  Status = DiskIo->WriteDisk (
                     DiskIo,
                     BlockIo->Media->MediaId,
                     MultU64x32 (BackupEntryLba, BlockIo->Media->BlockSize),
                     EntrySpanSize,
                     ZeroBuffer
                     );
  if (EFI_ERROR (Status)) { goto Done; }

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

  // Zero the entire first block rather than preserving a blank MBR shell.
  Status = DiskIo->WriteDisk (DiskIo, BlockIo->Media->MediaId, 0, BlockSize, ZeroBuffer);
  if (EFI_ERROR (Status)) { goto Done; }

  Status = BlockIo->FlushBlocks (BlockIo);

Done:
  FreePool (ZeroBuffer);
  return Status;
}

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

STATIC
BOOLEAN
IsTargeted (
  IN UINTN             Index,
  IN CONST TARGET_LIST *Targets
  )
{
  if (Targets->TargetCount == 0) {
    return TRUE;
  }

  for (UINTN i = 0; i < Targets->TargetCount; i++) {
    if (Targets->TargetArray[i] == Index) {
      return TRUE;
    }
  }

  return FALSE;
}

// ---------------------------------------------------------------------------
// CLI Parameter List
// ---------------------------------------------------------------------------

STATIC CONST SHELL_PARAM_ITEM DvolParamList[] = {
  { L"-y",          TypeFlag  },
  { L"--yes",       TypeFlag  },
  { L"-n",          TypeFlag  },
  { L"--dry-run",   TypeFlag  },
  { L"-r",          TypeFlag  },
  { L"--removable", TypeFlag  },
  { L"-d",          TypeValue },
  { L"--disk",      TypeValue },
  { L"-o",          TypeValue },
  { L"--output",    TypeValue },
  { L"-h",          TypeFlag  },
  { L"--help",      TypeFlag  },
  { NULL,           TypeMax   }
};

// ---------------------------------------------------------------------------
// Scan phase: collect info about all disks and partitions
// ---------------------------------------------------------------------------

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

    GetDiskHardwareInfo (Handles[i], Info[Count].DiskTypeStr, Info[Count].DiskModelStr);
    GetDiskSizeStr (BlockIo, Info[Count].SizeStr, sizeof (Info[Count].SizeStr));

    if (Info[Count].DiskType == DiskTypeMbr) {
      MBR_TABLE Mbr;
      Status = DiskIo->ReadDisk (DiskIo, BlockIo->Media->MediaId, 0, sizeof (Mbr), &Mbr);
      if (EFI_ERROR (Status)) {
        Count++;
        continue;
      }
      for (UINTN p = 0; p < MBR_PARTITION_COUNT; p++) {
        if (Mbr.Partitions[p].PartitionType != 0 && Mbr.Partitions[p].SectorCount != 0) {
          Info[Count].PartitionCount++;
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
// Apply phase: delete partitions on targeted disks
// ---------------------------------------------------------------------------
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

    if (!Disk->WillProcess) {
      continue;
    }
    if (Disk->PartitionCount == 0) {
      continue;
    }

    if (DryRun) {
      LogPrintf (L"[DRY RUN] Would delete %d partitions.\r\n", Disk->PartitionCount);
      if (Disk->DevicePath != NULL) {
        PrintDevicePath (Disk->DevicePath);
      }
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
// Main Command Function
// ---------------------------------------------------------------------------

STATIC
SHELL_STATUS
MainCmdDvol (
  IN EFI_HANDLE        ImageHandle,
  IN LIST_ENTRY        *Package
  )
{
  EFI_STATUS      Status;
  UINTN           ProcessedDisks;
  UINTN           MbrPartitionsDeleted;
  UINTN           GptPartitionsDeleted;
  UINTN           SkippedDisks;
  BOOLEAN         AutoConfirm;
  BOOLEAN         DryRun;
  BOOLEAN         IncRemovable;
  CONST CHAR16    *LogPath;
  TARGET_LIST     Targets;
  DISK_SCAN_INFO  *ScanInfo;
  UINTN           DiskCount;
  UINTN           ProcessCount;

  (VOID)ImageHandle;
  DiskCount = 0;
  ProcessedDisks     = 0;
  MbrPartitionsDeleted  = 0;
  GptPartitionsDeleted  = 0;
  SkippedDisks       = 0;
  AutoConfirm        = FALSE;
  DryRun             = FALSE;
  IncRemovable       = FALSE;
  Targets.TargetCount   = 0;
  LogPath            = ShellCommandLineGetValue (Package, L"-o");
  if (LogPath == NULL) {
    LogPath = ShellCommandLineGetValue (Package, L"--output");
  }

  if (ShellCommandLineGetFlag (Package, L"-y") || ShellCommandLineGetFlag (Package, L"--yes")) {
    AutoConfirm = TRUE;
  }
  if (ShellCommandLineGetFlag (Package, L"-n") || ShellCommandLineGetFlag (Package, L"--dry-run")) {
    DryRun = TRUE;
  }
  if (ShellCommandLineGetFlag (Package, L"-r") || ShellCommandLineGetFlag (Package, L"--removable")) {
    IncRemovable = TRUE;
  }
  if (ShellCommandLineGetFlag (Package, L"-h") || ShellCommandLineGetFlag (Package, L"--help")) {
    LogWriteCrLf (L"Usage: dvol [OPTIONS]");
    LogWriteCrLf (L"Options:");
    LogWriteCrLf (L"  -y, --yes              Skip confirmation prompt");
    LogWriteCrLf (L"  -n, --dry-run          Scan only, do NOT write to disk");
    LogWriteCrLf (L"  -r, --removable        Include removable media (USB, SD, etc.)");
    LogWriteCrLf (L"  -d <N>, --disk <N>     Target specific scan index");
    LogWriteCrLf (L"  -o <PATH>, --output <PATH>  Export log (e.g., fs0:\\log.txt)");
    LogWriteCrLf (L"  -h, --help             Show this help message and exit");
    return SHELL_SUCCESS;
  }

  {
    CONST CHAR16  *DiskVal;

    DiskVal = ShellCommandLineGetValue (Package, L"-d");
    if (DiskVal == NULL) {
      DiskVal = ShellCommandLineGetValue (Package, L"--disk");
    }

    if (DiskVal != NULL) {
      UINTN  DiskIndex;

      if (Targets.TargetCount >= MAX_TARGET_DISKS) {
        LogPrintf (L"ERROR: Too many -d arguments (max %u).\r\n", MAX_TARGET_DISKS);
        return SHELL_INVALID_PARAMETER;
      }

      Status = ParseDiskIndexArg (DiskVal, &DiskIndex);
      if (EFI_ERROR (Status)) {
        LogPrintf (L"ERROR: Invalid disk index '%s'.\r\n", DiskVal);
        return SHELL_INVALID_PARAMETER;
      }

      Targets.TargetArray[Targets.TargetCount++] = DiskIndex;
    }
  }

  // Open log file
  if (LogPath != NULL && StrLen (LogPath) > 0) {
    Status = OpenLogFile (LogPath);
    if (EFI_ERROR (Status)) {
      LogPrintf (L"WARNING: Failed to open log file %s (%r). Console-only output.\r\n", LogPath, Status);
    } else {
      LogPrintf (L"Log output enabled: %s\r\n", LogPath);
    }
  }

  // Phase 1: Scan all disks
  ScanInfo = ScanDisks (&DiskCount);
  if (ScanInfo == NULL || DiskCount == 0) {
    LogWriteCrLf (L"No block devices found.");
    CloseLogFile ();
    if (ScanInfo) { FreePool (ScanInfo); }
    return SHELL_DEVICE_ERROR;
  }

  LogPrintf (L"\nFound %d block device(s).\r\n\r\n", DiskCount);

  // Display info, applying filters
  for (UINTN i = 0; i < DiskCount; i++) {
    DISK_SCAN_INFO *Disk = &ScanInfo[i];

    // Apply removable filter
    if (IsRemovableDevice (Disk->BlockIo) && !IncRemovable) {
      Disk->WillProcess = FALSE;
    }

    // Apply target filter
    if (Disk->WillProcess && !IsTargeted (Disk->DiskIndex, &Targets)) {
      Disk->WillProcess = FALSE;
    }

    // Display disk info
    CHAR16 TypeLabel[16];
    if (Disk->DiskType == DiskTypeGpt) { StrCpyS (TypeLabel, 16, L"GPT"); }
    else if (Disk->DiskType == DiskTypeMbr) { StrCpyS (TypeLabel, 16, L"MBR"); }
    else { StrCpyS (TypeLabel, 16, L"???"); }

    LogPrintf (L"#[%d] %-4s | %s | %s | %s",
              Disk->DiskIndex, TypeLabel,
              Disk->DiskTypeStr, Disk->DiskModelStr, Disk->SizeStr);

    if (Disk->PartitionCount > 0) {
      LogPrintf (L" | %d partition(s)", Disk->PartitionCount);
    } else {
      LogPrintf (L" | Empty or no partitions");
      if (Disk->WillProcess) { Disk->WillProcess = FALSE; }
    }

    if (!Disk->WillProcess) {
      LogWrite (L" (skipped)");
    }

    LogWriteCrLf (L"");

    if (Disk->DevicePath != NULL) {
      PrintDevicePath (Disk->DevicePath);
    }
    LogWriteCrLf (L"");
  }

  // Count how many will be processed
  ProcessCount = 0;
  for (UINTN i = 0; i < DiskCount; i++) {
    if (ScanInfo[i].WillProcess) { ProcessCount++; }
  }

  if (ProcessCount == 0) {
    LogWriteCrLf (L"\r\nNo disks to process.");
    CloseLogFile ();
    FreePool (ScanInfo);
    return SHELL_SUCCESS;
  }

  // Confirmation prompt
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
        return SHELL_DEVICE_ERROR;
      }

      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);

      if (EFI_ERROR (Status) || (Key.UnicodeChar != L'Y' && Key.UnicodeChar != L'y')) {
        LogWriteCrLf (L"\r\nOperation cancelled.");
        CloseLogFile ();
        FreePool (ScanInfo);
        return SHELL_SUCCESS;
      }
      LogPrintf (L"%c\r\n", Key.UnicodeChar);
    }
  } else if (DryRun) {
    LogWriteCrLf (L"\r\n[DRY RUN MODE]");
  } else {
    LogWriteCrLf (L"\r\nAuto-confirmed. Proceeding...");
  }

  // Phase 2: Apply changes
  LogWriteCrLf (L"");
  ApplyDiskChanges (
    ScanInfo, DiskCount,
    DryRun,
    &ProcessedDisks, &SkippedDisks,
    &MbrPartitionsDeleted, &GptPartitionsDeleted
  );

  // Summary
  LogWriteCrLf (L"");
  LogWriteCrLf (L"========================================");
  LogWriteCrLf (L"OPERATION COMPLETE");
  LogWriteCrLf (L"========================================");
  LogPrintf (L"Disks processed: %d\r\n", ProcessedDisks);
  LogPrintf (L"Disks skipped: %d\r\n", SkippedDisks);
  LogPrintf (L"MBR partitions cleared: %d\r\n", MbrPartitionsDeleted);
  LogPrintf (L"GPT partitions cleared: %d\r\n", GptPartitionsDeleted);
  LogPrintf (L"Total cleared: %d\r\n", MbrPartitionsDeleted + GptPartitionsDeleted);
  if (DryRun) {
    LogWriteCrLf (L"[DRY RUN] No data was actually written.");
  } else {
    LogWriteCrLf (L"Please reboot your system to apply changes.");
  }

  FreePool (ScanInfo);
  CloseLogFile ();
  return SHELL_SUCCESS;
}

// ---------------------------------------------------------------------------
// ShellCommandRunDvol — Command entry point
// ---------------------------------------------------------------------------

SHELL_STATUS
EFIAPI
ShellCommandRunDvol (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS    Status;
  LIST_ENTRY    *Package;
  CHAR16        *ProblemParam;
  SHELL_STATUS  ShellStatus;

  ProblemParam = NULL;
  ShellStatus  = SHELL_SUCCESS;

  Status = ShellInitialize ();
  ASSERT_EFI_ERROR (Status);

  Status = ShellCommandLineParse (DvolParamList, &Package, &ProblemParam, TRUE);
  if (EFI_ERROR (Status)) {
    if ((Status == EFI_VOLUME_CORRUPTED) && (ProblemParam != NULL)) {
      ShellPrintEx (-1, -1, L"ERROR: Invalid parameter: %s\r\n", ProblemParam);
      FreePool (ProblemParam);
      ShellStatus = SHELL_INVALID_PARAMETER;
    } else {
      ShellStatus = SHELL_INVALID_PARAMETER;
    }
    return ShellStatus;
  }

  ShellStatus = MainCmdDvol (ImageHandle, Package);
  ShellCommandLineFreeVarList (Package);
  return ShellStatus;
}
