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
	double lat;
	double lon;
	float alt;
	double acc;
};

class UbloxGPS {
public:
	UbloxGPS(Stream& serialPort);

	void reconfigure(unsigned long baud);
	void setUpdateRate(uint16_t ms);

	void update();
	void start();
	void stop();
	void shutdown();
	int sat_used();
	int sat_total();
	float speed(bool metric = true); // true=km/h, false=miles/h
	float speed_ms();
	float altitude();
	float course();
	uint32_t get_timestamp();
	static GPSPoint get_midpoint(GPSPoint p1, GPSPoint p2);

	bool fix = false;
	double lat = 0;
	double lon = 0;
	double hdop = 0;
	double acc = 0;

	static const int MAX_SATS = 16;
	GPSSatellite satellites[MAX_SATS];
	int satellites_count = 0;

	bool set_time_autoupdate = true;
	uint32_t last_timestamp = 0;

	struct tm get_time() { return _current_time; }
	void (*on_update)() = nullptr;

private:
	Stream& _serial;
	char _buffer[100];
	uint8_t _buf_idx = 0;
	struct tm _current_time;
	int _last_sync_day = -1;
	float _speed_knots = 0;
	float _altitude = 0;
	float _course = 0;
	uint32_t _block_time_num = 0; // HHMMSS
	bool _block_valid = false;
	uint8_t _used_ids[12];
	uint8_t _used_count = 0;

	double _last_hdop = 0;
	double _last_altitude = 0;
	double _last_lat = 0;
	double _last_lon = 0;

	bool _validateChecksum(char* line);
	void _parseField(const char* line, int fieldIndex, char* dest, int maxLen);
	void _parseNMEA(char* line);
	void _updateSystemTime();
};
