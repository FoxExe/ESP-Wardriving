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

	document.getElementById('configForm').onsubmit = async (e) => {
		e.preventDefault();
		const formData = new FormData(e.target);

		try {
			const response = await fetch('/config', {
				method: 'POST',
				body: formData
			});

			const result = await response.text();
			if (result === "OK") {
				alert("Настройки сохранены!");
				bootstrap.Modal.getInstance(document.getElementById('configModal')).hide();
			}
			else if (result === "RECONNECT") {
				const newSSID = document.getElementsByName('ssid')[0].value;
				alert(`ВНИМАНИЕ!\nНастройки точки доступа изменены.\nСейчас вы будете отключены.\nПереподключитесь к сети: ${newSSID}`);

				// Закрываем модалку и, например, редиректим на пустую страницу через 3 сек
				bootstrap.Modal.getInstance(document.getElementById('configModal')).hide();
				setTimeout(() => { window.location.reload(); }, 5000);
			}
			else {
				alert("Ошибка: " + result);
			}
		} catch (err) {
			alert("Ошибка сети: " + err.message);
		}
	};

	document.getElementById('log-list').addEventListener('change', (e) => {
		if (e.target.type === 'checkbox') {
			const main = document.getElementById('logsCheckbox');
			const all = document.querySelectorAll('#log-list input[type="checkbox"]');
			const checked = document.querySelectorAll('#log-list input[type="checkbox"]:checked');

			// Главный чекбокс активен только если выбраны ВСЕ под-чекбоксы
			main.checked = (all.length === checked.length);
			// Состояние "неопределенности", если выбрана часть (визуальный минус в боксе)
			main.indeterminated = (checked.length > 0 && checked.length < all.length);
		}
	});

	// Подсказки Bootstrap (работает весьма криво)
	//document.querySelectorAll('[data-toggle="tooltip"]').forEach(t => new bootstrap.Tooltip(t));
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
		const packet = new DataView(event.data);
		const type = packet.getUint8(0);

		/*
		#define REPORT_SYS 0x01
		#define REPORT_GPS 0x02
		#define REPORT_WIFI 0x03
		*/

		switch (type) {
			case 0x01: // REPORT_SYS
				parseStatusPacket(packet);
				break;

			case 0x02: // REPORT_GPS
				parseGPSPacket(packet);
				break;

			case 0x03: // REPORT_WIFI
				parseWiFiPacket(packet);
				break;
		}
	};

	ws.onclose = () => {
		updateConnStatus(false);
		wsReconnectTimer = setTimeout(initWebSocket, 3000);
	};
}

function showAlert(message, type = 'info') {
	const container = document.querySelector('body > .container');
	if (!container) return;

	const icons = {
		success: 'icon-success',
		danger: 'icon-warning',
		warning: 'icon-warning',
		info: 'icon-info'
	};

	const alert = document.createElement('div');
	alert.className = `alert alert-${type} alert-dismissible fade show d-flex align-items-center`;
	alert.setAttribute('role', 'alert');

	alert.innerHTML = `
		<svg class="bi flex-shrink-0 me-2" width="20" height="20" role="img">
			<use xlink:href="#${icons[type] || 'icon-info'}"/>
		</svg>
		<div>${message}</div>
		<button type="button" class="btn-close" data-bs-dismiss="alert" aria-label="Close"></button>
	`;

	container.prepend(alert);
}

function getAuthName(enc) {
	const map = { 2: 'WPA', 4: 'WPA2', 5: 'WEP', 7: 'Открытая', 8: 'WPA/WPA2' };
	return map[enc] || 'Unknown';
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
	const gpsDate = new Date((ts) * 1000);
	const now = new Date();

	document.getElementById('time-gps').innerText = gpsDate.toLocaleString(); // toLocaleTimeString()
	document.getElementById('time-browser').innerText = now.toLocaleString();

	// Подсветка расхождения
	const diff = Math.abs(gpsDate - now);
	document.getElementById('time-gps').classList.toggle('text-danger', diff > 3000);
}

// --- ПАРСЕРЫ ---
function parseStatusPacket(packet) {
	let offset = 1; // Skip packet type

	// GPS position log entry
	const battery = packet.getUint8(offset); offset += 1;
	const blocksTotal = packet.getUint32(offset, true); offset += 4;
	const blocksUsed = packet.getUint32(offset, true); offset += 4;
	const freeHeap = packet.getUint32(offset, true); offset += 4;
	const maxBlock = packet.getUint32(offset, true); offset += 4;
	const clients = packet.getUint8(offset); offset += 1;
	const points = packet.getUint32(offset, true); offset += 4;
	const current_block = packet.getUint16(offset, true); offset += 2;

	// Обновляем текстовые поля UI
	document.getElementById('sys-bat').innerText = battery;
	document.getElementById('sys-clients').innerText = clients;
	document.getElementById('gps-count').innerText = points;
	document.getElementById('current-block').innerText = current_block;

	// Flash bar
	const flashPct = (blocksUsed / blocksTotal * 100) || 0;
	document.getElementById('flash-bar').style.width = flashPct + '%';
	document.getElementById('sys-flash').innerText = `${blocksUsed} / ${blocksTotal.toFixed(0)}`;

	// Рендер RAM
	const totalHeap = 81920; // 80 KB для ESP8266
	const heapPct = (freeHeap / totalHeap * 100);
	document.getElementById('ram-bar').style.width = (100 - heapPct) + '%'; // Показываем ЗАНЯТОЕ
	document.getElementById('sys-ram').innerText = `${(freeHeap / 1024).toFixed(1)} КБ своб.`;
	document.getElementById('sys-ram-max').innerText = maxBlock;

	// Цвет бара в зависимости от фрагментации
	const ramBar = document.getElementById('ram-bar');
	ramBar.className = 'progress-bar ' + (maxBlock < 8000 ? 'bg-danger' : (maxBlock < 15000 ? 'bg-warning' : 'bg-info'));
}

function parseGPSPacket(packet) {
	let offset = 1; // Skip packet type

	const gpsUnix = packet.getUint32(offset, true); offset += 4;
	const lat = packet.getFloat32(offset, true); offset += 4;
	const lon = packet.getFloat32(offset, true); offset += 4;
	const alt = packet.getInt16(offset, true); offset += 2;
	const acc = packet.getUint16(offset, true); offset += 2;

	// Спутники
	const sats = [];
	const SAT_SIZE = 6; // 6 bytes NMEAGPS::satellite_view_t
	while (offset + SAT_SIZE <= packet.byteLength) {
		const id = packet.getUint8(offset);
		if (id !== 0) {
			sats.push({
				id: id,
				el: packet.getUint8(offset + 1),
				az: packet.getUint16(offset + 2, true),
				snr: packet.getUint8(offset + 4),
				tracked: packet.getUint8(offset + 5)
			});
		}
		offset += SAT_SIZE;
	}

	document.getElementById('gps-pos').innerText = `${lat.toFixed(6)}, ${lon.toFixed(6)}`;
	document.getElementById('gps-alt').innerText = alt.toFixed(1);
	document.getElementById('gps-hdop').innerText = acc.toFixed(2);

	// Время (GPS vs Browser)
	updateTimeDisplay(gpsUnix);

	// Обновляем счетчик на бейдже (Используется / Видно)
	const trackedCount = sats.filter(s => s.tracked).length;
	document.getElementById('gps-sats').innerText = `${trackedCount} / ${sats.length}`;

	// Отрисовка графиков SNR
	const chart = document.getElementById('sat-chart');
	chart.innerHTML = sats.map(s => {
		let color = 'bg-danger';
		if (s.snr > 35) color = 'bg-success';
		else if (s.snr > 22) color = 'bg-warning';
		return `<div class="d-flex flex-column align-items-center flex-grow-1" style="max-width:20px" title="EL: ${s.el}, AZ: ${s.az}">
            <span style="font-size:8px">${s.id}</span>
            <div class="progress w-100 rounded-0" style="height:70px; flex-direction:column-reverse; display:flex">
                <div class="progress-bar ${color}" style="height:${s.snr}%"></div>
            </div>
        </div>`;
	}).join('');
}

function parseWiFiPacket(packet) {
	const tbody = document.getElementById('wifi-list');
	tbody.innerHTML = '';  // Cleanup table

	let offset = 1; // Skip packet type
	const decoder = new TextDecoder();

	while (offset < packet.byteLength) {
		const macArray = new Uint8Array(packet.buffer, packet.byteOffset + offset, 6);
		const mac = Array.from(macArray)
			.map(b => b.toString(16).padStart(2, '0'))
			.join(':').toUpperCase();
		offset += 6;

		const rssi = packet.getInt8(offset++);
		const chan = packet.getUint8(offset++);
		const enc = packet.getUint8(offset++);
		const ssidLen = packet.getUint8(offset++);

		const ssidArray = new Uint8Array(packet.buffer, packet.byteOffset + offset, ssidLen);
		const ssid = decoder.decode(ssidArray);
		offset += ssidLen;

		// Signal -30 .. -90 dbm to 0..100%
		const signalPercent = Math.min(100, Math.max(0, Math.round((rssi + 90) * (100 / 60))));
		const tr = document.createElement('tr');

		tr.innerHTML = `
			<td class="ps-3">${ssid || '<em>&lt;Hidden&gt;</em>'}</td>
			<td class="small text-muted">${mac}</td>
			<td><small>${getAuthName(enc)}</small></td>
			<td class="pe-3" title="${rssi} dBm">
				<div class="d-flex align-items-center">
					<span class="badge bg-secondary me-2">CH ${chan}</span>
					<div class="progress flex-grow-1" style="height:6px; min-width:60px">
						<div class="progress-bar ${rssi > -65 ? 'bg-success' : 'bg-warning'}" 
								style="width:${signalPercent}%">
						</div>
					</div>
				</div>
			</td>`;
		tbody.appendChild(tr);
	}
}

// --- НАСТРОЙКИ И ФАЙЛЫ ---
async function loadConfig() {
	try {
		const r = await fetch('/config');
		const data = await r.json();
		const f = document.getElementById('configForm');

		// Заполняем поля по именам (name)
		f.ssid.value = data.ssid;
		f.pass.value = data.pass;
		f.scan_interval.value = data.scan_interval;
		f.min_acc.value = data.min_acc;
		f.tzOffset.value = data.tz_offset;

		// Чекбоксы заполняем через .checked
		f.autoTZ.checked = data.auto_tz;
		f.rotate_logs.checked = data.rotate_logs;

		// Обновляем UI (если есть зависимые поля)
		toggleTZUI();

	} catch (err) {
		showAlert("Ошибка: " + err, "danger")
		console.error("Ошибка:", err);
	}
}

async function rescanNetworks(btn) {
	// 1. Блокируем кнопку
	if (btn) btn.disabled = true;

	try {
		const tbody = document.getElementById('wifi-list');
		tbody.innerHTML = '<tr><td colspan="5" class="text-center text-muted">Сканирование...</td></tr>';

		const response = await fetch('/ScanWiFi', { method: 'POST' });
		const result = await response.text();

		switch (result) {
			case "OK":
				// Тут можно запустить таймер или WebSocket ожидание результатов
				break;
			case "-1":
				tbody.innerHTML = '<tr><td colspan="5" class="text-center text-danger">Ошибка запуска сканирования</td></tr>';
				break;
			default:
				showAlert("HTTP Error: " + result, "warning")
				console.warn("Unknown response:", result);
				break;
		}

	} catch (err) {
		showAlert("Ошибка: " + err, "danger")
		console.error("Ошибка:", err);
	} finally {
		// 2. Разблокируем кнопку обратно через пару секунд (чтобы не спамили)
		setTimeout(() => {
			if (btn) btn.disabled = false;
		}, 1000);
	}
}

async function loadLogs(btn) {
	if (btn) btn.disabled = true;

	try {
		const tbody = document.getElementById('log-list');
		tbody.innerHTML = '<tr><td colspan="5" class="text-center text-muted">Обновляю...</td></tr>';

		const response = await fetch('/GetUsedBlocks');
		const text = await response.text();
		tbody.innerHTML = ''; // Очистить старые данные

		if (!text) return; // Если список пуст

		const pairs = text.split(',');

		pairs.forEach(pair => {
			const [id, ts] = pair.split('=');
			const row = `<tr>
				<td class="ps-3"><input type="checkbox" class="form-check-input block-checkbox" value="${id}" onchange="updateBlockButtons()"/></td>
				<td><a href="/download?id=${id}" class="fw-bold text-decoration-none">Блок #${id}</a></td>
				<td>64 KB</td>
				<td class="log-time pe-3" data-ts="${ts}">${formatDate(ts)}</td>
			</tr>`;
			document.getElementById('log-list').insertAdjacentHTML('beforeend', row);
		});

		document.getElementById('logsCheckbox').checked = false;
		updateBlockButtons();

	} catch (err) {
		showAlert("Ошибка: " + err, "danger")
		console.error("Ошибка:", err);
	} finally {
		// 2. Разблокируем кнопку обратно через пару секунд (чтобы не спамили)
		setTimeout(() => {
			if (btn) btn.disabled = false;
		}, 1000);
	}
}

async function skipBlock(btn) {
	if (btn) btn.disabled = true;

	try {
		const response = await fetch('/SkipBlock', { method: 'POST' });
		const result = await response.text();
		if (result != "OK") { throw new Error(result); }
	} catch (err) {
		showAlert("Ошибка: " + err, "danger")
		console.error("Ошибка:", err);
	} finally {
		setTimeout(() => {
			if (btn) btn.disabled = false;
		}, 5000);
	}
}

async function eraseFlash(btn) {
	if (btn) btn.disabled = true;

	try {
		const tbody = document.getElementById('log-list');
		const response = await fetch('/EraseFlash', { method: 'POST' });
		const result = await response.text();
		if (result != "OK") { throw new Error(result); }
		tbody.innerHTML = '<tr><td colspan="5" class="text-center text-muted">- пусто -</td></tr>';
	} catch (err) {
		showAlert("Ошибка: " + err, "danger")
		console.error("Ошибка:", err);
	} finally {
		setTimeout(() => {
			if (btn) btn.disabled = false;
		}, 5000);
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
	document.getElementById('tzSelect').value = Math.round(-new Date().getTimezoneOffset());
}

function updateBlockButtons() {
	const selectedCount = document.querySelectorAll('.block-checkbox:checked').length;
	document.getElementById('btn-download-many').disabled = selectedCount === 0;
}

function downloadSelected() {
	const selected = document.querySelectorAll('.block-checkbox:checked');
	if (selected.length === 0) return;

	const ids = Array.from(selected).map(c => `id=${c.value}`).join('&');
	window.location.href = `/download?${ids}`;
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
				el.value = min;
				el.textContent = `${region}/${city} ${offsetStr}`;
				select.appendChild(el);
			});
		}
	}
}

function toggleAllBlocks(mainBox) {
	const checkboxes = document.querySelectorAll('#log-list .block-checkbox');

	checkboxes.forEach(cb => {
		cb.checked = mainBox.checked;
		const row = cb.closest('tr');
		if (row) {
			row.classList.toggle('table-active', cb.checked);
		}
	});

	updateBlockButtons();
}
