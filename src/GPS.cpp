#include "GPS.h"
#include <sys/time.h>


const uint8_t UBX_SLEEP[] PROGMEM = {
	0xB5, 0x62,
	0x02, 0x41,
	0x08, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, // Power mode: Backup
	0x4D, 0x3B
};

const uint8_t UBX_SHUTDOWN[] PROGMEM = {
	0xB5, 0x62, // Sync
	0x02, 0x41, // RXM-PMREQ
	0x08, 0x00, // Payload length
	0x00, 0x00, 0x00, 0x00, // Reserved
	0x01, 0x00, 0x00, 0x00, // Power mode: Off
	0x4C, 0x37  // Checksum
};

UbloxGPS::UbloxGPS(Stream& serialPort) : _serial(serialPort) {
	memset(satellites, 0, sizeof(satellites));
	memset(_buffer, 0, sizeof(_buffer));
	memset(&_current_time, 0, sizeof(_current_time));
}

void UbloxGPS::reconfigure(unsigned long baud) {
	// UBX-CFG-PRT (Port Configuration)
	uint8_t cfg[] = {
		0xB5, 0x62,       // Sync chars
		0x06, 0x00,       // Class: CFG, ID: PRT
		0x14, 0x00,       // Length: 20 bytes
		0x01,             // Port ID (1 = UART)
		0x00,             // Reserved
		0x00, 0x00,       // TX Ready pins (disabled)
		0xD0, 0x08, 0x00, 0x00, // Mode: 8N1
		0x00, 0x00, 0x00, 0x00, // Baudrate (индексы 14, 15, 16, 17)
		0x07, 0x00,       // Input protocols (UBX + NMEA)
		0x03, 0x00,       // Output protocols (UBX + NMEA)
		0x00, 0x00,       // Flags
		0x00, 0x00,       // Reserved
		0x00, 0x00        // Checksum (индексы 26, 27)
	};

	// Write speed as Little Endian
	cfg[14] = (uint8_t)(baud & 0xFF);
	cfg[15] = (uint8_t)((baud >> 8) & 0xFF);
	cfg[16] = (uint8_t)((baud >> 16) & 0xFF);
	cfg[17] = (uint8_t)((baud >> 24) & 0xFF);

	// CRC Calc (Fletcher-8)
	uint8_t ck_a = 0, ck_b = 0;
	for (int i = 2; i < 26; i++) {
		ck_a = ck_a + cfg[i];
		ck_b = ck_b + ck_a;
	}
	cfg[26] = ck_a;
	cfg[27] = ck_b;

	_serial.write(cfg, sizeof(cfg));
}

void UbloxGPS::setUpdateRate(uint16_t ms) {
	uint8_t cfg_rate[] = {
		0xB5, 0x62,
		0x06, 0x08,
		0x06, 0x00,
		0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
		0x00, 0x00
	};

	cfg_rate[6] = ms & 0xFF;
	cfg_rate[7] = (ms >> 8) & 0xFF;

	uint8_t ck_a = 0, ck_b = 0;
	for (int i = 2; i < 12; i++) {
		ck_a += cfg_rate[i];
		ck_b += ck_a;
	}
	cfg_rate[12] = ck_a;
	cfg_rate[13] = ck_b;

	_serial.write(cfg_rate, sizeof(cfg_rate));
}

void UbloxGPS::update() {
	while (_serial.available() > 0) {
		char c = _serial.read();

		if (c == '\n') {
			_buffer[_buf_idx] = '\0';
			if (_buf_idx > 10) {
				_parseNMEA(_buffer);
			}
			_buf_idx = 0;
		}
		else if (c != '\r') {
			if (_buf_idx < sizeof(_buffer) - 1) {
				_buffer[_buf_idx++] = c;
			}
			else {
				// Buffer overflow fix
				_buf_idx = 0;
			}
		}
	}
}

void UbloxGPS::_parseField(const char* line, int fieldIndex, char* dest, int maxLen) {
	int currentField = 0;
	int start = 0;
	dest[0] = '\0';

	for (int i = 0; ; i++) {
		if (line[i] == ',' || line[i] == '\0' || line[i] == '*') {
			if (currentField == fieldIndex) {
				int len = i - start;
				if (len >= maxLen) len = maxLen - 1;
				if (len > 0) {
					strncpy(dest, &line[start], len);
					dest[len] = '\0';
				}
				return;
			}

			if (line[i] == '*') break;

			start = i + 1;
			currentField++;
		}
	}
}

uint32_t parseTimeToNum(const char* f) {
	if (strlen(f) < 6) return 0;
	return (uint32_t)((f[0] - '0') * 100000 + (f[1] - '0') * 10000 + (f[2] - '0') * 1000 + (f[3] - '0') * 100 + (f[4] - '0') * 10 + (f[5] - '0'));
}

void UbloxGPS::_parseNMEA(char* line) {
#ifdef SERIAL_DEBUG
	Serial.print("[GPS] ");
	Serial.println(line);
#endif

	char* star = strrchr(line, '*');
	if (!star) return;

	uint8_t expected = (uint8_t)strtol(star + 1, NULL, 16);
	uint8_t actual = 0;
	for (char* c = line + 1; c < star; c++) actual ^= *c;
	if (actual != expected) return;

	char f[16];

	if (strncmp(line, "$GPRMC", 6) == 0) {
		// $GPRMC,184140.00,A,5000.00000,N,05000.00000,E,0.060,,260426,,,A*71
		// $GPRMC,182113.00,V,,,,,,,260426,,,N*71
		_parseField(line, 1, f, sizeof(f));
		_block_time_num = parseTimeToNum(f);
		if (_block_time_num == 0) return;

		_block_valid = true;
		_used_count = 0;

		_parseField(line, 2, f, sizeof(f));
		fix = (f[0] == 'A');

		// Latitude
		_parseField(line, 3, f, sizeof(f));
		if (f[0] != '\0') {
			double v = atof(f);
			int deg = (int)(v / 100);
			_last_lat = deg + (v - deg * 100.0) / 60.0;
			_parseField(line, 4, f, sizeof(f));
			if (f[0] == 'S') _last_lat = -_last_lat;
		}

		// Longtitude
		_parseField(line, 5, f, sizeof(f));
		if (f[0] != '\0') {
			double v = atof(f);
			int deg = (int)(v / 100);
			_last_lon = deg + (v - deg * 100.0) / 60.0;
			_parseField(line, 6, f, sizeof(f));
			if (f[0] == 'W') _last_lon = -_last_lon;
		}

		// Speed
		_parseField(line, 7, f, sizeof(f));
		if (f[0] != '\0') _speed_knots = atof(f);

		// Course
		_parseField(line, 8, f, sizeof(f));
		if (f[0] != '\0') _course = atof(f);

		// Date
		_parseField(line, 9, f, sizeof(f));
		if (strlen(f) >= 6) {
			_current_time.tm_mday = (f[0] - '0') * 10 + (f[1] - '0');
			_current_time.tm_mon = ((f[2] - '0') * 10 + (f[3] - '0')) - 1;
			_current_time.tm_year = ((f[4] - '0') * 10 + (f[5] - '0')) + 100;
		}
	}
	else if (strncmp(line, "$GPVTG", 6) == 0) {
		// $GPVTG,,T,,M,0.059,N,0.110,K,A*2F
		if (!_block_valid) return;

		// _course
		_parseField(line, 1, f, sizeof(f));
		if (f[0] != '\0') _course = atof(f);

		// Speed
		_parseField(line, 5, f, sizeof(f));
		if (f[0] != '\0') _speed_knots = atof(f);
	}
	else if (strncmp(line, "$GPGGA", 6) == 0) {
		// $GPGGA,182113.00,,,,,0,03,7.66,,,,,,*5A
		if (!_block_valid) return;
		_parseField(line, 1, f, sizeof(f));
		if (parseTimeToNum(f) != _block_time_num) { _block_valid = false; return; }

		_parseField(line, 8, f, sizeof(f)); // HDOP
		if (f[0] != '\0') _last_hdop = atof(f);

		_parseField(line, 9, f, sizeof(f)); // Altitude
		if (f[0] != '\0') _last_altitude = atof(f);
	}
	else if (strncmp(line, "$GPGSA", 6) == 0) {
		// $GPGSA,A,1,26,28,31,,,,,,,,,,7.73,7.66,1.00*09
		if (!_block_valid) return;
		_used_count = 0;
		for (int i = 3; i <= 14; i++) {
			_parseField(line, i, f, sizeof(f));
			if (f[0] != '\0' && _used_count < 12) {
				_used_ids[_used_count++] = (uint8_t)atoi(f);
			}
		}
	}
	else if (strncmp(line, "$GPGSV", 6) == 0) {
		// $GPGSV,2,1,05,01,06,202,,03,,,29,26,41,102,42,28,24,055,39*42
		if (!_block_valid) return;
		_parseField(line, 2, f, sizeof(f));
		if (atoi(f) == 1) satellites_count = 0;

		_parseField(line, 3, f, sizeof(f));
		int totalSats = atoi(f);
		_parseField(line, 2, f, sizeof(f));
		int msgNum = atoi(f);

		int satsInThisLine = totalSats - (msgNum - 1) * 4;
		if (satsInThisLine > 4) satsInThisLine = 4;

		for (int i = 0; i < satsInThisLine; i++) {
			_parseField(line, 4 + i * 4, f, sizeof(f));
			if (f[0] == '\0') break;
			uint8_t id = (uint8_t)atoi(f);

			bool isDuplicate = false;
			for (int j = 0; j < satellites_count; j++) if (satellites[j].id == id) { isDuplicate = true; break; }

			if (!isDuplicate && satellites_count < MAX_SATS) {
				satellites[satellites_count].id = id;
				_parseField(line, 7 + i * 4, f, sizeof(f));
				satellites[satellites_count].snr = (f[0] != '\0') ? (uint8_t)atoi(f) : 0;
				satellites[satellites_count].is_used = false;
				satellites_count++;
			}
		}
	}
	else if (strncmp(line, "$GPGLL", 6) == 0) {
		// $GPGLL,,,,,182113.00,V,N*42
		if (!_block_valid) return;
		_parseField(line, 5, f, sizeof(f));
		if (parseTimeToNum(f) != _block_time_num) { _block_valid = false; return; }

		// End of data block. Save it.
		lat = _last_lat;
		lon = _last_lon;
		hdop = _last_hdop;
		acc = hdop * 5.0;
		_altitude = _last_altitude;

		for (int i = 0; i < satellites_count; i++) {
			satellites[i].is_used = false;
			for (int j = 0; j < _used_count; j++) {
				if (satellites[i].id == _used_ids[j]) { satellites[i].is_used = true; break; }
			}
		}

		// Update time
		_current_time.tm_hour = _block_time_num / 10000;
		_current_time.tm_min = (_block_time_num / 100) % 100;
		_current_time.tm_sec = _block_time_num % 100;

		if (set_time_autoupdate) _updateSystemTime();

		_block_valid = false;
		if (on_update) on_update();
	}
}

void UbloxGPS::_updateSystemTime() {
	if (_current_time.tm_year > 100 && _current_time.tm_mday != _last_sync_day) {
		timeval tv = { mktime(&_current_time), 0 };
		settimeofday(&tv, NULL);
		_last_sync_day = _current_time.tm_mday;
	}
}

void UbloxGPS::start() {
	_serial.write(0xFF);
	delay(10);
}

void UbloxGPS::stop() {
	uint8_t cmd[16];
	memcpy_P(cmd, UBX_SLEEP, 16);
	_serial.write(cmd, 16);
}

void UbloxGPS::shutdown() {
	uint8_t cmd[16];
	memcpy_P(cmd, UBX_SHUTDOWN, 16);
	_serial.write(cmd, 16);

	// Reset values
	fix = false;
	satellites_count = 0;
}

int UbloxGPS::sat_total() { return satellites_count; }

int UbloxGPS::sat_used() {
	int count = 0;
	for (int i = 0; i < satellites_count; i++) {
		if (satellites[i].is_used) count++;
	}
	return count;
}

float UbloxGPS::speed(bool metric) {
	// Kilometers/h or Miles/h
	float res = _speed_knots * (metric ? 1.852f : 1.15078f);
	return res < 0.5f ? 0 : res; // Filter noise
}

float UbloxGPS::speed_ms() {
	// Meters/s
	float res = _speed_knots * 0.51444f;
	return (res < 0.15f) ? 0.0f : res;
}

float UbloxGPS::altitude() {
	return _altitude;
}

float UbloxGPS::course() {
	if (_speed_knots < 0.3f) return 0.0f;
	return _course;
}

uint32_t UbloxGPS::get_timestamp() {
	if (_current_time.tm_year < 100) return 0;
	return (uint32_t)mktime(&_current_time);
}

GPSPoint UbloxGPS::get_midpoint(GPSPoint p1, GPSPoint p2) {
	// Check if points is equal
	if (abs(p1.lat - p2.lat) < 0.0000001 && abs(p1.lon - p2.lon) < 0.0000001) {
		return {
			p1.lat,
			p1.lon,
			(p1.alt + p2.alt) / 2.0f,
			(p1.acc + p2.acc) / 2.0f
		};
	}

	// Spherical Midpoint calculation
	double lat1 = p1.lat * M_PI / 180.0;
	double lon1 = p1.lon * M_PI / 180.0;
	double lat2 = p2.lat * M_PI / 180.0;
	double lon2 = p2.lon * M_PI / 180.0;

	double dLon = lon2 - lon1;

	double Bx = cos(lat2) * cos(dLon);
	double By = cos(lat2) * sin(dLon);

	double midLat = atan2(sin(lat1) + sin(lat2), sqrt((cos(lat1) + Bx) * (cos(lat1) + Bx) + By * By));
	double midLon = lon1 + atan2(By, cos(lat1) + Bx);

	if (midLon > M_PI) midLon -= 2 * M_PI;
	if (midLon < -M_PI) midLon += 2 * M_PI;

	return {
		midLat * 180.0 / M_PI,
		midLon * 180.0 / M_PI,
		(p1.alt + p2.alt) / 2.0f,
		(p1.acc + p2.acc) / 2.0f
	};
}

