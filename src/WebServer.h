#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include "Settings.h"
#include "Logger.h"

#define WS_TYPE_STATS 0x01
#define WS_TYPE_WIFI 0x02


class WebServer {
private:
	AsyncWebServer _server;
	AsyncWebSocket _ws;

	bool _apChanged = false;
	bool _busy = false;

	void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
		AwsEventType type, void* arg, uint8_t* data, size_t len) {
		if (type == WS_EVT_DATA) {
			data[len] = 0;
			String msg = (char*)data;

			//if (msg == "toggle") {
			//	digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
			//	_ws.textAll("Status: Changed");
			//}
		}
	}

public:
	WebServer(uint16_t port) : _server(port), _ws("/ws") {}

	~WebServer() {
		end();
	}

	void begin() {
		LittleFS.begin();

		// Подключаем обработчик событий WebSocket через лямбду
		_ws.onEvent([this](AsyncWebSocket* s, AsyncWebSocketClient* c, AwsEventType t,
			void* a, uint8_t* d, size_t l) {
				this->onWsEvent(s, c, t, a, d, l);
			});

		_server.addHandler(&_ws);

		// Раздача статики из SPIFFS/LittleFS
		_server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

		// Обработка 404 ошибки (добавляем этот блок)
		_server.onNotFound([](AsyncWebServerRequest* request) {
			if (LittleFS.exists("/404.html")) {
				request->send(LittleFS, "/404.html", "text/html", false);
			}
			else {
				request->send(404, "text/plain", "404: Not Found");
			}
			});

		_server.on("/config", HTTP_GET, [this](AsyncWebServerRequest* request) {
			String json = "{";
			json += "\"ssid\":\"" + String(cfg.ssid) + "\",";
			json += "\"pass\":\"" + String(cfg.pass) + "\",";
			json += "\"scan_interval\":" + String(cfg.scan_interval) + ",";
			json += "\"min_acc\":" + String(cfg.min_acc) + ",";
			json += "\"tz_offset\":" + String(cfg.tz_offset) + ",";
			json += "\"auto_tz\":" + String(cfg.auto_tz ? "true" : "false") + ",";
			json += "\"rotate_logs\":" + String(cfg.rotate_logs ? "true" : "false");
			json += "}";

			request->send(200, "application/json", json);
			});


		_server.on("/config", HTTP_POST, [this](AsyncWebServerRequest* request) {
			// AP SSID
			if (request->hasParam("ssid", true)) {
				String newSSID = request->getParam("ssid", true)->value();
				if (newSSID != String(cfg.ssid)) {
					strncpy(cfg.ssid, newSSID.c_str(), 31);
					_apChanged = true;
				}
			}

			// AP Password
			if (request->hasParam("pass", true)) {
				String newPass = request->getParam("pass", true)->value();
				if (newPass != String(cfg.pass)) {
					strncpy(cfg.pass, newPass.c_str(), 63);
					_apChanged = true;
				}
			}

			// Numeric fields (uint16_t, int16_t)
			if (request->hasParam("scan_interval", true)) {
				cfg.scan_interval = (uint16_t)request->getParam("scan_interval", true)->value().toInt();
			}

			if (request->hasParam("min_acc", true)) {
				cfg.min_acc = (int16_t)request->getParam("min_acc", true)->value().toInt();
			}

			if (request->hasParam("tz_offset", true)) {
				cfg.tz_offset = (int16_t)request->getParam("tz_offset", true)->value().toInt();
			}

			// Checkbox'es - just check if it preset in data
			cfg.auto_tz = request->hasParam("auto_tz", true);
			cfg.rotate_logs = request->hasParam("rotate_logs", true);

			// Save new config
			if (cfg.save()) {
				if (_apChanged) {
					// Применяем новые настройки точки доступа немедленно
					request->send(200, "text/plain", "RECONNECT");
				}
				else {
					request->send(200, "text/plain", "OK");
				}
			}
			else {
				request->send(200, "text/plain", "FAIL: Save error");
				_apChanged = false;
			}
			});

		_server.on("/download", HTTP_GET, [this](AsyncWebServerRequest* request) {
			WiFi.scanDelete(); // Останавливаем сканирование, чтобы не мешало
			_busy = true;

			// Используем умный указатель, чтобы не утекла память при обрыве связи
			auto requestedBlocks = std::make_shared<std::vector<uint32_t>>();

			int params = request->params();
			for (int i = 0; i < params; i++) {
				AsyncWebParameter* p = request->getParam(i);
				if (p->name() == "id") {
					uint32_t id = strtoul(p->value().c_str(), NULL, 10);
					if (id < logger.blocksTotal()) {
						requestedBlocks->push_back(id);
					}
				}
			}

			if (requestedBlocks->empty()) {
				request->send(400, "text/plain", "No valid blocks");
				return;
			}

			size_t totalSize = requestedBlocks->size() * DATA_BLOCK_SIZE;

			// Передаем размер вторым аргументом -> библиотека сама поставит Content-Length
			AsyncWebServerResponse* response = request->beginResponse("application/octet-stream", totalSize,
				[this, requestedBlocks](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {

					size_t totalRequestedSize = requestedBlocks->size() * DATA_BLOCK_SIZE;
					if (index >= totalRequestedSize) return 0;

					size_t currentBlockIdxInList = index / DATA_BLOCK_SIZE;
					size_t offsetInBlock = index % DATA_BLOCK_SIZE;
					uint32_t flashBlockId = (*requestedBlocks)[currentBlockIdxInList];

					size_t remainingInBlock = DATA_BLOCK_SIZE - offsetInBlock;
					// Читаем маленькую порцию (обычно ~1.4KB), которую просит maxLen
					size_t lenToRead = std::min(maxLen, remainingInBlock);

					// Блокирующее чтение маленького кусочка из SPI (безопасно для WDT)
					if (logger.getBlockPart(flashBlockId, offsetInBlock, buffer, lenToRead)) {
						return lenToRead;
					}

					return 0; // Ошибка чтения — прерываем передачу
				});

			response->addHeader("Content-Disposition", "attachment; filename=\"log_dump.bin\"");
			request->send(response);
			_busy = false;
			});


		_server.on("/GetUsedBlocks", HTTP_GET, [this](AsyncWebServerRequest* request) {
			_busy = true;
			AsyncResponseStream* response = request->beginResponseStream("text/plain");

			// Контекст для отслеживания первой записи (чтобы не ставить лишнюю запятую)
			auto first = std::make_shared<bool>(true);

			logger.getUsedBlockIDs([response, first](int id, uint32_t timestamp) {
				if (!(*first)) {
					response->print(",");
				}
				// Формат: ID=Timestamp
				response->printf("%u=%u", id, timestamp);
				*first = false;
				});

			request->send(response);
			_busy = false;
			});

		_server.on("/ScanWiFi", HTTP_POST, [this](AsyncWebServerRequest* request) {
			// Проверяем, не идет ли уже сканирование
			int8_t scanResult = WiFi.scanComplete();
			if (scanResult == WIFI_SCAN_RUNNING) {
				request->send(200, "text/plain", "-1");
			}
			else {
				// Запускаем новое асинхронное сканирование
				WiFi.scanNetworks(true, true);
				request->send(200, "text/plain", "OK");
			}
			});

		_server.on("/EraseFlash", HTTP_POST, [this](AsyncWebServerRequest* request) {
			logger.requestErase();
			request->send(200, "text/plain", "OK");
			});

		_server.begin();
	}

	void sendWSData(uint8_t* buf, uint32_t len) {
		_busy = true;
		_ws.binaryAll(buf, len);
		_busy = false;
	}

	void end() {
		_ws.closeAll();
		_server.end();
	}

	void update() {
		_ws.cleanupClients();
		if (_apChanged) {
			delay(1000); // wait until browser get answer
			WiFi.softAP(cfg.ssid, cfg.pass);
			_apChanged = false;
		}
	}

	bool isBusy() { return _busy; }
};
