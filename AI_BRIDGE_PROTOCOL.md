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
  "map": {"size_x": 50, "size_y": 50, "size_z": 3, "global_shade": 5},  // global_shade: 0=brightest, 15=darkest (night). Affects accuracy.
  "units": [...],           // player units (full detail)
  "visible_enemies": [...], // enemies visible to any player unit
  "ascii_map": {"0": "...", "1": "..."},  // ASCII map per z-level
  "map_legend": {"a": "CULTIVAT", "u": "UFO", "s": "craft", "<": "stairs_up", ">": "stairs_down", "^": "gravlift"},
  "doors": [{"pos": [47, 39, 0], "side": "north", "ufo_door": true}, ...],
  "ufo_bounds": {"x_min":10, "y_min":20, "z_min":0, "x_max":15, "y_max":25, "z_max":1},
  "craft_bounds": {"x_min":12, "y_min":28, "z_min":0, "x_max":16, "y_max":36, "z_max":1}
}
```

### mission_end
Sent when the battle concludes (before TCP connection closes). Contains the mission outcome and unit statistics.

```json
{
  "type": "mission_end",
  "result": "victory",
  "turns": 9,
  "soldiers": {"alive": 3, "dead": 0, "stunned": 1},
  "enemies": {"killed": 5, "stunned": 1, "total": 6},
  "civilians": {"alive": 2, "dead": 1}
}
```

- `result`: `"victory"`, `"defeat"`, or `"abort"`
- `civilians` field only present if civilians exist on the map
- Connection closes shortly after this message

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

If the path is blocked mid-walk (e.g. another unit on the route), the unit stops early. The response always includes `pos` — compare with your target to detect partial movement. Use `get_path_cost` beforehand to check reachability.

Errors: `no_path`, `unit_not_found`

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
Read-only query. Returns TU cost and full path to reach target tile without moving.

```json
{"action": "get_path_cost", "unit_id": 3, "target": [8, 17, 0]}
```

Success response:
```json
{
  "type": "action_complete",
  "action": "get_path_cost",
  "unit_id": 3,
  "target": [8, 17, 0],
  "tu_cost": 24,
  "energy_cost": 12,
  "energy_sufficient": true,
  "path": [[9,18,0,4],[9,17,0,8],[8,17,0,12]],
  "success": true
}
```

`path`: array of waypoints `[x, y, z, cumulative_tu]`. Each entry is a tile along the route with the total TU cost to reach it. Use this to pick intermediate destinations (e.g. walk halfway when TU is limited).

`energy_cost`: total stamina cost for the path (each step costs `step_tu / 2` energy; gravlift steps cost 0). If `energy_sufficient` is `false`, the unit will stop mid-walk when energy runs out, even if TU remain.

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
  "visible_enemies": [{"id": 20, "type": "STR_SECTOID_SOLDIER", "name": "Sectoid Soldier", "pos": [12, 10, 0], "direction": 6, "kneeling": false, "faction": "hostile"}],
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
{"type": "unit_spotted", "spotter": 3, "spotted_unit": 20, "unit_type": "STR_SECTOID_SOLDIER", "position": [12, 10, 0], "spotted_faction": "hostile"}
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
  "direction": 4,          // 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW
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
  "accuracy_snap": 60, "accuracy_aimed": 110, "accuracy_auto": 35,  // raw weapon accuracy (not final hit chance — use get_fire_options for that)
  "tu_snap": 25, "tu_aimed": 50, "tu_auto": 35,
  "tu_throw": 25,
  "two_handed": true
}
```

`tu_snap`, `tu_aimed`, `tu_auto`, `tu_melee`, `tu_throw` are **absolute TU costs** already calculated for this specific unit (not percentages). Compare directly with the unit's `tu` to check affordability.

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

1x1 character per tile with coordinate axes. Only the bounding box of discovered tiles is rendered. No walls — cover is checked via `get_fire_options`, passability via `get_path_cost`.

Example:
```
        10 11 12 13 14 15
  22:    a  a  .  a  #  a
  23:    a  #  a  1  a  a
  24:    a  X  a  a  #  a
```

X-axis coordinates in header row, Y-axis coordinates as row labels. Column width adjusts to coordinate digits.

### Tile Characters
| Char | Meaning |
|------|---------|
| `.`  | Generic walkable floor |
| `a-z`| Building type (see `map_legend`) |
| `u`  | UFO interior |
| `s`  | X-COM craft interior |
| `<`  | Stairs up (can ascend to z+1) |
| `>`  | Stairs down (can descend to z-1) |
| `^`  | Gravlift (bidirectional) |
| `#`  | Impassable object |
| ` `  | Void / hole / undiscovered |
| `~`  | Smoke |
| `*`  | Fire |
| `1-9`, `A-E` | Player unit (by index) |
| `X`  | Visible enemy |

## Doors

Included in `turn_start`, `game_state`, and map updates (after explosions/walks). Lists all doors on discovered tiles.

```json
"doors": [
  {"pos": [17, 9, 0], "side": "west", "ufo_door": true, "type": "entry"},
  {"pos": [9, 3, 0], "side": "west", "ufo_door": true, "type": "internal"},
  {"pos": [34, 15, 1], "side": "north", "ufo_door": false}
]
```

- `side`: which wall the door is on (`"north"` or `"west"`)
- `ufo_door`: true for UFO power doors (open vertically), false for regular hinged doors
- `type` (UFO wall doors only): `"entry"` = hull entrance (one side UFO, other side terrain), `"internal"` = between UFO compartments. Use `"entry"` doors to find the UFO entrance — check surrounding tiles on the ASCII map to determine approach direction (`u` = inside, `a`/other = outside).
- Destroyed doors disappear from the list (map refresh after explosions)

---

## Persistent Proxy (recommended)

For efficient multi-command sessions, use the persistent proxy instead of direct TCP connections.

### Architecture

```
┌─────────────┐     Unix socket       ┌──────────────┐      TCP       ┌──────────┐
│  xcom_cmd.py │ ── command JSON ────► │ xcom_proxy.py │ ────────────► │ AIBridge │
│  (CLI)       │ ◄─ response JSON ──── │ (daemon)      │ ◄──────────── │ (game)   │
└─────────────┘  /tmp/xcom_proxy.sock  └──────────────┘ 127.0.0.1:    └──────────┘
                                                            12345
```

The proxy maintains a single persistent TCP connection and buffers push messages (`turn_start`, `mission_end`). CLI clients connect via Unix socket, send one command, receive only the command response (no hello/turn_start drain), and disconnect.

### Setup

```bash
# Start the proxy daemon (once, in background)
python3 xcom_proxy.py &

# Or with verbose logging
python3 xcom_proxy.py --verbose &

# Logs: /tmp/xcom_proxy.log
```

### Commands via proxy

```bash
# Game commands (same JSON as direct TCP)
python3 xcom_cmd.py '{"action":"get_state","include_map":true}'
python3 xcom_cmd.py '{"action":"walk","unit_id":1,"target":[13,18,0]}'
python3 xcom_cmd.py '{"action":"shoot","unit_id":1,"target":[12,10,0],"shot_type":"aimed"}'
python3 xcom_cmd.py '{"action":"get_path_cost","unit_id":1,"target":[8,17,0]}'
python3 xcom_cmd.py '{"action":"get_fire_options","unit_id":1}'
python3 xcom_cmd.py '{"action":"get_blast_check","target":[12,10,0],"radius":3}'
python3 xcom_cmd.py '{"action":"end_turn"}'

# Meta-commands (proxy-specific)
python3 xcom_cmd.py --status        # TCP connection status, buffer state
python3 xcom_cmd.py --turn-state    # Buffered turn_start (call once per turn)
python3 xcom_cmd.py --events        # Buffered push events (drains buffer)
python3 xcom_cmd.py --stats         # Token usage per turn and total
```

### Typical turn workflow

```bash
# 1. Get turn state (buffered, no TCP overhead)
python3 xcom_cmd.py --turn-state

# 2. Query and act (each call is instant — no hello/turn_start drain)
python3 xcom_cmd.py '{"action":"get_fire_options","unit_id":1}'
python3 xcom_cmd.py '{"action":"shoot","unit_id":1,"target":[12,10,0],"shot_type":"aimed"}'
python3 xcom_cmd.py '{"action":"get_path_cost","unit_id":3,"target":[8,17,0]}'
python3 xcom_cmd.py '{"action":"walk","unit_id":3,"target":[8,17,0]}'

# 3. End turn
python3 xcom_cmd.py '{"action":"end_turn"}'

# 4. Wait for next turn, then get new state
python3 xcom_cmd.py --turn-state
```

### Meta-command protocol

The proxy accepts meta-commands via JSON with a `meta` field (sent over Unix socket):

| Meta command | Request | Response |
|---|---|---|
| Status | `{"meta":"__status__"}` | `{"status":"connected","has_turn_start":true,...}` |
| Turn state | `{"meta":"__turn_state__"}` | Last `turn_start` message (full game state) |
| Events | `{"meta":"__events__"}` | `{"events":[...]}` — buffered pushes, clears after read |
| Mission end | `{"meta":"__mission_end__"}` | Last `mission_end` message |
| Stats | `{"meta":"__stats__"}` | Token usage: per-turn and total (cmds, sent/recv tokens) |

### Batch queries

Send a JSON array of **read-only** queries to execute them in a single call. Returns an array of responses (1:1 mapping). Only these actions are allowed in batch: `get_path_cost`, `get_fire_options`, `get_blast_check`, `get_reachable`, `get_state`.

Actions that modify game state (`walk`, `shoot`, `turn`, etc.) are **rejected** — they must be sent individually so the AI can react to events between commands.

```bash
# Batch: check paths for 3 units + fire options for 2 units — one call
python3 xcom_cmd.py '[
  {"action":"get_path_cost","unit_id":1,"target":[8,17,0]},
  {"action":"get_path_cost","unit_id":3,"target":[10,12,0]},
  {"action":"get_path_cost","unit_id":5,"target":[6,14,0]},
  {"action":"get_fire_options","unit_id":1},
  {"action":"get_fire_options","unit_id":3}
]'
```

Response: `[{resp1}, {resp2}, {resp3}, {resp4}, {resp5}]`

### Error responses

| Error | Meaning |
|---|---|
| `{"error":"tcp_disconnected"}` | Proxy lost TCP connection (auto-reconnects) |
| `{"error":"busy"}` | Another command is in progress |
| `{"error":"timeout"}` | Command timed out (60s) |
| `{"error":"no_turn_start"}` | No turn_start buffered yet |
| `{"error":"batch[N]: '...' is not a read-only query..."}` | Non-query action in batch |

---

## Direct TCP Client (debugging)

For direct TCP access without the proxy, use `test_interactive.py`. Each invocation opens a fresh TCP connection, drains hello/turn_start, sends one command, prints result, exits.

```bash
python3 test_interactive.py '{"action":"get_state","include_map":true}'
python3 test_interactive.py '{"action":"walk","unit_id":1,"target":[13,18,0]}'
python3 test_interactive.py '{"action":"end_turn"}'
```
