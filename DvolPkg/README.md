# DvolPkg

UEFI Shell command and standalone application for wiping MBR/GPT partitions.

## Overview

DvolPkg provides two ways to use the `dvol` utility:

1. **dvol.efi** — Standalone UEFI application. Copy to a FAT32 USB drive and run from the UEFI Shell.
2. **UefiShellDvolCommandsLib** — Library component. Add to your `ShellPkg` DSC to build the `dvol` command directly into `Shell.efi`.

## Features

- **Two-phase operation**: Scans and displays disk information first, then asks for confirmation before wiping.
- **MBR and GPT** partition table detection and erasure.
- **CLI Options**:
  - `-y, --yes` — Skip confirmation prompt (useful for scripting).
  - `-n, --dry-run` — Scan only; display info without writing to disk.
  - `-r, --removable` — Include removable media (USB, SD cards, etc.).
  - `-d <N>, --disk <N>` — Target specific disk(s) by scan index.
  - `-o <PATH>, --output <PATH>` — Export log to a FAT file (e.g., `fs0:\dvol.txt`).
  - `-h, --help` — Show help message.
- **Hardware info**: Displays disk type (SATA SSD/HDD, NVMe, USB/SCSI) and model where available.

## Building

### Standalone Application (dvol.efi)

```bash
build -p DvolPkg\DvolPkg.dsc -a X64 -t VS2022 -b RELEASE
```

Output: `Build/DvolPkg/RELEASE_VS2022/X64/DvolPkg/Application/DvolApp/DvolApp/OUTPUT/dvol.efi`

### Built-in Shell Command

To build `dvol` into `Shell.efi`, add the library to your `ShellPkg` DSC:

```ini
[Components]
      ...
      ShellPkg/Application/Shell/Shell.inf {
        <PcdsFixedAtBuild>
          gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
        <LibraryClasses>
          ### Оставляем как есть ###
          NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
          NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
          NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
          
          ### Добавляем вашу команду здесь ###
          NULL|DvolPkg/Library/UefiShellDvolCommandsLib/UefiShellDvolCommandsLib.inf
      }
	  
	  #
      # Build a second version of the shell with all commands integrated
      #
      ShellPkg/Application/Shell/Shell.inf {
       <Defines>
          FILE_GUID = EA4BB293-2D7F-4456-A681-1F22F42CD0BC
        <PcdsFixedAtBuild>
          gEfiShellPkgTokenSpaceGuid.PcdShellLibAutoInitialize|FALSE
        <LibraryClasses>
		  ### Оставляем как есть ###
          NULL|ShellPkg/Library/UefiShellLevel2CommandsLib/UefiShellLevel2CommandsLib.inf
          NULL|ShellPkg/Library/UefiShellLevel1CommandsLib/UefiShellLevel1CommandsLib.inf
          NULL|ShellPkg/Library/UefiShellLevel3CommandsLib/UefiShellLevel3CommandsLib.inf
		  
		  ### Добавляем вашу команду здесь ###
          NULL|DvolPkg/Library/UefiShellDvolCommandsLib/UefiShellDvolCommandsLib.inf
		}
```

## Usage

### Standalone Application

```
Shell> fs0:\dvol.efi
```

### Built-in Shell Command

```
Shell> dvol
```

## License

BSD-2-Clause-Patent

## Project Structure

```
DvolPkg/
├── DvolPkg.dec                    # Package declaration
├── DvolPkg.dsc                    # Platform description (builds dvol.efi)
├── Application/
│   └── DvolApp/
│       ├── DvolApp.inf            # Standalone application INF
│       └── DvolApp.c              # Standalone application source
├── Library/
│   └── UefiShellDvolCommandsLib/
│       ├── UefiShellDvolCommandsLib.inf  # Shell library INF
│       ├── UefiShellDvolCommandsLib.h    # Shell library header
│       ├── UefiShellDvolCommandsLib.c    # Constructor (registers command)
│       └── DvolCmd.c                     # Command implementation
└── Include/
    └── Guid/
        └── DvolPkgGuid.h          # Package GUID definition
```
