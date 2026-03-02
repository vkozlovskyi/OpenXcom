#!/usr/bin/env python3
"""
Test script for OpenXcom AI Bridge (Phases 1-3).
Connects to the game's TCP server, receives game state, sends commands interactively.

Usage:
  1. Launch OpenXcom: ./build/openxcom.app/Contents/MacOS/openxcom -ai-server 12345
  2. Load a battle save
  3. Run this script: python3 test_ai_bridge.py
"""

import socket
import json
import sys
import time
import select

HOST = "127.0.0.1"
PORT = 12345


def recv_messages(sock, timeout=5.0):
    """Receive all available JSON-lines messages within timeout."""
    messages = []
    buf = ""
    deadline = time.time() + timeout

    while time.time() < deadline:
        remaining = deadline - time.time()
        if remaining <= 0:
            break
        ready, _, _ = select.select([sock], [], [], min(remaining, 0.5))
        if ready:
            data = sock.recv(65536)
            if not data:
                print("[!] Server closed connection")
                sys.exit(1)
            buf += data.decode("utf-8")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                if line.strip():
                    msg = json.loads(line)
                    messages.append(msg)
            # If we got messages and no more data waiting, return
            if messages:
                r2, _, _ = select.select([sock], [], [], 0.1)
                if not r2:
                    break
    return messages


def print_state_summary(state):
    """Print a human-readable summary of turn_start state."""
    print(f"\n{'='*60}")
    print(f"  TURN {state['turn']}")
    print(f"  Map: {state['map']['size_x']}x{state['map']['size_y']}x{state['map']['size_z']}")

    # ASCII map info
    ascii_map = state.get("ascii_map", {})
    if ascii_map:
        z_levels = sorted(ascii_map.keys(), key=int)
        print(f"  ASCII map z-levels: {', '.join(z_levels)}")
    print(f"{'='*60}")

    print(f"\n  YOUR UNITS ({len(state['units'])}):")
    for u in state["units"]:
        weapon = "unarmed"
        for item in u.get("inventory", []):
            if item.get("slot") == "STR_RIGHT_HAND":
                weapon = item["type"]
                ammo = item.get("ammo_qty", "?")
                weapon += f" ({ammo})"
                break
        vis = u.get("visible_enemies", [])
        vis_str = f", sees {len(vis)} enemies" if vis else ""
        print(f"    [{u['id']}] {u['name']}  pos={u['pos']}  "
              f"TU={u['tu']}/{u['tu_max']}  HP={u['hp']}/{u['hp_max']}  "
              f"E={u['energy']}  {weapon}"
              f"{'  KNEELING' if u.get('kneeling') else ''}{vis_str}")

    enemies = state.get("visible_enemies", [])
    if enemies:
        print(f"\n  VISIBLE ENEMIES ({len(enemies)}):")
        for e in enemies:
            print(f"    [{e['id']}] {e.get('name', e['type'])}  pos={e['pos']}  "
                  f"faction={e.get('faction', '?')}")
    else:
        print("\n  No visible enemies.")

    # JSON size
    raw = json.dumps(state)
    print(f"\n  Raw JSON size: {len(raw):,} bytes ({len(raw)//1024} KB)")


def print_ascii_map(state, z_level=None):
    """Print the ASCII map for a given z-level (or z=0 by default)."""
    ascii_map = state.get("ascii_map", {})
    if not ascii_map:
        print("  No ASCII map in state.")
        return

    if z_level is None:
        z_level = min(ascii_map.keys(), key=int)

    z_str = str(z_level)
    if z_str not in ascii_map:
        print(f"  No map data for z={z_level}. Available: {', '.join(sorted(ascii_map.keys(), key=int))}")
        return

    print(f"\n  ASCII MAP z={z_level}:")
    print(f"  Legend: . walkable  # impassable  | west wall  - north wall")
    print(f"          \\ door  + corner  1-9/A-E your units  X enemy  ~ smoke  * fire")
    print()
    for line in ascii_map[z_str].split("\n"):
        if line:  # skip empty trailing line
            print(f"  {line}")


def send_command(sock, cmd):
    """Send a JSON command and wait for response."""
    line = json.dumps(cmd) + "\n"
    print(f"\n>>> Sending: {json.dumps(cmd)}")
    sock.sendall(line.encode("utf-8"))
    msgs = recv_messages(sock, timeout=10.0)
    for m in msgs:
        mtype = m.get("type", "?")
        if mtype == "action_complete":
            status = "OK" if m.get("success") else f"FAILED: {m.get('error', '?')}"
            print(f"<<< action_complete [{m.get('action')}] unit={m.get('unit_id')}: {status}")
            # Special handling for get_reachable
            if m.get("action") == "get_reachable" and m.get("success"):
                tiles = m.get("tiles", [])
                print(f"    Reachable tiles: {len(tiles)}")
                if tiles:
                    print(f"    Sample (first 10):")
                    for t in tiles[:10]:
                        print(f"      [{t[0]:2}, {t[1]:2}, {t[2]}] TU={t[3]}")
                    if len(tiles) > 10:
                        print(f"      ... and {len(tiles)-10} more")
        elif mtype == "action_error":
            print(f"<<< action_error [{m.get('action')}]: {m.get('error', '?')}")
        elif mtype == "turn_start":
            print(f"<<< New turn_start received (turn {m.get('turn')})")
            print_state_summary(m)
        else:
            print(f"<<< {json.dumps(m)}")
    return msgs


def interactive_mode(sock, state):
    """Interactive command entry."""
    units = {u["id"]: u for u in state["units"]}

    print("\n" + "="*60)
    print("  INTERACTIVE MODE")
    print("  Commands:")
    print("    select <unit_id>")
    print("    walk <unit_id> <x> <y> <z>")
    print("    shoot <unit_id> <x> <y> <z> [snap|aimed|auto] [right|left]")
    print("    kneel <unit_id>")
    print("    throw <unit_id> <x> <y> <z> [right|left]")
    print("    prime <unit_id> [right|left] [fuse=0]")
    print("    reachable <unit_id>  — get reachable tiles")
    print("    map [z]             — display ASCII map for z-level")
    print("    end_turn")
    print("    state  — re-print state summary")
    print("    raw    — print raw JSON of last state (no map)")
    print("    unit <unit_id> — print unit details")
    print("    quit")
    print("="*60)

    last_state = state

    while True:
        try:
            line = input("\nai> ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nBye!")
            break

        if not line:
            continue

        parts = line.split()
        action = parts[0].lower()

        try:
            if action == "quit":
                break

            elif action == "state":
                print_state_summary(last_state)

            elif action == "raw":
                # Print state without ascii_map to keep it readable
                compact = {k: v for k, v in last_state.items() if k != "ascii_map"}
                print(json.dumps(compact, indent=2)[:5000])
                if len(json.dumps(compact)) > 5000:
                    print("... (truncated)")

            elif action == "map":
                z = int(parts[1]) if len(parts) > 1 else None
                print_ascii_map(last_state, z)

            elif action == "unit":
                uid = int(parts[1])
                if uid in units:
                    print(json.dumps(units[uid], indent=2))
                else:
                    print(f"Unit {uid} not found. Available: {list(units.keys())}")

            elif action == "select":
                uid = int(parts[1])
                msgs = send_command(sock, {"action": "select", "unit_id": uid})

            elif action == "reachable":
                uid = int(parts[1])
                msgs = send_command(sock, {"action": "get_reachable", "unit_id": uid})

            elif action == "walk":
                uid = int(parts[1])
                x, y, z = int(parts[2]), int(parts[3]), int(parts[4])
                msgs = send_command(sock, {"action": "walk", "unit_id": uid, "target": [x, y, z]})
                # Check for turn_start (new state after movement)
                for m in msgs:
                    if m.get("type") == "turn_start":
                        last_state = m
                        units = {u["id"]: u for u in m["units"]}

            elif action == "shoot":
                uid = int(parts[1])
                x, y, z = int(parts[2]), int(parts[3]), int(parts[4])
                shot_type = parts[5] if len(parts) > 5 else "snap"
                hand = parts[6] if len(parts) > 6 else "right"
                msgs = send_command(sock, {
                    "action": "shoot", "unit_id": uid,
                    "target": [x, y, z], "shot_type": shot_type, "hand": hand
                })

            elif action == "kneel":
                uid = int(parts[1])
                msgs = send_command(sock, {"action": "kneel", "unit_id": uid})

            elif action == "throw":
                uid = int(parts[1])
                x, y, z = int(parts[2]), int(parts[3]), int(parts[4])
                hand = parts[5] if len(parts) > 5 else "right"
                msgs = send_command(sock, {
                    "action": "throw", "unit_id": uid,
                    "target": [x, y, z], "hand": hand
                })

            elif action == "prime":
                uid = int(parts[1])
                hand = parts[2] if len(parts) > 2 else "right"
                fuse = int(parts[3]) if len(parts) > 3 else 0
                msgs = send_command(sock, {
                    "action": "prime", "unit_id": uid,
                    "hand": hand, "fuse": fuse
                })

            elif action == "end_turn":
                msgs = send_command(sock, {"action": "end_turn"})
                # After enemy turn, we should get new turn_start
                print("\nWaiting for new turn...")
                msgs2 = recv_messages(sock, timeout=30.0)
                for m in msgs2:
                    if m.get("type") == "turn_start":
                        last_state = m
                        units = {u["id"]: u for u in m["units"]}
                        print_state_summary(m)
                    else:
                        print(f"<<< {json.dumps(m)}")

            else:
                print(f"Unknown command: {action}")

        except (IndexError, ValueError) as e:
            print(f"Error parsing command: {e}")
        except Exception as e:
            print(f"Error: {e}")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else PORT

    print(f"Connecting to OpenXcom AI Bridge at {HOST}:{port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.connect((HOST, port))
    except ConnectionRefusedError:
        print(f"[!] Connection refused. Make sure OpenXcom is running with -ai-server {port}")
        print(f"    and you're in a tactical battle.")
        sys.exit(1)

    print("Connected!")
    sock.setblocking(False)

    # Wait for initial messages (connected + turn_start)
    print("Waiting for game state...")
    msgs = recv_messages(sock, timeout=10.0)

    state = None
    for m in msgs:
        mtype = m.get("type", "?")
        if mtype == "connected":
            print(f"<<< Server hello: version={m.get('version')}")
        elif mtype == "turn_start":
            state = m
            print_state_summary(state)
        elif mtype == "battle_start":
            print(f"<<< battle_start")
        else:
            print(f"<<< {json.dumps(m)}")

    if not state:
        print("[!] Did not receive turn_start. Make sure it's the player's turn.")
        print("    Waiting longer...")
        msgs = recv_messages(sock, timeout=30.0)
        for m in msgs:
            if m.get("type") == "turn_start":
                state = m
                print_state_summary(state)

    if not state:
        print("[!] Still no turn_start. Entering raw mode anyway...")
        state = {"turn": "?", "units": [], "map": {"size_x": 0, "size_y": 0, "size_z": 0}}

    interactive_mode(sock, state)
    sock.close()
    print("Disconnected.")


if __name__ == "__main__":
    main()
