/** @file
  Header file for the UEFI Shell Dvol Commands Library.

  Copyright (c) 2026. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef __UEFI_SHELL_DVOL_COMMANDS_LIB_H__
#define __UEFI_SHELL_DVOL_COMMANDS_LIB_H__

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/ShellCommandLib.h>
#include <Library/ShellLib.h>

#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DevicePathToText.h>
#include <Protocol/DiskInfo.h>
#include <Guid/Gpt.h>
#include <Uefi/UefiGpt.h>
#include <Guid/FileInfo.h>
#include <Library/DevicePathLib.h>

SHELL_STATUS
EFIAPI
ShellCommandRunDvol (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

#endif
