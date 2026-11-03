/* Presses whose action runs one of the game's shared scripts (not in the character's motion_map.json): the game's code
   picks the character's own scripts for them, so the data alone can't say which animation plays. These were traced in
   the native build (psprecomp/games/dissidia/tools/script_trace.py: every fighter's script id, frame by frame) and tell
   the Studio which script, and so which animation, the press really plays, and what follows it.
   { character code: { action id: { start, on_hit, on_landing, traced } } } */
const TRACED_SCRIPTS = {
  sev200: {                                        // Sephiroth: Hell's Gate (command 0x1BB), action 62 AIR_ABI_FINISH, script 48
    62: { start: 10060, on_hit: 10062, on_landing: 10061,
          traced: '28 Oct 2026: 10060 (the dive) every time; 10062 when the dive hits, 10061 (the action\'s second script) when it reaches the ground' },
  },
};
function tracedFor(action) {
  const code = (S.mod && S.mod.character && S.mod.character.code) || (S.rig && S.rig.retail && S.rig.retail.code);
  return (code && TRACED_SCRIPTS[code] && TRACED_SCRIPTS[code][action]) || null;
}
