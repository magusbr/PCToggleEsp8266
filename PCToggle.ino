// Import required libraries
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <AddrList.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "certs.h"

// IPv6 support based on:
// https://github.com/me-no-dev/AsyncTCP/pull/105/commits/0dc3b996a47ac63b3583667330103d8e0e6d5bb2

// To support IPv6:
// - Set Tools/lwIP variant to "v2 IPv6 Lower Memory"
// - Copy ESPAsyncTCP to libraries folder of your Arduino

// Retrieved from JSON cfg file (max 2024)
/*
{"wifi_ssid":"","wifi_password":"","afraid_org_key":"","ydns_eu_path":"","ydns_eu_auth":""}
 */
char afraid_org[128] = {0};
char ssid[32] = {0};
char password[32] = {0};
char ydns_path[64] = {0};
char ydns_auth[128] = {0};

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
    strcat(ydns_path, a.toString().c_str());
    Serial.printf("full path = [%s]\n", ydns_path);
  
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

  load_cfg();

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
    if (!digitalRead(ledInput)) {
      //Serial.println("led on");
      request->send(201, "text/plain", "ON");
    } else {
      request->send(204, "text/plain", "OFF");
      //Serial.println("led off");
    }
  });

  // attach AsyncWebSocket
  server.addHandler(&ws);

  // Start server
  server.begin();
}

void setClock() {
  configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");

  Serial.print("Waiting for NTP time sync: ");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println("");
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  Serial.print("Current time: ");
  Serial.print(asctime(&timeinfo));
}

void fetchURL(BearSSL::WiFiClientSecure *client, const char *host, const uint16_t port, const char *path) {
  if (!path) { path = "/"; }

  ESP.resetFreeContStack();
  uint32_t freeStackStart = ESP.getFreeContStack();
  Serial.printf("Trying: %s:443...", host);

//  IPAddress result;
//  if (WiFi.hostByName(host, result, 10000, DNSResolveType::DNS_AddrType_IPv6)) {
//    Serial.printf("Solved ipv6\n");
//    Serial.printf("ip %s\n", result.toString().c_str());
//  }
//  char pom[1024];
//  sprintf(pom, "[%s]", result.toString().c_str());
//  Serial.printf("%s\n", pom);
//  client->connect(host, port);

  // it connects but only as a ipv4 - need to investigate why it doesn't connect as ipv6 even using ip

  client->connect(host, port);
  if (!client->connected()) {
    Serial.printf("*** Can't connect. ***\n-------\n");
    return;
  }
  Serial.printf("Connected!\n-------\n");
  client->write("GET ");
  client->write(path);
  client->write(" HTTP/1.0\r\nHost: ");
  client->write(host);
  client->write(ydns_auth);
  client->write("\r\nUser-Agent: ESP8266\r\n");
  client->write("\r\n");
  uint32_t to = millis() + 5000;
  if (client->connected()) {
    do {
      char tmp[32];
      memset(tmp, 0, 32);
      int rlen = client->read((uint8_t *)tmp, sizeof(tmp) - 1);
      yield();
      if (rlen < 0) { break; }
//      // Only print out first line up to \r, then abort connection
//      char *nl = strchr(tmp, '\r');
//      if (nl) {
//        *nl = 0;
//        Serial.print(tmp);
//        break;
//      }
      Serial.print(tmp);
    } while (millis() < to);
  }
  client->stop();
  uint32_t freeStackEnd = ESP.getFreeContStack();
  Serial.printf("\nCONT stack used: %d\n", freeStackStart - freeStackEnd);
  Serial.printf("BSSL stack used: %d\n-------\n\n", stack_thunk_get_max_usage());
}

void https_connect() {
  BearSSL::WiFiClientSecure client;
  BearSSL::X509List cert(cert_ISRG_Root_X1);
  client.setTrustAnchors(&cert);
  setClock();
  fetchURL(&client, "ydns.io", 443, ydns_path);
}

void load_cfg() {
  char buffer[2048+1] = {0};
  
  File f = LittleFS.open("/secrets.json", "r");
  if (f) {
    f.readBytes(buffer, 2048);
    //Serial.printf("[%s]\n", buffer);

    f.close();
  } else {
    return;
  }
  
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, buffer);
  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }

  Serial.printf("ssid = [%s]\n", (const char*)doc["wifi_ssid"]);
  Serial.printf("afraid_org = [%s]\n", (const char*)doc["afraid_org_key"]);
  Serial.printf("ydns_path = [%s]\n", (const char*)doc["ydns_eu_path"]);
  
  strcpy(ssid, (const char*)doc["wifi_ssid"]);
  strcpy(password, (const char*)doc["wifi_password"]);
  sprintf(afraid_org, "http://v6.sync.afraid.org/u/%s", (const char*)doc["afraid_org_key"]);
  strcpy(ydns_path, (const char*)doc["ydns_eu_path"]);
  strcpy(ydns_auth, (const char*)doc["ydns_eu_auth"]);
}

void loop() {
  unsigned long currentDns = millis();
  if ((currentDns - previousDns >= intervalDns) || (previousDns == 0)) {
    //https_connect();
    Serial.println("reset dns - begin");

    if (WiFi.status() == WL_CONNECTED) {
      WiFiClient client;
      HTTPClient http;
      http.setTimeout(2000);
      
      if (http.begin(client, afraid_org)) {
        int httpCode = http.GET();
        if (httpCode > 0) {
          Serial.print("dns http code: ");
          Serial.println(httpCode);
          String payload = http.getString();
          Serial.println(payload);
        } else {
          Serial.print("error: ");
          Serial.println(httpCode);
          Serial.println(http.errorToString(httpCode).c_str());
        }
        http.end();
      } else {
        Serial.println("connect error");
      }
    } else {
      Serial.println("WiFi disconnected");
    }

    previousDns = currentDns;
    Serial.println("reset dns - end");
  }

  unsigned long currentStatus = millis();
  if ((currentStatus - previousStatus >= intervalStatus) || (previousDns == 0)) {
    if (!digitalRead(ledInput)) {
      //Serial.println("led on");
      ws.textAll("ON");
    } else {
      ws.textAll("OFF");
      //Serial.println("led off");
    }
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
