#!/usr/bin/env python3
"""
Mock ZMQ ROUTER for testing trading engine's ZMQ client.

Usage:
    python script/mock_zmq_router.py [--port1 13380] [--port2 13381]

Architecture:
    This script acts as two ROUTER sockets:
    1. ROUTER (port 13380) - sends commands to engine's DEALER socket
    2. ROUTER (port 13381) - sends commands to engine's DEALER socket

Message formats:
    ROUTER -> DEALER: {"req_id": "...", "payload": {"action": "...", ...}}

Supported actions:
    - add_hot_stock_ht: Add hot stock with target price
    - remove_hot_stock_ht: Remove hot stock
"""

import zmq
import json
import time
import argparse
import threading
from datetime import datetime

# Default strategy name
DEFAULT_STRATEGY = "BreakoutPriceVolumeStrategy"


class MockZmqRouter:
    def __init__(self, port: int, name: str = "ROUTER"):
        self.port = port
        self.name = name
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.ROUTER)
        self.running = False
        self.clients = {}  # identity -> {"last_seen": timestamp, "info": {...}}
        self.msg_seq = 0
        self.lock = threading.Lock()

    def start(self):
        endpoint = f"tcp://*:{self.port}"
        self.socket.bind(endpoint)
        print(f"[{self.name}] Listening on {endpoint}")

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
                print(f"\r[{timestamp}] [{self.name}:{self.port}] <-- [{identity_str}] heartbeat seq={payload.get('seq', '?')}")
            elif action == "register":
                print(f"\r[{timestamp}] [{self.name}:{self.port}] <-- [{identity_str}] REGISTERED (client={payload.get('client', '?')})")
            else:
                # Response from client (success/error/etc)
                print(f"\r[{timestamp}] [{self.name}:{self.port}] <-- [{identity_str}] {action}")
                if "data" in payload:
                    print(f"    data: {json.dumps(payload['data'], ensure_ascii=False, indent=2)}")
                else:
                    print(f"    payload: {json.dumps(payload, ensure_ascii=False)}")

            print("> ", end="", flush=True)

        except json.JSONDecodeError as e:
            print(f"\r[ERROR] [{self.name}:{self.port}] Invalid JSON from {identity_str}: {e}")
            print(f"    raw: {data[:200]}")
            print("> ", end="", flush=True)

    def send_to_client(self, identity_str: str, payload: dict):
        """Send a message to a specific client (format: {req_id, payload})."""
        self.msg_seq += 1
        req_id = f"router_{self.port}_{self.msg_seq}"

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
        print(f"[{timestamp}] [{self.name}:{self.port}] --> [{identity_str}] {payload.get('action', 'unknown')}")
        print(f"    msg: {json.dumps(msg, ensure_ascii=False)}")

    def broadcast(self, payload: dict):
        """Send a message to all connected clients."""
        with self.lock:
            clients_copy = dict(self.clients)

        if not clients_copy:
            print(f"[WARN] [{self.name}:{self.port}] No clients connected.")
            return

        for identity in clients_copy:
            self.send_to_client(identity, payload)

    def get_clients(self):
        with self.lock:
            return dict(self.clients)


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
===========================================================================
                    Mock ZMQ ROUTER - Commands
===========================================================================

  Commands (sent via ROUTER/DEALER to both ports):
  ------------------------------------------------
    add <symbol> <price>       - add_hot_stock_ht (add hot stock)
    remove <symbol>            - remove_hot_stock_ht (remove hot stock)

  Target port selection:
    1 <cmd>                    - Send to port 13380 only
    2 <cmd>                    - Send to port 13381 only
    <cmd>                      - Send to both ports (default)

  System:
    clients                    - Show connected DEALER clients
    raw <json>                 - Send raw payload to both routers

  Other:
    help                       - Show this help
    quit/exit/q                - Exit

===========================================================================
  Examples:
    add 600000 98.50           - Send add_hot_stock_ht to both ports
    remove 600000              - Send remove_hot_stock_ht to both ports
    1 add 600000 98.50         - Send add_hot_stock_ht to port 13380 only
    2 remove 600000            - Send remove_hot_stock_ht to port 13381 only
    raw {"action": "add_hot_stock_ht", "symbol": "600000.SH", "target_price": 98.5}
===========================================================================
""")


def main():
    parser = argparse.ArgumentParser(description="Mock ZMQ ROUTER for testing trading engine")
    parser.add_argument("--port1", type=int, default=13380, help="First ROUTER port (default: 13380)")
    parser.add_argument("--port2", type=int, default=13381, help="Second ROUTER port (default: 13381)")
    args = parser.parse_args()

    # Create two ROUTER sockets
    router1 = MockZmqRouter(port=args.port1, name="ROUTER1")
    router2 = MockZmqRouter(port=args.port2, name="ROUTER2")

    router1.start()
    router2.start()

    print(f"[INFO] Both ROUTER sockets started (ports {args.port1}, {args.port2})")
    print(f"[INFO] Waiting for DEALER connections...")
    print()

    print_help()

    def broadcast_to_routers(payload: dict, target: str = "both"):
        """Send to specified router(s)."""
        if target == "1":
            router1.broadcast(payload)
        elif target == "2":
            router2.broadcast(payload)
        else:  # both
            router1.broadcast(payload)
            router2.broadcast(payload)

    try:
        while True:
            try:
                cmd = input("> ").strip()
            except EOFError:
                break

            if not cmd:
                continue

            # Check for target prefix (1 or 2)
            target = "both"
            if cmd.startswith("1 "):
                target = "1"
                cmd = cmd[2:].strip()
            elif cmd.startswith("2 "):
                target = "2"
                cmd = cmd[2:].strip()

            parts = cmd.split(maxsplit=1)
            action = parts[0].lower()
            args_str = parts[1] if len(parts) > 1 else ""

            if action in ("quit", "exit", "q"):
                break

            elif action == "help":
                print_help()

            elif action == "clients":
                print(f"\n[ROUTER1:{router1.port}] clients:")
                clients1 = router1.get_clients()
                if not clients1:
                    print("  No clients connected")
                else:
                    for identity, info in clients1.items():
                        ago = time.time() - info["last_seen"]
                        print(f"  - {identity} (last seen {ago:.1f}s ago)")

                print(f"\n[ROUTER2:{router2.port}] clients:")
                clients2 = router2.get_clients()
                if not clients2:
                    print("  No clients connected")
                else:
                    for identity, info in clients2.items():
                        ago = time.time() - info["last_seen"]
                        print(f"  - {identity} (last seen {ago:.1f}s ago)")
                print()

            elif action == "add":
                if not args_str:
                    print("Usage: add <symbol> <price>")
                    print("  e.g., add 600000 98.50")
                    continue

                parts = args_str.split()
                if len(parts) < 2:
                    print("Usage: add <symbol> <price>")
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
                broadcast_to_routers(payload, target)

            elif action == "remove":
                if not args_str:
                    print("Usage: remove <symbol>")
                    continue

                symbol = normalize_symbol(args_str.split()[0])
                payload = {
                    "action": "remove_hot_stock_ht",
                    "symbol": symbol,
                    "strategy": DEFAULT_STRATEGY
                }
                broadcast_to_routers(payload, target)

            elif action == "raw":
                if not args_str:
                    print("Usage: raw <json>")
                    print('  e.g., raw {"action": "add_hot_stock_ht", "symbol": "600000.SH", "target_price": 98.5}')
                    continue
                try:
                    payload = json.loads(args_str)
                    broadcast_to_routers(payload, target)
                except json.JSONDecodeError as e:
                    print(f"Invalid JSON: {e}")

            else:
                print(f"Unknown command: {action}. Type 'help' for available commands.")

    except KeyboardInterrupt:
        print("\nShutting down...")

    router1.stop()
    router2.stop()
    print("Bye!")


if __name__ == "__main__":
    main()
