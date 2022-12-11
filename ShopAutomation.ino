#include <Wire.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "HT_SSD1306Wire.h"
#include <DHT.h>
#include <EEPROM.h>

#define EEPROM_SIZE 1

#define DHTPIN 22
#define DHTTYPE DHT11

#define FURNACE_PIN 5

#define SOFT_RESTART 0
#define HARD_RESTART 1

/*
Save # of restarts - 1
Save bool value for if it was restarted by the automated process - 2
00021111
*/

// NOT recommended: Usually doesn't fix not connecting problem
// Soft Restart: WiFi gets reinitialized
//#define WIFI_ERR_RESTART_TYPE SOFT_RESTART
// Hard Restart: ESP32 restarts
#define WIFI_ERR_RESTART_TYPE HARD_RESTART

SSD1306Wire  display(0x3c, 500000, SDA_OLED, SCL_OLED, GEOMETRY_128_64, RST_OLED); // addr , freq , i2c group , resolution , rst
WebServer server(80);
DHT dht(DHTPIN, DHTTYPE);

String ConnectionState = String("");
String Temp = String("Unknown");
String humidity = String("Unknown");

// START CONFIG
const char* ssid = "test";
const char* password = "testtest";

const int minTemp = 40;
const int maxTemp = 75;
const int defaultTemp = minTemp;

const int maxRestarts = 8;

// ticks per second
const int tickRate = 1;

// 5 min cooldown time
const int cooldownTimeInSeconds = 5 * 60;
const int maxFurnaceOnTime = 30 * 60; // 30 minutes
// END CONFIG

// START GLOBAL
unsigned long lastTickTime;
int currentTemp = 0;
int targetTemp = defaultTemp;
bool furnaceOn = false;
bool cooldown = false;

// In Seconds
int furnaceOnFor = 0;
int cooldownTimer = 0;
// END GLOBAL

// START EEPROM
int wifiErrorRestarts = 0;
bool automatedRestart = false;
bool hitMaxRestarts = false;
// END EEPROM

void VextON(void)
{
  pinMode(Vext,OUTPUT);
  digitalWrite(Vext, LOW);
}

void VextOFF(void) //Vext default OFF
{
  pinMode(Vext,OUTPUT);
  digitalWrite(Vext, HIGH);
}

void HandleRoot() {
  String output = "";
  output += String("Humidity: ");
  output += humidity + String("\n");
  output += String("Current Temp | Target Temp: \n");
  output += String(Temp) + String("\n");
  String furnaceState;
  if (furnaceOn) {
    furnaceState = String("Furnace On For: ") + furnaceOnFor + String("s\n");
  } else if (cooldown) {
    furnaceState = String("Cooling Down For: ") + cooldownTimer + String("s\n");
  } else {
    furnaceState = "Furnace Off \n";
  }
  output += furnaceState;

  server.send(200, "text/plain", output);
}

void HandleNotFound() {
  server.send(404, "text/plain", "Not found");
}


void HandleTemp() {

  if (server.args() != 1) {
    server.send(400, "application/json", "{\"error\": \"no temp query param passed\"}");
  }

  setTargetTemp(server.arg(0).toInt());

  server.send(200, "application/json", "{\"message\": \"success\"}");    
}

void WriteRestartInfoAndRestart(int restarts, bool automated) {
  Serial.println("Restarts: " + String(restarts));
  int infoByte = 0;
  infoByte = restarts & 0b00001111;
  infoByte = infoByte | (((int)automated & 1) << 4);
  Serial.println("InfoByte: " + String(infoByte));
  EEPROM.write(0, infoByte);
  EEPROM.commit();
  delay(500);
  ESP.restart();
}

void SetupWifi() {
  WiFi.begin(ssid, password);
  Serial.println(String("Connecting to ") + String(ssid));

  updateTemp();

  // Wait for connection
  int dotCount = 0;
  int notConnectedCounter = 0;
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(100);
    Serial.print(".");

    if (dotCount == 8) {
      dotCount = 0;
    }
    if (dotCount == 0) {
      ConnectionState = String("Connecting to: ") + String(ssid);
    }

    ConnectionState = String(ConnectionState)+ String(".");
    dotCount++;
    notConnectedCounter++;


    if (notConnectedCounter > 150) {
      if (WIFI_ERR_RESTART_TYPE == SOFT_RESTART) {
        WiFi.disconnect(true);
        WiFi.begin(ssid, password);
        notConnectedCounter = 0;
        
      } else if (WIFI_ERR_RESTART_TYPE == HARD_RESTART) {
        WriteRestartInfoAndRestart(wifiErrorRestarts+1, true);
      }
    }

    tick();
  }

  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  ConnectionState = String("Connected: ") + WiFi.localIP().toString();

  if (MDNS.begin("esp32")) {
    Serial.println("MDNS responder started");
  }

  server.on("/", HandleRoot);

  server.on("/temp", HandleTemp);

  server.on("/inline", []() {
    server.send(200, "text/plain", "this works as well");
  });

  server.onNotFound(HandleNotFound);

  server.begin();
  Serial.println("HTTP server started");
}


void heatOn() {
  furnaceOn = true;
  digitalWrite(FURNACE_PIN, HIGH);
}

void heatOff() {
  furnaceOn = false;
  digitalWrite(FURNACE_PIN, LOW);
  furnaceOnFor = 0;
  cooldown = true;
}

void PrintStatus() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  if (!hitMaxRestarts) {
    display.drawString(0, 0, "Wifi Status: ");
    display.drawString(0, 12, ConnectionState);
  } else {
    display.drawString(0, 0, "Error Connecting!");
    display.drawString(0, 12, "Attempted Restarts: " + String(wifiErrorRestarts));
  }
  
  display.drawString(0, 24, "Current Temp | Target Temp: ");
  display.drawString(0, 36, Temp);

  String furnaceState;
  if (furnaceOn) {
    furnaceState = String("Furnace On For: ") + furnaceOnFor + String("s");
  } else if (cooldown) {
    furnaceState = String("Cooling Down For: ") + cooldownTimer + String("s");
  } else {
    furnaceState = "Furnace Off";
  }

  display.drawString(0, 48, furnaceState);
  display.display();
}

void updateTemp() {
  float t = dht.readTemperature(true);

  float h = dht.readHumidity();

  Serial.println("Temp: ");
  Serial.println(t);

  humidity = String(h, 2) + String("%");

  currentTemp = int(round(t));
  Temp = String(t, 2) + String("F | ") + String(targetTemp) + String("F");
}

void checkFurnace() {
  if (cooldown) {
    cooldownTimer++;
    if (cooldownTimer >= cooldownTimeInSeconds) {
      cooldown = false;
      cooldownTimer = 0;
    }
    return;
  }

  if (furnaceOn) {
    furnaceOnFor++;
    if (currentTemp >= targetTemp || furnaceOnFor > maxFurnaceOnTime) {
      heatOff();
    }
  } else {
    if (currentTemp < targetTemp) {
      heatOn();
    }
  }
}

void setTargetTemp(int temp) {
  if (temp > maxTemp) {
    targetTemp = maxTemp;
  } else if (temp < minTemp) {
    targetTemp = minTemp;
  } else {
    targetTemp = temp;
  }
}

void ReadRestartInfo() {

  int infoByte = EEPROM.read(0);
  Serial.println("Info: " + String(infoByte));
  // Account for blank eeprom
  if (infoByte == 255) {
    infoByte = 0;
  }

  wifiErrorRestarts = infoByte & 0b00001111;
  automatedRestart = infoByte >> 4 & 1;
  // hacky way to implement resetting this
  if (!automatedRestart) {
    wifiErrorRestarts = 0;
  }

  if (wifiErrorRestarts >= maxRestarts) {
    hitMaxRestarts = true;
  }

  // Reset to 0 for automatedRestart flag to work
  EEPROM.write(0, 0);
  EEPROM.commit();
  delay(100);  
}


void setup() {
  // Setup display
  display.init();
  display.clear();
  display.display();
  display.setContrast(255);
  
  if (!EEPROM.begin(EEPROM_SIZE)) {
    display.drawString(0, 0, "Can't init EEPROM");
    display.display();
    delay(100000000000);
  };

  Serial.begin(115200);
  Serial.println();

  ReadRestartInfo();
  
  VextON();
  delay(100);

  // Temp Sensor
  dht.begin();

  // Furnace Pin
  pinMode(FURNACE_PIN, OUTPUT);

  if (!hitMaxRestarts) {
    SetupWifi();    
  }

  lastTickTime = millis();
}

void tick() {
  if (millis() - lastTickTime > (1000 / tickRate)) {
    updateTemp();
    checkFurnace();
    PrintStatus();
    lastTickTime = millis();
  }
}

void loop() { 
  delay(5);
  server.handleClient();
  tick();
}
