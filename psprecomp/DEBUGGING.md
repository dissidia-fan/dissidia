# DEBUGGING.md — psprecomp debugging methodology

The single home for project-specific debugging practices, workflows, commands, failure
patterns, and lessons learned. CLAUDE.md points here; new debugging knowledge goes here,
not into CLAUDE.md. Distilled from ~48 recorded debugging sessions
(`.planning/handoff/next-session/`) and the 2026-06 infrastructure research
(`.planning/research/debug-infrastructure-research.md`).

Audience: humans **and** AI agents. Every workflow below is scriptable without window focus
or GUI interaction unless explicitly noted.

---

## 1. Core principles (violating these has cost real sessions)

1. **No fix without a measured store/branch/value.** Guessing the subsystem ("it's probably
   the X system") produced zero pixels across four phases once. Find the instruction or
   write that diverges first.
2. **Same-snapshot only.** Never track an absolute heap address across boot phases —
   addresses get reused; only within-snapshot diffs are trustworthy. Re-derive pointer
   chains per boot (double-deref!).
3. **Verify without band-aids.** Shims and debug overrides fabricate phantom blockers; always
   reproduce with them disabled before believing a divergence is real.
4. **A probe that doesn't replicate the game's own access conditions will lie.** Verify a
   surprising result a second, independent way before acting on it.
5. **Don't conclude "never" from one run.** Boot is nondeterministic; use ≥120 s windows and
   multiple runs.
6. **Anti-cascade.** If a fix only reveals the next layer, characterize it, file/comment the
   issue, and stop — use the oracle to find the *first* divergence instead of grinding.
7. **Define gates operationally** — by the exact log line and code line, never by a human
   label ("has 3D geometry" once meant something else than its label).
8. **Logs are radioactive.** Trace-enabled runs reach multi-GB. Always `timeout`, grep,
   delete. Never `cat` a runtime log.
9. **One dynamic resource, one agent.** One SDL window, one debug-socket port, one build
   tree. Parallel agents must use worktrees (symlink `disc0/` and `analysis.json`) and kill
   only their own binary path.
10. **Distrust prior conclusions without evidence links.** Six sessions were misled by stale
    handoff framings. Check `.planning/handoff/next-session/<highest>-START-HERE.md` §0
    corrections and §8 don't-re-chase before re-opening any closed lead.

## 2. Build & verification commands

# Full Dissidia 012 build (EBOOT + overlays) and run: see games/dissidia/README.md
games/dissidia/tools/pipeline.sh <workdir>
games/dissidia/tools/run.sh <workdir>            # log: <workdir>/last_run.log

# Rust pipeline (single binary by hand)
cargo build --release && cargo test --release
PSPRECOMP_CROSS_MID=1 cargo run --release -- recompile analysis.json \
    --config games/<id>/game.toml -o output

# Runtime (Release for verification, Debug for lldb). PSPRECOMP_GAME defaults
# to "dissidia" (compiles in games/dissidia/runtime/); -DPSPRECOMP_GAME=none
# builds the pure generic runtime — zero game hooks, used for quarantine runs
# (a generic-run log must contain NO game hook/override lines).
cmake -B runtime/build -S runtime && cmake --build runtime/build -j$(sysctl -n hw.ncpu)
cmake -B runtime/build-debug -S runtime -DCMAKE_BUILD_TYPE=Debug
cmake --build runtime/build-debug -j$(sysctl -n hw.ncpu)

# Unit gates
./runtime/build/test_vfpu                                              # 118 tests
runtime/tests/run_vfpu_semantics.sh --iters 2000                       # VFPU vs PPSSPP interpreter, bitwise (needs a PPSSPP checkout)

# Purity gate (#46/#47 P5): proves the runtime core is game-free. Checks
# (1) source literals: every 0x08xxxxxx/0x09xxxxxx under runtime/src +
#     runtime/include is one of five allowlisted class-(b) PSP constants;
# (2) nm over the build's core object files (games/ objects excluded):
#     no defined OR undefined FUN_0*/game-module symbol.
# Canonical run is against a generic build; the everyday game build is
# also valid input (continuous check).
cmake -B runtime/build-none -S runtime -DPSPRECOMP_GAME=none
cmake --build runtime/build-none -j$(sysctl -n hw.ncpu)
./runtime/tools/purity_gate.sh runtime/build-none   # prints PASS/FAIL per check
./runtime/tools/purity_gate.sh runtime/build        # game build, same invariant

# Log gates for a verification run (grep the run log, then DELETE it):
# sentinel-only LOOKUP_MISS; 0 "marking dead"; [GE_GEOM_HEARTBEAT] monotonic.

# VISIBLE-OUTPUT gate (a prim-count gate CANNOT see a black screen: a broken
# transform can submit thousands of real_nonsprite prims that all collapse to a
# sub-pixel sliver, leaving the frame black while real_nonsprite stays high —
# #27 turned a title screen black this way). During a run, grab a frame
# through the debug socket and assert it is actually VISIBLE:
printf 'S /tmp/frame.tga\n' | nc -w 15 127.0.0.1 9999
python3 runtime/tools/visible_output_gate.py /tmp/frame.tga
# PASS == distinct colors >= 8 AND non-black >= 1% (defaults). A black grab
# (1 color, 0%) FAILS the gate. DELETE /tmp/frame.tga after.

**Stale-output trap:** `output/` is gitignored. After ANY emitter change you MUST
regenerate (`recompile`) before building the runtime, and `PSPRECOMP_CROSS_MID=1` must be
set at recompile time. This check is now **mechanical** (issue #36): the runtime's CMake
configure step diffs `output/fingerprint.json` against the live emitter sources and fails
with the regeneration command on mismatch; every run log starts with the
`[RT] output fingerprint:` line including the cross_mid flag (see "#36 — build
fingerprinting" below). Symptom of slipping past it (`-DPSPRECOMP_ALLOW_STALE=ON`, or a
build dir configured before the staleness appeared): measurements that contradict the code
you just changed — re-run a bare `cmake -B runtime/build -S runtime` to re-check.

## 3. Triage by symptom

| Symptom | First moves |
|---|---|
| **LOOKUP_MISS** | Function not in dispatch table. Check analysis.json; `[LOOKUP_MISS_CTX]` dumps args + 32-entry func ring; `PSPRECOMP_STRICT=1` aborts at first miss for a backtrace. |
| **SIGSEGV** | Address-masking issue. Check the access is within PSP_MEM_SIZE (128MB, mask `0x07FFFFFFU`). lldb on the Debug build. |
| **Hang/stall** | `lldb` → `process interrupt` → `bt`; `psp_threads` (custom command) for PSP thread wait states. Mid-entry dispatch: check `ctx->entry_point` set/cleared. Sema STUCK warns appear rate-limited. |
| **Wrong branch / wrong data** | Oracle-driven differential debugging (§4). Hardware watchpoint on "the value that should change but doesn't". |
| **No render / wrong render** | `PSPRECOMP_GE_TRACE=1`; geometry sentinels (`[GE_GEOM_VERDICT]`, heartbeat); matrix fallback notices; `PSPRECOMP_SCREENSHOT=/tmp/f.tga`; compare vs PPSSPP frame. |
| **Build failure in `output/generated/`** | Fix the Rust emitter and regenerate — NEVER edit generated files. |
| **Linker errors** | funcs.h vs batch names; `_ADDR` dedup suffixes; CMake glob must be `batch_*.cpp` only. |

## 4. Oracle-driven differential debugging (the method for hard data/render bugs)

PPSSPP is a scriptable behavioral oracle, not a passive reference:

1. **Drive it**: launch GUI PPSSPP with the game image (debugger WebSocket auto-listens —
   find the port: `lsof -i -P | grep -i PPSSPP | grep LISTEN`). `game.reset` arms
   breakpoints before one-time boot events; `input.buttons.press` advances menus. Some
   builds drop the `cpu.resume` reply — poll `cpu.status`.
2. **Capture ground truth**: exec-bp a function to read args; one-shot-bp the caller's `ra`
   for return values; memory-write-bp a field to find its writer.
3. **CAVEAT**: the current PPSSPP build **crashes under exec breakpoints/stepping**
   (CoreAudio). Poll-only: memory reads, `press`, memory-write `watch`. Relaunch with the
   ISO to replay boot.
4. **Diff same-snapshot only** (principle 2).
5. **psp-reclass** (`~/Projects/PersonalWork/psp-reclass`): dual-target memory bridge
   (PPSSPP WebSocket + our debug socket): `read`, `read-struct`, `follow` (pointer chains),
   `diff`, `watch`, `press`; `--json` on everything for agents.
6. **A/B with git stash** settles "what did this change really do" disputes decisively.
7. **Differential unit harness**: when an operation's semantics are in doubt, build a tiny
   host harness linking our implementation against PPSSPP's source semantics (the VFPU bug
   was pinned this way without any emulator run).

**Tooling verdict (2026-06)** — full rationale in
`.planning/research/ppsspp-rpc-mcp-research.md`:

- No new PPSSPP RPC bridge and no self-built MCP server. PPSSPP's WebSocket debugger is
  the daemon; psp-reclass is the agent-facing client/bridge over it (and over our debug
  socket, §6).
- psp-reclass's runtime-socket client read the v2 framed protocol (§6) as unframed —
  header bytes spliced into every payload, silent corruption. The fix lives on its branch
  `tooling/framed-runtime-client`.
- Third-party `dmang-dev/mcp-ppsspp` MAY be used as a convenience for PPSSPP-side
  poll-safe ops, under hard conditions on this crash-prone build: NEVER use its pause /
  step / breakpoint_* / screenshot tools (screenshot silently sends `cpu.stepping` — the
  documented crash path, item 3 above). It cannot reach the runtime debug socket, so it
  does not replace psp-reclass for differential/`diff` workflows.
- PPSSPP `gpu.buffer.*` events (including screenshot capture) hard-require
  `CORE_STEPPING_CPU` — the same crash path on this build. `gpu.record.dump` is the safe
  frame-capture route.

## 5. lldb recipes

Launch, don't attach (macOS denies attach). `runtime/.lldbinit` auto-loads
`runtime/tools/psp_lldb.py` (`psp_thread`, `psp_threads`, `psp_module`, `psp_sema`,
`psp_heap`). `g_rdram` symbol is `__ZL7g_rdram`.

```bash
lldb ./runtime/build-debug/psprecomp_runtime
(lldb) b FUN_0881E7A8                                   # recompiled function
(lldb) b hle_sceKernelCreateThread                      # HLE stub
(lldb) p/x ctx->r[4]                                    # a0 (r[2]=v0, r[29]=sp, r[31]=ra)
(lldb) memory read rdram+(0x089F69B4&0x07FFFFFF) -c 32  # PSP memory (masked)
(lldb) b FUN_0881E558 -c 'ctx->r[4] == 0x09000000'      # conditional
(lldb) watchpoint set expression -- rdram+(0x089F69B4&0x07FFFFFF)  # write watch
(lldb) process interrupt                                 # catch a stall
```

Landmines: counting breakpoint *hits* must grep `frame #0:` lines only (command echoes were
once counted as hits); a `/tmp/dis.py` shadowing a stdlib module once corrupted lldb reads;
`PC_TRACE`'s atexit dump won't fire on SIGKILL (heartbeat sentinels are fflushed and do
survive).

**Tripwire pattern** for rare, lldb-suppressed races: plant a one-shot in-process state dump
at the symptom site and let normal runs capture it.

## 6. Runtime debug socket (TCP 127.0.0.1:9999)

Started automatically (`runtime/src/psp_debug_socket.cpp`). One command per line via `nc`;
multiple concurrent clients supported. **v2 framing:** every command replies
`OK <len>\n` + `<len>` payload bytes, or `ERR <reason>\n` — malformed input is never
silently dropped. Full protocol details in the issue #35 section below.

| Command | Effect | Reply |
|---|---|---|
| `I` | Runtime info (uptime, GE counters, LOOKUP_MISS, recent funcs, threads) | `OK <len>` + one JSON line |
| `R <hexaddr> <decsize>` | Read masked PSP memory (≤65536 bytes; out-of-range zero-filled) | `OK <size>` + raw bytes |
| `RAW <hexaddr> <decsize>` | Legacy v1 read — unframed | raw bytes only |
| `W <hexaddr> <hexbytes>` | Write bytes into rdram (masked; whole range must be in bounds) | `OK 0` |
| `B <hexmask> <decms>` | Inject button mask for duration (UP=10, CROSS=4000, START=8; clamped 60 s) | `OK 0` |
| `S <path>` | Screenshot: render thread writes 480x272 TGA to `<path>` | `OK 0` (≤10 s) or `ERR timeout` |

```bash
printf 'B 10 250\n'   | nc 127.0.0.1 9999   # UP
printf 'B 4000 250\n' | nc 127.0.0.1 9999   # CROSS
printf 'I\n' | nc -w 3 127.0.0.1 9999 | tail -n +2 | jq .   # info as JSON
```

Keyboard (window focus required): arrows=D-pad, X/Enter=CROSS, Z=CIRCLE, A=SQUARE,
S=TRIANGLE, Q/W=L/R, Space=START, Tab=SELECT. Buttons auto-release on focus loss — prefer
the socket for scripted/agent input.

## 7. Trace environment flags

| Flag | Effect |
|---|---|
| `PSPRECOMP_STRICT=1` | Abort on LOOKUP_MISS / unimplemented NID |
| `PSPRECOMP_HLE_TRACE=1` | Per-call HLE arg/ret logging (`[HLE-TRACE]`) |
| `PSPRECOMP_PC_TRACE=1` | Per-function frequency counters + atexit top-20 |
| `PSPRECOMP_GE_TRACE=1` | GE command-stream logging |
| `PSPRECOMP_SEMA_TRACE=1` | Per-call sema logging (off by default — floods ~1 GB/min) |
| `PSPRECOMP_SCREENSHOT=path` | One-shot TGA after first prims + shutdown capture |
| `PSPRECOMP_3D_DIAG=<period>` (+`_FROM=<draw#>`, `_MINN=<min verts>`, `_MAT=1`) | Every <period>-th non-through draw: fb, GL viewport/depth range, depth/cull/tex/blend state, first 3 verts' clip+NDC, optional W/V/P/bone matrices, and an occlusion-query `samples_passed` (0 == nothing visible) |
| `PSPRECOMP_3D_DIAG_BIG=<samples>` | Log only non-through draws that cover >= N pixels (find the layer that blankets the screen); uses `_FROM` |
| `PSPRECOMP_3D_SKIP_VTYPE=<hex,...>` | Drop non-clear draws with these exact vertex types (isolate layers) |
| debug socket `T <n>` | Arm a compact per-draw trace (`[DT]` lines: occlusion `pass=`, fb, depth/blend/tex state, v0 clip pos + colour, vertex addr + raw bytes, lighting-state changes, `BEGIN_LIST`/`PRESENT`/`CLEAR` markers) for the next <n> PRIMs starting at the next display list. Diff a good and a bad frame with it |
| `PSPRECOMP_XP_VT=<hex vtype>` + `PSPRECOMP_XP_MODE=<mask>` | Experiment: for draws with exactly this vertex type force depth test off (1), alpha test off (2), blend off (4), texture off (8) — to see whether a layer that reports `pass=0` is hidden by that state. `PSPRECOMP_GE_RECT_OLD=1` restores the pre-fix (no perspective divide) 3D-sprite expansion for A/B |
| `PSPRECOMP_GE_FAST=0` (alias `PSPRECOMP_GE_LEGACY=1`) | Select the legacy per-PRIM GE path (CPU vertex decode + CPU skinning/lighting, one GL draw per PRIM). **The batched fast path (skinning/lighting in the vertex shader, state-keyed draw batching, ring VBO) is the default on every platform, macOS included.** Debug-socket `G <0\|1>` flips between them at run time |
| `PSPRECOMP_GL_STATS=1` | One `[GLSTATS]` line per second: frames/s and per-frame GL calls by class (draws, uniforms, buffer uploads, texture uploads, binds, state, sync/query) plus the six hottest entry points. Implemented by wrapping glad's function pointers (`psp_gl_stats.cpp`), so every call site is counted at zero cost when unset. `=2` additionally polls `glGetError` after **every** call and logs the first 40 as `[GLERR] 0xNNNN after <function>` - run this on any new driver (Mac) to find the exact offending call |
| `PSPRECOMP_FX_SHADERS=0` / `PSPRECOMP_FX_SHADER_LOG=1\|2` / `PSPRECOMP_FX_SHADER_RELOAD=1` | Custom GLSL effect shaders ([FX_SHADERS.md](FX_SHADERS.md)): draws whose texture-pack id is in `<mods>/fx_shaders/manifest.json` use that entry's fragment shader instead of the uber-shader. `=0` turns the system off; `LOG=1` logs loads, compile errors and `[FXSHADER] match <id> <name>` per effect instance, plus per-second stats; `LOG=2` adds GPU ms/frame of the override draws; `RELOAD=1` re-reads the folder on change. Off (zero cost) without a manifest. Turn it off for `PSPRECOMP_GE_AB=1` (the legacy twin has no overrides) |
| `PSPRECOMP_GE_AB=1` | **Same-frame fast-vs-legacy comparison.** Every render target gets a twin; each PRIM/clear is executed twice with identical state - fast path into the real target, legacy path into the twin - and at every present all targets are read back (native res) and compared. Logs `[AB] present#N target key=...: <n> px differ (>4), max channel delta <d>` and writes `ab_NN_{fast,legacy,diff}.tga` (first 24 bad frames) to `PSPRECOMP_AB_DIR` (default `/tmp`; `PSPRECOMP_AB_THR` = per-channel tolerance, default 4); `[AB] summary` every 120 presents. ~3x slower. Needed because menu/battle animation makes two separate captures differ; `PSPRECOMP_AB_FAULT=1` skips every 7th legacy draw to prove the harness detects divergence |
| `PSPRECOMP_GE_VERIFY=1` / `PSPRECOMP_GE_SELFTEST=<n>` | Per-vertex check of the GPU vertex stage against the CPU reference via transform feedback (clip pos, uv, colour), and n synthetic random vertex formats/weights/lights |
| `PSPRECOMP_GE_RING=sub\|map` | Ring-VBO upload: `glBufferSubData` (default except macOS) or unsynchronised `glMapBufferRange` (default on macOS: Apple's GL-over-Metal makes each `glBufferSubData` into the in-use ring cost ~40 us, 29 ms/frame on Sky Fortress at 4x vs 0.17 ms mapped). Both are made safe by orphaning (`glBufferData(NULL)`) on ring wrap - there are no fences |


### GE fast path on Mac / new drivers (macOS 4.1 core via Metal, Mesa, Steam Deck radeonsi)

The fast path is the default everywhere. It requires only what GL 3.3 core provides: no
`GL_ARB_buffer_storage` (macOS has none), compute/SSBO, geometry shaders, GLSL above 330 or
integer vertex attributes; transform matrices are plain square `mat4` uniforms (no `mat4x3`).
The ring VBO is synchronised by orphaning on wrap, with no fences or `glGetError` per draw.
Verification matrix used (Mesa llvmpipe, `MESA_GL_VERSION_OVERRIDE=4.1 MESA_GLSL_VERSION_OVERRIDE=410`,
i.e. a strict 4.1 core context): Quick Battle menus, grid and a Yuna battle with Energy Ray / aeon summons
under `PSPRECOMP_GE_AB=1 PSPRECOMP_GL_STATS=2` => 0 differing targets over ~2700 presents, 0 GL errors;
`PSPRECOMP_GE_SELFTEST=400` => 0 vertex-stage error (lit + skinned, all weight counts).
**If the fast path looks wrong on a Mac**, run `PSPRECOMP_GE_AB=1 PSPRECOMP_AB_DIR=/tmp/ab PSPRECOMP_GL_STATS=2`
and send `last_run.log` plus the `ab_*_{fast,legacy,diff}.tga` files: the `[AB]` lines say which target diverged
and the `[GLERR]` lines name the GL call; `PSPRECOMP_GE_FAST=0` is the escape hatch meanwhile. GL-call
profile of a Yuna Quick Battle frame (llvmpipe, `PSPRECOMP_GL_STATS=1`): legacy ~3400 calls and ~1060 draws
per frame; fast (old, fenced) ~2100 calls incl. ~950 fence/wait calls; fast (orphaning) ~1250 calls, 0 sync.

Verified 2026-10-16: with the 3D-rect perspective divide, Quick Battle shows slash trails, sparkles, impact flashes and HP-attack cinematics (shots psprecomp_work/shots/3d_new_all.png, 3d_legacy_all.png); with PSPRECOMP_GE_RECT_OLD=1 the same fights show only giant stretched streak garbage (3d_old_all.png, 3dm_old_all.png). Both fast and legacy paths call rect_clip_to_ndc. GE_VERIFY over ~1M draws: 0 error.
| debug socket `A2 <x> <y> <ms>` / `V` | `A2`: hold the RIGHT stick (0..255, 128 centre; the left-stick twin is `A`). `V`: print the GE view matrix (12 floats) as of now (the HUD sets identity: sample several times); with `PSPRECOMP_VM_LOG=<n>` (list address of non-trivial VIEWMATRIXDATA words) and a `/proc/PID/mem` scan of the 128 MB rdram mapping (size >= 128 MB, rw, anonymous) this is how the Dissidia camera object was found (games/dissidia/CAMERA notes in runtime/camera.cpp) |
| `PSPRECOMP_CTRL_LOG=1` | Log the first poll of each distinct sceCtrl buffer address (recompiled code does not maintain `ra`, so the caller is found with `gdb` `break hle_sceCtrlPeekBufferPositive` + `bt`) |
| `PSPRECOMP_CAMX_LOG=<n>` / `PSPRECOMP_ANALOG_LOG=<n>` | Dissidia: log n analogue-camera updates (game yaw, stick offset, eye, target) / n stick updates (raw, the game's 8-way vector, our continuous vector) |
| `PSPRECOMP_ARG_WATCH=<hexfunc>[,<hexoff>]` | Log a0..a3 (+ guest word at a2+off, on change per a2) at every entry of a recompiled function |
| debug socket `M <hexaddr>` | Arm `MEM_WATCH` at runtime for one guest word (no restart; avoids the slowdown of watching from boot). Like the env flag it is only checked at function entries, so the writer is the function entered just before the change. For the exact writer, attach `gdb -p PID`, take the 128 MB rdram mapping base from `/proc/PID/maps`, and use `watch *(float*)(base+(addr&0x07FFFFFF))` with `commands`/`bt`; function-entry breakpoints (`break FUN_xxxxxxxx`) read args from ctx (`$rsi`, r[n] at offset 4*n). Never kill gdb with SIGKILL while attached: it takes the runtime with it; use `timeout` (SIGTERM) or `detach` in a breakpoint command |
| `PSPRECOMP_MEM_WATCH_HI=<hex>` | With `MEM_WATCH`: only log when the new word's top byte equals this (e.g. `5C` = GE AMBIENTCOLOR cmd) |
| (always on) `[FPS]` | GE frames (display flips) per second, every 5 s |
| `PSPRECOMP_BONE_TRACE=1` | First 600 BONEMATRIXDATA words (index, raw, float) |
| `PSPRECOMP_SKIN_DIAG=<n>[,<from>]` | For the first n skinned draws (from PRIM #from): `[SKIN]` line with vtype, weight count, stride, count and a fingerprint of each bone matrix slot in use (+ translation, + how many PRIMs ago the slot was last uploaded), then the first 3 raw vertices at the GE vertex address. Shows what the game really hands the GE (matrix order, vertex stream alignment) independent of the draw path |
| `PSPRECOMP_CROSS_MID=1` | (recompile-time) cross-function mid-entry emission — must match workflow |

Narrow one-off investigation probes from past bugs live with their hooks in the game
module (`games/<id>/runtime/*.cpp`, compiled only under that `PSPRECOMP_GAME`); the
generic core reads none of them. Discover via `rg 'getenv\("PSPRECOMP_' runtime games crates`,
and prefer not to rely on them (issue #43 tracks a proper registry/channel system).

## 8. Static-side oracles

- **pyghidra-mcp** (BOOT.BIN in a Ghidra project): `decompile_function`,
  `list_cross_references`, `search_symbols_by_name`, `gen_callgraph`, `search_code`.
  A Ghidra RPC daemon alongside it was evaluated and rejected (2026-06): its
  differentiators (write-back, patching) conflict with "Ghidra is byte-gate-verified,
  never a byte source" (#52) and have no consumer here — see
  `.planning/research/ppsspp-rpc-mcp-research.md` §7.
- `cargo run --release -- dump analysis.json <mode>` for analysis.json inspection
  (see issue #38 section for single-function C++ dumps).

## 9. Known failure patterns (don't re-discover)

- **PPSSPP crashes when paused/stepping** — poll-only oracle work (§4).
- **Stale `output/`** after emitter changes — §2 trap; regenerate first.
- **`g_last_func_addr` in HLE logs is NOT a return address** — it's the last dispatched
  function; misread twice historically.
- **Band-aid stacks fabricate phantom blockers** — reproduce with them disabled (§1 rule 3).
- **lv.q/VFPU register-file class bugs live in the runtime register file**, not the
  emitter/decoder (verified faithful against raw BOOT.BIN words).
- **The freq-que/at3 audio workers parked on their reqsemas is NORMAL idle state.**
- **A render bug that disappears under `T <n>`, `PSPRECOMP_DRAW_TRACE`, `PSPRECOMP_3D_DIAG*` or `G 0` /
  `PSPRECOMP_GE_FAST=0` lives in the batched fast path**: all of those select the legacy per-PRIM path. Compare
  with `PSPRECOMP_GE_AB=1`; suspect state carried between PRIMs (the stale-`w` HUD smear, below).
- **`verify_geometry.sh`-style oracles need a self-test path** (a synthesized known-good
  draw) to prove they're not constant-false.
- **A loop that never advances inside ONE function (frozen `recent_funcs` ring, one frozen
  register) was the BIDS class** (#56, fixed): a branch into another branch's delay slot
  used to land on an empty label / the swapped branch. The decoder now duplicates the
  delay instruction at the slot's label (`DelaySlotRejoin`); a control transfer in a
  branched-into slot is a loud `BranchInDelaySlot` decode error (stub).

---

# Per-issue infrastructure notes

Sections below are added by the issue that introduced the infrastructure. Each section:
what was added, how to use it, how it was verified, and any new failure modes discovered.

## #35 — Debug socket v2 (ACK framing, info JSON, write, screenshot, multi-client)

**What was added** (`runtime/src/psp_debug_socket.cpp`, hooks wired in `runtime/src/main.cpp`):

- **ACK framing.** Every command replies `OK <len>\n` followed by exactly `<len>` payload
  bytes, or `ERR <reason>\n` with no payload. Malformed/unknown input always gets an ERR
  (v1 dropped it silently, which made scripted probing indistinguishable from a hang).
- **Multi-client.** Thread-per-client accept loop; concurrent `nc` sessions are independent
  (v1 served one client at a time — a second connect hung until the first closed).
- **Legacy compat choice:** v2 frames `R`; the v1 unframed read survives verbatim as the
  `RAW <hexaddr> <decsize>` alias (success replies are raw bytes with no header; malformed
  RAW still gets ERR). Old clients switch `R`→`RAW`, new clients use framed `R`.

### Command reference

| Command | Args | Success reply | ERR reasons |
|---|---|---|---|
| `I` | — | `OK <len>` + one newline-terminated JSON line | — |
| `R` | `<hexaddr> <decsize>` (1..65536) | `OK <size>` + raw bytes; masked `0x07FFFFFF`; out-of-range tail zero-filled | `bad-addr`, `bad-size`, `size-out-of-range` |
| `RAW` | same as `R` | raw bytes, **no header** (v1 behavior) | same as `R` |
| `W` | `<hexaddr> <hexbytes>` (even-length hex, no spaces) | `OK 0` | `bad-addr`, `bad-bytes`, `out-of-range` (whole range must fit — no partial writes) |
| `B` | `<hexmask> <decms>` (clamped 60 s) | `OK 0` | `bad-mask`, `bad-duration` |
| `S` | `<path>` (rest of line, spaces allowed) | `OK 0` after the TGA is on disk | `bad-path`, `unsupported`, `timeout` (10 s; also covers pre-GL boot and a concurrent capture in flight) |
| anything else | | | `unknown-command`, `empty`, `line-too-long` (>4095 chars; connection stays usable) |

### `I` JSON schema (one line, jq-able)

```json
{"uptime_sec": 70.9,
 "ge": {"frames": 1667, "prims": 12696, "real_nonsprite": 11030,
        "sprite_nonclear": 0, "clears": 1666},
 "lookup_miss": {"unique": 1, "total": 1},
 "recent_funcs": ["0x089B440C", "..."],
 "threads": [{"id": 0, "name": "user_main", "status": "RUNNING", "wait": ""},
             {"id": 2, "name": "sgx-psp-freq-thr", "status": "RUNNING", "wait": "sema:278"}]}
```

- `ge.*` mirrors the `[GE_GEOM_*]` sentinel counters (`real_nonsprite > 0` == GRAPHICS).
- `recent_funcs` is a shared 64-entry ring fed by every recompiled-function entry,
  oldest first — entries from all game threads interleave, so cross-thread ordering is
  approximate. (The per-thread PC-TRACE ring is `thread_local` and not readable here.)
- `threads[].status` ∈ DORMANT/READY/RUNNING/WAIT/DEAD/WAIT_SLEEP. `wait` is `sema:<uid>` /
  `semacb:<uid>` / `sleep` / `thread_end:<id>` / `""`. **Caveat:** sema waits block on the
  sema's own condvar without changing scheduler status, so a sema-blocked thread shows
  `status:"RUNNING"` with a non-empty `wait` — trust `wait` over `status` for "is it stuck".
- All reads are deliberately racy snapshots (documented in code); never assert on exact counts.

```bash
printf 'I\n' | nc -w 3 127.0.0.1 9999 | tail -n +2 \
  | jq '{frames: .ge.frames, waits: [.threads[] | select(.wait != "") | {name, wait}]}'
printf 'W 9FFFF00 DEADBEEF\n' | nc -w 3 127.0.0.1 9999       # OK 0
printf 'R 9FFFF00 4\n' | nc -w 3 127.0.0.1 9999 | xxd        # OK 4 + dead beef
printf 'S /tmp/frame.tga\n' | nc -w 12 127.0.0.1 9999        # OK 0 once written
```

**Screenshot path obeys the GL rule:** the socket thread only posts a request; the main
(GL) thread polls `ge_draw_service_screenshot_request()` each event-loop pass next to
`render_queue_process()` and does the FBO readback there. Give `nc` a `-w` ≥ the service
latency (capture is usually <50 ms once GL is up; the 10 s bound covers pre-GL boot).

**Failure modes discovered during verification:**
- `S` before `ge_draw_init` (first ~1 s of boot) fails fast → `ERR timeout`; retry later.
- v1's `strtoul` parsing accepted garbage as address 0 — v2 rejects empty/garbage tokens
  explicitly (`bad-addr`/`bad-size`), so don't rely on `R zz 4` reading address 0 anymore.
- `W` refuses partially-out-of-range writes entirely (no partial write), unlike `R` which
  zero-fills — asymmetry is intentional.

**How verified:** `runtime/build/test_debug_socket` (59 checks: framing, ERR paths, W/R
round-trip, masking, JSON shape + hostile-name escaping, two concurrent clients, oversized
line) plus a live run — `I` during boot and at the title, `W`+`R` round-trip at
`0x09FFFF00`, `S` produced a 480x272 TGA mid-run, `B 4000 250` → `OK 0`,
malformed commands → ERR, two simultaneous `nc` sessions served concurrently.
## #36 — build fingerprinting (stale-output detection)

Every `psprecomp recompile` writes `<output>/fingerprint.json` and
`<output>/include/recomp_fingerprint.h`. The runtime's CMake configure step verifies the
fingerprint; the runtime prints it at boot. Together they make the stale-output trap (§2)
mechanical instead of procedural.

### What the fingerprint covers

- **Emitter sources** (`emitter_sources_hash`): content hash over every `*.rs` under
  `crates/psp-emitter/src`, `crates/psp-decoder/src`, `crates/psp-ir/src`,
  `crates/psp-optimizer/src`, plus the psp-cli files that shape emission
  (`recompile.rs`, `config.rs`, `hle_entry_scanner.rs`). Content hashing catches
  **uncommitted** edits — a git commit hash cannot.
- **analysis.json**: SHA-256 of the input file's bytes (re-analyze without recompile is
  also stale).
- **Flags**: `cross_mid` — whether `PSPRECOMP_CROSS_MID=1` was set at recompile time
  (previously invisible after the fact).
- **Context** (not hashed): ISO-8601 timestamp, git commit + dirty flag, headline counts
  (functions / mid-entries / batch files).

**NOT covered:** `runtime/` sources (CMake rebuilds those itself), the NID database, game
config TOMLs passed via `--config`, and Cargo dependency versions (`cargo update` that
changes codegen is invisible). A stale **build dir** configured before the staleness
appeared also escapes until the next configure — `cmake --build` alone does not re-check.

### Hash recipe (v1) and why content hashes are listed per file

Per-file SHA-256 of raw bytes; combined hash = SHA-256 over `"{path}\n{hash}\n"` sorted by
repo-root-relative path. Deterministic: same trees → same hash; timestamps and git state do
not feed it. fingerprint.json embeds the **full per-file list**, and the configure check
recomputes hashes for exactly those files — so the Rust and Python sides can never disagree
about the file set. The recipe roots are embedded too, so files *added* after the recompile
are still detected. Authoritative recipe doc: module header of
`crates/psp-cli/src/fingerprint.rs`; mirror: `runtime/cmake/check_fingerprint.py`.

### Boot line (grep-stable, first line of every run log)

```
[RT] output fingerprint: <sha256> (cross_mid=1, recompiled 2026-06-12T06:18:21Z)
```

`unavailable (output/ predates issue #36)` appears for pre-fingerprint output dirs.

### Configure-time check, failure message, override

On mismatch, `cmake -B runtime/build -S runtime` fails with:

```
output/ is stale relative to the emitter sources — run:
    PSPRECOMP_CROSS_MID=1 cargo run --release -- recompile analysis.json --config games/<id>/game.toml -o output
STALE: emitter sources changed since output/ was recompiled (recorded <hash>..., actual <hash>...)
  changed: crates/psp-emitter/src/lib.rs
```

Override with `-DPSPRECOMP_ALLOW_STALE=ON` → configures with a loud warning instead.
**The option is cached** — pass `-DPSPRECOMP_ALLOW_STALE=OFF` to re-arm an existing build
dir. Missing `fingerprint.json` (pre-#36 output dirs) and unverifiable states (sources not
on disk at recompile time, missing Python3) are warnings, never errors.

### Verified (2026-06-12)

Fresh recompile → configure printed `output/ fingerprint OK`, build + boot printed the
banner. Appending a comment to `crates/psp-emitter/src/lib.rs` without recompiling →
configure exited 1 with the message above naming the file; `-DPSPRECOMP_ALLOW_STALE=ON` →
exit 0 with the warning; revert + recompile → `fingerprint OK` with the **identical**
combined hash (determinism). Removing fingerprint.json → warning, configure + build OK.


## #47 P2 — recomp_module.h (generated module facts)

Every `psprecomp recompile` also writes `<output>/include/recomp_module.h` from the
analysis.json `module{}` block (plus `segments[0]`, `heap_base`, `constructors[]`):
`RECOMP_MODULE_NAME/ENTRY/GP/TEXT_START/TEXT_SIZE`, `RECOMP_SEG0_VADDR/MEMSZ`,
`RECOMP_HEAP_BASE`, `RECOMP_CTOR_COUNT/FIRST_CTOR`. The runtime boot path (NativeModule
block, boot GP, bump-heap base, boot probes, `sceKernelGetModuleIdByAddress` text range)
hard-includes it — per-game constants no longer live in runtime sources. It regenerates
with the output dir; staleness is covered by the #36 fingerprint (analysis.json hash).

`module{}` is produced by `analyze` for **both** ET_EXEC and PRX inputs from the same
SceModuleInfo parser path (`crates/psp-cli/src/prx_load.rs::build_module_facts`).
Text extent follows PPSSPP `ElfReader` semantics (`psp-parser elf::text_extent`).

### Differential note: text_size

`RECOMP_MODULE_TEXT_SIZE` follows PPSSPP. The exec phdr's `p_filesz`
(`ElfReader::GetTotalTextSizeFromSeg`) is used only for **sectionless** inputs. With section
headers PPSSPP computes `GetTotalTextSize()` = sum of SHF_ALLOC, non-WRITE, non-STRINGS
section sizes, and that is what a game reads via `sceKernelQueryModuleInfo`. A runtime that
hardcoded the phdr size was wrong; the generated fact is PPSSPP-faithful.

The boot ctor probe uses `RECOMP_FIRST_CTOR` + `psp_dispatch_probe_lookup` (nullable), so it
actually checks. `constructors[]` is unsorted, and a `RECOMP_LOOKUP(...) == nullptr` check
can never fire (non-STRICT lookup returns a noop stub).

### Upgrading an analysis.json baseline (#47 Phase 2)

`recompile` **hard-errors** on an analysis.json without `module{}` (anything analyzed
before Phase 2) and on an empty `imports[]` (anything analyzed before the ELF import
walker, issue #40 — the runtime would boot with zero HLE bindings). For a normal
baseline, just re-run analyze:

```bash
cargo run --release -- analyze --ghidra-dir $(brew --prefix ghidra)/libexec \
    disc0/PSP_GAME/SYSDIR/BOOT.BIN -o analysis.json
```

**Force-mid-entry collision guard.** A fresh analyze can promote addresses to standalone
functions or move bodies into a different function. `game.toml` force-mid-entries authored
against the old layout would then emit cross-function `goto`s (`error: use of undeclared
label`, CLAUDE.md #9). `inject_force_mid_entries` (`crates/psp-cli/src/recompile.rs`)
resolves each force-mid-entry's *actual* owning function: it skips entries that are
themselves a discovered standalone function (reached via `RECOMP_LOOKUP`) and re-points
entries whose declared parent is stale to their real owner. When the declared parents
already match, the guard is a no-op.

## #46/#47 P4 — per-game manifest + game module (PSPRECOMP_GAME)

A game's curated layer lives under `games/<id>/` (ARCHITECTURE.md "Per-Game Layer"):
`game.toml` feeds `recompile --config` (force entries; `[game]/[boot]/[module]/[runtime]`
choices → generated `recomp_game_config.h`), and `games/<id>/runtime/*.cpp` holds the
address-keyed hooks, compiled in via the CMake cache var `PSPRECOMP_GAME` (default
`dissidia`; `none` = pure generic build).

Debugging handles:

- **Boot banner**: `[RT] game module: <id>` is the second log line; a
  `game-module mismatch` WARNING means the compiled-in module id differs from the
  output dir's manifest id (`RECOMP_GAME_ID`) — wrong-game hooks silently corrupt, so
  always resolve the warning before trusting a run.
- **Quarantine gate** (generic-build sanity): a `-DPSPRECOMP_GAME=none` run against ANY
  output must contain zero lines printed by the game module's hooks. Misses the game module
  would have handled surface as loud `[LOOKUP_MISS]` lines instead — expected and
  informational.
- **Forgot the manifest?** Recompiling WITHOUT `--config games/<id>/game.toml` silently
  drops its force entries and force mid-entries — counts shift and `--expect-*` flags catch
  it. The mismatch warning also fires (output id `""` vs the compiled-in module id).

## #37 — recompile_report.json (silent-path audit)

Every `psprecomp recompile` run writes `<output>/recompile_report.json` and prints a
one-paragraph summary. The report turns the pipeline's four historically *silent* failure
paths into named, counted artifacts — read it first when triaging a new game or an
unexplained runtime divergence.

### The four silent paths it exposes

| Path | What used to happen | Report field |
|------|--------------------|--------------|
| Unhandled relocation types | `reloc.rs` left the word untouched for anything other than R_MIPS_32/26/HI16/LO16 — unrelocated pointers with no trace | `unhandled_relocations` (r_type → count); also warned once per type at analyze time |
| Unresolved NIDs | a database miss silently became a `NID_0x%08X` stub name | `unresolved_nids[]` (nid, stub_addr, fallback_name) |
| Decode errors | function silently emitted as an empty stub with a comment | `decode_errors[]` (address, name, error) |
| Unbacked static dispatch targets | emitter generated `RECOMP_LOOKUP(0xADDR)` for a target absent from the dispatch table — a guaranteed LOOKUP_MISS if reached | `dispatch_audit.missing_targets[]` |

### Schema (schema_version 1)

Top-level keys: `schema_version`, `generated_at` (ISO-8601 UTC), `counts`,
`decode_errors[]`, `unresolved_nids[]`, `unhandled_relocations{}`, `dispatch_audit{}`,
`dedup_renames[]`. All addresses are hex strings (`"0x%08X"`, analysis.json style);
`unhandled_relocations` keys are decimal r_type values as strings.

`counts` carries the self-check numbers: `functions_total`, `functions_by_source`
(analysis.json `source` field), `discovery` (recompile-stage pass breakdown:
force/raw_scan/prologue/gap_start/gap_rescued), `mid_entries`, `batch_files`,
`imports_total`, plus a total per silent-path category. `dedup_renames[]` lists every
function the emitter renamed with the `_ADDR` suffix for ODR safety (the dispatch/funcs.h
name to grep for is `unique_name`, not the Ghidra name).

Authoritative schema doc: module header of `crates/psp-cli/src/report.rs`.

### Count assertions (--expect flags)

```bash
PSPRECOMP_CROSS_MID=1 cargo run --release -- recompile analysis.json \
    --config games/<id>/game.toml -o output \
    --expect-functions <N> --expect-mid-entries <M>   # the game's known census
```

On mismatch, recompile exits nonzero with a clear error — *after* writing the report, so
the counts breakdown is available for triage. Both flags are optional; use them in scripts
and verification gates to turn the documented baseline into an assertion instead of a
doc-comment that drifts.

### Triaging a new game with the report

1. Recompile without expect flags; read the printed summary line.
2. `unhandled_relocations` non-empty (PRX games) → relocation support gap; affected code
   reads unrelocated pointers. Fix in `psp-parser/src/reloc.rs` before chasing runtime bugs.
3. `unresolved_nids` non-empty → update the NID database; each listed stub is an import the
   runtime can only no-op.
4. `decode_errors`: cluster the addresses. A contiguous run of entries with ASCII-looking
   words (e.g. `0x44555453` = "STUD") is data misdetected as code by heuristic discovery —
   harmless stubs. A decode error in a *Ghidra-sourced* function is a real decoder gap.
5. `dispatch_audit.missing_targets`: targets **inside** the loaded segment range are real —
   each is a guaranteed LOOKUP_MISS if reached (cross-check against runtime LOOKUP_MISS
   logs). Targets outside the segment range are artifacts of data words misdecoded as `j`
   inside heuristically recovered functions (same root cause as the decode-error cluster).

## #40 — generated syscall table (NID import bindings)

Every `psprecomp recompile` writes `<output>/syscall_table.cpp`: one row per
analysis.json `imports[]` entry (stub address, NID, resolved name, library), sorted by
stub address, header stamped with the analysis.json sha256. The runtime's
`psp_hle_init()` walks `recomp_nid_stubs` (decls: `runtime/include/hle/psp_hle_imports.h`)
and binds HLE handlers **by name** at the stub addresses — the hand-maintained
`runtime/include/hle/psp_hle_syscall_table.h` is gone, and the runtime contains no
per-game stub addresses. Runtime code that needs a stub keys on the NID (a universal PSP
API constant) via `psp_hle_stub_addr_for_nid(nid)` — e.g. the GE sub-intr replay in
`psp_hle_ge.cpp`.

Pieces to know when debugging:

- **Boot line** (unchanged format): `[HLE] Import stubs: X/Y implemented, Z unimplemented`.
  A sudden drop means HLE
  registration names diverged from NID-db names — diff the generated table's `func_name`
  column against `psp_hle_register` calls.
- **Unimplemented imports are loud, never silent — and return a deterministic v0=0**:
  each unbound stub is registered with a per-stub handler that logs
  `[HLE] UNIMPLEMENTED import <name> (NID 0x..., module ..., stub 0x...)` once on first
  call and sets `v0 = 0` (SCE_OK). It originally mirrored the raw stub (`jr $ra; nop` —
  no register effects), which made every consumer of the result branch on leftover r2
  garbage (e.g. `sceKernelGetThreadCurrentPriority` garbage once fed a main wait
  loop's branch). If a game needs a specific non-zero return, that is the signal to
  implement the NID, not to special-case the stub. Unresolved NIDs carry the walker's
  canonical fallback name `NID_0x%08X`; an HLE handler for such an import registers under
  exactly that name (example: `psp_hle_power.cpp` registers `NID_0xEBD177D6`).
- **Call sites**: the emitter lowers `jal <import stub>` to
  `RECOMP_LOOKUP(0xADDR)(rdram, ctx); /* sceName */` — through the dispatch table, never a
  direct named call (the by-name HLE symbols do not exist; psp_hle_init's registration is
  what makes the lookup hit the handler). The #37 dispatch audit therefore counts import
  stub addresses as registered.
- **NID 0xEBD177D6** is missing from data/niddb. PPSSPP `Core/HLE/scePower.cpp` identifies
  it as scePowerSetClockFrequency (exported alias "scePower_EBD177D6"); the walker emits
  `NID_0xEBD177D6` and the runtime handler registers under that name, returning SCE_OK.
- **recompile hard-errors on an empty `imports[]`** (analysis.json predating the ELF
  import walker): the runtime would otherwise boot with zero HLE bindings. Fix: re-analyze.

## #38 — `dump <analysis.json> 0xADDR` (single-function C++ emission)

Emits exactly one function's generated C++ to stdout — no full recompile, no grepping
batch files, no writes under `output/`. The list modes are unchanged
(`dump analysis.json functions|imports|relocations|segments|mid_entries`, positional or
`--what`; default `functions`).

```bash
# Module start (named "entry" in emitter output)
PSPRECOMP_CROSS_MID=1 cargo run --release -- dump analysis.json 0x089ACCD0

# Ordinary function; 0x prefix optional, hex case-insensitive
PSPRECOMP_CROSS_MID=1 cargo run --release -- dump analysis.json 0881e7a8 > /tmp/f.cpp

# Mid-entry address -> actionable error on stderr, exit 1:
#   Error: 0x08804CE8 is a mid-entry inside FUN_08804a4c (entry 0x08804A4C); ...
#   dump the parent: psprecomp dump <analysis.json> 0x08804A4C
cargo run --release -- dump analysis.json 0x08804CE8
```

Address resolution is against the **post-pipeline** function list (after discovery,
force-injection, and the CROSS_MID passes). Non-entry addresses never silently emit the
wrong thing: a registered mid-entry names its parent; an address strictly inside a body
names the containing function; an unknown address lists the nearest entries below/above;
bad hex is a usage error. Errors and all tracing go to stderr — stdout carries only C++,
so it pipes/redirects cleanly.

**Fidelity / CROSS_MID note:** the dump goes through the same `prepare_emission` +
`emit_one_function` path as the batch emitter (same IR, same global `_ADDR` name dedup,
same mid-entry dispatch), so its output is **byte-identical** to the function's text in
its `batch_*.cpp` for the same flag set. That means `PSPRECOMP_CROSS_MID` (and
`PSPRECOMP_NO_COALESCE`/`PSPRECOMP_NO_RA_MODEL`, and `--config`) must match whatever the
`output/` you are comparing against was generated with — CROSS_MID changes coalescing and
mid-entries, which changes bodies. Pipeline prep takes a few seconds (it re-runs discovery,
and under CROSS_MID the coalesce/recovery decode passes) before the single function emits.

**How verified:** unit tests in `crates/psp-cli/src/dump.rs` compare the dump path against
in-memory batch emission for the real analysis.json (entry, an ordinary `FUN_`, a mid-entry
parent, an `_ADDR`-deduplicated thunk — all byte-identical substrings), plus end-to-end CLI
runs with `PSPRECOMP_CROSS_MID=1` on both sides: dumps of `entry`, a mid-entry parent, a
deduplicated thunk and an ordinary function were each exact byte substrings of their
`output/generated/batch_*.cpp`.

## #52 — PRX bring-up (rebase, relocations, imports, byte-equality gate)

Relocatable PSP modules (`e_type 0xFFA0` — e.g. a decrypted `EBOOT.BIN` of a game whose
`BOOT.BIN` is a dummy) take a different analyze path than ET_EXEC binaries:
psp-parser rebases all segments to `PSP_USER_MODULE_BASE` (0x08804000, override with
`analyze --load-base <hex>`), applies the Type-A relocation tables, parses SceModuleInfo +
the `.lib.stub` import table from the *relocated* image, and pins Ghidra's loader
(`-loader PspElfLoader -loader-imagebase <hex>`) so both engines see the same image.

### Diagnosing a base-0 / no-imports analysis (the original #52 symptom)

Any of these in an analyze run of a PRX means the rebase path did not engage — suspect a
stale cache (below) or a non-0xFFA0 e_type:

- functions named `FUN_000xxxxx`, `entry` missing, or a stray `FUN_00000000`
- `heap_base` ≈ `0x005xxxxx` instead of `0x08Dxxxxx`
- `Applied 0 relocations` / `Resolved 0 import stubs` in the log (import failures on a
  PRX are now hard errors, never silent empty arrays)
- 0 RAW_SCAN xrefs and 0 constructors (the Java script's PSP-range gates see base-0 addresses)

### "byte-equality gate FAILED for block ..."

`ExtractAnalysis.java` exports a SHA-256 per initialized memory block; analyze recomputes
each hash over its own relocated `segments[]` slice and hard-fails on mismatch. A failure
means the two relocation engines (psp-parser `reloc.rs` vs ghidra-allegrex) diverged —
**do not** weaken the gate; one of the engines is wrong. To find which: dump the named
block's first differing bytes from Ghidra (re-import the binary in the GUI with the same
imagebase) and compare against the analysis.json segment slice at the same VA
(`jq -r '.segments[0].data_b64' | base64 -d | xxd`). Plan
`.planning/plans/52-prx-support-plan.md` §1 D1 records the fallback strategy (pre-relocated
temp ELF) if a divergence cannot be fixed in `reloc.rs`. A *warning* that the gate was not
enforced means the `blocks` key is absent — legacy `ghidra_raw.json` or a `--ghidra-dir`-less
run.

### `.ghidra_raw.meta.json` cache rule

The `<output>.ghidra_raw.json` cache is reused only when its sidecar
`<output>.ghidra_raw.meta.json` matches the run (binary sha256, loader, imagebase) — a
pre-#52 base-0 cache can therefore never silently poison a rebased re-run. Mismatch or
missing meta: with `--ghidra-dir`, Ghidra re-runs (one-time cost for legacy caches);
without it, analyze errors and tells you to re-run with `--ghidra-dir` or delete the cache.

### Type-B relocations

`0x700000A1` packed relocation tables (stripped kernel-style PRX) are detected but
deliberately unimplemented: analyze fails loudly naming the table. Spec for a future
implementation: `.planning/research/52-prx-format-spec.md` §5.

## #57 — byte-reproducible recompile output (emitter-change byte-diff gate)

Two recompiles of the same analysis.json with the same binary produce a byte-identical
output directory — `diff -r out1 out2` shows ONLY the three timestamp lines
(`generated_at` in fingerprint.json + recompile_report.json, `RECOMP_FINGERPRINT_TIMESTAMP`
in include/recomp_fingerprint.h). This holds under `PSPRECOMP_CROSS_MID=1` too: the
coalesce pass registers absorbed-sibling mid-entries sorted by address (it previously
iterated a HashMap, scrambling the wrapper order in mid_entries.cpp / funcs.h /
dispatch.cpp on every run). Byte-diff against a master-built output is therefore a valid
emitter regression gate with no sort-normalization needed; any other residual diff is a
real behavior change. Regression test:
`coalesce_mid_entry_registration_is_deterministic_and_sorted` (crates/psp-cli/src/recompile.rs).

## LwMutex HLE (runtime/src/hle/psp_hle_kernel_lwmutex.cpp)

`sceKernelCreate/Delete/Lock/LockCB/TryLock/UnlockLwMutex`, PPSSPP-faithful
(`Core/HLE/sceKernelMutex.cpp`). The part that bites when debugging: **the LwMutex's
state is GUEST-visible** — a 32-byte workarea the game allocated (`+0` lockLevel,
`+4` lockThread = the `sceKernelGetThreadId` UID of the owner, `+8` attr,
`+16` kernel-object uid; `lockThread` 0 = free, −1 = deleted; `uid` −1 = invalid).
Read it live over the debug socket to see lock state — there is no host-side mirror to
trust; every HLE decision derives from those words. The host object exists only to block
waiters (one global host mutex serializes all transitions; per-object condvar; 5 s
safety valve + `g_should_exit` escape; `wait` tag `lwmutex:<uid>` in the socket `I`
thread dump). `Create` returns **0**, not the uid (the uid lives in the workarea — a
game comparing v0 against the uid is misreading the API, not hitting a runtime bug).
Recursion requires attr `0x200`; a second lock without it returns
`SCE_LWMUTEX_ERROR_ALREADY_LOCKED` (0x800201CF), exactly like PPSSPP.

### Stage vanishes (flat grey) in Dissidia battles -- VFPU `1 << imm5` overflow (fixed)

Symptom: stage sky/floor drawn but invisible, only characters + HUD on grey, mostly while
"EX Core in play"/assist prompts show (a stage-ambient fade runs); some frames washed out.
`T <n>` showed the draw list identical to a good frame, but every stage draw had `pass=0` and
v0 colour `00000000`: lighting is on with material-ambient = vertex colour, AMBIENTCOLOR/ALPHA
were `000000/00` in the list (good: `808080/FF`), so lit colour + alpha = 0 and the alpha test
discards everything (the GE is faithful). The list builder (FUN_089A0C20) takes the colour from a
fade object (+0x44) that FUN_089A48CC packs from a float4 lerp. The lerp endpoints were
negative because `vi2f ..., 31` (unpack 0xAABBGGRR via vuc2i+vi2f) computed `1 << 31` as an
`int` (INT_MIN) so the scale was negative; vsat0 then clamped to 0. Fix: `ldexpf(1.0f, imm5)` in
all vf2i*/vi2f/vwbn, plus saturating float->int32 casts. Lesson: a GE symptom can be CPU-side;
diff list *state* (ambient, matrices, colours) before suspecting depth/FBO/clear paths.

### Distorted / stuttering BGM and sounds, fast-forwarding or laggy movies -- audio sink pacing + ignored `sceAudioSetChannelDataLen` (fixed)

Symptoms (Dissidia 012): BGM crackles, SFX distorted, the opening movie plays fast/laggy with
audio chopped up. Measure, do not guess: `PSPRECOMP_AUDIO_DUMP=<file>` (raw s16le stereo
**44100 Hz**, the SDL callback output) + `PSPRECOMP_AUDIO_STATS=1` (an `[AUDIO]` line every
5 s: callback cadence, per source `underruns`/`short` frames/`overruns`, ring `fill`, producer
`push_gap_max`, `wait_avg`) + `PSPRECOMP_MOVIE_STATS=1` (per 60 frames: decode/convert/write
ms, achieved fps, ATRAC AU/s, `vpts-apts`). Compare the dump with an `ffmpeg` decode of the
BGM (`ffprobe` shows PSMF video only, so there is no movie-audio reference): a cross-correlated
window with SNR > 70 dB means decode + sink are bit-faithful.

Findings: (1) `sceAudioOutput*Blocking` slept one full grain *after* the guest's own decode
time, so the producer ran at ~92 % of real time -> ~320 dropouts/50 s. Pacing is now real
backpressure (`psp_audio_pace`: block until the sink ring is drained to `PSPRECOMP_AUDIO_LAT_MS`,
default 64 ms; absolute-timeline sleep when the sink is off), sources only start draining once
primed, so a stall costs one clean re-buffer. (2) The game reserves channels with
`sceAudioChReserve(-1, 256)` then switches the movie channel with
`sceAudioSetChannelDataLen(ch, 2048)`; the stub ignored it, so only 256 of every 2048 decoded
samples were played and the audio thread (which paces the movie) ran 8x too fast: 50 fps video,
57 AU/s instead of 21.5, vpts-apts drifting 1 s per 1.2 s. Also `-1` now allocates the first
free channel instead of always channel 0. `PSPRECOMP_AUDIO_TRACE=1` logs channel config calls.
(3) Movie textures: the GE texture cache hashed only the first 4 KiB (two rows of a 512-px RGBA
frame); `psp_mpeg_frame_info` now tells the cache which guest buffers are movie frames and their
serial, so the bind skips hashing and uploads just the decoded rows with `glTexSubImage2D`.

### Garbage LOOKUP_MISS at battle teardown (caller 0x08AD0170), flood and hang after ~1 h -- literal pool swallowed a constructor (fixed)

Symptoms (Dissidia 012, retail and modded, also on macOS): at the end of a battle (Quick Battle result, Arcade
fight-to-fight) `[LOOKUP_MISS]` with a garbage target, always `caller=0x08AD0170` (battle overlay), a0 a float-looking
pointer; after ~1 h of arcade fights a flood from other callers and a hang in `FUN_088a6aa4` (loop count read from a
trampled object).

Method that found it (reusable): (1) the miss context gave obj `r17=0x09714770`; `R` over the debug socket showed it
held SPU-ADPCM-looking data, identical in every run, and the `sceIoReadAsync` log showed the same bytes in the
`PACKAGE.BIN` read into the FILE heap staging block -- stale file data, not a trampled live object; (2) a gdb
breakpoint on `ovl_battle_FUN_08ad0170` with `bt` showed the caller chain `ovl_battle_FUN_08ad091c` ->
`FUN_0899372c` -> `FUN_089935a4`, i.e. a compiler-generated `delete[]` over 32 elements of 0x130 bytes (a pool built by
`FUN_08ad0810` with `FUN_08993560(obj, 32, 0x130, ctor=0x08AD0160)`); (3) dumping the 32 elements at battle time
showed elements 3..31 with garbage at +60/+64 (the child-list head the destructor walks) while the first three, used by
the game, were clean. `FN_WATCH` on the FILE-heap allocator (`FUN_0888204C`) showed the pool is carved out of memory
the staging buffer used earlier, so uninitialised elements hold file data by design; the game relies on the element
constructor (`sw zero,64(a0); sw zero,60(a0); jr ra; move v0,a0`) to clear them.

Cause: the constructor never ran. It sits right after a literal pool (float constants at 0x08AD0150, `0x40490FDB` =
pi). Gap-fill minted `FUN_08AD0150` over the pool (a decode-error stub with an empty body), and the
`PSPRECOMP_CROSS_MID` sibling coalescer absorbed the real, frameless function at 0x08AD0160 into it as a mid-entry
(`FrameSig` of an undecodable owner defaults to `Owner`). Calling the constructor therefore executed an empty stub
and raised no `LOOKUP_MISS`. Whether the destructor then faulted depended on what the garbage happened to point at
in low memory, hence "~3 minutes in", different targets per run and no miss at all in some runs.

Fix (generic, `crates/psp-cli/src/recompile.rs`): an owner whose bytes do not decode is never a coalesce owner, so the
real functions behind a literal pool stay functions. Unit test
`coalesce_does_not_absorb_into_undecodable_literal_pool`. Blast radius (diff of the generated dispatch tables):
EBOOT unchanged, battle overlay gains `FUN_08AD0160`, menu overlay turns ~25 former `func_*_entry` mid-entries at
0x08B22Dxx / 0x08B343xx / 0x08B485xx.. into real functions (the same class of silent no-op calls). Symptom to grep for
in future: a `// decode error: unknown instruction word` stub with a `func_XXXXXXXX_entry` wrapper pointing at it in
`mid_entries.cpp`.

### HUD smeared to the screen edges while dashing at the EX Core -- stale clip `w` in the fast path's reused vertex vector (fixed)

Symptom (Dissidia 012, every character, default fast path only): lock on to the EX Core and dash at it
(R+Triangle). For one or two game frames the whole HUD vanishes (portraits, BREAK, brave/HP numbers and gauges,
"EX Core in play") and is replaced by huge stretched slabs and streaks of HUD art (red/blue bottom-left, a green
slab bottom-right, pink top-left, green streaks out of the lock-on reticle); the 3D scene is untouched.

Reading the frames (screen recording, 2026-10-20): each sprite keeps ONE hard edge at its true position and every
other edge lies on a ray from the screen centre to the screen border. Example: the HP-gauge slab's left edge runs
from PSP (280,250) to (286,272), exactly on the ray from (240,136); the "EX Core in play" banner keeps its bottom
edge at y=37 and fans up to the top along centre rays. That is the signature of clip-space vertices with **w = 0**:
a homogeneous point at infinity in the direction of its NDC position, which the GL clipper turns into a wedge from
the finite vertices out to the border. Through-mode RECTANGLES (text, number glyph runs) with `w <= 0` are dropped
by the rect expansion, so text vanished outright. A wrong viewport, offset, UV or a framebuffer-feedback blur does
not produce centre-ray edges; check this geometry first when 2D art "streams" off the screen.

Cause: `ge_decode_vertices` wrote every `DecodedVertex` field except `w`, and `ge_transform_vertices` set `w` only on
its real-MVP branch. The legacy path decodes into a fresh vector per PRIM (`w` defaults to 1), but the batched fast
path's CPU-transform route (through mode, RECTANGLES, NDC-direct fallback; `fast_prim` in `psp_ge_draw.cpp`) reuses
one `static thread_local std::vector<DecodedVertex> decoded`, and `resize()` keeps the old elements. A 3D sprite batch
(non-through RECTANGLES) with a corner behind the eye gets `w = 0` from `rect_clip_to_ndc` (its "skip this rect"
marker); every later through-mode PRIM that reused those slots inherited `w = 0`, and kept it until another 3D sprite
batch rewrote the slots, so the HUD stayed broken for the rest of the frame. Dashing at the EX Core presumably puts
some of its particle sprites (or the dash wisps) behind the camera, hence the reliable trigger; the slot index decides
which corners of each HUD quad blow up. With `PSPRECOMP_GE_RECT_OLD=1` the leaked value was the raw clip `w` instead
(HUD shrunk toward the centre or mirrored), so RECT_OLD A/B shots taken before this fix carry that artefact too.

Fix (generic, `runtime/src/psp_ge_vertex.cpp`): `ge_decode_vertices` resets `v.w = 1` for every vertex, and the
through-mode and NDC-direct branches of `ge_transform_vertices` set `w = 1` explicitly. Regression tests
`test_reused_vector_through_w_reset` and `test_ndc_direct_w_reset` in `runtime/tests/test_ge_vertex.cpp`
(`test_ge_vertex`); both fail on the old code with `got 0, expected 1`.

Why the usual tools missed it: the debug socket `T <n>`, `PSPRECOMP_DRAW_TRACE` and `PSPRECOMP_3D_DIAG*` all force the
legacy per-PRIM path, which never had the bug, so a draw trace of a bad frame looks clean; `PSPRECOMP_GE_VERIFY` only
checks GPU-path triangles. `PSPRECOMP_GE_AB=1` catches this class (fast vs legacy on the same frame). The quickest
check is socket `G 0` (or `PSPRECOMP_GE_FAST=0`) while the glitch is on screen: if it goes away on the legacy path,
look for per-PRIM state the fast path carries from one PRIM to the next (reused staging vectors, batch keys, cached
uniforms/GL state).

## GE stencil / texture diagnostics (ge-3d)

| Flag | Effect |
|---|---|
| `PSPRECOMP_GE_STENCIL=<bits>` (socket `ST <bits>`) | bit 0 = GE stencil test/ops on a real GL stencil buffer (default; PPSSPP STENCILTEST 0xDC / STENCILOP 0xDD); bit 1 = "stencil to alpha" (REPLACE writes ref into dst alpha). Bit 1 is off by default: it makes the Story free-roam terrain vanish (cause not found). 0 = off |
| `PSPRECOMP_GE_FEEDBACK=0` | disable the snapshot used when a draw samples the render target it is writing |
| `PSPRECOMP_TEX_HASH=head` or `full` | texture cache key: default samples 16 KiB plus 48 strided windows; `head` = old 4 KiB; `full` = everything |
| `PSPRECOMP_TEX_WHITE=1`, `PSPRECOMP_TEX_DUMP=<hexaddr,...>` | log near-all-white textures; dump textures as tex_<addr>_<n>.tga |
| socket `D <dir>` | dump every live render target (and alpha) at the next present |
