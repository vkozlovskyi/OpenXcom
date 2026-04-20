#!/usr/bin/env python3
"""Persistent TCP-to-Unix-socket proxy for OpenXcom AI Bridge.

Maintains a single TCP connection to AIBridge and exposes a Unix socket
for lightweight per-command CLI access. Buffers push messages (turn_start,
mission_end) so CLI clients only receive command responses.

Usage:
    python3 xcom_proxy.py [--verbose]
"""

import socket, select, json, logging, os, sys, signal, time, errno

TCP_HOST = "127.0.0.1"
TCP_PORT = 12345
UNIX_SOCK = "/tmp/xcom_proxy.sock"
LOG_FILE = "/tmp/xcom_proxy.log"
TCP_RECONNECT_INTERVAL = 2.0
COMMAND_TIMEOUT = 60.0

# Response types that indicate a command is complete
TERMINAL_TYPES = frozenset(["action_complete", "action_error", "error", "game_state", "reachable", "mission_end"])

# Read-only queries that can be batched (no game state changes, no animations)
QUERY_ACTIONS = frozenset(["get_path_cost", "get_fire_options", "get_blast_check", "get_reachable", "get_state", "get_launch_path"])

log = logging.getLogger("xcom_proxy")


class XcomProxy:
    def __init__(self):
        self.tcp_sock = None
        self.tcp_buf = b""
        self.unix_listen = None
        self.unix_client = None
        self.unix_buf = b""
        self.last_turn_start = None
        self.last_mission_end = None
        self.buffered_pushes = []
        self.pending_command = False
        self.last_tcp_connect_attempt = 0.0
        self.running = True
        self._captured_response = None
        # Token stats (~4 chars ≈ 1 token for JSON)
        self._current_turn = 0
        self._turn_cmds = 0
        self._turn_sent_bytes = 0
        self._turn_recv_bytes = 0
        self._total_cmds = 0
        self._total_sent_bytes = 0
        self._total_recv_bytes = 0
        self._turn_history = []  # last N turns

    def start(self):
        self._setup_logging()
        self._setup_signals()
        self._setup_unix_socket()
        self.connect_tcp()
        log.info("Proxy started, Unix socket: %s", UNIX_SOCK)
        self.run()

    def _setup_logging(self):
        fmt = logging.Formatter("%(asctime)s %(levelname)s %(message)s", datefmt="%H:%M:%S")
        fh = logging.FileHandler(LOG_FILE)
        fh.setFormatter(fmt)
        sh = logging.StreamHandler()
        sh.setFormatter(fmt)
        level = logging.DEBUG if "--verbose" in sys.argv else logging.INFO
        log.setLevel(level)
        log.addHandler(fh)
        log.addHandler(sh)

    def _setup_signals(self):
        def handler(sig, frame):
            log.info("Signal %d received, shutting down", sig)
            self.running = False
        signal.signal(signal.SIGINT, handler)
        signal.signal(signal.SIGTERM, handler)

    def _setup_unix_socket(self):
        if os.path.exists(UNIX_SOCK):
            os.unlink(UNIX_SOCK)
        self.unix_listen = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.unix_listen.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.unix_listen.bind(UNIX_SOCK)
        self.unix_listen.listen(2)
        self.unix_listen.setblocking(False)

    def connect_tcp(self):
        """Attempt TCP connection to AIBridge. Returns True on success."""
        self.last_tcp_connect_attempt = time.time()
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(3.0)
            sock.connect((TCP_HOST, TCP_PORT))
            sock.setblocking(False)
            self.tcp_sock = sock
            self.tcp_buf = b""
            log.info("TCP connected to %s:%d", TCP_HOST, TCP_PORT)
            # Drain initial messages (connected + possibly turn_start)
            self._drain_initial()
            return True
        except (OSError, ConnectionRefusedError) as e:
            log.warning("TCP connect failed: %s", e)
            return False

    def _drain_initial(self):
        """Read initial messages after TCP connect (connected, turn_start)."""
        deadline = time.time() + 5.0
        while time.time() < deadline:
            remaining = deadline - time.time()
            try:
                ready, _, _ = select.select([self.tcp_sock], [], [], min(remaining, 0.5))
            except (ValueError, OSError):
                break
            if ready:
                if not self._tcp_recv():
                    break
                self._process_tcp_buf(is_initial=True)
            # Stop once we have turn_start (or if connection phase is done)
            if self.last_turn_start is not None:
                # Wait a tiny bit more for stragglers
                try:
                    ready2, _, _ = select.select([self.tcp_sock], [], [], 0.2)
                except (ValueError, OSError):
                    break
                if ready2:
                    self._tcp_recv()
                    self._process_tcp_buf(is_initial=True)
                break

    def _tcp_recv(self):
        """Read available data from TCP. Returns False if connection lost."""
        try:
            data = self.tcp_sock.recv(65536)
            if not data:
                self._tcp_lost()
                return False
            self.tcp_buf += data
            return True
        except BlockingIOError:
            return True
        except (ConnectionResetError, BrokenPipeError, OSError) as e:
            log.error("TCP recv error: %s", e)
            self._tcp_lost()
            return False

    def _tcp_lost(self):
        """Handle TCP connection loss."""
        log.warning("TCP connection lost")
        if self.tcp_sock:
            try:
                self.tcp_sock.close()
            except OSError:
                pass
        self.tcp_sock = None
        self.tcp_buf = b""
        if self.pending_command:
            self._send_unix({"error": "tcp_disconnected"})
            self.pending_command = False

    def _process_tcp_buf(self, is_initial=False):
        """Parse complete JSON lines from TCP buffer."""
        while b"\n" in self.tcp_buf:
            line, self.tcp_buf = self.tcp_buf.split(b"\n", 1)
            if not line.strip():
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError as e:
                log.error("Bad JSON from TCP: %s", e)
                continue
            self._handle_tcp_message(msg, is_initial)

    def _handle_tcp_message(self, msg, is_initial=False):
        """Route a parsed TCP message."""
        mtype = msg.get("type", "?")
        log.debug("TCP msg: type=%s", mtype)

        if mtype == "connected":
            log.info("AIBridge connected (version %s)", msg.get("version", "?"))
            return

        if mtype == "turn_start":
            self.last_turn_start = msg
            self.last_mission_end = None
            self._new_turn(msg.get("turn", 0))
            if not is_initial:
                self.buffered_pushes.append(msg)
            log.info("Turn %s started (%d units, %d enemies)",
                     msg.get("turn", "?"),
                     len(msg.get("units", [])),
                     len(msg.get("visible_enemies", [])))
            return

        if mtype == "mission_end":
            self.last_mission_end = msg
            self.buffered_pushes.append(msg)
            log.info("Mission ended: %s", msg.get("result", "?"))
            # If a command is pending, this IS the terminal response
            if self.pending_command:
                self._send_unix(msg)
                self.pending_command = False
            return

        # Terminal response types for commands
        if mtype in TERMINAL_TYPES and self.pending_command:
            self._send_unix(msg)
            self.pending_command = False
            return

        # Unexpected message
        log.warning("Unhandled TCP message type: %s", mtype)

    def run(self):
        """Main event loop."""
        while self.running:
            rlist = [self.unix_listen]
            if self.tcp_sock:
                rlist.append(self.tcp_sock)
            if self.unix_client:
                rlist.append(self.unix_client)

            try:
                ready_r, _, _ = select.select(rlist, [], [], 1.0)
            except (ValueError, OSError):
                # Socket closed during select
                continue

            # TCP reconnect if needed
            if not self.tcp_sock:
                now = time.time()
                if now - self.last_tcp_connect_attempt >= TCP_RECONNECT_INTERVAL:
                    self.connect_tcp()

            # Accept new Unix client
            if self.unix_listen in ready_r:
                self._accept_unix()

            # Read from TCP
            if self.tcp_sock and self.tcp_sock in ready_r:
                if self._tcp_recv():
                    self._process_tcp_buf()

            # Read from Unix client
            if self.unix_client and self.unix_client in ready_r:
                self._read_unix()

        self.cleanup()

    def _accept_unix(self):
        """Accept a new Unix socket client."""
        try:
            client, _ = self.unix_listen.accept()
            client.setblocking(True)
        except OSError:
            return

        if self.pending_command:
            # Reject — we're busy waiting for a response
            try:
                client.sendall(json.dumps({"error": "busy"}).encode() + b"\n")
            except OSError:
                pass
            client.close()
            log.debug("Rejected Unix client (busy)")
            return

        # Replace any existing idle client
        if self.unix_client:
            try:
                self.unix_client.close()
            except OSError:
                pass
        self.unix_client = client
        self.unix_buf = b""
        log.debug("Unix client accepted")

    def _read_unix(self):
        """Read command from Unix client."""
        try:
            data = self.unix_client.recv(8192)
            if not data:
                self._close_unix()
                return
            self.unix_buf += data
        except BlockingIOError:
            return
        except OSError:
            self._close_unix()
            return

        if b"\n" not in self.unix_buf:
            return

        line, self.unix_buf = self.unix_buf.split(b"\n", 1)
        if not line.strip():
            return

        try:
            cmd = json.loads(line)
        except json.JSONDecodeError as e:
            self._send_unix({"error": f"bad_json: {e}"})
            return

        # Batch query — JSON array of read-only commands
        if isinstance(cmd, list):
            self._execute_batch(cmd)
            return

        # Meta-commands
        meta = cmd.get("meta")
        if meta:
            self._handle_meta(meta)
            return

        # Game command — forward to TCP
        if not self.tcp_sock:
            self._send_unix({"error": "tcp_disconnected"})
            return

        action = cmd.get("action", "?")
        log.info("Forwarding command: %s", action)
        try:
            self.tcp_sock.sendall(line + b"\n")
        except (BrokenPipeError, ConnectionResetError, OSError) as e:
            log.error("TCP send failed: %s", e)
            self._tcp_lost()
            return

        self._track_sent(len(line))
        self.pending_command = True
        self._wait_for_response()

    def _wait_for_response(self):
        """Block until we get a terminal response from TCP."""
        deadline = time.time() + COMMAND_TIMEOUT
        while self.pending_command and self.running:
            remaining = deadline - time.time()
            if remaining <= 0:
                log.error("Command timeout after %.0fs", COMMAND_TIMEOUT)
                self._send_unix({"error": "timeout"})
                self.pending_command = False
                return

            rlist = [self.tcp_sock] if self.tcp_sock else []
            if not rlist:
                return  # TCP lost, error already sent by _tcp_lost

            try:
                ready, _, _ = select.select(rlist, [], [], min(remaining, 0.5))
            except (ValueError, OSError):
                return

            if ready:
                if self._tcp_recv():
                    self._process_tcp_buf()

    def _execute_batch(self, commands):
        """Execute a batch of read-only queries and return array of responses."""
        if not isinstance(commands, list) or len(commands) == 0:
            self._send_unix({"error": "batch_empty"})
            return

        # Validate: all commands must be read-only queries
        for i, cmd in enumerate(commands):
            if not isinstance(cmd, dict):
                self._send_unix({"error": f"batch[{i}]: not an object"})
                return
            action = cmd.get("action", "")
            if action not in QUERY_ACTIONS:
                self._send_unix({"error": f"batch[{i}]: '{action}' is not a read-only query. "
                                 f"Allowed: {', '.join(sorted(QUERY_ACTIONS))}"})
                return

        if not self.tcp_sock:
            self._send_unix({"error": "tcp_disconnected"})
            return

        log.info("Batch query: %d commands", len(commands))
        results = []
        for i, cmd in enumerate(commands):
            resp = self._forward_one(cmd)
            if resp is None:
                # TCP lost mid-batch — fill remaining with errors
                results.append({"error": "tcp_disconnected"})
                for _ in range(i + 1, len(commands)):
                    results.append({"error": "tcp_disconnected"})
                break
            # Track each batch response individually
            resp_bytes = len(json.dumps(resp, separators=(",", ":")).encode())
            self._track_recv(resp_bytes)
            results.append(resp)

        # Send combined result (don't double-count in _send_unix)
        self._send_unix_raw(results)

    def _forward_one(self, cmd):
        """Forward a single command to TCP, wait for response. Returns response dict or None on TCP loss."""
        line = json.dumps(cmd, separators=(",", ":")).encode()
        self._track_sent(len(line))
        try:
            self.tcp_sock.sendall(line + b"\n")
        except (BrokenPipeError, ConnectionResetError, OSError) as e:
            log.error("TCP send failed in batch: %s", e)
            self._tcp_lost()
            return None

        # Wait for terminal response
        deadline = time.time() + COMMAND_TIMEOUT
        self.pending_command = True
        captured = None

        while self.pending_command and self.running:
            remaining = deadline - time.time()
            if remaining <= 0:
                log.error("Batch command timeout: %s", cmd.get("action"))
                self.pending_command = False
                return {"error": "timeout"}

            rlist = [self.tcp_sock] if self.tcp_sock else []
            if not rlist:
                return None

            try:
                ready, _, _ = select.select(rlist, [], [], min(remaining, 0.5))
            except (ValueError, OSError):
                return None

            if ready:
                if not self._tcp_recv():
                    return None
                # Process buffer but capture response instead of sending to unix
                self._process_tcp_buf_capture()
                if self._captured_response is not None:
                    captured = self._captured_response
                    self._captured_response = None
                    self.pending_command = False

        return captured

    def _process_tcp_buf_capture(self):
        """Like _process_tcp_buf but captures terminal response instead of sending to unix."""
        while b"\n" in self.tcp_buf:
            line, self.tcp_buf = self.tcp_buf.split(b"\n", 1)
            if not line.strip():
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError as e:
                log.error("Bad JSON from TCP: %s", e)
                continue
            mtype = msg.get("type", "?")
            log.debug("TCP msg (batch): type=%s", mtype)

            if mtype == "connected":
                log.info("AIBridge connected (version %s)", msg.get("version", "?"))
                continue
            if mtype == "turn_start":
                self.last_turn_start = msg
                self.last_mission_end = None
                self.buffered_pushes.append(msg)
                continue
            if mtype == "mission_end":
                self.last_mission_end = msg
                self.buffered_pushes.append(msg)
                if self.pending_command:
                    self._captured_response = msg
                    self.pending_command = False
                continue
            if mtype in TERMINAL_TYPES and self.pending_command:
                self._captured_response = msg
                self.pending_command = False
                continue
            log.warning("Unhandled TCP message type (batch): %s", mtype)

    def _handle_meta(self, meta):
        """Handle proxy meta-commands."""
        if meta == "__status__":
            self._send_unix({
                "status": "connected" if self.tcp_sock else "disconnected",
                "has_turn_start": self.last_turn_start is not None,
                "has_mission_end": self.last_mission_end is not None,
                "buffered_pushes": len(self.buffered_pushes),
            })
        elif meta == "__turn_state__":
            if self.last_turn_start:
                self._send_unix(self.last_turn_start)
            else:
                self._send_unix({"error": "no_turn_start"})
        elif meta == "__events__":
            self._send_unix({"events": self.buffered_pushes})
            self.buffered_pushes = []
        elif meta == "__mission_end__":
            if self.last_mission_end:
                self._send_unix(self.last_mission_end)
            else:
                self._send_unix({"error": "no_mission_end"})
        elif meta == "__stats__":
            self._send_unix({
                "current_turn": self._current_turn,
                "turn_cmds": self._turn_cmds,
                "turn_sent_tokens": self._turn_sent_bytes // 4,
                "turn_recv_tokens": self._turn_recv_bytes // 4,
                "total_cmds": self._total_cmds,
                "total_sent_tokens": self._total_sent_bytes // 4,
                "total_recv_tokens": self._total_recv_bytes // 4,
                "turn_history": self._turn_history,
            })
        else:
            self._send_unix({"error": f"unknown_meta: {meta}"})

    def _track_sent(self, nbytes):
        """Track bytes sent to game."""
        self._turn_cmds += 1
        self._turn_sent_bytes += nbytes
        self._total_cmds += 1
        self._total_sent_bytes += nbytes

    def _track_recv(self, nbytes):
        """Track bytes received from game."""
        self._turn_recv_bytes += nbytes
        self._total_recv_bytes += nbytes

    def _new_turn(self, turn_num):
        """Record turn boundary and reset per-turn counters."""
        if self._turn_cmds > 0:
            self._turn_history.append({
                "turn": self._current_turn,
                "cmds": self._turn_cmds,
                "sent_tokens": self._turn_sent_bytes // 4,
                "recv_tokens": self._turn_recv_bytes // 4,
            })
            # Keep last 20 turns
            if len(self._turn_history) > 20:
                self._turn_history = self._turn_history[-20:]
        self._current_turn = turn_num
        self._turn_cmds = 0
        self._turn_sent_bytes = 0
        self._turn_recv_bytes = 0

    def _send_unix_raw(self, msg):
        """Send JSON response to Unix client without tracking recv bytes."""
        if not self.unix_client:
            return
        try:
            data = json.dumps(msg, separators=(",", ":")).encode() + b"\n"
            self.unix_client.sendall(data)
            self.unix_client.shutdown(socket.SHUT_WR)
        except (BrokenPipeError, ConnectionResetError, OSError) as e:
            log.warning("Unix send failed: %s", e)
        self._close_unix()

    def _send_unix(self, msg):
        """Send JSON response to Unix client and close it."""
        if not self.unix_client:
            return
        try:
            data = json.dumps(msg, separators=(",", ":")).encode() + b"\n"
            self._track_recv(len(data))
            self.unix_client.sendall(data)
            self.unix_client.shutdown(socket.SHUT_WR)
        except (BrokenPipeError, ConnectionResetError, OSError) as e:
            log.warning("Unix send failed: %s", e)
        self._close_unix()

    def _close_unix(self):
        """Close Unix client socket."""
        if self.unix_client:
            try:
                self.unix_client.close()
            except OSError:
                pass
            self.unix_client = None
            self.unix_buf = b""

    def cleanup(self):
        """Clean shutdown."""
        log.info("Cleaning up")
        if self.unix_client:
            try:
                self.unix_client.close()
            except OSError:
                pass
        if self.unix_listen:
            try:
                self.unix_listen.close()
            except OSError:
                pass
        if self.tcp_sock:
            try:
                self.tcp_sock.close()
            except OSError:
                pass
        if os.path.exists(UNIX_SOCK):
            os.unlink(UNIX_SOCK)
        log.info("Proxy stopped")


if __name__ == "__main__":
    proxy = XcomProxy()
    proxy.start()
