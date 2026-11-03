#!/usr/bin/env python3
"""Writes studio/ops.js: the Move Studio's event catalogue.

For every event op: its name and argument names (charmod_objx.NAMED), a label, a category, a one-line description
(from docs/moves-and-events.md), whether it is ranged (start..end) or keyed (one frame), and the shape retail moves give it
(studio_op_shapes.json: most common args and length, from all 30 fighters). Run after changing NAMED:

    python3 games/dissidia/tools/studio_ops.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import charmod_objx as CO  # noqa: E402

CATS = ['Hits', 'Combo & cancels', 'Defence', 'Movement', 'Weapons & parts', 'Effects & sound', 'Camera & screen',
        'EX Burst', 'CPU', 'Other']

# op: (category, label, description)
INFO = {
    0x0200: ('Hits', 'Hit window', 'The attack can connect from start to end. hit = the hit profile (damage, knock-back), group = the hitboxes.'),
    0x0112: ('Hits', 'Weapon volume', 'Turns on a group that never deals damage (no hit profile). Retail uses it for projectile-contact spheres (the weapon meets shots), weapon-trail points, or extra hurt / push spheres. Often outlasts the hit window, and about 1 in 5 run with no hit at all (blow-away motions, shot-only moves).'),
    0x0743: ('Hits', 'Hit window + projectile', 'Hit window that fires a projectile when it connects (and can show a caption).'),
    0x027B: ('Hits', 'Guard counter hit', 'Hit window that is live only after your guard blocked something.'),
    0x0108: ('Hits', 'Forced hit', 'Hits the foe with this hit at once: no hitbox, no range check (EX Bursts).'),
    0x0204: ('Hits', 'Fire projectile', 'Fires a projectile record (projectiles.json).'),
    0x0524: ('Hits', 'Button shot', 'Pressing ○ / □ during the range fires extra projectiles.'),
    0x0633: ('Hits', 'Shot by presses', 'Fires a projectile depending on how many presses the move got.'),
    0x042D: ('Hits', 'Projectile variant', 'Projectile, or its alternate after a rare-variant roll.'),
    0x0101: ('Combo & cancels', 'Chain window', 'The next press of the combo is accepted here.'),
    0x0467: ('Combo & cancels', 'Chain + follow-up', 'Chain window that also offers an action as the next move when the input matches.'),
    0x0278: ('Combo & cancels', 'Chain window if hit', 'Chain window that opens only if this hit landed.'),
    0x0579: ('Combo & cancels', 'Follow-up if hit', 'Chain + follow-up, only if a hit you landed matches.'),
    0x0120: ('Combo & cancels', 'Chase window', 'After this hit blew the foe away, a press starts the Chase.'),
    0x0005: ('Combo & cancels', 'Can act', 'Neutral: may attack, block, dodge, dash. Stand / idle / fall motions carry it whole.'),
    0x041E: ('Combo & cancels', 'Can act now', 'End of recovery: may act at once. Non-zero args block jump / dash / steering or force a different / the same move.'),
    0x047A: ('Combo & cancels', 'Can act now if', 'Can act now, only if the move hit (or only on a whiff).'),
    0x0028: ('Combo & cancels', 'Dodge-cancel allowed', 'May dodge out of the move from here; the CPU picks its next move here too.'),
    0x0059: ('Combo & cancels', 'Dodge-cancel if foe free', 'Dodge-cancel, only while the foe is not reeling.'),
    0x003A: ('Combo & cancels', 'Attack out of dodge', 'Brave, HP and EX inputs may start a move (end of dodges).'),
    0x0036: ('Combo & cancels', 'Brave attack allowed', 'Only the Brave input may start a move now.'),
    0x0158: ('Combo & cancels', 'Act after guard', 'After a successful guard, may act at once.'),
    0x0072: ('Combo & cancels', 'Can guard', 'May block or start a guard-type move as if neutral.'),
    0x016C: ('Combo & cancels', 'Dash cancel', 'Dash-cancel window (optionally only after a hit).'),
    0x004A: ('Combo & cancels', 'No jump', 'Jumping blocked.'),
    0x004C: ('Combo & cancels', 'Timed-input latch', 'A press here voids later timed-input windows.'),
    0x006A: ('Combo & cancels', 'No Assist / EX Revenge cancel', 'The move can not be cut short by Assist or EX Revenge.'),
    0x0069: ('Combo & cancels', 'Force Assist cancel', 'Assist / EX Revenge may always cut the move.'),
    0x0002: ('Combo & cancels', 'End of move', 'The move ends here.'),
    0x0006: ('Defence', 'Invincible', 'Incoming hits pass through (dodges: first 20 ticks). Own hits still land.'),
    0x0047: ('Defence', 'Invincible (with ability)', 'Invincible only with Evasion Boost equipped (dodges: doubles the invincible ticks).'),
    0x017C: ('Defence', 'Wake-up invincibility', 'Invincible for N ticks when getting up after a blow-away.'),
    0x0074: ('Defence', 'Perfect-dodge window', 'With Precision Evasion equipped, a well-timed dodge stays invincible (glow + icon).'),
    0x0145: ('Defence', 'Perfect-guard window', 'With Precision Block equipped, a well-timed guard is upgraded.'),
    0x002F: ('Defence', 'Counts as evading', 'The foe and the AI treat you as evading (not invincible).'),
    0x0049: ('Defence', "Can't be pushed", 'Other fighters are pushed off this one.'),
    0x004B: ('Defence', 'Heavy body', 'Push mass ×10.'),
    0x0013: ('Defence', 'No body collision', 'Pass through fighters: no pushing either way.'),
    0x0402: ('Movement', 'Face target', 'Turn toward the target, arg0 = max degrees per tick (180 = snap).'),
    0x040B: ('Movement', 'Turn to opponent', 'Turn toward the opponent, arg0 = max degrees per tick.'),
    0x046D: ('Movement', 'Turn to target', 'Turn to the lock-on target, with reeling / guarding conditions.'),
    0x086E: ('Movement', 'Turn to target / aim', "Turn toward the target, or twist the drawn body toward it (as Turn / body swivel). The first value, called 'mode' in the files, is the max degrees per tick."),
    0x0811: ('Movement', 'Turn / body swivel', 'Turn to the target, or twist the drawn body toward it within limits.'),
    0x0024: ('Movement', 'Stop turning', 'End any turn in progress.'),
    0x0210: ('Movement', 'Face flight direction', 'Face along your own velocity.'),
    0x0D10: ('Movement', 'Homing dash', 'Air dash toward the foe: speed, tracking ticks, height, angle, distance, arrive radius, max ticks.'),
    0x0925: ('Movement', 'Ground homing dash', 'Ground lunge toward the foe: speed, tracking ticks, angle, stand-off distance.'),
    0x0548: ('Movement', 'Straight lunge', 'Lunge at a fixed speed along the facing (pitch, speed); optionally only without a target.'),
    0x044F: ('Movement', 'Ground slide', 'Short slide on the ground (angle, speed).'),
    0x001D: ('Movement', 'Chase dash', 'Dash toward the foe at the start of a Chase.'),
    0x0123: ('Movement', 'Take off', 'Switch to an air action (where ground-follow hands over to air motion).'),
    0x0000: ('Movement', 'Enter state', 'Enter the movement state the move armed (jump fall, dodge, knock-back, homing).'),
    0x0518: ('Movement', 'Steer during move', 'The stick steers during the move (turn ×, speed ×).'),
    0x000A: ('Movement', 'Air motion', 'No ground snapping; counts as airborne.'),
    0x0015: ('Movement', 'Ground follow', 'Keep the feet on the floor.'),
    0x0032: ('Movement', 'Follow floor slope', 'Movement follows the floor slope.'),
    0x002D: ('Movement', 'Landing allowed', 'Lands when the height reaches 0.'),
    0x0023: ('Movement', 'Keep motion when falling', 'Keep this motion when converted to a fall.'),
    0x0038: ('Movement', 'Stop momentum', 'Zero velocity every tick.'),
    0x0030: ('Movement', 'Floating physics', 'Air-drag momentum; landing keeps velocity.'),
    0x013B: ('Movement', 'Gravity', 'Gravity multiplier.'),
    0x0062: ('Movement', '3D root motion', 'Root motion follows the full body orientation.'),
    0x0539: ('Movement', 'Pull foe', 'Pull the foe (and their assist) toward you.'),
    0x0070: ('Movement', 'Ground contact switch', "On ground contact, switch to the action's alternate script."),
    0x0D64: ('Movement', 'Aim bones at target', 'Up to two bones aim at the target (guns, bows).'),
    0x0D63: ('Movement', 'Aim bones at foe', 'Up to two bones aim at the opponent.'),
    0x0216: ('Movement', 'Pin physics group', 'A hair / cloth physics group follows the animation (≤ degrees per tick).'),
    0x0227: ('Movement', 'Pin physics bone', 'One hair / cloth bone follows the animation (180 = snap).'),
    0x0652: ('Movement', 'Steer lean', 'Player-steered body lean.'),
    0x0173: ('Movement', 'Assist faces partner', 'An assist turns toward its main fighter.'),
    0x011B: ('Weapons & parts', 'Show part', 'Draw a weapon / effect part for the range.'),
    0x011C: ('Weapons & parts', 'Hide part', 'Hide a part for the range.'),
    0x010B: ('Weapons & parts', 'Show part once', 'Draw a part from this frame on.'),
    0x010C: ('Weapons & parts', 'Hide part once', 'Hide a part from this frame on.'),
    0x075A: ('Weapons & parts', 'Hold prop', 'Hold a prop object (wpnp row).'),
    0x0C65: ('Weapons & parts', 'Summon object', 'Summon an object (wpnp row).'),
    0x025E: ('Weapons & parts', 'Prop hit volume', 'Hit volume on a weapon prop.'),
    0x045B: ('Weapons & parts', 'Prop hit', 'The weapon prop deals a hit.'),
    0x055F: ('Weapons & parts', 'Prop effect', "Trail from the prop's own effect data."),
    0x0360: ('Weapons & parts', 'Prop fade', 'Fade a prop out and back in.'),
    0x022E: ('Weapons & parts', 'Trail length', 'How far the weapon trail reaches along a bone.'),
    0x011E: ('Weapons & parts', 'Switch form', 'Switch form: model, moves, parameters.'),
    0x042A: ('Effects & sound', 'Bone effect / trail', 'Effect on a bone while the range runs (record 9999 = weapon trail).'),
    0x0219: ('Effects & sound', 'Effect', 'Play an effect (FEPA record).'),
    0x0466: ('Effects & sound', 'Timed effect', 'Effect once at a total motion time.'),
    0x042F: ('Effects & sound', 'Effect variant', 'Effect, or its alternate after a rare-variant roll.'),
    0x032E: ('Effects & sound', 'Rare-variant roll', '1-in-N roll for the alternate effect / projectile.'),
    0x0335: ('Effects & sound', 'Summon effect', 'Summons only: an effect attached to the summon.'),
    0x0303: ('Effects & sound', 'Sound', 'Play a sound effect.'),
    0x0311: ('Effects & sound', 'Sound if part visible', 'Sound only if a draw part is visible.'),
    0x0312: ('Effects & sound', 'Sound if form', 'Sound only in a form.'),
    0x0315: ('Effects & sound', 'Voice', 'Voice line.'),
    0x060F: ('Effects & sound', 'Footstep sound', 'Footstep / landing / impact sound by floor surface.'),
    0x0422: ('Effects & sound', 'Ground dust', 'Dust / splash where a bone meets the floor.'),
    0x072C: ('Effects & sound', 'Falling feathers', 'Feathers from random bones.'),
    0x0214: ('Effects & sound', 'Face texture', 'Switch a face texture (eyes 0/1, mouth 2) for the range.'),
    0x0475: ('Effects & sound', 'Pulse tint', 'Pulsing colour tint.'),
    0x0476: ('Effects & sound', 'Pulse tint (partner)', 'Pulsing tint on your partner.'),
    0x0340: ('Effects & sound', 'Fade self', 'Fade out and back (teleports).'),
    0x0109: ('Effects & sound', 'Fade self in', 'Fade yourself in.'),
    0x003D: ('Effects & sound', 'No shadow', 'No drop shadow.'),
    0x0B36: ('Effects & sound', 'Timed buff', 'Timed buff with a pulsing glow: adds to physical / magic attack, defence and crit chance for its ticks.'),
    0x0306: ('Camera & screen', 'Screen shake', 'Camera shake.'),
    0x0353: ('Camera & screen', 'Screen shake (range)', 'Shake that lasts the range.'),
    0x030C: ('Camera & screen', 'Motion camera', "Cinematic camera from the model's camera bone."),
    0x051B: ('Camera & screen', 'Screen flash', 'Full-screen flash (white / black / red).'),
    0x012B: ('Camera & screen', 'White flash', 'Solid white flash for the range.'),
    0x0133: ('Camera & screen', 'Dim stage', 'Darken the stage.'),
    0x021D: ('Camera & screen', 'Caption', '2D caption from the character pack.'),
    0x0126: ('EX Burst', 'Slow foe', 'The foe runs at arg0 × speed (0 = frozen).'),
    0x0134: ('EX Burst', 'Slow motion', 'The whole game runs at arg0 × speed.'),
    0x0121: ('EX Burst', 'Set foe speed', "Freeze or restore the foe (percent)."),
    0x0220: ('EX Burst', 'Fade foe out', 'Fade the foe out.'),
    0x011F: ('EX Burst', 'Fade foe in', 'Fade the foe back in.'),
    0x003E: ('EX Burst', 'Free victim', 'Stop holding the victim; they fall normally.'),
    0x0031: ('EX Burst', 'Release', 'Release the held victim.'),
    0x002B: ('EX Burst', 'Snap yaw to foe line', 'Snap yaw along the line to the foe.'),
    0x0022: ('CPU', 'Attack over (CPU)', 'The dangerous part is over; read only by the CPU AI.'),
    0x0029: ('CPU', 'CPU hint', 'Read only by the CPU AI.'),
    0x0131: ('Other', 'Brave bonus', "Adds to your Brave (to your partner's for an assist), scaled by the value / 100; clamped 0–9999. Exact formula open."),
    # settled 2026-10-31 (docs/archive/open-questions/events.md, atkp.md, event-ops-crosscheck.md)
    0x0051: ('Defence', 'Armor', "Brave hits don't interrupt your action (HP hits still do)."),
    0x0071: ('Defence', 'Protect self and partner', 'Hits cause no reaction and take no Brave from you or your partner; the attacker gets no on-hit extras.'),
    0x0077: ('CPU', 'CPU flag (0x0077)', 'Read only by the CPU AI; what it tells the AI is open.'),
    0x0225: ('Movement', 'Steer on ground', 'The stick moves you during the move (turn ×, speed ×), without the air state Steer during move uses.'),
}
KIND_ONLY = {0x0037: {'ranged': ('Combo & cancels', 'HP attack allowed', 'Only the HP input may start a move now.'),
                      'keyed': ('Other', "Release foe's hold", 'Ends the hang a reaction-19 hit put the foe in.')},
             0x0117: {'ranged': ('Movement', 'Physics rest follows pose', "A physics group's rest pose follows the animation."),
                      'keyed': ('Weapons & parts', 'Eject cartridge', 'Throws a shell casing from a bone.')},
             # ranged 0x0219 (dispatcher 0x0882AE0C, traced 2026-10-30): every tick FUN_08834E08 moves him toward a
             # point (the fighter the actor vtable +288 returns, its position with y + arg1) at up to arg0 m a tick,
             # never past it; retail: 137 uses, arg0 0.1-1.0, arg1 -1 .. 2
             0x0219: {'ranged': ('Movement', 'Move toward target', "Each tick moves him toward the target (its feet, plus the second value in height) by up to the first value, in metres; he stops on reaching it. The 'effect' field is that speed, not an effect."),
                      'keyed': ('Effects & sound', 'Effect', 'Play an effect (FEPA record).')}}

# Display-only names for values the timeline files keep unnamed in "args" (renaming them in charmod_objx.NAMED would
# change the file format). op (or 'op:kind') -> {value index (0 = the event's first value): (label, tip)}.
ARG_NOTES = {
    0x0101: {0: ('CPU flag', 'Only the CPU AI reads it.')},
    0x0112: {0: ('Group', 'The hit-volume group.')},
    0x0204: {1: ('Origin effect', 'Effect (FEPA) id an origin-6 projectile starts at, when not 0.')},
    0x0303: {1: ('No Doppler', 'Only with At body: 0 = the pitch shifts with speed (Doppler).', 'bool'),
             2: ('At body', 'Plays at the body-centre bone instead of following the fighter.', 'bool')},
    0x0306: {0: ('Duration (ticks)', '0 or less = 30.'), 1: ('X amplitude %', ''), 2: ('Y amplitude %', '')},
    0x0402: {1: ('Only if target reeling', '', 'bool'), 2: ('Even if target busy', 'Keeps turning while the target evades / guards.', 'bool'),
             3: ('Skip if target reeling', '', 'bool')},
    0x041E: {0: ('No jump / dash', 'Non-zero: jump, dash and steering are blocked (attack, block, dodge still work).', 'bool'),
             1: ('No hit-confirm cancel', '', 'bool'), 2: ('Not the same move', 'The next move must differ (also blocks jump / dash).', 'bool'),
             3: ('Only the same move', 'The next move must be this one (also blocks jump / dash).', 'bool')},
    0x047A: {0: ('No jump / dash', '', 'bool'), 1: ('Not the same move', '', 'bool'), 2: ('Only on a whiff', '0 = only if the move hit; 1 = only if it missed.', 'bool'),
             3: ('Only the same move', '', 'bool')},
    0x0518: {2: ('Move along facing', 'A guess: not confirmed.'), 3: ('Face move direction', ''), 4: ('Stick relative to facing', 'A guess: not confirmed.')},
    0x0225: {0: ('Turn ×', ''), 1: ('Speed ×', '')},
    0x051B: {1: ('Hold (ticks)', ''), 2: ('Fade out (ticks)', ''), 3: ('Colour', '0 white, 1 black, 2 red.'), 4: ('Unused', '')},
    0x055F: {2: ('Record bit', ''), 3: ('Scale', ''), 4: ('Prop', '')},
    0x0579: {0: ('CPU flag', ''), 1: ('Action', 'Offered as the next move when the input matches.'), 2: ('Even on a miss', ''),
             3: ('Mid-action', 'Input accepted mid-action.'), 4: ('Hit', 'The hit that has to have landed.')},
    0x060F: {1: ('Line probe', ''), 2: ('Volume probe', ''), 3: ('Dust foot A', ''), 4: ('Dust foot B', ''), 5: ('Fixed position', '')},
    0x0652: {0: ('Max pitch', ''), 1: ('Max roll', ''), 2: ('Pitch rate', ''), 3: ('Roll rate', ''), 4: ('Follow smoothing', ''),
             5: ('Return factor', '')},
    0x072C: {2: ('Bone 1', ''), 3: ('Bone 2', ''), 4: ('Bone 3', ''), 5: ('Bone 4', '−1 = none.'), 6: ('EX Mode only', '', 'bool')},
    0x0743: {3: ('Projectile arg', ''), 4: ('Show caption', '', 'bool'), 5: ('Caption', ''), 6: ('Caption flag', '')},
    0x075A: {0: ('Bone', 'Owner bone it hangs on (0x4000 = the owner itself).'), 1: ('Row', "wpnp row (the object)."),
             2: ('Pick', '1 = the game picks the row (Gilgamesh weapon roll).', 'bool'), 3: ('Release', '1 = left in the world where it appears; 0 = follows the bone.', 'bool'),
             4: ('Fade in', ''), 5: ('Fade out', ''), 6: ('Start', '')},
    0x0811: {3: ('Limit left °', ''), 4: ('Limit right °', ''), 5: ('Hold ticks', ''), 6: ('Smoothing', ''), 7: ('Decay', '')},
    0x086E: {2: ('Swivel', '0 = turn, 1 = twist the drawn body.'), 3: ('Limit left °', ''), 4: ('Limit right °', ''), 5: ('Hold ticks', ''),
             6: ('Smoothing', ''), 7: ('Decay', '')},
    0x0925: {6: ('Ignore target reaction', '', 'bool'), 7: ('Stop if target busy', '', 'bool'), 8: ('Alt target', '')},
    0x0B36: {0: ('Ticks', ''), 1: ('Phys attack +', ''), 2: ('Phys defence +', ''), 3: ('Phys crit +', ''), 4: ('Magic attack +', ''),
             5: ('Magic defence +', ''), 6: ('Magic crit +', ''), 7: ('Glow R', ''), 8: ('Glow G', ''), 9: ('Glow B', ''), 10: ('Glow period', '')},
    0x0C65: {0: ('Bone', 'Owner bone (0x4000 = the owner itself).'), 1: ('Row', 'wpnp row (the object).'), 2: ('Pick', '1 = the game picks the row.'),
             3: ('Release', ''), 4: ('Fade in', ''), 5: ('Fade out', ''), 6: ('X', ''), 7: ('Y', ''), 8: ('Z', ''), 9: ('On root', ''),
             10: ('Follow', ''), 11: ('Follow id', '')},
    0x0D10: {8: ('No rise', '', 'bool'), 9: ('No fall', '', 'bool'), 10: ('Ignore target reaction', '', 'bool'), 11: ('Stop if target busy', '', 'bool'), 12: ('Alt target', '')},
    0x0D63: {0: ('Bone A', ''), 1: ('Weight A', ''), 2: ('Bone B', ''), 3: ('Weight B', ''), 4: ('Yaw left °', ''), 5: ('Yaw right °', ''),
             6: ('Pitch up °', ''), 7: ('Pitch down °', ''), 8: ('° per tick', ''), 9: ('Hold ticks', ''), 10: ('Smoothing', ''), 11: ('Relax', ''),
             12: ('Player steered', 'Probably: strong evidence, not confirmed.')},
}
ARG_NOTES[0x0D64] = ARG_NOTES[0x0D63]

# How the inspector names an event's named values (charmod_objx.NAMED arg keys), sentence case; keys not here are shown
# with their underscores as spaces. Uncertain meanings stay out of the label (the field's tip says so).
ARG_LABELS = {
    'hit': 'Hit', 'group': 'Hitbox group', 'projectile': 'Projectile', 'alt_projectile': 'Alt projectile', 'effect': 'Effect',
    'alt_effect': 'Alt effect', 'lifetime': 'Lifetime (ticks)', 'alt_lifetime': 'Alt lifetime (ticks)', 'part': 'Part',
    'draw_part': 'Needs part', 'ticks': 'Ticks', 'form': 'Form', 'sound': 'Sound', 'voice': 'Voice', 'positional': 'Positional',
    'speed': 'Speed', 'percent': 'Speed (%)', 'deg_per_tick': '° per tick', 'max_deg_per_tick': 'Max ° per tick',
    'backwards': 'Backwards', 'only_without_target': 'Only without a target', 'only_after_hit': 'Only after a hit',
    'value': 'Value', 'brightness': 'Brightness', 'multiplier': 'Multiplier', 'off': 'Unknown', 'face_part': 'Face group',
    'pattern': 'Pattern', 'image': 'Image', 'floor_percent': 'Fade (%)', 'extent': 'Reach', 'slot': 'Slot', 'prop': 'Prop',
    'cpu_flag': 'CPU flag', 'preset': 'Camera preset', 'camera_bone': 'Camera bone', 'only_if_alt_off': 'Only if alt off',
    'only_if_alt_on': 'Only if alt on', 'min': 'Minimum', 'unlock_bit': 'Unlock bit', 'one_in': 'One in N',
    'fade_out': 'Fade out (ticks)', 'fade_in': 'Fade in (ticks)', 'fade_back': 'Fade back (ticks)', 'hold': 'Hold (ticks)',
    'opacity': 'Opacity', 'duration': 'Duration (ticks)', 'x_amp': 'X amplitude', 'y_amp': 'Y amplitude', 'offset': 'Offset',
    'size': 'Size', 'variant': 'Variant', 'record': 'Effect record', 'record_bit': 'Record bit', 'scale': 'Scale',
    'arg': 'Argument', 'alt_arg': 'Alt argument', 'angle': 'Angle (°)', 'skip_if_target': 'Skip with a target',
    'after_homing': 'After homing', 'modifier': 'Modifier', 'tick': 'At tick', 'flag': 'Flag', 'action': 'Action',
    'even_on_miss': 'Even on a miss', 'mid_action': 'Mid-action', 'only_if_reeling': 'Only if foe reeling',
    'only_if_not_reeling': 'Only if foe not reeling', 'skip_if_reeling': 'Skip if foe reeling',
    'even_if_evading': 'Even if foe evades', 'r': 'Red', 'g': 'Green', 'b': 'Blue', 'period': 'Period (ticks)',
    'turn_mult': 'Turn ×', 'speed_mult': 'Speed ×', 'colour': 'Colour', 'cooldown': 'Cooldown (ticks)', 'button': 'Button',
    'outer_radius': 'Outer radius (m)', 'inner_radius': 'Inner radius (m)', 'pitch': 'Pitch (°)', 'yaw': 'Yaw (°)',
    'kind': 'Kind', 'count': 'Count', 'if_le': 'If at most', 'if_eq': 'If exactly', 'if_ge': 'If at least', 'rate': 'Rate',
    'chance': 'Chance', 'away': 'Turn away', 'swivel': 'Swivel', 'mode': 'Mode', 'track_ticks': 'Track (ticks)',
    'distance': 'Stand-off (m)', 'arrive_radius': 'Arrive radius (m)', 'max_ticks': 'Max ticks', 'height': 'Height (m)',
    'vspeed': 'Vertical speed',
}
# named values that are a 0 / 1 switch (the inspector shows them as a two-button switch while they hold 0 or 1)
BOOL_ARGS = ['positional', 'backwards', 'only_without_target', 'only_after_hit', 'only_if_alt_off', 'only_if_alt_on',
             'skip_if_target', 'after_homing', 'even_on_miss', 'mid_action', 'only_if_reeling', 'only_if_not_reeling',
             'skip_if_reeling', 'even_if_evading', 'away']


def build():
    shapes = json.load(open(os.path.join(HERE, 'studio_op_shapes.json')))
    ops = {}
    every = set(INFO) | set(CO.NAMED) | set(KIND_ONLY) | {int(k, 16) for k in shapes}
    for op in sorted(every):
        sh = shapes.get('0x%04X' % op, {})
        name, argn = CO.NAMED.get(op, (None, []))
        for kind in (('ranged', 'keyed') if op in KIND_ONLY else (None,)):
            cat, label, desc = (KIND_ONLY[op][kind] if kind else INFO.get(op, ('Other', None, '')))
            kinds = [k for k in ('ranged', 'keyed') if k in sh] or ['ranged']
            if kind:
                kinds = [kind]
            e = {'op': '0x%04X' % op, 'name': name, 'args': argn[:op >> 8], 'nargs': op >> 8, 'cat': cat,
                 'label': label or (name.replace('_', ' ').capitalize() if name else '0x%04X' % op), 'desc': desc,
                 'kinds': kinds, 'uses': sum(v['uses'] for v in sh.values()),
                 'shape': {k: sh[k] for k in sh if k in kinds}}
            notes = ARG_NOTES.get('0x%04X:%s' % (op, kind) if kind else None) or ARG_NOTES.get(op)
            if notes:
                e['arg_notes'] = {str(i): list(v) for i, v in notes.items()}
            ops[('0x%04X' % op) + (':' + kind if kind else '')] = e
    reac = {'0x0D': 'stagger', '0x0B': 'knock', '0x0C': 'blow', '0x04': 'blow away', '0x06': 'launch', '0x01': 'flinch / hold'}
    return {'cats': CATS, 'ops': ops, 'hit_info': CO.HIT_INFO, 'reactions': reac, 'arg_labels': ARG_LABELS,
            'bool_args': BOOL_ARGS}


if __name__ == '__main__':
    out = os.path.join(HERE, '..', '..', '..', '..', 'studio', 'ops.js')
    data = build()
    with open(out, 'w') as f:
        f.write('// Generated by tools/studio_ops.py from charmod_objx.NAMED and docs/moves-and-events.md. Do not edit.\n')
        f.write('window.STUDIO_OPS = ' + json.dumps(data, ensure_ascii=False, separators=(',', ':')) + ';\n')
    print('wrote', os.path.normpath(out), len(data['ops']), 'ops')
