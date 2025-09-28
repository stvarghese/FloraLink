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
    fetch('/nodeslist')
        .then(r => r.json())
        .then(nodes => {
            if (!Array.isArray(nodes) || nodes.length === 0) {
                panel.textContent = 'No nodes connected.';
                return;
            }
            let html = '';
            nodes.forEach(node => {
                // Icons: thermometer, droplet, battery
                const tempIcon = '<span style="font-size:1.2em">🌡️</span>';
                const humidIcon = '<span style="font-size:1.2em">💧</span>';
                const battIcon = '<span style="font-size:1.2em">🔋</span>';
                let online = node.status === 'Online';
                let cardClass = online ? 'node-card' : 'node-card offline';
                let uptime = node.uptime_s ? formatUptime(node.uptime_s) : '-';
                let lastSeen = node.last_seen ? formatDateTime(node.last_seen) : '-';
                html += `<div class='${cardClass}'>`;
                html += `<div class='node-title'>Node: <span class='node-id'>${node.id || '-'}</span></div>`;
                html += `<div class='node-info'>`;
                html += `${tempIcon} ${node.temp !== undefined ? node.temp + '°C' : '-'} &nbsp;`;
                html += `${humidIcon} ${node.humid !== undefined ? node.humid + '%' : '-'} &nbsp;`;
                html += `${battIcon} ${node.batt !== undefined ? node.batt + 'V' : '-'} &nbsp;`;
                html += `${online ? '<span class="node-status online">Online</span>' : '<span class="node-status offline">Offline</span>'}`;
                html += `</div>`;
                if (online) {
                    html += `<div class='node-uptime'>Uptime: ${uptime}</div>`;
                } else {
                    html += `<div class='node-lastseen'>Offline since: <span style='color:#d32f2f'>${lastSeen}</span></div>`;
                    html += `<div class='node-uptime'>Last Uptime: ${uptime}</div>`;
                }
                html += `</div>`;
            });
            panel.innerHTML = html;
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
