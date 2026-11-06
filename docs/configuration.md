# Configuration

Both profiles live in `C:\ProgramData\xwwPulse`. The service watches the folder and reloads them when either file changes; no restart is needed. The app edits both through the service, so you never need to open them; editing them by hand needs administrator rights.

## config.toml

The *Optimizations* page of the app writes this file. You can also edit it by hand; comments you add are not kept.

| Key | Default | Meaning |
| --- | --- | --- |
| `general.enabled` | `true` | game mode on or off |
| `general.pause_on_alt_tab` | `true` | undo power, background and compositor changes while the game is not in front |
| `general.notifications` | `true` | a Windows notification when game mode turns on |
| `general.start_with_windows` | `true` | start the app at sign-in for every user; the service also starts it when it is missing. Off means you start xwwPulse yourself, and games are detected only while it runs |
| `detection.min_fps` | `20` | average presents per second that confirm a game (10 to 240) |
| `detection.confirm_seconds` | `2` | how long the average is measured (0.5 to 10) |
| `detection.score_threshold` | `3` | heuristic points a candidate needs |
| `detection.game_dirs` | `[]` | more folder patterns that count as game libraries, matched anywhere in the exe path |
| `detection.launchers` | `[]` | more exe names that count as launchers |
| `detection.non_games` | `[]` | more exe names that are never games |
| `power.idle_disable` | `true` | keep cores out of deep C-states |
| `power.skip_on_battery` | `true` | leave idle states alone on battery |
| `gpu.process_priority` | `"high"` | `normal`, `above_normal` or `high` |
| `gpu.priority` | `"high"` | GPU scheduling: `off`, `high` or `realtime` |
| `gpu.dwm_mmcss` | `true` | compositor threads under MMCSS |
| `cpu.ccd` | `"auto"` | `auto` (the V-Cache CCD on dual-CCD Ryzen, the P-cores on Intel hybrid), `ccd0`, `vcache` or `off` |
| `cpu.cpu_set_mode` | `"soft"` | `soft` (CPU Sets) or `hard` (affinity mask) |
| `cpu.background_cores` | `true` | move the `[cores]` list to the cores the game does not use |
| `cpu.thread_boost` | `false` | give the busiest game threads their own cores |
| `cpu.thread_boost_with_anticheat` | `false` | allow thread boost when an anti-cheat runs |
| `background.suspend` | `true` | freeze the `[suspend]` list |
| `background.throttle` | `true` | throttle the `[throttle]` list |

Lists in `detection` add to the built-in ones; they do not replace them. Game folders also come from the launchers themselves (Steam, Epic Games, GOG, Ubisoft Connect, EA, Battle.net, Riot, Rockstar), so a game in a custom install folder needs no entry here.

<details>
<summary>Show a complete config.toml</summary>

```toml
[general]
enabled = true
pause_on_alt_tab = true
notifications = true

[detection]
min_fps = 20
confirm_seconds = 2
score_threshold = 3
game_dirs = ["D:\\Games\\"]
launchers = []
non_games = ["obs64.exe"]

[power]
idle_disable = true
skip_on_battery = true

[gpu]
process_priority = "high"
priority = "high"
dwm_mmcss = true

[cpu]
ccd = "auto"
cpu_set_mode = "soft"
background_cores = true
thread_boost = false
thread_boost_with_anticheat = false

[background]
suspend = true
throttle = true
```

</details>

Realtime GPU priority can freeze the desktop when a game hangs on the GPU, until Windows resets the driver. High is safe for nearly every game.

## background.toml

The *Background* page edits these lists: *Add app* picks a running app or an exe file, *Remove* takes one off. Apps xwwPulse ships with carry a *Default* badge; a default you removed can be brought back with *Restore* in its list, and *Restore defaults* resets all three lists. Changes apply from the next game session. In the file, the lists are exe file names, in lower or upper case:

```toml
[suspend]
processes = ["searchindexer.exe", "onedrive.exe", "teams.exe", "ms-teams.exe", "dropbox.exe"]

[throttle]
processes = ["chrome.exe", "brave.exe", "msedge.exe", "firefox.exe", "discord.exe", "steamwebhelper.exe", "epicwebhelper.exe"]

[cores]
processes = ["discord.exe", "vesktop.exe", "chrome.exe", "msedge.exe", "brave.exe", "firefox.exe", "opera.exe", "vivaldi.exe",
             "spotify.exe", "obs64.exe", "steamwebhelper.exe", "epicwebhelper.exe", "telegram.exe", "wallpaper64.exe"]
```

- `[suspend]` apps are frozen while the game is in front and resumed afterwards. Do not add apps you need during a game, such as a voice chat.
- `[throttle]` apps keep running with EcoQoS, very low I/O priority and no dynamic priority boost.
- `[cores]` apps run on the cores the game does not use: the CCD without V-Cache on a dual-CCD Ryzen, the E-cores on Intel hybrid. It applies only when CPU placement found something to choose, and only for the session. Throttling and moving combine.
- The game, its launcher chain and xwwPulse are never touched. An app you switch to during a session is released at once.

## Your game lists

*Always optimize* and *Never optimize* on the *Games* page are stored per user in `HKCU\Software\xwwPulse\ForcedGames` and `BlockedApps`. Each value name is an exe path or file name. The app sends both lists to the service, which matches them against the exe path it resolved itself, so they also work for games that run elevated.
