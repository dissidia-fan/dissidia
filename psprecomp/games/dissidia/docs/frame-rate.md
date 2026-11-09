# 60 fps and the settings menu

## Using it

- **Settings menu**: Esc or F1 on a keyboard, Guide or L3 + R3 (both stick buttons) on a pad. The game pauses while it
  is open. Rows: Display (Fullscreen / Window), Window Size (a whole multiple of 480x272, or 1280 x 800, the Deck's
  screen, when the desktop is big enough; in fullscreen it is remembered for Window mode), Render Scale (1x to 4x),
  Frame Rate (30 / 60), Quit Game (press twice). Left / right changes a value, Cross cycles it, Circle or Start closes.
  It is drawn from the game's own menu textures, like the save dialog.
- **Debug page**: L / R (shoulder buttons), or Q / E / Tab on a keyboard, switches between System and Debug:
  - Performance Overlay: frames per second, average and worst frame time over the last half second, top-left.
    Saved (`perf_overlay`); `PSPRECOMP_PERF_HUD=1` turns it on without the menu.
  - FX Shaders: bypasses the `mods/fx_shaders` overrides live ("Not loaded" when none were loaded at start).
    This session only.
  - GPU Sync: the `--ge-sync` modes live (Off, Every batch = 1, Every list = 2, Every frame = 3, Flush only = flush).
    This session only.
  - Vertex Upload: Map / Sub (`PSPRECOMP_GE_RING`). Saved (`ge_ring`) and applied at the next start.
  The last three exist for the Steam Deck flicker hunt: flip them mid-battle instead of relaunching per test.
- **On-screen keyboard** (naming the player at the first start): a key grid in the save dialog's style. Pad: d-pad
  moves, Cross types the key, Circle deletes, Triangle = space, Square = upper / lower case, Start = done. A real
  keyboard types directly (Enter = done, Backspace deletes). On a Steam Deck, Steam's own keyboard opens with it
  (its Enter key finishes the name); `PSPRECOMP_STEAM_KEYBOARD=0` keeps it closed, `=1` forces it elsewhere. Look
  for `[OSK]` lines in the log if it doesn't appear. `PSPRECOMP_OSK_TEXT=<name>` or `PSPRECOMP_OSK_AUTO=1`
  skips it (headless runs), as before.
- **Load trace**: the last 32 PACKAGE.BIN reads (time, offset, size, index crc, and the mod file when one was
  served) are printed as `[LOAD]` lines when the game quits, so a load that never finishes can be traced by quitting
  from the menu. A mod file shorter than what the game asked for is logged as it happens. `PSPRECOMP_LOAD_TRACE=0`
  turns it off.
- Changes apply at once and are saved to `<workdir>/settings.ini`. On the next start the saved values win over
  `run.sh` / `play.sh` flags. Delete a line (or the file) to go back to the flags.
- Without the menu: `run.sh <workdir> --fps 60` (`PSPRECOMP_FPS=60`).
- `PSPRECOMP_FPS_LOG=1` logs the logic rate, dt, the global step and how often
  body poses change from one frame to the next (about 100% = animation runs at 60, about 50% = it steps at 30)
  every 5 s, and each 30/60 switch.
  `PSPRECOMP_FPS_SCOPE=all` also runs menus at 60 (they then run fast; for testing only).
  `PSPRECOMP_SETTINGS_MENU=0` disables the menu, `PSPRECOMP_SETTINGS=<file>` moves the settings file.

## In multiplayer

In an ad-hoc session ([multiplayer.md](multiplayer.md)) the game runs at the lowest rate any player asked for, so a
60 fps player and a 30 fps player both run at 30; the setting returns to normal when the session ends. Every player
shares the wanted rate in their beacons; `frame_end_hook` in `runtime/framerate.cpp` applies the cap.

## What 60 fps means here

It is **native**: the battle simulation runs 60 updates a second. Nothing is interpolated, and there's no extra
display latency. It costs twice the CPU and GPU time of 30 Hz.

## How the game paces itself

The EBOOT is byte-identical between the US and EU discs, so these addresses hold for both.

| What | Where | Notes |
|---|---|---|
| Frame timer object | pointer at `0x089D69E0`, built by `FUN_08883E74` | `+0x04` mode (0), `+0x0C` vcount at the last flip, `+0x10` **interval in vblanks (2)**, `+0x14` vblanks this frame, `+0x18` **dt** (float), `+0x24`/`+0x25` dt enables |
| Frame end | `FUN_08883F9C(timer, useNonCB, flag)` | Every loop ends here (11 call sites: battle, menus, loading, movies). Waits until `interval` vblanks have passed since the last flip, swaps, stores `dt = vblanks x 1.0` (`0x089C9F50`). In mode 0 the count is clamped to the interval: **dt is a fixed 2.0 at 30 Hz**, in 1/60 s ticks |
| Interval readers / writers | constructor only; `FUN_08883F54` (inside the frame end) reads it | So the interval can change at any frame |
| Global step | `0x089D5EAC`, written by `FUN_08864DD8` | `= timer dt`, or a slow-motion value derived from it |
| Actor step | `FUN_08838300(actor)` | `actor clock (+0x3C8) x global step`, or the actor's own clock (`+0x3CC`) when it is flagged to ignore slow motion. 217 call sites |

The battle code is written against that step: 85 reads of `timer+0x18` and 146 of `0x089D5EAC` in the battle overlay
(31 and 48 in the EBOOT). Motion time, timeline events (keyed events fire in `(prev, now]`), movement (speed x dt),
decays (`pow(a, dt/2)`), effect lifetimes and the camera all advance by it. Comparisons against dt are countdowns
(`timer <= 0`) and float accumulators truncated to ints, which work at any step. So **interval 1 gives dt 1.0 and
the same game at 60 updates a second**. Menus use dt in 4 places only and count frames otherwise, so they stay at 30.

## What does not follow the step (fixed in `runtime/framerate.cpp`, only at 60 Hz)

| Code | Problem at dt 1 | Fix |
|---|---|---|
| Pad auto-repeat `FUN_08871BD0` | Repeat timer counts down by `max(step, 2.0)`: menu cursors would repeat twice as fast | Reload values (pad manager `*0x089D6008` `+0x25C` delay 30, `+0x260` interval 4) scaled by `2 / step` for the call |
| Actor own clock `actor+0x3CC` | Set to a constant 2.0 by the battle overlay (`0x08A374B0`, `0x08A37868`, `0x08A37960`, `0x08B46BE0`): those actors would run at double speed | `FUN_08838300`'s result scaled by `interval / 2` for such actors |
| Camera mode 2 (`FUN_08B3B4FC`, camera `+0x100 == 2`) | Uses camera `+0x104`, a constant 2.0 from the constructor | Result scaled by `interval / 2` |
| Mod code (`roster.cpp`) | The warp-dash link window counted bone passes; `pinspin=` turned per pass | Counted in ticks (`dissidia_frame_ticks()`); spin and the own-clock copy scaled (`dissidia_own_clock_scale()`) |
| Effect components that step per update (see below) | Envelopes strobed, sparks / lines lived half as long and flew twice as fast, trails were half as long | Stepped at 30 Hz: every other update with dt x 2 |

Scope: the frame end hook writes interval 1 only while the battle overlay is resident (overlay id 1), 2 elsewhere.

### Effect components (fixed 1 Nov)

The effect system advances its components by `dt` = effect instance `+352` (the global step x the handle's scale,
written by `FUN_0894B48C`): 2.0 per update on the PSP, 1.0 here at 60 Hz. Most components scale by it, but these step
once per **update** (archive/open-questions/fx.md item 7, archive/fx-components/F.md, E.md):

| Channel | Update | Per update, not x dt | At 60 Hz before the fix |
|---|---|---|---|
| 61 envelope (on colour, via 62) | m2 `0x08953A50` | `mult += rate` (the timer does `+= 0.5 dt`) | each ramp twice as many steps of the same rate: 2x overshoot, clamped by 62 into a strobe |
| 95 points, 100 streaks, 102 line burst, 103 line rays | m2 `0x0897F3E4`, `0x0897DE38` (→ `FUN_08984508`), `0x08974DCC`, `0x089760E8` | life −1, motion, history (only emission is x dt) | half the life, twice the speed, same particle count |
| 68 trail path (for the ch80 ribbons) | m2 `0x08966018` (→ `FUN_0896B710`) | one ring-buffer sample | ribbons half as long |
| 70 smoke path (for ch82) | m2 `0x08967438` (→ `FUN_0896C9B0`) | shift + drift | twice as fast |
| 105 locus renderer | m2 `0x08964B40` | UV scroll `u -= P+48, v -= P+52` | texture scrolls twice as fast |
| 106 locus history (weapon / body trails) | `FUN_0896808C` (from m2 `0x089672C4`) | one shift whenever `t += dt` reaches `last + 1`: every update at dt 2 **and** at dt 1 | trails half as long |

Fix (`runtime/framerate.cpp`, only at 60 Hz, `PSPRECOMP_FX60=0` turns it off for A/B checks): each of these runs on
every other update of its own state block, with `instance+352` doubled for that call, so it does exactly what the
PSP does on a 30 Hz frame and holds in between (as the PSP's picture does). The emitters read dt through the current
runner `*0x09B6F478 +136`, the same instance. ch106 keeps a live trail head: the updates between run with dt 0 (no
shift, but `pos[0]` still follows the bone), and before each shift update `pos[0]` of both point buffers is put back
to the point of the previous shift update, so the shift copies the sample the PSP would. It is wrapped at
`FUN_0896808C`, inside `roster.cpp`'s lingering-trail hook on the m2. Checked with fakes of these functions driven at
30 Hz and 60 Hz: identical states on every 60 Hz update (held ones included) and the trail head on the bone every
update. Not yet checked in a game run. Checked and fine as they are (time- or dt-based, or event-driven): the
transform / colour curves (1–9, 15–17), 10, 18, 20, 21, 26, 80, 101, 107, 109–111, 150, 151; 66 (a rope with a
fixed link length) depends on distance, not on the update count; 79 is inert. Not read: 58 and 82 (one node each).

## First run (28 Oct, Mac M2 Pro, render scale 1)

Menus held 30 Hz (interval 2, overlay 2). The battle switched to 60 Hz as it loaded and ran at 59.4-59.8 logic updates
and presented frames a second for the whole fight (the first 5 s window, 50.7, includes the switch). Body poses changed
on 97-100% of frame-to-frame checks (one window 79%, likely hit-stop or a pause): the engine samples motions at
fractional frames, so **body animation runs at 60 with no extra work**. Clean exit, no new lookup misses.

## Still to check in play

Found statically and not yet confirmed in a run: anything in the battle code that counts frames as integers without
dt (none found among the dt readers; code that never reads dt can't be found this way), integer fades whose
`rate x dt` truncates to 0 at dt 1 (`FUN_08911AA8`-style `byte += trunc(rate x dt)`), the 30 fps GMO motions sampled
at half frames (confirmed fine in the first run), and the CPU cost on the Deck.

## Settings menu internals

Generic runtime code (`runtime/src/psp_settings.cpp`, see ARCHITECTURE.md). The page layout lives next to the save
dialog (`psp_sddialog::render_menu`) and uses the same `SdTheme`. The Dissidia theme adds the d-pad left/right glyphs
(button sheet, entry 8) and the blue/white selector triangles (common UI sheet, entry 0) for the value arrows.
