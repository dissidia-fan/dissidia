# Effect components (24 Oct 2026)

Six notes, one per part of the game's effect code, read from the recompiled EBOOT. The summary is [../../effects.md](../../effects.md). Move Studio's renderer (`studio/fx.js`) follows them.

- [A.md](A.md): placement (FEPA records, attach bits), timing (segments, loops, stopping), the transform channel (14), colour, blending and depth.
- [B.md](B.md): meshes (channel 11, the MDL resource).
- [C.md](C.md): flipbooks (channel 10, the ANM resource).
- [D.md](D.md): particle emitters (channel 21), randomisers (22–24), the node spawner (18).
- [E.md](E.md): weapon trails (105/106), mesh-vertex spawners (26/30/150), the VTL resource, scrolling meshes (20), events 0x042A and 0x022E.
- [F.md](F.md): every other channel (flash, blurs, shockwave, sparks, flicker, randomisers) and the dispatch tables.
- [G.md](G.md): 8888 weapon glows (bone_effect), channel 111 (a screen shift, not a rotation blur), and the line / point emitters 100, 102, 103, 95 (82 and 58 partly).
