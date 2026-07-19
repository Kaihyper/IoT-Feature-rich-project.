/*
  Minimal DHT22 test — no WiFi, no Blynk, no deep sleep.
  Just confirms the sensor itself is wired and reading correctly.
  Upload this alone, open Serial Monitor at 115200 baud.
*/

#include <DHT.h>

#define DHTPIN  4
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("DHT22 test starting...");
  dht.begin();
  delay(2000); // let the sensor stabilize before the first read
}

void loop() {
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("Read failed — check wiring, pull-up resistor, and pin number.");
  } else {
    Serial.printf("Temp: %.1f C | Humidity: %.1f%%\n", temperature, humidity);
  }

  delay(2000); // DHT22 needs at least ~2s between reads
}
