// Import required libraries
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <AddrList.h>
#include <LittleFS.h>

// IPv6 support based on:
// https://github.com/me-no-dev/AsyncTCP/pull/105/commits/0dc3b996a47ac63b3583667330103d8e0e6d5bb2

// Replace with your network credentials
#define DNS_IPV6_KEY ""
const char* ssid = "";
const char* password = "";
String dnsKey = ""; // key to update dyndns
String dnsFingerprint = ""; // certificate freedns.afraid.org

// Set LED GPIO
const int ledPin = 4; //d2
const int ledInput = 13; //d7
// Stores LED state
String ledInputState;

// Create AsyncWebServer object
AsyncWebServer server(5443);
AsyncWebSocket ws("/ws"); // access at ws://[esp ip]/ws

#define FQDN  F("www.google.com")  // with both IPv4 & IPv6 addresses
#define FQDN2 F("www.yahoo.com")   // with both IPv4 & IPv6 addresses
#define FQDN6 F("ipv6.google.com") // does not resolve in IPv4

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

void fqdn(Print& out, const String& fqdn) {
  Serial.print(F("resolving "));
  Serial.print(fqdn);
  Serial.print(F(": "));
  IPAddress result;
  if (WiFi.hostByName(fqdn.c_str(), result)) {
    result.printTo(out);
    Serial.println();
  } else {
    Serial.println(F("timeout or not found"));
  }
}

#if LWIP_IPV4 && LWIP_IPV6
void fqdn_rt(Print& out, const String& fqdn, DNSResolveType resolveType) {
  Serial.print(F("resolving "));
  Serial.print(fqdn);
  Serial.print(F(": "));
  IPAddress result;
  if (WiFi.hostByName(fqdn.c_str(), result, 10000, resolveType)) {
    result.printTo(out);
    Serial.println();
  } else {
    Serial.println(F("timeout or not found"));
  }
}
#endif

void status(Print& out) {
  Serial.println(F("------------------------------"));
  Serial.println(ESP.getFullVersion());

  for (int i = 0; i < DNS_MAX_SERVERS; i++) {
    IPAddress dns = WiFi.dnsIP(i);
    if (dns.isSet()) {
      Serial.printf("dns%d: %s\n", i, dns.toString().c_str());
    }
  }

  Serial.println(F("My addresses:"));
  for (auto a : addrList) {
    Serial.printf("IF='%s' IPv6=%d local=%d hostname='%s' addr= %s",
               a.ifname().c_str(),
               a.isV6(),
               a.isLocal(),
               a.ifhostname(),
               a.toString().c_str());

    if (a.isLegacy()) {
      Serial.printf(" / mask:%s / gw:%s",
                 a.netmask().toString().c_str(),
                 a.gw().toString().c_str());
    }

    Serial.println();

  }

  // lwIP's dns client will ask for IPv4 first (by default)
  // an example is provided with a fqdn which does not resolve with IPv4
  fqdn(out, FQDN);
  fqdn(out, FQDN6);
#if LWIP_IPV4 && LWIP_IPV6
  fqdn_rt(out, FQDN,  DNSResolveType::DNS_AddrType_IPv4_IPv6); // IPv4 before IPv6
  fqdn_rt(out, FQDN2, DNSResolveType::DNS_AddrType_IPv6_IPv4); // IPv6 before IPv4
#endif
  Serial.println(F("------------------------------"));
}

void setup() {
  // Serial port for debugging purposes
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  pinMode(ledInput, INPUT_PULLUP);

  delay(2000);

  // Initialize LittleFS
  if (!LittleFS.begin()) {
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }

  // Connect to Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  status(Serial);

  for (bool configured = false; !configured;) {
    for (auto addr : addrList)
      if ((configured = !addr.isLocal()
                        // && addr.isV6() // uncomment when IPv6 is mandatory
                        // && addr.ifnumber() == STATION_IF
          )) {
        break;
      }
    Serial.print('.');
    delay(500);
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
    if (http.begin("http://v6.sync.afraid.org/u/" DNS_IPV6_KEY)) {
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
