# Agent C: ops 0x0214, 0x0227, 0x030C

**Summary**
- **0x0214 = face texture pattern** {TexAnm group, pattern number}. While the range is active it switches one of the model's texture-animation groups (built from GMO "target:/source:/default:" blocks) to the named pattern. The group reverts to its default 1–2 ticks after the range ends. Confirmed in code. Which group is the eyes and which is the mouth is still a guess.
- **0x0227 = physics bone follows the animation** {physics bone index, max deg/tick}. Every tick of the range it re-seats one spring-physics bone (hair, cloth) on its animated pose, turning it at most arg1 degrees per tick (180 = snap). Physics resumes when the range ends. Strong.
- **0x030C = motion camera** {camera preset, camera bone 0/1, unused}. For battle fighters it sets the battle camera's mode-2 parameters and raises actor+0x191 bit 0x01. While that bit is set, the camera switches to mode 2, which takes eye, target, up and FOV from the model's `camera_include` (arg1=0) or `camera2_include` (arg1=1) bone; the old mode comes back when the range ends. Confirmed. Retail uses it only in EX Burst and script 200/203 cinematics.

Conventions: "actor" = character (dispatcher s2/s3 = *(owner+0x30)); "owner" = dispatcher a1 = motion player = **actor+0x130**, so owner+0x60 = actor+0x190 (cleared every tick by FUN_0882FBFC: `memset(owner+0x60,0,0x10)`, called from FUN_08829848). FUN_0883739C(actor) returns 1 when actor+0x542 (actor kind) is 0 or 1 (battle fighters); kind 2 is a different actor class (module 0x08804000–0x08806200, flags at +0x580; probably cutscene/menu models, guess).

Note on tools: `psd.py` prints `(delay)` instead of the delay-slot instruction after `jr $ra`. Two of the functions below (08B3E840/08B3E864) look like no-ops in psd, but their stores are in the delay slot (checked in all.s). Anyone else reading psd output should check delay slots.

---

### 0x0214 — Face texture pattern (eyes/mouth expression)

**Kind / args:** ranged, 2 args. arg0 = texture-animation **group id**: the number after `_` in the GMO TexAnm block name, stored in group entry +0x20. arg1 = **pattern number**: the number after `_` in one of the group's pattern names (entry +0x54 list). Seen: arg0 ∈ {0,1,2,3}, arg1 ∈ {1..7}. Nothing happens if the group or pattern is not in the model.

**What the handler does:** the case for code 0x14 is 0x0882A998 (table 0x089C7910[0x14]). It runs every tick of the range, with no first-tick test, and calls `FUN_08831CEC(actor, arg0, arg1)`:
- `FUN_0886B254(model=actor+0x394, arg0)` returns the index of the group whose +0x20 == arg0. The groups are an array of 0x60-byte entries at `*(model+0x84)->+0`, with the count at `->+4`.
- `FUN_0886B2A8(model, idx, arg1)` returns the index of the pattern whose number == arg1. The pattern count is s16 at +0x48 and the list is at +0x54.
- If the pattern differs from the current one (s16 +0x4A), it calls `FUN_0886A9B0(model, idx, pat, -1)`. That sets +0x4A = pat and +0x24 = pat, and zeroes +0x4C (frame), +0x4E (timer) and +0x46 (state), so the pattern animation restarts.
- Finally it sets `actor+0x528[idx] = 2` (0x08831D8C–0x08831D9C).

**Who uses it / effect in game:**
- **Revert to default:** `FUN_08831DBC`, called each frame from FUN_0883C2E8 at 0x0883C588, steps each group's override state. A state of 2 becomes 1. A state of 1 makes it call `FUN_0886A9B0(model, i, default=entry+0x50, -1)` and sets the state to 0. So the override lasts while the event keeps refreshing it, and the group goes back to its `default:` pattern one or two ticks after the range ends.
- **Where the groups come from:** `FUN_0886D8D4` parses GMO blocks of type 0x0F / 0xF1. Nearby strings are "target:", "source:", "default:" and "TexAnm" (0x089C959C..). "target:"/"source:" resolve texture names through FUN_08869F4C (GMO texture blocks 0x0A/0x12), and the rectangle goes to +0x28..+0x2E. Each other sub-block is a pattern, with its frame list at +0x58[pat] (0x0886DEF0).
- **Playback:** `FUN_0886E5D0` plays the current pattern. It walks the frame list of +0x4A, writes UV offsets (+0x30/+0x32), sets the dirty flag (+0x44) and uses random frame durations (RNG at 0x09B26B0C). A pattern can therefore be an animation, for example random blinking, rather than a single still image.
- **Other users of FUN_08831CEC:** kind-2 actors only (FUN_08804BA0 slots at +0x604/+0x584; FUN_08805728), so the same system also drives cutscene/menu faces.

**Data check (422 uses):**
- arg0 0 and 1 always come as a pair with the same pattern, which suggests the two eyes (guess). Group 2 changes on its own, mostly at attack and hit moments, which suggests the mouth (guess). Group 3 appears only in Vivi (Shantotto) timelines and is unknown.
- Damage/knock-back motions (scripts 500–750; Noctis motions 023–036) have {0,3},{1,3},{2,6} over the whole motion; Y'shtola has {0,5},{1,5},{2,3}; Vivi has {0,4},{1,4},{2,2}. That fits a pained face.
- Attacks: Noctis 039 has {2,5} over 0–22 and then {2,6} over 22–60 around the 24–28 hit; Noctis 047 has {2,7} over 69–92, which matches the 69–87 hit window. That fits the mouth opening on a shout.
- So the earlier "hands?" guess is wrong: the op changes face textures, not hands.

**Confidence:** **Confirmed** that it switches a GMO texture-animation group to a pattern for the duration of the range (traced from the handler to the UV writer). Which group number is which facial part is a strong guess and needs the model's TexAnm names. In-game test: put {2,7} over a long idle range and look at the mouth; put {0,3} alone and check whether only one eye changes.

---

### 0x0227 — Physics bone follows the animation (pin hair/cloth to the anim)

**Kind / args:** ranged, 2 args. arg0 = **physics bone id**: the s16 at +0 of a physics-chain record, which is a GMO bone index (FUN_0880D82C passes it to FUN_089B0E78 to get the bone). arg1 = **maximum correction angle per tick, in degrees**; 180 means snap at once. Seen: {37,30},{62,30},{38,30},{63,30},{49,30},{51,30}, and many {5..53,180}. arg0 = -1 (0xFFFFFFFF) is also seen; it matches no bone, so it does nothing.

**What the handler does:** the case for code 0x27 is 0x0882AAE4. It runs only if actor+0x542 ∈ {0,1,2,4}; otherwise it exits. It runs every tick of the range:
- `obj = *(actor+0x398)` is the physics (spring-bone) set: count at +0x20, 0x170-byte entries at +0x34.
- `e = FUN_0880CD9C(obj, arg0)` finds the entry whose `*(e+0x158)->s16 +0` == arg0, or returns null.
- If e is found: if `e+0x164 == 0` it sets `e+0x164 = 1`; it sets `e+0x150 = 0`; it sets `e+0x168 = arg1 * (π/180)`. The constant at 0x089C78F0 is 3.14159; the code is 0x0882AB60–0x0882ABA0.

**Who uses it / effect in game:** the per-frame physics update `FUN_0880EA20(obj, …, actor)` runs for battle actors from FUN_08B30814 at 0x08B3086C. For each entry:
- **e+0x150 != 0** (0x0880F038): normal spring simulation, and +0x164 is cleared (0x0880F5B8).
- **e+0x150 == 0** (the "re-seat" path, set by this op every tick):
  - It builds the bone's animated pose matrix (sp+0x240).
  - If `e+0x164 == 1`, it measures the angle between the simulated and the animated bone direction with `FUN_08867FEC` (acos of the dot product).
    - If the angle is less than `e+0x168`, it snaps to the animated pose and sets `e+0x164 = 2` (0x0880F380–0x0880F3A8).
    - Otherwise it rotates the simulated bone toward the animation by exactly `e+0x168` radians around their cross-product axis (0x0880F3AC–0x0880F564: sin/cos at 0x089A47A4/0x089A46DC, rotation matrix, FUN_0880CAB4).
  - If `e+0x164 == 2`, it snaps directly (0x0880F57C).
  - It then sets `e+0x150 = 1`.
- Because the op runs every tick, the bone is pulled onto its keyframed pose at no more than arg1 °/tick and then stays locked to the animation. One tick after the range ends, e+0x150 stays 1, so the spring simulation takes over again from the animated pose without a pop.

**Data check:**
- Y'shtola (Yuna-based) EX Burst motions 075–078 (scripts 8000/8101/8500/8600) pin 10–11 bones at 180 for the whole motion: 41, 38, 28, 25, 53, 17, 11, 5, 49, 51 (+47). Intro/victory motions 079/082 (scripts 1100/1210) pin subsets. Cinematics want the hair and skirt to follow the authored animation, so this fits.
- Noctis 002 (script 100) and 068 (Apocalypsis midair) pin bones 37/62 (+38/63 in 068 over 0–150) at 30°/tick. That is a soft blend back to the animated pose during fast motion.
- Y'shtola 080 uses {49,30},{51,30}.

**Confidence:** **Strong**: the code path is traced end to end. The one inference is that the target matrix at sp+0x240 is the animated bone pose; it is built from the bone's GMO matrix in the same block. In-game test: on a character with long hair, add {hair bone, 180} over a run or dash motion and the hair should stay rigid with the animation. With 5 instead, it should drift slowly back to the animated pose.

---

### 0x030C — Motion camera (camera from the model's camera bone)

**Kind / args:** ranged, 3 args. For battle fighters (kind 0/1): arg0 = **camera preset index** into the camera parameter table; if arg0 is at or above the count it becomes 0. arg1 = **camera bone**: 0 = `camera_include`, 1 = `camera2_include`; 2 or more becomes 0. **arg2 is never read.** Seen: {1,0,0}, {5,0,0}, {4,0,0}.

**What the handler does:** the case for code 0x0C is 0x0882A818.
- **Kind 0/1** (FUN_0883739C true):
  - `cam2 = *(actor+0x5AC) + 0x168`. actor+0x5AC is the actor's battle camera; +0x168 is its mode-2 object (constructor FUN_08B3E3C8, vtable 0x08B3F118).
  - `FUN_08B3E840(cam2, arg0)` → `cam2+4 = arg0`, clamped against `*(*(0x089D4790)+8)`.
  - `FUN_08B3E864(cam2, arg1)` → `cam2+8 = arg1`, where values of 2 or more become 0.
  - Both stores are in the `jr $ra` delay slots at 0x08B3E860 and 0x08B3E878.
- **Kind 2:**
  - If arg0 == 0, it calls `FUN_08806B1C`: actor+0x580 |= 0x40. Otherwise it calls `FUN_08806B58`: actor+0x580 |= 0x80.
  - On the first tick (s4) either function also calls `FUN_08805C0C`, which takes camera ownership: it sets bit 0x100 on this object and clears it on all others.
- **All kinds:** `owner+0x61 |= 1`, which is **actor+0x191 bit 0x01**, cleared every tick (0x0882A89C–0x0882A8AC).

**Who uses it / effect in game:**
- `FUN_088378D8(actor)` returns actor+0x191 & 1. It has two callers:
  - **Battle camera update `FUN_08B3C760(cam)`**, at 0x08B3C858. If the followed actor (cam+0xF8) has the bit, the latch cam+0x118 is clear and cam+0xFC == 0, it saves the current mode (cam+0x110 = cam+0x108 unless that is 3) and calls `FUN_08B3B3EC(cam, 2, 0)` to switch to mode 2. It then sets the latch. When the bit disappears it clears the latch and restores the saved mode, or mode 3 if cam+0x119 == 1 (0x08B3C8AC–0x08B3C8D4).
  - `FUN_08B3AFD4`, a camera mode toggle, also reads the bit.
- **Mode 2 = camera on a bone.** Its per-frame method `FUN_08B3E6D0` calls `FUN_08836A30(actor, eye, target, up, fov, t1 = cam2+8)`. That reads the model's named bone `0xA4[0x11 + t1]`. The named-bone table at 0x089D5F74 has index 17 = `camera_include` and 18 = `camera2_include`. Eye, target, up and FOV are copied to the camera (+0x20/+0x60/+0xA0/+0xD0).
- **Preset `cam2+4`** selects a 0x38-byte record from `*(0x089D479C)` (FUN_08B3E8B0); the table is loaded by FUN_08811598.
  - Flag +0x34 & 4: the camera looks at the actor (+0x100).
  - Flag & 2: it looks at the opponent.
  - Neither flag: it looks along the bone at distance +0x30.
  - Flag & 8: it keeps the bone's up vector; otherwise up = (0,1,0).
  - In init (FUN_08B3E404): if flag 0x20 is clear, it ORs a mask (from 0x08B3F240/0x08B3F244) into actor+0x388/+0x38C, and flag 0x40 sets +0x380 bit 0x800000 on the target. The floats +0x10..+0x28 go to FUN_08B3BECC.. (unknown).
- **Kind 2 consumer:** `FUN_08805A38`, if the object owns the camera (0x100) and 0x40 or 0x80 is set, calls FUN_08836A30 with mode 0/1. It pushes the result to the global camera (`*(0x089D5EC0)+0x720`) with FOV through FUN_0887358C, then clears 0x40 and 0x80. For kind 2, arg0 picks the bone instead of arg1.

**Data check:** it covers the whole motion (0–65535) in every case:
- motion 008 (scripts 200/203) for all three characters: {1,0,0}, twice with pad 1 / pad 2, starting at ticks 10 and 0. Probably the EX Mode activation pose (guess).
- EX Burst motions with preset 5: Noctis 071–078 (scripts 8000–8106, 8500), Y'shtola 075–077, Vivi 062 (8000).
- Vivi 064 (8500, the burst finisher): {4,0,0}.

All of these are cinematics that use an authored camera, so the meaning fits.

**Confidence:** **Confirmed** that it switches the fighter's battle camera into bone-driven mode 2 for the range, using preset arg0 and bone arg1. What each preset number looks like is unknown without decoding the parameter file. In-game test: add {5,0,0} over a normal attack motion that has a `camera_include` bone and the camera should follow that bone. Using arg1 = 1 on a model without `camera2_include` should make FUN_08836A30 return 0, so the camera keeps its last values.
