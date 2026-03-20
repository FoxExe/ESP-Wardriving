#pragma once
#include <Arduino.h>
#include <SPIMemory.h>
#include <vector>

// Константы
#define SPI_CS_PIN            16
#define DATA_BLOCK_SIZE       (64 * 1024) // !DO NOT CHANGE! (Or change all "_flash.eraseBlock64K()" in code!)
#define BLOCK_HEADER_MAGIC    0xDDCC
#define NEOGPS_TS_OFFSET      946684800UL
#define MIN_VALID_TS          (1767225600UL - NEOGPS_TS_OFFSET) // Unix timestamp. 01.01.2026
#define MAX_CACHE_SIZE        1024  // 6 (MAC) + 2(Offset) = 8 bytes per record. 8 * 1024 = 8KB RAM

#pragma pack(push, 1)
struct BlockHeader {
	uint16_t magic;  // Must be = BLOCK_HEADER_MAGIC
	uint32_t timestamp; // TS from first [GPS_Position_Info] record
};

// Структура описания точки доступа (8 байт)
struct AP_Info {
	uint8_t mac[6];
	uint8_t channel_enc;
	uint8_t ssid_len;
};

// Ссылка на точку в записи GPS (3 байта) - исправлено под uint16_t offset
struct AP_Signal_Record {
	uint16_t offset;  // Смещение от начала блока (0..65535)
	int8_t rssi;
};

// Основные данные GPS (18 байт)
struct GPS_Position_Info {
	uint32_t timestamp;
	float lat;
	float lon;
	int16_t alt;
	uint16_t accuracy;
	uint8_t bat_charge;
	uint8_t ap_count;
};

struct CacheEntry {
	uint8_t mac[6];
	uint16_t offset; // Смещение внутри блока
};
#pragma pack(pop)

class Logger {
private:
	SPIFlash _flash;

	uint32_t _currentBlockAddr = 0; // Физический адрес начала текущего блока
	uint16_t _ptrTop = 0;           // Указатель записи данных (сверху)
	uint16_t _ptrBottom = 0;        // Указатель записи AP_Info (снизу)
	uint32_t _flashSize = 8 * 1024 * 1024; // 8MB for Winbond 25Q64, updated in begin()
	uint32_t _blocksUsed = 0;

	std::vector<CacheEntry> _blockCache;

	uint32_t _pointsSaved = 0;      // Счетчик за текущий сеанс
	bool _rotateLogs = true;        // Флаг циклической записи
	bool _isFull = false;           // Флаг остановки при заполнении
	bool _requestedErase = false;

	// Внутренние методы
	uint16_t align4(uint16_t addr) { return (addr + 3) & ~3; }
	void addToCache(uint8_t* mac, uint16_t offset);
	uint16_t getOffsetFromCache(uint8_t* mac);
public:
	Logger(uint8_t csPin);

	// Поиск последнего блока, вернёт False в случае ошибок
	bool begin();

	// Основной метод записи
	bool storeRecord(GPS_Position_Info& gps);

	// Поблочное форматирование
	void requestErase() { _requestedErase = true; }
	bool needErase() { return _requestedErase; }
	void eraseFlash(std::function<void(int)> onProgress);
	bool prepareNextBlock();

	// Управление и статистика
	void setRotation(bool enable) { _rotateLogs = enable; }
	bool isFull() { return _isFull; }
	uint32_t blocksTotal() { return _flashSize / DATA_BLOCK_SIZE; }
	uint32_t blocksFree() { return blocksTotal() - blocksUsed(); }
	uint32_t blocksUsed() { return _blocksUsed; }
	uint16_t getCurrentBlockID() { return (uint16_t)(_currentBlockAddr / DATA_BLOCK_SIZE); }
	uint32_t flashSize() { return _flashSize; }
	uint32_t pointsSaved() { return _pointsSaved; }
	uint8_t getUsagePercentage() { return (uint8_t)((blocksUsed() * 100) / blocksTotal()); };
	void getUsedBlockIDs(std::function<void(int, uint32_t)> onIdFound);

	// Метод для AsyncWebServer (чтение блока порциями)
	bool getBlockPart(uint32_t blockIdx, uint32_t offset, uint8_t* buffer, size_t len);
};

extern Logger logger;
extern GPS_Position_Info current;
