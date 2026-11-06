# Changelog

## 1.0.0

The first release of xwwPulse.

### Added

- **Game detection without polling.** Process starts and frame presents come from ETW, so xwwPulse reacts when a game starts and measures its frame rate without checking the process list every second.
- **Games recognized by their launcher.** Games installed through Steam, Epic Games, GOG, Ubisoft Connect, EA, Battle.net, Riot and Rockstar are recognized by their install folder, including custom folders on other drives. Games that Windows itself has matched on your PC count too.
- **Every graphics API.** Frames are counted from the kernel present event every API emits, so Vulkan and OpenGL games are confirmed as reliably as DirectX ones.
- **Patient with loading screens.** A possible game is watched for as long as it is in front, and its window is checked again for a minute after it appears, so a game that goes fullscreen late is still found. A game needs an average of 20 frames per second over two seconds, which suits games capped at 30 fps.
- **Optimizations that end with the game.** While a game is in front: no deep C-states, High CPU and GPU priority, the compositor on MMCSS, and background apps suspended or throttled. Each change is recorded first and undone when the game closes, on Alt+Tab, or on the next start after a crash.
- **CPU placement by topology.** On dual-CCD Ryzen the game runs on the 3D V-Cache CCD, on Intel hybrid CPUs on the P-cores, and background apps such as Discord, browsers and OBS move to the other cores. CPUs with one L3 and equal cores are left alone.
- **Anti-cheat aware.** When Easy Anti-Cheat, BattlEye, Vanguard, FACEIT or another known anti-cheat runs, xwwPulse does not touch the game's threads.
- **An app that explains itself.** The tray app shows each optimization and why it is on, paused or skipped, the live frame rate, and why each app in front was or was not treated as a game. `Pulse.log` records the same decisions.
- **Always running when needed.** Detection needs the app in your session; when it did not start with Windows or crashed, the service starts it after you sign in.
- **Lists you edit in the app.** *Add game* and the background lists offer the apps running right now, or any exe file. Default entries are marked, and anything you removed can be restored.
- **Your choice at sign-in.** Starting with Windows is a switch in Settings.
- **A quiet console look.** One dark, monochrome interface: the frame rate in large digits, state told with icons and words, and the keyboard working everywhere (Tab, Enter, Space, Esc, Ctrl+,).
- **One file to install.** `xwwPulse-Setup-1.0.0.exe` installs the service and the app to `C:\Program Files\xwwPulse`, adds a Start menu shortcut and an entry in *Settings > Apps*, and updates an installed copy in place. It removes everything again from Settings, `uninstall.exe` or *Settings > Apps*, optionally with your settings and logs. `/silent`, `/uninstall` and `/purge` are available for scripts.
