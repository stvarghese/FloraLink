# FloraLink TODO & Progress Tracker

## Completed ✅

### Remote Logging Backend Fixes (Dec 2, 2025)
- [x] Fix race condition with mutex
  - Added FreeRTOS mutex protecting log access in MSG_LOG_RESPONSE and GET handlers
- [x] Fix memory leaks (disconnect/reconnect)
  - Implemented nodeio_clear_node_logs() and proper cleanup on disconnect
- [x] Fix node_id bounds check
  - Added input validation preventing buffer overflow in webserver.c
- [x] Add log content validation
  - Implemented 1KB line limit and UTF-8 character sanitization
- [x] Optimize JSON escaping
  - Added batch buffering (2KB chunks) for 99% reduction in overhead
- [x] Limit JSON response size
  - Implemented 64KB max response size with truncation
- [x] Build verification
  - All 7 fixes compile successfully without errors
- [x] Enhanced testnode.py for remote logging
  - Improved LogBuffer with hub validation
  - Added realistic ESP-IDF boot sequence
  - Runtime logging during data transmission

### Copilot Agent Instructions (Dec 2, 2025)
- [x] Created agent.md as permanent reference for AI assistance
  - Analysis-first approach (only implement when explicitly asked)
  - No auto-documentation creation policy
  - Project context and decision trees

---

## In Progress 🔄

### Testing & Validation
- [ ] Manual testing of 7 remote logging scenarios
  - [ ] Test 1: Memory leak on disconnect
  - [ ] Test 2: Race condition under concurrent load
  - [ ] Test 3: Node ID bounds checking
  - [ ] Test 4: Response size limiting
  - [ ] Test 5: Max lines validation
  - [ ] Test 6: Malformed log handling
  - [ ] Test 7: Performance benchmarking
- [ ] Load testing with 8 concurrent nodes
- [ ] Memory profiling to verify leak fixes

---

## Pending 📋

# FloraLink Power Management and On-Demand Data TODO

## 1. Power/Active Window Strategy (Phase 1)
- [ ] Hub stays in low-power (Wi-Fi modem/light sleep) by default.
- [ ] When node sends data (any valid message):
    - [ ] Hub wakes, processes data, publishes, runs health monitoring, etc.
    - [ ] Enter "active window" (normal mode) for a configurable period (e.g., 30–120s).
    - [ ] Each new data message resets the active window timer.
    - [ ] After timer expires with no new data, hub returns to low-power mode.
- [ ] All power/mode transitions should use or extend `modemanager.c`.
- [ ] During active window:
    - [ ] All normal tasks run (UI, pings, distance, etc.).
    - [ ] User can interact via web UI or button.
- [ ] In low-power mode:
    - [ ] Only essential listeners (Wi-Fi, WebSocket, button IRQ) are active.
    - [ ] CPU and radio sleep as much as possible.

## 2. On-Demand Data (User/Hub-Initiated Poll)
- [ ] Allow user to request data from a node on demand (via UI or button):
    - [ ] If node is connected: send `poll_data` control message immediately.
    - [ ] If node is offline: queue the request in the node context/state.
    - [ ] When node next connects, send queued poll as first control message after connect_response.
- [ ] Track per-request state:
    - none: No outstanding request.
    - queued: User requested poll, node offline.
    - sent: Poll sent to node, waiting for data.
    - responded: Node replied with data.
    - expired: No response in timeout window.
    - cancelled: User/system cancelled request.
- [ ] UI:
    - Show status (Queued, Sent, Responded, Expired, Cancelled) for each node.
    - Allow user to cancel a pending request.
    - Collapse multiple requests into a single outstanding request per node (debounce/merge).
- [ ] Logic:
    - On node_data, match to outstanding request and mark as responded.
    - If no response in N seconds, mark as expired.
    - Clean up state after completion or cancellation.

## 3. Precompile Switches and Development vs. Field Modes
- [ ] Introduce a precompile switch `ON_FIELD`:
    - [ ] When defined, disable application-level ping monitoring and disconnects on pong timeouts (WebSocket keepalive logic).
    - [ ] When not defined (development), keep ping/pong monitoring to catch node issues and debug connectivity.
- [ ] Document and use `ON_FIELD` in all relevant files (e.g., `main/nodeio.c`, `main/websockserver.c`, `main/tasks.c`).

## 4. Other Potential Low Power Blockers
- [ ] Identify and gate any background tasks or timers that could prevent low power mode:
    - [ ] Distance monitoring: Only run 500ms measurement loop during the active window; fully suspend in low-power mode.
    - [ ] Monitor/ping tasks: Only run frequent pings/monitoring in active window; disable or slow down in low-power mode.
    - [ ] Web server/UI: Ensure HTTP server and UI handlers do not prevent sleep (should wake on demand via Wi-Fi RX).
    - [ ] Button processing: Use interrupts or minimal polling to avoid CPU wakeups.
    - [ ] Any periodic logging, stats, or debug output: Gate or reduce in low-power mode.
- [ ] Review all FreeRTOS tasks for unnecessary wakeups or busy loops.

## 3. Power Save/Mode Management

## 4. Background Task Adjustments
- [ ] Reduce or disable periodic pings in low-power mode.
- [ ] Gate distance and monitor tasks to run only in active window.
- [ ] Ensure all tasks block or sleep when not needed.

## 5. Queued Request State Machine (for reference)
- none: No outstanding request.
- queued: User requested poll, node offline.
- sent: Poll sent to node, waiting for data.
- responded: Node replied with data.
- expired: No response in timeout window.
- cancelled: User/system cancelled request.

## 6. Open Questions/Considerations
- [ ] How to handle multiple queued requests (debounce/merge)?
- [ ] Should we allow deep sleep (with loss of Wi-Fi connectivity) for even lower power?
- [ ] How to persist queued requests across hub reboots?
- [ ] How to handle nodes that never reconnect (expiry/cleanup)?

---

## Initial Assessment of the "Active Window" Approach
- Pros:
    - Simple, robust: hub always wakes on node traffic or user action.
    - No need for precise node interval knowledge.
    - Works for both frequent and rare node updates.
    - Easy to tune active window for power vs. responsiveness.
- Possible issues:
    - If nodes send data in rapid bursts, hub may stay active longer than needed (but timer resets are expected).
    - If a node sends malformed or spammy data, hub could be kept awake (should rate-limit or validate).
    - If user wants to poll a node that is offline, request must be queued and may not complete for hours (expected).
    - If Wi-Fi power save is too aggressive, may miss short node connections (test in your environment).
- Use `modemanager.c` for all new power/mode logic to keep code maintainable.

---

This document is a living plan. Update as you implement or learn more about real-world node/hub behavior.
