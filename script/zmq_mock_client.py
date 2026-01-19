#!/usr/bin/env python3
"""
ZMQ Mock Client - 模拟 trading engine 发送消息到 ROUTER

用法:
    python script/zmq_mock_client.py [endpoint]

示例:
    python script/zmq_mock_client.py                      # 默认连接 localhost:13380
    python script/zmq_mock_client.py tcp://localhost:13381  # 连接到 13381
"""

import zmq
import json
import time
import sys
import os
import re


def strip_symbol_suffix(symbol: str) -> str:
    """去掉股票代码的 .SH/.SZ 后缀"""
    return re.sub(r'\.(SH|SZ)$', '', symbol, flags=re.IGNORECASE)


class MockClient:
    def __init__(self, endpoint="tcp://localhost:13380"):
        self.endpoint = endpoint
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.DEALER)
        self.identity = f"mock-client-{os.getpid()}"
        self.socket.setsockopt_string(zmq.IDENTITY, self.identity)
        self.socket.setsockopt(zmq.RCVTIMEO, 3000)  # 3s timeout

    def connect(self):
        self.socket.connect(self.endpoint)
        print(f"[Mock Client] Connected to {self.endpoint} as '{self.identity}'")

    def send(self, req_id: str, payload: dict):
        msg = {
            "req_id": req_id,
            "payload": payload
        }
        data = json.dumps(msg)
        self.socket.send_string(data)
        print(f"[Send] req_id={req_id}, payload={json.dumps(payload, ensure_ascii=False)}")

    def recv(self) -> dict:
        try:
            data = self.socket.recv_string()
            msg = json.loads(data)
            print(f"[Recv] {json.dumps(msg, ensure_ascii=False, indent=2)}")
            return msg
        except zmq.Again:
            print("[Recv] Timeout")
            return None

    def close(self):
        self.socket.close()
        self.context.term()
        print("[Mock Client] Closed")

    # ==========================================
    # 发送注册消息
    # ==========================================
    def send_register(self):
        payload = {
            "action": "register",
            "client": self.identity,
            "timestamp": int(time.time() * 1000)
        }
        self.send("register_0", payload)

    # ==========================================
    # 发送 add_hot_stock_ht_resp
    # ==========================================
    def send_add_hot_stock_ht_resp(self, req_id: str, symbol: str, strategy: str,
                                    target_price: float, success: bool = True, msg: str = ""):
        payload = {
            "action": "add_hot_stock_ht_resp",
            "success": success,
            "symbol": strip_symbol_suffix(symbol),
            "strategy": strategy,
            "target_price": target_price
        }
        if msg:
            payload["msg"] = msg
        self.send(req_id, payload)

    # ==========================================
    # 发送 remove_hot_stock_ht_resp
    # ==========================================
    def send_remove_hot_stock_ht_resp(self, req_id: str, symbol: str, strategy: str,
                                       success: bool = True, msg: str = ""):
        payload = {
            "action": "remove_hot_stock_ht_resp",
            "success": success,
            "symbol": strip_symbol_suffix(symbol),
            "strategy": strategy
        }
        if msg:
            payload["msg"] = msg
        self.send(req_id, payload)

    # ==========================================
    # 发送 place_order
    # ==========================================
    def send_place_order(self, symbol: str, price: float, volume: int,
                         side: str = "buy", order_type: str = "limit"):
        payload = {
            "action": "place_order",
            "symbol": strip_symbol_suffix(symbol),
            "price": price,
            "volume": volume,
            "side": side,
            "order_type": order_type,
            "timestamp": int(time.time() * 1000)
        }
        req_id = f"order_{int(time.time() * 1000)}"
        self.send(req_id, payload)


def interactive_mode(client: MockClient):
    """交互模式"""
    print("\n" + "=" * 50)
    print("交互模式 - 输入命令发送消息")
    print("=" * 50)
    print("命令:")
    print("  1. register          - 发送注册消息")
    print("  2. add <symbol> <price>  - 发送 add_hot_stock_ht_resp")
    print("  3. remove <symbol>   - 发送 remove_hot_stock_ht_resp")
    print("  4. order <symbol> <price> <volume> [buy|sell]  - 发送 place_order")
    print("  5. recv              - 接收消息")
    print("  6. quit              - 退出")
    print("=" * 50 + "\n")

    while True:
        try:
            cmd = input("> ").strip()
            if not cmd:
                continue

            parts = cmd.split()
            action = parts[0].lower()

            if action in ("quit", "q", "exit"):
                break

            elif action in ("1", "register", "reg"):
                client.send_register()

            elif action in ("2", "add"):
                if len(parts) < 3:
                    print("用法: add <symbol> <target_price>")
                    continue
                symbol = parts[1]
                price = float(parts[2])
                strategy = parts[3] if len(parts) > 3 else "BreakoutPriceVolumeStrategy_v2"
                client.send_add_hot_stock_ht_resp(
                    req_id=f"add_{int(time.time())}",
                    symbol=symbol,
                    strategy=strategy,
                    target_price=price
                )

            elif action in ("3", "remove", "rm"):
                if len(parts) < 2:
                    print("用法: remove <symbol>")
                    continue
                symbol = parts[1]
                strategy = parts[2] if len(parts) > 2 else "BreakoutPriceVolumeStrategy_v2"
                client.send_remove_hot_stock_ht_resp(
                    req_id=f"rm_{int(time.time())}",
                    symbol=symbol,
                    strategy=strategy
                )

            elif action in ("4", "order", "buy", "sell"):
                if len(parts) < 4:
                    print("用法: order <symbol> <price> <volume> [buy|sell]")
                    continue
                symbol = parts[1]
                price = float(parts[2])
                volume = int(parts[3])
                side = parts[4] if len(parts) > 4 else "buy"
                if action in ("buy", "sell"):
                    side = action
                client.send_place_order(symbol, price, volume, side)

            elif action in ("5", "recv", "r"):
                client.recv()

            else:
                print(f"未知命令: {action}")

        except KeyboardInterrupt:
            print("\n退出...")
            break
        except Exception as e:
            print(f"错误: {e}")


def demo_mode(client: MockClient):
    """演示模式 - 发送一系列测试消息"""
    print("\n[Demo] 发送测试消息序列...")

    # 1. 注册
    print("\n--- 1. 注册 ---")
    client.send_register()
    time.sleep(0.5)
    client.recv()

    # 2. 发送 add_hot_stock_ht_resp
    print("\n--- 2. add_hot_stock_ht_resp ---")
    client.send_add_hot_stock_ht_resp(
        req_id="test_add_001",
        symbol="600000.SH",
        strategy="BreakoutPriceVolumeStrategy_v2",
        target_price=12.50,
        success=True
    )
    time.sleep(0.5)
    client.recv()

    # 3. 发送 remove_hot_stock_ht_resp
    print("\n--- 3. remove_hot_stock_ht_resp ---")
    client.send_remove_hot_stock_ht_resp(
        req_id="test_rm_001",
        symbol="600000.SH",
        strategy="BreakoutPriceVolumeStrategy_v2",
        success=True
    )
    time.sleep(0.5)
    client.recv()

    # 4. 发送 place_order
    print("\n--- 4. place_order ---")
    client.send_place_order(
        symbol="600000.SH",
        price=12.50,
        volume=100,
        side="buy"
    )
    time.sleep(0.5)
    client.recv()

    print("\n[Demo] 完成")


def main():
    endpoint = sys.argv[1] if len(sys.argv) > 1 else "tcp://localhost:13380"
    mode = sys.argv[2] if len(sys.argv) > 2 else "interactive"

    client = MockClient(endpoint)
    client.connect()

    try:
        if mode == "demo":
            demo_mode(client)
        else:
            interactive_mode(client)
    finally:
        client.close()


if __name__ == "__main__":
    main()
