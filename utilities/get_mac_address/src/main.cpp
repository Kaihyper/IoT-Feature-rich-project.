/*
  Run this on EACH board once, note down the printed MAC address.
  You'll paste these addresses into the ESP-NOW peer config on the OTHER board.
*/
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(1000);
  WiFi.mode(WIFI_STA);
  Serial.print("This board's MAC address: ");
  Serial.println(WiFi.macAddress());
}

void loop() {}
