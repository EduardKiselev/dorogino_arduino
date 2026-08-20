#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <esp_wifi.h>
#include <HTTPClient.h> 

// === НАСТРОЙКИ ===
#define CONTROLLER_ID 6  // ← Уникальный ID устройства (1-255)
const char* ssid = "ELTEX-8478";
const char* pass = "eSm-kp7-VdF-PtA";

const char* servers[] = {"192.168.1.100", "192.168.1.101"};
const int serverCount = 2;
const int serverPort = 5000;

// Добавить эти переменные глобально:
bool isReconnecting = false;
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectTimeout = 10000; // 10 секунд таймаут

WebServer server(80);
bool valveOpen = false;
uint32_t openedAt = 0;
const int VALVE_PIN = 25;
const uint32_t CLOSE_DELAY = 300000; // 300 сек


const unsigned long HEARTBEAT_INTERVAL = 60000;
unsigned long lastHeartbeat = 0;

void handleOpen() {
  Serial.println("[HTTP] GET /valve/open");
  digitalWrite(VALVE_PIN, HIGH);
  valveOpen = true;
  openedAt = millis();
  Serial.println("[DEBUG] Valve OPENED");
  server.send(200, "text/plain", "OK: opened");
}

void handleClose() {
  Serial.println("[HTTP] GET /valve/close");
  digitalWrite(VALVE_PIN, LOW);
  valveOpen = false;
  Serial.println("[DEBUG] Valve CLOSED (manual)");
  server.send(200, "text/plain", "OK: closed");
}

// === Генерация и установка кастомного MAC-адреса ===
// Локально-администрируемый адрес: 02:XX:XX:XX:XX:XX
void setCustomMAC(uint32_t id) {
  uint8_t mac[6];
  
  mac[0] = 0x02;                    // Locally administered, unicast
  mac[1] = 0xCA;                    // Фиксированный байт для ваших устройств
  mac[2] = 0xFE;                    // (можно менять для разных серий)
  mac[3] = 0x00;                    // Фиксированный байт
  mac[4] = 0x00;                    // Фиксированный байт
  mac[5] = id & 0xFF;               // Уникальная часть: номер контроллера
  
  esp_wifi_set_mac(WIFI_IF_STA, mac);
  
  Serial.printf("[MAC] Set: %02X:%02X:%02X:%02X:%02X:%02X\n", 
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// === Функция проверки и восстановления Wi-Fi ===
void checkWiFiConnection() {
  // Если подключены — выходим
  if (WiFi.status() == WL_CONNECTED) {
    isReconnecting = false;
    return;
  }

  // Если разрыв произошел и мы еще не начали переподключение
  if (!isReconnecting) {
    Serial.println("[WIFI] Connection lost! Attempting to reconnect...");
    WiFi.begin(ssid, pass);
    isReconnecting = true;
    lastReconnectAttempt = millis();
  }

  // Проверка таймаута попытки
  if (isReconnecting && millis() - lastReconnectAttempt > reconnectTimeout) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WIFI] Reconnect attempt timed out. Will retry later.");
      isReconnecting = false; // Сброс флага, чтобы попытаться снова в следующем цикле
    } else {
      Serial.println("[WIFI] Reconnected successfully!");
      Serial.print("[WIFI] New IP: ");
      Serial.println(WiFi.localIP());
      isReconnecting = false;
    }
  }
}
void sendHeartbeat() {
  String json = "{"
                "\"device_type\":\"controller\","
                "\"device_id\":" + String(CONTROLLER_ID) + ","
                "\"health\":{"
                  "\"rssi\":" + String(WiFi.RSSI()) + ","
                  "\"heap\":" + String(ESP.getFreeHeap()) + ","
                  "\"uptime\":" + String(millis()) +
                "}"
                "}";


  String url = "http://" + String(servers[0]) + ":" + String(serverPort) + "/api/heartbeat";
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int httpCode = http.POST(json);
  Serial.printf("[HB] Sent to %s | Code: %d\n", servers[0], httpCode);
  http.end();
  
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n[INIT] ESP32 booting...");

  pinMode(VALVE_PIN, OUTPUT);
  digitalWrite(VALVE_PIN, LOW);

  // === 1. Устанавливаем MAC ДО подключения ===
  WiFi.mode(WIFI_STA);              // Обязательно перед esp_wifi_set_mac
  setCustomMAC(CONTROLLER_ID + 200);

  // === 2. Задаём понятное имя хоста ===
  char hostname[32];
  snprintf(hostname, sizeof(hostname), "esp32-controller-%d", CONTROLLER_ID);
  WiFi.setHostname(hostname);
  Serial.printf("[HOST] Set: %s\n", hostname);

  // === 3. Подключаемся к Wi-Fi ===
  Serial.printf("[WIFI] Connecting to '%s'...", ssid);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[INFO] Hostname: %s, MAC: %s\n", 
                WiFi.getHostname(), WiFi.macAddress().c_str());

  // === HTTP сервер ===
  server.on("/valve/open", HTTP_GET, handleOpen);
  server.on("/valve/close", HTTP_GET, handleClose);
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain", "ESP32 Valve Controller\nEndpoints: /valve/open, /valve/close");
  });

  server.begin();
  Serial.println("[HTTP] Server listening on port 80");
}


void loop() {

  checkWiFiConnection();

  server.handleClient();
  
  // 3. Логика клапана
  if (valveOpen && millis() - openedAt >= CLOSE_DELAY) {
    digitalWrite(VALVE_PIN, LOW);
    valveOpen = false;
    Serial.println("[TIMER] Valve CLOSED (auto)");
  }

  // 4. Heartbeat (отправляем только если есть связь)  
    if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeat = millis();
  }
}