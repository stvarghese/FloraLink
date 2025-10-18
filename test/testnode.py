

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

# Optional nice UI: colors and interactive prompt
try:
    import colorama
    colorama.init()
    _HAS_COLORAMA = True
except Exception:
    _HAS_COLORAMA = False

try:
    from prompt_toolkit import PromptSession
    from prompt_toolkit.history import InMemoryHistory
    from prompt_toolkit.styles import Style
    _HAS_PROMPT_TOOLKIT = True
except Exception:
    _HAS_PROMPT_TOOLKIT = False


class Logger:
    """Simple colored logger used by the testnode client.

    Logging respects an asyncio.Event to enable/disable live log printing
    (used to pause logs while in command mode).
    """
    ANSI_RESET = '\x1b[0m'
    COLORS = [
        '\x1b[38;5;39m',  # blue
        '\x1b[38;5;82m',  # green
        '\x1b[38;5;214m', # orange
        '\x1b[38;5;201m', # pink
        '\x1b[38;5;226m', # yellow
        '\x1b[38;5;51m',  # cyan
        '\x1b[38;5;208m', # coral
        '\x1b[38;5;45m',  # teal
    ]

    def __init__(self):
        self._enabled_event = None

    def set_enabled_event(self, ev: 'asyncio.Event'):
        self._enabled_event = ev

    def _enabled(self):
        return (self._enabled_event is None) or (self._enabled_event.is_set())

    def _color(self, s, color_code):
        if _HAS_COLORAMA or os.name != 'nt':
            return f"{color_code}{s}{self.ANSI_RESET}"
        return s

    def info(self, msg: str):
        if not self._enabled():
            return
        ts = time.strftime('%H:%M:%S')
        global LAST_ACTIVITY
        LAST_ACTIVITY = time.time()
        print(self._color(f"[{ts}] {msg}", '\x1b[37m'))

    def node(self, node_id: int, msg: str, kind: str = 'INFO', payload=None):
        if not self._enabled():
            return
        ts = time.strftime('%H:%M:%S')
        global LAST_ACTIVITY
        LAST_ACTIVITY = time.time()
        color = self.COLORS[node_id % len(self.COLORS)]
        header = f"[{ts}] Node {node_id:02d} | {kind}"
        hdr = self._color(header, color)
        if payload is None:
            print(f"{hdr} - {msg}")
        else:
            # pretty-print small payloads inline, larger payloads as JSON
            try:
                s = json.dumps(payload, ensure_ascii=False)
            except Exception:
                s = str(payload)
            print(f"{hdr} - {msg}: {s}")

    def warn(self, msg: str):
        if not self._enabled():
            return
        ts = time.strftime('%H:%M:%S')
        global LAST_ACTIVITY
        LAST_ACTIVITY = time.time()
        print(self._color(f"[{ts}] WARN: {msg}", '\x1b[38;5;208m'))

    def error(self, msg: str):
        ts = time.strftime('%H:%M:%S')
        global LAST_ACTIVITY
        LAST_ACTIVITY = time.time()
        print(self._color(f"[{ts}] ERROR: {msg}", '\x1b[38;5;196m'))

    def prompt(self):
        # Synchronous fallback prompt (used when prompt_toolkit not available).
        try:
            return input(self._color('> ', '\x1b[36m'))
        except EOFError:
            return ''


async def _spinner_task(stop_event: asyncio.Event):
    """Background spinner shown when idle for SPINNER_IDLE_SECONDS."""
    idx = 0
    printed = False
    while not stop_event.is_set():
        # Don't show spinner while logs are disabled (command mode) or when using prompt_toolkit
        try:
            enabled = LOGGER._enabled()
        except Exception:
            enabled = True
        if (not enabled) or _HAS_PROMPT_TOOLKIT:
            if printed:
                # clear previous spinner
                print('\r' + ' ' * 60 + '\r', end='', flush=True)
                printed = False
            await asyncio.sleep(0.2)
            continue

        now = time.time()
        if now - LAST_ACTIVITY < SPINNER_IDLE_SECONDS:
            # Not idle
            if printed:
                print('\r' + ' ' * 60 + '\r', end='', flush=True)
                printed = False
            await asyncio.sleep(0.2)
            continue

        # Show spinner inline
        ch = SPINNER_CHARS[idx % len(SPINNER_CHARS)]
        print(f"\r{ch} idle... press 'cmd' to enter command mode", end='', flush=True)
        printed = True
        idx += 1
        try:
            await asyncio.wait_for(stop_event.wait(), timeout=0.15)
        except asyncio.TimeoutError:
            pass

    # clear spinner line on exit
    if printed:
        print('\r' + ' ' * 60 + '\r', end='', flush=True)


# Global logger instance
LOGGER = Logger()

# Timestamp of last visible activity (seconds since epoch)
LAST_ACTIVITY = time.time()

# Idle spinner settings
SPINNER_IDLE_SECONDS = 2.0
SPINNER_CHARS = ['|', '/', '-', '\\']

# Protocol constants (should match nodeioprotocol.h)
PROTOCOL_MAGIC = 0xBEEFBEEF
MAX_NODES = 8
MSG_TYP_CONNECT = "connect"
MSG_TYP_NODE_DATA = "node_data"
MSG_PAYLOAD_TYPE_SENSOR = "sensor"
MSG_PAYLOAD_TYPE_DIAGNOSTICS = "diagnostics"
MSG_PAYLOAD_TYPE_OTA_STATUS = "ota_status"
MSG_TYP_DISCONNECT_REQUEST = "disconnect_request"
MSG_TYP_NODE_EVENT = "node_event"
MSG_EVENT_DOOR = "EVENT_DOOR"
MSG_EVENT_SERVICE = "EVENT_SERVICE"

SAMPLE_MSG_PATH = os.path.join(os.path.dirname(__file__), "..", "main", "samplenodemsg.json")

# Attempt to auto-detect NUM_DOOR_SENSORS from the C header so the test client
# stays in sync with the firmware's declaration. Falls back to 1 if parsing fails.
def _detect_num_door_sensors():
    try:
        hdr = os.path.join(os.path.dirname(__file__), '..', 'main', 'nodeioprotocol.h')
        with open(hdr, 'r') as f:
            for line in f:
                line = line.strip()
                if line.startswith('#define') and 'NUM_DOOR_SENSORS' in line:
                    parts = line.split()
                    if len(parts) >= 3:
                        try:
                            return int(parts[2])
                        except Exception:
                            pass
    except Exception:
        pass
    return 1

DOOR_COUNT = _detect_num_door_sensors()

def build_base_message(msg_type, node_id, seq_num):
    # Ensure node_id is numeric for strict backend parsing
    node_id = int(node_id)
    sensors_default = ["temperature", "humidity", "moisture"]
    # Advertise door_state capability when door sensors exist so the hub accepts door events
    try:
        if DOOR_COUNT and DOOR_COUNT > 0:
            sensors_default.append("door_state")
    except NameError:
        pass

    return {
        "magic": PROTOCOL_MAGIC,
        "type": msg_type,
        "node_id": node_id,
        "controller": 2,  # CONTROLLER_ARDUINO
        "sw_version": "1.0.0",
        "sensors": sensors_default,
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


def build_event_message(node_id, seq_num, event_type, event_fields: dict):
    """Build a node_event message where payload is a single object."""
    msg = build_base_message(MSG_TYP_NODE_EVENT, node_id, seq_num)
    payload = {"event_type": event_type}
    payload.update(event_fields)
    msg["payload"] = payload
    return msg


def build_door_event_descriptor(door_states):
    """Return an event descriptor for door states (list of strings or numbers)."""
    return {"kind": "door", "states": list(door_states)}


def build_service_event_descriptor(name, **kwargs):
    """Return an event descriptor for a service event. kwargs depend on service."""
    desc = {"kind": "service", "name": name}
    desc.update(kwargs)
    return desc

async def simulate_node(uri, node_id, interval, sample_msg, control_event, log_enabled, disconnect_event=None, send_event=None, event_queue=None):
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
        LOGGER.node(node_id, msg, kind='INFO')

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
            LOGGER.node(node_id, f"Connected to {uri} (subprotocol: {websocket.subprotocol})", kind='CONNECT')
            if websocket.subprotocol != "arduino":
                LOGGER.node(node_id, f"Warning: negotiated subprotocol is '{websocket.subprotocol}', expected 'arduino'", kind='WARN')
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
        LOGGER.node(node_id, "Sent connect request", kind='TX')

        # Wait for acceptance
        try:
            response = await asyncio.wait_for(websocket.recv(), timeout=10)
            LOGGER.node(node_id, "Received", kind='RX', payload=json.loads(response) if response else None)
            resp_obj = json.loads(response)
            if resp_obj.get("type") != "connect_response" or resp_obj.get("status") != "accepted":
                log(f"[Node {node_id}] Connection not accepted. Exiting.")
                return
        except asyncio.TimeoutError:
            LOGGER.node(node_id, "No response to connect request (timeout). Exiting.", kind='WARN')
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
                        LOGGER.node(node_id, "<- server push", kind='RX', payload=json.loads(msg) if msg else None)
                    except asyncio.CancelledError:
                        break
                    except websockets.ConnectionClosed:  # server closed
                        break
                    except Exception as e:
                        # Keep receiving unless fatal
                        LOGGER.node(node_id, f"Receiver error: {e}", kind='ERROR')
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
                    LOGGER.node(node_id, "Sending disconnect request", kind='TX')
                    await websocket.send(json.dumps(disconnect_msg))
                    LOGGER.node(node_id, "Sent disconnect request", kind='TX')
                    return  # exit loop → final cleanup happens in finally

                # Wait until allowed to send (pause/resume)
                await control_event.wait()

                # First, drain any queued events and send them immediately
                if event_queue is not None:
                    while True:
                        try:
                            ev = event_queue.get_nowait()
                        except asyncio.QueueEmpty:
                            break
                        try:
                            await websocket.send(json.dumps(ev))
                            LOGGER.node(node_id, "Sent event", kind='EVENT', payload=ev.get('payload', ev))
                            seq_num += 1
                        except Exception as e:
                            LOGGER.node(node_id, f"Failed to send event: {e}", kind='ERROR')

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
                    LOGGER.node(node_id, "Sent live data (manual)", kind='TX', payload=msg["payload"]) 
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
                    LOGGER.node(node_id, "Sent live data", kind='TX', payload=msg["payload"])

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
                    LOGGER.node(node_id, "Sent disconnect request (cancel)", kind='TX')
                except Exception:
                    pass
                raise  # re-raise so outer task manager knows

    except Exception as e:
        LOGGER.node(node_id, f"Exception: {e}", kind='ERROR')
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
                    LOGGER.node(node_id, "Connection closed cleanly.", kind='INFO')
                except Exception as e:
                    LOGGER.node(node_id, f"Error during final close: {e}", kind='ERROR')

class NodeManager:
    def __init__(self, uri, interval, sample_msg, log_enabled):
        self.uri = uri
        self.interval = interval
        self.sample_msg = sample_msg
        # node_tasks maps node_id -> (task, control_event, disconnect_event, send_event, event_queue)
        self.node_tasks = {}  # node_id: (task, control_event, disconnect_event, send_event, event_queue)
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
                for node_id, (task, _control_event, disconnect_event, _send_event, _event_queue) in list(self.node_tasks.items()):
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
                event_queue = asyncio.Queue()
                task = asyncio.create_task(
                    simulate_node(self.uri, node_id, self.interval, self.sample_msg, control_event, self.log_enabled, disconnect_event, send_event, event_queue)
                )
                self.node_tasks[node_id] = (task, control_event, disconnect_event, send_event, event_queue)
                print(f"Node {node_id} added and started.")

    async def remove_node(self, *node_ids):
        to_remove = []
        async with self.lock:
            for node_id in node_ids:
                if node_id not in self.node_tasks:
                    print(f"Node {node_id} does not exist.")
                    continue
                task, _, disconnect_event, _, _ = self.node_tasks.pop(node_id)
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
                _, control_event, _, _, _ = self.node_tasks[node_id]
                control_event.clear()
                print(f"Node {node_id} paused.")

    async def resume_node(self, *node_ids):
        async with self.lock:
            for node_id in node_ids:
                if node_id not in self.node_tasks:
                    print(f"Node {node_id} does not exist.")
                    continue
                _, control_event, _, _, _ = self.node_tasks[node_id]
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
                task, control_event, _, _, _ = self.node_tasks[node_id]
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
            task, control_event, disconnect_event, send_event, event_queue = self.node_tasks[node_id]
            if not control_event.is_set():
                print(f"Node {node_id} is paused/inactive. Resume it before sending.")
                return
            if disconnect_event.is_set() or task.done():
                print(f"Node {node_id} is disconnected.")
                return
            send_event.set()
            print(f"Triggered one-shot send for node {node_id}.")

    async def queue_event(self, node_id: int, event_msg: dict):
        """Queue a pre-built event message for a node to send as soon as possible.

        The event_msg should already be a fully-formed JSON-serializable dict (e.g., from
        build_event_message())."""
        async with self.lock:
            if node_id not in self.node_tasks:
                print(f"Node {node_id} does not exist.")
                return
            _, control_event, disconnect_event, _, event_queue = self.node_tasks[node_id]
            if disconnect_event.is_set():
                print(f"Node {node_id} is disconnected; cannot queue event.")
                return
            await event_queue.put(event_msg)
            print(f"Queued event for node {node_id}.")

    async def queue_door_event(self, node_id: int, door_states):
        # Build event payload with door_state_0..door_state_{DOOR_COUNT-1} keys.
        # Accept door_states as list of states (strings like 'OPEN'/'CLOSE' or numeric values).
        async with self.lock:
            if node_id not in self.node_tasks:
                print(f"Node {node_id} does not exist.")
                return
            task, control_event, disconnect_event, send_event, event_queue = self.node_tasks[node_id]
            if disconnect_event.is_set():
                print(f"Node {node_id} is disconnected; cannot queue event.")
                return

            payload = {}
            for i in range(DOOR_COUNT):
                key = f"door_state_{i}"
                if i < len(door_states):
                    v = door_states[i]
                    # Normalize common string inputs
                    if isinstance(v, str):
                        vs = v.strip().upper()
                        if vs in ("OPEN", "OPEN\r", "OPEN\n"):
                            payload[key] = "OPEN"
                        elif vs in ("CLOSE", "CLOSED", "CLOSE\r"):
                            payload[key] = "CLOSE"
                        else:
                            # leave as provided (hub will normalize or interpret)
                            payload[key] = v
                    else:
                        # numeric value
                        payload[key] = int(v)
                else:
                    # Not provided → leave absent (hub treats missing as unknown)
                    # but put explicit "UNKNOWN" for clarity
                    payload[key] = "UNKNOWN"

            ev = build_event_message(node_id, 0, MSG_EVENT_DOOR, payload)
            await event_queue.put(ev)
            print(f"Queued door event for node {node_id} (door count {DOOR_COUNT}).")

    async def queue_service_event(self, node_id: int, service_name: str, **kwargs):
        desc = build_service_event_descriptor(service_name, **kwargs)
        async with self.lock:
            if node_id not in self.node_tasks:
                print(f"Node {node_id} does not exist.")
                return
            ev = build_event_message(node_id, 0, MSG_EVENT_SERVICE, desc)
            _, _, disconnect_event, _, event_queue = self.node_tasks[node_id]
            if disconnect_event.is_set():
                print(f"Node {node_id} is disconnected; cannot queue service event.")
                return
            await event_queue.put(ev)
            print(f"Queued service event '{service_name}' for node {node_id}.")


async def cli_loop(node_manager, log_enabled):
    print("\nCommands: add <id> [<id>...], remove <id> [<id>...], pause <id> [<id>...], resume <id> [<id>...], status <id> [<id>...], list, sd <id> [<id>...], ev <id> door <state> [<state>...], sev <id> <service> [key=value ...], wipe, exit, done")
    print("<id>: Node ID(s) to be managed (space-separated)")
    print("list: List all nodes and their status")
    print("wipe: Remove all nodes")    
    print("done: Resume log printing")
    print("exit: Exit the tester completely")
    loop = asyncio.get_event_loop()
    session = None
    if _HAS_PROMPT_TOOLKIT:
        style = Style.from_dict({'prompt': 'ansicyan bold'})
        session = PromptSession('> ', history=InMemoryHistory(), style=style)

    while True:
        if session is not None:
            # prompt_toolkit async prompt
            try:
                cmdline = await session.prompt_async()
            except (EOFError, KeyboardInterrupt):
                cmdline = ''
        else:
            cmdline = await loop.run_in_executor(None, sys.stdin.readline)
        cmd = cmdline
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
        elif action == "ev" and len(cmd) >= 3:
            # ev <node_id> door <state> [<state>...]
            try:
                node_id = int(cmd[1])
            except ValueError:
                print("Invalid node id for ev command")
                continue
            if cmd[2].lower() != 'door':
                print("Only 'door' kind supported for ev. Use 'sev' for service events.")
                continue
            door_states = cmd[3:] if len(cmd) > 3 else ['open']
            await node_manager.queue_door_event(node_id, door_states)
        elif action == "sev" and len(cmd) >= 3:
            # sev <node_id> <service> [key=value ...]
            try:
                node_id = int(cmd[1])
            except ValueError:
                print("Invalid node id for sev command")
                continue
            service_name = cmd[2]
            kv = {}
            for pair in cmd[3:]:
                if '=' in pair:
                    k, v = pair.split('=', 1)
                    kv[k] = v
            await node_manager.queue_service_event(node_id, service_name, **kv)
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
    LOGGER.set_enabled_event(log_enabled)
    # Spinner control
    spinner_stop = asyncio.Event()
    spinner = asyncio.create_task(_spinner_task(spinner_stop))
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
            spinner_stop.set()
            try:
                await asyncio.wait_for(spinner, timeout=1.0)
            except asyncio.TimeoutError:
                spinner.cancel()
            await node_manager.shutdown()
            break
        if cmd.strip().lower() == 'cmd':
            log_enabled.clear()
            print("\n--- Command mode: log printing paused. Type commands, or 'done' to resume logs. ---")
            await cli_loop(node_manager, log_enabled)
            print("--- Log printing resumed. Type 'cmd' to enter command mode again, or 'exit' to fully quit. ---\n")
        elif cmd.strip().lower() == 'quit':
            spinner_stop.set()
            try:
                await asyncio.wait_for(spinner, timeout=1.0)
            except asyncio.TimeoutError:
                spinner.cancel()
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
