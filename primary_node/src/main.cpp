/*
  ESP32-S3 Primary Node — Safety Hub
  ------------------------------------------------------
  Role: always-on fire/gas safety monitor + multi-room anomaly detection.
  Sensors: DHT22, MQ-2 (via voltage divider), flame sensor, PIR — all local.
  Plus: room 2 data (temp/humidity/light) received over ESP-NOW from the
  secondary node, used as extra context for the anomaly check.
  Actuators: passive buzzer, OLED.

  Design: three FreeRTOS tasks, so a slow Blynk/network call can NEVER
  delay the hard-limit safety response.
    - safetyTask   (core 1, HIGH priority)   -> flame/gas hard-limit check + buzzer
    - mlTask       (core 0, MEDIUM priority) -> TinyML anomaly check + OLED
    - blynkTask    (core 0, LOW priority)    -> Blynk.run() + batched/alert uploads

  TinyML: runAnomalyCheck() below is a PLACEHOLDER. Replace its contents
  with the actual Edge Impulse–exported inference call once the model is
  trained (see the "TODO: TinyML" comment).

  Libraries required:
    - "DHT sensor library" by Adafruit (+ "Adafruit Unified Sensor")
    - "Adafruit SSD1306" + "Adafruit GFX"
    - "Blynk" (Blynk IoT library, v1.x)
    - esp_now.h — built into the ESP32 core, no separate install

  Fill in WiFi/Blynk credentials before flashing.
*/

#define BLYNK_TEMPLATE_ID   "YOUR_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "YOUR_TEMPLATE_NAME"
#define BLYNK_AUTH_TOKEN    "YOUR_AUTH_TOKEN"

#include <WiFi.h>
#include <esp_wifi.h>
#include <BlynkSimpleEsp32.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_now.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

// ---------------- Pin assignments ----------------
#define DHTPIN        4
#define DHTTYPE       DHT22
#define MQ2_PIN       5      // ADC1 - goes through the 10k/20k voltage divider
#define FLAME_PIN     14     // interrupt-driven, digital
#define PIR_PIN       6      // interrupt-driven, digital
#define BUZZER_PIN    15
#define OLED_SDA      8
#define OLED_SCL      9
#define OLED_WIDTH    128
#define OLED_HEIGHT   64

// ---------------- Thresholds ----------------
#define GAS_HARD_LIMIT_RAW   2200   // raw ADC value at GPIO5 - calibrate after MQ-2 burn-in
#define ANOMALY_THRESHOLD    0.7f   // placeholder scale 0.0-1.0, tune once model is trained
#define ROOM2_STALE_MS       (6UL * 60UL * 1000UL)  // flag room 2 as offline past this

// ---------------- WiFi credentials ----------------
char ssid[] = "YOUR_WIFI_SSID";
char pass[] = "YOUR_WIFI_PASSWORD";

DHT dht(DHTPIN, DHTTYPE);
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

// ---------------- Shared sensor state (protected by mutex) ----------------
typedef struct {
  float temperature;
  float humidity;
  int   gasRaw;
  volatile bool flameFlag;
  volatile unsigned long lastMotionMs;

  // room 2, via ESP-NOW
  float room2Temp;
  float room2Humidity;
  float room2LightPct;
  unsigned long room2LastSeen;

  bool  alarmActive;
  float anomalyScore;
} SharedData;

SharedData shared = {};
SemaphoreHandle_t sharedMutex;

// Queue used to hand "please send this now" alert events to the Blynk task,
// so the safety task never blocks on network calls itself.
typedef struct {
  uint8_t type; // 0 = hard-limit alarm, 1 = anomaly alert, 2 = room2 offline
} AlertEvent;
QueueHandle_t alertQueue;

// ---------------- ESP-NOW: matches the secondary node's struct exactly ----------------
typedef struct {
  float temperature;
  float humidity;
  float lightPct;
  uint8_t wakeCause;
  uint32_t seq;
} SensorMessage;

void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(SensorMessage)) return;
  SensorMessage incoming;
  memcpy(&incoming, data, sizeof(incoming));

  if (xSemaphoreTake(sharedMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    shared.room2Temp = incoming.temperature;
    shared.room2Humidity = incoming.humidity;
    shared.room2LightPct = incoming.lightPct;
    shared.room2LastSeen = millis();
    xSemaphoreGive(sharedMutex);
  }
}

// ---------------- Interrupts: flame + PIR ----------------
void IRAM_ATTR onFlameChange() {
  shared.flameFlag = digitalRead(FLAME_PIN) == LOW; // most flame modules pull LOW when flame detected - verify against your module
}
void IRAM_ATTR onMotionChange() {
  shared.lastMotionMs = millis();
}

// ---------------- Buzzer ----------------
#define BUZZER_CHANNEL 0
void buzzerOn()  { ledcWriteTone(BUZZER_CHANNEL, 2000); }
void buzzerOff() { ledcWriteTone(BUZZER_CHANNEL, 0); }

// ---------------- TinyML placeholder ----------------
// TODO: TinyML — replace this with the real Edge Impulse inference call once
// the anomaly-detection model is trained. For now this is a rough stand-in
// heuristic so the rest of the pipeline (OLED, Blynk, alerting) can be built
// and tested before the model exists.
float runAnomalyCheck(float temp, float humidity, int gasRaw, bool recentMotion, float room2Temp) {
  float score = 0.0f;
  if (gasRaw > GAS_HARD_LIMIT_RAW * 0.6f) score += 0.4f;      // gas trending up, not yet critical
  if (temp > 30.0f && !recentMotion) score += 0.3f;           // heat with nobody around
  if (!isnan(room2Temp) && room2Temp > 30.0f) score += 0.2f;  // both rooms warm simultaneously
  if (score > 1.0f) score = 1.0f;
  return score;
}

// ---------------- Task 1: Safety monitor (core 1, HIGH priority) ----------------
void safetyTask(void *param) {
  pinMode(FLAME_PIN, INPUT);
  pinMode(PIR_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(FLAME_PIN), onFlameChange, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIR_PIN), onMotionChange, RISING);

  ledcSetup(BUZZER_CHANNEL, 2000, 8);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);

  bool lastAlarmState = false;

  for (;;) {
    int gasRaw = analogRead(MQ2_PIN);

    bool hardLimitTriggered;
    if (xSemaphoreTake(sharedMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      shared.gasRaw = gasRaw;
      hardLimitTriggered = shared.flameFlag || (gasRaw > GAS_HARD_LIMIT_RAW);
      shared.alarmActive = hardLimitTriggered;
      xSemaphoreGive(sharedMutex);
    } else {
      hardLimitTriggered = false; // couldn't get the mutex this cycle, skip rather than block
    }

    if (hardLimitTriggered) {
      buzzerOn();
      if (!lastAlarmState) {
        AlertEvent evt = {0};
        xQueueSend(alertQueue, &evt, 0); // non-blocking - never stall the safety loop
      }
    } else {
      buzzerOff();
    }
    lastAlarmState = hardLimitTriggered;

    vTaskDelay(pdMS_TO_TICKS(300)); // fast poll - this is the safety-critical path
  }
}

// ---------------- Task 2: TinyML + OLED (core 0, MEDIUM priority) ----------------
void mlTask(void *param) {
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();

  for (;;) {
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();

    float score;
    bool alarmActive, room2Stale;
    float room2Temp, room2Humidity, room2LightPct;
    unsigned long room2LastSeen;
    unsigned long motionMs;

    if (xSemaphoreTake(sharedMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      shared.temperature = temperature;
      shared.humidity = humidity;
      motionMs = shared.lastMotionMs;
      bool recentMotion = (millis() - motionMs) < 60000; // motion in the last minute

      score = runAnomalyCheck(temperature, humidity, shared.gasRaw, recentMotion, shared.room2Temp);
      shared.anomalyScore = score;
      alarmActive = shared.alarmActive;

      room2Temp = shared.room2Temp;
      room2Humidity = shared.room2Humidity;
      room2LightPct = shared.room2LightPct;
      room2LastSeen = shared.room2LastSeen;
      room2Stale = (room2LastSeen == 0) || (millis() - room2LastSeen > ROOM2_STALE_MS);
      xSemaphoreGive(sharedMutex);
    } else {
      continue; // skip this cycle if we can't get the mutex promptly
    }

    if (!alarmActive && score >= ANOMALY_THRESHOLD) {
      AlertEvent evt = {1};
      xQueueSend(alertQueue, &evt, 0);
    }

    // --- Update OLED ---
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    if (alarmActive) {
      display.setTextSize(2);
      display.println("ALARM!");
    } else {
      display.printf("T:%.1fC H:%.1f%%\n", temperature, humidity);
      display.printf("Anomaly: %.0f%%\n", score * 100);
      if (room2Stale) {
        display.println("Room2: OFFLINE");
      } else {
        display.printf("Rm2 T:%.1fC L:%.0f%%\n", room2Temp, room2LightPct);
      }
    }
    display.display();

    vTaskDelay(pdMS_TO_TICKS(2000)); // ML + display refresh doesn't need fast polling
  }
}

// ---------------- Task 3: Blynk comms (core 0, LOW priority) ----------------
void blynkTask(void *param) {
  Blynk.config(BLYNK_AUTH_TOKEN);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (WiFi.status() == WL_CONNECTED) {
    Blynk.connect(10000);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM); // modem sleep once connected - stays responsive
  }

  unsigned long lastBatchSend = 0;

  for (;;) {
    if (Blynk.connected()) Blynk.run();

    // Handle any queued alert events immediately, regardless of batch timing
    AlertEvent evt;
    while (xQueueReceive(alertQueue, &evt, 0) == pdTRUE) {
      if (Blynk.connected()) {
        if (evt.type == 0) {
          Blynk.virtualWrite(V6, 1); // hard-limit alarm
          Blynk.logEvent("hard_limit_alarm", "Flame or gas threshold triggered!");
        } else if (evt.type == 1) {
          Blynk.virtualWrite(V6, 1);
          Blynk.logEvent("anomaly_alert", "TinyML anomaly score exceeded threshold");
        } else if (evt.type == 2) {
          Blynk.logEvent("room2_offline", "No update from room 2 in over 6 minutes");
        }
      }
    }

    // Batched routine update, roughly every 20s
    if (millis() - lastBatchSend > 20000 && Blynk.connected()) {
      if (xSemaphoreTake(sharedMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        Blynk.virtualWrite(V0, shared.temperature);
        Blynk.virtualWrite(V1, shared.humidity);
        Blynk.virtualWrite(V2, shared.gasRaw);
        Blynk.virtualWrite(V4, shared.flameFlag ? 1 : 0);
        Blynk.virtualWrite(V5, shared.anomalyScore);
        Blynk.virtualWrite(V6, shared.alarmActive ? 1 : 0);

        bool room2Stale = (shared.room2LastSeen == 0) ||
                           (millis() - shared.room2LastSeen > ROOM2_STALE_MS);
        if (!room2Stale) {
          Blynk.virtualWrite(V9, shared.room2Temp);
          Blynk.virtualWrite(V10, shared.room2Humidity);
          Blynk.virtualWrite(V11, shared.room2LightPct);
        }
        Blynk.virtualWrite(V12, room2Stale ? 0 : 1);
        xSemaphoreGive(sharedMutex);
      }
      lastBatchSend = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  sharedMutex = xSemaphoreCreateMutex();
  alertQueue = xQueueCreate(10, sizeof(AlertEvent));

  dht.begin();
  Wire.begin(OLED_SDA, OLED_SCL);

  WiFi.mode(WIFI_STA); // needed before esp_now_init
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.println("ESP-NOW receiver ready.");
  } else {
    Serial.println("ESP-NOW init failed - room 2 data won't be available.");
  }

  xTaskCreatePinnedToCore(safetyTask, "SafetyTask", 4096, NULL, 3, NULL, 1); // core 1, high priority
  xTaskCreatePinnedToCore(mlTask,     "MLTask",     8192, NULL, 2, NULL, 0); // core 0, medium priority
  xTaskCreatePinnedToCore(blynkTask,  "BlynkTask",  8192, NULL, 1, NULL, 0); // core 0, low priority
}

void loop() {
  // Everything runs in the three tasks above - nothing needed here.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
