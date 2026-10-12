# Whole-game audit: can the campaign be finished without a blocker?

Status (2026-10-12): yes. The whole campaign has been played on PC from
start to finish and is playable. If the game stops anyway, it shows a message
and saves a crash report: please send it. The sections below are the
evidence gathered before that playthrough (2026-09-28 onward).

Question (2026-09-28): can the whole game be played through, for example
Chapter 3, without a stop? Only the Chapter 1 opening, the Chinatown street
and its combat had been played on PC. This audit answers with evidence:
what the game can ask of the runtime, what the runtime answers, what the
tests have actually executed, and what remains open.

Tools: `scripts/import_reach_audit.py` (static callers of every trapped
import, with coverage), the function coverage recorder
(`REX_FUNCTION_COVERAGE=<file>`, off by default), the recompiler log
(`logs/switch_correction_recomp.log`) and the maintainers' automated test
harness (scripted runs from saved games and from the game's own developer
start option).

## 1. System calls (imports)

The executable imports 224 kernel/XAM functions. Every import the runtime
does not implement is compiled as a trap: if the game calls it, it stops with
a report (`UNRESOLVED_IMPORT` in `logs\runtime_crash.log` plus a dump), never
silently continuing.

| | V430 | V432 | V433 and later |
|---|---:|---:|---:|
| implemented | 139 | 149 | 206 |
| trapped | 85 | 75 | 18 |

V432 answers the calls a player can reach from the menus, or that executed
code comes close to, the way a console does for a local profile without Xbox
Live: game region, the guide UIs (achievements, friends, gamer card, player
review: no overlay, the call succeeds), the sign-in UI (the guide opens and
closes; the profile stays local), Xbox Live privileges (denied), launching
another title (the game ends, as on a console), launch data.

V433 found a real stop: **Multiplayer > Quick Player Match ended the game**
(its network start-up called an unimplemented import). The runtime now
answers the whole network and Xbox LIVE layer as a console with a network
adapter but no cable, no headset, and a local profile that is never signed in
to Xbox LIVE:

- Winsock/XNet on local sockets: they open, bind and switch to non-blocking;
  nothing is ever received, stream connections fail, datagrams go nowhere;
  the network address has a link-layer address and no IP. Session keys are
  local; quality-of-service probes and DNS fail. Nothing reaches the host
  network.
- The local profile has an offline XUID, a name, sign-in info (signed in
  locally) and no friends; statistics, leaderboards and Xbox LIVE base
  messages fail as they do offline; no headset is present.
- Session handles, the voice engine's kernel objects (NT timers for its
  10 ms worker, handle duplication), the XAM heap, the music player's
  playback-controller query, and two library fallbacks (the dashboard
  return, which ends the game cleanly, and wide-to-ANSI debug text).

The 18 remaining traps, by who can call them (direct calls in the recompiled
code; coverage from all runs below):

| Group | Imports | Reached from | Evidence |
|---|---|---|---|
| Console cache and signing | XeKeysConsolePrivateKeySign/SignatureVerification, StfsCreateDevice/ControlDevice, IoDismountVolume*, NtDeviceIoControlFile, RtlImageXexHeaderField, XamTask*, NtAllocate/FreeVirtualMemory | one routine, 0x82205FE8 (cache set-up) | verified 2026-08-24: the raw cache partition is absent and the game takes its no-cache path (`docs/CONSOLE_SIGNING_BLOCKER.md`); runs every start without a trap |
| L2 cache lock | KeLockL2, KeUnlockL2 | a system-library worker command (0x82873B28 -> 0x828732F0) | the worker ran in every test without that command; if a later scene sends it the game stops with a report. The lock window aliases guest RAM in this runtime, so succeeding silently could corrupt memory; left as a trap on purpose |
| Exception unwinding | RtlUnwind, __C_specific_handler | after a C++ throw | a throw already ends the game (below) |
| Other | NtReadFileScatter | never called | - |

Implemented on purpose as a stop (a console ends the game there too):
`XamShowMessageBoxUIEx` (only the C runtime's fatal-error path calls it),
`RtlRaiseException` (a C++ throw), `KeBugCheck` (the game's own crash
handlers, 41 callers), `XamLoaderTerminateTitle`, `XamLoaderLaunchTitle`,
`HalReturnToFirmware`.

## 2. Recompiled code

- 31,860 functions recompiled; switch tables verified (0 recompiler errors).
- Found by this audit: 80 instruction sites the recompiler had skipped
  (emitted as comments only), in 9 functions: `vctuxs`/`vcfpuxws128`
  (float to unsigned integer; 72 sites in four system-library functions),
  `fnmadd` (one game function, 0x825E2C28, and one library function) and
  `dcbst` (a cache hint, harmless). Skipped instructions leave stale register
  values, a classic "fails later in the game" defect. Fixed in the recompiler
  (V431): the regenerated code has zero unrecognized instructions; test
  `vctuxs` checks the conversion against a reference.

## 3. Graphics, video, audio

- Graphics: 606 automated runs and the manual test sessions logged no
  unsupported or unimplemented GPU feature.
- Video: the four startup logos are WMV files decoded by the game's own
  video player (recompiled system library) at every start; the other WMV
  files (the title's attract video, the construction-site cutscene, the
  end-credits logo) use the same player, and *Extra Content > Painting demo*
  plays through it (V432 menu run). The in-game TV channels are Theora/Vorbis
  files decoded by the game's code.
- Audio: XMA through the runtime's decoder, streamed wave banks (music, voice,
  TV) through the same path as Chapter 1.

## 4. Levels

The campaign has 27 worlds (NY1 x9, SUB x2, NY2 x5, OW1 x5, OW2 x3, NY3 x3,
named by the load lists in `Content\Xdf`); each has its own packs on the disc
and loads through the same file system. The whole campaign has been played on
PC from start to finish (2026-10); the automated tests cover a new game from
its start (NY1_Tunnel), the Chapter 1 opening ride, and NY1_Chinatown (street,
combat), plus every menu.

## 5. Menus and multiplayer (V435 runs)

| Menu | Result |
|---|---|
| Main menu, Options, Checkpoints, Extra Content | open and work |
| Extra Content > Achievements | the game's own list with descriptions |
| Extra Content > Painting demo | the video plays |
| Multiplayer > Quick/Custom Player Match, Quick/Custom Ranked Match | the game asks for an Xbox LIVE sign-in; the guide closes at once and the game stays on the menu |
| Multiplayer > System Link | "No servers found"; Create Game opens its game-mode and advanced-settings screens |
| Multiplayer > Setup Character | the character selection screen |
| Multiplayer > Leaderboards | the category list; a category asks for Xbox LIVE and stays |

None of these stops the game any more. Before V433 the first four and
Leaderboards ended it; V432 and V433 (test packages only) answered the
sign-in UI with a sign-in change, and the game restarted with "Restarting due
to Gamer Profile changes"; V434 fixed that (the guide opens and closes, no
sign-in change).

## 6. Function coverage

Functions entered at least once, recorded by `REX_FUNCTION_COVERAGE`:

| Run | Functions |
|---|---:|
| every menu, including all multiplayer items | 5,533 |
| new game from its start (NY1_Tunnel, with walking and fighting) | 7,945 |
| Chapter 1 opening ride | 9,929 |
| NY1_Chinatown street fight | 10,193 |
| all runs together | 10,944 of 31,860 (34%) |

Most of the rest is code no single-player session uses (the multiplayer
game modes, editor and debug paths, platform variants, unused library code)
or later-level content (scripted events, enemies, weapons, the Otherworld).

## 7. Findings for players (known issues)

- Multiplayer: the menus work, but there is no online or local network play:
  the port behaves like a console without a network connection.
- The whole campaign is playable on PC from start to finish. If the game
  stops anyway (for example a trapped call in a rarely used path), it shows
  a message and saves a crash report: please send it.

## 8. Closing the gap: coverage from saved games

The test harness copies a player's saves into a disposable profile (never
writing them), starts a chapter they have reached from the Checkpoints screen
(or the latest checkpoint), runs a scripted soak (look around, walk, fight)
with snapshots and function coverage, and reports traps, stalls and fatal
stops. Each newly reached chapter gets such a run.

The game's own developer start option (`-MAP <world>`, read from
`EnvironmentXbox.cfg`) also works in test runs (through a loose-file overlay
and a test-only runtime switch, `REX_TEST_UNSET_USER_AS_LOCAL`, unset in
ordinary launches), but without the engine's remembered profile it goes
through the profile screen and starts a new game at NY1_Tunnel, so it does
not reach later worlds; later chapters need saves from them.
