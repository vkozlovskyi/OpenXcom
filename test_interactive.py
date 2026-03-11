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

def print_map(m):
    amap = m.get('ascii_map', {})
    if not amap:
        return
    legend = m.get('map_legend', {})
    if legend:
        print(f"  MAP LEGEND: {' '.join(k+'='+v for k,v in legend.items())}")
    for z in sorted(amap.keys(), key=lambda x: int(x)):
        lines = amap[z].split('\n')
        nonblank = [l for l in lines if l.strip()]
        if nonblank:
            print(f"  === Z={z} ===")
            for l in nonblank:
                print(f"  {l}")

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
            print(f"{'OK' if ok else 'FAIL'} pos={m.get('pos')} tu={m.get('tu')} energy={m.get('energy')} hp={m.get('hp')}{' err='+err if err else ''}")
        enemies = m.get('visible_enemies', [])
        if enemies:
            print(f"ENEMIES SPOTTED: {len(enemies)}")
            for e in enemies:
                print(f"  id={e['id']} pos={e['pos']}")
        for ev in m.get('events', []):
            print(f"  EVENT: {json.dumps(ev)}")
        print_map(m)
    elif t == 'action_error':
        print(f"ERROR: {m.get('error')} (action={m.get('action')})")
    elif t == 'game_state':
        for u in m.get('units', []):
            print(f"  {u['id']:2d} {u['name']:20s} pos={u['pos']} tu={u['tu']}/{u['tu_max']} hp={u['hp']}")
        enemies = m.get('visible_enemies', [])
        if enemies:
            print(f"ENEMIES: {len(enemies)}")
            for e in enemies:
                print(f"  {e['id']} {e.get('name','?')} at {e['pos']}")
        for ev in m.get('events', []):
            print(f"  EVENT: {json.dumps(ev)}")
        print_map(m)
    elif t == 'turn_start':
        print(f"[turn {m['turn']} started]")
        for ev in m.get('events', []):
            print(f"  EVENT: {json.dumps(ev)}")
        enemies = m.get('visible_enemies', [])
        if enemies:
            print(f"ENEMIES: {len(enemies)}")
            for e in enemies:
                print(f"  {e['id']} {e.get('name','?')} at {e['pos']}")
        for u in m.get('units', []):
            hp_warn = " !!WOUNDED!!" if u['hp'] < u['hp_max'] else ""
            print(f"  {u['id']:2d} {u['name']:20s} pos={u['pos']} tu={u['tu']}{hp_warn}")
        print_map(m)
    else:
        print(json.dumps(m)[:300])

sock.close()
