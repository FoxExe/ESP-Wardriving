#pragma once

#include <Arduino.h>

#pragma pack(push, 1)
struct GPSSatellite {
	uint8_t id;
	uint8_t snr;
	bool is_used;
};
#pragma pack(pop)

struct GPSPoint {
	float lat;
	float lon;
	float alt;
	uint16_t acc;
	uint32_t ts;
};

uint16_t hdopToAccuracy(uint16_t hdop) {
	if (hdop == 0) { return 999; }
	return (uint16_t)((hdop * 0.0045f) + 0.5f);
}

GPSPoint getGPSMidpoint(const GPSPoint& p1, const GPSPoint& p2) {
	GPSPoint middle;
	middle.lat = (p1.lat + p2.lat) / 2.0;
	middle.lon = (p1.lon + p2.lon) / 2.0;
	middle.alt = (p1.alt + p2.alt) / 2.0;
	middle.acc = (uint16_t)((p1.acc + p2.acc + 1) / 2);
	//middle.ts = (p1.ts + p2.ts) / 2;
	middle.ts = p1.ts;
	return middle;
}

class UbloxGPS {
private:
	Stream* _serial;

	void sendUBX(uint8_t* msg, uint8_t len) {
		uint8_t ck_a = 0, ck_b = 0;
		for (uint8_t i = 2; i < len - 2; i++) {
			ck_a += msg[i];
			ck_b += ck_a;
		}
		msg[len - 2] = ck_a;
		msg[len - 1] = ck_b;

		_serial->write(msg, len);
	}

public:
	UbloxGPS(Stream& serial) : _serial(&serial) {}

	void sleep() {
		uint8_t payload[] = {
			0xB5, 0x62,
			0x06, 0x11,
			0x02, 0x00, 0x08, 0x01,
			0x00, 0x00
		};
		sendUBX(payload, sizeof(payload));
	}

	void shutdown() {
		uint8_t payload[] = {
			0xB5, 0x62, // Header (Sync)
			0x02, 0x41, // Message ID (RXM-PMREQ)
			0x08, 0x00, // Payload size (8bytes)
			0x00, 0x00, 0x00, 0x00, // Reserved
			0x02, 0x00, 0x00, 0x00, // Mode: 0x02 = Backup (sleep mode)
			0x00, 0x00  // CRC, calculated inside sendUBX(). Default for this CMD: 0x4D, 0x3B
		};
		sendUBX(payload, sizeof(payload));
	}

	void wakeUp() {
		_serial->write(0xFF);
	}

	void setUpdateRate(uint16_t intervalMs) {
		uint8_t payload[] = {
			0xB5, 0x62,
			0x06, 0x08,
			0x06, 0x00,
			(uint8_t)(intervalMs & 0xFF), (uint8_t)(intervalMs >> 8),
			0x01, 0x00, 0x01, 0x00,
			0x00, 0x00
		};
		sendUBX(payload, sizeof(payload));
	}

	void setBaudRate(uint32_t baud) {
		uint8_t b0 = baud & 0xFF;
		uint8_t b1 = (baud >> 8) & 0xFF;
		uint8_t b2 = (baud >> 16) & 0xFF;
		uint8_t b3 = (baud >> 24) & 0xFF;

		uint8_t payload[] = {
			0xB5, 0x62,              // Header (Sync)
			0x06, 0x00,              // Class: CFG, ID: PRT
			0x14, 0x00,              // Length: 20 bytes
			0x01, 0x00, 0x00, 0x00,  // Port ID (1 = UART), Reserved, TX Ready pins (2 bytes, disabled)
			0xD0, 0x08, 0x00, 0x00,  // Mode: 8N1
			b0, b1, b2, b3,          // Baudrate
			0x07, 0x00, 0x03, 0x00,  // In/Out protocols (UBX + NMEA)
			0x00, 0x00, 0x00, 0x00,  // 2xFlags, 2xReserved
			0x00, 0x00               // Checksum/CRC
		};
		sendUBX(payload, sizeof(payload));
	}

	void saveConfig() {
		uint8_t payload[] = {
			0xB5, 0x62,
			0x06, 0x09,
			0x0D, 0x00,
			0x00, 0x00, 0x00, 0x00, // Clear mask
			0xFF, 0xFF, 0x00, 0x00, // Save mask (all)
			0x00, 0x00, 0x00, 0x00, // Load mask
			0x03,                   // Dev mask (BBR + Flash)
			0x00, 0x00              // CS
		};
		sendUBX(payload, sizeof(payload));
	}
};
