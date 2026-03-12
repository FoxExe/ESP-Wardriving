#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <GyverOLED.h>
#include <NMEAGPS.h>
#include <EncButton.h>
#include <time.h>

#include "Logger.h"
#include "Battery.h"
#include "Settings.h"
#include "WebServer.h"


#define MAX_SAT_BARS 16
#define MAX_APS_BARS 25  // (128 - 78) / 2 = graph width / (bar_width + padding_right)

GyverOLED<SSD1306_128x32, OLED_BUFFER> screen;
SoftwareSerial gpsPort(15, 2);
NMEAGPS gps;
gps_fix fix;
WebServer web(80);
EncButton btn(0);

const uint8_t GPS_OFF[] = {
	0xB5, 0x62, // Заголовок (Sync)
	0x02, 0x41, // ID сообщения (RXM-PMREQ)
	0x08, 0x00, // Длина данных (8 байт)
	0x00, 0x00, 0x00, 0x00, // Резерв
	0x02, 0x00, 0x00, 0x00, // Режим: 0x02 = Backup (сон)
	0x4D, 0x3B  // Контрольная сумма (CK_A, CK_B)
};

const uint8_t GPS_ON[] = {
	0xB5, 0x62,
	0x02, 0x41,
	0x08, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00,
	0x4C, 0x37
};

unsigned long lastScanMillis = 0;
unsigned long lastBatteryUpdate = 0;

#pragma pack(push, 1)
struct ExtendedReport {
	uint32_t blocks_total = 0;
	uint32_t blocks_used = 0;
	uint32_t heap_free = 0;
	uint32_t heap_max = 0;
	uint8_t ap_clients = 0;
	uint32_t saved_points = 0;
	uint16_t current_block = 0;
} reportExt;

struct ActiveSatellites {
	uint8_t ids[12] = { 0x00 };
	uint8_t count = 0;
} sats_active;
#pragma pack(pop)

uint16_t hdopToAccuracy(uint16_t hdop) {
	if (hdop == 0) { return 0xFFFF; }
	return (uint16_t)((hdop * 0.0045f) + 0.5f);
}

void sync_clock(const NeoGPS::time_t& dt) {
	struct tm t;
	t.tm_hour = dt.hours;
	t.tm_min = dt.minutes;
	t.tm_sec = dt.seconds;
	t.tm_year = dt.year + 2000 - 1900; // Год с 1900
	t.tm_mon = dt.month - 1;          // Месяцы 0-11
	t.tm_mday = dt.date;

	time_t now = mktime(&t);
	struct timeval tv = { .tv_sec = now, .tv_usec = 0 };
	settimeofday(&tv, NULL); // Устанавливаем системное время ESP8266
}

void draw_loading_screen(const __FlashStringHelper* msg) {
	screen.clear();
	screen.setScale(2);
	screen.setCursorXY(0, 0);
	screen.print(F("LOADING"));

	screen.rect(0, 24, 127, 31, OLED_CLEAR);
	screen.setCursorXY(0, 24);
	screen.setScale(1);
	screen.print(msg);

	screen.update();

	Serial.println(msg); // Debug
}


void updateUI() {
	//screen.clear();

	// ========= HEADER  =========
	screen.rect(0, 0, 127, 7, OLED_CLEAR); // clear line
	screen.setScale(1);
	screen.setCursorXY(0, 0);

	time_t now = time(nullptr) + (cfg.tz_offset * 60);
	struct tm* ptm = gmtime(&now);

	screen.printf("%02d:%02d:%02d", ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
	/*
	if (fix != nullptr && fix->valid.time) {
		screen.printf("%02d:%02d:%02d", fix->dateTime.hours, fix->dateTime.hours, fix->dateTime.hours);
	}
	else {
		screen.print("--:--:--");
	}
	*/

	// GPS points counter
	screen.setCursorXY(52, 0);
	screen.printf("%7d", logger.pointsSaved());

	// Draw battery icon
	screen.rect(99, 1, 102, 6, OLED_STROKE);
	screen.line(100, 0, 101, 0, OLED_FILL);

	// Battery charge text
	screen.setCursorXY(104, 0);
	screen.printf("%3d%%", battery.getPercentage());

	// ========= GRAPH 1 =========
	// Clear graph
	screen.rect(48, 8, 53, 31, OLED_CLEAR);
	// Draw scale at right
	screen.line(51, 8, 51, 31, OLED_FILL);
	screen.line(48, 12, 51, 12, OLED_FILL);
	screen.line(48, 22, 51, 22, OLED_FILL);
	for (uint8_t i = 8; i < 32; i++) {
		if (i % 2 != 0) continue;
		screen.dot(50, i, OLED_FILL);
	}

	// Clear graph
	screen.rect(0, 8, 47, 31, OLED_CLEAR);
	// Note: We can display max 16. In lib max is 20. But in reality - max is ~12.
	// Just cut off weaked satellites!
	uint8_t thresholdSNR = 0;
	if (gps.sat_count > MAX_SAT_BARS) {
		uint8_t allSNRs[NMEAGPS_MAX_SATELLITES];
		for (uint8_t i = 0; i < gps.sat_count; i++) allSNRs[i] = gps.satellites[i].snr;

		// Sorting...
		for (uint8_t i = 0; i < gps.sat_count - 1; i++) {
			for (uint8_t j = 0; j < gps.sat_count - i - 1; j++) {
				if (allSNRs[j] < allSNRs[j + 1]) {
					uint8_t temp = allSNRs[j];
					allSNRs[j] = allSNRs[j + 1];
					allSNRs[j + 1] = temp;
				}
			}
		}
		thresholdSNR = allSNRs[MAX_SAT_BARS - 1]; // Порог — это SNR 16-го по силе спутника
	}

	// Draw bars and mark used/tracked sattelites
	uint8_t bar_num = 0;
	for (uint8_t i = 0; i < gps.sat_count; i++) {
		auto& sat = gps.satellites[i];
		if (sat.snr >= thresholdSNR) {
			uint8_t h = map(sat.snr, 0, 99, 0, 23);
			screen.rect(i * 3, 31 - h, i * 3 + 1, 31, OLED_FILL);

			// sat.tracked - is a "sat is visible" not "sat used for positioning", so we need this hack:
			for (uint8_t j = 0; j < sats_active.count; j++) {
				if (sats_active.ids[j] == sat.id) {
					screen.line(i * 3, 31 - h + 1, i * 3 + 1, 31 - h + 1, OLED_CLEAR);
					break;
				}
			}

			bar_num++;
			if (bar_num > MAX_SAT_BARS) break; // Max 16 bars
		}
	}

	// ========= GRAPH 2 =========
	// Clear graph
	screen.rect(72, 8, 77, 31, OLED_CLEAR);
	// Draw scale at right
	screen.line(73, 8, 73, 31, OLED_FILL);
	screen.line(73, 12, 76, 12, OLED_FILL);
	screen.line(73, 22, 76, 22, OLED_FILL);
	for (uint8_t i = 8; i < 32; i++) {
		if (i % 2 != 0) continue;
		screen.dot(74, i, OLED_FILL);
	}

	// Draw bars
	int8_t scanned = WiFi.scanComplete();
	if (scanned >= 0) {
		screen.rect(78, 8, 127, 31, OLED_CLEAR);
		for (uint8_t i = 0; i < scanned; i++) {
			uint8_t h = map(constrain(WiFi.RSSI(i), -100, -50), -100, -50, 0, 23);
			screen.line(78 + (i * 2), 31 - h, 78 + (i * 2), 31, OLED_FILL);
			//screen.rect(78 + (i * 2), 31 - h, 78 + (i * 3), 31, OLED_FILL);
			//if (WiFi.isHidden(i)) {
			if (WiFi.encryptionType(i) == ENC_TYPE_NONE) {
				screen.dot(78 + (i * 2), 31 - h + 1, OLED_CLEAR);
			}
			if (i > MAX_APS_BARS) break; // max 25 bars
		}
	}
	if (scanned == WIFI_SCAN_RUNNING) {
		screen.rect(119, 9, 127, 15, OLED_CLEAR);
		// Reload icon
		screen.circle(123, 12, 3, OLED_STROKE);
		screen.line(120, 9, 126, 15, OLED_CLEAR);
		screen.line(119, 12, 121, 12, OLED_FILL);
		screen.line(125, 12, 127, 12, OLED_FILL);

		// Play icon (triangle)
		//screen.line(124, 9, 126, 11, OLED_FILL);
		//screen.line(126, 11, 124, 13, OLED_FILL);
		//screen.line(124, 13, 124, 9, OLED_FILL);
		//screen.dot(125, 11, OLED_FILL);

		/*
		screen.setScale(2);
		screen.setCursorXY(79, 13);
		screen.print("SCAN"); // max 4 chars!
		screen.setScale(1);
		*/
	}
	else if (scanned == WIFI_SCAN_FAILED) {
		screen.rect(119, 9, 127, 15, OLED_CLEAR);
		// Pause icon
		screen.rect(121, 9, 122, 15, OLED_FILL);
		screen.rect(124, 9, 125, 15, OLED_FILL);
		//screen.rect(78, 8, 127, 31, OLED_CLEAR);
		//screen.setCursorXY(90, 16);
		//screen.print(F("PAUSE"));
	}

	// ========= OTHER   =========
	// clear area
	screen.rect(54, 8, 71, 31, OLED_CLEAR);

	// Fix OK, points capture in progress
	if (fix.valid.location && current.accuracy <= cfg.min_acc && WiFi.scanComplete() >= 0) {
		// Play icon
		screen.line(61, 9, 61, 13, OLED_FILL);
		screen.line(62, 10, 62, 12, OLED_FILL);
		screen.dot(63, 11, OLED_FILL);
	}
	else {
		screen.rect(60, 9, 61, 13, OLED_FILL);
		screen.rect(63, 9, 64, 13, OLED_FILL);
	}

	// Accuracy
	screen.setCursorXY(54, 16);
	uint16_t acc = hdopToAccuracy(fix.hdop);
	screen.printf("%3d", (acc > 999) ? 999 : acc);

	// SPI Flash usage percentage
	screen.setCursorXY(54, 24);
	screen.printf("%2d%%", (logger.getUsagePercentage() >= 100) ? 99 : logger.getUsagePercentage());

	screen.update();
}

void sendStats() {
	constexpr size_t offset1 = 1;
	constexpr size_t offset2 = offset1 + sizeof(current);
	constexpr size_t offset3 = offset2 + sizeof(reportExt);
	uint16_t size = offset3 + sizeof(NMEAGPS::satellite_view_t) * gps.sat_count;

	uint8_t* buff = (uint8_t*)malloc(size);
	if (buff == nullptr) return;

	buff[0] = 0x01;
	memcpy(&buff[offset1], &current, sizeof(current));
	memcpy(&buff[offset2], &reportExt, sizeof(reportExt));

	// Send satellites detailed info
	for (uint16_t i = 0; i < gps.sat_count; i++) {
		auto sat = gps.satellites[i];
		sat.tracked = false;
		for (uint8_t j = 0; j < sats_active.count; j++) {
			if (sats_active.ids[j] == sat.id) {
				sat.tracked = true;
				break;
			}
		}
		memcpy(&buff[offset3 + (sizeof(NMEAGPS::satellite_view_t) * i)], &sat, sizeof(sat));
	}

	web.sendWSData(buff, size);
	free(buff);
}

#pragma pack(push, 1)
struct APDataPacket {
	uint8_t mac[6];
	uint8_t chan;
	uint8_t enc;
	uint8_t rssi;
	uint8_t ssid_len;
	//uint8_t ssid[32]; // dynamic string
};
#pragma pack(pop)

void sendWiFiScanResult() {
	int8_t results = WiFi.scanComplete();
	if (results < 0) { return; }

	// TODO: Send only if changed. Or change it to HTTP_GET request.

	std::vector<uint8_t> buffer;
	buffer.reserve(2 + (results * (sizeof(APDataPacket) + 32)));

	buffer.push_back(0x02);
	buffer.push_back((uint8_t)results);

	for (int8_t i = 0; i < results; i++) {
		APDataPacket p;
		// MAC Address
		memcpy(p.mac, WiFi.BSSID(i), 6);
		p.chan = WiFi.channel(i);
		p.enc = WiFi.encryptionType(i);
		p.rssi = (uint8_t)WiFi.RSSI(i);

		String ssid = WiFi.SSID(i);
		p.ssid_len = (ssid.length() > 32) ? 32 : ssid.length();

		uint8_t* pReg = (uint8_t*)&p;
		for (size_t j = 0; j < sizeof(APDataPacket); j++) buffer.push_back(pReg[j]);
		for (size_t j = 0; j < p.ssid_len; j++) buffer.push_back(ssid[j]);
	}

	web.sendWSData(buffer.data(), buffer.size());
}

void setup() {
	// Fix internal timer
	struct timeval tv = { .tv_sec = 946684800, .tv_usec = 0 };
	settimeofday(&tv, nullptr);

	// Debug
	Serial.begin(115200);
	Serial.println();
	Serial.println(F("Loading..."));
	Serial.println();

	// Screen init
	screen.init();
	screen.clear();
	screen.setScale(1);

	// Button (GPIO-0)
	btn.setHoldTimeout(1000); // 1 sec

	// Flash init
	draw_loading_screen(F("Flash..."));
	logger.setRotation(cfg.rotate_logs);
	if (!logger.begin()) {
		//screen.clear();
		screen.setScale(1);
		screen.setCursorXY(0, 16);
		screen.println(F("Flash error!"));
		screen.printf("Blocks: %3d/%3d/%3d\n", logger.blocksUsed(), logger.blocksFree(), logger.blocksTotal());
		screen.update();

		while (true) { yield(); delay(100); }
	}

	// GPS
	draw_loading_screen(F("GPS..."));
	gpsPort.begin(9600);

	// Configure Wi-Fi
	draw_loading_screen(F("Wi-Fi..."));
	WiFi.setAutoConnect(false); // fix "-1" scan status after boot
	WiFi.mode(WIFI_AP_STA);
	WiFi.softAP(cfg.ssid, cfg.pass);

	// Web server / web interface
	draw_loading_screen(F("Web..."));
	web.begin();

	// Battery
	battery.update();

	draw_loading_screen(F("Done!"));
	screen.clear();
	screen.update();
}

void loop() {
	btn.tick();

	if (btn.click()) {
		Serial.println(F("Клик!"));

		gpsPort.write(GPS_ON, sizeof(GPS_ON));
	}

	if (btn.hasClicks(2)) {
		Serial.println(F("Двойной клик!"));
	}

	if (btn.hold()) {
		Serial.println(F("Удержание 1 сек!"));

		gpsPort.write(GPS_OFF, sizeof(GPS_OFF));
	}

	//if (gpsPort.available()) {
	//	Serial.write(gpsPort.read());
	//}
	while (gpsPort.available()) {
		char c = gpsPort.read();
		static char buffer[100];
		static int pos = 0;
		if (c == '$') pos = 0;
		if (pos < 99) buffer[pos++] = c;
		if (c == '\n') {
			buffer[pos] = 0;
			// Если поймали GSA — вытаскиваем ID активных спутников
			if (strstr(buffer, "GSA")) {
				sats_active.count = 0;

				char* p = buffer;
				for (int i = 0; i < 3; i++) {
					p = strchr(p, ',');
					if (p) p++;
				}

				// Читаем 12 полей ID
				for (int i = 0; i < 12; i++) {
					if (p && *p != ',') { // Если между запятыми есть число
						int id = atoi(p);
						if (id > 0) sats_active.ids[sats_active.count++] = (uint8_t)id;
					}
					if (p) p = strchr(p, ','); // Переходим к следующей запятой
					if (p) p++;
				}
			}
			// Если поймали GLL — блок данных от Neo-6M закончился
			//if (strstr(buffer, "GLL")) break;
		}

		// Стандартная обработка данных NeoGPS
		gps.handle(c);
	}

	//if (gps.available(gpsPort)) {
	if (gps.available()) {
		fix = gps.read();

		// Update GPS info
		if (fix.valid.time) {
			current.timestamp = (uint32_t)fix.dateTime;
			sync_clock(fix.dateTime);
		}

		if (fix.valid.location) {
			current.lat = fix.latitude();
			current.lon = fix.longitude();
		}

		if (fix.valid.altitude) {
			current.alt = (int16_t)fix.altitude();
		}

		if (fix.valid.hdop) {
			current.accuracy = hdopToAccuracy(fix.hdop);
		}
		else {
			current.accuracy = 999;
		}

		current.bat_charge = battery.getPercentage();

		// TODO: Used only in browser, store as ExtendedInfo structure
		reportExt.blocks_total = logger.blocksTotal();
		reportExt.blocks_used = logger.blocksUsed();
		reportExt.heap_free = ESP.getFreeHeap();
		reportExt.heap_max = ESP.getMaxFreeBlockSize();
		reportExt.ap_clients = WiFi.softAPgetStationNum();
		reportExt.saved_points = logger.pointsSaved();
		reportExt.current_block = logger.getCurrentBlockID();

		// && WiFi.softAPgetStationNum() == 0
		if (millis() - lastScanMillis >= cfg.scan_interval * 1000UL) {
			lastScanMillis = millis();
			if (fix.valid.location && current.accuracy <= cfg.min_acc && WiFi.scanComplete() >= 0) {
				logger.storeRecord(current);
			}
			WiFi.scanDelete();
			WiFi.scanNetworks(true, true); // Асинхронно
		}

		updateUI();

		// send to browser [current] + [reportExt]
		sendStats();
		sendWiFiScanResult(); // Send to browser. Can be requested by user.

		// DEBUG
		// Warning: Too heavy! Can crash (stack overflow)
		time_t now = time(nullptr) + (cfg.tz_offset * 60);
		struct tm* ptm = gmtime(&now);
		Serial.printf("[%02d.%02d.%04d %02d:%02d:%02d]", ptm->tm_mday, ptm->tm_mon + 1, ptm->tm_year + 1900, ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
		Serial.printf(" | HEAP: %5d", ESP.getFreeHeap());
		Serial.printf(" | BAT: %3d%% (%7.4fv) ", battery.getPercentage(), battery.getVoltage());
		Serial.printf(" | BLOCKS: %3d/%3d POINTS: %7d", logger.blocksUsed(), logger.blocksTotal(), logger.pointsSaved());
		Serial.printf(" | LAT: %9.5f LON: %10.5f ALT: %7.4f HDOP: %7.4f ACC: %3d SATS: %2d/%2d", fix.latitude(), fix.longitude(), fix.altitude(), (float)(fix.hdop / 1000), hdopToAccuracy(fix.hdop), fix.satellites, gps.sat_count);
		Serial.printf(" | WIFI: %2d", WiFi.scanComplete());
		Serial.println();
	}

	if (WiFi.scanComplete() >= 0) {
		current.ap_count = WiFi.scanComplete();
	}

	web.update();

	if (logger.needErase()) {
		Serial.println();
		Serial.println(F("!!! ERASING FLASH !!!"));
		Serial.println();

		screen.clear();
		screen.setScale(2);
		screen.setCursorXY(0, 0);
		screen.print(F("ERASING..."));
		screen.setScale(1);
		screen.update();

		logger.eraseFlash([](int percent) {
			screen.rect(0, 16, 127, 31, OLED_CLEAR);
			screen.setCursorXY(6 * 9, 16);
			screen.printf("%3d%%", percent);
			screen.rect(0, 24, 127, 31, OLED_STROKE);
			uint8_t w = map(percent, 0, 100, 2, 125);
			screen.rect(2, 26, w, 29, OLED_FILL);
			screen.update();

			Serial.print(F("Progress: ")); Serial.print(percent); Serial.println("%");
			});

		screen.clear();
		screen.update();
	}

	if (millis() - lastBatteryUpdate >= 1000) {
		lastBatteryUpdate = millis();
		battery.update();
	}

	if (battery.is_low()) {
		Serial.println(F("Battery low! Shutting down..."));
		screen.clear();
		screen.setScale(2);
		screen.setCursorXY(0, 8);
		screen.print(F("LOW BAT"));
		screen.update();
		for (int i = 0; i < 30; i++) {
			delay(100);
			yield();
		}

		web.end();

		WiFi.disconnect(true);
		delay(100);
		WiFi.mode(WIFI_OFF);
		WiFi.forceSleepBegin();
		delay(1);


		ESP.deepSleep(0); // Turn off device
		//while (true) { yield(); } // Stop loop()
	}
}
