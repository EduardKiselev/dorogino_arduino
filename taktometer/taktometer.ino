// Совместимо с Arduino Uno R4 WiFi (Renesas RA4M1 + ESP32-S3)
// Библиотека WiFiS3 позволяет Renesas управлять ESP32

#include <WiFiS3.h> 

// ================= НАСТРОЙКИ ПОЛЬЗОВАТЕЛЯ =================
#define DEPTHMETER 1 
const char* ssid = "ELTEX-8478";
const char* password = "eSm-kp7-VdF-PtA";

const char* servers[] = {"192.168.1.100", "192.168.1.101"};
const int SERVER_COUNT = 2;
const int serverPort = 5000;
const char* endpoint = "/depth";
const int SAMPLES_PER_INTERVAL = 30;
const float PERCENTILE_FACTOR = 0.8f; 

// Пины Renesas (D2, D3 безопасны и не заняты)
const int trigPin = 4; 
const int echoPin = 5;

const unsigned long MEASURE_INTERVAL = 1000; 
const int MAX_READINGS = 6;

const float MIN_DISTANCE = 10;
const float MAX_DISTANCE = 2000.0;
// ==========================================================

unsigned long previousMillis = 0;
float readings[MAX_READINGS] = {0};
int readingIndex = 0;
int validReadingsCount = 0;
String sessionPUID = ""; 

// Генерация ID (Renesas не имеет esp_random)
String generateSessionPUID() {
  char buf[16];
  uint32_t seed = analogRead(A0) + micros();
  snprintf(buf, sizeof(buf), "%08X", seed);
  return String(buf);
}

void connectToWiFi() {
  Serial.print("Подключение к Wi-Fi");
  // WiFiS3 автоматически свяжется с ESP32-S3 на плате
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ Wi-Fi подключен!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

bool sendToServer(const char* host, String json) {
  WiFiClient client;
  Serial.print("  → Отправка на ");
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

void storeReading(float distance) {
  readings[readingIndex] = distance;
  readingIndex = (readingIndex + 1) % MAX_READINGS;
  if (validReadingsCount < MAX_READINGS) {
    validReadingsCount++;
  }
}

float calculateAverage() {
  float sum = 0;
  for (int i = 0; i < validReadingsCount; i++) {
    sum += readings[i];
  }
  return sum / validReadingsCount;
}

int readUltrasonicFiltered(float* outSamples, int maxSamples) {
  int validCount = 0;

  for (int i = 0; i < SAMPLES_PER_INTERVAL && validCount < maxSamples; i++) {
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);

    // pulseIn на Renecas работает стабильно
    long duration = pulseIn(echoPin, HIGH, 150000L); 
    
    if (duration > 600) { 
      outSamples[validCount++] = (duration * 0.0343) / 2.0;
    }
    
    delay(100); 
  }

  if (validCount == 0) return 0;

  // Сортировка пузырьком
  for (int i = 0; i < validCount - 1; i++) {
    for (int j = 0; j < validCount - i - 1; j++) {
      if (outSamples[j] > outSamples[j + 1]) {
        float temp = outSamples[j];
        outSamples[j] = outSamples[j + 1];
        outSamples[j + 1] = temp;
      }
    }
  }

    Serial.print("Отсортированные сэмплы: [");
  for (int i = 0; i < validCount; i++) {
    Serial.print(outSamples[i], 1); // 1 знак после запятой
    if (i < validCount - 1) Serial.print(", ");
  }
  Serial.println("]");

  return validCount;
}

void setup() {
  Serial.begin(115200);
  unsigned long timeout = millis() + 3000; 

  while (!Serial && millis() < timeout) { 
    delay(10); 
  }

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  //connectToWiFi();

  sessionPUID = generateSessionPUID();
  Serial.print("Session PUID: ");
  Serial.println(sessionPUID);
}

void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - previousMillis >= MEASURE_INTERVAL) {
    previousMillis = currentMillis;

    // if (WiFi.status() != WL_CONNECTED) {
    //   connectToWiFi();
    // }

    float samples[SAMPLES_PER_INTERVAL];
    int validCount = readUltrasonicFiltered(samples, SAMPLES_PER_INTERVAL);

    if (validCount == 0) {
      Serial.println("⚠ Нет валидных измерений, пропуск.");
      return;
    }

    int targetIndex = (int)(validCount * PERCENTILE_FACTOR);
    if (targetIndex >= validCount) targetIndex = validCount - 1;

    float distance = samples[targetIndex];
    Serial.print("Distance: "); Serial.println(distance);

    if (distance >= MIN_DISTANCE && distance <= MAX_DISTANCE) {
      storeReading(distance);
      float average = calculateAverage();
      
      Serial.print("Avg: "); Serial.println(average);

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
    //    sendToServer(servers[i], json); 
        delay(100);
      }
    }
  }
}