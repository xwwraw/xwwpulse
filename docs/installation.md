# Installation

xwwPulse has two programs that work together: `xwwPulseService.exe`, a Windows service running as SYSTEM, and `xwwPulse.exe`, the app in your tray. Both are needed: the service makes the changes, and the app tells it which window is in front. `xwwPulse-Setup-1.0.0.exe` carries both and installs them in one step.

## With the setup

1. Download `xwwPulse-Setup-<version>.exe`, for example `xwwPulse-Setup-1.0.0.exe`, from the [latest release](https://github.com/xwwraw/xwwpulse/releases/latest). Its SHA-256 checksum is in the `.sha256` file next to it.
2. Run it, confirm the administrator prompt, then choose *Install*.

The setup:

- copies both programs to `C:\Program Files\xwwPulse`;
- registers the service (automatic start, restarts after a failure) and starts it;
- creates the default profiles in `C:\ProgramData\xwwPulse` when they are missing;
- starts the app at sign-in for every user and opens it now;
- adds xwwPulse to the Start menu;
- adds xwwPulse to *Settings > Apps*, where it can be removed.

Running a newer setup over an installed xwwPulse updates it: the service is stopped, every change it still had applied is undone, the programs are replaced and the service starts again. Your profiles are kept. The setup finds the installed service wherever an earlier version put it and removes the old files from there.

It needs Windows 10 or 11, 64-bit.

| Switch | Effect |
| --- | --- |
| `/silent` | install or remove without dialogs |
| `/uninstall` | remove xwwPulse; `uninstall.exe` in the install folder does this without the switch |
| `/purge` | with a removal, also delete settings, logs and the rollback journal |

<details>
<summary>Show an unattended install and removal</summary>

From an elevated PowerShell:

```powershell
Start-Process .\xwwPulse-Setup-1.0.0.exe -ArgumentList '/silent' -Wait
Get-Service xwwPulse

Start-Process 'C:\Program Files\xwwPulse\uninstall.exe' -ArgumentList '/silent', '/purge' -Wait
```

</details>

## Checking the download

```powershell
(Get-FileHash .\xwwPulse-Setup-1.0.0.exe -Algorithm SHA256).Hash.ToLower()
Get-Content .\xwwPulse-Setup-1.0.0.exe.sha256
```

The two values must match. The setup is not code-signed, so Windows SmartScreen may ask you to confirm it the first time; choose *More info* and *Run anyway*.

## Removing it

Any of these removes xwwPulse:

- *Uninstall* in the app, under *Settings > Remove*;
- `C:\Program Files\xwwPulse\uninstall.exe`;
- *Settings > Apps > Installed apps > xwwPulse > Uninstall* in Windows.

The service is stopped, every change still active is undone, and the service, the sign-in entry, the programs, the Start menu shortcut and the Apps entry are removed. Tick *Also delete my settings and logs* to remove `C:\ProgramData\xwwPulse` and the registry keys under `HKLM\SOFTWARE\xwwPulse` and `HKCU\Software\xwwPulse` too; otherwise they stay for a later install.

## Command-line modes

<details>
<summary>Show every mode of xwwPulseService.exe</summary>

| Command | What it does |
| --- | --- |
| `--install` | register and start the service, start the app at sign-in |
| `--uninstall` | undo everything and remove the service |
| `--console` | run in this console instead of as a service; Ctrl+C stops it and rolls back |
| `--detect-only` | like `--console`, but only logs what it would do |
| `--etw-test [seconds]` | list the processes presenting frames, without changing anything |
| `--recover` | replay the rollback journal and exit |

</details>

All modes except the service itself need an elevated PowerShell. `--console`, `--detect-only` and `--etw-test` cannot run while the installed service is running: stop it first with `Stop-Service xwwPulse`.
