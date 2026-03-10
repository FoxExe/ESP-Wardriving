#include <LittleFS.h>


// --- Конфигурация по умолчанию ---
#define CFG_FILE "/settings.bin"
#pragma pack(push, 1)

struct ConfigData {
	uint16_t version = 1;
	char ssid[32] = "GPS_Tracker";
	char pass[64] = "testtest";
	uint16_t scan_interval = 10;	// Секунд
	uint16_t min_hdop = 20;			// Метров
	int16_t tz_offset = 300;		// минуты, 300=Екатеринбург, 180=Москва
	bool auto_tz = false;			// Автообнаружение, на основе текущих координат по меридианам (долго и неточно)
	bool rotate_logs = true;
} data;
#pragma pack(pop)


class ConfigManager : public ConfigData {
public:
	ConfigManager() {
		LittleFS.begin();
		load();
	}

	bool save() {
		File f = LittleFS.open(CFG_FILE, "w");
		if (!f) return false;
		// Записываем "себя" как структуру ConfigData
		f.write((const uint8_t*)static_cast<ConfigData*>(this), sizeof(ConfigData));
		f.close();
		return true;
	}

	bool load() {
		if (!LittleFS.exists(CFG_FILE)) return false;

		File f = LittleFS.open(CFG_FILE, "r");
		if (!f || f.size() != sizeof(ConfigData)) {
			if (f) f.close();
			return false;
		}
		ConfigData temp;
		f.read((uint8_t*)&temp, sizeof(ConfigData));
		f.close();
		if (temp.version != version) {
			return false;
		}

		*static_cast<ConfigData*>(this) = temp;
		return true;
	}
};

ConfigManager cfg;
