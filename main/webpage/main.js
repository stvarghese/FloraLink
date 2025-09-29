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

// Persistence helpers for last seen/uptime across reloads
function loadPersistedState() {
    try {
        const nt = localStorage.getItem('nodeTimes');
        const kn = localStorage.getItem('knownNodes');
        if (nt && !window.nodeTimes) window.nodeTimes = JSON.parse(nt);
        if (kn && !window.knownNodes) window.knownNodes = JSON.parse(kn);
    } catch (_) { /* ignore */ }
}
function savePersistedState() {
    try {
        if (window.nodeTimes) localStorage.setItem('nodeTimes', JSON.stringify(window.nodeTimes));
        if (window.knownNodes) localStorage.setItem('knownNodes', JSON.stringify(window.knownNodes));
    } catch (_) { /* ignore */ }
}

function fetchDistance() {
    fetch('/distance').then(r => r.json()).then(j => {
        document.getElementById('distance').textContent = j.distance + ' cm';
        if (j.error && j.error !== 0) {
            document.getElementById('error').textContent = 'Error: 0x' + j.error.toString(16).toUpperCase();
        } else {
            document.getElementById('error').textContent = '';
        }
    });
}

function updateFooter() {
    const now = new Date();
    const date = now.toLocaleDateString();
    const time = now.toLocaleTimeString();
    document.getElementById('footer').textContent = 'On WLAN: ' + ssid + ', ' + date + ', ' + time;
}

let statsVisible = false;
let statsInterval = null;
function toggleStats() {
    statsVisible = !statsVisible;
    document.getElementById('statsPanel').style.display = statsVisible ? 'block' : 'none';
    document.getElementById('statsBtn').textContent = statsVisible ? 'Hide Device Stats' : 'Show Device Stats';
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
        var freeKB = (typeof j.free_heap === 'number') ? Math.round(j.free_heap / 1024) : '-';
        var minFreeKB = (typeof j.min_free_heap === 'number') ? Math.round(j.min_free_heap / 1024) : '-';
        document.getElementById('statHeap').textContent = freeKB + ' KB';
        document.getElementById('statMinHeap').textContent = minFreeKB + ' KB';
        let ms = j.uptime_ms; let sec = Math.floor(ms / 1000) % 60, min = Math.floor(ms / 60000) % 60, hr = Math.floor(ms / 3600000);
        document.getElementById('statUptime').textContent = hr + 'h ' + min + 'm ' + sec + 's';
        document.getElementById('statCpuLoad').textContent = Math.round(j.cpu_load * 100) + '%';
    });
}

function fetchNodes() {
    const panel = document.getElementById('nodesPanel');
    if (!panel) return;
    // Load any persisted state once
    loadPersistedState();
    fetch('/nodeslist')
        .then(r => r.json())
        .then(nodes => {
            // Ensure state holders exist
            if (!window.nodeTimes) window.nodeTimes = {};
            if (!window.knownNodes) window.knownNodes = {};

            const nowTs = Date.now();
            const presentIds = new Set();

            const STALE_MS = 10000; // consider node offline if no data progress for 10s

            if (Array.isArray(nodes)) {
                nodes.forEach(node => {
                    presentIds.add(node.id);
                    // Update last seen/uptime while online
                    const nt = window.nodeTimes[node.id] || {};
                    if (!nt.firstSeen) nt.firstSeen = nowTs;
                    nt.lastOnline = nowTs;
                    // Track uptime changes to detect staleness
                    if (typeof node.uptime_s === 'number') {
                        if (nt.lastUptime !== node.uptime_s) {
                            nt.lastUptime = node.uptime_s;
                            nt.lastDataTs = nowTs; // data progressed
                            if (nt.offlineSince) delete nt.offlineSince; // back online
                        } else if (!nt.lastDataTs) {
                            // initialize once
                            nt.lastDataTs = nowTs;
                        }
                    }
                    // Node is online again; clear any prior offline marker
                    if (nt.offlineSince) delete nt.offlineSince;
                    window.nodeTimes[node.id] = nt;
                    // Remember the latest payload for rendering
                    window.knownNodes[node.id] = { ...node };
                });
            }

            // Build combined list: online first (from current fetch), then offline (previously known but missing)
            const combined = [];
            if (Array.isArray(nodes)) combined.push(...nodes);
            for (const idStr of Object.keys(window.knownNodes)) {
                const id = parseInt(idStr, 10);
                if (!presentIds.has(id)) {
                    const last = window.knownNodes[id];
                    // Create a shallow copy and mark offline
                    // Set offlineSince once
                    const nt = window.nodeTimes[id] || {};
                    if (!nt.offlineSince) nt.offlineSince = nowTs;
                    window.nodeTimes[id] = nt;
                    // Important: spread last first, then override with Offline fields
                    combined.push({ ...last, id, status: 'Offline', uptime_s: 0 });
                }
            }

            if (combined.length === 0) {
                // Nothing ever seen
                panel.textContent = 'No nodes connected.';
                return;
            }

            // Render
            let html = '';
            const tempIcon = '<span style="font-size:1.2em">🌡️</span>';
            const humidIcon = '<span style="font-size:1.2em">💧</span>';
            const battIcon = '<span style="font-size:1.2em">🔋</span>';
            const moistIcon = '<span style="font-size:1.2em">🪴</span>';

            // Online first, then by id ascending
            combined.sort((a, b) => {
                const aOnline = a.status === 'Online';
                const bOnline = b.status === 'Online';
                if (aOnline !== bOnline) return aOnline ? -1 : 1;
                return (a.id || 0) - (b.id || 0);
            });
            combined.forEach(node => {
                const nt = window.nodeTimes[node.id] || {};
                const isStale = nt.lastDataTs ? (nowTs - nt.lastDataTs) > STALE_MS : false;
                const isPresent = presentIds.has(node.id);
                // Only treat as online if the node is present in the latest payload and not stale
                const online = isPresent && (node.status === 'Online') && !isStale;
                const cardClass = online ? 'node-card' : 'node-card offline';
                const uptime = online && node.uptime_s ? formatUptime(node.uptime_s) : '-';
                // Set offlineSince when we first notice staleness for present nodes
                if (!online && !nt.offlineSince) {
                    nt.offlineSince = nt.lastDataTs || nt.lastOnline || nowTs;
                    window.nodeTimes[node.id] = nt;
                }
                const lastSeenTs = nt.offlineSince || nt.lastDataTs || nt.lastOnline;
                const lastSeen = !online && lastSeenTs ? formatDateTime(lastSeenTs) : '-';
                const lastUptime = !online && nt.lastUptime ? formatUptime(nt.lastUptime) : '-';

                html += `<div class='${cardClass}'>`;
                html += `<div class='node-title'>Node: <span class='node-id'>${node.id ?? '-'}</span></div>`;
                html += `<div class='node-info'>`;
                // Use backend field names: temperature, humidity, moisture
                html += `${tempIcon} ${node.temperature !== undefined && node.temperature !== null ? node.temperature + '°C' : '-'} &nbsp;`;
                html += `${humidIcon} ${node.humidity !== undefined && node.humidity !== null ? node.humidity + '%' : '-'} &nbsp;`;
                html += `${battIcon} ${node.battery !== undefined ? node.battery + 'V' : (node.batt !== undefined ? node.batt + 'V' : '-')} &nbsp;`;
                html += `${moistIcon} ${node.moisture !== undefined && node.moisture !== null ? node.moisture + '' : '-'} &nbsp;`;
                html += `${online ? '<span class="node-status online">Online</span>' : '<span class="node-status offline">Offline</span>'}`;
                html += `</div>`;
                if (online) {
                    html += `<div class='node-uptime'>Uptime: ${uptime}</div>`;
                } else {
                    html += `<div class='node-lastseen'>Offline since: <span style='color:#d32f2f'>${lastSeen}</span></div>`;
                    html += `<div class='node-uptime'>Last Uptime: ${lastUptime}</div>`;
                }
                html += `</div>`;
            });
            panel.innerHTML = html;
            // Persist updates
            savePersistedState();
        })
        .catch(() => {
            panel.textContent = 'Failed to load node data.';
        });
}

function formatUptime(seconds) {
    seconds = Math.floor(seconds);
    let h = Math.floor(seconds / 3600);
    let m = Math.floor((seconds % 3600) / 60);
    let s = seconds % 60;
    return `${h}h ${m}m ${s}s`;
}
function formatDateTime(dtStr) {
    // dtStr: ISO string or timestamp
    let d = new Date(dtStr);
    if (isNaN(d)) return dtStr;
    return d.toLocaleDateString() + ', ' + d.toLocaleTimeString();
}

// Periodically update nodesPanel if present
function setupNodesPolling() {
    if (document.getElementById('nodesPanel')) {
        fetchNodes();
        setInterval(fetchNodes, 2000);
    }
}
window.addEventListener('DOMContentLoaded', setupNodesPolling);

fetchDistance();
setInterval(fetchDistance, 1000);
updateFooter();
setInterval(updateFooter, 1000);
