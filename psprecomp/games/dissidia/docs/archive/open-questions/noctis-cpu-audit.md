# Extra characters as the CPU: audit, fix, log (1 Nov 2026)

Symptom (reported): Noctis does nothing as the CPU; then confirmed **every extra** (Noctis, Vivi, Y'shtola, ...) stands still.
Engine trace: `cpu-ai.md` (same folder). Branch `noctis-cpu-ai`.

## Cause (strong; confirm with the log)
Not character data. Battle request type 0 (`FUN_088F9BB0`, "own save record" + CPU flag) makes a side CPU-controlled
(`+0xF34 = 2` in setup block `0x09AFB8A0 + side*0x1060`) only if `FUN_088C04B4` finds the charId's play-style record,
and `FUN_088C04F8` searches the fixed list at EBOOT `0x089CBA68` = {1..22, 24..32}. Extra charIds (33+) are not in it,
so `+0xF34` stays 0 ("human", no pad), the brain mode (`*(actor+0x5C4)+0x54`) stays -1 and the AI tick
`FUN_08A64400` returns at once: no movement, no attacks.

## Fix (runtime, roster.cpp `cpu_req0_hook`, default on; `PSPRECOMP_CPU_EXTRA_FIX=0` turns it off)
Wrapper on `FUN_088F9BB0`: after the original, for a CPU request of a roster character not in the list, it does what
the function does for the base character (roster.txt `like`: Noctis -> Bartz 9, generic for every extra):
`FUN_088C04B4(save+0x124A4, like)`, `+0xF34 = 2`, strength `+0xF38` from `FUN_088C0640(FUN_088CBB3C(block))`,
`FUN_088C0274` copies the play style to `block+0xF40`. If the like has no record, `+0xF34 = 1` (and level `+0xF36` 0 if
it was -1). Chosen over hooking `FUN_088C04F8`: `FUN_088C04B4` has three other callers (play-style recording,
`FUN_088F5AE8`, `FUN_088F5FEC`, battle `FUN_08AEF78C`) that would then write an extra's play into the base character's
record. Side effect: the extra CPU uses the base character's recorded play style; commands it does not list (Noctis's
new ones) weigh 0.1 (overlay default at `0x08A9806C`), Bartz-numbered ones (A2..AE) take Bartz's learned weights.
Not built or run yet (the VM cannot build for macOS); `roster.cpp` passes `g++ -std=gnu++17 -fsyntax-only -Wall`.

## Log: `PSPRECOMP_CPU_LOG=1` (stderr, `[CPU]`; `PSPRECOMP_CPU_LOG_CHAR=34` limits the choice lines to one charId)
* `battle request (own record): side S charId C (base L) CPU flag F: controller +0xF34 a -> b ... fixed/unchanged`
  (printed even without the log whenever the fix changes something). `a=0 -> b=2` = the fix fired.
* `brain B (actor A charId C): mode M (...)` at the first tick, on every mode change and every 600 ticks with the
  number of attack choices. **mode -1 = no AI** (the bug); 0 = CPU; 2 = human helper.
* `choice charId C ...`: one attack choice (`FUN_08A7C1DC`): request fields, objentry AI id vs brat id, foe distance /
  height, the result (`v0 -1` = nothing picked), then one line per command looked at: how far it got and why it was
  dropped: not equipped / no press / wrong kind (HP-brave, air-ground, plya +0x0E class) / not in the 12 set slots /
  outside the reach window (window shown) / late gate or no brat row / weight 0 / **CANDIDATE weight w**, with the
  command's brat codes per situation (0 = .2, 1 = .4, 2 = .7, 3 = never, 4 = .01). Identical outcomes are counted
  (`same outcome as above xN`). 30000 lines max.

## Data audit (p_noc100), against Bartz p_fiv100 and Cloud / Tidus / Squall / WoL
* brat: 15 commands, 18 rows (Bartz 14/14). **0xB7 Trident of the Oracle had no row (never picked)** -> fixed: row
  2/2/1/0/1/0 (Bartz's air HP 0xAD pattern). Pack/unpack of the objx round-trips byte-identical.
* Orphan rows 0xA5 / 0xA8 / 0xAD / 0xAF (commands no longer in comb): harmless (lookup is by command), left.
* No command has all weights "never"; first-press plya windows are non-zero and reachable (A2/A3 0-7.5 m, A4 4-13,
  AA 2-9, AB 0-5, AC 0-6 h -10..2, A6/AE/B4/B7 6-24, A7 0-10, B0 4-20, B3 10-24; heights as retail).
* Input-kind bytes match retail by type: brave first presses +0x0E 0/1, +0x0F 1, +0x18 0x10002; HP 3/4/5, 2, 0x10012.
  HP follow-ups B1/B2 open with the brave press (as Bartz's 0xAC follow-up).
* Timelines: common motions (stand/run/fall/...) carry the same can_act etc. as Bartz; every move press has
  attack_over_cpu and dodge_cancel; HP moves have cpu_hint.
* Abilities: 03/0D/0F/14/15 are disabled (command -2): never picked, as intended.
* Not checked in depth: `p_noc100_a` (EX Mode objx) still holds 4 Bartz commands (A2 3018, A7, AA, AD) and Bartz's brat;
  Vivi / Y'shtola / the others were not audited.
