#pragma once
#include <Arduino.h>
#include <GyverOLED.h>
#include <ESP8266WiFi.h>


#define MAX_BARS_1PX 25
#define MAX_BARS_2PX 16
#define MAX_WIFI_CHANNELS 14 // JP=14, EU=13, Others=11-12


class GUI {
private:
	GyverOLED<SSD1306_128x32, OLED_BUFFER> _screen;

	void _draw_graph(int x, int y, int h, int v_min, int v_max, uint8_t bar_w, int* items, uint8_t count, bool* flags = nullptr);
public:
	void begin();

	void clear_screen() { _screen.clear(); _screen.update(); }
	void update() { _screen.update(); };

	void draw_text(int x, int y, uint8_t size, const char* text);
	void draw_textf(int x, int y, uint8_t size, int len, const char* format, ...);
	void draw_progressbar(int x, int y, int w, int h, int val, int val_min = 0, int val_max = 100);
	void draw_ruler(int x, int y, int h, bool mirror = false);

	void draw_loading(const char* msg);
	void draw_progress(int value);

	void draw_time(tm* ptm = nullptr);
	void draw_points(unsigned int points);
	void draw_battery(unsigned int charge);
	void draw_isrunning_icon(bool flag);
	void draw_accuracy(unsigned int acc);
	void draw_flash_used(unsigned int percent);
	void draw_wifi_status(int8_t status);

	void draw_graph_gps(int* values, bool* is_used, unsigned int count);
	void draw_graph_wifi(int* values);
};
