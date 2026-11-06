# Troubleshooting

The service log is `C:\ProgramData\xwwPulse\Logs\Pulse.log`; the app shows it on its *Logs* page. The app's own log is `PulseDashboard.log` in the same folder.

## The app says the service is not running

Open *Settings* in the app and choose *Install service*, or run `xwwPulseService.exe --install` as administrator. If the service is installed but stopped, look for the reason in `Pulse.log` and in the Windows event log under *System*.

## A game is not recognized

1. Open *Games > Recent decisions*. Each entry says why it was or was not a game. `Pulse.log` has the same reasons, one line per decision.
2. *Undecided* with a low score usually means a windowed game outside the launcher libraries, started without its launcher. Add it with *Games > Add game*.
3. *Waiting for a steady frame rate* means it has not averaged `detection.min_fps` (20 by default) for two seconds yet, often during a long loading screen or in a menu with a low frame cap. xwwPulse keeps checking while the game is in front; lower `detection.min_fps`, or add the game to *Always optimize*.
4. Nothing at all in *Recent decisions* means the app is not running in your session: it reports the window in front. Open xwwPulse from the Start menu. The service also starts it about 20 seconds after you sign in when it is missing, unless you chose *Exit* in the tray menu.

## Something that is not a game turns game mode on

Add it to *Never optimize*. Benchmarks, emulator front ends and video tools can look like games.

## An optimization says skipped or failed

| Message | Meaning |
| --- | --- |
| blocked by the game's anti-cheat | the anti-cheat removed the right to change priority; expected for protected games |
| one L3 cache for all cores | the CPU has a single CCD, so there is nothing to choose |
| on battery | idle states stay on while `power.skip_on_battery` is set |
| ... is running; thread access stays off | an anti-cheat runs and `cpu.thread_boost_with_anticheat` is off |

## After a crash or a power cut

The next start replays the journal. To do it by hand:

```powershell
.\xwwPulseService.exe --recover
```

It re-activates your power plan, deletes the temporary *xwwPulse Game* plan, resumes frozen apps and restores priorities of processes that are still running.

## Getting help

Open an [issue](https://github.com/xwwraw/xwwpulse/issues) and attach `Pulse.log`.
