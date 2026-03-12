let ws;
let wsReconnectTimer;
let isUTC = false;
let incomingWifi = [];
let expectedWifiCount = 0;

// Инициализация при старте
window.onload = () => {
	initWebSocket();
	loadLogs();
	loadTimezones('ru');

	// Подсказки Bootstrap
	document.querySelectorAll('[data-bs-toggle="tooltip"]').forEach(t => new bootstrap.Tooltip(t));

	document.getElementById('configForm').onsubmit = async (e) => {
		e.preventDefault(); // Останавливаем обычную отправку формы

		const f = e.target;
		// Считаем размер: 32(SSID) + 32(Pass) + 4(int) + 4(float) + 1(int8) + 1(bool) + 4(crc) = 78 байт
		const buffer = new ArrayBuffer(78);
		const dv = new DataView(buffer);
		const enc = new TextEncoder();

		// 1. SSID (смещение 0, длина 32)
		const ssid = enc.encode(f.apSSID.value);
		new Uint8Array(buffer, 0, 32).set(ssid.slice(0, 31));

		// 2. Pass (смещение 32, длина 32)
		const pass = enc.encode(f.apPass.value);
		new Uint8Array(buffer, 32, 32).set(pass.slice(0, 31));

		// 3. logInterval (смещение 64, uint32)
		dv.setUint32(64, parseInt(f.logInterval.value), true);

		// 4. minHDOP (смещение 68, float32)
		dv.setFloat32(68, parseFloat(f.minHDOP.value), true);

		// 5. tzOffset (смещение 72, int8)
		dv.setInt8(72, parseInt(f.tzOffset.value));

		// 6. autoTZ (смещение 73, uint8/bool)
		dv.setUint8(73, f.autoTZ.checked ? 1 : 0);

		// 7. CRC (смещение 74) - можно забить нулями, ESP пересчитает сама
		dv.setUint32(74, 0, true);

		try {
			const response = await fetch('/config', {
				method: 'POST',
				body: buffer,
				headers: { 'Content-Type': 'application/octet-stream' }
			});

			if (response.ok) {
				alert("Настройки успешно сохранены!");
				bootstrap.Modal.getInstance(document.getElementById('configModal')).hide();
			} else {
				alert("Ошибка сервера: " + response.status);
			}
		} catch (err) {
			alert("Ошибка сети: " + err.message);
		}
	};
};

function initWebSocket() {
	if (ws) ws.close();

	ws = new WebSocket(`ws://${window.location.hostname}/ws`);
	ws.binaryType = "arraybuffer";

	ws.onopen = () => {
		console.log("WS Connected");
		updateConnStatus(true);
		clearTimeout(wsReconnectTimer);
	};

	ws.onmessage = (event) => {
		const dvFull = new DataView(event.data);
		const type = dvFull.getUint8(0);

		switch (type) {
			case 0x01: // Status Packet
				parseStatusPacket(new DataView(event.data, 1));
				break;

			case 0x02: // WiFi Header [Type][Count]
				expectedWifiCount = dvFull.getUint8(1);
				incomingWifi = [];
				if (expectedWifiCount === 0) renderWifiList([]);
				break;

			case 0x03: // WiFi Entry (Fixed 42 bytes) [Type][SSID(32)][BSSID(6)][RSSI(1)][Auth(1)][Chan(1)]
				parseWiFiEntry(event.data);
				break;
		}
	};

	ws.onclose = () => {
		updateConnStatus(false);
		wsReconnectTimer = setTimeout(initWebSocket, 3000);
	};
}

function updateConnStatus(online) {
	const badge = document.getElementById('status-indicator');
	if (online) {
		document.body.classList.remove('is-offline');
		badge.className = 'status-dot bg-success shadow-sm';
	} else {
		document.body.classList.add('is-offline');
		badge.className = 'status-dot bg-secondary shadow-sm';
	}
}

function updateTimeDisplay(ts) {
	if (!ts) return;
	const gpsDate = new Date(ts * 1000);
	const now = new Date();

	document.getElementById('time-gps').innerText = gpsDate.toLocaleTimeString();
	document.getElementById('time-browser').innerText = now.toLocaleTimeString();

	// Подсветка расхождения
	const diff = Math.abs(gpsDate - now);
	document.getElementById('time-gps').classList.toggle('text-danger', diff > 3000);
}

// --- ПАРСЕРЫ ---

function parseStatusPacket(dv) {
	let offset = 0;

	// GPS position log entry
	const gpsUnix = dv.getUint32(offset, true); offset += 4;
	const lat = dv.getFloat32(offset, true); offset += 4;
	const lon = dv.getFloat32(offset, true); offset += 4;
	const alt = dv.getInt16(offset, true); offset += 2;
	const hdop = dv.getUint16(offset, true); offset += 2;
	const battery = dv.getUint8(offset); offset += 1;
	const ap_count = dv.getUint8(offset); offset += 1;

	// Extended info entry
	const flashTotal = dv.getUint32(offset, true); offset += 4;
	const flashUsed = dv.getUint32(offset, true); offset += 4;
	const freeHeap = dv.getUint32(offset, true); offset += 4;
	const maxBlock = dv.getUint32(offset, true); offset += 4;
	const clients = dv.getUint8(offset); offset += 1;
	const points = dv.getUint32(offset, true); offset += 4;
	const sats_visible = dv.getUint8(offset); offset += 1;
	const sats_used = dv.getUint8(offset); offset += 1;

	// Спутники
	const sats = [];
	const SAT_SIZE = 6;  // [1 id][1 el][2 az][1 snr][1 tracked]

	while (offset + SAT_SIZE <= dv.byteLength) {
		const id = dv.getUint8(offset);
		if (id !== 0) {
			sats.push({
				id: id,
				el: dv.getUint8(offset + 1),
				az: dv.getUint16(offset + 2, true),
				snr: dv.getUint8(offset + 4),
				tracked: dv.getUint8(offset + 5)
			});
		}
		offset += SAT_SIZE;
	}

	//console.log("B: %d C: %d LAT: %f LNG: %f ALT: %f HDOP: %f TS: %d P: %d FLASH: %d/%d", battery, clients, lat, lon, alt, hdop, gpsUnix, points, flashUsed, flashTotal);

	// Обновляем текстовые поля UI
	document.getElementById('gps-pos').innerText = `${lat.toFixed(6)}, ${lon.toFixed(6)}`;
	document.getElementById('gps-alt').innerText = alt.toFixed(1);
	document.getElementById('gps-hdop').innerText = hdop.toFixed(2);
	document.getElementById('gps-count').innerText = points;
	document.getElementById('sys-clients').innerText = clients;
	document.getElementById('sys-bat').innerText = battery;

	// Flash bar
	const flashPct = (flashUsed / flashTotal * 100) || 0;
	document.getElementById('flash-bar').style.width = flashPct + '%';
	document.getElementById('sys-flash').innerText = `${(flashUsed / 1024).toFixed(0)} / ${(flashTotal / 1024).toFixed(0)} КБ`;

	// Рендер RAM
	const totalHeap = 81920; // 80 KB для ESP8266
	const heapPct = (freeHeap / totalHeap * 100);
	document.getElementById('ram-bar').style.width = (100 - heapPct) + '%'; // Показываем ЗАНЯТОЕ
	document.getElementById('sys-ram').innerText = `${(freeHeap / 1024).toFixed(1)} КБ своб.`;
	document.getElementById('sys-ram-max').innerText = maxBlock;

	// Цвет бара в зависимости от фрагментации
	const ramBar = document.getElementById('ram-bar');
	ramBar.className = 'progress-bar ' + (maxBlock < 8000 ? 'bg-danger' : (maxBlock < 15000 ? 'bg-warning' : 'bg-info'));

	// Время (GPS vs Browser)
	updateTimeDisplay(gpsUnix);

	// Обновляем счетчик на бейдже (Используется / Видно)
	const trackedCount = sats.filter(s => s.tracked).length;
	document.getElementById('gps-sats').innerText = `${trackedCount} / ${sats.length}`;

	// Отрисовка графиков SNR
	renderSatBars(sats);
}

function parseWiFiEntry(data) {
	const dv = new DataView(data);
	const ssid = new TextDecoder().decode(new Uint8Array(data, 1, 32)).replace(/\0/g, '');
	const bssid = Array.from(new Uint8Array(data, 33, 6)).map(b => b.toString(16).padStart(2, '0')).join(':');

	incomingWifi.push({
		ssid: ssid || '[Hidden]',
		bssid: bssid,
		rssi: dv.getInt8(39),
		auth: dv.getUint8(40),
		chan: dv.getUint8(41)
	});

	if (incomingWifi.length >= expectedWifiCount) renderWifiList(incomingWifi);
}

// --- ВИЗУАЛИЗАЦИЯ ---

function renderSatBars(sats) {
	const chart = document.getElementById('sat-chart');
	chart.innerHTML = sats.map(s => {
		let color = 'bg-danger';
		if (s.snr > 35) color = 'bg-success';
		else if (s.snr > 22) color = 'bg-warning';
		return `<div class="d-flex flex-column align-items-center flex-grow-1" style="max-width:20px">
            <span style="font-size:8px">${s.id}</span>
            <div class="progress w-100 rounded-0" style="height:70px; flex-direction:column-reverse; display:flex">
                <div class="progress-bar ${color}" style="height:${s.snr}%"></div>
            </div>
        </div>`;
	}).join('');
}

function renderWifiList(nets) {
	const tbody = document.getElementById('wifi-list');
	nets.sort((a, b) => b.rssi - a.rssi);
	tbody.innerHTML = nets.map(n => `<tr>
        <td class="ps-3">${n.ssid}</td>
        <td class="small text-muted">${n.bssid}</td>
        <td><small>${n.auth}</small></td>
        <td class="pe-3">
            <div class="d-flex align-items-center">
                <span class="badge bg-secondary me-2">CH ${n.chan}</span>
                <div class="progress flex-grow-1" style="height:6px; min-width:60px">
                    <div class="progress-bar ${n.rssi > -65 ? 'bg-success' : 'bg-warning'}" style="width:${Math.max(0, 100 + n.rssi * 1.2)}%"></div>
                </div>
            </div>
        </td>
    </tr>`).join('');
}

// --- НАСТРОЙКИ И ФАЙЛЫ ---

async function loadConfig() {
	const r = await fetch('/config');
	const buf = await r.arrayBuffer();
	const dv = new DataView(buf);
	const f = document.getElementById('configForm');

	f.apSSID.value = new TextDecoder().decode(buf.slice(0, 32)).replace(/\0/g, '');
	f.apPass.value = new TextDecoder().decode(buf.slice(32, 64)).replace(/\0/g, '');
	f.logInterval.value = dv.getUint32(64, true);
	f.minHDOP.value = dv.getFloat32(68, true).toFixed(2);
	f.autoTZ.checked = dv.getUint8(73) === 1;
	f.tzOffset.value = dv.getInt8(72);
	toggleTZUI();
}

function gpsToUnix(gpsSeconds, leapSeconds = 18) {
	const GPS_EPOCH_OFFSET = 315964800;
	return gpsSeconds + GPS_EPOCH_OFFSET - leapSeconds;
}

async function loadLogs() {
	try {
		const response = await fetch('/GetUsedBlocks');
		const text = await response.text();

		if (!text) return; // Если список пуст

		const pairs = text.split(',');
		const tbody = document.getElementById('log-list');
		tbody.innerHTML = ''; // Очистить старые данные

		pairs.forEach(pair => {
			const [id, ts] = pair.split('=');
			const row = `<tr>
                <td class="ps-3"><input type="checkbox" class="form-check-input block-checkbox" value="${id}" onchange="updateBlockButtons()"/></td>
                <td><a href="/download?id=${id}" class="fw-bold text-decoration-none">Блок #${id}</a></td>
                <td>64 KB</td>
                <td class="log-time pe-3" data-ts="${ts}">${formatDate(gpsToUnix(ts))}</td>
            </tr>`;
			document.getElementById('log-list').insertAdjacentHTML('beforeend', row);
		});

	} catch (e) {
		console.error("Ошибка загрузки блоков:", e);
	}
}

// --- ВСПОМОГАТЕЛЬНЫЕ ---

function updateBatteryColors(p) {
	const el = document.getElementById('bat-container');
	el.className = 'me-3 d-flex align-items-center fw-bold transition-color ' +
		(p < 15 ? 'text-danger' : (p < 35 ? 'text-warning' : 'text-success'));
}

function formatDate(ts) {
	const d = new Date(ts * 1000);
	return isUTC ? d.toISOString().slice(0, 19).replace('T', ' ') : d.toLocaleString();
}

function toggleTZUI() {
	const auto = document.getElementById('autoTZ').checked;
	document.getElementById('tzSelect').disabled = auto;
	document.getElementById('btnBrowserTZ').disabled = auto;
}

function setBrowserTZ() {
	document.getElementById('tzSelect').value = Math.round(-new Date().getTimezoneOffset() / 60);
}

function updateBlockButtons() {
	document.getElementById('btn-download-many').disabled = !document.querySelectorAll('.block-chk:checked').length;
}

function downloadSelected() {
	const ids = Array.from(document.querySelectorAll('.block-chk:checked')).map(c => `id=${c.value}`).join('&');
	window.location.href = `/block?${ids}`;
}

async function apiPost(url) {
	if (!confirm("Вы уверены?")) return;
	await fetch(url, { method: 'POST' });
}

async function loadTimezones(lang = 'en') {
	const response = await fetch(`tz_${lang}.json`);
	const regions = await response.json();
	const select = document.getElementById('tzSelect');

	// Cleanup
	select.innerHTML = '';

	for (const [region, offsets] of Object.entries(regions)) {
		for (const [offsetMin, cities] of Object.entries(offsets)) {
			const min = parseInt(offsetMin);

			// Форматируем строку (+HH:MM)
			const abs = Math.abs(min);
			const h = String(Math.floor(abs / 60)).padStart(2, '0');
			const m = String(abs % 60).padStart(2, '0');
			const offsetStr = `(${min >= 0 ? '+' : '-'}${h}:${m})`;

			cities.forEach(city => {
				const el = document.createElement('option');
				el.value = min * 60; // Передаем на ESP секунды
				el.textContent = `${region}/${city} ${offsetStr}`;
				select.appendChild(el);
			});
		}
	}
}
