#include "Logger.h"
#include <ESP8266WiFi.h>

// Глобальные объекты
Logger logger(SPI_CS_PIN);

// Вспомогательные функции для работы со структурами
template <typename T>
void flashWriteStruct(SPIFlash& f, uint32_t addr, const T& data) {
	f.writeByteArray(addr, (uint8_t*)&data, sizeof(T));
}

template <typename T>
void flashReadStruct(SPIFlash& f, uint32_t addr, T& data) {
	f.readByteArray(addr, (uint8_t*)&data, sizeof(T));
}

Logger::Logger(uint8_t csPin) : _flash(csPin) {
	_blockCache.reserve(MAX_CACHED_APS);
}

bool Logger::begin() {
	if (!_flash.begin()) return false;

	_flashSize = _flash.getCapacity();
	_blocksUsed = 0;

	Serial.printf("Flash init done. JEDECID: %08X, Capacity: %d\n", _flash.getJEDECID(), _flashSize);

	// Search last used block
	uint32_t maxTs = 0;
	uint32_t latestBlockAddr = 0xFFFFFFFF; // impossible value as indicator "No blocks found"

	for (uint32_t i = 0; i < blocksTotal(); i++) {
		uint32_t addr = i * DATA_BLOCK_SIZE;
		BlockHeader bh;
		flashReadStruct(_flash, addr, bh);

		//Serial.printf("Block %3d@0x%08X: TS = %08X\n", i, addr, bh.timestamp);
		if (bh.magic != BLOCK_HEADER_MAGIC) continue;

		_blocksUsed++;

		// NOTE: Block can be "used", but "empty". TS=0xFFFFFFFF - correct and must be used first.
		if (bh.timestamp == 0xFFFFFFFF) {
			latestBlockAddr = addr;
			break;
		}
		else if (bh.timestamp >= MIN_VALID_TS && bh.timestamp > maxTs) {
			maxTs = bh.timestamp;
			latestBlockAddr = addr;
		}
	}

	// If no blocks found (flash empty of filled with wrong/random data)
	if (latestBlockAddr == 0xFFFFFFFF) {
		// Select random block (wear leveling)
		randomSeed(analogRead(0) + micros());
		_currentBlockAddr = random(0, blocksTotal()) * DATA_BLOCK_SIZE;
		prepareNextBlock(); // NOTE: Prepare NEXT block, not current!
	}
	else {
		_currentBlockAddr = latestBlockAddr;

		_blockCache.clear();
		_ptrTop = 2; // skip Magic
		_ptrBottom = DATA_BLOCK_SIZE - sizeof(AP_Info);

		// Search for GPS+SIGNALS records
		while (_ptrTop < DATA_BLOCK_SIZE - sizeof(GPS_Position_Info)) {
			GPS_Position_Info tempGps;
			flashReadStruct(_flash, _currentBlockAddr + _ptrTop, tempGps);

			if (tempGps.timestamp < MIN_VALID_TS || tempGps.timestamp == 0xFFFFFFFF) break;

			_ptrTop += sizeof(GPS_Position_Info) + (tempGps.signals * sizeof(AP_Signal_Record));
		}

		// Restore AP_Info data
		while (_ptrBottom > _ptrTop) {
			AP_Info info;
			flashReadStruct(_flash, _currentBlockAddr + _ptrBottom, info);
			if (info.ssid_len == 0xFF || info.channel_enc == 0xFF) break;

			addToCache(info.mac, _ptrBottom);
			_ptrBottom -= (info.ssid_len + sizeof(AP_Info));
		}
	}

	Serial.printf("Logger ready. Blocks used: %d/%d. Current block: %d\n", _blocksUsed, blocksTotal(), _currentBlockAddr / DATA_BLOCK_SIZE);

	return true;
}

bool Logger::storeRecord(uint32_t ts, float lat, float lon, int16_t alt, uint16_t acc, uint8_t charge) {
	if (_isFull) return false;

	int networksFound = WiFi.scanComplete();
	if (networksFound < 0) return false;

	// 1. Подготовка данных GPS
	GPS_Position_Info point;
	point.timestamp = ts;
	point.lat = lat;
	point.lon = lon;
	point.alt = alt;
	point.acc = acc;
	point.bat_charge = charge;
	point.signals = (uint8_t)networksFound;

	// 2. Анализируем, какие AP нужно записать (новые или изменившиеся)
	uint16_t newApSpace = 0;
	std::vector<int> newNetsIdx;

	for (int i = 0; i < networksFound; i++) {
		uint8_t* mac = WiFi.BSSID(i);
		String ssid = WiFi.SSID(i);
		uint8_t sLen = (ssid.length() > 32) ? 32 : ssid.length();

		uint8_t ch = WiFi.channel(i);
		uint8_t enc = WiFi.encryptionType(i);
		if (enc > 0x0E) enc = 0;
		uint8_t currentChEnc = ((ch & 0x0F) << 4) | (enc & 0x0F);

		uint16_t cachedOffset = getOffsetFromCache(mac);

		// Проверяем: нет в кэше/памяти ИЛИ данные изменились
		if (cachedOffset == 0 || isApChanged(mac, cachedOffset, currentChEnc, ssid)) {
			newApSpace += (sizeof(AP_Info) + sLen);
			newNetsIdx.push_back(i);
		}
	}

	// 3. Проверка места в блоке
	// gpsSpace: сама структура + записи сигналов (RSSI + offset) для всех найденных сетей
	uint16_t gpsSpace = sizeof(GPS_Position_Info) + (networksFound * sizeof(AP_Signal_Record));

	if ((gpsSpace + newApSpace) > (_ptrBottom - _ptrTop)) {
		if (!prepareNextBlock()) return false;
		return storeRecord(ts, lat, lon, alt, acc, charge);
	}

	// 4. Запись новых/изменившихся AP во Flash (двигаемся от хвоста к голове)
	for (int idx : newNetsIdx) {
		String ssid = WiFi.SSID(idx);
		uint8_t sLen = (ssid.length() > 32) ? 32 : ssid.length();

		AP_Info info;
		memcpy(info.mac, WiFi.BSSID(idx), 6);
		uint8_t ch = WiFi.channel(idx);
		uint8_t enc = WiFi.encryptionType(idx);
		if (enc > 0x0E) enc = 0;
		info.channel_enc = ((ch & 0x0F) << 4) | (enc & 0x0F);
		info.ssid_len = sLen;

		// Пишем структуру (она всегда по текущему адресу _ptrBottom)
		flashWriteStruct(_flash, _currentBlockAddr + _ptrBottom, info);

		// Сразу добавляем в кэш (теперь этот MAC привязан к этому смещению)
		addToCache(info.mac, _ptrBottom);

		// Пишем SSID слева от структуры
		_ptrBottom -= sLen;
		if (sLen > 0) {
			_flash.writeByteArray(_currentBlockAddr + _ptrBottom, (uint8_t*)ssid.c_str(), sLen);
		}

		// Сдвигаем указатель для следующей записи AP
		_ptrBottom -= sizeof(AP_Info);
	}

	// 5. Запись точки GPS
	flashWriteStruct(_flash, _currentBlockAddr + _ptrTop, point);
	_ptrTop += sizeof(GPS_Position_Info);

	// 6. Запись уровней сигналов со ссылками на AP_Info
	for (int i = 0; i < networksFound; i++) {
		AP_Signal_Record sig;
		sig.offset = getOffsetFromCache(WiFi.BSSID(i)); // Теперь точно вернет смещение (из RAM или Flash)
		sig.rssi = (int8_t)WiFi.RSSI(i);
		flashWriteStruct(_flash, _currentBlockAddr + _ptrTop, sig);
		_ptrTop += sizeof(AP_Signal_Record);
	}

	_pointsSaved++;
	return true;
}

bool Logger::prepareNextBlock() {
	uint32_t nextAddr = _currentBlockAddr + DATA_BLOCK_SIZE;
	if (nextAddr >= _flashSize) nextAddr = 0;

	// Сначала проверяем, можно ли использовать следующий блок
	BlockHeader bh;
	flashReadStruct(_flash, nextAddr, bh);

	if (bh.magic == BLOCK_HEADER_MAGIC && bh.timestamp != 0xFFFFFFFF && !_rotateLogs) {
		_isFull = true;
		return false;
	}

	// Switch to new block
	_currentBlockAddr = nextAddr;

	// Erase only if not empty ()
	if (bh.magic != BLOCK_HEADER_MAGIC || bh.timestamp != 0xFFFFFFFF) {
		if (!_flash.eraseBlock64K(_currentBlockAddr)) return false;
		uint16_t magic = BLOCK_HEADER_MAGIC;
		_flash.writeWord(_currentBlockAddr, magic);
	}

	_blockCache.clear();
	_cacheIndex = 0;
	_ptrTop = 2; // skip Magic
	_ptrBottom = DATA_BLOCK_SIZE - sizeof(AP_Info);

	// Увеличиваем счетчик занятых блоков (но не выше максимума)
	if (_blocksUsed < blocksTotal()) {
		_blocksUsed++;
	}

	return true;
}

bool Logger::isApChanged(uint8_t* mac, uint16_t offset, uint8_t newChannelEnc, String newSsid) {
	AP_Info cachedInfo;
	flashReadStruct(_flash, _currentBlockAddr + offset, cachedInfo);

	if (cachedInfo.channel_enc != newChannelEnc) return true;

	uint8_t newLen = (newSsid.length() > 32) ? 32 : newSsid.length();
	if (cachedInfo.ssid_len != newLen) return true;

	if (newLen > 0) {
		uint8_t buffer[32];
		_flash.readByteArray(_currentBlockAddr + offset - newLen, buffer, newLen);
		if (memcmp(buffer, newSsid.c_str(), newLen) != 0) return true;
	}
	return false;
}

uint16_t Logger::findApInFlash(uint8_t* mac) {
	uint16_t offset = DATA_BLOCK_SIZE - sizeof(AP_Info);

	while (offset > _ptrBottom) {
		AP_Info info;
		flashReadStruct(_flash, _currentBlockAddr + offset, info);

		if (info.ssid_len > 32) break;

		if (memcmp(info.mac, mac, 6) == 0) {
			return offset;
		}

		uint16_t step = info.ssid_len + sizeof(AP_Info);
		if (offset < step) break; // Защита от выхода за 0
		offset -= step;
	}
	return 0;
}

void Logger::addToCache(uint8_t* mac, uint16_t offset) {
	// Проверяем, нет ли уже этого MAC, чтобы просто обновить offset
	for (auto& e : _blockCache) {
		if (memcmp(e.mac, mac, 6) == 0) {
			e.offset = offset;
			return;
		}
	}

	if (_blockCache.size() < MAX_CACHED_APS) {
		_blockCache.push_back({ {0}, offset });
		memcpy(_blockCache.back().mac, mac, 6);
	}
	else {
		// Заменяем самую старую запись
		memcpy(_blockCache[_cacheIndex].mac, mac, 6);
		_blockCache[_cacheIndex].offset = offset;
		_cacheIndex = (_cacheIndex + 1) % MAX_CACHED_APS;
	}
}

uint16_t Logger::getOffsetFromCache(uint8_t* mac) {
	// 1. Ищем в RAM
	for (const auto& e : _blockCache) {
		if (memcmp(e.mac, mac, 6) == 0) return e.offset;
	}

	// 2. Ищем во Flash
	uint16_t offset = findApInFlash(mac);

	// 3. Если нашли, кэшируем, чтобы не дергать SPI снова
	if (offset != 0) {
		addToCache(mac, offset);
	}

	return offset;
}

void Logger::eraseFlash(std::function<void(int)> onProgress) {
	uint32_t total = blocksTotal();
	uint32_t nextBlockID = ((_currentBlockAddr / DATA_BLOCK_SIZE) + 1) % total;

	for (uint32_t i = 0; i < total; i++) {
		int percent = (i * 100) / total;
		if (onProgress) onProgress(percent);

		if (i == nextBlockID) { continue; } // Skip next block (will format it later)
		_flash.eraseBlock64K(i * DATA_BLOCK_SIZE);
		delay(1);
	}

	_isFull = false;
	_blocksUsed = 0;

	// Format and use next block from current
	bool oldState = _rotateLogs;
	_rotateLogs = true; // Hack - we need format next block in any way!
	prepareNextBlock();
	_rotateLogs = oldState;

	Serial.println(F("FLASH ERASED."));

	_requestedErase = false;
}

bool Logger::getBlockPart(uint32_t blockIdx, uint32_t offset, uint8_t* buffer, size_t len) {
	uint32_t addr = (blockIdx * DATA_BLOCK_SIZE) + offset;
	return _flash.readByteArray(addr, buffer, len);
}

void Logger::getUsedBlockIDs(std::function<void(int, uint32_t)> onIdFound) {
	//Serial.println("> getUsedBlockIDs()");
	for (uint32_t i = 0; i < blocksTotal(); i++) {
		BlockHeader bh;
		flashReadStruct(_flash, i * DATA_BLOCK_SIZE, bh);

		//Serial.printf(">> #%3d: %04X %d\n", i, bh.magic, bh.timestamp);

		// Проверяем валидность блока
		if (bh.magic == BLOCK_HEADER_MAGIC && bh.timestamp != 0xFFFFFFFF) {
			onIdFound(i, bh.timestamp);
		}
	}
}