# Native build (static recompilation): history and findings

How the native build came together, from the first code census to the first battle, and the soak-test findings after. How to build and run it today: [../README.md](../README.md). Engine internals: [psprecomp/ARCHITECTURE.md](../../../ARCHITECTURE.md) and [psprecomp/DEBUGGING.md](../../../DEBUGGING.md).

- [Dissidia 012 static recompilation (2026-10-15)](#dissidia-012-static-recompilation-2026-10-15)
- [Dissidia soak findings (sas-voices worktree, port 9993, private save ms0_sas)](#dissidia-soak-findings-sas-voices-worktree-port-9993-private-save-ms0_sas)

## Dissidia 012 static recompilation (2026-10-15)

Goal: run the game natively (Steam Deck / PC) without PPSSPP, via static recompilation on PSPRecomp. Files: `work/recomp/` in the local dissidia folder — decrypted EBOOT, Ghidra 12.1.3 + ghidra-allegrex for the VM, census scripts, Ghidra project `gproj_allegrex/`, and `psprecomp/` with the **git bundle of the fork (branch `dissidia`)**, the full patch, `games/dissidia/` manifests + tools, all `analysis*.json.gz`, boot/draw traces and a README with the exact rebuild recipe.

### Code inventory (Europe PSN ISO) — Ghidra 12.1.3 with the Allegrex processor
| Binary | Code | Instructions | Functions | VFPU instrs |
|---|---|---|---|---|
| EBOOT.BIN (decrypted → `EBOOT.dec.elf`) | 1.84 MB | 448k | 8,583 | 9,011 |
| OVL_BATTLE_APP.ELF | 1.60 MB | 376k | 6,179 | 1,693 |
| OVL_MENU_APP.ELF | 1.56 MB | 353k | 6,270 | 107 |
| OVL_EXTRA_APP.ELF | 24 KB | 5.7k | 97 | 0 |
| LIBSUPPREACC.PRX | 14 KB | | 73 | — relocatable PRX; recompiled, not HLE'd |
| LIBFONT.PRX | 20 KB | | | encrypted Sony module; HLE as sceLibFont |

≈21,100 functions, ≈1.18M instructions. EBOOT: static ELF at 0x08804000, module `FFSTJP`, gp 0x08B6B260, 16.8 MB BSS, `e_entry`=0 with `module_start` (export NID 0xD632ACDB) at 0x08804040. Decrypt with `pspdecrypt`. ≈60 base VFPU ops.

### Overlays — solved
- Three flat blobs load at **0x089DD4C0** (window to 0x08B63170); header `{id, flags, ptr, ptr}` (id: battle 1, menu 2, extra 3; ptrs = ctor table inside the blob), entry at +0x10. The game reads the ELF header (0x54) then the body to 0x089DD46C so the section lands exactly on the window, then `sceKernelIcacheInvalidateRange`.
- EBOOT has 852 direct call sites (542 targets) into the window; PSPRecomp emits those as runtime `RECOMP_LOOKUP`.
- Implementation: each overlay recompiled as a prefixed function set (`tools/build_overlay.py`) with its own lookup table; a runtime seam `psp_dispatch_set_overlay_resolver` is consulted before a LOOKUP_MISS; the Dissidia resolver reads the id word at 0x089DD4C0 (the game wrote it) and picks the table. Overlay data needs nothing — the game copies the blob itself.
- Two traps: (1) the EBOOT set minted empty functions over the window (it's inside the EBOOT image range) and shadowed the resolver → `[recompile] exclude_ranges = ["0x089DD4C0-0x08B63170"]`; (2) the overlay entry and the EBOOT→overlay targets have no xrefs inside the overlay → forced into each overlay's manifest.
- LIBSUPPREACC.PRX (Memory Stick / save-data pre-access library; 13 exports, 26 sceIo/callback imports, no `$gp` use): analysed with `--load-base 0x0B800000`, built with `build_overlay.py --module` (keeps prefixed data sections + import table), loaded in `register_hooks`, its import table bound via `psp_hle_bind_stub_table`, and the EBOOT's 8 `scesupPreAcc` stubs pointed at its exports.

### Jump tables and gap functions
Ghidra's data xrefs for switch tables don't reach the recompiler, so cases surfaced one per boot as LOOKUP_MISS. `tools/jumptables.py` scans the loaded image for the `sltiu … lui/addiu … lw … jr` idiom and emits force entries: EBOOT 77 tables / 901 cases, battle 69 / 718, menu 42 / 300, extra 1 / 7.

Three more classes of miss, all fixed the same afternoon:
- **Overlay→EBOOT targets** (2,262 of them, e.g. 0x08862444 = "start DLC scan", which the title state machine waited on forever): the overlay sets call EBOOT functions the EBOOT's own xrefs never reach. Forced via `game.toml`.
- **Code Ghidra left outside any function**: tiny qsort comparators reached by `lui/addiu` pointers, callback tables, and straight-line tails Ghidra chopped off their parent. `tools/gapfuncs.py` scans for `jal`/`j`/branch targets, `lui/addiu` code pointers and pointer-table words that land outside every function (756 in the EBOOT).
- **Frameless placeholder size**: the recompiler gave heuristic (gap-fill) functions a 256-byte placeholder and only grew the ones with a stack frame, so a frameless 0xA38-byte store run fell off its end every 0x100 bytes. Fixed in `hle_entry_scanner.rs`: frameless functions now extend to the first unconditional transfer no forward branch skips.
- LIBSUPPREACC's own indirect calls are answered by the overlay resolver too.

Result: **zero lookup misses** through the title into the menu.

### Pipeline / runtime changes (branch `dissidia`, 6 commits on upstream d38e10b)
- analyze: `module_start` fallback when `e_entry` is 0 (+ synthesise the function); recompile: `exclude_ranges`.
- decoder/emitter: `round/ceil/floor.w.s`, `cfc1/ctc1`, `wsbh/wsbw` (were under SPECIAL2; Allegrex encodes them as BSHFL), VFPU `vocp`, `vsgn` (+ runtime impls). EBOOT decode errors 16 → 4 (the 4 are data-as-code).
- runtime: Linux build fixes (`<cmath>`, `<algorithm>`, `std::sinf`→`::sinf`), case-insensitive device prefix (`DISC0:`), overlay resolver seam, `PSPRECOMP_OVERLAY_DIRS` CMake option, `psp_hle_bind_stub_table`, HLE for `sceKernelAllocMemoryBlock`/`GetMemoryBlockAddr`/`FreeMemoryBlock` (NIDs missing from the NID db: 0xFE707FDF/0xDB83A952/0x50F61D8A), `sceRtc` tick API, `sceKernelMemcpy/Memset`, `sceDmacMemcpy`, scePower clocks/callbacks (0 MHz would poison timing), cache no-ops, `sceKernelGetGPI`; GE: colourless vertices take `MATERIALAMBIENT`+`MATERIALALPHA` (PSP semantics) instead of opaque white; vertex stride padded to the largest component (8888 colour → 4-byte alignment; vtype 0x80011E is 16 bytes, not 14 — this was the black screen); through-mode UVs/positions raw; TEXFUNC colour doubling + RGBA bit (`u_tex_double`/`u_tex_rgba`) — without doubling everything was mid-grey; Memory Stick: `ms0:` mounted on a host dir (`PSPRECOMP_MS0`), real mkdir/rename/remove/chstat, free-space devctls; sceRtc, minimal sceLibFont, sceAtrac3plus set-data/ID bookkeeping (no decode yet); `PSPRECOMP_DRAW_TRACE` / `PSPRECOMP_TEX_DUMP` diagnostics.

### Where it stands (15 Oct, late evening)
The native build boots a full session: splash → title → first-run Data Config (on-screen keyboard, every setting) → save written and re-loaded on later boots → story prologue (dialogue text boxes, tutorial cards) → **the first battle, HUD rendering** (portraits, bravery/HP bars, COM opponent, battle BGM decoded). No dispatch misses on the menu path.

Open: the 3D arena/characters are not drawn yet (non-through-mode geometry path, first time exercised); the health-warning splash text doesn't render.

#### 16 Oct: the opening movie plays; four translation bugs behind the missing 3D
- **sceMpeg via libavcodec** (own MPEG-PS demux: AU split at the H.264 AUD, 0x1BD audio sub-header, ATRAC3+ params 0x285C; AvcDecode accepts RAM or VRAM targets). The opening movie now plays on screen with subtitles in sync (the strips are drawn as GE RECTANGLES, which must never be back-face culled). `PSPRECOMP_NO_MPEG=1` makes Init/Create fail so the intro skips to the prologue — the quick route to the first battle (~100 s).
- The 3D path exercised code PSPRecomp had never run, and each wrong matrix led to a different bug — none in Dissidia, all in the translator:
  1. `madd/maddu/msub/msubu` are SPECIAL funct 0x1C/0x1D/0x2E/0x2F; 0x1C was decoded as `mul`, so the model builder (`FUN_089b72f4`) sized buffers wrong and vertex pointers were junk.
  2. VFPU opcode 0x34 conversion sub-ops (vf2h/vh2f/vsbz/vlgb/vuc2i/vc2i/vus2i/vs2i/vi2uc/vi2c/vi2us/vi2s) were undecoded.
  3. COP2 `mfv`/`mtv` are rs 3/7 with the VFPU register in the low byte (≥128 → mfvc/mtvc); 268 `mtv` and 16 `mfv` in the EBOOT were mis-decoded, so the view matrix never reached the VFPU.
  4. **Branch-likely VFPU branches (`bvtl`/`bvfl`) dropped their delay slot** in the emitter (the match in `emit_op_with_ds` had no arm for them; the cross-function tail-call path dropped it for every likely branch). The game's VFPU `atan2` (`FUN_089a457c`) keeps `vsgn` in a `bvtl` delay slot for the x = 0 case, so the camera angle came out inf and the rotate-X matrix (`FUN_089a4180`, `vrot`) turned the projection into NaN. Found with the VFPU non-finite write trap (`PSPRECOMP_VFPU_NANTRAP=1`, which prints the function ring at the first inf/NaN write) and pinned down by a host test (`runtime/tests/test_vfpu_atan2.cpp`) that transcribes the generated function against the real `vfpu_*` runtime ops.
- Also: `vcmp` EN/EI/ES/NN/NI/NS test only `vs` and `vcmp` preserves CC bits it doesn't write (PPSSPP semantics); `__sceSasCoreWithMix` keeps the mix; SAS voice rendering (VAG/PCM/noise, ADSR) implemented but not yet verified by ear; SDL game controller + keyboard analogue; `games/dissidia/tools/pipeline.sh` (analyse → recompile → overlays → cmake) is the one-shot rebuild for a new machine.

Landed this evening (agents + lead): GE blend constants (FIXA/FIXB per draw), double-alpha factors, blend equations, colour masks, alpha/colour test, wrap/filter, CLUT-aware texture cache, TEXBUFWIDTH row stride, FBO-as-texture (render targets sampled by the GE, UV remap), FRAMEBUFPTR key 0 fix; sceFont on FreeType (PGF layouts, negative descender — Dissidia hard-codes an 18 px baseline and sizes line buffers as baseline − descender + 3); ATRAC3/3+ via libavcodec with an SDL sink and PCM dump; `__sceSasCoreWithMix` no longer zeroes the mix; sceUtilityOsk (headless auto-complete, `PSPRECOMP_OSK_TEXT`), sceUtilitySavedata read+write (dir = gameName+saveName, `ULES01505GameData00`); sceKernelVolatileMem; async IO completion notifies pre-registered fd callbacks; overlay constructor tables forced; gap-function scanner; force entries in a junk jump's delay slot are never coalesced; jump tables with `slti` bounds.

Facts worth keeping: the story intro is a talk-event script (VM `FUN_088E150C`, ticked by `FUN_088E0F14`, scene object 0x08B851A0) whose waits are keyed to the movie clock at `scene+0x13A4` (movie object 0x08B864FC, `+0x48` seconds); the SQEXTEC movie player object is at 0x09ADCD90 (`DAT_089D7F00`); `0x08993300` is the array-constructor helper (calls a ctor pointer per element — misses with it as caller are constructor-table targets to force).

Workflow: scoped Sonnet agents take bounded pieces in git worktrees (one runtime build at a time, owned by the lead); the session-level flow is boot → `[LOOKUP_MISS]`/trace → fix → rebuild, now ~2 min per cycle thanks to the save.

#### 16 Oct, later: the first battle renders in 3D

![First battle in 3D](images/first-battle-3d.png)
![First battle victory](images/first-battle-victory.png)
Three more runtime bugs stood between the finite matrices and a picture: (1) the depth compare was flipped for a reversed depth range (an earlier workaround) — `glDepthRange(near>far)` already reproduces the PSP's depth values, so Dissidia's GEQUAL became LEQUAL against a far-cleared buffer and every 3D fragment failed; (2) `vhtfm3` was transformed as 3×3 — the printed size is the *input* size, the hardware does (N+1)×(N+1) and writes the w lane, which the bone-matrix builder (`0x089BA2C4`) relies on, so every skinned model's translation was multiplied by stale register garbage; (3) no fixed-function lighting at all, so with the game's 0x808080 ambient and 2× texture doubling everything saturated to white. With CPU per-vertex lighting (ambient/diffuse, directional/point/spot) **Order's Sanctuary, Lightning and the opponent render, the camera tracks, attacks animate and bravery changes** (`shots/v_all.png`). The prologue's moogle scene renders too. Diagnostics: `PSPRECOMP_3D_DIAG[_FROM/_MINN/_MAT/_BIG]`, `PSPRECOMP_3D_SKIP_VTYPE`, `PSPRECOMP_BONE_TRACE` (DEBUGGING.md §7). Not modelled yet: specular, fog, CLUT shift/mask/start.

### Next
1. Battle fidelity: specular/fog, compare shading against PPSSPP, performance (software GL does millions of prims per minute — a real GPU on the Deck will be fine, but the vertex path is CPU-side), then play through to the end of the battle and the result screen.
2. Packaging for Steam Deck (SDL2/OpenGL build, data from the user's own ISO via `setup_from_iso.py` + `pipeline.sh`; untested end to end on a Deck).
3. Upstream the generic bits — none are Dissidia-specific.

### Tooling notes
- Ghidra in the Claude VM on the Mac (arm64 Linux): built the missing decompiler from source; Ghidra 12 scripts are Java. PSPRecomp pins Ghidra 12.0.2 + allegrex v21.3, but 12.1.3 + v21.4 works.
- Runtime needs cargo, JDK 21, cmake, SDL2 dev, OpenGL; headless: `xvfb-run` + `LIBGL_ALWAYS_SOFTWARE=1`. Run with `PSPRECOMP_DISC0=<extracted iso> PSPRECOMP_MS0=<memory stick dir> PSPRECOMP_CROSS_MID=1 PSPRECOMP_CLEANROOM=1`; debug socket on TCP 9999 (`I` state, `B` buttons, `S` screenshot); `PSPRECOMP_HLE_TRACE=1` logs every OS call.

## Dissidia soak findings (sas-voices worktree, port 9993, private save ms0_sas)

> Soak run of 17 Oct. Paths to shots refer to the cloud work folder of that session and were not kept.

Shots in /home/claude/recomp/psprecomp_work/shots_sas/ (.tga + .png). Box load average ~13 -> game runs 3-5 fps; timings below are real seconds.

### F1 FIXED: Story intro cutscene shows a tiny image in the top-left corner on black
- Repro: boot -> Load game (B 4000) -> Story Mode -> Main Scenario -> Prologue -> YES. After ~8 s the screen is black with a ~128x64 picture top-left (shots_sas/s4.png, s5.png, s6.png).
- Log: endless `[DRAW] FBO pool overflow: evicting key=0x0F4000 for key=0x000000` etc. The scene cycles through 5 distinct framebuffer keys (0x000000, 0x044000, 0x0EC000, 0x0CC200, 0x0F4000); GE_MAX_TARGETS was 4, so LRU eviction every frame deleted the render-to-texture target (re-created cleared to black).
- Fix: runtime/src/psp_ge_draw.cpp GE_MAX_TARGETS 4 -> 8 (commit on sas-voices). After: intro (s7.png) renders full-screen with text, no overflow lines.

### F2 FIXED: LOOKUP_MISS in story (0x0891FF60 and the jump-table neighbours)
- 0x0891FF60 (eboot, caller 0x088C5E4C then 0x088BD640/0x0891FF60) hit on entering the prologue free-roam area (first right after the cutscene). The target is a run of tiny leaf getters (`jr ra; li v0,0` / `lw v0,4(a0)` ...) not covered as entries; forced 0x0891FF08, FF28..FF58, FF68, FF70, FF98 in game.toml (commit).
- 0x08B46ECC (battle overlay, caller 0x08B46DBC): forced in overlay_battle.toml.

### F3 FIXED (pending verification run): arcade/quick battle LOOKUP_MISS
- 0x08A6E9B8 (menu overlay id 2, first hit while stepping through Arcade Mode setup; caller 0x0885C6CC a0=0x09723F00).
- 0x08ADB390 (battle overlay, in function 0x08ADB330): jump table at 0x08ADBBC8 (9 entries: 8ADB358, 8ADB3E4 x3, 8ADB390, 8ADB374, 8ADB3E4, 8ADB3AC, 8ADB3C8). Forced all targets in overlay_battle.toml. Seen from round 2 of an Arcade fight (a0=0x10 a1=3/2) - 8 hits.
- Fixed forces (all committed on sas-voices): menu overlay 0x08A6E9B8; battle overlay 0x08ADB390 jump-table cases + 0x08ADB3E4; battle overlay vtable getters 0x08A384F8..0x08A38528 (caller 0x08A3D858, a1=0x08A384F8 passed as callback). Verified present in regenerated overlays_sas dispatch; NOT re-verified in a long run (the run that hit them takes ~10 min of arcade fighting).

### F4 OPEN (serious): memory corruption / garbage indirect calls after arcade fight transitions, then a hard hang
- Repro: Main menu -> Battle Mode -> Arcade Mode -> Quick -> Yes -> NEXT -> fight; mash (SQUARE/CROSS + stick toward enemy) through fights 1..N (about 55-60 min runtime at 3-10 fps; scripts /tmp/claude-0/mash.sh, longmash.sh).
- Seen on all three arcade runs at the first fight->fight transition: `[LOOKUP_MISS] addr=0x100101E8 / 0xF1000000 / 0xBDA40085` all with caller=0x08AD0170 (battle overlay, the "destroy child list" walker: loops obj+60 -> child+244, calls vtable fn at (*(child+248))+8+4 with a1=3) and a0 = garbage non-aligned pointer (0x00F4011D, 0x01459D21, 0x008E3C92). i.e. a child object / its vtable was freed or overwritten before teardown. r17 (obj) = 0x09746970 / 0x097462C0 / 0x09779D00.
- After ~1 h: `[LOOKUP_MISS]` flood from caller=0x088A1E44 with garbage targets (0xFF25263F, 0xFF63887D, 0x00020002, 0xFFDDB283, 0x00000200); 289 total misses; then the screen freezes (shots_sas/cur.png == cur2.png), log stops growing (no HLE calls), game thread spins in ovl_battle_FUN_08aca4d8 -> FUN_088a6aa4 -> FUN_088a751c: loop count at obj+108 (return value of FUN_088A9E60) is garbage/huge so the loop at 0x08ACA5CC never ends. Backtrace (gdb): FUN_088a751c <- FUN_088a6aa4 <- ovl_battle_FUN_08aca4d8 <- ovl_battle_FUN_08aca938 <- ovl_battle_FUN_08b2eb34 <- FUN_088413d0.
- Also throughout: `[atrac3plus ...] Frame data doesn't match channel configuration!` / `Invalid gain location` (144 lines in first 20k lines, from the first fight onward), BGM AT3+ buffers at 0x0999E580 / 0x095CE040 close to async IoRead buffers (0x099A0064, 0x099AC314) -> suspect an ATRAC or IO buffer overrun as the corruption source; worth checking whether the AT3+ buffer size for id=0 fileSize=3068964 overlaps the streaming buffers.
- Not isolated whether this exists on the plain `dissidia` runtime without the models-re heap/staging change (01c28e3): not tested.
- Filtered log: /tmp/claude-0/sas_soak_filtered.log

### F5 visual: "CRITICAL" banner has an opaque white rectangle behind the gold glow (shots_sas/lm_8.png, crop crit.png)
- Seen in battle when a critical hit lands (Arcade, Squall/Shantotto-like fight). Glow texture alpha is not applied (should be transparent around the golden text). Likely blend/alpha-test state for this sprite.

### F6 visual (suspect): EX-burst/summon cut-in frames show flat green HUD blocks and a pink/yellow diagonal block top-left (shots_sas/lm_24.png, lm_28.png). May be the intended QTE bar drawn without its texture; compare against a reference.

### F7 visual: missing-glyph boxes (hollow rectangle) after stat numbers / bullet markers
- Equipment screen (c_8.png, zoom glyph.png): each stat shows `0▯`; Quick Battle Opponent Info (l1.png) and Customization class text ("▯Uses one of three roles..." c_6.png). The glyph is a character the runtime font lacks (probably an arrow/bullet from the PSP system font).

### F8 visual (suspect): story free-roam sky shows a hard diagonal seam between a blue-white half and a dark orange half (shots_sas/s_6.png, s_4.png)

### Coverage notes / blockers
- Story: Load game -> main menu -> Story Mode -> Main Scenario -> Prologue -> YES: intro cutscene OK after F1 (skippable with START->Down->Cross), dialogue/name-in scenes OK, first fight (Lightning vs Garland?), pause menu, Forfeit dialog, DEFEATED screen, Tournament result 0-1 screens all render and return to the party menu. A story "board" with treasure/manikin was NOT reached: this save's Story Mode goes straight into the free-roam prologue field (shots_sas/d_1.png, s_*.png); walking with analog (A x y ms) is too imprecise at 3-10 fps to reach the enemy/chests without a map. The chest with the CROSS prompt (s56.png) opened and showed item text (s57.png).
- PP Catalog is greyed out in this save ("You cannot select this yet.", f3.png) - could not buy anything.
- Collection opens a submenu containing only "Creation" (Original Quest / Battle Replay: Create New Quest, Edit Quest, Quest Select, Delete..., Play/Edit Replay Data, Convert to AVI...). Opened Create New Quest/Edit Quest menus fine; no model/movie viewer is unlocked on this save.
- Options screen OK (Battle Style toggled Action-style -> RPG Mode, cursor/arrow animation fine). Data Config OK.
- Battle Mode: Quick Battle (1-on-1) menu, Quick Battle (Party) -> Player Select/CPU Select/Job select/Party A/Enter Battle/Opponent Info/stage load -> fight (Lightning vs Lightning, Order's Sanctuary), DEFEATED, Tournament result: all OK. Arcade Mode: Quick -> Yes -> VS screen -> fight; fights 1..~5 ran, different stages (Order's Sanctuary, Cosmos castle) rendered correctly.
- Customization (Lightning): Character Select -> Customization menu -> Equipment screen render OK.
- No `[NANTRAP]`; GE `Hit command limit (4194304)` appeared in an earlier arcade run (run 3, 321 lines, at pc 0x054BD30C etc.) after a `Unknown cmd 0xB6/0xF9/0x03` at pc=0x04121D54 inside a sublist (0x04121CA0 CALL target ending without RET; vertex data being executed). Not reproduced after the F2/F3 forces; it was probably downstream of F4 corruption (the GE list for the frame was garbage). GE_NAV ring added (psp_ge.cpp) to trace the CALL chain if it recurs.
