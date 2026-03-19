#!/usr/bin/env python3
"""CLI tool to send commands to xcom_proxy via Unix socket.

Usage:
    python3 xcom_cmd.py '{"action":"walk","unit_id":1,"target":[13,18,0]}'
    python3 xcom_cmd.py '{"action":"get_state","include_map":true}'
    python3 xcom_cmd.py '{"action":"get_fire_options","unit_id":1}'
    python3 xcom_cmd.py '{"action":"end_turn"}'
    python3 xcom_cmd.py --status
    python3 xcom_cmd.py --turn-state
    python3 xcom_cmd.py --events
"""

import socket, json, sys, os

UNIX_SOCK = "/tmp/xcom_proxy.sock"
RECV_BUF = 1 << 20  # 1MB — large enough for any response


def connect():
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.connect(UNIX_SOCK)
    except (FileNotFoundError, ConnectionRefusedError):
        print("ERROR: Cannot connect to proxy. Is xcom_proxy.py running?", file=sys.stderr)
        sys.exit(1)
    return sock


def send_recv(sock, msg_bytes):
    """Send message and read full response (one JSON line)."""
    sock.sendall(msg_bytes + b"\n")
    buf = b""
    while b"\n" not in buf:
        data = sock.recv(RECV_BUF)
        if not data:
            break
        buf += data
    sock.close()
    if not buf.strip():
        print("ERROR: Empty response from proxy", file=sys.stderr)
        sys.exit(1)
    return json.loads(buf.split(b"\n", 1)[0])


# ── Pretty-printing ─────────────────────────────────────────────

def print_unit(u):
    kneel = " [kneeling]" if u.get("kneeling") else ""
    morale = f" mor={u.get('morale','?')}"
    energy = f" en={u.get('energy','?')}/{u.get('energy_max','?')}"
    hp_warn = " !!WOUNDED!!" if u["hp"] < u.get("hp_max", u["hp"]) else ""
    print(f"  {u['id']:2d} {u['name']:20s} pos={u['pos']} tu={u['tu']}/{u.get('tu_max', '?')} hp={u['hp']}{energy}{morale}{kneel}{hp_warn}")
    for item in u.get("inventory", []):
        if item.get("slot") in ("STR_RIGHT_HAND", "STR_LEFT_HAND"):
            hand = "R" if item["slot"] == "STR_RIGHT_HAND" else "L"
            ammo = f" ammo={item['ammo_qty']}" if "ammo_qty" in item else ""
            tu_info = ""
            if "tu_snap" in item:
                parts = []
                for mode in ("snap", "aimed", "auto"):
                    if f"tu_{mode}" in item:
                        parts.append(f"{mode}={item[f'tu_{mode}']}({item.get(f'accuracy_{mode}',0)}%)")
                tu_info = " " + " ".join(parts)
            print(f"      [{hand}] {item['type']}{ammo}{tu_info}")


def print_doors(m):
    doors = m.get("doors", [])
    if not doors:
        return
    print(f"  DOORS ({len(doors)}):")
    for d in doors:
        ufo = " [UFO]" if d.get("ufo_door") else ""
        dtype = f" {d['type']}" if "type" in d else ""
        print(f"    [{d['pos'][0]:2d},{d['pos'][1]:2d},{d['pos'][2]}] {d['side']}{ufo}{dtype}")


def print_map(m):
    amap = m.get("ascii_map", {})
    if not amap:
        return
    legend = m.get("map_legend", {})
    if legend:
        print(f"  MAP LEGEND: {' '.join(k+'='+v for k,v in legend.items())}")
    print_doors(m)
    for z in sorted(amap.keys(), key=lambda x: int(x)):
        lines = amap[z].split("\n")
        nonblank = [l for l in lines if l.strip()]
        if nonblank:
            print(f"  === Z={z} ===")
            for l in nonblank:
                print(f"  {l}")


def print_enemies(m):
    enemies = m.get("visible_enemies", [])
    if enemies:
        print(f"ENEMIES: {len(enemies)}")
        for e in enemies:
            print(f"  {e['id']} {e.get('name','?')} at {e['pos']}")


def print_events(m):
    for ev in m.get("events", []):
        print(f"  EVENT: {json.dumps(ev)}")


# ── Message display ──────────────────────────────────────────────

def display(m):
    """Display a response message with appropriate formatting."""
    t = m.get("type", "?")

    if "error" in m and "type" not in m:
        # Proxy error
        print(f"PROXY ERROR: {m['error']}")
        return 1

    if t == "action_complete":
        action = m.get("action", "?")
        ok = m.get("success", "?")
        err = m.get("error", "")
        if action in ("get_path_cost", "get_fire_options", "get_blast_check", "get_reachable"):
            display_obj = {k: v for k, v in m.items() if k not in ("ascii_map", "map_legend")}
            if action == "get_reachable" and "tiles" in display_obj:
                n = len(display_obj["tiles"])
                display_obj["tiles"] = f"[{n} tiles]"
            print(json.dumps(display_obj, indent=2))
        else:
            ammo = ""
            if "ammo_right" in m:
                ammo += f" ammo_r={m['ammo_right']}"
            if "ammo_left" in m:
                ammo += f" ammo_l={m['ammo_left']}"
            dir_info = f" dir={m['direction']}" if "direction" in m else ""
            morale = f" mor={m['morale']}" if "morale" in m else ""
            print(f"{'OK' if ok else 'FAIL'} pos={m.get('pos')} tu={m.get('tu')} energy={m.get('energy')} hp={m.get('hp')}{dir_info}{morale}{ammo}{' err='+err if err else ''}")
        enemies = m.get("visible_enemies", [])
        if enemies:
            print(f"ENEMIES SPOTTED: {len(enemies)}")
            for e in enemies:
                print(f"  id={e['id']} pos={e['pos']}")
        print_events(m)
        print_map(m)
        return 0

    if t == "action_error":
        print(f"ERROR: {m.get('error')} (action={m.get('action')})")
        print_events(m)
        return 1

    if t == "game_state":
        for u in m.get("units", []):
            print_unit(u)
        print_enemies(m)
        print_events(m)
        print_doors(m)
        print_map(m)
        return 0

    if t == "turn_start":
        print(f"[turn {m.get('turn', '?')}] {len(m.get('units', []))} units, {len(m.get('visible_enemies', []))} enemies")
        for u in m.get("units", []):
            print_unit(u)
        print_enemies(m)
        print_events(m)
        print_map(m)
        return 0

    if t == "mission_end":
        result = m.get("result", "?").upper()
        turns = m.get("turns", "?")
        sol = m.get("soldiers", {})
        ene = m.get("enemies", {})
        print(f"\n=== MISSION {result} (turn {turns}) ===")
        print(f"  Soldiers: {sol.get('alive',0)} alive, {sol.get('dead',0)} dead, {sol.get('stunned',0)} stunned")
        print(f"  Enemies:  {ene.get('killed',0)} killed, {ene.get('stunned',0)} stunned (of {ene.get('total',0)})")
        civ = m.get("civilians")
        if civ:
            print(f"  Civilians: {civ.get('alive',0)} alive, {civ.get('dead',0)} dead")
        return 0

    # Status response
    if "status" in m:
        print(f"TCP: {m['status']}")
        print(f"  turn_start buffered: {m.get('has_turn_start', False)}")
        print(f"  mission_end buffered: {m.get('has_mission_end', False)}")
        print(f"  buffered pushes: {m.get('buffered_pushes', 0)}")
        return 0

    # Events response
    if "events" in m and "type" not in m:
        events = m["events"]
        if not events:
            print("No buffered events")
        else:
            print(f"{len(events)} buffered event(s):")
            for ev in events:
                t2 = ev.get("type", "?")
                if t2 == "turn_start":
                    print(f"  turn_start (turn {ev.get('turn','?')})")
                elif t2 == "mission_end":
                    print(f"  mission_end ({ev.get('result','?')})")
                else:
                    print(f"  {json.dumps(ev)[:200]}")
        return 0

    # Fallback
    print(json.dumps(m)[:500])
    return 0


# ── Main ─────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 xcom_cmd.py '<json command>'")
        print("       python3 xcom_cmd.py --status")
        print("       python3 xcom_cmd.py --turn-state")
        print("       python3 xcom_cmd.py --events")
        print()
        print("Examples:")
        print("  python3 xcom_cmd.py '{\"action\":\"get_state\",\"include_map\":true}'")
        print("  python3 xcom_cmd.py '{\"action\":\"walk\",\"unit_id\":1,\"target\":[13,18,0]}'")
        print("  python3 xcom_cmd.py '{\"action\":\"shoot\",\"unit_id\":1,\"target\":[12,10,0],\"shot_type\":\"aimed\"}'")
        print("  python3 xcom_cmd.py '{\"action\":\"get_path_cost\",\"unit_id\":1,\"target\":[8,17,0]}'")
        print("  python3 xcom_cmd.py '{\"action\":\"get_fire_options\",\"unit_id\":1}'")
        print("  python3 xcom_cmd.py '{\"action\":\"end_turn\"}'")
        sys.exit(0)

    arg = sys.argv[1]

    # Meta-commands
    if arg == "--status":
        msg = json.dumps({"meta": "__status__"}).encode()
    elif arg == "--turn-state":
        msg = json.dumps({"meta": "__turn_state__"}).encode()
    elif arg == "--events":
        msg = json.dumps({"meta": "__events__"}).encode()
    else:
        # Validate JSON
        try:
            json.loads(arg)
        except json.JSONDecodeError as e:
            print(f"ERROR: Invalid JSON: {e}", file=sys.stderr)
            sys.exit(1)
        msg = arg.encode()

    sock = connect()
    resp = send_recv(sock, msg)
    rc = display(resp)
    sys.exit(rc)


if __name__ == "__main__":
    main()
