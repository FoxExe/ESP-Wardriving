#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <EncButton.h>
#include <time.h>
#include <LwipDhcpServer.h>
#include <SoftwareSerial.h>

#include "Logger.h"
#include "Battery.h"
#include "Settings.h"
#include "WebServer.h"
#include "GUI.h"
#include "GPS.h"


WebServer web(80);
EncButton btn(0);
GUI gui;
SoftwareSerial gpsPort(15, 2);
UbloxGPS gps(gpsPort);
//UbloxGPS gps(Serial);
Battery battery;

GPSPoint gpsPointA;
GPSPoint gpsPointB;

unsigned long lastScanMillis = 0;
unsigned long lastScreenUpdate = 0;

#pragma pack(push, 1)
struct CurrentReport {
	uint32_t timestamp = 0;
	float lat = 0;
	float lon = 0;
	int16_t alt = 0;
	uint16_t acc = 0;
	uint8_t bat_charge = 0;
	uint32_t blocks_total = 0;
	uint32_t blocks_used = 0;
	uint32_t heap_free = 0;
	uint32_t heap_max = 0;
	uint8_t ap_clients = 0;
	uint32_t saved_points = 0;
	uint16_t current_block = 0;
} report;
#pragma pack(pop)

void handleGpsUpdate() {
	// Fill report
	report.timestamp = gps.get_timestamp();
	report.lat = gps.lat;
	report.lon = gps.lon;
	report.alt = gps.altitude();
	report.acc = gps.acc;
	report.bat_charge = battery.getPercentage();
	report.blocks_total = logger.blocksTotal();
	report.blocks_used = logger.blocksUsed();
	report.heap_free = ESP.getFreeHeap();
	report.heap_max = ESP.getMaxFreeBlockSize();
	report.ap_clients = WiFi.softAPgetStationNum();
	report.saved_points = logger.pointsSaved();
	report.current_block = logger.getCurrentBlockID();

	// Send report
	uint16_t size = 1 + sizeof(report) + (sizeof(GPSSatellite) * gps.satellites_count);
	uint8_t* buff = (uint8_t*)malloc(size);
	if (buff == nullptr) return;

	buff[0] = 0x01; // This is report packet

	memcpy(&buff[1], &report, sizeof(report));

	// Set satellites detailed info
	if (gps.satellites_count > 0) {
		memcpy(&buff[1 + sizeof(report)], gps.satellites, sizeof(GPSSatellite) * gps.satellites_count);
	}

	web.sendWSData(buff, size); // Send report throught websocket
	free(buff);

	// Updatre GPS graph
	int snr_values[gps.satellites_count];
	bool tracked[gps.satellites_count];
	for (int i = 0; i < gps.satellites_count; i++) {
		snr_values[i] = gps.satellites[i].snr;
		tracked[i] = gps.satellites[i].is_used;
	}
	gui.draw_graph_gps(snr_values, tracked, gps.satellites_count);

	gui.draw_isrunning_icon(gps.fix && gps.acc <= cfg.min_acc);
	gui.draw_accuracy(gps.acc > 999 ? 999 : gps.acc);

#ifdef SERIAL_DEBUG
	struct tm ptm = gps.get_time();
	Serial.printf("[%02d.%02d.%04d %02d:%02d:%02d]", ptm.tm_mday, ptm.tm_mon + 1, ptm.tm_year + 1900, ptm.tm_hour, ptm.tm_min, ptm.tm_sec);
	Serial.printf(" | LAT: %10.6f  LON: %11.6f  ALT: %7.4f  HDOP: %7.4f  ACC: %6.2f  SATS: %2d/%2d", gps.lat, gps.lon, gps.altitude(), gps.hdop, gps.acc, gps.sat_used(), gps.sat_total());
	Serial.println("\n\tID   RSSI U SIGNAL");
	for (int i = 0; i < gps.satellites_count; i++) {
		Serial.printf("\t%2d %3ddbm %d ", gps.satellites[i].id, gps.satellites[i].snr, gps.satellites[i].is_used);
		int bars = map(gps.satellites[i].snr, 0, 100, 0, 32);
		for (int j = 0; j < bars; ++j) { Serial.print("#"); }
		Serial.println("");
	}
#endif
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
/*
void updateWiFiGraph() {
	int8_t scanned = WiFi.scanComplete();
	if (scanned <= 0) return;

	int rssi_values[scanned];
	bool is_open[scanned];

	for (int i = 0; i < scanned; i++) {
		rssi_values[i] = WiFi.RSSI(i);
		is_open[i] = (WiFi.encryptionType(i) == ENC_TYPE_NONE);
	}

	gui.draw_graph_wifi(rssi_values, is_open, scanned);
}
*/
void updateWiFiGraph() {
	int8_t scanned = WiFi.scanComplete();
	if (scanned <= 0) return;

	int values[MAX_WIFI_CHANNELS];
	for (int i = 0; i < MAX_WIFI_CHANNELS; i++) values[i] = -100;

	for (int i = 0; i < scanned; i++) {
		int ch = WiFi.channel(i) - 1;
		int rssi = WiFi.RSSI(i);
		if (rssi > values[ch]) values[ch] = rssi;
	}

	gui.draw_graph_wifi(values);
}

void handleButtons() {
	btn.tick();

	if (btn.click()) {
		gps.start();
#ifdef SERIAL_DEBUG
		Serial.println(F("GPS IS ON"));
#endif
	}

	if (btn.hold()) {
		gps.stop();
#ifdef SERIAL_DEBUG
		Serial.println(F("GPS IS OFF"));
#endif
	}
}

void onDhcpOptions(const DhcpServer& server, DhcpServer::OptionsBuffer& options) {
	// Append route to 192.168.4.0/24
	uint8_t route_data[] = { 0x18, 0xC0, 0xA8, 0x04, 0xC0, 0xA8, 0x04, 0x01 };
	options.add(121, route_data, sizeof(route_data));
}

// ================================================================
void setup() {
	// Fix internal timer
	//struct timeval tv = { .tv_sec = 946684800, .tv_usec = 0 };
	//settimeofday(&tv, nullptr);

#ifdef SERIAL_DEBUG
	// Debug
	//Serial1.begin(115200); // Debug
	Serial.begin(115200);
	Serial.println();
	Serial.println(F("Loading..."));
	Serial.println();
#endif

	// Button (GPIO-0)
	btn.setHoldTimeout(1000); // 1 sec

	// Flash init
	gui.clear_screen();
	gui.draw_loading("Flash");
	logger.setRotation(cfg.rotate_logs);
	if (!logger.begin()) {
		gui.draw_loading("Flash error!");
		while (true) { yield(); delay(100); }
	}

	// GPS
	gui.draw_loading("GPS");
	gpsPort.begin(9600, SWSERIAL_8N1, 15, 2, false, 192); // Increase buffer
	//Serial.begin(9600); // GPS
	//Serial.setRxBufferSize(512);

	// Switch module to faster speed
	//gps.setUpdateRate(500); // Update rate 1Hz -> 2Hz, up to 10Hz
	/*! WARNING: SoftwareSerial can't handle more than 9600! Even in overclock mode (160MHz CPU)
	gps.reconfigure(115200);
	gpsPort.flush();
	gpsPort.end();
	delay(100);
	gpsPort.begin(115200);
	*/
	gps.on_update = handleGpsUpdate;

	// Configure Wi-Fi
	gui.draw_loading("Wi-Fi");
	WiFi.setAutoConnect(false); // fix "-1" scan status after boot
	WiFi.mode(WIFI_AP_STA);
	WiFi.softAP(cfg.ssid, cfg.pass);
	WiFi.softAPDhcpServer().onSendOptions(onDhcpOptions);  // Prioritize network

	// Web server / web interface
	gui.draw_loading("Web");
	web.begin();

	// Battery
	battery.update();

	gui.draw_loading("Done!");
	delay(500);

	// Initial screen elements
	gui.clear_screen();
	gui.draw_time();
	gui.draw_points(logger.pointsSaved());
	gui.draw_battery(battery.getPercentage());
	gui.draw_ruler(48, 8, 23, true);
	gui.draw_ruler(73, 8, 23, false);
	gui.draw_isrunning_icon(false);
	gui.draw_accuracy(999);
	gui.draw_flash_used(map(logger.blocksUsed(), 0, logger.blocksTotal(), 0, 100));
	gui.redraw();
}

// ================================================================
void loop() {
	/* DEBUG
	if (gpsPort.available()) {
		Serial.write(gpsPort.read());
	}
	return;
	*/

	gps.update();
	web.update();
	battery.update();
	handleButtons();

	// Update display every one second
	if (millis() - lastScreenUpdate >= 1000) {
		lastScreenUpdate = millis();

		// Update time
		time_t now = time(nullptr) + (cfg.tz_offset * 60);
		struct tm ptm = *gmtime(&now);
		gui.draw_time(&ptm);

		// Update battery
		gui.draw_battery(battery.getPercentage());

#ifdef SERIAL_DEBUG
		unsigned long totalSeconds = millis() / 1000;
		int seconds = totalSeconds % 60;
		int minutes = (totalSeconds / 60) % 60;
		int hours = (totalSeconds / 3600);
		Serial.printf("[ UPTIME: %4d:%02d:%02d ]", hours, minutes, seconds);
		Serial.printf(" | HEAP: %5d", ESP.getFreeHeap());
		Serial.printf(" | BAT: %3d%% (%7.4fv) ", battery.getPercentage(), battery.getVoltage());
		Serial.printf(" | USED: %3d/%3d CURRENT: %3d SAVED: %7d", logger.blocksUsed(), logger.blocksTotal(), logger.getCurrentBlockID(), logger.pointsSaved());
		Serial.printf(" | WIFI: %2d", WiFi.scanComplete());
		Serial.println();
#endif
	}

	if (millis() - lastScanMillis >= cfg.scan_interval * 1000UL) {
		if (gps.fix && gps.acc <= cfg.min_acc) {
			if (WiFi.scanComplete() < 0) {
				WiFi.scanNetworks(true, true); // Run async, scan hidden
				gui.draw_wifi_status(WIFI_SCAN_RUNNING);

				gpsPointA = { gps.lat, gps.lon, gps.altitude(), gps.acc };
				gpsPointB = { 0 }; // null all fields

				lastScanMillis = millis();
			}
			else {
				// Wait until scan completed? (Allow run other code in loop()!)
			}
		}
	}

	if (WiFi.scanComplete() >= 0) {
		updateWiFiGraph();
		//gui.draw_wifi_status(WiFi.scanComplete());

		sendWiFiScanResult(); // to browser/websocket

		// Check if GPS still accurate and current scan initiated by cfg.scan_interval timer, not by user (web)
		if (gps.fix && gps.acc <= cfg.min_acc && gpsPointB.lat == 0.0 && gpsPointB.lon == 0.0) {
			// Calculate gps midpoint for accuracy
			gpsPointB = { gps.lat, gps.lon, gps.altitude(), gps.acc };
			gpsPointA = gps.get_midpoint(gpsPointA, gpsPointB);
			logger.storeRecord(gps.get_timestamp(), gpsPointA.lat, gpsPointA.lon, gpsPointA.alt, gpsPointA.acc, battery.getPercentage());

			gui.draw_points(logger.pointsSaved());
			gui.draw_flash_used(map(logger.blocksUsed(), 0, logger.blocksTotal(), 0, 100));
		}

		WiFi.scanDelete();
	}

	if (logger.needErase()) {
#ifdef SERIAL_DEBUG
		Serial.println();
		Serial.println("!!! ERASING FLASH !!!");
		Serial.println();
#endif
		gui.clear_screen();
		gui.draw_text(0, 0, 2, "ERASING...");
		// Update screen?

		logger.eraseFlash([](int percent) {
			gui.draw_progress(percent);
			});
		gui.clear_screen();
	}

	if (battery.is_low()) {
#ifdef SERIAL_DEBUG
		Serial.println(F("Battery low! Shutting down..."));
#endif
		gui.draw_text(0, 0, 2, "LOW BAT");
		gui.redraw();

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
