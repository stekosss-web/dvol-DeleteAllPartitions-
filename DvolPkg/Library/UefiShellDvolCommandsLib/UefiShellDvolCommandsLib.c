/** @file
  UEFI Shell Dvol Commands Library Constructor.

  Registers the 'dvol' command with the UEFI Shell environment.

  Copyright (c) 2026. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UefiShellDvolCommandsLib.h"

STATIC CONST CHAR16  mDvolManFileName[] = L"ShellCommands";
STATIC EFI_HII_HANDLE gDvolShellHiiHandle = NULL;

STATIC
CONST CHAR16 *
EFIAPI
ShellCommandGetManFileNameDvol (
  VOID
  )
{
  return mDvolManFileName;
}

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
  (VOID)SystemTable;

  gDvolShellHiiHandle = HiiAddPackages (&gDvolShellHiiGuid, ImageHandle, UefiShellDvolCommandsLibStrings, NULL);
  if (gDvolShellHiiHandle == NULL) {
    return EFI_DEVICE_ERROR;
  }

  //
  // Register the 'dvol' command. 
  // Level 0 means it is available at any shell support level.
  //
  ShellCommandRegisterCommandName (
    L"dvol",
    ShellCommandRunDvol,
    ShellCommandGetManFileNameDvol,
    0,      // Available at any level
    L"",
    TRUE,   // Can affect LastError
    gDvolShellHiiHandle,
    STRING_TOKEN (STR_GET_HELP_DVOL)
    );

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ShellDvolCommandsLibDestructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  (VOID)ImageHandle;
  (VOID)SystemTable;

  if (gDvolShellHiiHandle != NULL) {
    HiiRemovePackages (gDvolShellHiiHandle);
    gDvolShellHiiHandle = NULL;
  }

  return EFI_SUCCESS;
}
