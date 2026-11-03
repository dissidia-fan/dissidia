# Cross-check: event-op leads from VCKing101/Dissidia012-pc (moveset-format.md)

Research note, 2026-10-31. Their notes are for the US build; their "motion" = our motion player actor+0x130, so their
motion+0x60.. = our actor+0x190... Each lead was re-read in our EU recompilation (`work/output/generated`,
`work/overlays/battle/generated`); nothing was copied without a code read. Labels as in the rest of the archive.
Several addresses they quote (0x0883A150, 0x08B34F40) are the same in our build.

| Lead | Our code | Verdict |
|---|---|---|
| 0x0051 "armor": motion+0x67 \|= 1, read by take-hit 0x0883A150 | Handler 0x0882C408 sets actor+0x197 bit 0x01. **Reader found**: `FUN_0883A150` at 0x0883A380: if +0x197 bit 0x01 and the defender is in an action (`FUN_088377C0`, +0x380 bit 0x08) and the hit is **not** an HP hit (`FUN_088132BC`), it returns before `FUN_0883A6FC`, so no reaction. HP hits and hits outside an action still react. Damage is applied by the damage passes, not here (strong). | **confirmed**, with the nuance "Brave hits only, while in an action". event-ops/K.md "no reader" is wrong. |
| 0x0071: motion+0x67 \|= 0x80 (assist: owner +0x197 too), read by take-hit | Handler 0x0882CD4C: +0x197 \|= 0x80, partner too for kind 1. Read in `FUN_0883A150` (0x0883A4A8) and `FUN_0883A6FC` (returns at once), in `FUN_08B33938` (attacker extras skipped), also tested in the damage passes `FUN_08AE45B8` / `FUN_08AE5BC8` (effect there not traced), and CPU AI. | **confirmed** (matches K.md) |
| 0x0077: motion+0x68 \|= 0x20, reader unknown | Handler 0x0882CE70: +0x198 \|= 0x20. **Reader found**: CPU AI `FUN_08A75D38` (0x08A76028): when set, the AI runs `FUN_08A6D664` (acts on the current action record `FUN_08B30898`) and returns −4. Same function checks +0x196 0x80 (0x0652) and +0x193 0x20. No player-side reader. | **strong**: a CPU-only behaviour flag; exact AI behaviour open |
| 0x0225 "air_params", sibling of ranged 0x18 air_control | Handler 0x0882ADE4: +0x192 \|= 0x20, +0x3D0 = turn mul, +0x3D4 = speed mul. Sibling is our **0x0518** (handler 0x0882ACAC, sets +0x192 0x08, enters state 0x12 and the air bit). The 0x20 bit **is read** in `FUN_08B30544` (stick movement allowed during the move). Unlike 0x0518 it does not enter the air state. | **confirmed** as the sibling; "air" is a misnomer for 0x0225 (ground stick-steer). K.md "bit unread" is wrong; H.md already had it. |
| Keyed 0x0037: clears lock-target +0x188 bit 0x10 (Ultimecia) | Handler 0x0882E6B8: opp = vtable+0x130 (+0x300, the opponent), clears opp+0x188 bit 0x10 = the opponent's frozen-animation bit (`FUN_08830444` feeds dt 0 while set). Uses: Ultimecia eht200, Aerith sev120_a. | **confirmed** (I.md/M.md); "lock-target" = our opponent +0x300 |
| 0x0131 sets char+0x6DC = value (Lightning) | Keyed handler 0x0882E61C: fighters only, +0x6DC = arg0; `FUN_08AFD424` posts manager message 0x1F {value, actor} and clears it. Users gst130/gst130_a (+1 sev120_a). | **confirmed** (L.md); listener of 0x1F still unknown |
| 0x0B36 calls a function with 11 args (US 0x08B34F40) | Keyed handler 0x0882E660 → `FUN_08B34F40` (same address) with 11 args: timed buff + pulsing tint; bytes used by the damage passes `FUN_08AE45B8` / `FUN_08AE70E4`, set chosen by atkp flag 0x80. | **confirmed** (M.md) |
| Ability 0x136 gates 0x0047 | 0x0882A76C: +0x190 \|= 0x20 only with ability 0x136. | **confirmed** (H.md) |
| Ability 0x41 gates 0x0074 (motion+0x68 \|= 0x10) | 0x0882BD88: +0x198 \|= 0x10 only with ability 0x41; read by `FUN_08B2DC38` (perfect dodge). | **confirmed** (H.md) |

Doc fixes this implies: event-ops/K.md 0x0051 and 0x0077 ("flag with no reader") and the 0x0225 "unread flag" line;
moves-and-events.md line "0x0051 / 0x0077 (no reader found)".
