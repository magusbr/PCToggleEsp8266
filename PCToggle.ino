// Import required libraries
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <LittleFS.h>

// Replace with your network credentials
const char* ssid = "";
const char* password = "";
String dnsKey = ""; // key to update dyndns
String dnsFingerprint = ""; // certificate freedns.afraid.org

// Set LED GPIO
const int ledPin = 4; //d2
const int ledInput = 5; //d1
// Stores LED state
String ledInputState;

// Create AsyncWebServer object on port 80
AsyncWebServer server(80);
AsyncWebSocket ws("/ws"); // access at ws://[esp ip]/ws

IPAddress ip(192, 168, 0, 5);
IPAddress gateway(192, 168, 0, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);
IPAddress dns2(8, 8, 4, 4);

const long intervalDns = 10 * 60 * 1000; // milliseconds
unsigned long previousDns = 0;
const long intervalStatus = 2 * 1000; // milliseconds
unsigned long previousStatus = 0;

int togglepc = 0;

// Replaces placeholder with LED state value
String processor(const String& var) {
  if (var == "STATE") {
    if (!digitalRead(ledInput))
      ledInputState = "setOn();";
    else
      ledInputState = "setOff();";
    return ledInputState;
  }
  return String();
}

void setup() {
  // Serial port for debugging purposes
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  pinMode(ledInput, INPUT_PULLUP);

  // Initialize LittleFS
  if (!LittleFS.begin()) {
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }

  // Connect to Wi-Fi
  WiFi.config(ip, gateway, subnet, dns, dns2);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi..");
  }

  // Print ESP32 Local IP Address
  Serial.println(WiFi.localIP());

  // Route for root / web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(LittleFS, "/index.html", String(), false, processor);
  });

  // Route to load style.css file
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(LittleFS, "/style.css", "text/css");
  });

  server.on("/togglepc", HTTP_GET, [](AsyncWebServerRequest * request) {
    togglepc = 1;
    request->redirect("/");
  });

  server.on("/getstatus", HTTP_GET, [](AsyncWebServerRequest * request) {
    if (!digitalRead(ledInput))
      request->send(201, "text/plain", "ON");
    else
      request->send(204, "text/plain", "OFF");
  });

  // attach AsyncWebSocket
  server.addHandler(&ws);

  // Start server
  server.begin();
}

void loop() {
  unsigned long currentDns = millis();
  if ((currentDns - previousDns >= intervalDns) || (previousDns == 0)) {
    Serial.println("reset dns - begin");
    HTTPClient http;
    http.setTimeout(2000);
    if (http.begin("https://freedns.afraid.org/dynamic/update.php?" + dnsKey, dnsFingerprint)) {
      int httpCode = http.GET();
      if (httpCode > 0) {
        Serial.print("dns http code: ");
        Serial.println(httpCode);
        String payload = http.getString();
        Serial.println(payload);
      } else {
        Serial.println(http.errorToString(httpCode).c_str());
      }
      http.end();
    } else {
      Serial.println("connect error");
    }
    previousDns = currentDns;
    Serial.println("reset dns - end");
  }

  unsigned long currentStatus = millis();
  if ((currentStatus - previousStatus >= intervalStatus) || (previousDns == 0)) {
    if (!digitalRead(ledInput))
      ws.textAll("ON");
    else
      ws.textAll("OFF");
    previousStatus = currentStatus;
  }

  if (togglepc == 1) {
    togglepc = 0;
    Serial.print("TOGGLE PC CALLED");
    Serial.print(ledPin);
    digitalWrite(ledPin, HIGH);   // Turn the LED on by making the voltage LOW
    delay(200);                      // Milliseconds
    digitalWrite(ledPin, LOW);  // Turn the LED off by making the voltage HIGH
    Serial.println(" - end");
  }
}
