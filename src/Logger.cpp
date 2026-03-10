#include "Logger.h"
#include <ESP8266WiFi.h>

// Глобальные объекты
Logger logger(SPI_CS_PIN);
GPS_Position_Info current;

// Вспомогательные функции для работы со структурами
// Если библиотека капризничает с шаблонами, используем побайтовую запись
template <typename T>
void flashWriteStruct(SPIFlash& f, uint32_t addr, const T& data) {
	uint8_t* ptr = (uint8_t*)&data;
	for (size_t i = 0; i < sizeof(T); i++) {
		f.writeByte(addr + i, ptr[i]);
	}
}

template <typename T>
void flashReadStruct(SPIFlash& f, uint32_t addr, T* data) {
	uint8_t* ptr = (uint8_t*)data;
	for (size_t i = 0; i < sizeof(T); i++) {
		ptr[i] = f.readByte(addr + i);
	}
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

		flashReadStruct(_flash, addr, &bh);
		Serial.printf("Block %3d@0x%08X: TS = %08X\n", i, addr, bh.timestamp);
		if (bh.magic != BLOCK_HEADER_MAGIC) {
			continue;
		}
		// NOTE: Timestamp, sometime, can be "0xFFFFFFFF" (prepared, but empty block)
		if (bh.timestamp >= MIN_VALID_TS && bh.timestamp > maxTs) {
			maxTs = bh.timestamp;
			latestBlockAddr = addr;

			// DEBUG: Dump first bytes
			uint8_t buff[32];
			_flash.readByteArray(addr, buff, 32);
			Serial.printf("> FOUND DATA:");
			for (uint8_t i = 0; i < 32; i++) {
				Serial.printf(" %02X", buff[i]);
			}
			Serial.println();

			_blocksUsed += 1;
		}
		// Blocks with wrong timestamp will be ignored here.
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
		_ptrBottom = DATA_BLOCK_SIZE - 1 - sizeof(AP_Info);

		// Search for GPS+SIGNALS records
		while (_ptrTop < DATA_BLOCK_SIZE - 64) {
			GPS_Position_Info tempGps;
			flashReadStruct(_flash, _currentBlockAddr + _ptrTop, &tempGps);

			if (tempGps.timestamp < MIN_VALID_TS || tempGps.timestamp == 0xFFFFFFFF) break;

			uint16_t recordSize = sizeof(GPS_Position_Info) + (tempGps.ap_count * sizeof(AP_Signal_Record));
			_ptrTop = align4(_ptrTop + recordSize);
		}

		while (_ptrBottom < _ptrTop) {
			AP_Info info;
			flashReadStruct(_flash, _currentBlockAddr + _ptrBottom, &info);

			if (info.ssid_len == 0xFF || info.channel_enc == 0xFF) break;

			// Fill cache
			CacheEntry entry;
			memcpy(entry.mac, &info.mac, 6); // Внимательно: тут mac[6] в структуре
			entry.offset = _ptrBottom;
			_blockCache.push_back(entry);

			_ptrBottom -= info.ssid_len - sizeof(AP_Info);
		}
	}

	Serial.printf("Logger ready. Blocks used: %d/%d. Current block: %d\n", _blocksUsed, blocksTotal(), _currentBlockAddr / DATA_BLOCK_SIZE);

	return true;
}

bool Logger::storeRecord(GPS_Position_Info& gps) {
	// If flash full and log rotation not enabled
	if (_isFull) { return false; }

	int networksFound = WiFi.scanComplete();
	// If there is no WiFi info available
	if (networksFound < 0) return false;

	uint32_t newApSpace = 0;
	std::vector<int> newNets;
	for (int i = 0; i < networksFound; i++) {
		if (getOffsetFromCache(WiFi.BSSID(i)) == 0) {
			newApSpace += (sizeof(AP_Info) + WiFi.SSID(i).length());
			newNets.push_back(i);
		}
	}

	uint32_t gpsSpace = sizeof(GPS_Position_Info) + (networksFound * sizeof(AP_Signal_Record));

	if (align4(_ptrTop + gpsSpace) >= (_ptrBottom - newApSpace)) {
		if (!prepareNextBlock()) return false;
		return storeRecord(gps);
	}

	// Save new nets (not listened in cache)
	for (int idx : newNets) {
		String ssid = WiFi.SSID(idx);
		uint8_t sLen = ssid.length();

		AP_Info info;
		memcpy(&info.mac, WiFi.BSSID(idx), 6);
		info.channel_enc = (WiFi.channel(idx) << 4) | (uint8_t)WiFi.encryptionType(idx);
		info.ssid_len = sLen;
		flashWriteStruct(_flash, _currentBlockAddr + _ptrBottom, info);
		// Save offset into cache
		addToCache((uint8_t*)&info.mac, _ptrBottom);
		// Save SSID (Wi-Fi ap name)
		_ptrBottom -= sLen;
		_flash.writeStr(_currentBlockAddr + _ptrBottom, ssid);
		_ptrBottom -= sizeof(AP_Info); // Prepare address for next data
	}

	flashWriteStruct(_flash, _currentBlockAddr + _ptrTop, gps);
	_ptrTop += sizeof(GPS_Position_Info);

	for (int i = 0; i < networksFound; i++) {
		AP_Signal_Record sig;
		sig.offset = getOffsetFromCache(WiFi.BSSID(i));
		sig.rssi = WiFi.RSSI(i);
		flashWriteStruct(_flash, _currentBlockAddr + _ptrTop, sig);
		_ptrTop += sizeof(AP_Signal_Record);
	}

	_ptrTop = align4(_ptrTop);
	_pointsSaved++;
	return true;
}

bool Logger::prepareNextBlock() {
	_currentBlockAddr += DATA_BLOCK_SIZE;
	if (_currentBlockAddr >= _flashSize) {
		_currentBlockAddr = 0;
	}

	// Check if block is empty
	uint16_t magic = 0;
	flashReadStruct(_flash, _currentBlockAddr, &magic);
	if (magic == BLOCK_HEADER_MAGIC && !_rotateLogs) {
		_isFull = true;
		return false;
	}

	if (!_flash.eraseBlock64K(_currentBlockAddr)) return false;

	magic = BLOCK_HEADER_MAGIC;
	flashWriteStruct(_flash, _currentBlockAddr, magic);
	_blockCache.clear();
	_ptrTop = 2; // skip Magic
	_ptrBottom = DATA_BLOCK_SIZE - 1 - sizeof(AP_Info);

	if (_blocksUsed + 1 < blocksTotal()) { _blocksUsed += 1; }
	return true;
}

void Logger::addToCache(uint8_t* mac, uint16_t offset) {
	if (_blockCache.size() < MAX_CACHE_SIZE) {
		CacheEntry e;
		memcpy(&e.mac, mac, 6);
		e.offset = offset;
		_blockCache.push_back(e);
	}
}

uint16_t Logger::getOffsetFromCache(uint8_t* mac) {
	for (auto const& e : _blockCache) {
		if (memcmp(&e.mac, mac, 6) == 0) return e.offset;
	}
	return 0;
}

void Logger::formatFlash() {
	uint32_t nextBlockID = _currentBlockAddr / DATA_BLOCK_SIZE;
	if (nextBlockID >= blocksTotal()) { nextBlockID = 0; }

	for (uint32_t i = 0; i < blocksTotal(); i++) {
		if (i == nextBlockID) { continue; } // Skip next block (will format it later)
		_flash.eraseBlock64K(i * DATA_BLOCK_SIZE);
		yield();
	}

	_isFull = false;
	_blocksUsed = 0;

	// Format and use next block from current
	bool oldState = _rotateLogs;
	_rotateLogs = true; // Hack - we need format next block in any way!
	prepareNextBlock();
	_rotateLogs = oldState;
}

bool Logger::getBlockPart(uint32_t blockIdx, uint32_t offset, uint8_t* buffer, size_t len) {
	uint32_t addr = (blockIdx * DATA_BLOCK_SIZE) + offset;
	for (size_t i = 0; i < len; i++) {
		buffer[i] = _flash.readByte(addr + i);
	}
	return true;
}

void Logger::getUsedBlockIDs(std::function<void(int)> onIdFound) {
	for (uint32_t i = 0; i < blocksTotal(); i++) {
		uint32_t addr = i * DATA_BLOCK_SIZE;
		BlockHeader bh;

		flashReadStruct(_flash, addr, &bh);
		if (bh.magic == BLOCK_HEADER_MAGIC && bh.timestamp != 0xFFFFFFFF) {
			onIdFound(i);
		}
	}
}