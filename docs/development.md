# Development

xwwPulse is C++20 for MSVC x64, with the Windows API only: no third-party libraries. The app draws with Direct2D and DirectWrite. Its colors, type scale, spacing, radii and motion are tokens in `src/ui/theme.h`: one dark, achromatic theme. Text is set in Geist and Geist Mono, which ship inside `xwwPulse.exe`, and icons are Lucide, stroked 1.5 wide.

## Requirements

- Windows 10 or 11.
- Visual Studio 2022 or later, or Build Tools, with the C++ workload (MSVC).
- A Windows SDK. When none is installed, `build.ps1` downloads `Microsoft.Windows.SDK.CPP` and `Microsoft.Windows.SDK.CPP.x64` from nuget.org and unpacks them into `.build\sdk`. Nothing is installed system-wide.

## Build

```powershell
.\build.ps1             # release build into dist\
.\build.ps1 -Debug      # no optimization, with PDBs
```

The result is `dist\xwwPulseService.exe`, `dist\xwwPulse.exe` and `dist\xwwPulse-Setup-<version>.exe`, which carries the other two, each with a `.sha256` file. Release builds are reproducible: the same sources give the same bytes.

`CMakeLists.txt` builds the service and the app for Visual Studio and other CMake front ends; the setup is built by `build.ps1` only.

## Check

```powershell
.\dist\xwwPulse.exe --render C:\temp\pulse          # every page in both themes, with sample data
.\dist\xwwPulseService.exe --etw-test 10            # processes presenting frames (administrator)
.\dist\xwwPulseService.exe --detect-only            # full detection without changes (administrator, needs the app running)
```

Stop the installed service first (`Stop-Service xwwPulse`): the ETW session and the pipe have fixed names.

`tools/present-probe.cpp` is a small D3D11 flip-model window that presents with vsync. Run it during `--etw-test` or `--detect-only` to check frame tracking on a machine without games.

<details>
<summary>Show how to build present-probe</summary>

From a Developer PowerShell:

```powershell
cl /nologo /std:c++20 /O2 /DUNICODE /D_UNICODE tools\present-probe.cpp /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib user32.lib shell32.lib
.\present-probe.exe 5 1
```

</details>

## Assets

| Script | Writes |
| --- | --- |
| `tools/Update-Icons.ps1` | `src/ui/icons.h` from Lucide; edit the list in the script |
| `tools/Update-Fonts.ps1` | `res/fonts/*.ttf` and `OFL.txt` from Google Fonts |

The logo (`branding/logo.png`), the wordmark (`branding/logotext.png`) and the app icon made from them (`branding/icon.png`, `res/xwwPulse.ico`) are finished artwork; replace the files to change them. README images in `.github/assets/readme` are generated from `branding/readme/readme.json`. Regenerate them when the version, the tagline or the README sections change.

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request:

1. PSScriptAnalyzer over every PowerShell script.
2. `build.ps1`, which compiles with `/W4`.
3. Every page rendered in both themes.
4. The ETW frame test.
5. The setup checked for the right version, then installed silently, the service checked, and removed again.

The finished `xwwPulse-Setup-<version>.exe` and its checksum are attached to every run as the `xwwPulse-Setup` artifact, so any commit can be installed.

## Release

1. Update `version.txt` and add a section for the version to `CHANGELOG.md`.
2. Commit, for example `build: [ゝ] release 1.1.0`.
3. Push a tag: `git tag v1.1.0` and `git push origin v1.1.0`.

CI builds and tests the tag, checks that it matches `version.txt`, and publishes *xwwPulse 1.1.0* with `xwwPulse-Setup-1.1.0.exe` and its `.sha256` file. The notes are the `CHANGELOG.md` section of that version.
