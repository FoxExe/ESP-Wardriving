#include "GUI.h"


#define FONT_W 6
#define FONT_H 8



GUI::GUI() {
	_screen.init();
}

void GUI::_draw_graph(int x, int y, int h, int v_min, int v_max, uint8_t bar_w, int* items, uint8_t count, bool* flags) {
	int bottom = y + h - 1;

	// Draw bars
	for (uint8_t i = 0; i < count; i++) {
		int val = constrain(items[i], v_min, v_max);
		uint8_t bar_h = map(val, v_min, v_max, 0, h);

		// Make bars always visible, if value > v_min
		if (items[i] > v_min && bar_h == 0) bar_h = 1;

		bool hasFlag = (flags != nullptr && flags[i]);
		if (hasFlag && bar_h < 2) bar_h = 2;

		if (bar_h > 0) {
			int cur_x = x + (i * (bar_w + 1));
			int right_x = cur_x + bar_w - 1;
			int top_y = bottom - bar_h + 1;

			_screen.rect(cur_x, bottom, right_x, top_y, OLED_FILL);

			if (hasFlag) {
				_screen.rect(cur_x, top_y + 1, right_x, top_y + 1, OLED_CLEAR);
			}
		}
	}
}

void GUI::draw_ruler(int x, int y, int h, bool mirror) {
	// Clear area
	_screen.rect(x, y, x + 3, y + h - 1, OLED_CLEAR);

	// Vertical line
	int lineX = mirror ? x + 3 : x;
	_screen.line(lineX, y, lineX, y + h - 1, OLED_FILL);

	// Marks on ruler
	for (int i = 0; i < h; i++) {
		int currentY = y + h - 1 - i;

		if (i > 0 && i % 10 == 0) {
			_screen.line(mirror ? x + 1 : x + 2, currentY, mirror ? x + 2 : x + 1, currentY, OLED_FILL);
		}
		else if (i % 2 == 0) {
			_screen.dot(mirror ? x + 2 : x + 1, currentY, OLED_FILL);
		}
	}
}

void GUI::draw_text(int x, int y, uint8_t size, const char* text) {
	int clear_len = strlen(text);
	_screen.rect(x, y, x + (clear_len * FONT_W * size) - 1, y + (FONT_H * size) - 1, OLED_CLEAR);
	_screen.setScale(size);
	_screen.setCursorXY(x, y);
	_screen.print(text);
}

void GUI::draw_textf(int x, int y, uint8_t size, int len, const char* format, ...) {
	_screen.rect(x, y, x + (len * FONT_W * size) - 1, y + (FONT_H * size) - 1, OLED_CLEAR);
	_screen.setScale(size);
	_screen.setCursorXY(x, y);

	char buf[len + 1]; // va auto append \0 at end!
	va_list args;
	va_start(args, format);
	vsnprintf(buf, sizeof(buf), format, args);
	va_end(args);
	_screen.print(buf);
}

void GUI::draw_progressbar(int x, int y, int w, int h, int val, int val_min, int val_max) {
	val = constrain(val, val_min, val_max);
	int x1 = x + w - 1;
	int y1 = y + h - 1;

	// Clear area
	_screen.rect(x, y, x1, y1, OLED_CLEAR);

	if (h < 4) {
		// Draw as line with limiters. Like this:
		// #.#####..............#
		_screen.rect(x, y, x, y1, OLED_FILL);
		_screen.rect(x1, y, x1, y1, OLED_FILL);
		int bar_w = map(val, val_min, val_max, 0, w - 4);
		if (bar_w > 0) _screen.rect(x + 2, y, x + 1 + bar_w, y1, OLED_FILL);
	}
	else {
		// Draw as line inside border
		_screen.rect(x, y, x1, y1, OLED_STROKE);
		int bar_w = map(val, val_min, val_max, 0, w - 4);
		if (bar_w > 0) _screen.rect(x + 2, y + 2, x + 1 + bar_w, y1 - 2, OLED_FILL);
	}
}


void GUI::draw_loading(const char* msg) {
#ifdef SERIAL_DEBUG
	Serial.println(msg); // Debug
#endif
	_screen.clear();
	draw_text(0, 0, 2, "LOADING");
	draw_text(0, 24, 1, msg);
	_screen.update();
}

void GUI::draw_progress(int value) {
#ifdef SERIAL_DEBUG
	Serial.printf("Progress: %3d%%", value);
#endif
	draw_textf(9 * 6, 16, 1, 4, "%3d%%", value);
	draw_progressbar(0, 24, 128, 8, value, 0, 100);
	_screen.update();
}

void GUI::draw_time(tm* ptm) {
	if (ptm == nullptr) {
		draw_text(0, 0, 1, "--:--:--");
	}
	else {
		draw_textf(0, 0, 1, 8, "%02d:%02d:%02d", ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
	}
	_screen.update();
}

void GUI::draw_points(unsigned int points) {
	// logger.pointsSaved()
	draw_textf(52, 0, 1, 7, "%7d", points);
	_screen.update();
}

void GUI::draw_battery(unsigned int charge) {
	// battery.getPercentage()
	// Clear area
	_screen.rect(99, 1, 127, 7, OLED_CLEAR);
	// Draw icon
	_screen.rect(99, 1, 102, 6, OLED_STROKE);
	_screen.line(100, 0, 101, 0, OLED_FILL);
	// Draw text
	_screen.setCursorXY(104, 0);
	_screen.printf("%3d%%", charge);
	_screen.update();
}

void GUI::draw_isrunning_icon(bool flag) {
	// Clear area
	_screen.rect(54, 8, 71, 15, OLED_CLEAR);
	if (flag) {
		// Play icon
		_screen.line(61, 9, 61, 13, OLED_FILL);
		_screen.line(62, 10, 62, 12, OLED_FILL);
		_screen.dot(63, 11, OLED_FILL);
	}
	else {
		// Pause icon
		_screen.rect(60, 9, 61, 13, OLED_FILL);
		_screen.rect(63, 9, 64, 13, OLED_FILL);
	}
	_screen.update();
}

void GUI::draw_accuracy(unsigned int acc) {
	draw_textf(54, 16, 1, 3, "%3d", (acc > 999) ? 999 : acc);
	_screen.update();
}

void GUI::draw_flash_used(unsigned int percent) {
	// (logger.getUsagePercentage() >= 100) ? 99 : logger.getUsagePercentage()
	draw_textf(54, 24, 1, 3, "%2d%%", percent);
	_screen.update();
}

void GUI::draw_wifi_status(int8_t status) {
	if (status == WIFI_SCAN_RUNNING) {
		// Clear area
		_screen.rect(119, 9, 127, 15, OLED_CLEAR);
		// Reload icon
		_screen.circle(123, 12, 3, OLED_STROKE);
		_screen.line(120, 9, 126, 15, OLED_CLEAR);
		_screen.line(119, 12, 121, 12, OLED_FILL);
		_screen.line(125, 12, 127, 12, OLED_FILL);
	}
	else if (status == WIFI_SCAN_FAILED) {
		// Clear area
		_screen.rect(119, 9, 127, 15, OLED_CLEAR);
		// Pause icon
		_screen.rect(121, 9, 122, 15, OLED_FILL);
		_screen.rect(124, 9, 125, 15, OLED_FILL);
	}
	else {
		// Draw wi-fi count or do nothing
	}
}

void GUI::draw_graph_gps(int* values, bool* is_used, unsigned int count) {
	_screen.rect(0, 8, 47, 31, OLED_CLEAR);
	_draw_graph(0, 8, 23, 0, 99, 2, values, count > MAX_SAT_BARS ? MAX_SAT_BARS : count, is_used);
	_screen.update();
}

void GUI::draw_graph_wifi(int* values, bool* is_open, unsigned int count) {
	_screen.rect(78, 8, 127, 31, OLED_CLEAR);
	_draw_graph(78, 8, 23, -90, -40, 1, values, count > MAX_WIFI_BARS ? MAX_WIFI_BARS : count, is_open);
	_screen.update();
}
