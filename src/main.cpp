#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <time.h>

#include <EncButton.h>
#include <LwipDhcpServer.h>
#include <NMEAGPS.h>

//#include <SoftwareSerial.h>

#include "Settings.h"
#include "Logger.h"
#include "Battery.h"
#include "WebServer.h"
#include "GUI.h"
#include "GPS_Utils.h"

#define REPORT_SYS 0x01
#define REPORT_GPS 0x02
#define REPORT_WIFI 0x03

WebServer web(WEB_PORT);
EncButton btn(0);
Battery battery;
GUI gui;

//SoftwareSerial gpsPort(15, 2);
NMEAGPS gps;
gps_fix fix;
UbloxGPS gps_ctrl(Serial); // gpsPort
GPSPoint gpsPointA = { 0 };
GPSPoint gpsPointB = { 0 };

unsigned long lastScanMillis = 0;
unsigned long lastScreenUpdate = 0;
unsigned long lastClockSync = 0;
bool process_point = false;
bool has_new_fix = false;

#pragma pack(push, 1)
struct SystemReport {
	uint8_t bat_charge;
	uint32_t blocks_total;
	uint32_t blocks_used;
	uint32_t heap_free;
	uint32_t heap_max;
	uint8_t ap_clients;
	uint32_t saved_points;
	uint16_t current_block;
};

struct APInfo {
	uint8_t mac[6];
	//char ssid[32];
	int8_t rssi;
	uint8_t chan;
	uint8_t enc;
	uint8_t ssid_len;
};

struct GPSDataReport {
	uint32_t timestamp;
	float lat;
	float lon;
	int16_t alt;
	uint16_t acc;
};
#pragma pack(pop)

void SendWiFiReport() {
	if (web.WSConnections() == 0) { return; };

	int8_t results = WiFi.scanComplete();
	if (results < 0) { return; }

	uint32_t packet_size = 1; // Reserve 1 byte for packet type
	for (int8_t i = 0; i < results; i++) {
		const bss_info* ap = WiFi.getScanInfoByIndex(i);
		if (!ap) continue;
		packet_size += sizeof(APInfo) + ap->ssid_len;
	}

	// Prepare buffer
	uint8_t* buff = (uint8_t*)malloc(packet_size);
	if (buff == nullptr) return;

	// Set packet type
	buff[0] = REPORT_WIFI;

	// Fill Wi-Fi info objects
	uint32_t pos = 1;
	for (int8_t i = 0; i < results; i++) {
		const bss_info* ap = WiFi.getScanInfoByIndex(i);  // Much faster and less RAM consumed!
		if (!ap) continue;

		APInfo a = { 0 };
		memcpy(a.mac, ap->bssid, 6);
		a.chan = ap->channel;
		a.rssi = ap->rssi;
		a.ssid_len = ap->ssid_len;

		// ESP8266WiFiScanClass::encryptionType()
		switch (ap->authmode) {
		case AUTH_OPEN:         a.enc = ENC_TYPE_NONE; break;
		case AUTH_WEP:          a.enc = ENC_TYPE_WEP;  break;
		case AUTH_WPA_PSK:      a.enc = ENC_TYPE_TKIP; break;
		case AUTH_WPA2_PSK:     a.enc = ENC_TYPE_CCMP; break;
		case AUTH_WPA_WPA2_PSK: a.enc = ENC_TYPE_AUTO; break;
		default:                a.enc = ENC_TYPE_AUTO; break;
		}

		memcpy(buff + pos, &a, sizeof(APInfo));
		pos += sizeof(APInfo);

		if (ap->ssid_len > 0) {
			memcpy(buff + pos, ap->ssid, ap->ssid_len);
			pos += ap->ssid_len;
		}
	}

	web.sendWSData(buff, packet_size);
	free(buff);
}

void sendGPSReport(gps_fix& fix) {
	if (web.WSConnections() == 0) { return; };

	GPSDataReport report = { 0 };
	report.timestamp = (NeoGPS::clock_t)fix.dateTime + Y2K_OFFSET;
	report.lat = fix.latitude();
	report.lon = fix.longitude();
	report.alt = fix.altitude();
	report.acc = hdopToAccuracy(fix.hdop);

	uint16_t size = 1 + sizeof(report) + (sizeof(NMEAGPS::satellite_view_t) * gps.sat_count);
	uint8_t* buff = (uint8_t*)malloc(size);
	if (buff == nullptr) return;
	buff[0] = REPORT_GPS;
	memcpy(&buff[1], &report, sizeof(report));
	if (gps.sat_count > 0) {
		memcpy(&buff[1 + sizeof(report)], gps.satellites, sizeof(NMEAGPS::satellite_view_t) * gps.sat_count);
	}
	web.sendWSData(buff, size);
	free(buff);
}

void sendSysReport() {
	if (web.WSConnections() == 0) { return; };

	// Fill report
	SystemReport report = { 0 };
	report.bat_charge = battery.getPercentage();
	report.blocks_total = logger.blocksTotal();
	report.blocks_used = logger.blocksUsed();
	report.heap_free = ESP.getFreeHeap();
	report.heap_max = ESP.getMaxFreeBlockSize();
	report.ap_clients = WiFi.softAPgetStationNum();
	report.saved_points = logger.pointsSaved();
	report.current_block = logger.getCurrentBlockID();

	// Send report
	uint16_t size = 1 + sizeof(report);
	uint8_t* buff = (uint8_t*)malloc(size);
	if (buff == nullptr) return;
	buff[0] = REPORT_SYS;
	memcpy(&buff[1], &report, sizeof(report));
	web.sendWSData(buff, size);
	free(buff);

#ifdef SERIAL_DEBUG
	unsigned long totalSeconds = millis() / 1000;
	int seconds = totalSeconds % 60;
	int minutes = (totalSeconds / 60) % 60;
	int hours = (totalSeconds / 3600);
	Serial1.printf("[ UPTIME: %4d:%02d:%02d ]", hours, minutes, seconds);
	Serial1.printf(" | HEAP: %5d", ESP.getFreeHeap());
	Serial1.printf(" | BAT: %3d%% (%7.4fv) ", battery.getPercentage(), battery.getVoltage());
	Serial1.printf(" | USED: %3d/%3d CURRENT: %3d SAVED: %7d", logger.blocksUsed(), logger.blocksTotal(), logger.getCurrentBlockID(), logger.pointsSaved());
	Serial1.printf(" | WIFI: %2d", WiFi.scanComplete());
	Serial1.println();
#endif
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
	int8_t results = WiFi.scanComplete();
	if (results <= 0) return;

	int channel_rssi[MAX_WIFI_CHANNELS];
	for (uint8_t c = 0; c < MAX_WIFI_CHANNELS; c++) {
		channel_rssi[c] = -100;
	}

	for (int8_t i = 0; i < results; i++) {
		const bss_info* it = WiFi.getScanInfoByIndex(i);
		if (!it) continue;

		uint8_t ch_idx = it->channel - 1;
		if (ch_idx < MAX_WIFI_CHANNELS) {
			if (it->rssi > channel_rssi[ch_idx]) {
				channel_rssi[ch_idx] = it->rssi;
			}
		}
	}

	gui.draw_graph_wifi(channel_rssi);
}

void handleButtons() {
	btn.tick();
	/*
		if (btn.click()) {
			gps_ctrl.wakeUp();
	#ifdef SERIAL_DEBUG
			Serial1.println(F("GPS IS ON"));
	#endif
		}

		if (btn.hold()) {
			gps_ctrl.shutdown();
	#ifdef SERIAL_DEBUG
			Serial1.println(F("GPS IS OFF"));
	#endif
		}
	*/
}

void handleGpsUpdate() {
	// Updatre GPS graph
	int snr_values[gps.sat_count];
	bool tracked[gps.sat_count];
	uint8_t sat_used = 0;
	for (uint8_t i = 0; i < gps.sat_count; i++) {
		snr_values[i] = gps.satellites[i].snr;
		tracked[i] = gps.satellites[i].tracked;
		if (gps.satellites[i].tracked) sat_used++;
	}
	gui.draw_graph_gps(snr_values, tracked, gps.sat_count);

	uint16_t acc = fix.valid.hdop ? hdopToAccuracy(fix.hdop) : 999;
	gui.draw_accuracy(acc);
	gui.draw_isrunning_icon(fix.valid.location && acc <= cfg.min_acc);

	// Send report to WS
	sendGPSReport(fix);

	if (fix.valid.date && fix.valid.time) {
		// Sync system time, if possible
		if (lastClockSync == 0 || millis() - lastClockSync >= CLOCK_SYNC_INTEVAL) {
			timeval tv;
			tv.tv_sec = (NeoGPS::clock_t)fix.dateTime + Y2K_OFFSET;
			tv.tv_usec = 0;
			settimeofday(&tv, NULL);
			lastClockSync = millis();
		}
	}

	if (fix.valid.location && acc <= cfg.min_acc) {
		uint32_t timestamp = (uint32_t)time(nullptr); // Yeah, use system time, because sometime we can't get GPS time :(

		if (millis() - lastScanMillis >= cfg.scan_interval * 1000UL) {
			if (WiFi.scanComplete() < 0) {
				WiFi.scanNetworks(true, true); // Run async, scan hidden networks
				gui.draw_wifi_status(WIFI_SCAN_RUNNING);
				gpsPointA = { fix.latitude(), fix.longitude(), fix.altitude(), hdopToAccuracy(fix.hdop), timestamp };
				lastScanMillis = millis();
				process_point = true;
			}
		}

		if (process_point && WiFi.scanComplete() > 0) {
			gpsPointB = { fix.latitude(), fix.longitude(), fix.altitude(), hdopToAccuracy(fix.hdop), timestamp };;
			if (gpsPointB.ts - gpsPointA.ts < 10) {
				GPSPoint tmp = getGPSMidpoint(gpsPointA, gpsPointB);
				logger.storeRecord(tmp.ts, tmp.lat, tmp.lon, tmp.alt, tmp.acc, battery.getPercentage());
				gui.draw_points(logger.pointsSaved());
				gui.draw_flash_used(map(logger.blocksUsed(), 0, logger.blocksTotal(), 0, 100));
			}
			else {
				// Something wrong. WiFi scan take too much time.
			}
			WiFi.scanDelete();
			process_point = false;
		}
	}

#ifdef SERIAL_DEBUG
	time_t now = time(nullptr);
	struct tm* ptm = localtime(&now);
	Serial1.printf("[%02d.%02d.%04d %02d:%02d:%02d]", ptm->tm_mday, ptm->tm_mon + 1, ptm->tm_year + 1900, ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
	Serial1.printf(" | LAT: %10.6f  LON: %11.6f  ALT: %7.4f  ACC: %4d  SATS: %2d/%2d", fix.latitude(), fix.longitude(), fix.altitude(), hdopToAccuracy(fix.hdop), sat_used, gps.sat_count);
	Serial1.println("\n\tID   RSSI U SIGNAL");
	for (uint8_t i = 0; i < gps.sat_count; i++) {
		Serial1.printf("\t%2d %3ddbm %d ", gps.satellites[i].id, gps.satellites[i].snr, gps.satellites[i].tracked);
		int bars = map(gps.satellites[i].snr, 0, 100, 0, 32);
		for (int j = 0; j < bars; ++j) { Serial1.print("#"); }
		Serial1.println("");
	}
#endif
}

void onDhcpOptions(const DhcpServer& server, DhcpServer::OptionsBuffer& options) {
	// Append route to 192.168.4.0/24
	uint8_t route_data[] = { 0x18, 0xC0, 0xA8, 0x04, 0xC0, 0xA8, 0x04, 0x01 };
	options.add(121, route_data, sizeof(route_data));  // RFC 3442
	options.add(249, route_data, sizeof(route_data));  // Microsoft Classless Static Route
}

// ================================================================
void setup() {
#ifdef SERIAL_DEBUG
	// Debug
	//Serial1.begin(115200); // Debug
	Serial1.begin(115200);
	Serial1.println();
	Serial1.println(F("Loading..."));
	Serial1.println();
#endif

	// Button (GPIO-0)
	btn.setHoldTimeout(1000); // 1 sec

	// GUI/Display init
	gui.begin();
	gui.clear_screen();

	// Flash init
	gui.draw_loading("Flash");
	logger.setRotation(cfg.rotate_logs);
	if (!logger.begin()) {
		gui.draw_loading("Flash error!");
		while (true) { yield(); delay(100); }
	}

	// GPS
	gui.draw_loading("GPS");
	//gpsPort.begin(9600);
	//gpsPort.begin(9600, SWSERIAL_8N1, 15, 2, false, 256); // Increase buffer up to [8 * 82 * 2]
	Serial.begin(9600);
	gps_ctrl.setBaudRate(115200);
	Serial.flush();
	delay(50);
	Serial.end();
	delay(50);
	Serial.setRxBufferSize(1024);
	Serial.begin(115200);

	// Configure Wi-Fi
	gui.draw_loading("Wi-Fi");
	WiFi.setAutoConnect(false); // fix "-1" scan status after boot
	WiFi.mode(WIFI_AP_STA);
	WiFi.softAP(cfg.ssid, cfg.pass);
	WiFi.hostname("GPSLogger");
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
	gui.update();
}

// ================================================================
void loop() {
	web.update();
	battery.update();
	handleButtons();

	if (WiFi.scanComplete() >= 0) {
		updateWiFiGraph();
		SendWiFiReport(); // to browser/websocket
		if (!process_point) WiFi.scanDelete();
	}

	while (gps.available(Serial)) {
		fix = gps.read();
		has_new_fix = true;
	}

	if (has_new_fix) {
		has_new_fix = false;
		handleGpsUpdate();
	}

	// Update stats and display every one second
	if (millis() - lastScreenUpdate >= 1000) {
		lastScreenUpdate = millis();

		// Update time on screen
		time_t now = time(nullptr) + (cfg.tz_offset * 60);
		struct tm ptm = *gmtime(&now);
		gui.draw_time(&ptm);
		gui.draw_battery(battery.getPercentage());
		gui.update();

		// Send report to WS
		sendSysReport();
	}

	if (logger.needErase()) {
#ifdef SERIAL_DEBUG
		Serial1.println();
		Serial1.println("!!! ERASING FLASH !!!");
		Serial1.println();
#endif
		gui.clear_screen();
		gui.draw_text(0, 0, 2, "ERASING...");
		gui.update();
		// Update screen?

		logger.eraseFlash([](int percent) {
			gui.draw_progress(percent);
			gui.update();
			});
		gui.clear_screen();
		gui.update();
	}

	if (battery.is_low()) {
#ifdef SERIAL_DEBUG
		Serial1.println(F("Battery low! Shutting down..."));
#endif
		gui.draw_text(0, 0, 2, "LOW BAT");
		gui.update();

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
