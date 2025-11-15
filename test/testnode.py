

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
import argparse
import signal

# Global stop event set by SIGINT handler to improve Ctrl-C responsiveness
STOP_EVENT = threading.Event()

# Protocol constants (should match nodeioprotocol.h)
PROTOCOL_MAGIC = 0xBEEFBEEF
MAX_NODES = 8
MSG_TYP_CONNECT = "connect"
MSG_TYP_NODE_DATA = "node_data"
MSG_PAYLOAD_TYPE_SENSOR = "sensor"
MSG_PAYLOAD_TYPE_DIAGNOSTICS = "diagnostics"
MSG_PAYLOAD_TYPE_OTA_STATUS = "ota_status"
MSG_TYP_DISCONNECT_REQUEST = "disconnect_request"
MSG_PAYLOAD_TYPE_EVENT = "event"
MSG_TYP_LOG_REQUEST = "log_request"
MSG_TYP_LOG_RESPONSE = "log_response"

# Remote logging configuration
LOG_BUFFER_SIZE = 100  # Number of log lines to keep
LOG_ENTRY_LINE_LEN = 256  # Max characters per line

SAMPLE_MSG_PATH = os.path.join(os.path.dirname(__file__), "..", "main", "samplenodemsg.json")

class LogBuffer:
    """Ring buffer for storing recent log lines (simulates node log capture)."""
    def __init__(self, size=LOG_BUFFER_SIZE):
        self.size = size
        self.buffer = []
        self.write_index = 0
        
    def add(self, line):
        """Add a log line to the buffer."""
        line = str(line)[:LOG_ENTRY_LINE_LEN]  # Truncate to max length
        if len(self.buffer) < self.size:
            self.buffer.append(line)
        else:
            self.buffer[self.write_index] = line
            self.write_index = (self.write_index + 1) % self.size
    
    def get_logs(self, max_lines=None):
        """Get logs in chronological order (oldest first)."""
        if not self.buffer:
            return []
        
        # If buffer not full, logs are already in order
        if len(self.buffer) < self.size:
            logs = self.buffer[:]
        else:
            # Buffer is full, need to reorder from write_index (oldest)
            logs = self.buffer[self.write_index:] + self.buffer[:self.write_index]
        
        # Limit to max_lines if specified
        if max_lines and max_lines < len(logs):
            logs = logs[-max_lines:]  # Return most recent max_lines
        
        return logs

def build_base_message(msg_type, node_id, seq_num):
    # Ensure node_id is numeric for strict backend parsing
    node_id = int(node_id)
    return {
        "magic": PROTOCOL_MAGIC,
        "type": msg_type,
        "node_id": node_id,
        "controller": 2,  # CONTROLLER_ARDUINO
        "sw_version": "1.0.0",
        "sensors": ["temperature", "humidity", "moisture", "doorsense"],
        "services": ["diagnostics", "ota", "remote_logging"],
        "seq_num": seq_num,
        "timestamp": int(time.time()),
    }


def build_payloads(sensors):
    # Generate dummy sensor values for each sensor
    # Create simple varying values across sensors
    t = int(time.time())
    sensor_values = {}
    for idx, s in enumerate(sensors):
        # Doorsense is a sporadic/event-driven sensor. Do not include it in periodic payloads.
        if s == "doorsense":
            continue
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

async def simulate_node(uri, node_id, interval, sample_msg, control_event, log_enabled, disconnect_event=None, send_event=None, config=None, event_queue=None):
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
        # Add to log buffer
        log_buffer.add(msg)
        # Print to console if enabled
        if log_enabled.is_set():
            print(msg)

    websocket = None
    recv_task = None
    seq_num = 1
    
    # Create log buffer for this node
    log_buffer = LogBuffer()
    
    # Simulate some initial boot logs
    log_buffer.add(f"I (0) boot: ESP-IDF v4.4.6 2nd stage bootloader")
    log_buffer.add(f"I (0) boot: Compile time: Jan 15 2025 12:34:56")
    log_buffer.add(f"I (12) boot: Enabling RNG early entropy source...")
    log_buffer.add(f"I (17) boot: SPI Speed: 40MHz")
    log_buffer.add(f"I (22) boot: SPI Mode: DIO")
    log_buffer.add(f"I (26) boot: SPI Flash Size: 4MB")
    log_buffer.add(f"I (31) boot: Partition Table:")
    log_buffer.add(f"I (34) boot: End of partition table")
    log_buffer.add(f"I (43) esp_image: segment 0: Verified")
    log_buffer.add(f"I (156) cpu_start: Pro cpu up.")
    log_buffer.add(f"I (167) cpu_start: Starting scheduler on PRO CPU.")
    log_buffer.add(f"I (0) cpu_start: Starting scheduler on APP CPU.")
    log_buffer.add(f"I (234) main: FloraNode starting (node_id={node_id})...")
    log_buffer.add(f"I (256) WiFi: Connecting to AP...")

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
                nonlocal seq_num
                while True:
                    try:
                        msg = await websocket.recv()
                        # Optional: log unexpected server pushes
                        if log_enabled.is_set():
                            print(f"[Node {node_id}] <- {msg}")
                        # Try JSON decode and dispatch known control messages
                        try:
                            msg_obj = json.loads(msg)
                        except Exception:
                            msg_obj = None

                        if isinstance(msg_obj, dict):
                            mtype = msg_obj.get("type")
                            # Only process messages addressed to this node (if node_id present)
                            msg_node_id = msg_obj.get("node_id")
                            if msg_node_id is not None and int(msg_node_id) != int(node_id):
                                # Not for this node
                                continue

                            # notify_subscription: adjust interval and filter
                            if mtype == "notify_subscription":
                                payload = msg_obj.get("payload", {})
                                status = payload.get("status")
                                filt = payload.get("filter", {}) or {}
                                interval_ms = payload.get("interval")
                                if status == "subscribed":
                                    # update per-node config (if provided)
                                    if config is not None:
                                        # filters may omit sensors/services; keep existing when omitted
                                        sensors = filt.get("sensors")
                                        services = filt.get("services")
                                        if sensors is not None:
                                            config["sensors"] = sensors
                                        if services is not None:
                                            config["services"] = services
                                        if interval_ms is not None:
                                            # hub uses milliseconds in examples
                                            try:
                                                config["interval"] = float(interval_ms) / 1000.0
                                            except Exception:
                                                pass
                                        config["subscribed"] = True
                                    # ensure sending is active
                                    control_event.set()
                                    if log_enabled.is_set():
                                        print(f"[Node {node_id}] Subscribed: sensors={config.get('sensors')}, services={config.get('services')}, interval={config.get('interval')}")
                                elif status == "unsubscribed":
                                    if config is not None:
                                        config["subscribed"] = False
                                    control_event.clear()
                                    if log_enabled.is_set():
                                        print(f"[Node {node_id}] Unsubscribed by hub")

                            # log_request: send log buffer contents
                            elif mtype == "log_request":
                                payload = msg_obj.get("payload", {})
                                max_lines = payload.get("max_lines", LOG_BUFFER_SIZE)
                                if log_enabled.is_set():
                                    print(f"[Node {node_id}] Log request received (max_lines: {max_lines})")
                                
                                # Get logs from buffer
                                logs = log_buffer.get_logs(max_lines)
                                
                                # Build response
                                response = {
                                    "magic": PROTOCOL_MAGIC,
                                    "type": MSG_TYP_LOG_RESPONSE,
                                    "node_id": int(node_id),
                                    "seq_num": seq_num,
                                    "timestamp": int(time.time()),
                                    "payload": {
                                        "total_lines": len(logs),
                                        "logs": [{"line": line} for line in logs]
                                    }
                                }
                                
                                # Send response
                                try:
                                    await websocket.send(json.dumps(response))
                                    seq_num += 1
                                    if log_enabled.is_set():
                                        print(f"[Node {node_id}] Sent {len(logs)} log lines to hub")
                                except Exception as e:
                                    if log_enabled.is_set():
                                        print(f"[Node {node_id}] Failed to send log response: {e}")

                            # (poll_data handling intentionally omitted in this tester)

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
        # Initialize per-node config if not provided
        if config is None:
            config = {
                "interval": (float(interval) if (interval is not None and float(interval) > 0.0) else None),
                "sensors": sample_msg.get("sensors", ["temperature", "humidity", "moisture"]),
                "services": sample_msg.get("services", ["diagnostics", "ota"]),
                # Default to unsubscribed so node waits for hub notify_subscription
                "subscribed": False,
            }

        # manual mode is determined dynamically from config['interval'] (None/0 => manual)
        while True:
            try:
                # If a global stop has been requested (SIGINT), signal disconnect
                if STOP_EVENT.is_set():
                    if disconnect_event:
                        disconnect_event.set()
                    # allow the normal disconnect handling at top of loop to run
                    await asyncio.sleep(0)

                # Graceful disconnect requested externally
                if disconnect_event and disconnect_event.is_set():
                    disconnect_msg = build_base_message(MSG_TYP_DISCONNECT_REQUEST, node_id, seq_num)
                    log(f"[Node {node_id}] Sending disconnect request")
                    await websocket.send(json.dumps(disconnect_msg))
                    log(f"[Node {node_id}] Sent disconnect request")
                    return  # exit loop → final cleanup happens in finally

                # Wait until allowed to send (pause/resume)
                await control_event.wait()

                # First, drain any queued sporadic events and send them immediately
                if event_queue is not None:
                    # Drain all pending events
                    while True:
                        try:
                            ev = event_queue.get_nowait()
                        except Exception:
                            break
                        # ev expected to be dict: {"event_type": str, ...other fields...}
                        if isinstance(ev, dict):
                            out = {
                                "magic": PROTOCOL_MAGIC,
                                "type": "node_event",
                                "node_id": int(node_id),
                                "seq_num": seq_num,
                                "timestamp": int(time.time()),
                                "payload": ev,
                            }
                            try:
                                await websocket.send(json.dumps(out))
                                log(f"[Node {node_id}] Sent event: {ev.get('event_type')}")
                                seq_num += 1
                            except Exception as e:
                                log(f"[Node {node_id}] Failed sending event: {e}")

                # Determine current mode & sensors/services from config
                current_interval = config.get("interval")
                if current_interval is None or float(current_interval) == 0.0:
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
                    # Build and send one payload using config filters
                    sensors = config.get("sensors", sample_msg.get("sensors", ["temperature", "humidity", "moisture"]))
                    services = config.get("services", sample_msg.get("services", ["diagnostics", "ota"]))
                    msg = build_base_message(MSG_TYP_NODE_DATA, node_id, seq_num)
                    msg["sensors"] = sensors
                    msg["services"] = services
                    msg["payload"] = build_payloads(sensors)
                    await websocket.send(json.dumps(msg))
                    log(f"[Node {node_id}] Sent live data (manual)")
                    seq_num += 1
                else:
                    # Periodic mode (interval in seconds)
                    sensors = config.get("sensors", sample_msg.get("sensors", ["temperature", "humidity", "moisture"]))
                    services = config.get("services", sample_msg.get("services", ["diagnostics", "ota"]))

                    # Determine if any of the configured sensors are periodic (i.e., not sporadic like 'doorsense')
                    sporadic_sensors = {"doorsense", "door_state"}
                    has_periodic = any((s not in sporadic_sensors) for s in sensors)

                    if has_periodic:
                        msg = build_base_message(MSG_TYP_NODE_DATA, node_id, seq_num)
                        msg["sensors"] = sensors
                        msg["services"] = services
                        msg["payload"] = build_payloads(sensors)

                        await websocket.send(json.dumps(msg))
                        log(f"[Node {node_id}] Sent live data")

                        seq_num += 1
                    # else:
                        # Hub subscription contains only sporadic sensors (e.g., doorsense).
                        # Do not send periodic node_data - the node should instead send sporadic
                        # events via the event queue when they occur. Sleep until the next interval
                        # or until disconnect; draining of event_queue happens earlier in the loop.
                        # if log_enabled.is_set():
                            # print(f"[Node {node_id}] Subscription contains only sporadic sensors; skipping periodic node_data")

                    # Wait for next send cycle or disconnect trigger
                    try:
                        await asyncio.wait_for(asyncio.shield(disconnect_event.wait()), timeout=current_interval)
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
        self.node_tasks = {}  # node_id: (task, control_event, disconnect_event, send_event, config, event_queue)
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
                for node_id, (task, _control_event, disconnect_event, _send_event, _config, _evtq) in list(self.node_tasks.items()):
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
                    task, control_event, disconnect_event, send_event, _cfg, _evtq = self.node_tasks[node_id]
                    if task.done() or (disconnect_event.is_set()):
                        # Stale entry; remove and allow re-add
                        self.node_tasks.pop(node_id, None)
                    else:
                        print(f"Node {node_id} already exists.")
                        continue
                control_event = asyncio.Event()
                # Start paused: follow hub-controlled subscription model.
                # Nodes will remain paused until they receive a notify_subscription
                # with status 'subscribed'. This matches the protocol behavior.
                control_event = asyncio.Event()
                disconnect_event = asyncio.Event()
                send_event = asyncio.Event()
                event_queue = asyncio.Queue()
                # Create per-node config and pass into simulate_node
                cfg = {
                    "interval": (float(self.interval) if (self.interval is not None and float(self.interval) > 0.0) else None),
                    "sensors": self.sample_msg.get("sensors", ["temperature", "humidity", "moisture"]),
                    "services": self.sample_msg.get("services", ["diagnostics", "ota"]),
                    # Start unsubscribed to match protocol: hub must subscribe to enable sends
                    "subscribed": False,
                }
                task = asyncio.create_task(
                    simulate_node(self.uri, node_id, self.interval, self.sample_msg, control_event, self.log_enabled, disconnect_event, send_event, cfg, event_queue)
                )
                self.node_tasks[node_id] = (task, control_event, disconnect_event, send_event, cfg, event_queue)
                print(f"Node {node_id} added and started.")

    async def remove_node(self, *node_ids):
        to_remove = []
        async with self.lock:
            for node_id in node_ids:
                if node_id not in self.node_tasks:
                    print(f"Node {node_id} does not exist.")
                    continue
                task, _, disconnect_event, _, _, _ = self.node_tasks.pop(node_id)
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
                _, control_event, _, _, _, _ = self.node_tasks[node_id]
                control_event.clear()
                print(f"Node {node_id} paused.")

    async def resume_node(self, *node_ids):
        async with self.lock:
            for node_id in node_ids:
                if node_id not in self.node_tasks:
                    print(f"Node {node_id} does not exist.")
                    continue
                _, control_event, _, _, _, _ = self.node_tasks[node_id]
                control_event.set()
                print(f"Node {node_id} resumed.")

    async def list_nodes(self):
        async with self.lock:
            print("Active nodes:")
            for node_id, (task, control_event, _, _, _cfg, _evtq) in self.node_tasks.items():
                status = "active" if control_event.is_set() else "paused"
                print(f"  Node {node_id}: {status}")

    async def status_node(self, *node_ids):
        async with self.lock:
            for node_id in node_ids:
                if node_id not in self.node_tasks:
                    print(f"Node {node_id} does not exist.")
                    continue
                task, control_event, _, _, _cfg, _evtq = self.node_tasks[node_id]
                status = "active" if control_event.is_set() else "paused"
                print(f"Node {node_id} status: {status}, task done: {task.done()}")

    async def shutdown(self):
        # Stop janitor first to avoid races with task cleanup
        await self.stop_janitor()
        async with self.lock:
            tasks = list(self.node_tasks.items())
            self.node_tasks.clear()

        for node_id, (task, _, disconnect_event, _, _, _) in tasks:
            disconnect_event.set()

        for node_id, (task, _, _, _, _, _) in tasks:
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
            task, control_event, disconnect_event, send_event, _cfg, _evtq = self.node_tasks[node_id]
            if not control_event.is_set():
                print(f"Node {node_id} is paused/inactive. Resume it before sending.")
                return
            if disconnect_event.is_set() or task.done():
                print(f"Node {node_id} is disconnected.")
                return
            send_event.set()
            print(f"Triggered one-shot send for node {node_id}.")

    async def event_node(self, node_id: int, event_type: str, message: str = None, fields: dict = None):
        """Enqueue a sporadic event for a node. Event will be sent immediately by simulate_node."""
        async with self.lock:
            if node_id not in self.node_tasks:
                print(f"Node {node_id} does not exist.")
                return
            task, control_event, disconnect_event, send_event, cfg, event_queue = self.node_tasks[node_id]
            if disconnect_event.is_set() or task.done():
                print(f"Node {node_id} is disconnected.")
                return
            # Gate events by subscription: do not enqueue/send events while
            # the node is unsubscribed per protocol behavior.
            if not cfg.get("subscribed"):
                print(f"Node {node_id} is not subscribed; event discarded.")
                return
            payload = {"event_type": event_type}
            if message:
                payload["message"] = message
            if fields and isinstance(fields, dict):
                # Normalize doorsense array -> per-index keys expected by the hub
                # Hub expects keys like 'doorsense_0', 'doorsense_1', ... (or legacy 'door_state_N').
                fs = dict(fields)  # make a shallow copy so we can mutate
                if 'doorsense' in fs and isinstance(fs['doorsense'], (list, tuple)):
                    arr = fs.pop('doorsense')
                    for i, v in enumerate(arr):
                        sval = None
                        if isinstance(v, str):
                            sval = v.upper()
                            if sval not in ("OPEN", "CLOSED"):
                                # try simple heuristics
                                if sval.startswith('O'):
                                    sval = "OPEN"
                                elif sval.startswith('C'):
                                    sval = "CLOSED"
                                else:
                                    sval = "UNKNOWN"
                        elif isinstance(v, (int, float)):
                            sval = "OPEN" if int(v) == 1 else "CLOSED" if int(v) == 0 else "UNKNOWN"
                        elif isinstance(v, bool):
                            sval = "OPEN" if v else "CLOSED"
                        else:
                            sval = "UNKNOWN"
                        fs[f"doorsense_{i}"] = sval
                # also accept legacy 'door_state' array
                if 'door_state' in fs and isinstance(fs['door_state'], (list, tuple)):
                    arr = fs.pop('door_state')
                    for i, v in enumerate(arr):
                        sval = None
                        if isinstance(v, str):
                            sval = v.upper()
                        elif isinstance(v, (int, float)):
                            sval = "OPEN" if int(v) == 1 else "CLOSED" if int(v) == 0 else "UNKNOWN"
                        elif isinstance(v, bool):
                            sval = "OPEN" if v else "CLOSED"
                        else:
                            sval = "UNKNOWN"
                        fs[f"door_state_{i}"] = sval
                payload.update(fs)
            await event_queue.put(payload)
            print(f"Enqueued event for node {node_id}: {event_type}")


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
        elif action == "ev" and len(cmd) >= 3:
            # ev <id> <type> [args...]
            # Supported shorthand for door events:
            #   ev <id> DOOR <idx> <OPEN|CLOSE> [<idx> <OPEN|CLOSE> ...]
            try:
                nid = int(cmd[1])
            except ValueError:
                print("Invalid node ID")
                continue
            etok = cmd[2].upper()
            # Map shorthand to protocol event_type
            if etok in ("DOOR", "EVENT_DOOR"):
                event_type = "EVENT_DOOR"
                args = cmd[3:]
                if len(args) == 0 or (len(args) % 2) != 0:
                    print("Usage: ev <id> DOOR <idx> <OPEN|CLOSE> [<idx> <OPEN|CLOSE> ...]")
                    continue
                fields = {}
                ok = True
                for i in range(0, len(args), 2):
                    try:
                        idx = int(args[i])
                    except ValueError:
                        print(f"Invalid index: {args[i]}")
                        ok = False
                        break
                    state = args[i+1].upper()
                    if state in ("OPEN", "1", "TRUE", "ON"):
                        sval = "OPEN"
                    elif state in ("CLOSE", "CLOSED", "0", "FALSE", "OFF"):
                        sval = "CLOSED"
                    else:
                        print(f"Invalid state: {args[i+1]} (expected OPEN/CLOSE)")
                        ok = False
                        break
                    fields[f"doorsense_{idx}"] = sval
                if not ok:
                    continue
                await node_manager.event_node(nid, event_type, fields=fields)
            else:
                # Generic event: treat remainder as free-form message
                event_type = cmd[2]
                ev_msg = ' '.join(cmd[3:]) if len(cmd) > 3 else None
                await node_manager.event_node(nid, event_type, ev_msg)
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

async def main(uri, num_nodes, interval, sensors_override=None, services_override=None):
    # Load the sample message as a template
    with open(SAMPLE_MSG_PATH, "r") as f:
        sample_msg = json.load(f)
    # Apply any runtime overrides for advertised sensors/services
    if sensors_override:
        sample_msg["sensors"] = sensors_override
    if services_override:
        sample_msg["services"] = services_override
    log_enabled = asyncio.Event()
    log_enabled.set()
    node_manager = NodeManager(uri, interval, sample_msg, log_enabled)
    # Start initial nodes
    for i in range(num_nodes):
        await node_manager.add_node(i)

    # Start background janitor
    await node_manager.start_janitor()

    loop = asyncio.get_event_loop()
    try:
        while True:
            # Wait for 'cmd' or 'exit' from user
            # If a global stop has been requested (SIGINT), shutdown and exit
            if STOP_EVENT.is_set():
                print("\nSIGINT received — shutting down from main loop...")
                try:
                    await node_manager.shutdown()
                except Exception:
                    pass
                return
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
    except (KeyboardInterrupt, asyncio.CancelledError):
        # Graceful shutdown on Ctrl-C or cancellation
        print("\nKeyboard interrupt received — shutting down nodes...")
        try:
            await node_manager.shutdown()
        except Exception:
            pass
        return

if __name__ == "__main__":
    # Special-case the literal 'help' positional for convenience: many users
    # run `python testnode.py help` expecting usage. argparse doesn't treat the
    # word 'help' as a flag, so handle it explicitly before parsing.
    if len(sys.argv) >= 2 and sys.argv[1].lower() == 'help':
        parser = argparse.ArgumentParser(description="NodeIO Protocol Tester")
        parser.print_help()
        sys.exit(0)

    # Use argparse so we can accept optional --sensors/--services overrides
    parser = argparse.ArgumentParser(description="NodeIO Protocol Tester")
    parser.add_argument("uri", help="WebSocket URI of hub, e.g. ws://<host>/ws")
    parser.add_argument("num_nodes", nargs="?", type=int, default=1, help="Number of nodes to simulate (default: 1)")
    parser.add_argument("interval", nargs="?", type=float, default=0.0, help="Send interval in seconds (0 => manual mode)")
    parser.add_argument("--sensors", help="Comma-separated list of sensors to advertise (overrides sample_msg)")
    parser.add_argument("--services", help="Comma-separated list of services to advertise (overrides sample_msg)")
    args = parser.parse_args()

    uri = args.uri
    num_nodes = args.num_nodes
    interval = args.interval
    sensors = [s.strip() for s in args.sensors.split(",")] if args.sensors else None
    services = [s.strip() for s in args.services.split(",")] if args.services else None
    # Normalize URI: ensure path and port defaulting like before
    from urllib.parse import urlparse
    parsed = urlparse(uri)
    if parsed.scheme.startswith('ws'):
        host = parsed.hostname
        port = parsed.port or 80
        path = parsed.path or ''
        if path in ('', '/'):
            path = '/ws'
        uri = f"{parsed.scheme}://{host}:{port}{path}"

    # Register SIGINT handler for interactive shells to set STOP_EVENT so
    # running tasks can observe and begin graceful shutdown immediately.
    try:
        signal.signal(signal.SIGINT, lambda s, f: STOP_EVENT.set())
    except Exception:
        # Not critical; continue without signal handler if unsupported
        pass

    try:
        asyncio.run(main(uri, num_nodes, interval, sensors_override=sensors, services_override=services))
    except KeyboardInterrupt:
        # Fallback: if asyncio.run raises KeyboardInterrupt, ensure exit
        print("Interrupted by user. Exiting.")
        sys.exit(0)
    
