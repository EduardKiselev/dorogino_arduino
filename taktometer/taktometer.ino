#include <WiFi.h>
#include <HTTPClient.h>

// --- Настройки Wi-Fi и серверов ---
const char* ssid = "ELTEX-8478";
const char* password = "eSm-kp7-VdF-PtA";

const int TAKTOMETER = 1;  

const char* servers[] = {"192.168.1.100", "192.168.1.101"};
const int serverCount = 2;
const int serverPort = 5000;

const float MIN_DIST = 25.0;  
const float MAX_DIST = 110.0; 

#define RADAR_RX_PIN 18
#define RADAR_TX_PIN 5


const unsigned long CHECK_INTERVAL = 1 * 1000; 
unsigned long lastCheck = 0;

// --- Состояния ---
enum State { EMPTY, NOT_EMPTY };
State currentState = EMPTY;
int taktCount = 0;

// --- UART и парсинг ---
#define BUFFER_SIZE 20
uint8_t uartBuffer[BUFFER_SIZE];
uint8_t bufIndex = 0;

volatile float lastDistanceCm = -1.0; 
String sessionPUID;           

void parseRadarData() {
  while (Serial1.available()) {
    uint8_t c = Serial1.read();
    
    if (bufIndex == 0) {
      if (c == 0xF4) uartBuffer[bufIndex++] = c;
    } 
    else if (bufIndex == 1) {
      if (c == 0xF3) uartBuffer[bufIndex++] = c;
      else bufIndex = (c == 0xF4) ? 1 : 0;
    } 
    else if (bufIndex == 2) {
      if (c == 0xF2) uartBuffer[bufIndex++] = c;
      else bufIndex = (c == 0xF4) ? 1 : 0;
    } 
    else if (bufIndex == 3) {
      if (c == 0xF1) uartBuffer[bufIndex++] = c;
      else bufIndex = (c == 0xF4) ? 1 : 0;
    } 
    else {
      if (bufIndex < BUFFER_SIZE) {
        uartBuffer[bufIndex++] = c;
      } else {
        bufIndex = 0; 
        continue;
      }
      
      if (bufIndex == 14) {
        if (uartBuffer[4] == 0x04 && uartBuffer[5] == 0x00 &&
            uartBuffer[10] == 0xF8 && uartBuffer[11] == 0xF7 && 
            uartBuffer[12] == 0xF6 && uartBuffer[13] == 0xF5) {
          
          uint32_t floatBits = uartBuffer[6] | (uartBuffer[7] << 8) | 
                               (uartBuffer[8] << 16) | (uartBuffer[9] << 24);
          float distanceMm;
          memcpy(&distanceMm, &floatBits, sizeof(float));
          
          lastDistanceCm = distanceMm / 10.0;

        }
        bufIndex = 0; 
      }
    }
  }
}

String generateSessionPUID() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08X", esp_random());
  return String(buf);
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  sessionPUID = generateSessionPUID(); // Сохраняем в глобальную переменную
  Serial.println("\nWiFi connected");
}

void loop() {
  // 1. Постоянно читаем UART, чтобы не терять данные
  parseRadarData(); 

  // 2. Проверяем таймер 5 минут
  if (millis() - lastCheck >= CHECK_INTERVAL) {

    lastCheck = millis();
    
    float dist = lastDistanceCm;
    Serial.print("Distance: "); Serial.println(dist);

    if (dist < 0) {
        Serial.println("No radar data yet.");
        return;
    }

    State newState = currentState;

    if (dist < MIN_DIST) {
      Serial.println("ERROR: Distance too low. State unchanged.");
      return; 
    } 
    else if (dist <= MAX_DIST) { // dist >= MIN_DIST уже проверено выше
      newState = NOT_EMPTY;
    } 
    else { 
      newState = EMPTY;
    }

    if (currentState == NOT_EMPTY && newState == EMPTY) {
      taktCount++;
      Serial.print("Takt counted! Total: "); Serial.println(taktCount);
      sendData();
    }

    currentState = newState;
  }
}

void sendData() {
  String json = "{"
                "\"puid\":\"" + sessionPUID + "|" + String(millis()) + "\","
                "\"sensor_id\":" + String(TAKTOMETER) +
                "}";

  for (int i = 0; i < serverCount; i++) {
    String url = "http://" + String(servers[i]) + ":" + String(serverPort) + "/takts";
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    
    int httpCode = http.POST(json);
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
      Serial.println("Data sent successfully to: " + String(servers[i]));
      http.end();
      return; 
    }
    
    Serial.println("Failed to send to " + String(servers[i]) + ", Code: " + String(httpCode));
    http.end();
  }
  Serial.println("ERROR: Failed to send data to all servers.");
}