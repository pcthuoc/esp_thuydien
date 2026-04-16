// ============================================================
// SPA Router + API helper
// ============================================================

const $ = (sel) => document.querySelector(sel);
const $$ = (sel) => document.querySelectorAll(sel);
const content = $('#content');

// Toast notification
function toast(msg, type = 'success') {
    const el = document.createElement('div');
    el.className = `toast toast-${type}`;
    el.textContent = msg;
    document.body.appendChild(el);
    setTimeout(() => el.remove(), 3000);
}

// API helpers
const configCache = {};

async function apiGet(url) {
    const res = await fetch(url);
    return res.json();
}

async function apiGetConfig(url) {
    if (configCache[url]) return configCache[url];
    const data = await apiGet(url);
    configCache[url] = data;
    return data;
}

async function apiPost(url, data) {
    // Xóa cache config khi lưu
    delete configCache[url];
    const res = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(data)
    });
    return res.json();
}

// ============================================================
// Debug: Raw poll + client-side calcReal
// ============================================================
let debugTimer = null;
let lastRawData = {};
let debugGroup = '';

function calcReal(raw, calcMode, weight, x1, y1, x2, y2) {
    if (raw === null || raw === undefined) return null;
    if (calcMode === 'interpolation_2point') {
        const dx = (x2 || 0) - (x1 || 0);
        if (Math.abs(dx) < 1e-9) return raw;
        return (y1 || 0) + (raw - (x1 || 0)) * ((y2 || 0) - (y1 || 0)) / dx;
    }
    return raw * (weight || 1);
}

function startDebugPoll(group, updateFn) {
    stopDebugPoll();
    debugGroup = group;
    lastRawData = {};
    const btn = $('#btnDebug');
    if (btn) { btn.textContent = '⏹ Dừng Debug'; btn.classList.add('btn-danger'); btn.classList.remove('btn-primary'); }
    const poll = async () => {
        try {
            lastRawData = await apiGet('/api/debug/raw?group=' + group);
            updateFn(lastRawData);
        } catch(e) {}
    };
    poll();
    debugTimer = setInterval(poll, 3000);
}

function stopDebugPoll() {
    if (debugTimer) { clearInterval(debugTimer); debugTimer = null; }
    debugGroup = '';
    lastRawData = {};
    const btn = $('#btnDebug');
    if (btn) { btn.textContent = '🔍 Debug'; btn.classList.remove('btn-danger'); btn.classList.add('btn-primary'); }
}

function toggleDebug(group, updateFn) {
    if (debugTimer && debugGroup === group) stopDebugPoll();
    else startDebugPoll(group, updateFn);
}

// Recalc Real từ raw đã cache + params hiện tại trên form
function recalcFromForm(updateFn) {
    if (debugTimer && Object.keys(lastRawData).length > 0) updateFn(lastRawData);
}

function toggleDetail(btn) {
    const detail = btn.closest('tr').nextElementSibling;
    if (!detail || !detail.classList.contains('detail-row')) return;
    const opening = detail.style.display === 'none';
    detail.style.display = opening ? '' : 'none';
    btn.textContent = opening ? '▲' : '▼';
}

function deleteRow(btn) {
    const main = btn.closest('tr');
    const detail = main.nextElementSibling;
    if (detail && detail.classList.contains('detail-row')) detail.remove();
    main.remove();
}

// ============================================================
// Page: Home
// ============================================================
async function pageHome() {
    content.innerHTML = '<h2>📊 Trang chủ</h2><div class="status-grid" id="statusGrid">Đang tải...</div>';
    try {
        const d = await apiGet('/api/status');
        const modeLabel = {'wifi':'WiFi Only','4g':'4G Only','auto':'Auto (WiFi+4G)'}[d.net_mode] || 'WiFi Only';
        $('#statusGrid').innerHTML = `
            <div class="status-item"><div class="label">Uptime</div><div class="value">${formatUptime(d.uptime)}</div></div>
            <div class="status-item"><div class="label">Free Heap</div><div class="value">${(d.free_heap/1024).toFixed(0)} KB</div></div>
            <div class="status-item"><div class="label">Chế độ mạng</div><div class="value">${modeLabel}</div></div>
            <div class="status-item"><div class="label">WiFi</div><div class="value">${d.wifi_mode}</div></div>
            <div class="status-item"><div class="label">IP</div><div class="value">${d.ip}</div></div>
            <div class="status-item"><div class="label">SD Tổng</div><div class="value">${d.sd_total_mb} MB</div></div>
            <div class="status-item"><div class="label">SD Đã dùng</div><div class="value">${d.sd_used_mb} MB</div></div>
        `;
    } catch(e) {
        $('#statusGrid').innerHTML = '<p>Lỗi kết nối API</p>';
    }
}

function formatUptime(sec) {
    const h = Math.floor(sec / 3600);
    const m = Math.floor((sec % 3600) / 60);
    const s = sec % 60;
    return `${h}h ${m}m ${s}s`;
}

// ============================================================
// Page: Network config (WiFiManager style)
// ============================================================
async function pageNetwork() {
    const cfg = await apiGetConfig('/api/config/network');
    content.innerHTML = `
        <h2>🌐 Cấu hình Mạng / MQTT</h2>
        <div class="card">
            <h3>WiFi</h3>
            <div class="form-group">
                <label>SSID</label>
                <div style="display:flex;gap:8px;align-items:center">
                    <input id="wifi_ssid" value="${cfg.wifi_ssid || ''}" style="flex:1">
                    <button class="btn btn-primary" id="btnScan" onclick="scanWifi()">📡 Scan</button>
                </div>
            </div>
            <div id="wifiList" style="display:none;margin:8px 0">
                <div id="wifiListInner" style="max-height:200px;overflow-y:auto;border:1px solid #555;border-radius:6px;background:#2a2a4a"></div>
            </div>
            <div class="form-group">
                <label>Password</label>
                <div style="display:flex;gap:8px;align-items:center">
                    <input id="wifi_pass" type="text" value="${cfg.wifi_pass || ''}" style="flex:1">
                    <button class="btn" onclick="togglePass()" style="padding:6px 10px" title="Hiện/ẩn">👁</button>
                </div>
            </div>
        </div>
        <div class="card">
            <h3>MQTT</h3>
            <div class="form-row">
                <div class="form-group">
                    <label>Broker</label>
                    <input id="mqtt_broker" value="${cfg.mqtt_broker || ''}">
                </div>
                <div class="form-group">
                    <label>Port</label>
                    <input id="mqtt_port" type="number" value="${cfg.mqtt_port || 1883}">
                </div>
            </div>
            <div class="form-row">
                <div class="form-group">
                    <label>Device ID</label>
                    <input id="device_id" value="${cfg.device_id || ''}" maxlength="10">
                </div>
                <div class="form-group">
                    <label>MQTT Password</label>
                    <input id="mqtt_pass" type="text" value="${cfg.mqtt_pass || ''}">
                </div>
            </div>
        </div>
        <div class="card">
            <h3>Ethernet W5500 (Static IP)</h3>
            <small style="color:#aaa">Dùng khi DHCP thất bại. Để trống = dùng giá trị mặc định (192.168.0.200/255.255.0.0)</small>
            <div class="form-row" style="margin-top:10px">
                <div class="form-group">
                    <label>Static IP</label>
                    <input id="eth_static_ip" value="${cfg.eth_static_ip || ''}" placeholder="192.168.0.200">
                </div>
                <div class="form-group">
                    <label>Gateway</label>
                    <input id="eth_gateway" value="${cfg.eth_gateway || ''}" placeholder="192.168.0.1">
                </div>
                <div class="form-group">
                    <label>Subnet Mask</label>
                    <input id="eth_subnet" value="${cfg.eth_subnet || ''}" placeholder="255.255.0.0">
                </div>
            </div>
        </div>
        <div class="card">
            <h3>📶 4G / SIM</h3>
            <div class="form-group">
                <label>Chế độ kết nối</label>
                <select id="net_mode">
                    <option value="wifi" ${!cfg.net_mode||cfg.net_mode==='wifi'?'selected':''}>WiFi Only</option>
                    <option value="4g" ${cfg.net_mode==='4g'?'selected':''}>4G Only</option>
                    <option value="auto" ${cfg.net_mode==='auto'?'selected':''}>Auto (WiFi ưu tiên, fallback 4G)</option>
                </select>
                <small style="color:#aaa;display:block;margin-top:4px">Auto: kết nối WiFi trước, mất WiFi tự chuyển sang 4G</small>
            </div>
            <div class="form-group">
                <label>Nhà mạng (chọn để điền APN tự động)</label>
                <div style="display:flex;gap:8px">
                    <select id="sim_carrier" onchange="applyCarrierPreset()" style="flex:1">
                        <option value="custom" ${!cfg.carrier||cfg.carrier==='custom'?'selected':''}>-- Tùy chỉnh --</option>
                        <option value="viettel" ${cfg.carrier==='viettel'?'selected':''}>Viettel</option>
                        <option value="vinaphone" ${cfg.carrier==='vinaphone'?'selected':''}>VinaPhone (VNPT)</option>
                        <option value="mobifone" ${cfg.carrier==='mobifone'?'selected':''}>MobiFone</option>
                        <option value="vietnamobile" ${cfg.carrier==='vietnamobile'?'selected':''}>Vietnamobile</option>
                        <option value="reddi" ${cfg.carrier==='reddi'?'selected':''}>Reddi (Indochina)</option>
                    </select>
                </div>
            </div>
            <div class="form-row">
                <div class="form-group">
                    <label>APN</label>
                    <input id="sim_apn" value="${cfg.apn || ''}" placeholder="v-internet">
                </div>
                <div class="form-group">
                    <label>SIM PIN (để trống nếu không có)</label>
                    <input id="sim_pin" value="${cfg.sim_pin || ''}" placeholder="">
                </div>
            </div>
        </div>
        <div class="btn-group">
            <button class="btn btn-primary" onclick="saveNetwork()">💾 Lưu</button>
        </div>
    `;
}

const CARRIER_PRESETS = {
    viettel:      { apn: 'v-internet' },
    vinaphone:    { apn: 'internet' },
    mobifone:     { apn: 'm-wap' },
    vietnamobile: { apn: 'internet' },
    reddi:        { apn: 'internet' },
};

function applyCarrierPreset() {
    const carrier = $('#sim_carrier').value;
    const preset = CARRIER_PRESETS[carrier];
    if (preset) {
        $('#sim_apn').value = preset.apn;
    }
}

function togglePass() {
    const el = $('#wifi_pass');
    el.type = el.type === 'password' ? 'text' : 'password';
}

async function scanWifi() {
    const btn = $('#btnScan');
    const listDiv = $('#wifiList');
    const inner = $('#wifiListInner');
    
    btn.disabled = true;
    btn.textContent = '⏳ Scanning...';
    listDiv.style.display = 'block';
    inner.innerHTML = '<div style="padding:12px;text-align:center;color:#ccc">Đang quét mạng WiFi...</div>';

    // Trigger scan and poll for results
    await apiGet('/api/scan');
    
    let attempts = 0;
    const poll = setInterval(async () => {
        attempts++;
        try {
            const res = await apiGet('/api/scan');
            if (res.status === 'done') {
                clearInterval(poll);
                btn.disabled = false;
                btn.textContent = '📡 Scan';
                
                if (!res.networks || res.networks.length === 0) {
                    inner.innerHTML = '<div style="padding:12px;text-align:center;color:#ccc">Không tìm thấy mạng WiFi</div>';
                    return;
                }

                // Sort by signal strength
                res.networks.sort((a, b) => b.rssi - a.rssi);
                
                // Remove duplicates (keep strongest)
                const seen = new Set();
                const unique = res.networks.filter(n => {
                    if (n.ssid === '' || seen.has(n.ssid)) return false;
                    seen.add(n.ssid);
                    return true;
                });

                inner.innerHTML = unique.map(n => {
                    const bars = n.rssi > -50 ? '▂▄▆█' : n.rssi > -65 ? '▂▄▆' : n.rssi > -75 ? '▂▄' : '▂';
                    const lock = n.enc ? '🔒' : '';
                    return `<div class="wifi-item" onclick="selectWifi('${n.ssid.replace(/'/g, "\\'")}')" 
                        style="padding:10px 12px;cursor:pointer;border-bottom:1px solid #444;display:flex;justify-content:space-between;align-items:center;color:#fff"
                        onmouseover="this.style.background='#3a3a6a'" onmouseout="this.style.background=''">
                        <span>${lock} ${n.ssid}</span>
                        <span style="color:#aaa;font-size:0.85em">${bars} ${n.rssi}dBm</span>
                    </div>`;
                }).join('');
            }
        } catch(e) {}
        
        if (attempts > 15) {
            clearInterval(poll);
            btn.disabled = false;
            btn.textContent = '📡 Scan';
            inner.innerHTML = '<div style="padding:12px;text-align:center;color:#f66">Scan timeout</div>';
        }
    }, 1500);
}

function selectWifi(ssid) {
    $('#wifi_ssid').value = ssid;
    $('#wifiList').style.display = 'none';
    $('#wifi_pass').focus();
}

async function saveNetwork() {
    const data = {
        wifi_ssid: $('#wifi_ssid').value,
        wifi_pass: $('#wifi_pass').value,
        mqtt_broker: $('#mqtt_broker').value,
        mqtt_port: parseInt($('#mqtt_port').value) || 1883,
        device_id: $('#device_id').value,
        mqtt_pass: $('#mqtt_pass').value,
        eth_static_ip: $('#eth_static_ip').value.trim(),
        eth_gateway:   $('#eth_gateway').value.trim(),
        eth_subnet:    $('#eth_subnet').value.trim(),
        net_mode: $('#net_mode').value,
        carrier:  $('#sim_carrier').value,
        apn:      $('#sim_apn').value.trim(),
        sim_pin:  $('#sim_pin').value.trim()
    };
    const res = await apiPost('/api/config/network', data);
    if (res.ok) toast('Đã lưu cấu hình mạng');
    else toast('Lỗi lưu config', 'error');
}

// ============================================================
// Page: Analog config
// ============================================================
async function pageAnalog() {
    const cfg = await apiGetConfig('/api/config/analog');
    const channels = cfg.channels || {};

    const names = ['A1','A2','A3','A4','A5','A6','A7','A8'];
    const types = ['Điện áp','Điện áp','Điện áp','Điện áp','Dòng','Dòng','Dòng','Dòng'];

    let rows = names.map((name, i) => {
        const ch = channels[name] || {};
        return `<tr class="main-row">
            <td><input type="checkbox" data-ch="${name}" class="ch-enable" ${ch.enabled ? 'checked' : ''}></td>
            <td>${name} <small>(${types[i]})</small></td>
            <td class="debug-raw" data-ch="${name}" style="color:#0ff">--</td>
            <td class="debug-real" data-ch="${name}" style="color:#0f0">--</td>
            <td><button class="btn btn-expand" onclick="toggleDetail(this)" style="padding:4px 10px">▼</button></td>
        </tr>
        <tr class="detail-row" style="display:none">
            <td colspan="5" style="padding:12px 16px">
                <div style="display:flex;gap:14px;flex-wrap:wrap;align-items:center">
                    <label style="display:flex;gap:6px;align-items:center">Calc:
                        <select data-ch="${name}" class="ch-calc" oninput="recalcFromForm(updateAnalogDebug)">
                            <option value="weight" ${ch.calc_mode==='weight'?'selected':''}>Weight</option>
                            <option value="interpolation_2point" ${ch.calc_mode==='interpolation_2point'?'selected':''}>Nội suy 2 điểm</option>
                        </select>
                    </label>
                    <label style="display:flex;gap:6px;align-items:center">Weight: <input type="number" step="any" data-ch="${name}" class="ch-weight" value="${ch.weight ?? ''}" style="width:80px" oninput="recalcFromForm(updateAnalogDebug)"></label>
                    <label style="display:flex;gap:6px;align-items:center">X1: <input type="number" step="any" data-ch="${name}" class="ch-x1" value="${ch.x1 ?? ''}" style="width:70px" oninput="recalcFromForm(updateAnalogDebug)"></label>
                    <label style="display:flex;gap:6px;align-items:center">Y1: <input type="number" step="any" data-ch="${name}" class="ch-y1" value="${ch.y1 ?? ''}" style="width:70px" oninput="recalcFromForm(updateAnalogDebug)"></label>
                    <label style="display:flex;gap:6px;align-items:center">X2: <input type="number" step="any" data-ch="${name}" class="ch-x2" value="${ch.x2 ?? ''}" style="width:70px" oninput="recalcFromForm(updateAnalogDebug)"></label>
                    <label style="display:flex;gap:6px;align-items:center">Y2: <input type="number" step="any" data-ch="${name}" class="ch-y2" value="${ch.y2 ?? ''}" style="width:70px" oninput="recalcFromForm(updateAnalogDebug)"></label>
                </div>
            </td>
        </tr>`;
    }).join('');

    content.innerHTML = `
        <h2>📈 Cấu hình Analog</h2>
        <div class="card">
            <table>
                <thead><tr>
                    <th>Bật</th><th>Kênh</th><th>Raw</th><th>Real</th><th></th>
                </tr></thead>
                <tbody>${rows}</tbody>
            </table>
        </div>
        <div class="btn-group">
            <button class="btn btn-primary" onclick="saveAnalog()">💾 Lưu</button>
            <button class="btn btn-primary" id="btnDebug" onclick="toggleDebug('analog', updateAnalogDebug)">🔍 Debug</button>
        </div>
    `;
}

function updateAnalogDebug(d) {
    ['A1','A2','A3','A4','A5','A6','A7','A8'].forEach(name => {
        const rawEl = $(`.debug-raw[data-ch="${name}"]`);
        const realEl = $(`.debug-real[data-ch="${name}"]`);
        if (!rawEl) return;
        const raw = d[name] ?? null;
        rawEl.textContent = raw !== null ? raw : '--';
        if (raw !== null) {
            const mode = $(`.ch-calc[data-ch="${name}"]`).value;
            const w = parseFloat($(`.ch-weight[data-ch="${name}"]`).value);
            const x1 = parseFloat($(`.ch-x1[data-ch="${name}"]`).value);
            const y1 = parseFloat($(`.ch-y1[data-ch="${name}"]`).value);
            const x2 = parseFloat($(`.ch-x2[data-ch="${name}"]`).value);
            const y2 = parseFloat($(`.ch-y2[data-ch="${name}"]`).value);
            const real = calcReal(raw, mode, w, x1, y1, x2, y2);
            realEl.textContent = real !== null ? real.toFixed(3) : '--';
        } else {
            realEl.textContent = '--';
        }
    });
}

async function saveAnalog() {
    const channels = {};
    ['A1','A2','A3','A4','A5','A6','A7','A8'].forEach(name => {
        channels[name] = {
            enabled: $(`.ch-enable[data-ch="${name}"]`).checked,
            calc_mode: $(`.ch-calc[data-ch="${name}"]`).value,
            weight: parseFloat($(`.ch-weight[data-ch="${name}"]`).value) || null,
            x1: parseFloat($(`.ch-x1[data-ch="${name}"]`).value) || null,
            y1: parseFloat($(`.ch-y1[data-ch="${name}"]`).value) || null,
            x2: parseFloat($(`.ch-x2[data-ch="${name}"]`).value) || null,
            y2: parseFloat($(`.ch-y2[data-ch="${name}"]`).value) || null,
        };
    });
    const res = await apiPost('/api/config/analog', { channels });
    if (res.ok) toast('Đã lưu cấu hình Analog');
    else toast('Lỗi lưu', 'error');
}

// ============================================================
// Page: Encoder config
// ============================================================
async function pageEncoder() {
    const cfg = await apiGetConfig('/api/config/encoder');
    const channels = cfg.channels || {};

    let rows = ['E1','E2'].map(name => {
        const ch = channels[name] || {};
        return `<tr class="main-row">
            <td><input type="checkbox" data-ch="${name}" class="ch-enable" ${ch.enabled ? 'checked' : ''}></td>
            <td>${name}</td>
            <td class="debug-raw" data-ch="${name}" style="color:#0ff">--</td>
            <td class="debug-real" data-ch="${name}" style="color:#0f0">--</td>
            <td><button class="btn btn-expand" onclick="toggleDetail(this)" style="padding:4px 10px">▼</button></td>
        </tr>
        <tr class="detail-row" style="display:none">
            <td colspan="5" style="padding:12px 16px">
                <div style="display:flex;gap:14px;flex-wrap:wrap;align-items:center">
                    <label style="display:flex;gap:6px;align-items:center">Calc:
                        <select data-ch="${name}" class="ch-calc" oninput="recalcFromForm(updateEncoderDebug)">
                            <option value="weight" ${ch.calc_mode==='weight'?'selected':''}>Weight</option>
                            <option value="interpolation_2point" ${ch.calc_mode==='interpolation_2point'?'selected':''}>Nội suy 2 điểm</option>
                        </select>
                    </label>
                    <label style="display:flex;gap:6px;align-items:center">Weight: <input type="number" step="any" data-ch="${name}" class="ch-weight" value="${ch.weight ?? ''}" style="width:80px" oninput="recalcFromForm(updateEncoderDebug)"></label>
                    <label style="display:flex;gap:6px;align-items:center">X1: <input type="number" step="any" data-ch="${name}" class="ch-x1" value="${ch.x1 ?? ''}" style="width:70px" oninput="recalcFromForm(updateEncoderDebug)"></label>
                    <label style="display:flex;gap:6px;align-items:center">Y1: <input type="number" step="any" data-ch="${name}" class="ch-y1" value="${ch.y1 ?? ''}" style="width:70px" oninput="recalcFromForm(updateEncoderDebug)"></label>
                    <label style="display:flex;gap:6px;align-items:center">X2: <input type="number" step="any" data-ch="${name}" class="ch-x2" value="${ch.x2 ?? ''}" style="width:70px" oninput="recalcFromForm(updateEncoderDebug)"></label>
                    <label style="display:flex;gap:6px;align-items:center">Y2: <input type="number" step="any" data-ch="${name}" class="ch-y2" value="${ch.y2 ?? ''}" style="width:70px" oninput="recalcFromForm(updateEncoderDebug)"></label>
                </div>
            </td>
        </tr>`;
    }).join('');

    content.innerHTML = `
        <h2>🔄 Cấu hình Encoder</h2>
        <div class="card">
            <table>
                <thead><tr><th>Bật</th><th>Kênh</th><th>Raw</th><th>Real</th><th></th></tr></thead>
                <tbody>${rows}</tbody>
            </table>
        </div>
        <div class="btn-group">
            <button class="btn btn-primary" onclick="saveEncoder()">💾 Lưu</button>
            <button class="btn btn-primary" id="btnDebug" onclick="toggleDebug('encoder', updateEncoderDebug)">🔍 Debug</button>
        </div>
    `;
}

function updateEncoderDebug(d) {
    ['E1','E2'].forEach(name => {
        const rawEl = $(`.debug-raw[data-ch="${name}"]`);
        const realEl = $(`.debug-real[data-ch="${name}"]`);
        if (!rawEl) return;
        const raw = d[name] ?? null;
        rawEl.textContent = raw !== null ? raw : '--';
        if (raw !== null) {
            const mode = $(`.ch-calc[data-ch="${name}"]`).value;
            const w = parseFloat($(`.ch-weight[data-ch="${name}"]`).value);
            const x1 = parseFloat($(`.ch-x1[data-ch="${name}"]`).value);
            const y1 = parseFloat($(`.ch-y1[data-ch="${name}"]`).value);
            const x2 = parseFloat($(`.ch-x2[data-ch="${name}"]`).value);
            const y2 = parseFloat($(`.ch-y2[data-ch="${name}"]`).value);
            realEl.textContent = calcReal(raw, mode, w, x1, y1, x2, y2)?.toFixed(3) ?? '--';
        } else {
            realEl.textContent = '--';
        }
    });
}

async function saveEncoder() {
    const channels = {};
    ['E1','E2'].forEach(name => {
        channels[name] = {
            enabled: $(`.ch-enable[data-ch="${name}"]`).checked,
            calc_mode: $(`.ch-calc[data-ch="${name}"]`).value,
            weight: parseFloat($(`.ch-weight[data-ch="${name}"]`).value) || null,
            x1: parseFloat($(`.ch-x1[data-ch="${name}"]`).value) || null,
            y1: parseFloat($(`.ch-y1[data-ch="${name}"]`).value) || null,
            x2: parseFloat($(`.ch-x2[data-ch="${name}"]`).value) || null,
            y2: parseFloat($(`.ch-y2[data-ch="${name}"]`).value) || null,
        };
    });
    const res = await apiPost('/api/config/encoder', { channels });
    if (res.ok) toast('Đã lưu cấu hình Encoder');
    else toast('Lỗi lưu', 'error');
}

// ============================================================
// Page: DI (rain) config
// ============================================================
async function pageDi() {
    const cfg = await apiGetConfig('/api/config/di');
    const ch = (cfg.channels || {}).DI1 || {};

    content.innerHTML = `
        <h2>🌧 Cấu hình Cảm biến mưa (DI1)</h2>
        <div class="card">
            <div class="form-group">
                <label><input type="checkbox" id="di_enabled" ${ch.enabled ? 'checked' : ''}> Kích hoạt DI1</label>
            </div>
            <div class="form-row">
                <div class="form-group">
                    <label>Calc mode</label>
                    <select id="di_calc" oninput="recalcFromForm(updateDiDebug)">
                        <option value="weight" ${ch.calc_mode==='weight'?'selected':''}>Weight</option>
                        <option value="interpolation_2point" ${ch.calc_mode==='interpolation_2point'?'selected':''}>Nội suy 2 điểm</option>
                    </select>
                </div>
                <div class="form-group">
                    <label>Weight (VD: 0.2 = mỗi xung 0.2mm)</label>
                    <input id="di_weight" type="number" step="any" value="${ch.weight ?? ''}" oninput="recalcFromForm(updateDiDebug)">
                </div>
            </div>
            <div class="form-row">
                <div class="form-group"><label>X1</label><input id="di_x1" type="number" step="any" value="${ch.x1 ?? ''}" oninput="recalcFromForm(updateDiDebug)"></div>
                <div class="form-group"><label>Y1</label><input id="di_y1" type="number" step="any" value="${ch.y1 ?? ''}" oninput="recalcFromForm(updateDiDebug)"></div>
            </div>
            <div class="form-row">
                <div class="form-group"><label>X2</label><input id="di_x2" type="number" step="any" value="${ch.x2 ?? ''}" oninput="recalcFromForm(updateDiDebug)"></div>
                <div class="form-group"><label>Y2</label><input id="di_y2" type="number" step="any" value="${ch.y2 ?? ''}" oninput="recalcFromForm(updateDiDebug)"></div>
            </div>
            <div class="form-row" style="margin-top:12px">
                <div class="form-group">
                    <label>Raw</label>
                    <div id="di_debug_raw" style="color:#0ff;font-size:1.2em;font-weight:bold">--</div>
                </div>
                <div class="form-group">
                    <label>Real</label>
                    <div id="di_debug_real" style="color:#0f0;font-size:1.2em;font-weight:bold">--</div>
                </div>
            </div>
        </div>
        <div class="btn-group">
            <button class="btn btn-primary" onclick="saveDi()">💾 Lưu</button>
            <button class="btn btn-primary" id="btnDebug" onclick="toggleDebug('di', updateDiDebug)">🔍 Debug</button>
        </div>
    `;
}

function updateDiDebug(d) {
    const raw = d.DI1 ?? null;
    const rawEl = $('#di_debug_raw');
    const realEl = $('#di_debug_real');
    if (!rawEl) return;
    rawEl.textContent = raw !== null ? raw : '--';
    if (raw !== null) {
        const mode = $('#di_calc').value;
        const w = parseFloat($('#di_weight').value);
        const x1 = parseFloat($('#di_x1').value);
        const y1 = parseFloat($('#di_y1').value);
        const x2 = parseFloat($('#di_x2').value);
        const y2 = parseFloat($('#di_y2').value);
        realEl.textContent = calcReal(raw, mode, w, x1, y1, x2, y2)?.toFixed(3) ?? '--';
    } else {
        realEl.textContent = '--';
    }
}

async function saveDi() {
    const data = {
        channels: {
            DI1: {
                enabled: $('#di_enabled').checked,
                calc_mode: $('#di_calc').value,
                weight: parseFloat($('#di_weight').value) || null,
                x1: parseFloat($('#di_x1').value) || null,
                y1: parseFloat($('#di_y1').value) || null,
                x2: parseFloat($('#di_x2').value) || null,
                y2: parseFloat($('#di_y2').value) || null,
            }
        }
    };
    const res = await apiPost('/api/config/di', data);
    if (res.ok) toast('Đã lưu cấu hình DI');
    else toast('Lỗi lưu', 'error');
}

// ============================================================
// Page: RS485 config
// ============================================================
async function pageRs485() {
    const cfg = await apiGetConfig('/api/config/rs485');

    content.innerHTML = `
        <h2>🔌 Cấu hình RS485 Modbus RTU</h2>
        ${renderRs485Bus('rs485_1', 'Bus 1', cfg.rs485_1 || {})}
        ${renderRs485Bus('rs485_2', 'Bus 2', cfg.rs485_2 || {})}
        <div class="btn-group">
            <button class="btn btn-primary" onclick="saveRs485()">💾 Lưu</button>
            <button class="btn btn-primary" id="btnDebug" onclick="toggleDebug('rs485', updateRs485Debug)">🔍 Debug</button>
        </div>
    `;
}

function updateRs485Debug(d) {
    $$('#rs485_1_table tbody tr.main-row').forEach(tr => {
        const name = tr.querySelector('.rs-name').value;
        const raw = d[name] ?? null;
        tr.querySelector('.rs-debug-raw').textContent = raw !== null ? raw : '--';
        if (raw !== null) {
            const detail = tr.nextElementSibling;
            const mode = detail.querySelector('.rs-calc').value;
            const w = parseFloat(detail.querySelector('.rs-weight').value);
            const x1 = parseFloat(detail.querySelector('.rs-x1').value);
            const y1 = parseFloat(detail.querySelector('.rs-y1').value);
            const x2 = parseFloat(detail.querySelector('.rs-x2').value);
            const y2 = parseFloat(detail.querySelector('.rs-y2').value);
            tr.querySelector('.rs-debug-real').textContent = calcReal(raw, mode, w, x1, y1, x2, y2)?.toFixed(3) ?? '--';
        } else {
            tr.querySelector('.rs-debug-real').textContent = '--';
        }
    });
}

function renderRs485Bus(busId, label, bus) {
    const channels = bus.channels || [];
    let rows = channels.map((ch, i) => renderRs485Row(busId, i, ch)).join('');

    return `
        <div class="card">
            <h3>${label}</h3>
            <div class="form-row">
                <div class="form-group">
                    <label>Baud rate</label>
                    <select id="${busId}_baud">
                        ${[9600,19200,38400,115200].map(b => `<option value="${b}" ${bus.baud==b?'selected':''}>${b}</option>`).join('')}
                    </select>
                </div>
                <div class="form-group">
                    <label>Parity</label>
                    <select id="${busId}_parity">
                        <option value="none" ${bus.parity==='none'?'selected':''}>None</option>
                        <option value="even" ${bus.parity==='even'?'selected':''}>Even</option>
                        <option value="odd" ${bus.parity==='odd'?'selected':''}>Odd</option>
                    </select>
                </div>
            </div>
            <table id="${busId}_table">
                <thead><tr>
                    <th>Bật</th><th>Biến</th><th>Slave</th><th>Reg</th><th>Raw</th><th>Real</th><th></th><th></th>
                </tr></thead>
                <tbody>${rows}</tbody>
            </table>
            <button class="btn btn-primary" style="margin-top:8px" onclick="addRs485Row('${busId}')">+ Thêm kênh</button>
        </div>
    `;
}

function renderRs485Row(busId, idx, ch) {
    const vname = ch.name || ('V' + (idx + 1));
    return `<tr class="main-row">
        <td><input type="checkbox" class="rs-enable" ${ch.enabled !== false ? 'checked' : ''}></td>
        <td><input type="text" class="rs-name" value="${vname}" style="width:50px" placeholder="V${idx+1}"></td>
        <td><input type="number" class="rs-slave" value="${ch.slave_id ?? 1}" style="width:55px"></td>
        <td><input type="number" class="rs-reg" value="${ch.register ?? 0}" style="width:65px"></td>
        <td class="rs-debug-raw" style="color:#0ff">--</td>
        <td class="rs-debug-real" style="color:#0f0">--</td>
        <td><button class="btn btn-expand" onclick="toggleDetail(this)" style="padding:4px 10px">▼</button></td>
        <td><button class="btn btn-danger" onclick="deleteRow(this)" style="padding:4px 8px">✕</button></td>
    </tr>
    <tr class="detail-row" style="display:none">
        <td colspan="8" style="padding:12px 16px">
            <div style="display:flex;gap:12px;flex-wrap:wrap;align-items:center">
                <label style="display:flex;gap:6px;align-items:center">FC: <select class="rs-fc"><option value="03" ${ch.function_code==='04'||ch.fc==4?'':'selected'}>03</option><option value="04" ${ch.function_code==='04'||ch.fc==4?'selected':''}>04</option></select></label>
                <label style="display:flex;gap:6px;align-items:center">Type: <select class="rs-dtype">${['INT16','UINT16','INT32','UINT32','FLOAT32'].map(t => `<option ${ch.data_type===t?'selected':''}>${t}</option>`).join('')}</select></label>
                <label style="display:flex;gap:6px;align-items:center">Order: <select class="rs-order">${['BE','LE','MBE','MLE'].map(t => `<option ${ch.byte_order===t?'selected':''}>${t}</option>`).join('')}</select></label>
                <label style="display:flex;gap:6px;align-items:center">Calc: <select class="rs-calc" oninput="recalcFromForm(updateRs485Debug)"><option value="weight" ${ch.calc_mode==='weight'?'selected':''}>Weight</option><option value="interpolation_2point" ${ch.calc_mode==='interpolation_2point'?'selected':''}>Nội suy 2đ</option></select></label>
                <label style="display:flex;gap:6px;align-items:center">Weight: <input type="number" step="any" class="rs-weight" value="${ch.weight ?? ''}" style="width:70px" oninput="recalcFromForm(updateRs485Debug)"></label>
                <label style="display:flex;gap:6px;align-items:center">X1: <input type="number" step="any" class="rs-x1" value="${ch.x1 ?? ''}" style="width:65px" oninput="recalcFromForm(updateRs485Debug)"></label>
                <label style="display:flex;gap:6px;align-items:center">Y1: <input type="number" step="any" class="rs-y1" value="${ch.y1 ?? ''}" style="width:65px" oninput="recalcFromForm(updateRs485Debug)"></label>
                <label style="display:flex;gap:6px;align-items:center">X2: <input type="number" step="any" class="rs-x2" value="${ch.x2 ?? ''}" style="width:65px" oninput="recalcFromForm(updateRs485Debug)"></label>
                <label style="display:flex;gap:6px;align-items:center">Y2: <input type="number" step="any" class="rs-y2" value="${ch.y2 ?? ''}" style="width:65px" oninput="recalcFromForm(updateRs485Debug)"></label>
            </div>
        </td>
    </tr>`;
}

function nextVName(tableId) {
    const used = new Set();
    $$(`#${tableId} tbody .rs-name, #${tableId} tbody .tcp-name`).forEach(el => used.add(el.value));
    for (let i = 1; i <= 20; i++) { if (!used.has('V' + i)) return 'V' + i; }
    return 'V' + (used.size + 1);
}

function addRs485Row(busId) {
    const tbody = $(`#${busId}_table tbody`);
    if (tbody.querySelectorAll('tr.main-row').length >= 20) { toast('Tối đa 20 kênh', 'error'); return; }
    const name = nextVName(busId + '_table');
    tbody.insertAdjacentHTML('beforeend', renderRs485Row(busId, 0, { name }));
}

function collectRs485Bus(busId) {
    const rows = $$(`#${busId}_table tbody tr.main-row`);
    const channels = [];
    rows.forEach((tr, i) => {
        const detail = tr.nextElementSibling;
        channels.push({
            enabled: tr.querySelector('.rs-enable').checked,
            name: tr.querySelector('.rs-name').value || ('V' + (i + 1)),
            slave_id: parseInt(tr.querySelector('.rs-slave').value) || 1,
            register: parseInt(tr.querySelector('.rs-reg').value) || 0,
            function_code: detail.querySelector('.rs-fc').value,
            data_type: detail.querySelector('.rs-dtype').value,
            register_order: detail.querySelector('.rs-order').value,
            calc_mode: detail.querySelector('.rs-calc').value,
            weight: parseFloat(detail.querySelector('.rs-weight').value) || null,
            x1: parseFloat(detail.querySelector('.rs-x1').value) || null,
            y1: parseFloat(detail.querySelector('.rs-y1').value) || null,
            x2: parseFloat(detail.querySelector('.rs-x2').value) || null,
            y2: parseFloat(detail.querySelector('.rs-y2').value) || null,
        });
    });
    return {
        baud: parseInt($(`#${busId}_baud`).value),
        parity: $(`#${busId}_parity`).value,
        channels
    };
}

async function saveRs485() {
    const data = {
        rs485_1: collectRs485Bus('rs485_1'),
        rs485_2: collectRs485Bus('rs485_2')
    };
    const res = await apiPost('/api/config/rs485', data);
    if (res.ok) toast('Đã lưu cấu hình RS485');
    else toast('Lỗi lưu', 'error');
}

// ============================================================
// Page: TCP config
// ============================================================
async function pageTcp() {
    const cfg = await apiGetConfig('/api/config/tcp');
    const channels = cfg.channels || [];

    let rows = channels.map((ch, i) => renderTcpRow(i, ch)).join('');

    content.innerHTML = `
        <h2>🖧 Cấu hình Modbus TCP</h2>
        <div class="card">
            <table id="tcp_table">
                <thead><tr>
                    <th>Bật</th><th>Biến</th><th>IP</th><th>Port</th><th>Slave</th><th>Reg</th><th>Raw</th><th>Real</th><th></th><th></th>
                </tr></thead>
                <tbody>${rows}</tbody>
            </table>
            <button class="btn btn-primary" style="margin-top:8px" onclick="addTcpRow()">+ Thêm kênh</button>
        </div>
        <div class="btn-group">
            <button class="btn btn-primary" onclick="saveTcp()">💾 Lưu</button>
            <button class="btn btn-primary" id="btnDebug" onclick="toggleDebug('tcp', updateTcpDebug)">🔍 Debug</button>
        </div>
    `;
}

function updateTcpDebug(d) {
    $$('#tcp_table tbody tr.main-row').forEach(tr => {
        const name = tr.querySelector('.tcp-name').value;
        const raw = d[name] ?? null;
        tr.querySelector('.tcp-debug-raw').textContent = raw !== null ? raw : '--';
        if (raw !== null) {
            const detail = tr.nextElementSibling;
            const mode = detail.querySelector('.tcp-calc').value;
            const w = parseFloat(detail.querySelector('.tcp-weight').value);
            const x1 = parseFloat(detail.querySelector('.tcp-x1').value);
            const y1 = parseFloat(detail.querySelector('.tcp-y1').value);
            const x2 = parseFloat(detail.querySelector('.tcp-x2').value);
            const y2 = parseFloat(detail.querySelector('.tcp-y2').value);
            tr.querySelector('.tcp-debug-real').textContent = calcReal(raw, mode, w, x1, y1, x2, y2)?.toFixed(3) ?? '--';
        } else {
            tr.querySelector('.tcp-debug-real').textContent = '--';
        }
    });}

function renderTcpRow(idx, ch) {
    const vname = ch.name || ('V' + (idx + 1));
    return `<tr class="main-row">
        <td><input type="checkbox" class="tcp-enable" ${ch.enabled !== false ? 'checked' : ''}></td>
        <td><input type="text" class="tcp-name" value="${vname}" style="width:50px" placeholder="V${idx+1}"></td>
        <td><input type="text" class="tcp-ip" value="${ch.ip || ''}" style="width:120px" placeholder="192.168.1.x"></td>
        <td><input type="number" class="tcp-port" value="${ch.port || 502}" style="width:55px"></td>
        <td><input type="number" class="tcp-slave" value="${ch.slave_id ?? 1}" style="width:45px"></td>
        <td><input type="number" class="tcp-reg" value="${ch.register ?? 0}" style="width:65px"></td>
        <td class="tcp-debug-raw" style="color:#0ff">--</td>
        <td class="tcp-debug-real" style="color:#0f0">--</td>
        <td><button class="btn btn-expand" onclick="toggleDetail(this)" style="padding:4px 10px">▼</button></td>
        <td><button class="btn btn-danger" onclick="deleteRow(this)" style="padding:4px 8px">✕</button></td>
    </tr>
    <tr class="detail-row" style="display:none">
        <td colspan="10" style="padding:12px 16px">
            <div style="display:flex;gap:12px;flex-wrap:wrap;align-items:center">
                <label style="display:flex;gap:6px;align-items:center">FC: <select class="tcp-fc"><option value="03" ${ch.function_code==='04'||ch.fc==4?'':'selected'}>03</option><option value="04" ${ch.function_code==='04'||ch.fc==4?'selected':''}>04</option></select></label>
                <label style="display:flex;gap:6px;align-items:center">Type: <select class="tcp-dtype">${['INT16','UINT16','INT32','UINT32','FLOAT32'].map(t => `<option ${ch.data_type===t?'selected':''}>${t}</option>`).join('')}</select></label>
                <label style="display:flex;gap:6px;align-items:center">Order: <select class="tcp-order">${['BE','LE','MBE','MLE'].map(t => `<option ${ch.byte_order===t?'selected':''}>${t}</option>`).join('')}</select></label>
                <label style="display:flex;gap:6px;align-items:center">Calc: <select class="tcp-calc" oninput="recalcFromForm(updateTcpDebug)"><option value="weight" ${ch.calc_mode==='weight'?'selected':''}>Weight</option><option value="interpolation_2point" ${ch.calc_mode==='interpolation_2point'?'selected':''}>Nội suy 2đ</option></select></label>
                <label style="display:flex;gap:6px;align-items:center">Weight: <input type="number" step="any" class="tcp-weight" value="${ch.weight ?? ''}" style="width:70px" oninput="recalcFromForm(updateTcpDebug)"></label>
                <label style="display:flex;gap:6px;align-items:center">X1: <input type="number" step="any" class="tcp-x1" value="${ch.x1 ?? ''}" style="width:65px" oninput="recalcFromForm(updateTcpDebug)"></label>
                <label style="display:flex;gap:6px;align-items:center">Y1: <input type="number" step="any" class="tcp-y1" value="${ch.y1 ?? ''}" style="width:65px" oninput="recalcFromForm(updateTcpDebug)"></label>
                <label style="display:flex;gap:6px;align-items:center">X2: <input type="number" step="any" class="tcp-x2" value="${ch.x2 ?? ''}" style="width:65px" oninput="recalcFromForm(updateTcpDebug)"></label>
                <label style="display:flex;gap:6px;align-items:center">Y2: <input type="number" step="any" class="tcp-y2" value="${ch.y2 ?? ''}" style="width:65px" oninput="recalcFromForm(updateTcpDebug)"></label>
            </div>
        </td>
    </tr>`;
}

function addTcpRow() {
    const tbody = $('#tcp_table tbody');
    if (tbody.querySelectorAll('tr.main-row').length >= 20) { toast('Tối đa 20 kênh', 'error'); return; }
    const name = nextVName('tcp_table');
    tbody.insertAdjacentHTML('beforeend', renderTcpRow(0, { name }));
}

async function saveTcp() {
    const rows = $$('#tcp_table tbody tr.main-row');
    const channels = [];
    rows.forEach((tr, i) => {
        const detail = tr.nextElementSibling;
        channels.push({
            enabled: tr.querySelector('.tcp-enable').checked,
            name: tr.querySelector('.tcp-name').value || ('V' + (i + 1)),
            ip: tr.querySelector('.tcp-ip').value,
            port: parseInt(tr.querySelector('.tcp-port').value) || 502,
            slave_id: parseInt(tr.querySelector('.tcp-slave').value) || 1,
            register: parseInt(tr.querySelector('.tcp-reg').value) || 0,
            function_code: detail.querySelector('.tcp-fc').value,
            data_type: detail.querySelector('.tcp-dtype').value,
            register_order: detail.querySelector('.tcp-order').value,
            calc_mode: detail.querySelector('.tcp-calc').value,
            weight: parseFloat(detail.querySelector('.tcp-weight').value) || null,
            x1: parseFloat(detail.querySelector('.tcp-x1').value) || null,
            y1: parseFloat(detail.querySelector('.tcp-y1').value) || null,
            x2: parseFloat(detail.querySelector('.tcp-x2').value) || null,
            y2: parseFloat(detail.querySelector('.tcp-y2').value) || null,
        });
    });
    const res = await apiPost('/api/config/tcp', { channels });
    if (res.ok) toast('Đã lưu cấu hình TCP');
    else toast('Lỗi lưu', 'error');
}

// ============================================================
// Page: Monitor
// ============================================================
async function pageMonitor() {
    content.innerHTML = `
        <h2>📡 Giám sát Realtime</h2>
        <div class="card">
            <table id="monitor_table">
                <thead><tr><th>Group</th><th>Channel</th><th>Raw</th><th>Real</th><th>Trạng thái</th></tr></thead>
                <tbody><tr><td colspan="5">Chưa có dữ liệu (chờ sensor module)</td></tr></tbody>
            </table>
        </div>
    `;
}

// ============================================================
// Page: System
// ============================================================
async function pageSystem() {
    const cfg = await apiGetConfig('/api/config/system');
    content.innerHTML = `
        <h2>⚙ Hệ thống</h2>
        <div class="card">
            <h3>Thiết bị</h3>
            <div class="form-row">
                <div class="form-group">
                    <label>Tên thiết bị (hostname)</label>
                    <input id="sys_hostname" value="${cfg.hostname || 'THUYDIEN'}" placeholder="THUYDIEN">
                </div>
            </div>
        </div>
        <div class="card">
            <h3>WiFi AP (chế độ cấu hình)</h3>
            <div class="form-row">
                <div class="form-group">
                    <label>AP SSID</label>
                    <input id="sys_ap_ssid" value="${cfg.ap_ssid || 'THUYDIEN_CFG'}" placeholder="THUYDIEN_CFG">
                </div>
                <div class="form-group">
                    <label>AP Password (≥8 ký tự)</label>
                    <input id="sys_ap_pass" value="${cfg.ap_pass || '12345678'}" placeholder="12345678">
                </div>
            </div>
        </div>
        <div class="btn-group">
            <button class="btn btn-primary" onclick="saveSystem()">💾 Lưu</button>
        </div>
        <div class="card" style="margin-top:16px">
            <h3>Khởi động lại</h3>
            <p>ESP32 sẽ restart và áp dụng config mới.</p>
            <div class="btn-group">
                <button class="btn btn-danger" onclick="doRestart()">🔄 Restart</button>
            </div>
        </div>
        <div class="card" style="margin-top:16px">
            <h3>⚠ Xóa toàn bộ cấu hình</h3>
            <p>Xóa hết file config trên SD card (WiFi, MQTT, Analog, RS485, TCP...). Thiết bị sẽ trở về mặc định.</p>
            <div class="btn-group">
                <button class="btn btn-danger" onclick="doClearConfig()">🗑 Clear Config</button>
            </div>
        </div>
    `;
}

async function saveSystem() {
    const data = {
        hostname: $('#sys_hostname').value,
        ap_ssid: $('#sys_ap_ssid').value,
        ap_pass: $('#sys_ap_pass').value
    };
    if (data.ap_pass.length > 0 && data.ap_pass.length < 8) {
        toast('AP Password phải ≥ 8 ký tự', 'error');
        return;
    }
    const res = await apiPost('/api/config/system', data);
    if (res.ok) toast('Đã lưu cấu hình hệ thống');
    else toast('Lỗi lưu', 'error');
}

async function doRestart() {
    if (confirm('Xác nhận khởi động lại?')) {
        await apiPost('/api/restart', {});
        toast('Đang khởi động lại...');
    }
}

async function doClearConfig() {
    if (confirm('XÓA TOÀN BỘ cấu hình? Thiết bị sẽ trở về mặc định!')) {
        const res = await apiPost('/api/clear-config', {});
        if (res.ok) {
            // Xóa cache JS
            Object.keys(configCache).forEach(k => delete configCache[k]);
            toast(`Đã xóa ${res.cleared} file config`);
        } else {
            toast('Lỗi xóa config', 'error');
        }
    }
}

// ============================================================
// Router
// ============================================================
const pages = {
    home: pageHome,
    network: pageNetwork,
    analog: pageAnalog,
    encoder: pageEncoder,
    di: pageDi,
    rs485: pageRs485,
    tcp: pageTcp,
    system: pageSystem,
};

function navigate(page) {
    stopDebugPoll();

    // Update active sidebar
    $$('.sidebar a').forEach(a => a.classList.remove('active'));
    const link = $(`.sidebar a[data-page="${page}"]`);
    if (link) link.classList.add('active');

    // Load page
    const fn = pages[page];
    if (fn) fn();
    else content.innerHTML = '<h2>404</h2><p>Trang không tồn tại</p>';
}

// Sidebar click handler
$$('.sidebar a[data-page]').forEach(a => {
    a.addEventListener('click', (e) => {
        e.preventDefault();
        navigate(a.dataset.page);
    });
});

// Init
navigate('home');
