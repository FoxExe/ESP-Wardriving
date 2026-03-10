#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <GyverOLED.h>
#include <NMEAGPS.h>
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
WebServer web(80);

unsigned long lastScanMillis = 0;
unsigned long lastBatteryUpdate = 0;
bool readyToSave = false;

struct ExtendedReport {
	uint32_t blocks_total = 0;
	uint32_t blocks_used = 0;
	uint32_t heap_free = 0;
	uint32_t heap_max = 0;
	uint8_t ap_clients = 0;
} reportExt;

struct ActiveSatellites {
	uint8_t ids[12] = { 0x00 };
	uint8_t count = 0;
} sats_active;

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

void draw_loading_screen(const char* msg) {
	screen.clear();
	screen.setScale(2);
	screen.setCursorXY(0, 0);
	screen.print("LOADING");

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
	screen.setCursorXY(103, 0);
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

	if (gps.satellites_valid()) {
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
				uint8_t h = map(sat.snr, 0, 99, 0, 21);
				screen.rect(i * 3, 31 - h, i * 3 + 1, 31, OLED_FILL);

				// sat.tracked - is a "sat is visible" not "sat used for positioning", so we need this hack:
				for (uint8_t j = 0; j < sats_active.count; j++) {
					if (sats_active.ids[j] == sat.id) {
						screen.line(i * 3, 31 - h - 2, i * 3 + 1, 31 - h - 2, OLED_FILL);
						break;
					}
				}

				bar_num++;
				if (bar_num > MAX_SAT_BARS) break; // Max 16 bars
			}
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
	if (WiFi.softAPgetStationNum() > 0) {
		screen.rect(78, 8, 127, 31, OLED_CLEAR);
		screen.setCursorXY(85, 15);
		screen.print("PAUSED");
	}
	else if (scanned >= 0) {
		screen.rect(78, 8, 127, 31, OLED_CLEAR);
		for (uint8_t i = 0; i < scanned; i++) {
			uint8_t h = map(constrain(WiFi.RSSI(i), -100, -50), -100, -50, 0, 21);
			screen.line(78 + (i * 2), 31 - h, 78 + (i * 2), 31, OLED_FILL);
			//screen.rect(78 + (i * 2), 31 - h, 78 + (i * 3), 31, OLED_FILL);
			//if (WiFi.isHidden(i)) {
			if (WiFi.encryptionType(i) == ENC_TYPE_NONE) {
				screen.dot(78 + (i * 2), 31 - h - 2, OLED_FILL);
			}
			if (i > MAX_APS_BARS) break; // max 25 bars
		}
	}
	else if (scanned == -1) {
		screen.circle(122, 10, 2, OLED_FILL);
		/*
		screen.setScale(2);
		screen.setCursorXY(79, 13);
		screen.print("SCAN"); // max 4 chars!
		screen.setScale(1);
		*/
	}

	// ========= OTHER   =========
	// clear area
	screen.rect(54, 8, 71, 31, OLED_CLEAR);
	// SPI Flash usage percentage
	screen.setCursorXY(54, 24);
	screen.printf("%2d%%", (logger.getUsagePercentage() == 100) ? 99 : logger.getUsagePercentage());

	screen.update();
}


void setup() {
	// Debug
	Serial.begin(115200);
	Serial.println();
	Serial.println("Loading...");
	Serial.println();

	// Screen init
	screen.init();
	screen.clear();
	screen.setScale(1);

	// Flash init
	draw_loading_screen("Flash...");
	logger.setRotation(cfg.rotate_logs);
	if (!logger.begin()) {
		//screen.clear();
		screen.setScale(1);
		screen.setCursorXY(0, 16);
		screen.println("Flash error!");
		screen.printf("Blocks: %3d/%3d/%3d\n", logger.blocksUsed(), logger.blocksFree(), logger.blocksTotal());
		screen.update();

		while (true) { yield(); delay(100); }
	}

	// GPS
	draw_loading_screen("GPS...");
	gpsPort.begin(9600);

	// Configure Wi-Fi
	draw_loading_screen("Wi-Fi...");
	WiFi.setAutoConnect(false); // fix "-1" scan status after boot
	WiFi.mode(WIFI_AP_STA);
	WiFi.softAP(cfg.ssid, cfg.pass);

	// Web server / web interface
	draw_loading_screen("Web...");
	web.begin();

	draw_loading_screen("Done!");
	delay(1000);

	screen.clear();
}

void loop() {
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
		gps_fix fix = gps.read();

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
			current.alt = fix.altitude();
		}

		if (fix.valid.hdop) {
			current.accuracy = (uint16_t)((fix.hdop * 0.0045f) + 0.5f);
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

		if (millis() - lastScanMillis >= cfg.scan_interval * 1000UL && WiFi.softAPgetStationNum() == 0) {
			lastScanMillis = millis();
			WiFi.scanDelete();
			WiFi.scanNetworks(true, true); // Асинхронно
			readyToSave = (fix.valid.location && current.accuracy <= cfg.min_hdop);
		}

		// TODO: send to browser [current]
		// TODO: Send to browser when logger started new block (for auto update downloads table)

		updateUI();

		// DEBUG
		time_t now = time(nullptr) + (cfg.tz_offset * 60);
		struct tm* ptm = gmtime(&now);
		Serial.printf("[%02d:%02d:%02d]", ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
		Serial.printf(" | HEAP: %5d", ESP.getFreeHeap());
		Serial.printf(" | BAT: %3d%% (%6.4fv) ", battery.getPercentage(), battery.getVoltage());
		Serial.printf(" | BLOCKS: %3d/%3d POINTS: %7d", logger.blocksUsed(), logger.blocksTotal(), logger.pointsSaved());
		Serial.printf(" | LAT: %9.5f LON: %10.5f ALT: %7.4f HDOP: %7.4f SATS: %2d/%2d", fix.latitude(), fix.longitude(), fix.altitude(), (float)(fix.hdop / 1000), fix.satellites, gps.sat_count);
		Serial.printf(" | WIFI: %2d", WiFi.scanComplete());
		Serial.println();

		//Serial.printf("BLOCKS: %d/%d HEAP: %d/%d CLIENTS: %d\n", reportExt.blocks_used, reportExt.blocks_total, reportExt.heap_free, reportExt.heap_max, reportExt.ap_clients);
		//Serial.printf("Battery: %4.4fv (%03d%%)\n", battery.getVoltage(), battery.getPercentage());
		/*
		if (gps.sat_count > 0) {
			Serial.println("");
			Serial.print(" ID | SR | EL | AZ  | T\n");
			for (uint8_t i = 0; i < gps.sat_count; i++) {
				auto& sat = gps.satellites[i];

				bool isActive = false;
				for (uint8_t j = 0; j < sats_active.count; j++) {
					if (sats_active.ids[j] == sat.id) {
						isActive = true;
						break;
					}
				}

				Serial.printf("#%2d | %2d | %2d | %3d | %d\n", sat.id, sat.snr, sat.elevation, sat.azimuth, isActive);
			}
		}
		*/
	}

	if (WiFi.scanComplete() >= 0) {
		current.ap_count = WiFi.scanComplete();
		// TODO: send to browser [wifi info]

		if (readyToSave) {
			logger.storeRecord(current);
			readyToSave = false;
		}
	}

	if (millis() - lastBatteryUpdate >= 1000) {
		lastBatteryUpdate = millis();
		battery.update();
	}

	if (battery.is_low()) {
		Serial.println("Battery low! Shutting down...");
		screen.clear();
		screen.setScale(2);
		screen.setCursorXY(0, 8);
		screen.print("LOW BAT");
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

	web.update();
}
