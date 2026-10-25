# Environment Variables and Build Options

Complete reference for every `PSPRECOMP_*` environment variable and CMake
option in the project. Each entry lists where it is read, its type, default,
effect, and when you would set it.

Three kinds of knob appear here:

- **env** — an environment variable read at runtime or during the Rust
  pipeline (`std::env::var` in Rust, `std::getenv` in C++).
- **cmake** — a CMake cache variable/`option()` passed at configure time with
  `-D<NAME>=<VALUE>`.
- **both** — a name used as a CMake variable *and* exported into the
  environment of a sub-process (e.g. `PSPRECOMP_CROSS_MID`, which CMake only
  prints in its regenerate hint while the Rust pipeline reads it from the env).

Unless stated otherwise, a boolean env flag is "on" when set to `1` and
otherwise off; several flags activate on mere *presence* (any value) — that is
called out per entry.

## Most users only need these

| Flag | Type | What it does |
|------|------|--------------|
| `PSPRECOMP_CROSS_MID` | env | Recompile with cross-function mid-jump recovery. The documented build line sets `PSPRECOMP_CROSS_MID=1` before `cargo run -- recompile`. |
| `PSPRECOMP_GAME` | cmake | Selects which `games/<id>` module compiles into the runtime (`dissidia` default, `none` for a generic build). |
| `PSPRECOMP_OUTPUT_DIR` | cmake | Points the runtime build at a recompiler output directory other than `../output`. |
| `PSPRECOMP_DISC0` | env | Path to the extracted disc image the runtime reads game data from (default `./disc0`). |
| `PSPRECOMP_STRICT` | env | Abort the runtime on the first dispatch lookup miss instead of returning a noop stub. |

Everything below "Debug and trace flags (advanced)" is for investigation only;
none of it is needed for a normal recompile-build-run cycle.

---

## Pipeline / recompile (Rust)

These are read by the `psprecomp` Rust binary during `analyze`/`recompile`/`dump`.

### `PSPRECOMP_CROSS_MID`
- **Read in:** `crates/psp-cli/src/recompile.rs`, `crates/psp-cli/src/config.rs`,
  `crates/psp-cli/src/dump.rs`, `crates/psp-cli/src/main.rs`,
  `crates/psp-cli/src/fingerprint.rs`
- **Type:** env (`=1` to enable) — also referenced in `runtime/CMakeLists.txt`
  as part of the documented regenerate command.
- **Default:** off.
- **Effect:** When `=1`, the recompile pipeline coalesces Ghidra-over-split
  shared-frame siblings and runs systematic cross-function mid-jump recovery
  (D2+D3a), and activates the manifest's `force_entries_cross_mid` list. Gated
  off by default because landing it makes the broken decompressor at
  `0x089D7xxx` run and stall (D1 baseline note in `recompile.rs`).
- **Set it when:** generating output (the project's documented build line uses
  it). Pass the same value to `dump` so a single function's dumped C++ matches
  its batch file.

### `PSPRECOMP_NO_COALESCE`
- **Read in:** `crates/psp-cli/src/recompile.rs`
- **Type:** env (`=1` to enable). Only has effect when `PSPRECOMP_CROSS_MID=1`.
- **Default:** off.
- **Effect:** Disables only the shared-frame sibling merge while leaving
  cross-function mid-jump injection on (an A/B diagnostic isolating the merge).
- **Set it when:** bisecting a regression introduced by `PSPRECOMP_CROSS_MID`.

### `PSPRECOMP_NO_RA_MODEL`
- **Read in:** `crates/psp-cli/src/recompile.rs`
- **Type:** env (`=1` to enable).
- **Default:** off.
- **Effect:** Keeps the structural sibling merge but disables the Option-A
  LINK/RA lowering applied to coalesced owners (a diagnostic splitting the
  merge from the RA model).
- **Set it when:** isolating which half of the coalesce change moved behavior.

### `PSPRECOMP_COALESCE_DEBUG`
- **Read in:** `crates/psp-cli/src/recompile.rs`
- **Type:** env (`=1` to enable). *Advanced / diagnostic.*
- **Default:** off.
- **Effect:** Logs each coalesce owner that absorbed >= 4 siblings (owner
  address, sibling count, member list) via `tracing::info!`.
- **Set it when:** inspecting which functions the coalescer merged.

### `PSPRECOMP_ALLOW_STALE`
- **Read in:** `crates/psp-cli/src/fingerprint.rs` and
  `runtime/CMakeLists.txt` (as a CMake `option()`).
- **Type:** both (env for the pipeline-side check; CMake option for the
  configure-time check — `-DPSPRECOMP_ALLOW_STALE=ON`).
- **Default:** off / `OFF`.
- **Effect:** Downgrades the output-staleness fingerprint mismatch from a fatal
  error to a warning. The fingerprint compares the emitter source hash against
  `output/fingerprint.json`; a mismatch means `output/` does not reflect the
  current emitter sources.
- **Set it when:** you knowingly want to build/recompile against an out-of-date
  `output/` (accepting that runtime behavior will not match current sources).

---

## Recompile manifest selection

### `PSPRECOMP_GAME` (pipeline side)
- **Read in:** `crates/psp-cli/src/config.rs`
- **Type:** env. (Distinct from the CMake `PSPRECOMP_GAME` below; same name,
  different layer.)
- **Default:** unset.
- **Effect:** On the pipeline side, used while resolving the per-game config so
  the recompile uses the matching `games/<id>` manifest context.
- **Set it when:** scripting a recompile for a non-default game id. For a normal
  build the `--config games/<id>/game.toml` argument already pins the game.

---

## Runtime — run-time configuration (C++)

Read by the runtime binary at start-up or during execution.

### `PSPRECOMP_DISC0`
- **Read in:** `runtime/src/main.cpp`
- **Type:** env (path string).
- **Default:** `./disc0`.
- **Effect:** Overrides the disc0 directory the I/O layer maps
  (`disc0:/...` guest paths). If the path does not exist the runtime warns and
  continues with no game data.
- **Set it when:** running against an extracted ISO that is not in `./disc0`.

### `PSPRECOMP_STRICT`
- **Read in:** `runtime/src/psp_dispatch.cpp`, `runtime/src/main.cpp`,
  `runtime/src/hle/psp_hle_dispatch.cpp`
- **Type:** env (`=1` to enable).
- **Default:** off.
- **Effect:** On any `RECOMP_LOOKUP` miss, prints the address and calls
  `std::abort()` instead of returning the noop stub. Boot also probes a known
  invalid address to exercise the path. The HLE dispatch path honors it too.
- **Set it when:** you want a missing function to crash loudly (debugging) rather
  than silently no-op.

### `PSPRECOMP_GE_TEST`
- **Read in:** `runtime/src/main.cpp`
- **Type:** env (`=1` / first char `'1'`).
- **Default:** off.
- **Effect:** Runs the GE renderer self-test after GL init and reports PASS/FAIL,
  then continues booting.
- **Set it when:** verifying the renderer brings up independently of game assets.

### `PSPRECOMP_GE_TEST_ONLY`
- **Read in:** `runtime/src/main.cpp`.
- **Type:** env (`=1` / first char `'1'`). Implies `PSPRECOMP_GE_TEST`.
- **Default:** off.
- **Effect:** Runs the GE self-test, captures a frame (`frame.tga`), and exits
  `0` before the game thread starts.
- **Set it when:** capturing a renderer-only reference frame in CI/headless runs.

### `PSPRECOMP_SCREENSHOT`
- **Read in:** `runtime/src/psp_ge_draw.cpp`
- **Type:** env (path string; any non-empty value).
- **Default:** unset.
- **Effect:** Sets the file path the draw layer writes its captured screenshot
  to.
- **Set it when:** you want the captured frame written somewhere specific.

### `PSPRECOMP_PRESENT_STALE_MS`
- **Read in:** `runtime/src/psp_ge_draw.cpp` (declared in
  `runtime/include/psp_ge_draw.h`)
- **Type:** env (integer milliseconds).
- **Default:** `100`.
- **Effect:** Liveness-present budget. If content is dirty and no real
  page-flip happened within this many milliseconds, the front buffer is
  presented once (a no-flip safety net).
- **Set it when:** tuning the no-flip present heartbeat for a title that flips
  rarely.

### `PSPRECOMP_GEOM_SELFTEST`
- **Read in:** `runtime/src/psp_ge_draw.cpp`
- **Type:** env (any non-empty value).
- **Default:** off.
- **Effect:** Synthesizes exactly one real type-4 (TRIANGLE_STRIP, non-clear,
  non-sprite) draw through the real counter + sentinel path, for the geometry
  verification harness (`scripts/verify_geometry.sh`).
- **Set it when:** running the geometry verification harness.

---

## Scheduler (#66)

### `PSPRECOMP_PREEMPT`
- **Read in:** `runtime/src/psp_scheduler.cpp` (declared in
  `runtime/include/psp_scheduler.h`); the emitter
  (`crates/psp-emitter/src/cpp_generator.rs`,
  `crates/psp-emitter/src/function.rs`) emits the back-edge preemption point
  unconditionally and documents the flag.
- **Type:** env (exactly `1`).
- **Default:** off.
- **Effect:** Default-off, the emitted loop back-edge `sched_preempt(ctx)` call
  only reloads `ctx->preempt_budget` and returns — guest behavior is identical
  to the pre-#66 scheduler. With `=1` it additionally takes a fair cooperative
  yield so a syscall-free busy-poll reaches a reschedule point. Read once and
  cached.
- **Set it when:** evaluating the preemptive-yield path (e.g. a many-boot
  freeze-rate harness, `docs/SCHEDULER-DESIGN.md` §6). Leave unset for the default baseline.

---

## Build options (CMake)

Passed at configure time: `cmake -B <dir> -S runtime -D<NAME>=<VALUE>`.

### `PSPRECOMP_OUTPUT_DIR`
- **Defined in:** `runtime/CMakeLists.txt`
- **Type:** cmake (`PATH`, cache variable).
- **Default:** `${CMAKE_CURRENT_SOURCE_DIR}/../output` (the in-repo `output/`).
- **Effect:** The recompiler output directory the runtime builds against. Must
  contain a generated `CMakeLists.txt` and define the `psp::recomp` alias target,
  or configure fails with the regenerate hint.
- **Set it when:** building the runtime against an output directory outside the repo, e.g.
  `-DPSPRECOMP_OUTPUT_DIR=$WORK/output` (what `games/dissidia/tools/pipeline.sh` passes).

### `PSPRECOMP_GAME`
- **Defined in:** `runtime/CMakeLists.txt` (also referenced by
  `runtime/tools/purity_gate.sh`, `runtime/src/psp_game_default.cpp`,
  `runtime/include/psp_game_module.h`, and every `games/<id>/runtime/*.cpp`
  via the build).
- **Type:** cmake (`STRING`, cache variable).
- **Default:** `dissidia`.
- **Effect:** Selects the `games/<id>/runtime/*.cpp` module compiled into the
  runtime (exactly one strong `psp_game_module()` definition per build).
  `none` (or empty) compiles `src/psp_game_default.cpp` — a pure generic build
  with zero game hooks. An unknown id with no sources is a fatal error.
- **Set it when:** building the generic runtime
  (`-DPSPRECOMP_GAME=none`) or a non-default game module.

### `PSPRECOMP_ALLOW_STALE`
- **Defined in:** `runtime/CMakeLists.txt` (`option()`).
- **Type:** cmake (`option`, `ON`/`OFF`) — see also the pipeline-side env use
  above.
- **Default:** `OFF`.
- **Effect:** Turns the configure-time output-staleness fingerprint mismatch
  into a warning instead of a fatal error.
- **Set it when:** intentionally configuring against a stale `output/`.

> Internal CMake locals (`PSPRECOMP_REGEN_CMD`, `PSPRECOMP_FP_RESULT`,
> `PSPRECOMP_FP_OUTPUT`, `PSPRECOMP_FP_ERROR`) are not user-settable; they hold
> the regenerate-command string and the fingerprint-check sub-process results.

---

## Debug and trace flags (advanced)

Everything below is for investigation only. None is needed for a normal
recompile-build-run cycle. Trace flags exist to gate logging that can produce
gigabytes per minute; probe flags add read-only diagnostics; the remaining
flags toggle individual experimental fixes/band-aids in a game module.

### Generic runtime trace / diagnostics

| Flag | Read in | Activation | Effect |
|------|---------|------------|--------|
| `PSPRECOMP_PC_TRACE` | `runtime/src/psp_dispatch.cpp` (declared `crates/psp-emitter/src/cpp_generator.rs`) | `=1` | Logs every generated-function entry with per-address frequency counting; dumps a summary at exit. No-op otherwise. |
| `PSPRECOMP_GE_TRACE` | `runtime/src/psp_ge.cpp` | first char `'1'` | Enables GE command tracing. |
| `PSPRECOMP_HLE_TRACE` | `runtime/src/hle/psp_hle_dispatch.cpp` (declared `runtime/include/hle/psp_hle.h`); used by `scripts/diff_traces.sh` | `=1` | Every HLE call logs thread name + function name to stderr. Used to produce the runtime trace for the PPSSPP differential. |
| `PSPRECOMP_SEMA_TRACE` | `runtime/src/hle/psp_hle_kernel_sema.cpp` | `=1` | Gates the per-call WaitSema/Signal logs (≈1 GB/min during stalls) behind a flag. |

> `PSPRECOMP_SPLEAK` no longer exists — the shadow-stack sp-leak detector it
> gated was a game-tuned probe deleted in issue #47 Phase 5
> (`runtime/src/psp_dispatch.cpp` retains only the explanatory comment). It is
> listed here only so older notes referencing it are not mistaken for a live
> flag.

### Game-module flags

Flags read only by a game module live with that module in `games/<id>/runtime/` and are
compiled only under its `-DPSPRECOMP_GAME=<id>`. For Dissidia 012 see
`games/dissidia/README.md` and `games/dissidia/docs/steamdeck.md`.

## Where each flag is referenced

Consolidated `PSPRECOMP_*` flags discovered in-tree so contributors do not have to grep the codebase.

| Flag | Where referenced |
|------|------------------|
| `PSPRECOMP_ALLOW_STALE` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_COALESCE_DEBUG` | `crates/psp-cli/src/recompile.rs`, `docs/ENV_FLAGS.md` |
| `PSPRECOMP_CROSS_MID` | `ARCHITECTURE.md`, `CLAUDE.md`, `DEBUGGING.md`, `README.md`, `crates/psp-cli/src/config.rs` |
| `PSPRECOMP_DISC0` | `README.md`, `docs/ADDING_A_GAME.md`, `docs/ENV_FLAGS.md`, `runtime/src/main.cpp` |
| `PSPRECOMP_FP_ERROR` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_FP_OUTPUT` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_FP_RESULT` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_GAME` | `CLAUDE.md`, `DEBUGGING.md`, `README.md`, `runtime/CMakeLists.txt`, `runtime/include/psp_game_module.h`, `runtime/src/main.cpp` |
| `PSPRECOMP_GEOM_SELFTEST` | `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `runtime/src/psp_ge_draw.cpp` |
| `PSPRECOMP_GE_TEST` | `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `runtime/src/main.cpp` |
| `PSPRECOMP_GE_TEST_ONLY` | `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `runtime/src/main.cpp` |
| `PSPRECOMP_GE_TRACE` | `DEBUGGING.md`, `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `runtime/src/psp_ge.cpp` |
| `PSPRECOMP_HLE_TRACE` | `DEBUGGING.md`, `docs/ENV_FLAGS.md`, `runtime/include/hle/psp_hle.h`, `runtime/src/hle/psp_hle_dispatch.cpp`, `runtime/src/hle/psp_hle_kernel_sema.cpp` |
| `PSPRECOMP_NO_COALESCE` | `DEBUGGING.md`, `crates/psp-cli/src/recompile.rs`, `docs/ENV_FLAGS.md` |
| `PSPRECOMP_NO_RA_MODEL` | `DEBUGGING.md`, `crates/psp-cli/src/recompile.rs`, `docs/ENV_FLAGS.md` |
| `PSPRECOMP_OUTPUT_DIR` | `ARCHITECTURE.md`, `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_PC_TRACE` | `DEBUGGING.md`, `crates/psp-emitter/src/cpp_generator.rs`, `docs/ENV_FLAGS.md`, `runtime/src/psp_dispatch.cpp` |
| `PSPRECOMP_PREEMPT` | `crates/psp-emitter/src/cpp_generator.rs`, `crates/psp-emitter/src/function.rs`, `docs/ENV_FLAGS.md`, `docs/SCHEDULER-DESIGN.md`, `runtime/include/psp_scheduler.h` |
| `PSPRECOMP_PRESENT_STALE_MS` | `docs/ENV_FLAGS.md`, `runtime/include/psp_ge_draw.h`, `runtime/src/psp_ge_draw.cpp` |
| `PSPRECOMP_REGEN_CMD` | `docs/ENV_FLAGS.md`, `runtime/CMakeLists.txt` |
| `PSPRECOMP_SCREENSHOT` | `DEBUGGING.md`, `docs/ENV_FLAGS.md`, `docs/GRAPHICS.md`, `runtime/src/psp_ge_draw.cpp` |
| `PSPRECOMP_SEMA_TRACE` | `DEBUGGING.md`, `docs/ENV_FLAGS.md`, `runtime/src/hle/psp_hle_kernel_sema.cpp` |
| `PSPRECOMP_SPLEAK` | `docs/ENV_FLAGS.md`, `runtime/src/psp_dispatch.cpp` |
| `PSPRECOMP_STRICT` | `ARCHITECTURE.md`, `CLAUDE.md`, `DEBUGGING.md`, `README.md`, `docs/ADDING_A_GAME.md`, `runtime/src/hle/psp_hle_dispatch.cpp`, `runtime/src/main.cpp`, `runtime/src/psp_dispatch.cpp` |

Default values and semantics live next to the definitions; prefer those sources of truth when changing behavior.
