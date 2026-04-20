---
name: XCOM Tactical Rules
description: Tactical guidelines for AI player in battlescape — movement, combat, turn structure
---
# XCOM Tactical Rules for AI Player

## Core Principles
1. **Study the map** — look at the ASCII map to understand the terrain layout before planning moves
2. **Never spend all TU** — reserve enough for at least a snap shot (25% TU) or retreat
3. **Move scouts first**, shooters second — discover enemies before committing
4. **Use cover** — end turns behind walls, inside buildings, not in open fields
5. **Concentrate fire** — focus multiple units on one enemy rather than spreading

## Movement Rules
- **Use `get_path_cost`** before walking to know exact TU expense and check reachability
- **Compare `pos` in action_complete with target** to detect partial walks (blocked mid-path)
- **Spread out** — don't cluster units (grenades, explosions)
- **Use `get_reachable`** to find good positions when planning moves

## Combat Rules
- **CRITICAL: If you spot an enemy — IMMEDIATELY check `get_fire_options`**, before any further movement
- **Compare TU cost vs available TU** — check weapon's `tu_snap`/`tu_aimed`/`tu_auto` from inventory against unit's current TU
- **Shoot first, move second** — never waste TU walking toward an enemy you could shoot from current position
- **Use `get_fire_options` with `from`** to evaluate positions before moving there
- **Prefer aimed shots** (high accuracy) when TU allows, snap shots when conserving TU
- **Auto shots** only at close range or desperate situations
- **Use `get_blast_check`** before explosives to avoid friendly fire
- **Kneel for accuracy bonus** when holding position (+15% accuracy)

## Turn Structure
1. **Start of turn**: `get_state` — full situational awareness
2. **Scout phase**: move 1-2 forward units cautiously, check for enemies
3. **Fire phase**: shoot spotted enemies with units that have LoS
4. **Position phase**: move remaining units to good cover positions
5. **End turn**: verify no unit is exposed in the open

## Map Reading
- `#` = impassable — potential cover nearby
- `<` = stairs up, `>` = stairs down, `^` = gravlift — vertical movement
- `~` = smoke — concealment
- `*` = fire — avoid
- `u` = UFO interior — enemies likely inside
- Building letters (a-z) = potential cover and concealment
- Unit positions are in JSON data (`soldiers[].position`, `visible_enemies[].position`), not on the map

## Threat Assessment
- Enemies at close range (distance < 10) = immediate threat
- Enemies with plasma weapons = high damage, prioritize killing
- Multiple enemies visible = consider retreating to cover
- No enemies visible but shots received = enemies behind cover/in buildings

## Common Mistakes to Avoid
- Walking the whole squad into the open
- Ignoring vertical levels (enemies above/below)
- Not reserving TU for reaction fire
- Clustering near the craft exit
- Moving all units before shooting visible enemies
