/** @file
  UEFI Shell Dvol Commands Library Constructor.

  Registers the 'dvol' command with the UEFI Shell environment.

  Copyright (c) 2026. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiShellDvolCommandsLib.h"

/**
  Constructor for the Shell Dvol Commands library.

  Registers the "dvol" command handler.

  @param[in] ImageHandle    The image handle of the process.
  @param[in] SystemTable    The EFI System Table pointer.

  @retval EFI_SUCCESS       The command was registered successfully.
**/
EFI_STATUS
EFIAPI
ShellDvolCommandsLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  //
  // Register the 'dvol' command. 
  // Level 0 means it is available at any shell support level.
  //
  ShellCommandRegisterCommandName (
    L"dvol",
    ShellCommandRunDvol,
    NULL,   // No specific help file needed in this minimal setup
    0,      // Available at any level
    L"",
    TRUE,   // Can affect LastError
    NULL,   // No HII handle
    0       // No HII string ID
    );

  return EFI_SUCCESS;
}
