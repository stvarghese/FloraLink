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

// Nodes panel
function fetchNodes() {
    const panel = document.getElementById('nodesPanel');
    if (!panel) return;
    loadPersistedState();
    fetch('/nodeslist')
        .then(r => r.json())
        .then(nodes => {
            if (!window.nodeTimes) window.nodeTimes = {};
            if (!window.knownNodes) window.knownNodes = {};

            const nowTs = Date.now();
            const presentIds = new Set();
            const STALE_MS = 10000;

            if (Array.isArray(nodes)) {
                nodes.forEach(node => {
                    presentIds.add(node.id);
                    const nt = window.nodeTimes[node.id] || {};
                    if (!nt.firstSeen) nt.firstSeen = nowTs;
                    nt.lastOnline = nowTs;
                    if (typeof node.uptime_s === 'number') {
                        if (nt.lastUptime !== node.uptime_s) {
                            nt.lastUptime = node.uptime_s;
                            nt.lastDataTs = nowTs;
                            if (nt.offlineSince) delete nt.offlineSince;
                        } else if (!nt.lastDataTs) {
                            nt.lastDataTs = nowTs;
                        }
                    }
                    if (nt.offlineSince) delete nt.offlineSince;
                    window.nodeTimes[node.id] = nt;
                    window.knownNodes[node.id] = { ...node };
                });
            }

            const combined = [];
            if (Array.isArray(nodes)) combined.push(...nodes);
            for (const idStr of Object.keys(window.knownNodes)) {
                const id = parseInt(idStr, 10);
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

                html += `<div class='${cardClass}'>`;
                html += `<div class='node-title'>Node: <span class='node-id'>${node.id ?? '-'}</span></div>`;
                if (online) {
                    html += `<div class='node-status online'>Online</div>`;
                    html += `<div class='node-info'>`;
                    html += `<span class=\"sensor\">${tempIcon}<span>${(node.temperature ?? '-') + (node.temperature != null ? '°C' : '')}</span></span>`;
                    html += ` <span class=\"sensor\">${humidIcon}<span>${(node.humidity ?? '-') + (node.humidity != null ? '%' : '')}</span></span>`;
                    const battVal = (node.battery !== undefined ? node.battery : (node.batt !== undefined ? node.batt : null));
                    html += ` <span class=\"sensor\">${battIcon}<span>${battVal != null ? (battVal + 'V') : '-'}</span></span>`;
                    html += ` <span class=\"sensor\">${moistIcon}<span>${node.moisture != null ? node.moisture : '-'}</span></span>`;
                    html += `</div>`;
                    html += `<div class='node-uptime'>Uptime: ${uptime}</div>`;
                } else {
                    html += `<div class='node-status offline'>Offline</div>`;
                    html += `<div class='node-lastseen'>Last seen: <span style='color:#d32f2f'>${lastSeen}</span></div>`;
                    html += `<div class='node-uptime'>Last Uptime: ${lastUptime}</div>`;
                }
                html += `</div>`;
            });

            panel.innerHTML = html;
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
function formatDateTime(dt) {
    let d = new Date(dt);
    if (isNaN(d)) return String(dt);
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

// Keep footer ticking too
updateFooter();
setInterval(updateFooter, 1000);
