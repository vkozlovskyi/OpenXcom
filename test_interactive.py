#!/usr/bin/env python3
"""Interactive AI Bridge client — send command as argv, print result."""
import socket, json, sys, select

SOCK_PATH = '/tmp/xcom_sock'

def connect():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(('127.0.0.1', 12345))
    sock.setblocking(False)
    return sock

def recv_messages(sock, timeout=10.0):
    """Read all complete JSON lines, waiting up to timeout for first message."""
    buf = b''
    msgs = []
    deadline = __import__('time').time() + timeout
    while True:
        remaining = deadline - __import__('time').time()
        if remaining <= 0:
            break
        ready, _, _ = select.select([sock], [], [], min(remaining, 0.1))
        if ready:
            try:
                data = sock.recv(8192)
                if not data:
                    break
                buf += data
            except BlockingIOError:
                pass
        # Parse complete lines
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            msgs.append(json.loads(line))
        # If we got an actionable response, wait a tiny bit more for stragglers then stop
        if msgs and msgs[-1].get('type') in ('action_complete', 'action_error', 'game_state', 'reachable'):
            # Quick drain
            ready2, _, _ = select.select([sock], [], [], 0.2)
            if ready2:
                try:
                    data = sock.recv(8192)
                    if data:
                        buf += data
                except BlockingIOError:
                    pass
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    msgs.append(json.loads(line))
            break
    return msgs, buf

def print_unit(u):
    """Print a unit with status and weapons."""
    kneel = " [kneeling]" if u.get('kneeling') else ""
    morale = f" mor={u.get('morale','?')}"
    energy = f" en={u.get('energy','?')}/{u.get('energy_max','?')}"
    hp_warn = " !!WOUNDED!!" if u['hp'] < u.get('hp_max', u['hp']) else ""
    print(f"  {u['id']:2d} {u['name']:20s} pos={u['pos']} tu={u['tu']}/{u.get('tu_max', '?')} hp={u['hp']}{energy}{morale}{kneel}{hp_warn}")
    for item in u.get('inventory', []):
        if item.get('slot') in ('STR_RIGHT_HAND', 'STR_LEFT_HAND'):
            hand = 'R' if item['slot'] == 'STR_RIGHT_HAND' else 'L'
            ammo = f" ammo={item['ammo_qty']}" if 'ammo_qty' in item else ""
            tu_info = ""
            if 'tu_snap' in item:
                parts = []
                for mode in ('snap', 'aimed', 'auto'):
                    if f'tu_{mode}' in item:
                        parts.append(f"{mode}={item[f'tu_{mode}']}({item.get(f'accuracy_{mode}',0)}%)")
                tu_info = " " + " ".join(parts)
            print(f"      [{hand}] {item['type']}{ammo}{tu_info}")

def print_doors(m):
    doors = m.get('doors', [])
    if not doors:
        return
    print(f"  DOORS ({len(doors)}):")
    for d in doors:
        ufo = " [UFO]" if d.get('ufo_door') else ""
        dtype = f" {d['type']}" if 'type' in d else ""
        print(f"    [{d['pos'][0]:2d},{d['pos'][1]:2d},{d['pos'][2]}] {d['side']}{ufo}{dtype}")

def print_map(m):
    amap = m.get('ascii_map', {})
    if not amap:
        return
    legend = m.get('map_legend', {})
    if legend:
        print(f"  MAP LEGEND: {' '.join(k+'='+v for k,v in legend.items())}")
    print_doors(m)
    for z in sorted(amap.keys(), key=lambda x: int(x)):
        lines = amap[z].split('\n')
        nonblank = [l for l in lines if l.strip()]
        if nonblank:
            print(f"  === Z={z} ===")
            for l in nonblank:
                print(f"  {l}")

def print_enemies(m):
    enemies = m.get('visible_enemies', [])
    if enemies:
        print(f"ENEMIES: {len(enemies)}")
        for e in enemies:
            print(f"  {e['id']} {e.get('name','?')} at {e['pos']}")

def print_events(m):
    for ev in m.get('events', []):
        print(f"  EVENT: {json.dumps(ev)}")

sock = connect()
# Drain hello + turn_start
msgs, buf = recv_messages(sock, timeout=3.0)
for m in msgs:
    t = m.get('type')
    if t == 'turn_start':
        enemies = m.get('visible_enemies', [])
        print(f"[turn {m['turn']}] enemies={len(enemies)}")
    elif t == 'connected':
        print(f"[connected]")
    else:
        print(f"[{t}]")

# Send command from argv
if len(sys.argv) < 2:
    print("Usage: python3 test_interactive.py '<json command>'")
    print("Examples:")
    print("  python3 test_interactive.py '{\"action\":\"walk\",\"unit_id\":1,\"target\":[13,15,0]}'")
    print("  python3 test_interactive.py '{\"action\":\"get_state\"}'")
    print("  python3 test_interactive.py '{\"action\":\"end_turn\"}'")
    sock.close()
    sys.exit(0)

cmd = sys.argv[1]
sock.sendall((cmd + '\n').encode())
msgs, buf = recv_messages(sock, timeout=30.0)

for m in msgs:
    t = m.get('type', '?')
    if t == 'action_complete':
        ok = m.get('success', '?')
        err = m.get('error', '')
        action = m.get('action', '?')
        # Read-only queries: print full JSON (minus large fields)
        if action in ('get_path_cost', 'get_fire_options', 'get_blast_check', 'get_reachable'):
            display = {k: v for k, v in m.items() if k not in ('ascii_map', 'map_legend')}
            if action == 'get_reachable' and 'tiles' in display:
                n = len(display['tiles'])
                display['tiles'] = f"[{n} tiles]"
            print(json.dumps(display, indent=2))
        else:
            ammo = ""
            if 'ammo_right' in m:
                ammo += f" ammo_r={m['ammo_right']}"
            if 'ammo_left' in m:
                ammo += f" ammo_l={m['ammo_left']}"
            dir_info = f" dir={m['direction']}" if 'direction' in m else ""
            morale = f" mor={m['morale']}" if 'morale' in m else ""
            print(f"{'OK' if ok else 'FAIL'} pos={m.get('pos')} tu={m.get('tu')} energy={m.get('energy')} hp={m.get('hp')}{dir_info}{morale}{ammo}{' err='+err if err else ''}")
        enemies = m.get('visible_enemies', [])
        if enemies:
            print(f"ENEMIES SPOTTED: {len(enemies)}")
            for e in enemies:
                print(f"  id={e['id']} pos={e['pos']}")
        print_events(m)
        print_map(m)
    elif t == 'action_error':
        print(f"ERROR: {m.get('error')} (action={m.get('action')})")
        print_events(m)
    elif t == 'game_state':
        for u in m.get('units', []):
            print_unit(u)
        print_enemies(m)
        print_events(m)
        print_doors(m)
        print_map(m)
    elif t == 'turn_start':
        print(f"[turn {m['turn']} started]")
        print_events(m)
        print_enemies(m)
        for u in m.get('units', []):
            print_unit(u)
        print_map(m)
    else:
        print(json.dumps(m)[:300])

sock.close()
