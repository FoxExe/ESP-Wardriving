#pragma once

#include <Arduino.h>

// R = 1мОм + 330кОм
// BATT_DIVIDER_K = "Вольт на батарее" / "Вольт на пине ADC-0"

#define TURNOFF_THRESHOLD 1  // turn off at this percent
#define BATT_DIVIDER_K 5.5
#define BATT_REF_V 1.0
#define ADC_MAX 1023.0
#define BATT_SMOOTH 0.02

class Battery {
private:
	float _filteredVoltage = -1.0;
	float _k = BATT_DIVIDER_K;
	int _currentPercentage = 0;

	struct DischargePoint { float v; int p; };
	static constexpr int TABLE_SIZE = 12;
	static constexpr DischargePoint _table[TABLE_SIZE] = {
		{4.20, 100},
		{4.10, 96},
		{4.00, 90},
		{3.90, 82},
		{3.80, 73},
		{3.70, 60},
		{3.60, 45},
		{3.50, 25},
		{3.40, 12},
		{3.30, 7},
		{3.20, 3},
		{3.00, 0}
	};
public:
	Battery() {}

	void calibrate(float actualVoltage) {
		int raw = analogRead(A0);
		if (raw > 0) {
			float vPin = (raw / ADC_MAX) * BATT_REF_V;
			_k = actualVoltage / vPin;
		}
	}

	void update() {
		yield(); // Fix possible Wi-Fi conflicts
		int raw = analogRead(A0);
		float instantVoltage = ((float)raw / ADC_MAX) * BATT_REF_V * _k;

		if (_filteredVoltage < 0) _filteredVoltage = instantVoltage;
		else _filteredVoltage = (_filteredVoltage * (1.0 - BATT_SMOOTH)) + (instantVoltage * BATT_SMOOTH);

		// Расчет процентов
		_currentPercentage = 0;
		if (_filteredVoltage >= _table[0].v) _currentPercentage = 100;
		else if (_filteredVoltage <= _table[TABLE_SIZE - 1].v) _currentPercentage = 0;
		else {
			for (int i = 0; i < TABLE_SIZE - 1; i++) {
				if (_filteredVoltage <= _table[i].v && _filteredVoltage > _table[i + 1].v) {
					_currentPercentage = _table[i + 1].p + (int)((_filteredVoltage - _table[i + 1].v) *
						(_table[i].p - _table[i + 1].p) / (_table[i].v - _table[i + 1].v));
					break;
				}
			}
		}
	}

	float getVoltage()    const { return _filteredVoltage; }
	int   getPercentage() const { return _currentPercentage; }
	bool  is_low() { return _currentPercentage <= TURNOFF_THRESHOLD; };
};

Battery battery;
