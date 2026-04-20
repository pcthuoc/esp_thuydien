#include "webserver.h"
#include "sd_card.h"
#include "analog_reader.h"
#include "counter.h"
#include "rain_gauge.h"
#include "modbus_rtu.h"
#include "modbus_tcp.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD_MMC.h>

static AsyncWebServer server(80);
static bool running = false;
static WebConfigSavedCallback s_configSavedCb = nullptr;

void webserver_set_config_saved_callback(WebConfigSavedCallback cb) {
    s_configSavedCb = cb;
}

// ============================================================
// Helpers
// ============================================================

static String getContentType(const String& path) {
    if (path.endsWith(".html")) return "text/html";
    if (path.endsWith(".css"))  return "text/css";
    if (path.endsWith(".js"))   return "application/javascript";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".png"))  return "image/png";
    if (path.endsWith(".ico"))  return "image/x-icon";
    return "text/plain";
}

// ============================================================
// API: GET /api/status
// ============================================================
static void api_status(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["uptime"] = millis() / 1000;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["sd_total_mb"] = sd_total_bytes() / (1024 * 1024);
    doc["sd_used_mb"] = sd_used_bytes() / (1024 * 1024);
    doc["wifi_mode"] = "AP";
    doc["ip"] = WiFi.softAPIP().toString();

    // Đọc net_mode từ config để hiển thị trên Home
    String netJson = sd_read_file("/config/network.json");
    if (netJson.length() > 0) {
        JsonDocument netDoc;
        if (!deserializeJson(netDoc, netJson)) {
            doc["net_mode"] = netDoc["net_mode"] | "wifi";
        } else {
            doc["net_mode"] = "wifi";
        }
    } else {
        doc["net_mode"] = "wifi";
    }

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

// ============================================================
// API: GET /api/config/{module}
// ============================================================
static String extractModule(const String& url) {
    // /api/config/{module} -> module
    int lastSlash = url.lastIndexOf('/');
    if (lastSlash >= 0) return url.substring(lastSlash + 1);
    return "";
}

static void api_get_config(AsyncWebServerRequest* request) {
    String module = extractModule(request->url());
    String path = "/config/" + module + ".json";

    if (sd_exists(path.c_str())) {
        request->send(SD_MMC, path, "application/json");
    } else {
        request->send(200, "application/json", "{}");
    }
}

// ============================================================
// API: POST /api/config/{module}
// ============================================================
static void api_post_config(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
    // Accumulate body
    static String body;
    if (index == 0) body = "";
    body += String((char*)data).substring(0, len);

    if (index + len == total) {
        // Validate JSON
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);
        if (err) {
            request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
            body = "";
            return;
        }

        // Save to SD
        String module = extractModule(request->url());
        String path = "/config/" + module + ".json";

        sd_mkdir("/config");
        if (sd_write_file(path.c_str(), body.c_str())) {
            request->send(200, "application/json", "{\"ok\":true}");
            Serial.printf("[WEB] Config saved: %s\n", path.c_str());
            if (s_configSavedCb) s_configSavedCb(module.c_str());
        } else {
            request->send(500, "application/json", "{\"error\":\"SD write failed\"}");
        }
        body = "";
    }
}

// ============================================================
// API: GET /api/scan - Scan WiFi networks
// ============================================================
static void api_scan(AsyncWebServerRequest* request) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) {
        WiFi.scanNetworks(true); // async scan
        request->send(200, "application/json", "{\"status\":\"scanning\"}");
    } else if (n == WIFI_SCAN_RUNNING) {
        request->send(200, "application/json", "{\"status\":\"scanning\"}");
    } else {
        JsonDocument doc;
        doc["status"] = "done";
        JsonArray list = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n; i++) {
            JsonObject net = list.add<JsonObject>();
            net["ssid"] = WiFi.SSID(i);
            net["rssi"] = WiFi.RSSI(i);
            net["enc"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        }
        WiFi.scanDelete();
        String json;
        serializeJson(doc, json);
        request->send(200, "application/json", json);
    }
}

// ============================================================
// API: POST /api/restart
// ============================================================
static void api_restart(AsyncWebServerRequest* request) {
    request->send(200, "application/json", "{\"ok\":true,\"msg\":\"Restarting...\"}");
    delay(500);
    ESP.restart();
}

// ============================================================
// API: POST /api/clear-config - Xóa toàn bộ config trên SD
// ============================================================
static void api_clear_config(AsyncWebServerRequest* request) {
    const char* files[] = {
        "/config/network.json",
        "/config/analog.json",
        "/config/encoder.json",
        "/config/di.json",
        "/config/rs485.json",
        "/config/tcp.json",
        "/config/system.json",
        "/config/mode.json"
    };
    int count = 0;
    for (auto& f : files) {
        if (sd_exists(f) && sd_remove(f)) count++;
    }
    Serial.printf("[WEB] Cleared %d config files\n", count);

    String json = "{\"ok\":true,\"cleared\":" + String(count) + "}";
    request->send(200, "application/json", json);
}

// ============================================================
// API: GET /api/debug/raw?group=analog|encoder|di|rs485|tcp
// ============================================================
static void api_debug_raw(AsyncWebServerRequest* request) {
    String group = request->hasParam("group") ? request->getParam("group")->value() : "";
    JsonDocument doc;

    if (group == "analog") {
        if (analog_ads1_ok() || analog_ads2_ok()) {
            analog_poll();
            for (uint8_t i = 0; i < ANALOG_CHANNELS; i++) {
                char key[4]; snprintf(key, sizeof(key), "A%d", i + 1);
                const AnalogChannel* ch = analog_get_channel(i);
                if (ch && ch->valid) doc[key] = ch->raw_count;
                else doc[key] = (char*)nullptr;
            }
        }
    } else if (group == "encoder") {
        doc["E1"] = counter_get(0);
        doc["E2"] = counter_get(1);
    } else if (group == "di") {
        doc["DI1"] = rain_get_count();
    } else if (group == "rs485") {
        modbus_rtu_poll();
        uint8_t n = modbus_rtu_channel_count();
        for (uint8_t i = 0; i < n; i++) {
            const MbChannel* mch = modbus_rtu_get_channel(i);
            if (!mch) continue;
            if (mch->valid) doc[mch->name] = mch->value;
            else doc[mch->name] = (char*)nullptr;
        }
    } else if (group == "tcp") {
        modbus_tcp_poll();
        uint8_t n = modbus_tcp_channel_count();
        for (uint8_t i = 0; i < n; i++) {
            const TcpChannel* tch = modbus_tcp_get_channel(i);
            if (!tch) continue;
            if (tch->valid) doc[tch->name] = tch->value;
            else doc[tch->name] = (char*)nullptr;
        }
    }

    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
}

// ============================================================
// Setup routes
// ============================================================
static void setup_routes() {
    // --- API ---
    server.on("/api/status", HTTP_GET, api_status);
    server.on("/api/scan",   HTTP_GET, api_scan);
    server.on("/api/debug/raw", HTTP_GET, api_debug_raw);

    // Config GET/POST with path param
    server.on("/api/config/network",   HTTP_GET, api_get_config);
    server.on("/api/config/analog",    HTTP_GET, api_get_config);
    server.on("/api/config/encoder",   HTTP_GET, api_get_config);
    server.on("/api/config/di",        HTTP_GET, api_get_config);
    server.on("/api/config/rs485",     HTTP_GET, api_get_config);
    server.on("/api/config/tcp",       HTTP_GET, api_get_config);
    server.on("/api/config/system",    HTTP_GET, api_get_config);

    server.on("/api/config/network",   HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr, api_post_config);
    server.on("/api/config/analog",    HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr, api_post_config);
    server.on("/api/config/encoder",   HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr, api_post_config);
    server.on("/api/config/di",        HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr, api_post_config);
    server.on("/api/config/rs485",     HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr, api_post_config);
    server.on("/api/config/tcp",       HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr, api_post_config);
    server.on("/api/config/system",    HTTP_POST, [](AsyncWebServerRequest* r){}, nullptr, api_post_config);

    server.on("/api/restart", HTTP_POST, api_restart);
    server.on("/api/clear-config", HTTP_POST, api_clear_config);

    // --- Static files từ LittleFS (flash) với cache ---
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
        request->send(LittleFS, "/index.html", "text/html");
    });

    // Cache CSS/JS 1 giờ — trình duyệt không cần tải lại
    server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(LittleFS, "/style.css", "text/css");
        response->addHeader("Cache-Control", "max-age=3600");
        request->send(response);
    });
    server.on("/app.js", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(LittleFS, "/app.js", "application/javascript");
        response->addHeader("Cache-Control", "max-age=3600");
        request->send(response);
    });

    // Tất cả static files khác
    server.onNotFound([](AsyncWebServerRequest* request) {
        String path = request->url();
        if (LittleFS.exists(path)) {
            AsyncWebServerResponse* response = request->beginResponse(LittleFS, path, getContentType(path));
            response->addHeader("Cache-Control", "max-age=3600");
            request->send(response);
        } else {
            // SPA fallback: trả index.html cho mọi route không tìm thấy
            request->send(LittleFS, "/index.html", "text/html");
        }
    });
}

// ============================================================
// Public API
// ============================================================

void webserver_init(const char* ap_ssid, const char* ap_pass) {
    if (running) return;

    // Bật AP
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid, ap_pass);
    delay(100);

    // Mount LittleFS (web files trong flash)
    if (!LittleFS.begin(true)) {
        Serial.println("[WEB] LittleFS mount FAILED");
    }
    
    Serial.printf("[WEB] AP started: %s\n", ap_ssid);
    Serial.printf("[WEB] IP: %s\n", WiFi.softAPIP().toString().c_str());

    // Setup routes và start
    setup_routes();
    server.begin();
    running = true;

    Serial.println("[WEB] Server started on port 80");
}

void webserver_stop() {
    if (!running) return;
    server.end();
    WiFi.softAPdisconnect(true);
    running = false;
    Serial.println("[WEB] Server stopped");
}

bool webserver_is_running() {
    return running;
}
