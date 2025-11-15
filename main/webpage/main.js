function moveTabUnderline() {
    var nav = document.querySelector('.nav');
    if (!nav) return;
    var active = nav.querySelector('.active');
    var underline = nav.querySelector('.tab-underline');
    if (active && underline) {
        underline.style.left = active.offsetLeft + 'px';
        underline.style.width = active.offsetWidth + 'px';
    }
}

window.addEventListener('DOMContentLoaded', moveTabUnderline);
window.addEventListener('resize', moveTabUnderline);

window.addEventListener('DOMContentLoaded', function () {
    updateFooter();
    setInterval(updateFooter, 1000);
});

// Footer updater
function updateFooter() {
    const now = new Date();
    const date = now.toLocaleDateString();
    const time = now.toLocaleTimeString();
    try {
        if (typeof ssid !== 'undefined') {
            document.getElementById('footer').textContent = 'On WLAN: ' + ssid + ', ' + date + ', ' + time;
        }
    } catch (_) { /* ignore */ }
}

// Persistence helpers for last seen/uptime across reloads
function loadPersistedState() {
    try {
        const nt = localStorage.getItem('nodeTimes');
        const kn = localStorage.getItem('knownNodes');
        const cn = localStorage.getItem('clearedNodes');
        if (nt && !window.nodeTimes) window.nodeTimes = JSON.parse(nt);
        if (kn && !window.knownNodes) window.knownNodes = JSON.parse(kn);
        if (cn && !window.clearedNodes) window.clearedNodes = JSON.parse(cn);
    } catch (_) { /* ignore */ }
}
function savePersistedState() {
    try {
        if (window.nodeTimes) localStorage.setItem('nodeTimes', JSON.stringify(window.nodeTimes));
        if (window.knownNodes) localStorage.setItem('knownNodes', JSON.stringify(window.knownNodes));
        if (window.clearedNodes) localStorage.setItem('clearedNodes', JSON.stringify(window.clearedNodes));
    } catch (_) { /* ignore */ }
}

// Distance section
function fetchDistance() {
    fetch('/distance').then(r => r.json()).then(j => {
        const distanceEl = document.getElementById('distance');
        const errorEl = document.getElementById('error');
        if (!distanceEl || !errorEl) return;
        distanceEl.textContent = j.distance + ' cm';
        if (j.error && j.error !== 0) {
            errorEl.textContent = 'Error: 0x' + j.error.toString(16).toUpperCase();
        } else {
            errorEl.textContent = '';
        }
    }).catch(() => {/* ignore */ });
}
fetchDistance();
setInterval(fetchDistance, 1000);

// Stats panel
let statsVisible = false;
let statsInterval = null;
function toggleStats() {
    statsVisible = !statsVisible;
    const panel = document.getElementById('statsPanel');
    const btn = document.getElementById('statsBtn');
    if (panel) panel.style.display = statsVisible ? 'block' : 'none';
    if (btn) btn.textContent = statsVisible ? 'Hide Device Stats' : 'Show Device Stats';
    if (statsVisible) {
        fetchStats();
        statsInterval = setInterval(fetchStats, 1000);
    } else {
        if (statsInterval) clearInterval(statsInterval);
        statsInterval = null;
    }
}

function fetchStats() {
    fetch('/stats').then(r => r.json()).then(j => {
        const heapEl = document.getElementById('statHeap');
        const minHeapEl = document.getElementById('statMinHeap');
        const uptimeEl = document.getElementById('statUptime');
        const cpuEl = document.getElementById('statCpuLoad');
        if (!heapEl || !minHeapEl || !uptimeEl || !cpuEl) return;
        var freeKB = (typeof j.free_heap === 'number') ? Math.round(j.free_heap / 1024) : '-';
        var minFreeKB = (typeof j.min_free_heap === 'number') ? Math.round(j.min_free_heap / 1024) : '-';
        heapEl.textContent = freeKB + ' KB';
        minHeapEl.textContent = minFreeKB + ' KB';
        let ms = j.uptime_ms; let sec = Math.floor(ms / 1000) % 60, min = Math.floor(ms / 60000) % 60, hr = Math.floor(ms / 3600000);
        uptimeEl.textContent = hr + 'h ' + min + 'm ' + sec + 's';
        cpuEl.textContent = Math.round(j.cpu_load * 100) + '%';
    }).catch(() => {/* ignore */ });
}

// Track open config panels per node
window.configPanelsOpen = {};

// Nodes panel
async function fetchNodes() {
    const panel = document.getElementById('nodesPanel');
    if (!panel) return;

    // Skip refresh if any config panels are open
    if (Object.values(window.configPanelsOpen).some(Boolean)) return;

    loadPersistedState();

    // Auto-cleanup: Remove nodes that have been offline for more than 24 hours
    const AUTO_CLEANUP_THRESHOLD_MS = 24 * 60 * 60 * 1000; // 24 hours
    const nowTs = Date.now();
    if (window.knownNodes) {
        for (const idStr of Object.keys(window.knownNodes)) {
            const id = parseInt(idStr, 10);
            const node = window.knownNodes[id];
            const nt = window.nodeTimes[id];
            if (node && node.status === 'Offline' && nt && nt.offlineSince) {
                const offlineDuration = nowTs - nt.offlineSince;
                if (offlineDuration > AUTO_CLEANUP_THRESHOLD_MS) {
                    delete window.knownNodes[id];
                    delete window.nodeTimes[id];
                }
            }
        }
        savePersistedState();
    }

    fetch('/nodeslist')
        .then(r => r.json())
        .then(async nodes => {
            if (!window.nodeTimes) window.nodeTimes = {};
            if (!window.knownNodes) window.knownNodes = {};
            if (!window.clearedNodes) window.clearedNodes = {};

            const nowTs = Date.now();
            const presentIds = new Set();
            const STALE_MS = 10000;

            if (Array.isArray(nodes)) {
                nodes.forEach(node => {
                    // Skip nodes that were manually cleared
                    if (window.clearedNodes[node.id]) {
                        // If a cleared node comes back online, remove it from cleared list
                        if (node.status === 'Online') {
                            delete window.clearedNodes[node.id];
                            savePersistedState();
                        } else {
                            // Still offline and cleared, skip it
                            return;
                        }
                    }

                    presentIds.add(node.id);
                    const nt = window.nodeTimes[node.id] || {};
                    if (!nt.firstSeen) nt.firstSeen = nowTs;
                    // Mark lastOnline on every fetch
                    nt.lastOnline = nowTs;
                    // Treat presence in the nodeslist as fresh activity: update lastDataTs
                    // so nodes that only connect (without immediate telemetry) don't flicker offline.
                    nt.lastDataTs = nowTs;
                    // Preserve lastUptime when telemetry changes
                    if (typeof node.uptime_s === 'number') {
                        if (nt.lastUptime !== node.uptime_s) {
                            nt.lastUptime = node.uptime_s;
                        }
                    }
                    // Clear any offlineSince marker since the node is present now
                    if (nt.offlineSince) delete nt.offlineSince;
                    window.nodeTimes[node.id] = nt;
                    window.knownNodes[node.id] = { ...node };
                });
            }

            const combined = [];
            // Fetch LUTs once per fetchNodes call so rendering can use them.
            // Use promise chaining instead of await to avoid await in non-async contexts.
            let _luts = null;
            if (!window._flut_cache_promise) {
                window._flut_cache_promise = fetch('/luts').then(r => r.json()).catch(() => null);
            }
            // Fetch door map (names) as well
            if (!window._door_map_cache_promise) {
                window._door_map_cache_promise = fetch('/door_map').then(r => r.json()).catch(() => null);
            }
            try {
                // resolve synchronously via then; this keeps the surrounding code non-async
                _luts = await window._flut_cache_promise;
            } catch (e) {
                _luts = null;
            }
            // Resolve door map into a simple array cache
            try {
                const _dm = await window._door_map_cache_promise;
                if (_dm && Array.isArray(_dm.doors)) window.doorMap = _dm.doors;
                else window.doorMap = null;
            } catch (e) {
                window.doorMap = null;
            }
            if (Array.isArray(nodes)) combined.push(...nodes);
            for (const idStr of Object.keys(window.knownNodes)) {
                const id = parseInt(idStr, 10);
                // Skip cleared nodes
                if (window.clearedNodes && window.clearedNodes[id]) {
                    continue;
                }
                if (!presentIds.has(id)) {
                    const last = window.knownNodes[id];
                    const nt = window.nodeTimes[id] || {};
                    if (!nt.offlineSince) nt.offlineSince = nowTs;
                    window.nodeTimes[id] = nt;
                    combined.push({ ...last, id, status: 'Offline', uptime_s: 0 });
                }
            }

            if (combined.length === 0) {
                panel.textContent = 'No nodes connected.';
                return;
            }

            // Sort: Online first, then by id
            combined.sort((a, b) => {
                const aOnline = a.status === 'Online';
                const bOnline = b.status === 'Online';
                if (aOnline !== bOnline) return aOnline ? -1 : 1;
                return (a.id || 0) - (b.id || 0);
            });

            // Build HTML
            const tempIcon = '<span class="icon" style="font-size:1.2em">🌡️</span>';
            const humidIcon = '<span class="icon" style="font-size:1.2em">💧</span>';
            const battIcon = '<span class="icon" style="font-size:1.2em">🔋</span>';
            const moistIcon = '<span class="icon" style="font-size:1.2em">🪴</span>';
            let html = '';
            let hasOffline = false;

            combined.forEach(node => {
                const nt = window.nodeTimes[node.id] || {};
                const isStale = nt.lastDataTs ? (nowTs - nt.lastDataTs) > STALE_MS : false;
                const isPresent = presentIds.has(node.id);
                const online = isPresent && (node.status === 'Online') && !isStale;
                const cardClass = online ? 'node-card' : 'node-card offline';
                const uptime = online && node.uptime_s ? formatUptime(node.uptime_s) : '-';

                if (!online && !nt.offlineSince) {
                    nt.offlineSince = nt.lastDataTs || nt.lastOnline || nowTs;
                    window.nodeTimes[node.id] = nt;
                }
                const lastSeenTs = nt.offlineSince || nt.lastDataTs || nt.lastOnline;
                const lastSeen = !online && lastSeenTs ? formatDateTime(lastSeenTs) : '-';
                const lastUptime = !online && nt.lastUptime ? formatUptime(nt.lastUptime) : '-';

                if (!online) hasOffline = true;

                html += `<div class='${cardClass}' data-nodeid='${node.id}'>`;
                html += `<div class='node-title'>Node: <span class='node-id'>${node.id ?? '-'}</span></div>`;
                if (online) {
                    // Show special status and grey out sensors if config panel is open for this node
                    if (window.configPanelsOpen[node.id]) {
                        html += `<div class='node-status online' style='color:#1976d2'>Online - waiting to reconfigure...</div>`;
                        html += `<div class='node-info' style='opacity:0.5; pointer-events:none;'>`;
                    } else {
                        html += `<div class='node-status online'>Online</div>`;
                        html += `<div class='node-info'>`;
                    }
                    html += `<span class=\"sensor\">${tempIcon}<span>${(node.temperature ?? '-') + (node.temperature != null ? '°C' : '')}</span></span>`;
                    html += ` <span class=\"sensor\">${humidIcon}<span>${(node.humidity ?? '-') + (node.humidity != null ? '%' : '')}</span></span>`;
                    const battVal = (node.battery !== undefined ? node.battery : (node.batt !== undefined ? node.batt : null));
                    html += ` <span class=\"sensor\">${battIcon}<span>${battVal != null ? (battVal + 'V') : '-'}</span></span>`;
                    html += ` <span class=\"sensor\">${moistIcon}<span>${node.moisture != null ? node.moisture : '-'}</span></span>`;
                    html += `</div>`;
                    html += `<div class='node-uptime'>Uptime: ${uptime}</div>`;

                    // --- Lifecycle tracking info ---
                    // Only display anomaly info if we've received at least one anomaly
                    if (node.anomaly_count != null && node.anomaly_count > 0) {
                        const reason = node.last_anomaly_reason || 'unknown';
                        const refreshes = node.last_anomaly_refreshes || 0;

                        // Format reason for display
                        let reasonText = reason.replace(/_/g, ' ');
                        reasonText = reasonText.charAt(0).toUpperCase() + reasonText.slice(1);

                        // Severity indicator based on refresh count
                        let severity = '⚠️';
                        let severityClass = 'lifecycle-anomaly';
                        if (refreshes > 30) {
                            severity = '🔴';
                            severityClass = 'lifecycle-anomaly critical';
                        } else if (refreshes > 20) {
                            severity = '⚠️';
                            severityClass = 'lifecycle-anomaly warning';
                        }

                        html += `<div class='${severityClass}'>${severity} `;
                        if (node.anomaly_count === 1) {
                            html += `Device stayed awake ${refreshes}× longer than expected`;
                        } else {
                            html += `${node.anomaly_count} anomalies detected`;
                        }
                        html += `<div class='lifecycle-detail'>Cause: ${reasonText} (${refreshes} activity refreshes)</div>`;
                        html += `</div>`;
                    }

                    // Only display wake info if we've received at least one wake event
                    if (node.last_wake_source != null && node.last_wake_source !== '') {
                        const source = node.last_wake_source;
                        const prevSleep = node.prev_sleep || 0;
                        const prevUp = node.prev_uptime || 0;

                        // Format wake source
                        let sourceIcon = '💤';
                        let sourceText = source.replace(/_/g, ' ');
                        sourceText = sourceText.charAt(0).toUpperCase() + sourceText.slice(1);

                        // Choose icon based on source
                        if (source.includes('door')) sourceIcon = '🚪';
                        else if (source.includes('button')) sourceIcon = '🔘';
                        else if (source.includes('scheduled')) sourceIcon = '⏰';
                        else if (source.includes('ws_message')) sourceIcon = '📡';
                        else if (source.includes('boot')) sourceIcon = '🔌';

                        // Format durations
                        const formatDuration = (seconds) => {
                            if (seconds === 0) return 'N/A';
                            if (seconds < 60) return `${seconds}s`;
                            const mins = Math.floor(seconds / 60);
                            const secs = seconds % 60;
                            return secs > 0 ? `${mins}m ${secs}s` : `${mins}m`;
                        };

                        html += `<div class='lifecycle-info'>${sourceIcon} Woke from ${sourceText}`;
                        if (prevSleep > 0 || prevUp > 0) {
                            html += ` • Slept ${formatDuration(prevSleep)} • Previously up ${formatDuration(prevUp)}`;
                        }
                        html += `</div>`;
                    }

                    // --- Event / sporadic rows (doors, alerts, etc.) ---
                    try {
                        const sporadic = node.sporadic || {};
                        // Flatten sporadic entries: support value forms like
                        // { doorsense: ["UNKNOWN","OPEN"] } or { doorsense_0: "OPEN" }
                        const flat = {};
                        Object.keys(sporadic).forEach(k => {
                            const v = sporadic[k];
                            if (v === null || typeof v === 'undefined') return;
                            if (Array.isArray(v)) {
                                for (let i = 0; i < v.length; ++i) {
                                    if (v[i] === null || typeof v[i] === 'undefined') continue;
                                    flat[`${k}_${i}`] = v[i];
                                }
                            } else if (typeof v === 'object') {
                                // If it's an object, flatten its properties as k_subkey
                                Object.keys(v).forEach(sub => {
                                    const sv = v[sub];
                                    if (sv === null || typeof sv === 'undefined') return;
                                    flat[`${k}_${sub}`] = sv;
                                });
                            } else {
                                flat[k] = v;
                            }
                        });

                        const sKeys = Object.keys(flat);
                        if (sKeys.length > 0) {
                            // Group keys by prefix (e.g., 'doorsense_0' -> 'doorsense')
                            const grouped = {};
                            sKeys.forEach(k => {
                                const cat = k.indexOf('_') > 0 ? k.split('_')[0] : k;
                                if (!grouped[cat]) grouped[cat] = [];
                                grouped[cat].push(k);
                            });

                            const formatEventValue = (v) => {
                                if (typeof v === 'number') return (v === 1) ? 'OPEN' : (v === 0 ? 'CLOSED' : String(v));
                                if (typeof v === 'boolean') return v ? 'OPEN' : 'CLOSED';
                                if (typeof v === 'string') return v.toUpperCase();
                                return String(v);
                            };

                            // Render each category as its own row
                            Object.keys(grouped).forEach(cat => {
                                const keys = grouped[cat];
                                if (cat === 'doorsense') {
                                    // Special rendering for doors - one line per door with icon
                                    const doorIcon = '🚪';

                                    // Determine which doors are configured (have mappings) vs just available
                                    // Show all configured doors even if no event received yet
                                    const maxDoorIndex = Math.max(...keys.map(k => {
                                        const m = k.match(/^doorsense_(\d+)$/);
                                        return m ? parseInt(m[1], 10) : -1;
                                    }).filter(idx => idx >= 0));

                                    // Build a set of doors we've received events for
                                    const receivedDoors = new Set();
                                    keys.forEach(k => {
                                        const m = k.match(/^doorsense_(\d+)$/);
                                        if (m) receivedDoors.add(parseInt(m[1], 10));
                                    });

                                    // Only show doors that have actually received events
                                    const doorIndicesToShow = Array.from(receivedDoors).sort((a, b) => a - b);

                                    doorIndicesToShow.forEach(idx => {
                                        const mappedName = (window.doorMap && window.doorMap[idx] && window.doorMap[idx].trim())
                                            ? window.doorMap[idx].trim()
                                            : null;
                                        const friendly = mappedName || ('Door ' + idx);
                                        const k = `doorsense_${idx}`;

                                        // We have received an event for this door - show definitive state
                                        const state = formatEventValue(flat[k]);
                                        const stateColor = (state === 'OPEN') ? '#f57c00' : '#388e3c';
                                        const stateIcon = (state === 'OPEN') ? '🔓' : '🔒';
                                        html += `<div class='door-status' style='margin-top:6px; padding:6px 10px; background:#f8f9fa; border-radius:6px; display:flex; align-items:center; justify-content:space-between;'>
                                                    <span style='display:flex; align-items:center; gap:8px;'>
                                                        <span style='font-size:1.2em;'>${doorIcon}</span>
                                                        <span style='font-weight:500; color:#333;'>${friendly}</span>
                                                    </span>
                                                    <span style='display:flex; align-items:center; gap:6px; font-weight:600; color:${stateColor};'>
                                                        <span style='font-size:1.1em;'>${stateIcon}</span>
                                                        <span>${state}</span>
                                                    </span>
                                                </div>`;
                                    });
                                } else {
                                    // Other event categories - single line with title
                                    const title = lutNameToLabel(cat);
                                    const parts = keys.map(k => {
                                        return `${k} = ${formatEventValue(flat[k])}`;
                                    });
                                    html += `<div class='event-row event-${cat}'>${title}: ${parts.join(', ')}</div>`;
                                }
                            });
                        }
                    } catch (e) { /* ignore render errors */ }
                } else {
                    html += `<div class='node-status offline'>Offline</div>`;
                    html += `<div class='node-lastseen'>Last seen: <span style='color:#d32f2f'>${lastSeen}</span></div>`;
                    html += `<div class='node-uptime'>Last Uptime: ${lastUptime}</div>`;
                }
                if (online) {
                    // Configure expander and View Logs button
                    html += `<div style='display:flex; gap:8px; margin-top:12px;'>`;
                    html += `<button class='node-config-toggle' data-node='${node.id}'>Configure</button>`;
                    html += `<button class='node-logs-btn' data-node='${node.id}'>View Logs</button>`;
                    html += `</div>`;
                    // Inline config panel (hidden by default)
                    const capMask = Number(node.cap_mask || 0);
                    const subMask = Number(node.sub && node.sub.mask ? node.sub.mask : 0);
                    const intval = Math.round(Number(node.sub && node.sub.interval_ms ? node.sub.interval_ms : 5000) / 1000); // seconds
                    let sensorsHtml = '';
                    let servicesHtml = '';
                    try {
                        const luts = _luts;
                        if (luts) {
                            const periodic = (luts.sensors || []).filter(s => s.loc === 0);
                            const eventdriven = (luts.sensors || []).filter(s => s.loc !== 0);
                            const sv = (luts.services || []);

                            // Render checkboxes for LUT entries. Show entries even if the node
                            // doesn't advertise the capability; mark them disabled so user
                            // understands what's available globally but not supported by this node.
                            const renderCheckbox = (entry, isService) => {
                                const bitIndex = Math.log2(entry.cap_mask) | 0; // cap_mask is power of two
                                const bitMask = (1 << bitIndex) >>> 0;
                                const supported = !!(capMask & bitMask);
                                const checked = (subMask & bitMask) ? 'checked' : '';
                                const disabled = supported ? '' : 'disabled';
                                const name = lutNameToLabel(entry.name);
                                const cls = supported ? '' : 'cap-unsupported';
                                return `<label class='${cls}'><input class='cap-checkbox' data-node='${node.id}' data-bit='${bitIndex}' type='checkbox' ${checked} ${disabled}> ${name}</label>`;
                            };

                            if (periodic.length > 0) {
                                sensorsHtml += `<div class='cap-subtitle'>Periodic</div>`;
                                periodic.forEach(e => { sensorsHtml += renderCheckbox(e, false); });
                            }
                            if (eventdriven.length > 0) {
                                sensorsHtml += `<div class='cap-subtitle'>Event driven</div>`;
                                eventdriven.forEach(e => { sensorsHtml += renderCheckbox(e, false); });
                            }

                            // Split services into periodic vs event-driven if LUT provides loc
                            const sv_periodic = (sv || []).filter(s => (s.loc === 0));
                            const sv_event = (sv || []).filter(s => (s.loc !== 0));
                            if (sv_periodic.length > 0) {
                                servicesHtml += `<div class='cap-subtitle'>Periodic</div>`;
                                sv_periodic.forEach(e => { servicesHtml += renderCheckbox(e, true); });
                            }
                            if (sv_event.length > 0) {
                                servicesHtml += `<div class='cap-subtitle'>Event driven services</div>`;
                                sv_event.forEach(e => { servicesHtml += renderCheckbox(e, true); });
                            }
                        }
                    } catch (e) {
                        sensorsHtml = '<span style="color:#888">None</span>';
                        servicesHtml = '<span style="color:#888">None</span>';
                    }
                    const maskHex = '0x' + (subMask >>> 0).toString(16);
                    // A more compact, clearer subscription/config panel
                    html += `<div class='node-config' id='cfg-${node.id}'>
                                                             <div class='cap-groups two-column'>
                                                                 <div class='cap-column'>
                                                                     <div class='cap-group-title'>Sensors</div>
                                                                     <div class='cap-list compact'>${sensorsHtml || '<span style="color:#888">None</span>'}</div>
                                                                 </div>
                                                                 <div class='cap-column'>
                                                                     <div class='cap-group-title'>Services</div>
                                                                     <div class='cap-list compact'>${servicesHtml || '<span style="color:#888">None</span>'}</div>
                                                                 </div>
                                                             </div>
                                                             <div class='cfg-row'>
                                                                 <label class='cfg-interval'>Data Interval (s): <input class='interval-input' type='number' min='1' max='60' step='1' value='${intval}' data-node='${node.id}'></label>
                                                                 <div class='mask-row'>Mask: <span id='mask-${node.id}' class='mask-val'>${maskHex.toUpperCase()}</span></div>
                                                             </div>
                                                             <div class='cfg-help' style='color:#666; font-size:0.9em; margin:8px 0;'>Select which sensors/services the node should report. Event-driven sensors (like door_state) are sent as events; periodic sensors are included in telemetry.</div>
                                                             <div class='cfg-actions'><button class='apply-sub' data-node='${node.id}'>Apply</button><span class='apply-status' id='status-${node.id}' style='margin-left:10px; font-size:0.95em; color:#1976d2; display:none;'>Applying...</span></div>
                                                         </div>`;
                }
                html += `</div>`;
            });

            panel.innerHTML = html;
            // Add clear offline button if needed
            if (hasOffline) {
                html += `<button id='clearOfflineBtn' style='margin:18px auto 0 auto; display:block; background:#e3eaf3; color:#1976d2; border-radius:6px; border:none; font-size:0.98em; padding:7px 18px; box-shadow:0 1px 4px #0001; cursor:pointer;'>Clear offline cards</button>`;
            }
            panel.innerHTML = html;

            // Wire up toggles and live mask computation
            panel.querySelectorAll('.node-config-toggle').forEach(btn => {
                btn.addEventListener('click', () => {
                    const id = btn.getAttribute('data-node');
                    const cfg = document.getElementById('cfg-' + id);
                    if (cfg) {
                        const isOpening = cfg.style.display !== 'block';
                        cfg.style.display = isOpening ? 'block' : 'none';
                        // Track open/close per node
                        window.configPanelsOpen[id] = isOpening;
                        // Immediately re-render node cards to update status and sensor area
                        fetchNodes();
                    }
                });
            });

            const recomputeMask = (nodeId) => {
                const boxes = panel.querySelectorAll(`.cap-checkbox[data-node='${nodeId}']`);
                let mask = 0 >>> 0;
                boxes.forEach(b => { if (b.checked) { const bit = parseInt(b.getAttribute('data-bit'), 10); mask = (mask | (1 << bit)) >>> 0; } });
                const span = document.getElementById('mask-' + nodeId);
                if (span) span.textContent = '0x' + (mask >>> 0).toString(16);
                return mask >>> 0;
            };

            panel.querySelectorAll('.cap-checkbox').forEach(cb => {
                cb.addEventListener('change', () => {
                    const id = cb.getAttribute('data-node');
                    recomputeMask(id);
                });
            });

            panel.querySelectorAll('.apply-sub').forEach(btn => {
                btn.addEventListener('click', () => {
                    const id = btn.getAttribute('data-node');
                    const statusEl = document.getElementById('status-' + id);
                    const mask = recomputeMask(id);
                    const intervalEl = panel.querySelector(`.interval-input[data-node='${id}']`);
                    const intervalSec = intervalEl ? parseInt(intervalEl.value || '5', 10) : 5;
                    const intervalMs = intervalSec * 1000;
                    const body = `node_id=${encodeURIComponent(id)}&mask=${mask}&interval_ms=${intervalMs}`;
                    // Disable button and show status
                    btn.disabled = true;
                    if (statusEl) { statusEl.style.display = 'inline'; statusEl.textContent = 'Applying...'; }
                    fetch('/subscribe', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body })
                        .then(r => r.json()).then(_ => {
                            // collapse panel and allow refresh
                            const cfg = document.getElementById('cfg-' + id);
                            if (cfg) cfg.style.display = 'none';
                            window.configPanelsOpen[id] = false;
                            // trigger immediate refresh
                            fetchNodes();
                        }).catch(() => {/* ignore */ })
                        .finally(() => {
                            btn.disabled = false;
                            if (statusEl) { statusEl.style.display = 'none'; }
                        });
                });
            });

            // Wire up clear offline button
            const clearBtn = panel.querySelector('#clearOfflineBtn');
            if (clearBtn) {
                clearBtn.addEventListener('click', () => {
                    // Initialize clearedNodes if needed
                    if (!window.clearedNodes) window.clearedNodes = {};

                    // Find all offline cards and get their node IDs
                    const offlineCards = Array.from(panel.querySelectorAll('.node-card.offline'));

                    offlineCards.forEach(card => {
                        const nodeId = parseInt(card.dataset.nodeid, 10);
                        if (!isNaN(nodeId)) {
                            window.clearedNodes[nodeId] = true;
                            delete window.knownNodes[nodeId];
                            delete window.nodeTimes[nodeId];
                        }
                    });

                    savePersistedState();

                    // Animate cards away sequentially
                    let i = 0;
                    function swipeNext() {
                        if (i >= offlineCards.length) {
                            return;
                        }
                        const card = offlineCards[i];
                        card.classList.add('swipe-away');
                        setTimeout(() => {
                            card.remove();
                            i++;
                            swipeNext();
                        }, 120);
                    }
                    swipeNext();
                });
            }

            // Wire up View Logs buttons
            panel.querySelectorAll('.node-logs-btn').forEach(btn => {
                btn.addEventListener('click', () => {
                    const nodeId = btn.getAttribute('data-node');
                    openLogsModal(nodeId);
                });
            });

            savePersistedState();
        })
        .catch(() => {
            panel.textContent = 'Failed to load node data.';
        });
}

// Helper function to update the global flag based on visible config panels
// No longer needed: configPanelsOpen is now per-node

function formatUptime(seconds) {
    seconds = Math.floor(seconds);
    let h = Math.floor(seconds / 3600);
    let m = Math.floor((seconds % 3600) / 60);
    let s = seconds % 60;
    return `${h}h ${m}m ${s}s`;
}

function formatDateTime(dt) {
    let d = new Date(dt);
    if (isNaN(d)) return String(dt);
    return d.toLocaleDateString() + ', ' + d.toLocaleTimeString();
}

// Helper to make a human-friendly label from LUT name
function lutNameToLabel(name) {
    // explicit overrides for special cases
    const overrides = {
        'doorsense': 'Door state'
    };
    if (overrides[name]) return overrides[name];

    // replace underscores with spaces and Title Case each word
    return name.split('_').map(function (part) {
        return part.charAt(0).toUpperCase() + part.slice(1);
    }).join(' ');
}

// Periodically update nodesPanel if present
function setupNodesPolling() {
    if (document.getElementById('nodesPanel')) {
        fetchNodes();
        setInterval(fetchNodes, 2000);
    }
}
window.addEventListener('DOMContentLoaded', setupNodesPolling);

// Keep footer ticking too
updateFooter();
setInterval(updateFooter, 1000);

// Light sleep banner updater
function updateSleepBanner() {
    try {
        fetch('/sleepstatus')
            .then(r => r.json())
            .then(j => {
                const banner = document.getElementById('sleepBanner');
                if (!banner) return;
                if (j && j.light_sleep) {
                    banner.style.display = 'block';
                } else {
                    banner.style.display = 'none';
                }
            }).catch(() => { /* ignore */ });
    } catch (_) { /* ignore */ }
}
updateSleepBanner();
setInterval(updateSleepBanner, 1000);

// ========== Remote Logging Modal ==========

// Create logs modal on first use
function ensureLogsModal() {
    let modal = document.getElementById('logsModal');
    if (modal) return modal;

    // Create modal structure
    modal = document.createElement('div');
    modal.id = 'logsModal';
    modal.className = 'logs-modal';
    modal.innerHTML = `
        <div class='logs-modal-content'>
            <div class='logs-modal-header'>
                <h3 id='logsModalTitle'>Node Logs</h3>
                <button class='logs-modal-close' id='closeLogsModal'>&times;</button>
            </div>
            <div class='logs-modal-body'>
                <div id='logsLoadingStatus' style='text-align:center; padding:20px; color:#666;'>
                    <span style='font-size:1.2em;'>⏳</span> Requesting logs...
                </div>
                <div id='logsContent' style='display:none;'></div>
            </div>
            <div class='logs-modal-footer'>
                <button id='refreshLogsBtn'>Refresh</button>
                <button id='exportLogsBtn'>Export</button>
            </div>
        </div>
    `;
    document.body.appendChild(modal);

    // Wire up close button
    document.getElementById('closeLogsModal').addEventListener('click', closeLogsModal);
    modal.addEventListener('click', (e) => {
        if (e.target === modal) closeLogsModal();
    });

    return modal;
}

function openLogsModal(nodeId) {
    const modal = ensureLogsModal();
    const title = document.getElementById('logsModalTitle');
    const loadingStatus = document.getElementById('logsLoadingStatus');
    const logsContent = document.getElementById('logsContent');

    // Store current node ID for refresh/export
    modal.dataset.nodeId = nodeId;

    // Update title
    title.textContent = `Node ${nodeId} Logs`;

    // Show loading, hide content
    loadingStatus.style.display = 'block';
    logsContent.style.display = 'none';
    logsContent.innerHTML = '';

    // Show modal
    modal.style.display = 'flex';

    // Request logs
    requestNodeLogs(nodeId);

    // Wire up refresh button
    const refreshBtn = document.getElementById('refreshLogsBtn');
    refreshBtn.onclick = () => {
        loadingStatus.style.display = 'block';
        logsContent.style.display = 'none';
        requestNodeLogs(nodeId);
    };

    // Wire up export button
    const exportBtn = document.getElementById('exportLogsBtn');
    exportBtn.onclick = () => exportLogs(nodeId);
}

function closeLogsModal() {
    const modal = document.getElementById('logsModal');
    if (modal) modal.style.display = 'none';
}

function requestNodeLogs(nodeId, maxLines = 100) {
    const loadingStatus = document.getElementById('logsLoadingStatus');
    const logsContent = document.getElementById('logsContent');

    fetch(`/api/request_logs?node_id=${nodeId}&max_lines=${maxLines}`, { method: 'POST' })
        .then(r => r.json())
        .then(data => {
            if (data.status === 'ok') {
                loadingStatus.innerHTML = `<span style='color:#666;'>Waiting for response from Node ${nodeId}...</span>`;
                // Poll for logs (hub stores them temporarily)
                pollForLogs(nodeId);
            } else {
                throw new Error(data.message || 'Request failed');
            }
        })
        .catch(err => {
            loadingStatus.innerHTML = `<span style='color:#d32f2f;'>❌ Error: ${err.message}</span>`;
            setTimeout(() => {
                loadingStatus.style.display = 'none';
            }, 3000);
        });
}

function pollForLogs(nodeId, attempts = 0) {
    const maxAttempts = 20; // 10 seconds total (500ms intervals)
    const loadingStatus = document.getElementById('logsLoadingStatus');
    const logsContent = document.getElementById('logsContent');

    if (attempts >= maxAttempts) {
        loadingStatus.innerHTML = `<span style='color:#f57c00;'>⚠️ Timeout waiting for logs. Node may be offline or busy.</span>`;
        return;
    }

    // Poll the hub for stored logs
    fetch(`/api/get_node_logs?node_id=${nodeId}`)
        .then(r => r.json())
        .then(data => {
            if (data.status === 'available' && data.logs && data.logs.length > 0) {
                displayNodeLogs(nodeId, data.logs, data.total_lines);
            } else if (data.status === 'pending') {
                // Still waiting for node response, poll again
                setTimeout(() => pollForLogs(nodeId, attempts + 1), 500);
            } else {
                // No logs yet, poll again
                setTimeout(() => pollForLogs(nodeId, attempts + 1), 500);
            }
        })
        .catch(err => {
            loadingStatus.innerHTML = `<span style='color:#d32f2f;'>❌ Error fetching logs: ${err.message}</span>`;
        });
}

function displayNodeLogs(nodeId, logs, totalLines) {
    const loadingStatus = document.getElementById('logsLoadingStatus');
    const logsContent = document.getElementById('logsContent');

    loadingStatus.style.display = 'none';
    logsContent.style.display = 'block';

    let html = '<div class="logs-list">';

    logs.forEach((log, idx) => {
        const lineText = log.line || 'Empty log line';

        // Simple display without color coding - just show the line as-is
        html += `<div class='log-entry log-info'>
                    <span class='log-index'>#${idx + 1}</span>
                    <span class='log-message'>${escapeHtml(lineText)}</span>
                 </div>`;
    });

    html += '</div>';
    html += `<div style='margin-top:12px; padding:8px; background:#f0f4fa; border-radius:4px; text-align:center; font-size:0.9em; color:#666;'>
                Showing ${logs.length} of ${totalLines} total lines
             </div>`;

    logsContent.innerHTML = html;
}

function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

function exportLogs(nodeId) {
    const logsContent = document.getElementById('logsContent');
    const logEntries = logsContent.querySelectorAll('.log-entry');

    let text = `Node ${nodeId} Logs - Exported ${new Date().toLocaleString()}\n`;
    text += '='.repeat(60) + '\n\n';

    logEntries.forEach((entry, idx) => {
        const message = entry.querySelector('.log-message').textContent;
        text += `[${idx + 1}] ${message}\n`;
    });

    // Create download
    const blob = new Blob([text], { type: 'text/plain' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `node_${nodeId}_logs_${Date.now()}.txt`;
    a.click();
    URL.revokeObjectURL(url);
}

// Close modal with Escape key
document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') closeLogsModal();
});