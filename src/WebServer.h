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
			// 1. Собираем список запрошенных ID блоков из параметров
			std::vector<uint32_t> requestedBlocks;
			int params = request->params();
			for (int i = 0; i < params; i++) {
				AsyncWebParameter* p = request->getParam(i);
				if (p->name() == "id") {
					uint32_t id = atoi(p->value().c_str());
					if (id < logger.blocksTotal()) {
						requestedBlocks.push_back(id);
					}
				}
			}

			if (requestedBlocks.empty()) {
				request->send(400, "text/plain", "No valid block IDs provided");
				return;
			}

			// 2. Создаем потоковый ответ (Stream Response)
			// Тип данных application/octet-stream заставит браузер начать загрузку файла
			AsyncWebServerResponse* response = request->beginChunkedResponse("application/octet-stream",
				[requestedBlocks](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {

					// index - это сколько байт МЫ УЖЕ ПЕРЕДАЛИ в этом ответе
					size_t totalRequestedSize = requestedBlocks.size() * DATA_BLOCK_SIZE;

					if (index >= totalRequestedSize) {
						return 0; // Все данные переданы, закрываем соединение
					}

					// Вычисляем, какой блок и какое смещение внутри него сейчас нужно прочитать
					size_t currentBlockIdxInList = index / DATA_BLOCK_SIZE;
					size_t offsetInBlock = index % DATA_BLOCK_SIZE;
					uint32_t flashBlockId = requestedBlocks[currentBlockIdxInList];

					// Сколько байт мы можем прочитать за раз?
					// Не больше, чем просит сервер (maxLen) и не больше, чем осталось в текущем блоке
					size_t remainingInBlock = DATA_BLOCK_SIZE - offsetInBlock;
					size_t lenToRead = std::min(maxLen, remainingInBlock);

					// Читаем напрямую из логгера в буфер ответа
					if (logger.getBlockPart(flashBlockId, offsetInBlock, buffer, lenToRead)) {
						return lenToRead;
					}

					return 0; // Ошибка чтения
				});

			response->addHeader("Content-Disposition", "attachment; filename=\"log_dump.bin\"");
			request->send(response);
			});

		_server.on("/GetUsedBlocks", HTTP_GET, [this](AsyncWebServerRequest* request) {
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
		_ws.binaryAll(buf, len);
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
};
