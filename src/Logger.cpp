#include "Logger.h"
#include <ESP8266WiFi.h>

// Глобальные объекты
Logger logger(SPI_CS_PIN);
GPS_Position_Info current;

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
	_blockCache.reserve(MAX_CACHE_SIZE);
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

			_ptrTop += sizeof(GPS_Position_Info) + (tempGps.ap_count * sizeof(AP_Signal_Record));
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

bool Logger::storeRecord(GPS_Position_Info& gps) {
	// If flash full and log rotation not enabled
	if (_isFull) return false;

	int networksFound = WiFi.scanComplete();
	// If there is no WiFi info available
	if (networksFound < 0) return false;
	gps.ap_count = networksFound; // Just to be sure its same.

	uint16_t newApSpace = 0;
	std::vector<int> newNets;
	for (int i = 0; i < networksFound; i++) {
		if (getOffsetFromCache(WiFi.BSSID(i)) == 0) {
			newApSpace += (sizeof(AP_Info) + WiFi.SSID(i).length());
			newNets.push_back(i);
		}
	}

	uint16_t gpsSpace = sizeof(GPS_Position_Info) + (networksFound * sizeof(AP_Signal_Record));

	if ((gpsSpace + newApSpace) > (_ptrBottom - _ptrTop)) {
		if (!prepareNextBlock()) return false;
		return storeRecord(gps);
	}

	// Save new AP_Info (not listened in cache)
	for (int idx : newNets) {
		String ssid = WiFi.SSID(idx);
		uint8_t sLen = (ssid.length() > 32) ? 32 : ssid.length();

		AP_Info info;
		memcpy(info.mac, WiFi.BSSID(idx), 6);
		uint8_t ch = WiFi.channel(idx);
		uint8_t enc = WiFi.encryptionType(idx);
		if (enc > 0x0E) enc = 0;  // This happens from time to time - encode can return 0xFF...
		info.channel_enc = ((ch & 0x0F) << 4) | (enc & 0x0F);
		info.ssid_len = sLen;
		flashWriteStruct(_flash, _currentBlockAddr + _ptrBottom, info);
		addToCache(info.mac, _ptrBottom);
		// Save SSID (Wi-Fi ap name)
		_ptrBottom -= sLen;
		if (sLen > 0) _flash.writeByteArray(_currentBlockAddr + _ptrBottom, (uint8_t*)ssid.c_str(), sLen);
		_ptrBottom -= sizeof(AP_Info); // Prepare address for next data
	}

	// Save GPS_Position_Info
	flashWriteStruct(_flash, _currentBlockAddr + _ptrTop, gps);
	_ptrTop += sizeof(GPS_Position_Info);

	for (int i = 0; i < networksFound; i++) {
		AP_Signal_Record sig;
		sig.offset = getOffsetFromCache(WiFi.BSSID(i));
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
	_ptrTop = 2; // skip Magic
	_ptrBottom = DATA_BLOCK_SIZE - sizeof(AP_Info);

	// Увеличиваем счетчик занятых блоков (но не выше максимума)
	if (_blocksUsed < blocksTotal()) {
		_blocksUsed++;
	}

	return true;
}

void Logger::addToCache(uint8_t* mac, uint16_t offset) {
	if (_blockCache.size() >= MAX_CACHE_SIZE) return;
	CacheEntry e;
	memcpy(e.mac, mac, 6);
	e.offset = offset;
	_blockCache.push_back(e);
}

uint16_t Logger::getOffsetFromCache(uint8_t* mac) {
	for (const auto& e : _blockCache) {
		if (memcmp(e.mac, mac, 6) == 0) return e.offset;
	}
	return 0;
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