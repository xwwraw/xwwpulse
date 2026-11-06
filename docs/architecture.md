# Architecture review

This review came before any code was written. It checks the classic design of game optimizers (Process Lasso, KRNLGame-style tweakers, Feral GameMode) against how Windows actually works. Then it lists the decisions xwwPulse makes differently, and why.

## 1. Where things can run: session 0 isolation

A Windows service runs in session 0 as SYSTEM. From there it **cannot** see the user's desktop:

- `SetWinEventHook`, `GetForegroundWindow`, `GetCursorInfo`, `GetClipCursor` and `GetMonitorInfo` all work on the calling thread's desktop. In session 0, that desktop is empty.
- `DwmEnableMMCSS` only affects DWM *while the calling process is alive*. It has to be called from a process in the user's session.

So xwwPulse is split in two:

| Process | Session | Runs as | Does |
| --- | --- | --- | --- |
| `xwwPulseService.exe` (the service) | 0 | SYSTEM | ETW (frames, process and image events), the scoring pipeline, every privileged change, the rollback journal |
| `xwwPulse.exe` (the app and its agent) | user | user | foreground hook, window and cursor facts, the Windows game list, ForcedGames/BlockedApps (HKCU), `DwmEnableMMCSS`, tray, UI |

The two talk over the named pipe `\\.\pipe\xwwPulse` (message mode). Its DACL only lets interactive users, SYSTEM and administrators connect. The agent sends facts, never commands that touch arbitrary processes. The service acts only on the foreground process the agent reports and on the names in `background.toml`.

## 2. Game detection

### What is wrong with polling

Polling `EnumWindows` or `CreateToolhelp32Snapshot` every 1-2 s has four problems:

1. It wakes a core on a timer, which ends C-state residency and adds jitter.
2. The snapshot walk takes kernel locks.
3. It reacts 0-2 s late.
4. It has to open handles to every process, including the game, to read its modules.

That last point matters most. `EnumProcessModules` needs `PROCESS_VM_READ` on the game, and VM-read handles are exactly what EAC, BattlEye and Vanguard strip and log.

### Event-driven pipeline

```
 user session (agent)                                   session 0 (service)
 ───────────────────                                    ───────────────────
 SetWinEventHook(EVENT_SYSTEM_FOREGROUND,              ETW real-time session "xwwPulse"
   MINIMIZESTART/END), WINEVENT_OUTOFCONTEXT             ├ Microsoft-Windows-Kernel-Process
   │  (no DLL is loaded into anyone)                     │    ProcessStart / ProcessStop / ImageLoad
   ▼                                                     ├ Microsoft-Windows-DXGI    Present_Start (42)
 window facts: borderless popup, covers monitor,         └ Microsoft-Windows-DxgKrnl Present_Info (184)
   cursor hidden / clipped, D3D fullscreen state,              (event-ID filtered: every API, Vulkan and OpenGL too)
   UWP CoreWindow owner, Windows game list               │
   re-probed for a minute after each change              ▼
   │                                                    Level 0: LRU cache (pid + create time → verdict),
   ├──────── FOREGROUND {pid, facts} ──── pipe ────►      Always / Never optimize lists (sent by the agent)
   └──────── LISTS {forced, blocked} ──── pipe ────►    Level 1: static facts (no process handle)
                                                         exe in a launcher library or a game folder,
                                                         child of a launcher, PE imports of the exe,
                                                         engine fingerprints next to the exe,
                                                         ETW ImageLoad list (d3d12, vulkan-1, libcef…)
                                                        Level 2: average present rate ≥ min_fps over
                                                         confirm_seconds (ETW, per PID)
                                                         │
                                                         ▼  Candidate → Confirmed → Game
```

- **Foreground changes** come from a WinEvent hook out of context: Windows posts the events to the agent's message loop and nothing is injected.
  - Games often show a launcher or a windowed splash first and switch to fullscreen seconds later. The agent therefore probes the same window again 0.4, 1.5, 4, 8, 15, 30 and 60 seconds after a change, and stops once the service confirms a game. Each probe is a handful of window queries, not a process walk.
  - While a candidate waits for its frame rate, the cursor state is sampled every 500 ms for at most 4 s.
  - UWP apps are framed by `ApplicationFrameHost.exe`; the agent reports the process that owns the `Windows.UI.Core.CoreWindow` inside the frame instead.
- **The Windows game list.** Windows records the games it matched on this PC under `HKCU\System\GameConfigStore\Children`, with the exe in `MatchedExeFullPath`. Those are worth 3 points. The same key also holds a preseeded list of exe names, encrypted with DPAPI, that includes non-games such as DJ software and NW.js; xwwPulse ignores that part.
- **Launcher libraries.** The service reads the install folders that the launchers keep for themselves: `libraryfolders.vdf` for every Steam library, the Epic Games manifests in `ProgramData\Epic\EpicGamesLauncher\Data\Manifests`, the GOG, Ubisoft Connect and EA registry keys, and the uninstall entries of Battle.net, Riot and Rockstar games. A game in a custom folder still counts. The list is rebuilt at most every five minutes, when a detection needs it.
- **Process and module facts come from ETW** (`Kernel-Process`, keywords PROCESS | IMAGE), so the service never opens the game to read its modules.
  - Processes that started before the service: their exe's import table is parsed from disk instead. Scans are cached per exe path, so re-probes cost nothing.
  - Engines that load the renderer from a DLL: sibling fingerprints are checked (`UnityPlayer.dll`, `GameAssembly.dll`, `*_Data\`, `Engine\Binaries\Win64`, `steam_api64.dll`, `EOSSDK-Win64-Shipping.dll`).
- **The LRU cache is keyed by PID + process creation time.** PIDs are reused within seconds, so a PID alone would hand a new process the verdict of a dead one. Only clear verdicts are cached; a window that may still go fullscreen is evaluated again on the next probe.
- **The lists are matched in the service**, against the exe path it resolved from ETW. The agent may not be allowed to query an elevated or protected game, but the service always is.

### Frame confirmation

Measured on Windows 11 with an RTX 4070 (driver 32.0.15.9649) using a D3D11 flip-model probe (`tools/present-probe.cpp`) and a full DxgKrnl trace:

- The DXGI provider is `{CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}`; the `{CA11C00C-…}` GUID quoted in many write-ups does not exist on Windows.
- DxgKrnl `Present_Info` (184) fires once per present call in the presenting process, for every graphics API. It matched DXGI `Present_Start` one to one (17 380 against 17 505 in two seconds).
- `Blit_Info` (166), `Flip_Info` (168) and `FlipMultiPlaneOverlay_Info` (252), the events many frame counters rely on, never fired in the presenting process, windowed or fullscreen: with hardware flip queues the flips are logged elsewhere. On such systems a Vulkan or OpenGL game, which bypasses DXGI, showed no frames at all.
- An occluded window returns from `Present` at once and logs thousands of presents per second. That is why frames are recorded only for the foreground candidate and the running game.

So xwwPulse counts `Present_Start` and `Present_Info` per PID and takes the larger of the two, never the sum. A candidate becomes a game when its **average** over `confirm_seconds` (2 s) reaches `min_fps` (20). Requiring every 250 ms sample to pass would fail for games capped at 30 fps, which measure 29 as often as 31. The measured window ends a second ago, because ETW delivers events up to one flush interval late.

A candidate is not dropped when it does not reach the frame rate. It stays watched while it is in front, at 250 ms intervals for the first 30 s and once a second after that, and the app shows it as *waiting for a steady frame rate*. Long loading screens and shader compilation therefore delay game mode instead of cancelling it.

### Keeping the agent alive

Without the agent there are no foreground facts, and detection stands still. The agent starts at sign-in from the `Run` key, but that entry can be disabled in Task Manager, and the app can be closed. The service subscribes to session changes (`SERVICE_ACCEPT_SESSIONCHANGE`). 20 seconds after it starts, and after a sign-in, unlock or console connect, it checks for a connected agent. If none is connected, it starts `xwwPulse.exe --tray` in the active console session with `WTSQueryUserToken` and `CreateProcessAsUserW`. Choosing *Exit* in the tray menu tells the service to leave that session alone until the next sign-in. When *Start at sign-in* is off (`general.start_with_windows`), the service removes the `Run` entry and never starts the agent itself.

### Game exit

ETW drops events when its buffers overflow, and a lost `ProcessStop` would leave game mode on. The service therefore also waits on the game itself (`RegisterWaitForSingleObject` on a `SYNCHRONIZE` handle, a right anti-cheats allow). Whichever arrives first ends the session.

If ETW cannot start at all, a candidate needs two points above the threshold to count as a game, since there is no frame rate to confirm it.

### False positives

Present rate alone does **not** separate a game from YouTube: a browser playing 60 fps video presents at 60 Hz in borderless fullscreen with the cursor hidden. xwwPulse therefore never decides on frames alone. Frames only *confirm* a candidate that already passed Level 1. These are rejected before frames are even counted:

| Case | Why it is rejected |
| --- | --- |
| Browsers, Electron and CEF apps (Chrome, Edge, Brave, Firefox, Discord, Steam UI, VS Code, Spotify, Teams) | the built-in `non_games` list, plus stop modules `libcef.dll`, `node.dll`, `electron*.dll`, `chrome_elf.dll` |
| WPF and WinForms tools | `PresentationFramework.dll`, `System.Windows.Forms.dll` |
| Media players (VLC, mpv, MPC-HC, PotPlayer, Movies & TV) | `non_games`, and no 3D marker in their imports |
| Office | `non_games` |

A candidate needs a score at or above `score_threshold` (3) from these signs:

| Sign | Points |
| --- | --- |
| in the Windows game list | 3 |
| in a launcher library, or a configured game folder | 2 |
| started by a game launcher | 2 |
| engine or game middleware files next to the exe | 2 |
| exclusive fullscreen (`QUNS_RUNNING_D3D_FULL_SCREEN`) | 2 |
| borderless window covering the monitor | 1 |
| cursor hidden or locked to the window | 1 |
| a game input API in the imports (`xinput*`, `gameinput`, `dinput8`) | 1 |

Stop modules reject a candidate unless it sits in a launcher library or the Windows game list *and* has engine files: a few games ship CEF for their menus. Windowed games (no fullscreen, visible cursor) still pass on library, launcher and engine.

## 3. Anti-cheat safety

xwwPulse loads nothing into the game and reads nothing from its memory. It opens the game process with **only** these access rights:

`PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION | PROCESS_SET_LIMITED_INFORMATION | SYNCHRONIZE`

Those are the rights Task Manager uses to change priority. Anti-cheats allow them, because Windows itself (Game Mode, Task Manager, power management) uses them.

### Changes from the spec

1. **Suspending GameInput polling threads with `NtSuspendThread`: not implemented.**
   - It means opening `THREAD_SUSPEND_RESUME` handles to threads *inside the game*. That is the cross-process thread tampering anti-cheats exist to catch.
   - If the suspended thread holds a lock (heap, loader, an XInput or GameInput mutex), the game deadlocks.
   - It breaks requirement 2 of the spec, so it is left out. The GameInput stutter on some Windows 11 builds is fixed by updating the GameInput redistributable (Microsoft.GameInput, a winget package).
2. **Raising priorities of threads inside `dwm.exe`: not implemented.**
   - DWM is a critical system process, and its threads already run at priority 15 under MMCSS when the compositor asks for it.
   - `DwmEnableMMCSS(TRUE)` is the supported switch, and the agent calls it.
3. **Game thread tuning (power throttling off, ideal processor) is opt-in and switches itself off when an anti-cheat is present.**
   - It needs `THREAD_SET_LIMITED_INFORMATION` handles to game threads. Windows allows that, but some anti-cheats log it.
   - A game counts as protected when the EasyAntiCheat, BattlEye, Vanguard, FACEIT, ACE or PunkBuster service or driver is running, or when the game folder contains `EasyAntiCheat\` or `BattlEye\`. Then xwwPulse stays at process level.
4. **GPU scheduling priority defaults to `HIGH`, not `REALTIME`.**
   - With `D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME`, a game that hangs on the GPU starves the compositor: black screen, frozen cursor, and the TDR watchdog is the only way out.
   - It also needs `SeIncreaseBasePriorityPrivilege`. `realtime` remains available in `config.toml` for people who want it.
5. **CCD selection reads the cache topology instead of assuming CCD0.**
   - On Ryzen X3D parts with two CCDs, only one CCD has the 3D V-Cache, and it is not always CCD0.
   - xwwPulse groups logical processors by L3 (`GetLogicalProcessorInformationEx(RelationCache)`). In `auto` mode it picks the L3 domain with the largest cache, and the lowest index when sizes are equal.
   - On Intel hybrid CPUs (one L3, two efficiency classes in `GetSystemCpuSetInformation`) the fast domain is the P-cores: the CPU Sets with the highest `EfficiencyClass`.
   - The topology is probed once, at the first session. A CPU with one L3 and equal cores gets no placement at all.
   - It applies this with **CPU Sets** (`SetProcessDefaultCpuSets`): a soft preference the scheduler can override under pressure. A hard affinity mask breaks games that set their own affinity, and it survives a crash. `hard` mode exists in the config.
   - The `[cores]` list in `background.toml` (Discord, browsers, Spotify, OBS…) gets the other CPU Sets for the session: the CCD without V-Cache, or the E-cores. Each move is journaled as `bgcpuset`, and the rollback clears the CPU Sets with `SetProcessDefaultCpuSets(nullptr, 0)`.
6. **C-states: the spec's "write `IDLEDISABLE` into the current scheme" becomes "duplicate the scheme".**
   - Writing into the active plan means a crash leaves the user's own plan changed for good.
   - xwwPulse duplicates the active scheme into a temporary *xwwPulse Game* scheme and sets `IDLEDISABLE=1` (AC and DC) there with `PowerWriteACValueIndex`. Then it activates that scheme with `PowerSetActiveScheme`.
   - Rollback re-activates the original scheme and deletes the copy. The user's plan is never written to.
   - On battery, this step is skipped by default: with idle states disabled, cores never sleep.

## 4. Rollback

Every change is written to a journal (`HKLM\SOFTWARE\xwwPulse\Journal`) **before** it is made, with the value it replaces. Process entries store PID and creation time, so a reused PID is never resumed or restored by mistake.

| Trigger | What happens |
| --- | --- |
| Game exits (wait on its handle, ETW ProcessStop) | full rollback |
| Alt+Tab / minimize (`pause_on_alt_tab`) | power scheme back, background processes resumed and unthrottled, DWM MMCSS off. The game keeps its priority and CPU set, which are harmless in the background. Re-applied on return |
| Service stop, shutdown, preshutdown | full rollback |
| Crash in the service | unhandled-exception filter rolls back. SCM restarts the service (failure actions), and startup replays the journal |
| Crash in the agent | DWM MMCSS ends with the process (documented behaviour). The service sees the pipe close and pauses the session |
| Power loss or BSOD | at the next start the journal is replayed: original scheme re-activated, temporary scheme deleted. Process entries are dropped, since those processes are gone |

## 5. Background limiter

| List | Action | Undone by |
| --- | --- | --- |
| `[suspend]` (SearchIndexer, OneDrive, Teams, Dropbox) | `NtSuspendProcess` | `NtResumeProcess` |
| `[throttle]` (browsers, Discord, steamwebhelper…) | EcoQoS (`ProcessPowerThrottling`, `EXECUTION_SPEED`), I/O priority Very Low (`ProcessIoPriority`), dynamic boost off (`SetProcessPriorityBoost`) | the values saved before the change |

- The lists are read once when the session starts. Processes that start during the session are caught through ETW ProcessStart, with no polling.
- The game, its ancestors (the launcher) and the app are never touched.
- A process the user brings to the foreground is resumed and unthrottled at once, so tabbing to Discord just works.

## 6. Thread dispatcher (opt-in)

While the game is in the foreground, every 2 s the service reads one `NtQuerySystemInformation(SystemProcessInformation)` snapshot. This call needs no handle to the game. Threads above 25 % of a core over the interval are the hot ones (main loop, render, audio mixer). The top four get:

- `SetThreadInformation(ThreadPowerThrottling)` with `ControlMask = EXECUTION_SPEED` and `StateMask = 0`: never EcoQoS;
- `SetThreadIdealProcessorEx` on different physical cores of the chosen CCD: one logical processor per core, so SMT siblings are not shared.

The original ideal processor is stored and restored. The 2 s timer runs only while a game is active and in the foreground.

## 7. Overhead budget

- **Idle (no game):**
  - Service: zero timers. ETW is real-time with small buffers; DXGI and DxgKrnl are enabled only while a candidate is being confirmed or a game is active.
  - Agent: one hook and no timers.
- **Active game:** one 2 s timer, only when the thread dispatcher is on. One process wait.
