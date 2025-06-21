#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <HTTPClient.h>
#include <LiquidCrystal.h>
#include <WebServer.h>
#include "secrets.h"


// --- Pin Definitions ---
#define TEMP1_PIN 15     // Pool temperature sensor
#define TEMP2_PIN 16     // Solar output temperature sensor
#define OPEN_GATE_PIN 21
#define CLOSE_GATE_PIN 22
#define BUTTON_PIN 33

// LCD pin definitions
const int lcd_rs = 5;
const int lcd_en = 18;
const int lcd_d4 = 19;
const int lcd_d5 = 23;
const int lcd_d6 = 4;
const int lcd_d7 = 13;
LiquidCrystal lcd(lcd_rs, lcd_en, lcd_d4, lcd_d5, lcd_d6, lcd_d7);

// --- Temperature Sensor Setup ---
OneWire oneWire1(TEMP1_PIN);
DallasTemperature sensor1(&oneWire1);  // Pool sensor

OneWire oneWire2(TEMP2_PIN);
DallasTemperature sensor2(&oneWire2);  // Solar sensor

// --- Valve Cycle State Machine ---
enum ValveCycleState { WAITING, OPENING, CLOSING };
ValveCycleState valveCycleState = WAITING;
bool isValveOpen = false;
unsigned long stateStartTime = 0;

// --- Valve Motion Control ---
enum ValveMotion { IDLE, OPENING_MOTION, CLOSING_MOTION };
ValveMotion valveMotion = IDLE;
unsigned long valveMotionStart = 0;

// --- Manual Override ---
unsigned long manualOverrideStart = 0;
bool manualOverrideActive = false;

// --- Status ---
String pumpStatus = "OFF";
String solarStatus = "OFF";

// --- Cached Temperatures ---
float temp1 = 0.0;
float temp2 = 0.0;

// --- HTTP Server ---
WebServer server(80);

// --- Unified LCD Update ---
void updateLCD(float temp_pool, float temp_solar, const String& pumpStat, const String& solarStat) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("P:");
  lcd.print(temp_pool, 1);
  lcd.print(" Pump:");
  lcd.print(pumpStat);

  lcd.setCursor(0, 1);
  lcd.print("H:");
  lcd.print(temp_solar, 1);
  lcd.print(" Heat:");
  lcd.print(solarStat);
}

// --- Connect to WiFi ---
void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries++ < 30) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n❌ WiFi connection failed. Rebooting...");
    delay(5000);
    ESP.restart();
  }
}

// --- Shelly Pump Status ---
bool IsPumpOn() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ Can't check Shelly state: WiFi not connected.");
    return false;
  }

  HTTPClient http;
  http.begin("http://192.168.178.33/rpc/Switch.GetStatus?id=0");
  int httpCode = http.GET();

  if (httpCode > 0) {
    String payload = http.getString();
    int outputIndex = payload.indexOf("\"output\":");
    if (outputIndex != -1) {
      String stateStr = payload.substring(outputIndex + 9);
      stateStr.trim();
      bool isOn = stateStr.startsWith("true");
      Serial.print("🔌 Shelly plug is ");
      Serial.println(isOn ? "ON" : "OFF");
      return isOn;
    }
  }

  Serial.println("⚠️ Failed to get pump state.");
  return false;
}

// --- Pump Control ---
void CallPump(String state) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://192.168.178.33/relay/0?turn=" + state;

    http.begin(url);
    int httpCode = http.GET();

    if (httpCode > 0) {
      Serial.print("✅ Pump HTTP GET ");
      Serial.print(state);
      Serial.print(": ");
      Serial.println(httpCode);
      String response = http.getString();
      Serial.println(response);
    } else {
      Serial.print("❌ Pump HTTP GET ");
      Serial.print(state);
      Serial.print(" failed: ");
      Serial.println(http.errorToString(httpCode));
    }

    http.end();
  } else {
    Serial.println("❌ WiFi not connected. Can't call pump.");
  }
}

// --- Valve Control ---
void OpenValve() {
  digitalWrite(CLOSE_GATE_PIN, LOW);
  digitalWrite(OPEN_GATE_PIN, HIGH);
  valveMotionStart = millis();
  valveMotion = OPENING_MOTION;
  Serial.println("🔓 OpenValve started");
}

void CloseValve() {
  digitalWrite(OPEN_GATE_PIN, LOW);
  digitalWrite(CLOSE_GATE_PIN, HIGH);
  valveMotionStart = millis();
  valveMotion = CLOSING_MOTION;
  Serial.println("🔒 CloseValve started");
}

void StopValveMotion() {
  digitalWrite(OPEN_GATE_PIN, LOW);
  digitalWrite(CLOSE_GATE_PIN, LOW);
  valveMotion = IDLE;
  Serial.println("⏹️ Valve motion stopped");
}

void setup() {
  Serial.begin(115200);

  connectToWiFi();
  delay(10000);  // let Wi-Fi settle

  ArduinoOTA.setHostname("esp32-pool");
  ArduinoOTA.begin();
  Serial.println("OTA ready");

  sensor1.begin();
  sensor2.begin();

  pinMode(OPEN_GATE_PIN, OUTPUT);
  pinMode(CLOSE_GATE_PIN, OUTPUT);
  StopValveMotion();

  lcd.begin(16, 2);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Initializing...");

  valveCycleState = WAITING;
  isValveOpen = false;
  stateStartTime = millis();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // --- HTTP Server Routes ---
  server.on("/", []() {
    server.send(200, "text/plain", "ESP32 Pool Controller Ready");
  });

  server.on("/valve/toggle", []() {
    if (isValveOpen) {
      CloseValve();
      solarStatus = "OFF";
      isValveOpen = false;
    } else {
      OpenValve();
      solarStatus = "ON";
      isValveOpen = true;
    }
    manualOverrideStart = millis();
    manualOverrideActive = true;
    server.send(200, "text/plain", "Valve toggled");
  });

  server.on("/status", []() {
    String json = "{";
    json += "\"temp_pool\":" + String(temp1) + ",";
    json += "\"temp_solar\":" + String(temp2) + ",";
    json += "\"pump\":\"" + pumpStatus + "\",";
    json += "\"valve\":\"" + String(isValveOpen ? "open" : "closed") + "\"";
    json += "}";
    server.send(200, "application/json", json);
  });

  server.begin();
  Serial.println("🌐 HTTP server started");
}

void loop() {
  // --- WiFi Watchdog ---
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 10000) {
    lastWiFiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠️ WiFi lost! Reconnecting...");
      WiFi.disconnect(true);
      WiFi.begin(ssid, password);
    }
  }

  ArduinoOTA.handle();
  server.handleClient();

  unsigned long now = millis();

  // --- Manual Toggle Button Control ---
  static bool buttonPreviouslyPressed = false;
  bool buttonNowPressed = (digitalRead(BUTTON_PIN) == LOW);

  if (!manualOverrideActive && buttonNowPressed && !buttonPreviouslyPressed) {
    Serial.println("🔘 Manual toggle button pressed!");

    if (isValveOpen) {
      CloseValve();
      solarStatus = "OFF";
      isValveOpen = false;
    } else {
      OpenValve();
      solarStatus = "ON";
      isValveOpen = true;
    }

    manualOverrideStart = now;
    manualOverrideActive = true;
  }

  buttonPreviouslyPressed = buttonNowPressed;

  if (manualOverrideActive && now - manualOverrideStart >= 20000) {
    StopValveMotion();
    manualOverrideActive = false;
  }

  // --- Temperature Logging ---
  static unsigned long lastTempRead = 0;
  if (now - lastTempRead > 1000) {
    lastTempRead = now;

    sensor1.requestTemperatures();
    sensor2.requestTemperatures();

    unsigned long waitStart = millis();
    while (millis() - waitStart < 100) {
      ArduinoOTA.handle();
      delay(1);
    }

    temp1 = sensor1.getTempCByIndex(0);
    temp2 = sensor2.getTempCByIndex(0);

    Serial.print("🌡️ Pool Temp: ");
    Serial.print(temp1);
    Serial.print(" °C | ☀️ Solar Temp: ");
    Serial.print(temp2);
    Serial.println(" °C");
  }

  // --- Pump Status Polling ---
  static unsigned long lastPumpCheck = 0;
  if (now - lastPumpCheck >= 10000) {
    lastPumpCheck = now;
    pumpStatus = IsPumpOn() ? "ON" : "OFF";
  }

  updateLCD(temp1, temp2, pumpStatus, solarStatus);

  unsigned long waitStart = millis();
  while (millis() - waitStart < 10) {
    ArduinoOTA.handle();
    delay(1);
  }
}
