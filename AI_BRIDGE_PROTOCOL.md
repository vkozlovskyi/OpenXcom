# AI Bridge Protocol Reference

TCP socket server on `127.0.0.1:12345`. JSON-lines protocol (one JSON per line, `\n` delimited).

## Connection Flow

1. Client connects via TCP
2. Server sends: `{"type":"connected","version":"1.0"}`
3. If it's player's turn, server sends `turn_start` (see below)
4. Client sends commands, server responds
5. Reconnection: new client replaces old one automatically

---

## Server Push Messages (no command needed)

### turn_start
Sent automatically at the start of each player turn. Contains full game state.

```
{
  "type": "turn_start",
  "turn": 1,
  "map": {"size_x": 50, "size_y": 50, "size_z": 3, "global_shade": 5},
  "units": [...],           // player units (full detail)
  "visible_enemies": [...], // enemies visible to any player unit
  "ascii_map": {"0": "...", "1": "..."},  // ASCII map per z-level
  "map_legend": {"a": "CULTIVAT", "u": "UFO", "s": "craft", "/": "stairs", "^": "gravlift"},
  "ufo_bounds": {"x_min":10, "y_min":20, "z_min":0, "x_max":15, "y_max":25, "z_max":1},
  "craft_bounds": {"x_min":12, "y_min":28, "z_min":0, "x_max":16, "y_max":36, "z_max":1}
}
```

### battle_end
```
{"type": "battle_end"}
```

---

## Client Commands

### get_state
Read-only query. Works even while another action is animating.

```json
{"action": "get_state", "include_map": true}
```

Response type: `game_state` (same structure as `turn_start`, plus `events`).
If `include_map` is false (default), `ascii_map` and `map_legend` are omitted.

### select
Select a unit (no TU cost). Required before other actions if unit isn't already selected.

```json
{"action": "select", "unit_id": 3}
```

### walk
Move a unit to target tile. Async — unit animates, then `action_complete`.

```json
{"action": "walk", "unit_id": 3, "target": [10, 15, 0]}
```

Optional `"exact": true` — if unit cannot reach the exact target tile, return error instead of partial movement:
```json
{"action": "walk", "unit_id": 3, "target": [10, 15, 0], "exact": true}
```

Errors: `no_path`, `not_enough_tu` (with `exact`: includes `tu` and `tu_cost` fields), `unit_not_found`

### shoot
Fire a weapon at a tile. `shot_type`: `"snap"`, `"aimed"`, `"auto"`. `hand`: `"right"` (default) or `"left"`.
**Cannot be used on waypoint weapons (Blaster Launcher)** — use `launch` instead.

```json
{"action": "shoot", "unit_id": 3, "target": [10, 15, 0], "shot_type": "aimed", "hand": "right"}
```

Errors: `no_weapon`, `no_ammo`, `not_enough_tu`, `wrong_action_type`

### launch
Fire a waypoint weapon (Blaster Launcher) at a target tile. Engine auto-routes the missile.

```json
{"action": "launch", "unit_id": 3, "target": [12, 10, 0]}
```

Errors: `no_weapon`, `no_ammo`, `not_enough_tu`, `wrong_action_type` (weapon is not a launcher)

### throw
Throw held item at target tile.

```json
{"action": "throw", "unit_id": 3, "target": [10, 15, 0], "hand": "right"}
```

### kneel
Toggle kneeling. Costs 4 TU.

```json
{"action": "kneel", "unit_id": 3}
```

### turn
Turn a unit to face a direction. Costs 1 TU per 45° step (shortest path). Async — unit animates, then `action_complete`. Reveals fog of war in the new direction.

`direction`: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW

```json
{"action": "turn", "unit_id": 3, "direction": 4}
```

Errors: `invalid_direction`, `not_enough_tu`

### prime
Prime a grenade. `fuse`: 0 = contact, 1-4 = timer turns.

```json
{"action": "prime", "unit_id": 3, "fuse": 0}
```

### get_reachable
Returns all tiles the unit can reach with current TU, with TU cost per tile.

```json
{"action": "get_reachable", "unit_id": 3}
```

Response:
```json
{
  "type": "action_complete",
  "action": "get_reachable",
  "unit_id": 3,
  "success": true,
  "tiles": [[10,15,0, 8], [11,15,0, 12], ...]  // [x, y, z, tu_cost]
}
```

### get_path_cost
Read-only query. Returns TU cost to reach target tile without moving.

```json
{"action": "get_path_cost", "unit_id": 3, "target": [8, 17, 0]}
```

Success response:
```json
{"type": "action_complete", "action": "get_path_cost", "unit_id": 3, "target": [8, 17, 0], "tu_cost": 24, "success": true}
```

Errors: `no_path`

### get_fire_options
Read-only query. Returns hit chances for all visible enemies across all shot types. Use `from` for hypothetical position planning (e.g. "what can I hit from there?"). Zero chances = no line of fire = full cover.

Current position:
```json
{"action": "get_fire_options", "unit_id": 3}
```

Hypothetical position:
```json
{"action": "get_fire_options", "unit_id": 3, "from": [8, 15, 0]}
```

Response:
```json
{
  "type": "action_complete",
  "action": "get_fire_options",
  "unit_id": 3,
  "from": [8, 15, 0],
  "success": true,
  "targets": [
    {"enemy_id": 20, "pos": [12, 10, 0], "chance_snap": 34, "chance_aimed": 67, "chance_auto": 18},
    {"enemy_id": 21, "pos": [8, 5, 0], "chance_snap": 0, "chance_aimed": 0, "chance_auto": 0}
  ]
}
```

### get_blast_check
Read-only query. Returns friendly units within blast radius of a target tile. Pure geometry, no unit_id required.

```json
{"action": "get_blast_check", "target": [12, 10, 0], "radius": 3}
```

Response:
```json
{
  "type": "action_complete",
  "action": "get_blast_check",
  "success": true,
  "target": [12, 10, 0],
  "radius": 3,
  "friendlies_at_risk": [
    {"unit_id": 5, "pos": [13, 11, 0], "distance": 1},
    {"unit_id": 8, "pos": [10, 10, 0], "distance": 2}
  ]
}
```

### end_turn
End the player's turn.

```json
{"action": "end_turn"}
```

---

## Responses

### action_complete (success)
Returned after walk, shoot, kneel, throw, prime complete.

```json
{
  "type": "action_complete",
  "action": "walk",
  "unit_id": 3,
  "success": true,
  "pos": [10, 15, 0],
  "tu": 38,
  "energy": 72,
  "hp": 30,
  "direction": 4,
  "morale": 95,
  "visible_enemies": [{"id": 20, "pos": [12, 10, 0]}],
  "events": [...]
}
```

After `shoot` action, also includes ammo status:
```json
"ammo_right": 17,
"ammo_left": 0
```
`ammo_right`/`ammo_left` only present when that hand holds a firearm. Value 0 means empty magazine.

### action_error
Returned when a command fails validation.

```json
{
  "type": "action_error",
  "action": "walk",
  "unit_id": 3,
  "error": "no_path",
  "events": [...]
}
```

Possible errors: `no_path`, `not_enough_tu`, `no_weapon`, `no_ammo`, `unit_not_found`, `missing_unit_id`, `missing_target`, `unknown_action`, `busy`, `wrong_action_type`, `invalid_position`

---

## Events
Accumulated in a queue, flushed with every command response (in `events` array). NOT flushed with push notifications (`turn_start`).

### shot_result
```json
{"type": "shot_result", "shooter": 3, "weapon": "STR_RIFLE", "hit": true, "target": 20, "target_faction": "hostile"}
```

### unit_wounded
```json
{"type": "unit_wounded", "unit_id": 20, "faction": "hostile", "damage": 15, "hp": 22, "shooter": 3, "fatal_wounds": {"head":0,"torso":1,"right_arm":0,"left_arm":0,"right_leg":0,"left_leg":0}}
```
`fatal_wounds` only present if new wounds were inflicted.

### unit_killed
```json
{"type": "unit_killed", "unit_id": 20, "faction": "hostile", "murderer": 3, "murderer_faction": "player", "weapon": "STR_RIFLE"}
```

### unit_stunned
```json
{"type": "unit_stunned", "unit_id": 20, "faction": "hostile", "murderer": 3}
```

### reaction_fire
```json
{"type": "reaction_fire", "shooter": 20, "shooter_faction": "hostile", "target": 3, "weapon": "STR_PLASMA_RIFLE"}
```

### unit_spotted
```json
{"type": "unit_spotted", "spotter": 3, "spotted_unit": 20, "position": [12, 10, 0], "spotted_faction": "hostile"}
```

### unit_panicking
Unit entered panic state — drops weapons, may flee randomly. Uncontrollable until panic ends.
```json
{"type": "unit_panicking", "unit_id": 3, "faction": "player", "pos": [14, 30, 1]}
```

### unit_berserk
Unit went berserk — will fire wildly at random targets. Uncontrollable.
```json
{"type": "unit_berserk", "unit_id": 3, "faction": "player", "pos": [14, 30, 1]}
```

### mind_control
Unit's faction changed due to psi mind control.
```json
{"type": "mind_control", "controller_id": 20, "controller_faction": "hostile", "target_id": 3, "new_faction": "hostile", "pos": [14, 30, 1]}
```

### melee_attack
Melee attack hit/miss result (analogous to `shot_result` for ranged).
```json
{"type": "melee_attack", "attacker": 20, "attacker_faction": "hostile", "weapon": "STR_CHRYSSALID_MELEE", "hit": true, "target": 3, "target_faction": "player"}
```

### terrain_destroyed
Explosion destroyed terrain. AI should request updated map via `get_state` with `include_map: true`.
```json
{"type": "terrain_destroyed", "center": [12, 10, 0], "radius": 5, "power": 60}
```

### unit_falling
Unit is falling through a destroyed floor to a lower z-level.
```json
{"type": "unit_falling", "unit_id": 3, "faction": "player", "from": [14, 30, 2]}
```

### unit_spawned
A killed unit converted into a new unit (zombie from Chryssalid kill, Celatid spawn, etc.).
```json
{"type": "unit_spawned", "original_unit_id": 3, "original_faction": "player", "new_unit_id": 42, "new_type": "STR_ZOMBIE", "new_faction": "hostile", "pos": [14, 30, 1]}
```

### bleeding
Between-turn HP loss from fatal wounds. Sent at start of each turn for wounded units.
```json
{"type": "bleeding", "unit_id": 3, "faction": "player", "damage": 2, "hp": 18, "fatal_wounds": 2}
```

### door_opened
A door was opened (by walking through or manual click).
```json
{"type": "door_opened", "pos": [14, 30, 1], "unit_id": 3, "ufo_door": false}
```

---

## Player Unit Data (in `units` array)

```json
{
  "id": 3,
  "name": "Bruce White",
  "type": "SOLDIER",
  "pos": [14, 30, 1],
  "direction": 4,
  "tu": 62, "tu_max": 62,
  "hp": 29, "hp_max": 29,
  "energy": 80, "energy_max": 80,
  "morale": 100,
  "stun": 0,
  "kneeling": false,
  "fire": 0,
  "fatal_wounds": {"head":0, "torso":0, "right_arm":0, "left_arm":0, "right_leg":0, "left_leg":0},
  "armor_current": {"front":12, "left":8, "right":8, "rear":5, "under":2},
  "stats": {"tu":62, "stamina":80, "health":29, "bravery":30, "reactions":45, "firing":55, "throwing":60, "strength":30, "melee":30},
  "inventory": [...],
  "visible_enemies": [20, 21]   // IDs only; full data in top-level visible_enemies
}
```

## Inventory Item Data

```json
{
  "id": 42,
  "type": "STR_RIFLE",
  "slot": "STR_RIGHT_HAND",
  "battle_type": "firearm",
  "ammo_type": "STR_RIFLE_CLIP",
  "ammo_qty": 20,
  "power": 30,
  "max_range": 200,
  "accuracy_snap": 60, "accuracy_aimed": 110, "accuracy_auto": 35,
  "tu_snap": 25, "tu_aimed": 50, "tu_auto": 35,
  "tu_throw": 25,
  "two_handed": true
}
```

Battle types: `firearm`, `ammo`, `melee`, `grenade`, `proximity_grenade`, `medikit`, `scanner`, `mind_probe`, `psi_amp`, `flare`, `corpse`, `none`

## Visible Enemy Data

```json
{
  "id": 20,
  "type": "STR_SECTOID_SOLDIER",
  "name": "Sectoid Soldier",
  "pos": [12, 10, 0],
  "direction": 6,
  "kneeling": false,
  "faction": "hostile"
}
```

---

## ASCII Map Format

2x2 character block per tile:
```
[corner][north_wall]
[west_wall][floor]
```

### Corner
`+` always (where discovered)

### Walls
| Char | Meaning |
|------|---------|
| `-`  | North wall (regular) |
| `\|` | West wall (regular) |
| `=`  | North wall (UFO hull, armor 80+) |
| `!`  | West wall (UFO hull, armor 80+) |
| `;`  | North fence/light wall (armor <=20) |
| `:`  | West fence/light wall (armor <=20) |
| `\`  | Door (any direction) |
| ` `  | No wall |

### Floor
| Char | Meaning |
|------|---------|
| `.`  | Generic walkable floor |
| `a-z`| Building type (see `map_legend`) |
| `u`  | UFO interior |
| `s`  | X-COM craft interior |
| `/`  | Stairs (down) |
| `^`  | Gravlift |
| `#`  | Impassable object |
| ` `  | Void / hole / undiscovered |
| `~`  | Smoke |
| `*`  | Fire |
| `1-9`, `A-E` | Player unit (by index) |
| `X`  | Visible enemy |

---

## Map Size (measured)

Typical map at battle start (partial fog): **~4,500 chars = ~1,100 tokens** across all z-levels.
Fully explored map: estimated **~2,000-3,000 tokens**.

---

## Test Client

```bash
python3 test_interactive.py '{"action":"get_state","include_map":true}'
python3 test_interactive.py '{"action":"walk","unit_id":1,"target":[13,18,0]}'
python3 test_interactive.py '{"action":"walk","unit_id":1,"target":[13,18,0],"exact":true}'
python3 test_interactive.py '{"action":"shoot","unit_id":1,"target":[12,10,0],"shot_type":"snap"}'
python3 test_interactive.py '{"action":"launch","unit_id":1,"target":[12,10,0]}'
python3 test_interactive.py '{"action":"get_path_cost","unit_id":1,"target":[8,17,0]}'
python3 test_interactive.py '{"action":"get_fire_options","unit_id":1}'
python3 test_interactive.py '{"action":"get_fire_options","unit_id":1,"from":[8,15,0]}'
python3 test_interactive.py '{"action":"get_blast_check","target":[12,10,0],"radius":3}'
python3 test_interactive.py '{"action":"end_turn"}'
```

Each invocation opens fresh TCP connection, sends one command, prints result, exits.
