## FloraLink AI coding guide (ESP-IDF, ESP32-C3)

This repo is an ESP-IDF C project that runs a small “hub” on an ESP32-C3. It exposes an HTTP UI, a WebSocket endpoint for nodes, and several FreeRTOS tasks that monitor sensors and system state. Use these notes to be productive fast and to fit the project’s patterns.

### Big picture
- Target: ESP32-C3 (see build/project_description.json: target=esp32c3).
- Entrypoint: `main/tasks.c` → `app_main()` creates `init_task`, which initializes subsystems then launches tasks:
  - LED blink (`blink.c`), distance sensing (`distance.c`), 1s monitor loop (`monitor_task_1s`), RMT monitor, and GPIO button processing.
- HTTP/Web UI: `main/webserver.c` registers routes and serves a minimal UI. CSS/JS are embedded via `EMBED_FILES` and served at `/main.css` and `/main.js`.
- WebSocket server: `main/websockserver.c` registers `/ws`, manages sessions, and implements app-level ping/pong with per-session timers.
- Node I/O protocol: `main/nodeio.c|h` + `nodeioprotocol.h` define a JSON-over-WebSocket protocol with a magic number, type strings, capability masks, and structured payloads. Active nodes are tracked in `node_contexts[]` (indexed by `node_id`, max = `MAX_SESSIONS`).
- WiFi & Captive Portal: `main/wifi_setup.c` connects in STA mode if credentials exist (`main/nvm.*`), else falls back to an AP + captive portal with credential form and OS-specific probe endpoints.

### Data flow and endpoints
- HTTP routes in `webserver.c`:
  - `/` main page; `/configure` GET/POST; `/nodes` (list UI); `/stats` (HTML or JSON); `/nodeslist` (JSON of connected nodes); `/main.css`, `/main.js` assets.
- WebSocket: `/ws` (text frames). `websockserver_set_receive_callback()` is set by `nodeio_init()`; messages are dispatched in `nodeio_on_message()`/`nodeio_handle_message()`.
- Node message envelope (see `nodeioprotocol.h`): `{ magic: 0xBEEFBEEF, type: "...", node_id, seq_num, timestamp, payload: [...] }`.
  - Types map via `nodeio_type_str_to_enum()`; valid strings are `MSG_TYP_*` (externs in `nodeioprotocol.h`); update both the enum and the string map when adding a new type.
  - Payload items include `sensor`, `diagnostics`, `ota_status`; parsing uses lookup tables and field offsets—extend `sensor_lookup_t`/`service_lookup_t` and the capability enum when adding sensors/services.

### Conventions and patterns that matter
- Capability-driven parsing: The node’s `capability_mask` is built from its `sensors`/`services` array at connect. Only payload fields allowed by that mask are accepted.
- Memory safety tracing: Wrap hot paths with `HEAP_TRACE_START("TAG")` and end with `HEAP_TRACE_END_DEFAULT()` or `HEAP_TRACE_END(threshold)`. Use higher thresholds for operations that allocate timers or large JSON objects (see `HEAP_TRACING.md`). Always `cJSON_Delete(root)` on all parse paths.
- Sessions and timers: For each active WebSocket (`MAX_SESSIONS`), a pong timeout timer is created on ping and must be stopped/deleted on pong or close (`websockserver_reset_pong_timer()`, `websockserver_session_remove()`). Don’t leak timers.
- Node contexts and bounds: `MAX_NODES == MAX_SESSIONS`. Validate `node_id` in `[0, MAX_NODES)`. Free any previous `p_msg` before allocating a new `protocol_msg_t`.
- Embedded assets: If you add new static files, declare them in `main/CMakeLists.txt` `EMBED_FILES` and reference the generated linker symbols in code.
- Buttons: Multi-press detection is centralized in `gpiobutton.c`. Two presses within 3s on `WIFI_RESET_PIN` erases WiFi credentials and reboots to AP mode.

### Build, flash, and debug (Windows PowerShell)
- Ensure ESP-IDF v5.5 is installed and exported in the shell. Typical flow:
```powershell
# One-time per shell
$env:IDF_PATH = "C:\Users\stvar\esp\v5.5\esp-idf"; . "$env:IDF_PATH\export.ps1"
idf.py set-target esp32c3
idf.py build
idf.py -p COM9 flash monitor  # adjust COM port
```
- You can toggle heap tracing via menuconfig (if enabled) or by defining `HEAP_TRACING_ENABLED` (see `HEAP_TRACING.md`).

### Status and constraints
- Serial/flash example uses COM9 by default; change if your board enumerates differently.
- OTA: Not implemented end-to-end yet. `MSG_OTA_*` and `CAP_OTA` exist in the types, but there is no download/flash pipeline wired; treat OTA fields as placeholders until specified.
- Message types: Only those in `nodeioprotocol.h` are in use at the moment; no additional types are planned currently.

### Tests and tooling
- `pytest_blink.py` demonstrates `pytest-embedded` usage (prints binary size). Broader tests are not wired; if you add tests, keep them fast and IDF-compatible.

### Reference test node client
- File: `test/testnode.py` — Python asyncio client that can spawn up to 8 simulated nodes (0..7) speaking the exact protocol.
- Behavior:
  - Negotiates WebSocket subprotocol "arduino" (matches server). Disables client ping; relies on hub ping/PONG.
  - Sends `connect`, waits for `connect_response`, then periodically sends `node_data` with `sensor` + `diagnostics` (and optional `ota_status`).
  - Graceful `disconnect_request` on removal/cancel/exit.
  - Loads default sensors/services from `main/samplenodemsg.json` if present.
- Quick run (PowerShell):
```powershell
python .\test\testnode.py ws://<hub-ip>/ws 2 3   # interval=2s, 3 nodes (IDs 0..2)
```
- Interactive commands (type `cmd` then Enter to enter command mode):
  - `add <id> [<id>...]` or ranges like `add 2-5`
  - `remove <id> [<id>...]`
  - `pause <id> [<id>...]` / `resume <id> [<id>...]`
  - `status <id> [<id>...]`, `list`, `wipe` (remove all), `done` (exit command mode), `exit` (quit)
- Notes:
  - URL normalization adds default port 80 and `/ws` path if missing.
  - Keep node IDs within `[0, 7]` (matches `MAX_SESSIONS`).
  - Requires `websockets` Python package.

### When you extend the protocol or UI
- New message type: update `msg_type_t`, declare a `MSG_TYP_*` string, extend `msg_type_map` in `nodeio.c`, and implement handling in `nodeio_handle_message()` (and any response helpers).
- New sensor/service: add a flag to `capability_flag_t`, extend the relevant lookup table(s) with `name` and `offsetof(...)`, and ensure `nodeio_parse_message_payload()` sets the correct field and `current_cap_mask` bit.
- New HTTP route: add a handler in `webserver.c` and register it. For chunked HTML, use `SEND_HTML_CHUNK()` and end with a NULL chunk.

Key files to study first: `main/tasks.c`, `main/webserver.c`, `main/websockserver.c`, `main/nodeio.c`, `main/nodeioprotocol.h`, `main/wifi_setup.c`, `main/nvm.*`, `main/commonutils.*`, `main/heap_tracing.h`.

Questions or unclear areas? Tell me which part you’re expanding (protocol, UI, WiFi, timers), and I’ll point out the exact tables, handlers, and safety checks to update.

## Nodes and protocol quick reference

### What a node is
- A node is any device that connects via WebSocket to `/ws` and speaks the JSON protocol in `nodeioprotocol.h`.
- node_id must be in `[0, MAX_SESSIONS)` and maps to the hub’s session index; keep it stable and unique per concurrently connected node.

### Transport and session
- Transport: WebSocket (text frames). Hub sends WebSocket PING; node must reply with WebSocket PONG control frames.
- Hub may also send JSON control messages (e.g., `connect_response`, `notify_subscription`).

### Envelope (node → hub)
Required fields on all node → hub messages:
- `magic`: 0xBEEFBEEF
- `type`: one of the strings in `nodeioprotocol.c` (see list below)
- `node_id`: 0..MAX_SESSIONS-1
- `seq_num`: monotonically increasing per node
- `timestamp`: epoch seconds

### Connect handshake
Node → Hub: `type: "connect"` (advertises capabilities)
```json
{
  "magic": 3203391147,
  "type": "connect",
  "node_id": 1,
  "seq_num": 1,
  "timestamp": 1733333333,
  "controller": 1,
  "sw_version": "1.0.3",
  "sensors": ["temperature", "humidity", "distance"],
  "services": ["diagnostics"]
}
```
Hub → Node: `type: "connect_response"` (note: no `magic` in current implementation)
```json
{
  "type": "connect_response",
  "node_id": 1,
  "seq_num": 1,
  "timestamp": 1733333333,
  "status": "accepted"
}
```

### Subscription (hub → node)
Hub sends `type: "notify_subscription"` soon after connect; node should honor the interval and filter.
Subscribed example:
```json
{
  "magic": 3203391147,
  "type": "notify_subscription",
  "node_id": 1,
  "seq_num": 2,
  "timestamp": 1733333338,
  "payload": {
    "status": "subscribed",
    "filter": {
      "sensors": ["temperature", "humidity", "distance"],
      "services": ["diagnostics"]
    },
    "interval": 5000
  }
}
```
Unsubscribed example:
```json
{
  "magic": 3203391147,
  "type": "notify_subscription",
  "node_id": 1,
  "seq_num": 3,
  "timestamp": 1733339999,
  "payload": { "status": "unsubscribed" }
}
```

### Telemetry (node → hub)
Type: `"node_data"`. `payload` is an array of items; each item has a `type` and a typed object. Max 10 items.
Supported item types today:
- `sensor`: fields named by sensor (see list below)
- `diagnostics`: `{ uptime_sec, free_heap, rssi, error_code }`
- `ota_status`: present but OTA pipeline isn’t wired end-to-end

Example:
```json
{
  "magic": 3203391147,
  "type": "node_data",
  "node_id": 1,
  "seq_num": 10,
  "timestamp": 1733338444,
  "payload": [
    {
      "type": "sensor",
      "sensor": {
        "temperature": 22.7,
        "humidity": 45.1,
        "distance": 123.4
      }
    },
    {
      "type": "diagnostics",
      "diagnostics": {
        "uptime_sec": 864,
        "free_heap": 140000,
        "rssi": -56,
        "error_code": 0
      }
    }
  ]
}
```

### Heartbeats and ping/pong
- Hub sends WebSocket PING and expects a PONG control frame; failure closes the session.
- JSON-level heartbeat exists (`"heartbeat"`/`"heartbeat_ack"`) but isn’t required for liveness.

### Disconnect (node → hub)
```json
{
  "magic": 3203391147,
  "type": "disconnect_request",
  "node_id": 1,
  "seq_num": 99,
  "timestamp": 1733340000
}
```

### Capabilities and validation
- Capabilities are built from arrays sent on connect:
  - Sensors: `"temperature"`, `"moisture"`, `"humidity"`, `"distance"`, `"light"`
  - Services: `"diagnostics"`, `"ota"`
- On `node_data`, the hub only accepts fields present in the node’s capability mask; others are ignored/logged.
- Max payload items: 10 (`PROTOCOL_MAX_PAYLOAD_COUNT`).

### Contract summary
- WebSocket text frames at `/ws`; hub pings → node must PONG (control frame).
- Node → hub envelope must include `magic`, `type`, `node_id`, `seq_num`, `timestamp`.
- `node_id` range 0..7 (current `MAX_SESSIONS = 8`). Keep unique while connected.
- Hub suggests publish interval via `notify_subscription`.

### Defined strings (must match exactly)
- Message types (`type`):
  `"connect"`, `"connect_response"`, `"node_data"`, `"notify_subscription"`, `"poll_data"`, `"ota_request"`, `"ota_status"`, `"diagnostics"`, `"diagnostic_request"`, `"ack"`, `"heartbeat"`, `"ping"`, `"pong"`, `"disconnect_request"`, `"error"`, `"unknown"`.
- Payload item `type` values:
  `"sensor"`, `"diagnostics"`, `"ota_status"`.

### Limits and status
- OTA: Not implemented end-to-end; treat `ota_*` as placeholders.
- Only types declared in `nodeioprotocol.h` are in use; no additional types currently.
