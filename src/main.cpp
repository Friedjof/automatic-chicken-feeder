#include <Arduino.h>

#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#if defined(ESP32DEV) || defined(ESP32S3)
#include <WiFi.h>
#elif defined(ESP8266)
#include <ESPAsyncTCP.h>
#include <ESP8266WiFi.h>
#endif

#include <SPI.h>
#include <time.h>

#ifndef __CLOCK_H__
#define __CLOCK_H__
#include <Wire.h>
#include <DS3231.h>
#endif

#include <ConfigManager.h>
#include <AlertManager.h>

#if defined(ESP32S3)
#define CONFIG_FILE "/sd/config.json"

#define INDEX_FILE "/sd/index.html"
#define CSS_FILE "/sd/style.css"
#define JS_FILE "/sd/script.js"
#else
#define CONFIG_FILE "/config.json"

#define INDEX_FILE "/index.html"
#define CSS_FILE "/style.css"
#define JS_FILE "/script.js"
#endif

#define FEED_FACTOR 1 // factor to match the mass of the food

#if defined(ESP32DEV) || defined(ESP8266)
#define RELAY_PIN 2   // D2 (Onboard LED)
#define CLINT 4       // RTC interrupt pin for alarm 1
#elif defined(ESP32S3)
#define RELAY_PIN D8  // D2 (Onboard LED)
#define CLINT D7      // RTC interrupt pin for alarm 1
#endif

// Prototypes
void IRAM_ATTR interrupt_handler();
void setup_wifi();  // Setup WiFi
void setup_aws();   // Setup AsyncWebServer

void feed();        // Feed the chickens
void new_request(); // Will be executed when a new request is received
int remaining_auto_sleep_time();

// Global variables
bool interrupt_flag = false;
unsigned long feed_millis = millis();
unsigned long auto_sleep_millis = millis();

// deep sleep
#if defined(ESP32DEV) || defined(ESP32S3)
esp_sleep_wakeup_cause_t wakeup_reason;
#elif defined(ESP8266)
String wakeup_reason;
#endif

AsyncWebServer server(80);

ConfigManager configManager(CONFIG_FILE);
AlertManager alertManager(configManager);


void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("Starte Hühner-Futterautomat");

  #if defined(ESP32S3) && !defined(__INIT_SD_CARD__)
  #define __INIT_SD_CARD__
  // Start micro sd card
  if(!SD.begin()){
    Serial.println("Card Mount Failed");
    return;
  }
  
  uint8_t cardType = SD.cardType();
  
  if(cardType == CARD_NONE){
    Serial.println("No SD card attached");
    return;
  }
  #endif

  // Setup of the alert manager
  alertManager.setup();

  // print next alert
  optional_ds3231_timer_t next_alert = alertManager.get_next_alert();
  if (!next_alert.empty) {
    alertManager.print_timer(next_alert.timer);
  }

  // Setup PIN interrupt
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(CLINT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(CLINT), interrupt_handler, FALLING);

  #if defined(ESP32DEV) || defined(ESP32S3)
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, 0);

  wakeup_reason = esp_sleep_get_wakeup_cause();
  bool timer_wakeup = wakeup_reason == ESP_SLEEP_WAKEUP_EXT0;
  #elif defined(ESP8266)
  ESP.deepSleep(0);

  wakeup_reason = ESP.getResetReason();
  bool timer_wakeup = wakeup_reason == "Deep-Sleep Wake";
  #endif

  if (timer_wakeup) {
    Serial.println("Wakeup caused by external signal using RTC_IO");

    // feed the chickens
    feed();

    // go to sleep
    Serial.println("Schlafmodus aktiviert");

    #if defined(ESP32DEV) || defined(ESP32S3)
    esp_deep_sleep_start();
    #elif defined(ESP8266)
    ESP.deepSleep(0);
    #endif

  } else {
    // Setup WiFi
    setup_wifi();

    // Setup AsyncWebServer
    setup_aws();
  }
}

void loop() {
  // handle interrupt
  if (interrupt_flag) {
    feed();
  }

  // auto sleep depending on AUTO_SLEEP
  if (configManager.get_system_config().auto_sleep && 0 >= remaining_auto_sleep_time()) {
    Serial.println("Going to sleep because of auto sleep");

    // turn the relay off
    digitalWrite(RELAY_PIN, LOW);

    // go to sleep
    #if defined(ESP32DEV) || defined(ESP32S3)
    esp_deep_sleep_start();
    #elif defined(ESP8266)
    ESP.deepSleep(0);
    #endif
  }
}

// Interrupt handler
void IRAM_ATTR interrupt_handler() {
  interrupt_flag = true;
}

// Feed the chickens
void feed() {
  Serial.println("Fütterung gestartet");
  digitalWrite(RELAY_PIN, HIGH);

  delay(configManager.get_quantity() * configManager.get_factor());
  
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("Fütterung beendet");
  interrupt_flag = false;

  // Setup the new alert (if necessary)
  alertManager.set_next_alert();
}

// remaining auto sleep time
int remaining_auto_sleep_time() {
  return (configManager.get_system_config().auto_sleep_after - (millis() - auto_sleep_millis) / 1000);
}

// Will be executed when a new request is received
void new_request() {
  // reset auto sleep millis to prevent going to sleep
  auto_sleep_millis = millis();
}

void setup_wifi() {
  Serial.print("Waiting for SSID and password…");
  while (strcmp(configManager.get_wifi_ssid(), "") == 0 || strcmp(configManager.get_wifi_password(), "") == 0) {
    Serial.print(".");
    delay(1000);
  }
  Serial.println("Done");

  Serial.println("Starte WiFi Access Point");
  WiFi.softAP(configManager.get_wifi_ssid(), configManager.get_wifi_password());

  Serial.println();
  Serial.print("Hotspot-SSID: ");
  Serial.println(configManager.get_wifi_ssid());
  Serial.print("Hotspot-IP-Adresse: ");
  Serial.println(WiFi.softAPIP());
  Serial.println();
}

void setup_aws() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    new_request();

    #if defined(ESP32S3)
    request->send(SD, INDEX_FILE, "text/html");
    #else
    request->send(LittleFS, INDEX_FILE, "text/html");
    #endif
  });
  
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    #if defined(ESP32S3)
    request->send(SD, INDEX_FILE, "text/html");
    #else
    request->send(LittleFS, CSS_FILE, "text/css");
    #endif
  });

  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    #if defined(ESP32S3)
    request->send(SD, INDEX_FILE, "text/html");
    #else
    request->send(LittleFS, JS_FILE, "text/javascript");
    #endif
  });

  server.on("/get", HTTP_GET, [](AsyncWebServerRequest *request) {
    new_request();

    String jsonString;
    serializeJson(configManager.get_timers_json(), jsonString);

    Serial.println("Get configuration");

    request->send(200, "application/json", jsonString);
  });

  // This endpoint is used to set the timers
  AsyncCallbackJsonWebHandler* set_handler = new AsyncCallbackJsonWebHandler("/set", [](AsyncWebServerRequest *request, JsonVariant &json) {
    new_request();

    // Convert the data to a JSON object
    Serial.println("Set new configuration");
    configManager.set_timers_json(json);

    // Setup the new alert (if necessary)
    alertManager.set_next_alert();

    request->send(200);
  });

  server.addHandler(set_handler);

  // activate sleep mode
  server.on("/sleep", HTTP_GET, [](AsyncWebServerRequest *request) {
    new_request();

    Serial.println("Schlafmodus aktiviert");
    
    // go to sleep
    #if defined(ESP32DEV) || defined(ESP32S3)
    esp_deep_sleep_start();
    #elif defined(ESP8266)
    ESP.deepSleep(0);
    #endif

    request->send(200);
  });

  // get current time
  server.on("/time", HTTP_GET, [](AsyncWebServerRequest *request) {
    // get current time
    ds3231_datetime_t now = alertManager.now();

    // create json object
    StaticJsonDocument<JSON_OBJECT_SIZE(6)> json;
    json["year"] = now.year;
    json["month"] = now.month;
    json["day"] = now.day;
    json["hour"] = now.hour;
    json["minute"] = now.minute;
    json["second"] = now.second;

    String jsonString;
    serializeJson(json, jsonString);

    request->send(200, "application/json", jsonString); 
  });

  // get autosleep remaining time
  server.on("/autosleep", HTTP_GET, [](AsyncWebServerRequest *request) {
    // get remaining time
    int remaining = remaining_auto_sleep_time();

    // create json object
    StaticJsonDocument<JSON_OBJECT_SIZE(1)> json;
    json["remaining"] = remaining;

    String jsonString;
    serializeJson(json, jsonString);

    request->send(200, "application/json", jsonString); 
  });

  // feed manually
  AsyncCallbackJsonWebHandler* feed_handler = new AsyncCallbackJsonWebHandler("/feed", [](AsyncWebServerRequest *request, JsonVariant &json) {
    new_request();

    // if 'on' is true, start feeding
    if (json.containsKey("on") && json["on"].is<bool>()) {
      if (json["on"].as<bool>()) {
        Serial.println("Start feeding");
        digitalWrite(RELAY_PIN, HIGH);
      } else {
        Serial.println("Stop feeding");
        digitalWrite(RELAY_PIN, LOW);
      }
    }

    request->send(200);
  });

  server.addHandler(feed_handler);

  // Webserver starten
  Serial.println("Starte Webserver");
  server.begin();
}