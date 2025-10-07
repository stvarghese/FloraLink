

import asyncio
from asyncio import log
import websockets
import json
import sys
import time
import os
import threading
import contextlib
from contextlib import contextmanager

# Protocol constants (should match nodeioprotocol.h)
PROTOCOL_MAGIC = 0xBEEFBEEF
MAX_NODES = 8
MSG_TYP_CONNECT = "connect"
MSG_TYP_NODE_DATA = "node_data"
MSG_PAYLOAD_TYPE_SENSOR = "sensor"
MSG_PAYLOAD_TYPE_DIAGNOSTICS = "diagnostics"
MSG_PAYLOAD_TYPE_OTA_STATUS = "ota_status"
MSG_TYP_DISCONNECT_REQUEST = "disconnect_request"

SAMPLE_MSG_PATH = os.path.join(os.path.dirname(__file__), "..", "main", "samplenodemsg.json")

def build_base_message(msg_type, node_id, seq_num):
    # Ensure node_id is numeric for strict backend parsing
    node_id = int(node_id)
    return {
        "magic": PROTOCOL_MAGIC,
        "type": msg_type,
        "node_id": node_id,
        "controller": 2,  # CONTROLLER_ARDUINO
        "sw_version": "1.0.0",
        "sensors": ["temperature", "humidity", "moisture"],
        "services": ["diagnostics", "ota"],
        "seq_num": seq_num,
        "timestamp": int(time.time()),
    }


def build_payloads(sensors):
    # Generate dummy sensor values for each sensor
    # Create simple varying values across sensors
    t = int(time.time())
    sensor_values = {}
    for idx, s in enumerate(sensors):
        if s == "temperature":
            sensor_values[s] = 20.0 + (t % 10)  # 20..29 C
        elif s == "humidity":
            sensor_values[s] = 40 + (t % 20)    # 40..59 %
        elif s == "moisture":
            sensor_values[s] = 200 + (t % 100)  # arbitrary units
        elif s == "distance":
            sensor_values[s] = 50 + (t % 50)    # cm
        elif s == "light":
            sensor_values[s] = 100 + (t % 200)  # lux-ish
        else:
            sensor_values[s] = 42.0
    sensor_payload = {
        "type": MSG_PAYLOAD_TYPE_SENSOR,
        "sensor": sensor_values
    }
    diagnostics_payload = {
        "type": MSG_PAYLOAD_TYPE_DIAGNOSTICS,
        "diagnostics": {
            "uptime_sec": t % 200000,
            "free_heap": 20000 + (t % 5000),
            "rssi": -70 + (t % 5),
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

async def simulate_node(uri, node_id, interval, sample_msg, control_event, log_enabled, disconnect_event=None, send_event=None):
    """
    Simulates a single node over a websocket connection.

    Lifecycle:
      1. Connect to server.
      2. Send CONNECT request and wait for acceptance.
      3. Loop: send NODE_DATA messages at given interval (pause/resume controlled by control_event).
      4. Handle ping/pong and unexpected messages.
      5. Handle graceful disconnect on:
           - disconnect_event triggered
           - task cancellation
           - exceptions
      6. Always ensure websocket is closed in the end.
    """

    def log(msg):
        if log_enabled.is_set():
            print(msg)

    websocket = None
    recv_task = None
    seq_num = 1

    try:
        # === Connect phase ===
        try:
            # Match ESP-IDF server's supported_subprotocol ("arduino").
            # Disable client pings; the Hub pings and we must read to auto-pong.
            websocket = await websockets.connect(
                uri,
                subprotocols=["arduino"],
                ping_timeout=None,
                ping_interval=None,
                max_size=2**20,
            )
            log(f"[Node {node_id}] Connected to {uri} (subprotocol: {websocket.subprotocol})")
            if websocket.subprotocol != "arduino":
                log(f"[Node {node_id}] Warning: negotiated subprotocol is '{websocket.subprotocol}', expected 'arduino'")
        except getattr(websockets, 'NegotiationError', Exception) as e:  # older/newer websockets versions
            log(f"[Node {node_id}] Subprotocol negotiation failed: {e}")
            if disconnect_event:
                disconnect_event.set()
            return
        except Exception as e:
            log(f"[Node {node_id}] Failed to connect: {repr(e)}")
            if disconnect_event:
                disconnect_event.set()
            return

        # Send connect request
        connect_req = build_base_message(MSG_TYP_CONNECT, node_id, 0)
        await websocket.send(json.dumps(connect_req))
        log(f"[Node {node_id}] Sent connect request")

        # Wait for acceptance
        try:
            response = await asyncio.wait_for(websocket.recv(), timeout=10)
            log(f"[Node {node_id}] Received: {response}")
            resp_obj = json.loads(response)
            if resp_obj.get("type") != "connect_response" or resp_obj.get("status") != "accepted":
                log(f"[Node {node_id}] Connection not accepted. Exiting.")
                return
        except asyncio.TimeoutError:
            log(f"[Node {node_id}] No response to connect request (timeout). Exiting.")
            if disconnect_event:
                disconnect_event.set()
            return

        # Start background receiver to process control frames (ping/pong) and any messages
        async def _receiver():
            try:
                while True:
                    try:
                        msg = await websocket.recv()
                        # Optional: log unexpected server pushes
                        if log_enabled.is_set():
                            print(f"[Node {node_id}] <- {msg}")
                    except asyncio.CancelledError:
                        break
                    except websockets.ConnectionClosed:  # server closed
                        break
                    except Exception as e:
                        # Keep receiving unless fatal
                        if log_enabled.is_set():
                            print(f"[Node {node_id}] Receiver error: {e}")
                        await asyncio.sleep(0.05)
            finally:
                return

        recv_task = asyncio.create_task(_receiver())

        # === Active phase ===
        manual_mode = (interval is None) or (float(interval) == 0.0)
        while True:
            try:
                # Graceful disconnect requested externally
                if disconnect_event and disconnect_event.is_set():
                    disconnect_msg = build_base_message(MSG_TYP_DISCONNECT_REQUEST, node_id, seq_num)
                    log(f"[Node {node_id}] Sending disconnect request")
                    await websocket.send(json.dumps(disconnect_msg))
                    log(f"[Node {node_id}] Sent disconnect request")
                    return  # exit loop → final cleanup happens in finally

                # Wait until allowed to send (pause/resume)
                await control_event.wait()

                if manual_mode:
                    # Manual mode: wait for an explicit trigger to send one payload
                    if send_event is None:
                        # Safety: if no send_event provided, just idle
                        await asyncio.sleep(0.05)
                        continue
                    # Wait for send event, but also allow disconnect to break out
                    # Check disconnect quickly before blocking
                    if disconnect_event and disconnect_event.is_set():
                        continue
                    await send_event.wait()
                    # Clear the event (one-shot)
                    send_event.clear()
                    # Build and send one payload
                    sensors = sample_msg.get("sensors", ["temperature", "humidity", "moisture"])
                    services = sample_msg.get("services", ["diagnostics", "ota"])
                    msg = build_base_message(MSG_TYP_NODE_DATA, node_id, seq_num)
                    msg["sensors"] = sensors
                    msg["services"] = services
                    msg["payload"] = build_payloads(sensors)
                    await websocket.send(json.dumps(msg))
                    log(f"[Node {node_id}] Sent live data (manual)")
                    seq_num += 1
                else:
                    # Periodic mode
                    sensors = sample_msg.get("sensors", ["temperature", "humidity", "moisture"])
                    services = sample_msg.get("services", ["diagnostics", "ota"])
                    msg = build_base_message(MSG_TYP_NODE_DATA, node_id, seq_num)
                    msg["sensors"] = sensors
                    msg["services"] = services
                    msg["payload"] = build_payloads(sensors)

                    await websocket.send(json.dumps(msg))
                    log(f"[Node {node_id}] Sent live data")

                    # Receiver runs in background; no foreground recv needed

                    seq_num += 1

                    # Wait for next send cycle or disconnect trigger
                    try:
                        await asyncio.wait_for(asyncio.shield(disconnect_event.wait()), timeout=interval)
                    except asyncio.TimeoutError:
                        pass  # normal interval tick

            except asyncio.CancelledError:
                # Handle task cancellation gracefully
                disconnect_msg = build_base_message(MSG_TYP_DISCONNECT_REQUEST, node_id, seq_num)
                try:
                    await websocket.send(json.dumps(disconnect_msg))
                    log(f"[Node {node_id}] Sent disconnect request (cancel)")
                except Exception:
                    pass
                raise  # re-raise so outer task manager knows

    except Exception as e:
        log(f"[Node {node_id}] Exception: {e}")
        if disconnect_event:
            disconnect_event.set()

    finally:
        # === Final cleanup ===
        if recv_task is not None:
            try:
                recv_task.cancel()
                with contextlib.suppress(Exception):
                    await recv_task
            except Exception:
                pass
        if websocket is not None:
            try:
                await websocket.close()
                await websocket.wait_closed()
                log(f"[Node {node_id}] Connection closed cleanly.")
            except Exception as e:
                log(f"[Node {node_id}] Error during final close: {e}")

class NodeManager:
    def __init__(self, uri, interval, sample_msg, log_enabled):
        self.uri = uri
        self.interval = interval
        self.sample_msg = sample_msg
        self.node_tasks = {}  # node_id: (task, control_event, disconnect_event, send_event)
        self.lock = asyncio.Lock()
        self.log_enabled = log_enabled
        # Janitor to proactively clean stale/disconnected nodes
        self._janitor_task = None
        self._janitor_stop = asyncio.Event()

    async def _janitor_loop(self):
        while not self._janitor_stop.is_set():
            stale = []
            # Collect stale nodes under lock
            async with self.lock:
                for node_id, (task, _control_event, disconnect_event, _send_event) in list(self.node_tasks.items()):
                    if task.done() or disconnect_event.is_set():
                        # Remove from registry; we'll await outside of lock
                        self.node_tasks.pop(node_id, None)
                        stale.append((node_id, task))
            # Finalize stale tasks outside the lock
            for node_id, task in stale:
                try:
                    if not task.done():
                        await asyncio.wait_for(task, timeout=1.0)
                except asyncio.TimeoutError:
                    task.cancel()
                    with contextlib.suppress(asyncio.CancelledError):
                        await task
                except Exception:
                    # Swallow task exceptions during cleanup
                    pass
                print(f"Janitor: cleaned up node {node_id}.")

            # Sleep with stop awareness
            try:
                await asyncio.wait_for(self._janitor_stop.wait(), timeout=2.0)
            except asyncio.TimeoutError:
                pass

    async def start_janitor(self):
        if self._janitor_task is None or self._janitor_task.done():
            self._janitor_stop.clear()
            self._janitor_task = asyncio.create_task(self._janitor_loop())

    async def stop_janitor(self):
        if self._janitor_task is not None:
            self._janitor_stop.set()
            try:
                await asyncio.wait_for(self._janitor_task, timeout=2.0)
            except asyncio.TimeoutError:
                self._janitor_task.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await self._janitor_task
            finally:
                self._janitor_task = None

    async def add_node(self, *node_ids):
        async with self.lock:
            for node_id in node_ids:
                if not (0 <= node_id < MAX_NODES):
                    print(f"Node {node_id} is out of allowed range (0-{MAX_NODES-1}).")
                    continue
                if node_id in self.node_tasks:
                    task, control_event, disconnect_event, send_event = self.node_tasks[node_id]
                    if task.done() or (disconnect_event.is_set()):
                        # Stale entry; remove and allow re-add
                        self.node_tasks.pop(node_id, None)
                    else:
                        print(f"Node {node_id} already exists.")
                        continue
                control_event = asyncio.Event()
                control_event.set()  # Start as active
                disconnect_event = asyncio.Event()
                send_event = asyncio.Event()
                task = asyncio.create_task(
                    simulate_node(self.uri, node_id, self.interval, self.sample_msg, control_event, self.log_enabled, disconnect_event, send_event)
                )
                self.node_tasks[node_id] = (task, control_event, disconnect_event, send_event)
                print(f"Node {node_id} added and started.")

    async def remove_node(self, *node_ids):
        to_remove = []
        async with self.lock:
            for node_id in node_ids:
                if node_id not in self.node_tasks:
                    print(f"Node {node_id} does not exist.")
                    continue
                task, _, disconnect_event, _ = self.node_tasks.pop(node_id)
                disconnect_event.set()  # signal node to disconnect
                to_remove.append((node_id, task))

        # wait outside the lock so other ops aren’t blocked
        for node_id, task in to_remove:
            try:
                await asyncio.wait_for(task, timeout=2.0)
            except asyncio.TimeoutError:
                task.cancel()
                try:
                    await task
                except asyncio.CancelledError:
                    pass
            print(f"Node {node_id} removed.")


    async def pause_node(self, *node_ids):
        async with self.lock:
            for node_id in node_ids:
                if node_id not in self.node_tasks:
                    print(f"Node {node_id} does not exist.")
                    continue
                _, control_event, _, _ = self.node_tasks[node_id]
                control_event.clear()
                print(f"Node {node_id} paused.")

    async def resume_node(self, *node_ids):
        async with self.lock:
            for node_id in node_ids:
                if node_id not in self.node_tasks:
                    print(f"Node {node_id} does not exist.")
                    continue
                _, control_event, _, _ = self.node_tasks[node_id]
                control_event.set()
                print(f"Node {node_id} resumed.")

    async def list_nodes(self):
        async with self.lock:
            print("Active nodes:")
            for node_id, (task, control_event, _, _) in self.node_tasks.items():
                status = "active" if control_event.is_set() else "paused"
                print(f"  Node {node_id}: {status}")

    async def status_node(self, *node_ids):
        async with self.lock:
            for node_id in node_ids:
                if node_id not in self.node_tasks:
                    print(f"Node {node_id} does not exist.")
                    continue
                task, control_event, _, _ = self.node_tasks[node_id]
                status = "active" if control_event.is_set() else "paused"
                print(f"Node {node_id} status: {status}, task done: {task.done()}")

    async def shutdown(self):
        # Stop janitor first to avoid races with task cleanup
        await self.stop_janitor()
        async with self.lock:
            tasks = list(self.node_tasks.items())
            self.node_tasks.clear()

        for node_id, (task, _, disconnect_event, _) in tasks:
            disconnect_event.set()

        for node_id, (task, _, _, _) in tasks:
            try:
                await asyncio.wait_for(task, timeout=2.0)
            except asyncio.TimeoutError:
                task.cancel()
                try:
                    await task
                except asyncio.CancelledError:
                    pass
        print("All nodes removed.")

    async def send_data(self, node_id: int):
        async with self.lock:
            if node_id not in self.node_tasks:
                print(f"Node {node_id} does not exist.")
                return
            task, control_event, disconnect_event, send_event = self.node_tasks[node_id]
            if not control_event.is_set():
                print(f"Node {node_id} is paused/inactive. Resume it before sending.")
                return
            if disconnect_event.is_set() or task.done():
                print(f"Node {node_id} is disconnected.")
                return
            send_event.set()
            print(f"Triggered one-shot send for node {node_id}.")


async def cli_loop(node_manager, log_enabled):
    print("\nCommands: add <id> [<id>...], remove <id> [<id>...], pause <id> [<id>...], resume <id> [<id>...], status <id> [<id>...], list, sd <id> [<id>...], wipe, exit, done")
    print("<id>: Node ID(s) to be managed (space-separated)")
    print("list: List all nodes and their status")
    print("wipe: Remove all nodes")    
    print("done: Resume log printing")
    print("exit: Exit the tester completely")
    loop = asyncio.get_event_loop()
    while True:
        cmd = await loop.run_in_executor(None, sys.stdin.readline)
        if not cmd:
            continue
        cmd = cmd.strip().split()
        if not cmd:
            continue
        action = cmd[0].lower()
        # Actions with more than one node ID possible
        if action in {"add", "remove", "pause", "resume", "status", "sd"} and len(cmd) >= 2:
            try:
                node_ids = []
                for tok in cmd[1:]:
                    if '-' in tok:
                        a, b = tok.split('-', 1)
                        a = int(a); b = int(b)
                        node_ids.extend(list(range(min(a,b), max(a,b)+1)))
                    else:
                        node_ids.append(int(tok))
            except ValueError:
                print("Invalid node ID(s). Must be integers.")
                continue
            if action == "add":
                await node_manager.add_node(*node_ids)
            elif action == "remove":
                await node_manager.remove_node(*node_ids)
            elif action == "pause":
                await node_manager.pause_node(*node_ids)
            elif action == "resume":
                await node_manager.resume_node(*node_ids)
            elif action == "status":
                await node_manager.status_node(*node_ids)
            elif action == "sd":
                # Trigger one-shot send for each provided ID
                for nid in node_ids:
                    await node_manager.send_data(nid)
        elif action == "list":
            await node_manager.list_nodes()
        elif action == "wipe":
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
        elif action == "cmd":
            print("Already in command mode.")
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

    # Start background janitor
    await node_manager.start_janitor()

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
        print("Usage: python testnode.py ws://<host>/ws [num_nodes] [interval_sec]")
        print("- If interval_sec is 0 or omitted, nodes do NOT send periodically; use 'sd <id>' in CLI mode to send once.")
        print("Example periodic: python testnode.py ws://192.168.68.108/ws 3 2     # 3 nodes, 2s interval")
        print("Example manual:   python testnode.py ws://192.168.68.108/ws 2       # 2 nodes, manual send")
        sys.exit(1)

    print("Type 'cmd' and press Enter to enter command mode at any time.")
    uri = sys.argv[1]
    # Args: [num_nodes] [interval_sec]; if interval omitted → manual mode
    num_nodes = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    interval = float(sys.argv[3]) if len(sys.argv) > 3 else 0.0
    # If user provided ws://host/ws (no port), default to port 80
    from urllib.parse import urlparse
    parsed = urlparse(uri)
    if parsed.scheme.startswith('ws'):
        host = parsed.hostname
        port = parsed.port or 80
        path = parsed.path or ''
        if path in ('', '/'):
            path = '/ws'
        uri = f"{parsed.scheme}://{host}:{port}{path}"
    asyncio.run(main(uri, num_nodes, interval))
