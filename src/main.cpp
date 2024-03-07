#include <Arduino.h>

#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <TaskScheduler.h>

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

#define CONFIG_FILE "/config.json"

#define INDEX_FILE "/index.html"
#define CSS_FILE "/style.css"
#define JS_FILE "/script.js"

#define FEED_FACTOR 1 // factor to match the mass of the food

#if defined(ESP32DEV) || defined(ESP8266)
#define RELAY_PIN 2   // D2 (Onboard LED)
#define CLINT 4       // RTC interrupt pin for alarm 1
#elif defined(ESP32S3)
#define RELAY_PIN 8   // D2 (Onboard LED)
#define CLINT 7       // RTC interrupt pin for alarm 1
#endif

// Prototypes
void IRAM_ATTR interrupt_handler();
void setup_wifi();    // Setup WiFi
void setup_aws();     // Setup AsyncWebServer

bool is_feeding();    // Check if the chickens are being fed
void start_feeding(); // Start feeding the chickens
void stop_feeding();  // Stop feeding the chickens
void sleep_mode();    // Go to sleep
void new_request();   // Will be executed when a new request is received

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

// TaskScheduler
Scheduler runner;

Task startFeedingTask(TASK_IMMEDIATE, TASK_ONCE, &start_feeding, &runner, false);
Task stopFeedingTask(TASK_IMMEDIATE, TASK_ONCE, &stop_feeding, &runner, false);

// ------------------------------------- SETUP -------------------------------------

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("Starte Hühner-Futterautomat");

  // Init TaskScheduler
  runner.init();
  runner.addTask(startFeedingTask);
  runner.addTask(stopFeedingTask);

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

  // If the wakeup was caused by the timer
  if (timer_wakeup) {
    Serial.println("Wakeup caused by external signal using RTC_IO");

    // feed the chickens and wait for the feeding to finish
    startFeedingTask.enable();
    while (is_feeding()) {
      runner.execute();
    }

    // go to sleep
    sleep_mode();

  } else {
    // Setup WiFi
    setup_wifi();

    // Setup AsyncWebServer
    setup_aws();
  }
}

// ------------------------------------- LOOP -------------------------------------

void loop() {
  runner.execute();

  // handle feeding
  if (interrupt_flag && !is_feeding()) {
    startFeedingTask.enable();
  }

  // auto sleep depending on AUTO_SLEEP
  if (configManager.get_system_config().auto_sleep && (millis() - auto_sleep_millis > (long unsigned int)(1000 * configManager.get_system_config().auto_sleep_after))) {
    sleep_mode();
  }
}

bool is_feeding() {
  return startFeedingTask.isEnabled() || stopFeedingTask.isEnabled();
}

// Start feeding
void start_feeding() {
  Serial.println("Start feeding");
  digitalWrite(RELAY_PIN, HIGH);

  // Planen des Endes der Fütterung
  stopFeedingTask.setInterval(configManager.get_quantity() * configManager.get_factor());
  stopFeedingTask.enable();
}

// Stop feeding
void stop_feeding() {
  digitalWrite(RELAY_PIN, LOW);

  Serial.println("Stop feeding");
  interrupt_flag = false;

  alertManager.set_next_alert();
}

// Activate sleep mode
void sleep_mode() {
  Serial.println("Schlafmodus aktiviert");

  // go to sleep
  #if defined(ESP32DEV) || defined(ESP32S3)
  esp_deep_sleep_start();
  #elif defined(ESP8266)
  ESP.deepSleep(0);
  #endif
}

// Interrupt handler
void IRAM_ATTR interrupt_handler() {
  interrupt_flag = true;
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

// ------------------------------------- SETUP AWS -------------------------------------

void setup_aws() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    new_request();

    request->send(LittleFS, INDEX_FILE, "text/html");
  });
  
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, CSS_FILE, "text/css");
  });

  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, JS_FILE, "text/javascript");
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
    new_request();

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