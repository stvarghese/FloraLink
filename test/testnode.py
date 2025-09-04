

import asyncio
import websockets
import json
import sys
import time
import os
import threading
from contextlib import contextmanager

# Protocol constants (should match nodeioprotocol.h)
PROTOCOL_MAGIC = 0xBEEFBEEF
MSG_TYP_CONNECT = "connect"
MSG_TYP_NODE_DATA = "node_data"
MSG_PAYLOAD_TYPE_SENSOR = "sensor"
MSG_PAYLOAD_TYPE_DIAGNOSTICS = "diagnostics"
MSG_PAYLOAD_TYPE_OTA_STATUS = "ota_status"
MSG_TYP_DISCONNECT_REQUEST = "disconnect_request"

SAMPLE_MSG_PATH = os.path.join(os.path.dirname(__file__), "..", "main", "samplenodemsg.json")

def build_base_message(msg_type, node_id, seq_num):
    return {
        "magic": PROTOCOL_MAGIC,
        "type": msg_type,
        "node_id": node_id,
        "sensors": ["temperature", "humidity", "moisture"],
        "services": ["diagnostic", "ota"],
        "seq_num": seq_num,
        "timestamp": int(time.time()),
    }


def build_payloads(sensors):
    # Generate dummy sensor values for each sensor
    sensor_values = {s: 42.0 for s in sensors}
    sensor_payload = {
        "type": MSG_PAYLOAD_TYPE_SENSOR,
        "sensor": sensor_values
    }
    diagnostics_payload = {
        "type": MSG_PAYLOAD_TYPE_DIAGNOSTICS,
        "diagnostics": {
            "uptime_sec": 123456,
            "free_heap": 20480,
            "rssi": -65,
            "error_code": 0,
            "info": "No issues detected"
        }
    }
    ota_status_payload = {
        "type": MSG_PAYLOAD_TYPE_OTA_STATUS,
        "ota_status": {
            "status_code": 1,
            "message": "OTA update successful"
        }
    }
    return [sensor_payload, diagnostics_payload, ota_status_payload]

async def simulate_node(uri, node_id, interval, sample_msg, control_event, log_enabled, disconnect_event=None):
    def log(msg):
        if log_enabled.is_set():
            print(msg)
    websocket = await websockets.connect(uri)
    seq_num = 1
    try:
        log(f"[Node {node_id}] Connected to {uri}")
        # Send connect request (no payload)
        connect_req = build_base_message(MSG_TYP_CONNECT, node_id, 0)
        await websocket.send(json.dumps(connect_req))
        print(f"[Node {node_id}] Sent connect request")
        # Wait for acceptance
        try:
            response = await asyncio.wait_for(websocket.recv(), timeout=5)
            log(f"[Node {node_id}] Received: {response}")
            resp_obj = json.loads(response)
            if resp_obj.get("type") != "connect_response" or resp_obj.get("status") != "accepted":
                log(f"[Node {node_id}] Connection not accepted. Exiting.")
                return
        except asyncio.TimeoutError:
            log(f"[Node {node_id}] No response to connect request (timeout). Exiting.")
            return
        # Send sensor values at interval, but allow pausing/resuming
        while True:
            try:
                if disconnect_event and disconnect_event.is_set():
                    disconnect_msg = build_base_message(MSG_TYP_DISCONNECT_REQUEST, node_id, seq_num)
                    print(f"[Node {node_id}] Sending disconnect request")
                    await websocket.send(json.dumps(disconnect_msg))
                    print(f"[Node {node_id}] Sent disconnect request")
                    await websocket.close()
                    await websocket.wait_closed()
                    await asyncio.sleep(0.1)
                    return
                await control_event.wait()  # Wait until allowed to send
                sensors = sample_msg.get("sensors", sample_msg)
                msg = build_base_message(MSG_TYP_NODE_DATA, node_id, seq_num)
                msg["sensors"] = sensors
                msg["payload"] = build_payloads(sensors)
                await websocket.send(json.dumps(msg))
                log(f"[Node {node_id}] Sent data")
                try:
                    response = await asyncio.wait_for(websocket.recv(), timeout=0.1)
                    log(f"[Node {node_id}] Received (unexpected): {response}")
                except asyncio.TimeoutError:
                    pass  # No response expected
                seq_num += 1
                await asyncio.sleep(interval)
            except asyncio.CancelledError:
                # On cancellation, send disconnect if not already sent
                if disconnect_event and not disconnect_event.is_set():
                    disconnect_msg = build_base_message(MSG_TYP_DISCONNECT_REQUEST, node_id, seq_num)
                    print(f"[Node {node_id}] Sending disconnect request (cancel)")
                    await websocket.send(json.dumps(disconnect_msg))
                    print(f"[Node {node_id}] Sent disconnect request (cancel)")
                await websocket.close()
                await websocket.wait_closed()
                await asyncio.sleep(0.1)
                raise
    except Exception as e:
        if log_enabled.is_set():
            print(f"[Node {node_id}] Exception: {e}")
    finally:
        try:
            await websocket.close()
            await websocket.wait_closed()
        except Exception:
            pass


class NodeManager:
    def __init__(self, uri, interval, sample_msg, log_enabled):
        self.uri = uri
        self.interval = interval
        self.sample_msg = sample_msg
        self.node_tasks = {}  # node_id: (task, control_event, disconnect_event)
        self.lock = asyncio.Lock()
        self.log_enabled = log_enabled

    async def add_node(self, node_id):
        async with self.lock:
            if node_id in self.node_tasks:
                print(f"Node {node_id} already exists.")
                return
            control_event = asyncio.Event()
            control_event.set()  # Start as active
            disconnect_event = asyncio.Event()
            task = asyncio.create_task(
                simulate_node(self.uri, node_id, self.interval, self.sample_msg, control_event, self.log_enabled, disconnect_event)
            )
            self.node_tasks[node_id] = (task, control_event, disconnect_event)
            print(f"Node {node_id} added and started.")

    async def remove_node(self, node_id):
        async with self.lock:
            if node_id not in self.node_tasks:
                print(f"Node {node_id} does not exist.")
                return
            task, control_event, disconnect_event = self.node_tasks.pop(node_id)
            disconnect_event.set()  # Signal node to send disconnect
            await asyncio.sleep(0.2)  # Give time for disconnect to be sent
            task.cancel()
            print(f"Node {node_id} removed.")

    async def pause_node(self, node_id):
        async with self.lock:
            if node_id not in self.node_tasks:
                print(f"Node {node_id} does not exist.")
                return
            _, control_event, _ = self.node_tasks[node_id]
            control_event.clear()
            print(f"Node {node_id} paused.")

    async def resume_node(self, node_id):
        async with self.lock:
            if node_id not in self.node_tasks:
                print(f"Node {node_id} does not exist.")
                return
            _, control_event, _ = self.node_tasks[node_id]
            control_event.set()
            print(f"Node {node_id} resumed.")

    async def list_nodes(self):
        async with self.lock:
            print("Active nodes:")
            for node_id, (task, control_event, _) in self.node_tasks.items():
                status = "active" if control_event.is_set() else "paused"
                print(f"  Node {node_id}: {status}")

    async def shutdown(self):
        async with self.lock:
            for node_id, (task, _, disconnect_event) in list(self.node_tasks.items()):
                disconnect_event.set()
            await asyncio.sleep(0.2)  # Give time for disconnects
            for node_id, (task, _, _) in list(self.node_tasks.items()):
                task.cancel()
            self.node_tasks.clear()
            print("All nodes removed.")

async def cli_loop(node_manager, log_enabled):
    print("\nCommands: add <id>, remove <id>, pause <id>, resume <id>, list, quitnm, exit, done")
    print("<id>: Node ID to be managed")
    loop = asyncio.get_event_loop()
    while True:
        cmd = await loop.run_in_executor(None, sys.stdin.readline)
        if not cmd:
            continue
        cmd = cmd.strip().split()
        if not cmd:
            continue
        action = cmd[0].lower()
        if action == "add" and len(cmd) == 2:
            await node_manager.add_node(int(cmd[1]))
        elif action == "remove" and len(cmd) == 2:
            await node_manager.remove_node(int(cmd[1]))
        elif action == "pause" and len(cmd) == 2:
            await node_manager.pause_node(int(cmd[1]))
        elif action == "resume" and len(cmd) == 2:
            await node_manager.resume_node(int(cmd[1]))
        elif action == "list":
            await node_manager.list_nodes()
        elif action == "quitnm":
            await node_manager.shutdown()
            break
        elif action == "exit":
            print("Exiting the tester.")
            await node_manager.shutdown()
            exit(0)
        elif action == "done":
            print("Exiting command mode. Resuming log printing.")
            log_enabled.set()
            break
        else:
            print("Unknown command.")

async def main(uri, num_nodes, interval):
    # Load the sample message as a template
    with open(SAMPLE_MSG_PATH, "r") as f:
        sample_msg = json.load(f)
    log_enabled = asyncio.Event()
    log_enabled.set()
    node_manager = NodeManager(uri, interval, sample_msg, log_enabled)
    # Start initial nodes
    for i in range(num_nodes):
        await node_manager.add_node(i)

    loop = asyncio.get_event_loop()
    while True:
        # Wait for 'cmd' or 'exit' from user
        cmd = await loop.run_in_executor(None, sys.stdin.readline)
        if not cmd:
            continue
        if cmd.strip().lower() == 'exit':
            print("Exiting the tester.")
            await node_manager.shutdown()
            break
        if cmd.strip().lower() == 'cmd':
            log_enabled.clear()
            print("\n--- Command mode: log printing paused. Type commands, or 'done' to resume logs. ---")
            await cli_loop(node_manager, log_enabled)
            print("--- Log printing resumed. Type 'cmd' to enter command mode again, or 'exit' to fully quit. ---\n")
        elif cmd.strip().lower() == 'quit':
            await node_manager.shutdown()
            break

if __name__ == "__main__":
    print("NodeIO Protocol Tester")

    if len(sys.argv) < 2:
        print("Usage: python testnode.py ws://<host>/ws [interval_sec] [num_nodes]")
        print("Example: python testnode.py ws://192.168.68.108/ws 2 3")
        sys.exit(1)

    print("Type 'cmd' and press Enter to enter command mode at any time.")
    uri = sys.argv[1]
    interval = float(sys.argv[2]) if len(sys.argv) > 2 else 2
    num_nodes = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    # If user provided ws://host/ws (no port), default to port 80
    from urllib.parse import urlparse
    parsed = urlparse(uri)
    if parsed.port is None and parsed.scheme.startswith('ws'):
        # Rebuild URI with :80 if not present
        uri = f"{parsed.scheme}://{parsed.hostname}:80{parsed.path}"
    asyncio.run(main(uri, num_nodes, interval))
