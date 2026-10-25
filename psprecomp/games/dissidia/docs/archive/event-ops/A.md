# Agent A: ranged ops 0x0005, 0x0006, 0x0022, 0x0028

**Summary**
- `s1` in the ranged dispatcher is the motion/timeline controller embedded at **actor+0x130**, so `s1+0x60` is the same byte as **actor+0x190** (the per-tick flag block). That makes 0x0005 = +0x190 bit 0x01, 0x0006 = +0x190 bit 0x20, 0x0022 = +0x193 bit 0x04 and 0x0028 = +0x194 bit 0x04.
- **0x0005 = "actionable / neutral"**: it lets the character start attacks (vtable +0x150), block (+0x158), dodge, R+Triangle dash and air actions. **0x0006 = invincibility**: incoming hits are skipped in the hit path. Both are confirmed end to end in code.
- **0x0028 = "dodge-cancel allowed + CPU may decide again"**: this is strong. **0x0022 = "attack's dangerous part is over"**: only the CPU AI reads it, as a punish/threat hint, and I found no player-side effect (strong for the AI use).

## Shared mechanics (verified)

- **Delay-slot re-check (after the psd.py fix):** I re-read the `jr $ra` delay slots of every small helper used below. The return values are as described:
  - FUN_08837360 / FUN_0883739c / FUN_088379b8 / FUN_08b30544 end in `andi v0, aX, 0xff`.
  - FUN_08837a24 and FUN_08b34e38 end in `sltu v0, zero, v0`, which returns a bool.
  - FUN_08837b20 / FUN_08837b9c / FUN_088377d0 / FUN_08b2f514 / FUN_08b305e8 / FUN_0882fbfc end in a stack pop.

  The handlers' flag stores sit in the delay slot of `b 0x0882CE84`, and they are visible in the listings above.
- actor+0x542 is the actor kind: 0/1 = battle fighters (FUN_0883739c), 3 = a summoned or child object with its parent at +0x5D4.

- **s1 is actor+0x130.** The dispatcher FUN_08829930 sets `s1 = a1` and `s2 = *(s1+0x30)` (the actor) at 0x08829938/0x08829940. The controller's motion time is at +0x78; FUN_0882ef10 and FUN_08830444 read `lwc1 0x78($s1)`, and actor+0x1A8 is the motion time, so 0x130 + 0x78 = 0x1A8. The init FUN_08804498 calls FUN_0882fac4 with `a0 = actor+0x130` (0x08804558). Every handler that the notes describe as "+0x19x" writes `s1+0x6x`: 0x0101 → s1+0x60 |= 4 (0x08829C00), 0x000A → s1+0x60 |= 2 (0x08829A6C) and 0x0023 → s1+0x63 |= 0x10 (0x0882B1F4). So **s1+0x60+n == actor+0x190+n**.
- **Clear/dispatch order in one tick (FUN_0882ef10):**
  1. Keyed events run first: FUN_0882f17c at 0x0882F010.
  2. FUN_08829848 at 0x0882F018 clears the 16-byte block with `FUN_0882FBFC → memset(ctrl+0x60, 0, 0x10)` and resets +0x448..+0x454.
  3. Ranged events run last: FUN_0882f30c at 0x0882F024 → FUN_08828e94 → walker FUN_08828eec, with handler 0x08829930 loaded at 0x08828EB0.

  So a ranged flag holds from its own tick's dispatch until the start of the next tick. It is "on" exactly on the ticks inside [start, end].
- Other ranged codes that set the same bits (from scanning every case in the dispatcher, script `scratch_A/setters.py`):
  - +0x190 bit 0x01: 0x05, 0x1A, 0x1E (0x041E), 0x58, 0x7A.
  - +0x190 bit 0x20: 0x06, and 0x47 when the actor has ability 0x136 (0x0882A76C).
  - +0x193 bit 0x04: 0x22 only.
  - +0x194 bit 0x04: 0x28, and 0x59 only when the actor returned by vtable +0x130 is not in a hit-reaction script (0x0882B2E4..0x0882B344).
- Helpers used below:
  - FUN_088377c0: `actor+0x380 & 8`, meaning "in an action". The bit is set by FUN_08b382b8 (start action) at 0x08B387AC.
  - FUN_08838074: `actor+0x180` (current script) in 500..798, meaning "in a damage reaction".
  - FUN_08837310 / FUN_08837360: current script is 0x1D6 or 0x1D7 (Block, scripts 470/471 = motions 20/21) or `+0x384 & 0x20000`, meaning "is blocking".
  - FUN_088bf4cc(abilities, id): "has ability id".
  - Actor vtable at actor+0x544. The base table is 0x089C7C78 (installed at 0x088310E4). The player overrides are at 0x08B3A9A8 and 0x08A61CF8.

---

### 0x0005 — Actionable: can attack / block / dodge / dash (neutral state)

**Kind / args:** ranged, no args. Retail data uses whole-motion ranges (0..65535), or "from tick N to end" on motions that end in neutral.

**What the handler does:** 0x08829A58: `addiu a0,s1,0x60; lbu; ori 1; sb`, which sets **actor+0x190 bit 0x01** for this tick. Nothing else.

**Who uses it / effect in game:** these are all the readers of +0x190 bit 0x01 that I found by scanning all.s, which also covers the overlay.
- **FUN_08837b20** (actor vtable slot **+0x150**; the entry at 0x089C7DC8 points to it). It returns true if bit 0x01 is set, OR if +0x190 bit 0x04 (the 0x0101 chain window) is set, OR if `+0x384 & 0x40000` and the actor is not in an action.
  - The player override FUN_08b2f9f4 (0x08B3AAF8) first refuses when `+0x5B3 & 3` or `+0x5B4 & 2`. It also allows +0x198 bit 2, or ability 0x115 together with FUN_08b2f95c.
  - Its result is `s2` in the player input handler FUN_08b3526c (call at 0x08B36370). That result gates calls to FUN_08b2d664(actor, 2/4) at 0x08B36524..0x08B36540, which start the command's action through **FUN_08b382b8**.
  - **Meaning: may start an attack/command now.** The chain window is the only other normal way in, which is why chain windows work mid-move and 0x0005 works in neutral.
- **FUN_08837b9c** (slot **+0x158**; the player override is FUN_08b2fb6c). It returns true if (`+0x384 & 0x40000` and not in an action), OR if (bit 0x01 OR +0x198 bit 2) and the actor is not already blocking.
  - The player handler calls it at 0x08B368DC after the block-button tests (FUN_08b316e8 / FUN_08b3389c). On success it calls **FUN_08a5b2b4**, which plays script 0x1D6 (ground) or 0x1D7 (air) and sets state 0x15, i.e. **Block**.
  - **Meaning: may block now.** Note that this slot does not accept the chain-window bit.
- **FUN_08b3a5bc** (the dodge gate; see 0x0028). When the actor is not in an action, its final branch needs bit 0x01 (0x08B3A794 and 0x08B3A8B8). FUN_08b2d84c calls it first and then starts a dodge: FUN_08a54cd0, FUN_08a588fc, FUN_08a56a5c or FUN_08a61ac8, which play scripts 400/410/440/460 = motions 16–19.
- **FUN_08b305e8** (0x08B30610). When `+0x5B6 & 0x80` is set it returns `bit 0x01 && !in_action`. Otherwise, in an action it needs +0x193 bit 1 (from 0x041E) without +0x199 bit 4, and outside an action it needs bit 0x01. It is called by:
  - the R+Triangle dash entry predicate **FUN_08a50af4** (0x08A50B2C; it also needs ability 0x11D and a state that is not 0x13/0x19);
  - FUN_08a52ed8 (abilities 0x116/0x117);
  - the CPU predicates FUN_08a597dc, FUN_08a5c7a0 and FUN_08a5e528 (abilities 0x11D–0x11F);
  - the player handler at 0x08B36968.
- **FUN_08b2f514** (0x08B2F568). It requires bit 0x01, state 1 (free fall), height above ground (+0x3C0) ≥ 0.4 (float at 0x08B3ABC0) and not `+0x380 & 2`. The player handler uses it at 0x08B35F8C and 0x08B36124 next to ability checks 0x3D/0x3E, before an aerial action (FUN_08a5c110 / FUN_08a5c23c). This is an air jump or air action out of a fall (**guess** which one).
- CPU-side readers: FUN_08b30200 (via FUN_08a81518), FUN_08b30544 and FUN_08a6307c (0x08A63728).

**Data check:** fits.
- All three characters carry it 0..end in motion 000 (stand) and 001, in 003 and 005, and in 008/009/011/015.
- It runs "from N" where a motion returns to neutral: 002 from 8, 010 from 20 (after its hit window 0..?), and 037 from 24.
- In 031/035 (a recovery motion, scripts 702/752) it starts at 70, the same tick as 0x041E. The notes already had it filling the fall motion _021A.

**Confidence:** **confirmed** that it is the "may start a new action from neutral" bit read by the attack, block, dodge and dash gates.
- **In-game test:** remove 0x0005 from a character's stand motion (000). The character should be unable to attack, block or dodge while standing, but still able to move.
- Note for modders: any custom idle, run or fall motion **must** carry 0x0005 for its whole length.

### 0x0006 — Invincible (hits pass through) while active

**Kind / args:** ranged, no args. Ranges seen: 0..10, 0..20, 0..50, 0..100, 0..end.

**What the handler does:** 0x0882A758: actor+0x190 |= **0x20** for this tick. Related ops:
- 0x0047 (0x0882A76C) sets the same bit only if FUN_088bf4cc(FUN_08835778(actor), 0x136), i.e. the actor has ability 0x136.
- 0x0055 sets bit 0x40, which is read together with 0x20.

**Who uses it / effect in game:**
- **FUN_088377d0** is the "invulnerable" check. It is true if (+0x190 bit 0x20 OR bit 0x40 OR `+0x380 & 0x20000000`), AND `+0x388/+0x38C` do not match the mask at 0x089C7FB8, AND the actor kind (+0x542) is 0 or 1, AND not `+0x5B6 & 0x20`. Code is at 0x088377D0..0x088378B4.
- **Hit application FUN_08808ee4** (hit path) at 0x08808F34. If the attack's hit record flag `*(*(hit)+0x34)+8 & 0x20` is clear and the defender is invulnerable, the hit is **skipped entirely** (branch to 0x088090CC).
  - Exception: hits are not skipped when the defender is in scripts 0x2BE–0x2C0 or 0x2F0–0x2F2 (FUN_08b34e4c) and the attacker's owner (+0x18, or that owner's parent +0x5D4 for kind 3) matches defender+0x304 or attacker+0x1108. **Guess:** this is a special lock or assist case.
  - So hit-record flag 0x20 means "ignores invincibility" (**guess** at its in-game name).
- FUN_088170c0 at 0x08817130 skips a target that has bit 0x20 or 0x40, before targeting/hit-list work. Exactly which system this is is unknown.
- FUN_0888fe28 (0x0888FFB8) stops a timer reset while the actor is invulnerable. Unknown purpose.
- FUN_08b2dc38 (0x08B2DCD4): when invulnerable and FUN_08b2fff8 says an attack is about to connect, it sets +0x5B1 bit 0x08 and posts event 4 through FUN_088593b4. **Guess:** a "dodged an attack" event, used for stats or the AI.
- FUN_08b41bdc stores the check in a struct (+0x10). That struct is probably part of the AI or HUD.

**Data check:** fits, in all three characters.
- Dodge motions 016–019 (scripts 400/410/440/460, started by FUN_08a5679c, FUN_08a61ac8, FUN_08a54934 and FUN_08a5860c) have 0x0006 over 0..20, plus 0x0047 over 0..40 for the ability-extended invincibility.
- Motions 006/007 (scripts 901/911) have 0..10.
- Motion 022 (scripts 300/822/3000) has 0..50 and 0..20.
- Recovery motions 031/035 have 0..100, and 032/036 have the whole motion.
- Vivi's 062 has 0..end, which suggests an armoured or invincible special move.

**Confidence:** **confirmed** (flag → FUN_088377d0 → the hit is skipped in FUN_08808ee4).
- **Test:** give an attack motion `0x0006 {0,65535}` and let the CPU hit you during it. No damage should register.
- Note: 0x0006 does **not** stop your own hits from landing. It is not armour, because the hit is skipped rather than absorbed.

### 0x0022 — Attack over: its hits are no longer a threat (CPU AI hint)

**Kind / args:** ranged, no args. In every retail case it runs from just after the last hit window to 65535.

**What the handler does:** 0x0882B1CC: `lbu 3(s1+0x60); ori 4; sb`, which sets **actor+0x193 bit 0x04**. No other handler sets this bit.

**Who uses it / effect in game:** the only reader is the accessor **FUN_08837a24** (`lbu 0x193; andi 4`). Its callers are all in the CPU AI:
- **FUN_08a8310c**, called from the AI tick FUN_08a63c8c at 0x08A63DA0 and 0x08A63DC8, once for each of two actors. It returns 0 early unless the examined actor is in an action and does **not** have 0x0022 set (0x08A831A4..0x08A831C0). Only then does it evaluate the current action record (FUN_08a829d8 → FUN_08b30898 = record at +0x1020/+0x101C, and FUN_08a82c20 / FUN_08a82cbc), and it reads +0x193 bit 0x08 (op 0x0029) too. In other words, an action marked with 0x0022 stops being treated as an incoming or ongoing attack.
- **FUN_08a83590**, which fills the AI's situation table. It stores the opponent's 0x0022 bit (`s7`, from 0x08A835FC) at +0x70. If that bit is set it asks whether the AI's own actor can act (vtable +0x150) and stores the result at +0x7C (0x08A83744..0x08A83770). In other words, this is a punish opportunity: the opponent's attack is over and I can act.
- No reader in the player input handler or the hit code was found. That covers byte reads through a +0x190 or controller+0x60 base, and direct `0x193(` reads. Word-wide reads of actor+0x190 were not exhaustively excluded.

**Data check:** fits "ends right after the last active frame".
- Noctis 043: hits to 26, 0x0022 at 26. Noctis 045: hit 9..20, 0x0022 at 20. Noctis 062: last hit 38..56, 0x0022 at 56.
- Block motions 020/021: block volume 0..36, 0x0022 at 38.
- Motions 006/007 run from 12, after the 0..10 invincibility.
- Vivi's multi-variant motions (050–061) carry one 0x0022 per variant with different `x`.

**Confidence:** **strong** for "AI reads it as the end of the attack threat or the start of a punish window". It is **weak** for any effect on human players, since I found none.
- **Test:** give a CPU-controlled character an attack motion with 0x0022 removed. The opposing CPU should keep guarding or evading through the recovery and punish less. Moving 0x0022 earlier should make the CPU punish sooner. A human player should notice no change.

### 0x0028 — Recovery cancel: dodge allowed, and the CPU may pick its next move

**Kind / args:** ranged, no args. Always runs from tick N to 65535, usually 6–40 ticks after 0x0022 and inside or after the chain window.

**What the handler does:** 0x0882B2D0: `lbu 4(s1+0x60); ori 4; sb`, which sets **actor+0x194 bit 0x04**. The same bit is set by 0x0059, but only when the target is not in hitstun (0x0882B2E4..0x0882B344).

**Who uses it / effect in game:** two readers.
- **FUN_08b3a5bc** (0x08B3A67C), the dodge gate. If +0x194 bit 0x04 is set it returns 1 at once (0x08B3A68C → 0x08B3A710). It does the same for +0x193 bit 0x01 (from 0x041E) and for `+0x384 & 0x40/0x80`. Without these bits, an actor that is in an action can only dodge through narrower rules: while blocking with motion time below a constant, or through the ability 0x110/0x111 paths.
  - Callers: **FUN_08b2d84c**, the "try dodge" function, calls it first (0x08B2D874) and then starts the ground or air dodge (FUN_08a54cd0, FUN_08a588fc, FUN_08a56a5c, FUN_08a61ac8). It is reached from the player handler FUN_08b3526c at 0x08B35CD4 and 0x08B35D00. The gate is also used by FUN_08b3315c (an input check in the same handler, 0x08B3584C) and by the CPU code FUN_08ab344c and FUN_08a72a28.
  - **Meaning: from this tick on, the move can be cancelled into a dodge.**
- **FUN_08a64400**, the CPU AI tick (0x08A646B4). While its own actor is busy (FUN_08a82638(…, 0xE)), the AI waits in mode 1 (FUN_08a96eb8(+0x530, 1)). Once the actor's +0x193 bit 0x01 (0x041E) or +0x194 bit 0x04 (0x0028) is set, it runs the decision maker FUN_08a9ff80, which tests vtable +0x150 several times, and picks a new plan.
  - **Meaning: a CPU character does not decide its next action until 0x0028 or 0x041E.**
- The player's attack gate (vtable +0x150) and block gate (+0x158) do **not** read this bit. Attacks still need the chain window (0x0101), 0x0005 or 0x041E.

**Data check:** fits.
- Noctis 045: hit ends at 20, chain 30..52, 0x0028 at 40. Noctis 062: chain 56..106, 0x0022 at 56, 0x0028 at 66. Noctis 048: hit ends at 6, chain 10..30, 0x0028 at 26.
- Vivi 043: 0x0022 at 24, 0x0028 at 44.
- So 0x0028 is a late "dodge-cancel / CPU free" point that falls before the chain window closes.

**Confidence:** **strong** (code end to end for the dodge gate and for the AI re-decide; the button and player-handler wiring is only partly traced).
- **Test:** in a move without 0x041E, try dodging in its recovery before and after the 0x0028 tick. The dodge should come out only after it. Remove 0x0028 and the CPU should stand idle until the motion ends.
