#!/usr/bin/env python3
"""
Mock ZMQ ROUTER for testing trading engine's ZMQ client.

Usage:
    python script/mock_zmq_router.py [--port 13380] [--rep-port 13381]

Architecture:
    This script acts as:
    1. ROUTER (port 13380) - sends commands to engine's DEALER socket
    2. REQ client (port 13381) - sends requests to engine's REP socket

Message formats:
    ROUTER -> DEALER: {"req_id": "...", "payload": {"action": "...", ...}}
    REQ -> REP:       {"action": "add_hot_stock_ht", "symbol": "...", "target_price": ...}
"""

import zmq
import json
import time
import argparse
import threading
import sys
from datetime import datetime

# Default strategy name
DEFAULT_STRATEGY = "BreakoutPriceVolumeStrategy_v2"


class MockZmqRouter:
    def __init__(self, port: int = 13380):
        self.port = port
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.ROUTER)
        self.running = False
        self.clients = {}  # identity -> {"last_seen": timestamp, "info": {...}}
        self.msg_seq = 0
        self.lock = threading.Lock()

    def start(self):
        endpoint = f"tcp://*:{self.port}"
        self.socket.bind(endpoint)
        print(f"[ROUTER] Listening on {endpoint}")
        print(f"[ROUTER] Waiting for DEALER connections...")
        print()

        self.running = True

        # Start receiver thread
        self.recv_thread = threading.Thread(target=self._recv_loop, daemon=True)
        self.recv_thread.start()

    def stop(self):
        self.running = False
        self.socket.close()
        self.context.term()

    def _recv_loop(self):
        """Background thread to receive messages from clients."""
        poller = zmq.Poller()
        poller.register(self.socket, zmq.POLLIN)

        while self.running:
            try:
                socks = dict(poller.poll(100))  # 100ms timeout
                if self.socket in socks:
                    frames = self.socket.recv_multipart()
                    if len(frames) >= 2:
                        identity = frames[0]
                        data = frames[1].decode('utf-8')
                        self._handle_message(identity, data)
            except zmq.ZMQError:
                break

    def _handle_message(self, identity: bytes, data: str):
        """Handle incoming message from a client."""
        identity_str = identity.decode('utf-8', errors='replace')
        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]

        try:
            msg = json.loads(data)
            req_id = msg.get("req_id", "")
            payload = msg.get("payload", {})
            action = payload.get("action", "unknown")

            # Update client info
            with self.lock:
                self.clients[identity_str] = {
                    "last_seen": time.time(),
                    "identity_bytes": identity
                }

            # Format output based on action type
            if action == "heartbeat":
                # Silent for heartbeat, just show a dot
                print(f"\r[{timestamp}] <-- [{identity_str}] ♥ heartbeat seq={payload.get('seq', '?')}")
            elif action == "register":
                print(f"\r[{timestamp}] <-- [{identity_str}] ✓ REGISTERED (client={payload.get('client', '?')})")
            else:
                # Response from client (success/error/etc)
                print(f"\r[{timestamp}] <-- [{identity_str}] {action}")
                if "data" in payload:
                    print(f"    data: {json.dumps(payload['data'], ensure_ascii=False, indent=2)}")
                else:
                    print(f"    payload: {json.dumps(payload, ensure_ascii=False)}")

            print("> ", end="", flush=True)

        except json.JSONDecodeError as e:
            print(f"\r[ERROR] Invalid JSON from {identity_str}: {e}")
            print(f"    raw: {data[:200]}")
            print("> ", end="", flush=True)

    def send_to_client(self, identity_str: str, payload: dict):
        """Send a message to a specific client (format: {req_id, payload})."""
        self.msg_seq += 1
        req_id = f"router_{self.msg_seq}"

        # Message format matching zmq_client.h expectation
        msg = {
            "req_id": req_id,
            "payload": payload
        }

        data = json.dumps(msg, ensure_ascii=False)

        with self.lock:
            client_info = self.clients.get(identity_str)
            if client_info and "identity_bytes" in client_info:
                identity_bytes = client_info["identity_bytes"]
            else:
                identity_bytes = identity_str.encode()

        self.socket.send_multipart([identity_bytes, data.encode()])

        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(f"[{timestamp}] --> [{identity_str}] {payload.get('action', 'unknown')}")
        print(f"    msg: {json.dumps(msg, ensure_ascii=False)}")

    def broadcast(self, payload: dict):
        """Send a message to all connected clients."""
        with self.lock:
            clients_copy = dict(self.clients)

        if not clients_copy:
            print("[WARN] No clients connected. Waiting for client to connect...")
            return

        for identity in clients_copy:
            self.send_to_client(identity, payload)

    def get_clients(self):
        with self.lock:
            return dict(self.clients)


class ReqClient:
    """REQ client for sending requests to engine's REP socket (port 13381)."""

    def __init__(self, endpoint: str = "tcp://localhost:13381"):
        self.endpoint = endpoint
        self.context = zmq.Context()
        self.socket = None

    def connect(self):
        """Connect to the REP server."""
        self.socket = self.context.socket(zmq.REQ)
        self.socket.setsockopt(zmq.RCVTIMEO, 5000)  # 5s timeout
        self.socket.setsockopt(zmq.SNDTIMEO, 5000)
        self.socket.connect(self.endpoint)

    def close(self):
        if self.socket:
            self.socket.close()
            self.socket = None

    def send_request(self, payload: dict) -> dict:
        """Send a request and wait for response."""
        if not self.socket:
            self.connect()

        timestamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]

        try:
            data = json.dumps(payload, ensure_ascii=False)
            print(f"[{timestamp}] REQ --> {self.endpoint}")
            print(f"    {data}")

            self.socket.send_string(data)
            response = self.socket.recv_string()

            print(f"[{timestamp}] REP <--")
            try:
                resp_json = json.loads(response)
                print(f"    {json.dumps(resp_json, ensure_ascii=False, indent=2)}")
                return resp_json
            except json.JSONDecodeError:
                print(f"    {response}")
                return {"raw": response}

        except zmq.Again:
            print(f"[{timestamp}] REQ timeout - no response from {self.endpoint}")
            # Reconnect for next request
            self.close()
            return {"error": "timeout"}
        except zmq.ZMQError as e:
            print(f"[{timestamp}] REQ error: {e}")
            self.close()
            return {"error": str(e)}


def normalize_symbol(symbol: str) -> str:
    """Add exchange suffix if not present (matching symbol_utils::normalize_symbol)."""
    if '.' in symbol:
        return symbol.upper()
    # Auto-detect: 6xx/5xx -> SH, others -> SZ
    if symbol.startswith('6') or symbol.startswith('5'):
        return f"{symbol}.SH"
    else:
        return f"{symbol}.SZ"


def print_help():
    print("""
═══════════════════════════════════════════════════════════════════════════════
                        Mock ZMQ ROUTER - Commands
═══════════════════════════════════════════════════════════════════════════════

  Via ROUTER -> DEALER (port 13380):
  ──────────────────────────────────
    add <symbol> [strategy] [price]   - Add a new strategy instance
    remove <symbol> [strategy]        - Remove a strategy instance
    enable <symbol> [strategy]        - Enable an existing strategy
    disable <symbol> [strategy]       - Disable a strategy
    list                              - List all strategies
    status                            - Query engine status

  Via REQ -> REP (port 13381):
  ────────────────────────────
    ht_add <symbol> <price>           - add_hot_stock_ht (enable BreakoutPriceVolumeStrategy_v2)
    ht_remove <symbol>                - remove_hot_stock_ht (disable BreakoutPriceVolumeStrategy_v2)

  System:
    clients                           - Show connected DEALER clients
    raw <json>                        - Send raw payload via ROUTER
    req <json>                        - Send raw request via REQ

  Other:
    help                              - Show this help
    quit/exit/q                       - Exit

═══════════════════════════════════════════════════════════════════════════════
  Examples:
    # ROUTER commands (async, no response expected)
    add 600000 98.5
    enable 600000
    disable 600000
    list

    # REQ commands (sync, waits for response)
    ht_add 600000 98.50               - Enable strategy with target price
    ht_remove 600000                  - Disable strategy

    raw {"action": "status"}
    req {"action": "add_hot_stock_ht", "symbol": "600000", "target_price": 98.5}
═══════════════════════════════════════════════════════════════════════════════
""")


def parse_add_args(args_str: str):
    """Parse add command arguments: <symbol> [strategy] [price]"""
    parts = args_str.split()
    if not parts:
        return None, None, None

    symbol = normalize_symbol(parts[0])
    strategy = DEFAULT_STRATEGY
    price = 0.0

    if len(parts) >= 2:
        # Check if second arg is a number (price) or strategy name
        try:
            price = float(parts[1])
        except ValueError:
            strategy = parts[1]
            if len(parts) >= 3:
                try:
                    price = float(parts[2])
                except ValueError:
                    pass

    return symbol, strategy, price


def main():
    parser = argparse.ArgumentParser(description="Mock ZMQ ROUTER for testing trading engine")
    parser.add_argument("--port", type=int, default=13380, help="ROUTER port to listen on (default: 13380)")
    parser.add_argument("--rep-port", type=int, default=13381, help="REP port to connect to (default: 13381)")
    parser.add_argument("--rep-host", type=str, default="localhost", help="REP host to connect to (default: localhost)")
    args = parser.parse_args()

    router = MockZmqRouter(port=args.port)
    router.start()

    # REQ client for sending to engine's REP socket
    req_endpoint = f"tcp://{args.rep_host}:{args.rep_port}"
    req_client = ReqClient(endpoint=req_endpoint)
    print(f"[REQ] Will connect to {req_endpoint} for ht_add/ht_remove commands")
    print()

    print_help()

    try:
        while True:
            try:
                cmd = input("> ").strip()
            except EOFError:
                break

            if not cmd:
                continue

            parts = cmd.split(maxsplit=1)
            action = parts[0].lower()
            args_str = parts[1] if len(parts) > 1 else ""

            if action in ("quit", "exit", "q"):
                break

            elif action == "help":
                print_help()

            elif action == "clients":
                clients = router.get_clients()
                if not clients:
                    print("No clients connected")
                else:
                    print(f"Connected clients ({len(clients)}):")
                    for identity, info in clients.items():
                        ago = time.time() - info["last_seen"]
                        print(f"  • {identity} (last seen {ago:.1f}s ago)")

            elif action == "status":
                router.broadcast({"action": "status"})

            elif action == "list":
                router.broadcast({"action": "list_strategies"})

            elif action == "add":
                if not args_str:
                    print("Usage: add <symbol> [strategy] [price]")
                    print("  e.g., add 600000 98.5")
                    continue

                symbol, strategy, price = parse_add_args(args_str)
                payload = {
                    "action": "add_strategy",
                    "symbol": symbol,
                    "strategy": strategy,
                }
                # params can be number, string, or object
                if price > 0:
                    payload["params"] = int(price * 10000)  # Convert to internal price format

                router.broadcast(payload)

            elif action == "remove":
                if not args_str:
                    print("Usage: remove <symbol> [strategy]")
                    continue

                parts = args_str.split()
                symbol = normalize_symbol(parts[0])
                strategy = parts[1] if len(parts) > 1 else DEFAULT_STRATEGY

                payload = {
                    "action": "remove_strategy",
                    "symbol": symbol,
                    "strategy": strategy
                }
                router.broadcast(payload)

            elif action == "enable":
                if not args_str:
                    print("Usage: enable <symbol> [strategy]")
                    continue

                parts = args_str.split()
                symbol = normalize_symbol(parts[0])
                strategy = parts[1] if len(parts) > 1 else DEFAULT_STRATEGY

                payload = {
                    "action": "enable_strategy",
                    "symbol": symbol,
                    "strategy": strategy
                }
                router.broadcast(payload)

            elif action == "disable":
                if not args_str:
                    print("Usage: disable <symbol> [strategy]")
                    continue

                parts = args_str.split()
                symbol = normalize_symbol(parts[0])
                strategy = parts[1] if len(parts) > 1 else DEFAULT_STRATEGY

                payload = {
                    "action": "disable_strategy",
                    "symbol": symbol,
                    "strategy": strategy
                }
                router.broadcast(payload)

            elif action == "raw":
                if not args_str:
                    print("Usage: raw <json>")
                    print('  e.g., raw {"action": "status"}')
                    continue
                try:
                    payload = json.loads(args_str)
                    router.broadcast(payload)
                except json.JSONDecodeError as e:
                    print(f"Invalid JSON: {e}")

            # ============ REQ -> REP commands ============

            elif action == "ht_add":
                if not args_str:
                    print("Usage: ht_add <symbol> <price>")
                    print("  e.g., ht_add 600000 98.50")
                    continue

                parts = args_str.split()
                if len(parts) < 2:
                    print("Usage: ht_add <symbol> <price>")
                    continue

                symbol = normalize_symbol(parts[0])
                try:
                    price = float(parts[1])
                except ValueError:
                    print(f"Invalid price: {parts[1]}")
                    continue

                payload = {
                    "action": "add_hot_stock_ht",
                    "symbol": symbol,
                    "target_price": price,
                    "strategy": DEFAULT_STRATEGY
                }
                req_client.send_request(payload)

            elif action == "ht_remove":
                if not args_str:
                    print("Usage: ht_remove <symbol>")
                    continue

                symbol = normalize_symbol(args_str.split()[0])
                payload = {
                    "action": "remove_hot_stock_ht",
                    "symbol": symbol,
                    "strategy": DEFAULT_STRATEGY
                }
                req_client.send_request(payload)

            elif action == "req":
                if not args_str:
                    print("Usage: req <json>")
                    print('  e.g., req {"action": "add_hot_stock_ht", "symbol": "600000", "target_price": 98.5}')
                    continue
                try:
                    payload = json.loads(args_str)
                    req_client.send_request(payload)
                except json.JSONDecodeError as e:
                    print(f"Invalid JSON: {e}")

            else:
                print(f"Unknown command: {action}. Type 'help' for available commands.")

    except KeyboardInterrupt:
        print("\nShutting down...")

    req_client.close()
    router.stop()
    print("Bye!")


if __name__ == "__main__":
    main()
