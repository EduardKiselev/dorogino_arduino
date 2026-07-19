#include <WiFi.h>
#include <WiFiClient.h>

// ================= НАСТРОЙКИ ПОЛЬЗОВАТЕЛЯ =================
#define DEPTHMETER 1 
const char* ssid = "ELTEX-8478";
const char* password = "eSm-kp7-VdF-PtA";

const char* servers[] = {"192.168.1.100", "192.168.1.101"};
const int SERVER_COUNT = 2;
const int serverPort = 5000;
const char* endpoint = "/depth";
const int SAMPLES_PER_INTERVAL = 40;
const float PERCENTILE_FACTOR = 0.8f; 

// Пины (GPIO4 и GPIO5 стабильно работают на большинстве плат ESP32)
const int trigPin = 4; 
const int echoPin = 5;

const unsigned long MEASURE_INTERVAL = 10000; 

const float MIN_DISTANCE = 10;
const float MAX_DISTANCE = 2000.0;

// Настройки фильтрации по истории
const int HISTORY_SIZE = 20;
const float DEVIATION_THRESHOLD = 5.0; // Допустимое отклонение от медианы (см)
// ==========================================================

// Таймер для heartbeat
const unsigned long HEARTBEAT_INTERVAL = 60000; // раз в 60 сек
unsigned long lastHeartbeat = 0;

unsigned long previousMillis = 0;

// Буфер истории измерений для медианного фильтра
float depthHistory[HISTORY_SIZE] = {0};
int historyIndex = 0;
int historyCount = 0;

String sessionPUID = ""; 

// Генерация ID (ESP32 имеет аппаратный ГСЧ)
String generateSessionPUID() {
  char buf[16];
  snprintf(buf, sizeof(buf), "%08X", (unsigned int)esp_random());
  return String(buf);
}

void connectToWiFi() {
  Serial.print("Подключение к Wi-Fi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n Wi-Fi подключен!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

bool sendToServer(const char* host, String json) {
  WiFiClient client;
  Serial.print("  -> Отправка на ");
  Serial.print(host);
  
  if (!client.connect(host, serverPort)) {
    Serial.println(" [CONNECT FAIL]");
    return false;
  }

  client.print("POST ");
  client.print(endpoint);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.print(host);
  client.print(":");
  client.println(serverPort);
  client.println("Content-Type: application/json");
  client.print("Content-Length: ");
  client.println(json.length());
  client.println("Connection: close");
  client.println();
  client.println(json);

  delay(100);
  
  bool success = false;
  unsigned long timeout = millis() + 2000;
  while (client.connected() || client.available()) {
    if (client.available()) {
       String line = client.readStringUntil('\n');
       if (line.startsWith("HTTP/1.1")) {
         int code = line.substring(9, 12).toInt();
         success = (code == 200 || code == 201);
         break;
       }
    }
    if (millis() > timeout) break;
  }
  
  client.stop();
  Serial.println(success ? " [OK]" : " [ERROR]");
  return success;
}

// Вычисление медианы из текущего буфера
float getMedianFromHistory() {
  float sorted[HISTORY_SIZE];
  // Копируем только валидные элементы
  for(int i = 0; i < historyCount; i++) {
    sorted[i] = depthHistory[i];
  }
  
  // Сортировка пузырьком (для N=20 очень быстрая)
  for (int i = 0; i < historyCount - 1; i++) {
    for (int j = 0; j < historyCount - i - 1; j++) {
      if (sorted[j] > sorted[j + 1]) {
        float temp = sorted[j];
        sorted[j] = sorted[j + 1];
        sorted[j + 1] = temp;
      }
    }
  }
  
  if (historyCount % 2 == 0) {
    return (sorted[historyCount / 2 - 1] + sorted[historyCount / 2]) / 2.0;
  } else {
    return sorted[historyCount / 2];
  }
}

int readUltrasonicFiltered(float* outSamples, int maxSamples) {
  int validCount = 0;

  for (int i = 0; i < SAMPLES_PER_INTERVAL && validCount < maxSamples; i++) {
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);

    long duration = pulseIn(echoPin, HIGH, 150000L); 
    
    if (duration > 600) { 
      outSamples[validCount++] = (duration * 0.0343) / 2.0;
    }
    
    delay(100); 
  }

  if (validCount == 0) return 0;

    Serial.print(" сэмплы: [");
  for (int i = 0; i < validCount; i++) {
    Serial.print(outSamples[i], 1); 
    if (i < validCount - 1) Serial.print(", ");
  }
  Serial.println("]");

  // Сортировка пузырьком для отсечения шума
  for (int i = 0; i < validCount - 1; i++) {
    for (int j = 0; j < validCount - i - 1; j++) {
      if (outSamples[j] > outSamples[j + 1]) {
        float temp = outSamples[j];
        outSamples[j] = outSamples[j + 1];
        outSamples[j + 1] = temp;
      }
    }
  }

  return validCount;
}

void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return;

  // ESP32 поддерживает ESP.getFreeHeap(), используем вместо заглушки
  int heap = ESP.getFreeHeap();

  String json = "{"
                "\"device_type\":\"depth_meter\","
                "\"device_id\":" + String(DEPTHMETER) + ","
                "\"health\":{"
                  "\"rssi\":" + String(WiFi.RSSI()) + ","
                  "\"heap\":" + String(heap) + ","
                  "\"uptime\":" + String(millis()) +
                "}"
                "}";

  const char* hbHost = servers[0];
  WiFiClient client;
  if (client.connect(hbHost, serverPort)) {
    client.print("POST /api/heartbeat HTTP/1.1\r\n");
    client.print("Host: " + String(hbHost) + "\r\n");
    client.print("Content-Type: application/json\r\n");
    client.print("Content-Length: " + String(json.length()) + "\r\n");
    client.print("Connection: close\r\n\r\n");
    client.print(json);
    
    Serial.print("[HB] Sent -> ");
    Serial.print(hbHost);
    Serial.print(" | RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.print(" | Heap: ");
    Serial.println(heap);
    client.stop();
  } else {
    Serial.println("[HB] Connection failed");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500); // Стабильный старт USB-UART на ESP32

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  connectToWiFi();

  sessionPUID = generateSessionPUID();
  Serial.print("Session PUID: ");
  Serial.println(sessionPUID);
}

void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    sendHeartbeat();
    lastHeartbeat = currentMillis;
  }

  if (currentMillis - previousMillis >= MEASURE_INTERVAL) {
    previousMillis = currentMillis;

    if (WiFi.status() != WL_CONNECTED) {
      connectToWiFi();
    }

    float samples[SAMPLES_PER_INTERVAL];
    int validCount = readUltrasonicFiltered(samples, SAMPLES_PER_INTERVAL);

    if (validCount == 0) {
      Serial.println("⚠ Нет валидных измерений, пропуск.");
      return;
    }

    int targetIndex = (int)(validCount * PERCENTILE_FACTOR);
    if (targetIndex >= validCount) targetIndex = validCount - 1;

    // Усредняем верхние значения (фильтрация аппаратного шума)
    float tailSum = 0.0;
    int count = validCount - targetIndex;
    for (int i = targetIndex; i < validCount; i++) {
      tailSum += samples[i];
    }
    
    float distance = (count > 0) ? (tailSum / count) : samples[targetIndex];

    Serial.print("Distance: "); Serial.println(distance);

    if (distance >= MIN_DISTANCE && distance <= MAX_DISTANCE) {
      
      // Фильтрация по истории
      bool shouldSend = true;

      if (historyCount >= HISTORY_SIZE) {
        float median = getMedianFromHistory();
        float diff = distance - median;
        if (diff < 0) diff = -diff; 

        if (diff > DEVIATION_THRESHOLD) {
          shouldSend = false;
          Serial.print("⚠ Отклонение: "); Serial.print(diff, 1); 
          Serial.print(" см. Пропуск отправки. Медиана: "); Serial.println(median, 1);
        }
      } else {
        Serial.println("⚡ Массив истории заполняется, отправка без фильтра.");
      }

      // Всегда сохраняем замер в историю
      depthHistory[historyIndex] = distance;
      historyIndex = (historyIndex + 1) % HISTORY_SIZE;
      if (historyCount < HISTORY_SIZE) {
        historyCount++;
      }

      if (shouldSend) {
        String samplesJson = "[";
        for (int i = 0; i < validCount; i++) {
          samplesJson += String(samples[i], 1);
          if (i < validCount - 1) samplesJson += ",";
        }
        samplesJson += "]";

        String json = "{\"puid\":\"" + sessionPUID + "|" + String(millis()) + "\","
                      "\"sensor_id\":" + String(DEPTHMETER) + ","
                      "\"depth\":" + String(distance, 1) + ","
                      "\"samples\":" + samplesJson + "}";

        for (int i = 0; i < SERVER_COUNT; i++) {
          sendToServer(servers[i], json); 
          delay(100);
        }
      }
    }
  }
}