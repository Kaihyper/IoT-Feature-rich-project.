/**
 * @file secondary_node.cpp
 * @brief Remote environmental sensor node (Circuit-2).
 *
 * This module acts as a low-power secondary node in the mesh network.
 * It periodically samples ambient light, temperature, and humidity, then
 * broadcasts the data via ESP-NOW to the primary controller (Circuit-1)
 * and syncs to the Blynk cloud.
 *
 * Hardware Dependencies:
 * - ESP32 DevKit V1
 * - MH-series light sensor (Ambient Light)
 * - DHT22 (Temp/Humidity)
 *
 * @note Implements Deep-Sleep (5min interval) for battery longevity.
 * 
 * @par Wake Sources:
 * - RTC Timer: Periodic routine check-in (5-minute interval).
 * - GPIO 27: External interrupt via manual push-button.
 * 
 * @author Rahath Rahul
 * @date 2026-07-19
 * @version 1.2.0
 */
 
// -Blynk configuration-
#define BLYNK_TEMPLATE_ID "TMPL6_LgQcv94"
#define BLYNK_TEMPLATE_NAME "IoT project TinyML FRP"
#define BLYNK_AUTH_TOKEN "Mr1oMUtX10SXKstj35rRzzmHByiI_BAF"

// -Libraries-
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <DHT.h>
#include <esp_now.h>

// -Define pins-
#define DHTPIN  4                   // DHT11 data/signal pin
#define DHTTYPE DHT11
#define LIGHTPIN 35                 // MH light sensor A0 (ADC1, input-only pin)
#define WAKE_BUTTON_PIN GPIO_NUM_27 // button to GND, internal pull-up, RTC-capable

DHT dht(DHTPIN, DHTTYPE);

// - Function declaration-
void startConnection();
uint8_t getWakeReason();
void onEspNowSent(const uint8_t *mac, esp_now_send_status_t status);
bool setupEspNow();
void goToSleep();

// -Intervals- 

// How long the ESP32 sleeps before the next data collection
#define SLEEP_INTERVAL (5ULL * 60ULL * 1000000ULL)   // 5 minutes 

// Give up interval
#define WIFI_CONNECT_TIMEOUT_MS 10000                // give up on WiFi/Blynk after 10s

// -WiFi credentials-
char ssid[] = "Home@3375-2.4G";
char pass[] = "Rahul@7867";

// -ESP-NOW: primary board's MAC address-
uint8_t primaryMacAddress[] = {0x3C,0xDC,0x75,0x5C,0x8C,0x4C}; // ESP32-S3's Mac address

// -Data transfer-

// Message shared via ESP-NOW is in current format
typedef struct 
{
  float temperature;
  float humidity;
  float lightPct;
  uint8_t wakeCause;   // 0 = timer, 1 = button, 2 = other / miscellaneous
  uint32_t seq;
} SensorMessage;

SensorMessage outgoingMsg;

// Ensure variable survives deep sleep (RTC memory isn't wiped on deep-sleep wake, unlike normal RAM)
RTC_DATA_ATTR uint32_t bootCount = 0;

void setup()
{
 // Runs again after every sleep cycle (5mins)
 Serial.begin(115200);

 uint8_t wakeCause = getWakeReason();   // Get the wake up reason

 bootCount++ ; // Number of times the system has booted up

 dht.begin();
 delay(2000); // DHT22 needs a moment to stabilize on its first read after a cold boot
 
 // Starts Wifi and Blynk connection
 startConnection(); 

 // ==============Read sensors==============
  float temperature  = dht.readTemperature();
  float humidity     = dht.readHumidity();
  int   rawLight     = analogRead(LIGHTPIN);
  float lightPct     = (rawLight / 4095.0) * 100.0;

  if (isnan(temperature) || isnan(humidity)) 
  {
    Serial.println("DHT22 read failed — check wiring/pull-up resistor.");
  } 
  else 
  {
    Serial.printf("Temp: %.1f C | Humidity: %.1f%% | Light: %.1f%%\n",temperature, humidity, lightPct);
  }

  // Check if Blynk is connected
  if (Blynk.connected()) 
  {
    if (!isnan(temperature)) Blynk.virtualWrite(V0, temperature);
    if (!isnan(humidity)) Blynk.virtualWrite(V1, humidity);

    Blynk.virtualWrite(V8, lightPct);

    Serial.println("Sensor data sent to Blynk.");
  } 
  else 
  {
    Serial.println("Blynk not connected — skipping upload this cycle.");
  }

  // -Send the sensor reading directly to the primary board over ESP-NOW-
  if (setupEspNow()) 
  {
    outgoingMsg.temperature  = temperature;
    outgoingMsg.humidity     = humidity;
    outgoingMsg.lightPct     = lightPct;
    outgoingMsg.wakeCause    = wakeCause;
    outgoingMsg.seq          = bootCount;

    esp_err_t result = esp_now_send(primaryMacAddress, (uint8_t *) &outgoingMsg, sizeof(outgoingMsg));
    if (result != ESP_OK) 
    {
      Serial.println("ESP-NOW: send call failed to queue.");
    }
    delay(200); // brief pause so the send callback has time to fire before sleep
  }

  goToSleep();
}

void loop()
{
 // No code in main loop
}


void startConnection()
{
  /*
    This function connects to WiFi + Blynk but doesnt block the system forever and skips if it takes more than 200ms
  
    Args:
		None
		
	Returns:
		Void

  */

  Blynk.config(BLYNK_AUTH_TOKEN); // Configure blynk

  // Attempt to connect
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  unsigned long connectStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - connectStart < WIFI_CONNECT_TIMEOUT_MS) 
  {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) 
  {
    Blynk.connect(WIFI_CONNECT_TIMEOUT_MS);
  } 
  else 
  {
    Serial.println("WiFi connection failed this cycle — will read sensors and log locally only.");
  }
}


uint8_t getWakeReason() 
{
    /*
    This function returns a simple numeric wake cause (0=timer, 1=button, 2=other) and serial output for debugging
  
    Args:
		None
		
	Returns:
		uint8_t number (0,1 or 2) that indicate why the ESP32 woke up

  */
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  switch (cause) 
  {
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Wake cause: timer (routine check-in)");
      return 0;
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("Wake cause: button press");
      return 1;
    default:
      Serial.println("Wake cause: power-on / reset");
      return 2;
  }
}

void onEspNowSent(const uint8_t *mac, esp_now_send_status_t status) 
{   
    /*
    This function prints out if the ESP-NOW send attempt was a success or a failure
    Args:
		mac : Mac address of the receiever [ESP32-S3 in primary node] (const uint8_t *mac)
		status : Indicates the final result of the transmission attempt
	Returns:
		Void

  */

  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "ESP-NOW: delivered." : "ESP-NOW: send failed.");
}


bool setupEspNow() 
{ 
    /*
    This function sets up ESP-NOW transmission
    Args:
        None
	Returns:
		Void

  */
  if (esp_now_init() != ESP_OK) 
  {
    Serial.println("ESP-NOW init failed.");
    return false;
  }
  esp_now_register_send_cb(onEspNowSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, primaryMacAddress, 6);
  peerInfo.channel = 0;         // 0 = use the current WiFi channel
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) 
  {
    Serial.println("ESP-NOW: failed to add peer.");
    return false;
  }
  return true;
}


void goToSleep() 
{
   /*
    This function sets up ESP32 to deep sleep mode
    Args:
        None
	Returns:
		Void

  */

  pinMode(WAKE_BUTTON_PIN, INPUT_PULLUP);
  esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL);
  esp_sleep_enable_ext0_wakeup(WAKE_BUTTON_PIN, 0);   // 0 = wake on LOW (button pressed)

  Serial.println("Going to deep sleep now.");
  Serial.flush();
  esp_deep_sleep_start();
  // execution never returns past this point — board resets on wake
}
