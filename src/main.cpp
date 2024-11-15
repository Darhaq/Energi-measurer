/**
 * @file main.cpp
 * @brief ESP32 Webserver med WebSocket, SPIFFS og touchsensor-integration.
 *
 * Dette program implementerer en ESP32-baseret webserver med:
 * - WebSocket-kommunikation
 * - Berøringssensor til tælling af events
 * - SPIFFS til filhåndtering af Wi-Fi-konfiguration og logning
 * - Dynamisk konfiguration via et Access Point
 * - LED-kontrol og reset-knap med forsinkelse
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <SPIFFS.h>
#include <Update.h>
#include <DNSServer.h>

// Webserver og WebSocket
AsyncWebServer server(80);        ///< Webserver-objekt på port 80
AsyncWebSocket ws("/ws");         ///< WebSocket endepunkt på "/ws"

// GPIO-konfiguration
const int resetPin = 4;           ///< Pin til reset-knap
unsigned long buttonPressTime = 0; ///< Tidsregistrering for knaptryk
const unsigned long resetDelay = 10000; ///< Forsinkelse til reset i ms

// Parametre til Wi-Fi-konfiguration
const char* PARAM_INPUT_1 = "ssid";       ///< Wi-Fi SSID
const char* PARAM_INPUT_2 = "pass";       ///< Wi-Fi password
const char* PARAM_INPUT_3 = "ip";         ///< Statisk IP-adresse
const char* PARAM_INPUT_4 = "gateway";    ///< Gateway-adresse

String ssid;           ///< Gemt Wi-Fi SSID
String pass;           ///< Gemt Wi-Fi password
String ip;             ///< Gemt IP-adresse
String gateway;        ///< Gemt gateway-adresse

// Filstier til SPIFFS
const char* ssidPath = "/ssid.txt";       ///< Filsti til SSID
const char* passPath = "/pass.txt";       ///< Filsti til password
const char* ipPath = "/ip.txt";           ///< Filsti til IP-adresse
const char* gatewayPath = "/gateway.txt"; ///< Filsti til gateway-adresse

unsigned long previousMillis = 0;         ///< Timer til Wi-Fi timeout
const long interval = 10000;              ///< Timeout-interval for Wi-Fi (ms)

const int ledPin = 2;                     ///< GPIO til LED
String ledState;                          ///< Status for LED

// Touch sensor
const int touchPin = 15;                  ///< GPIO til berøringssensor
int counter = 0;                          ///< Tæller for touch events
unsigned long lastTouchTime = 0;          ///< Tidspunkt for sidste touch event
const unsigned long debounceTime = 500;   ///< Debounce-tid (ms)

// Datalog fil
const char* logFilePath = "/log.txt";     ///< Filsti til logfil

/**
 * @brief Initialiserer SPIFFS.
 */
void initSPIFFS() {
  if (!SPIFFS.begin(true)) {
    Serial.println("An error has occurred while mounting SPIFFS");
  }
  Serial.println("SPIFFS mounted successfully");
}

/**
 * @brief Læser indholdet af en fil fra SPIFFS.
 * @param fs Filesystem objektet (SPIFFS)
 * @param path Stien til filen
 * @return Indholdet af filen som en streng
 */
String readFile(fs::FS &fs, const char * path){
  Serial.printf("Reading file: %s\r\n", path);
  File file = fs.open(path);
  if(!file || file.isDirectory()){
    Serial.println("- failed to open file for reading");
    return String();
  }
  String fileContent;
  while(file.available()){
    fileContent = file.readStringUntil('\n');
    break;     
  }
  return fileContent;
}

/**
 * @brief Skriver data til en fil på SPIFFS.
 * @param fs Filesystem objektet
 * @param path Stien til filen
 * @param message Data, der skal skrives
 */
void writeFile(fs::FS &fs, const char * path, const char * message){
  Serial.printf("Writing file: %s\r\n", path);
  File file = fs.open(path, FILE_WRITE);
  if(!file){
    Serial.println("- failed to open file for writing");
    return;
  }
  if(file.print(message)){
    Serial.println("- file written");
  } else {
    Serial.println("- write failed");
  }
}

/**
 * @brief Logger data til en fil og sender over WebSocket.
 * @param data Den data, der skal logges
 */
void logData(String data) {
  File file = SPIFFS.open(logFilePath, FILE_APPEND);
  if (file) {
    file.println(data);
    file.close();
    Serial.println("Data logged: " + data);
  } else {
    Serial.println("Failed to open log file");
  }
  ws.textAll(data);
}

/**
 * @brief Sender tællerværdien over WebSocket.
 */
void sendCounter() {
  String message = "counter:" + String(counter);
  ws.textAll(message);
}

/**
 * @brief Håndterer berøringsinput med debouncing.
 */
void handleTouch() {
  unsigned long now = millis();
  if (touchRead(touchPin) < 40 && (now - lastTouchTime > debounceTime)) {
    counter++;
    sendCounter();
    lastTouchTime = now;
  }
}

/**
 * @brief Initialiserer Wi-Fi-forbindelsen.
 * @return True hvis forbindelse lykkes, ellers false.
 */
bool initWiFi() {
  if(ssid=="" || ip==""){
    Serial.println("Undefined SSID or IP address.");
    return false;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.println("Connecting to WiFi...");

  unsigned long currentMillis = millis();
  previousMillis = currentMillis;

  while(WiFi.status() != WL_CONNECTED) {
    currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      Serial.println("Failed to connect.");
      return false;
    }
  }

  Serial.println("Connected to Wi-Fi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  return true;
}

/**
 * @brief Returnerer dynamisk variabel til HTML-siderne.
 * @param var Navnet på variablen
 * @return Værdien af variablen som en streng
 */
String processor(const String& var) {
  if(var == "STATE") {
    ledState = digitalRead(ledPin) ? "ON" : "OFF";
    return ledState;
  }
  return String();
}

/**
 * @brief Håndterer modtagne WebSocket-beskeder.
 * @param client WebSocket klient
 * @param message Beskeden fra klienten
 */
void onWebSocketMessage(AsyncWebSocketClient *client, String message) {
  Serial.println("WebSocket Message: " + message);
  if (message == "clear_measurements") {
    if (SPIFFS.remove(logFilePath)) {
      client->text("Måleværdier slettet.");
    } else {
      client->text("Kunne ikke slette måleværdier.");
    }
  } else if (message == "clear_configuration") {
    bool success = SPIFFS.remove(ssidPath) && SPIFFS.remove(passPath) && SPIFFS.remove(ipPath) && SPIFFS.remove(gatewayPath);
    client->text(success ? "Konfiguration slettet." : "Kunne ikke slette konfiguration.");
  }
}

/**
 * @brief Setup-funktion til initialisering af systemet.
 */
void setup() {
  Serial.begin(115200);
  initSPIFFS();

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  pinMode(resetPin, INPUT_PULLUP);
  pinMode(touchPin, INPUT);
  
  ssid = readFile(SPIFFS, ssidPath);
  pass = readFile(SPIFFS, passPath);
  ip = readFile(SPIFFS, ipPath);
  gateway = readFile(SPIFFS, gatewayPath);

  if(initWiFi()) {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send(SPIFFS, "/index.html", "text/html", false, processor);
    });
    server.serveStatic("/", SPIFFS, "/");

    server.on("/on", HTTP_GET, [](AsyncWebServerRequest *request) {
      digitalWrite(ledPin, HIGH);
      logData("LED ON");
      request->send(SPIFFS, "/index.html", "text/html", false, processor);
    });

    server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request) {
      digitalWrite(ledPin, LOW);
      logData("LED OFF");
      request->send(SPIFFS, "/index.html", "text/html", false, processor);
    });
    
    ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
      if(type == WS_EVT_DATA) {
        onWebSocketMessage(client, String((char*)data).substring(0, len));
      }
    });
    server.addHandler(&ws);
    
    server.begin();
  } else {
    WiFi.softAP("ESP-WIFI-MANAGER-Darab", NULL);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
      request->send(SPIFFS, "/wifimanager.html", "text/html");
    });
    server.serveStatic("/", SPIFFS, "/");
    
    server.on("/", HTTP_POST, [](AsyncWebServerRequest *request) {
      int params = request->params();
      for(int i = 0; i < params; i++){
        const AsyncWebParameter* p = request->getParam(i);
        if(p->isPost()){
          if (p->name() == PARAM_INPUT_1) {
            ssid = p->value().c_str();
            writeFile(SPIFFS, ssidPath, ssid.c_str());
          }
          if (p->name() == PARAM_INPUT_2) {
            pass = p->value().c_str();
            writeFile(SPIFFS, passPath, pass.c_str());
          }
          if (p->name() == PARAM_INPUT_3) {
            ip = p->value().c_str();
            writeFile(SPIFFS, ipPath, ip.c_str());
          }
          if (p->name() == PARAM_INPUT_4) {
            gateway = p->value().c_str();
            writeFile(SPIFFS, gatewayPath, gateway.c_str());
          }
        }
      }
      request->send(200, "text/plain", "Done. ESP will restart.");
      delay(3000);
      ESP.restart();
    });
    server.begin();
  }
}

/**
 * @brief Hoved-løkken som kontrollerer touch, reset og WebSocket-klienter.
 */
void loop() {
  if (digitalRead(resetPin) == LOW) {
    if (buttonPressTime == 0) {
      buttonPressTime = millis();
    }
    if (millis() - buttonPressTime > resetDelay) {
      SPIFFS.format();
      ESP.restart();
    }
  } else {
    buttonPressTime = 0;
  }
  handleTouch();
  ws.cleanupClients();
}

// #include <Arduino.h>
// #include <WiFi.h>
// #include <ESPAsyncWebServer.h>
// #include <AsyncTCP.h>
// #include <SPIFFS.h>
// #include <Update.h>
// #include <DNSServer.h>

// const int resetPin = 4;  // GPIO 4 til reset-knappen
// unsigned long buttonPressTime = 0;
// const unsigned long resetDelay = 10000;  // 10 sekunder

// // Create AsyncWebServer object on port 80
// AsyncWebServer server(80);
// AsyncWebSocket ws("/ws");

// // Search for parameter in HTTP POST request
// const char* PARAM_INPUT_1 = "ssid";
// const char* PARAM_INPUT_2 = "pass";
// const char* PARAM_INPUT_3 = "ip";
// const char* PARAM_INPUT_4 = "gateway";

// //Variables to save values from HTML form
// String ssid;
// String pass;
// String ip;
// String gateway;

// // File paths to save input values permanently
// const char* ssidPath = "/ssid.txt";
// const char* passPath = "/pass.txt";
// const char* ipPath = "/ip.txt";
// const char* gatewayPath = "/gateway.txt";

// // Timer variables
// unsigned long previousMillis = 0;
// const long interval = 10000;  // interval to wait for Wi-Fi connection (milliseconds)

// // Set LED GPIO
// const int ledPin = 2;
// // Stores LED state
// String ledState;

// const char* logFilePath = "/log.txt";

// // Initialize SPIFFS
// void initSPIFFS() {
//   if (!SPIFFS.begin(true)) {
//     Serial.println("An error has occurred while mounting SPIFFS");
//   }
//   Serial.println("SPIFFS mounted successfully");
// }

// // Read File from SPIFFS
// String readFile(fs::FS &fs, const char * path){
//   Serial.printf("Reading file: %s\r\n", path);

//   File file = fs.open(path);
//   if(!file || file.isDirectory()){
//     Serial.println("- failed to open file for reading");
//     return String();
//   }
  
//   String fileContent;
//   while(file.available()){
//     fileContent = file.readStringUntil('\n');
//     break;     
//   }
//   return fileContent;
// }

// // Write file to SPIFFS
// void writeFile(fs::FS &fs, const char * path, const char * message){
//   Serial.printf("Writing file: %s\r\n", path);

//   File file = fs.open(path, FILE_WRITE);
//   if(!file){
//     Serial.println("- failed to open file for writing");
//     return;
//   }
//   if(file.print(message)){
//     Serial.println("- file written");
//   } else {
//     Serial.println("- write failed");
//   }
// }

// // Log data til fil og send over WebSocket
// void logData(String data) {
//   // Log til fil
//   File file = SPIFFS.open(logFilePath, FILE_APPEND);
//   if (file) {
//     file.println(data);
//     file.close();
//     Serial.println("Data logged: " + data);
//   } else {
//     Serial.println("Failed to open log file");
//   }
//   ws.textAll(data);
// }

// // Initialize WiFi
// bool initWiFi() {
//   if(ssid=="" || ip==""){
//     Serial.println("Undefined SSID or IP address.");
//     return false;
//   }

//   WiFi.mode(WIFI_STA);
//   WiFi.begin(ssid.c_str(), pass.c_str());  // Just start Wi-Fi connection without configuring IP manually
//   Serial.println("Connecting to WiFi...");

//   unsigned long currentMillis = millis();
//   previousMillis = currentMillis;

//   while(WiFi.status() != WL_CONNECTED) {
//     currentMillis = millis();
//     if (currentMillis - previousMillis >= interval) {
//       Serial.println("Failed to connect.");
//       return false;
//     }
//   }

//   Serial.println("Connected to Wi-Fi");
//   Serial.print("IP Address: ");
//   Serial.println(WiFi.localIP());  // Get the IP assigned by DHCP
//   return true;
// }

// // Replaces placeholder with LED state value
// String processor(const String& var) {
//   if(var == "STATE") {
//     if(digitalRead(ledPin)) {
//       ledState = "ON";
//     }
//     else {
//       ledState = "OFF";
//     }
//     return ledState;
//   }
//   return String();
// }

// void onWebSocketMessage(AsyncWebSocketClient *client, String message) {
//   Serial.println("WebSocket Message: " + message);
// }

// void setup() {
//   // Serial port for debugging purposes
//   Serial.begin(115200);

//   initSPIFFS();

//   // Set GPIO 2 as an OUTPUT
//   pinMode(ledPin, OUTPUT);
//   digitalWrite(ledPin, LOW);

//   pinMode(resetPin, INPUT_PULLUP);
  
//   // Load values saved in SPIFFS
//   ssid = readFile(SPIFFS, ssidPath);
//   pass = readFile(SPIFFS, passPath);
//   ip = readFile(SPIFFS, ipPath);
//   gateway = readFile(SPIFFS, gatewayPath);
//   Serial.println(ssid);
//   Serial.println(pass);
//   Serial.println(ip);
//   Serial.println(gateway);

//   if(initWiFi()) {
//     // Route for root / web page
//     server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
//       request->send(SPIFFS, "/index.html", "text/html", false, processor);
//     });
//     server.serveStatic("/", SPIFFS, "/");
    
//     // Route to set GPIO state to HIGH
//     server.on("/on", HTTP_GET, [](AsyncWebServerRequest *request) {
//       digitalWrite(ledPin, HIGH);
//       request->send(SPIFFS, "/index.html", "text/html", false, processor);
//     });

//     // Route to set GPIO state to LOW
//     server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request) {
//       digitalWrite(ledPin, LOW);
//       request->send(SPIFFS, "/index.html", "text/html", false, processor);
//     });
//     server.begin();
//   }
//   else {
//     // Connect to Wi-Fi network with SSID and password
//     Serial.println("Setting AP (Access Point)");
//     // NULL sets an open Access Point
//     WiFi.softAP("ESP-WIFI-MANAGER-Darab", NULL);

//     IPAddress IP = WiFi.softAPIP();
//     Serial.print("AP IP address: ");
//     Serial.println(IP); 

//     // Web Server Root URL
//     server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
//       request->send(SPIFFS, "/wifimanager.html", "text/html");
//     });
    
//     server.serveStatic("/", SPIFFS, "/");
    
//     server.on("/", HTTP_POST, [](AsyncWebServerRequest *request) {
//       int params = request->params();
//       for(int i=0;i<params;i++){
//         const AsyncWebParameter* p = request->getParam(i);
//         if(p->isPost()){
//           // HTTP POST ssid value
//           if (p->name() == PARAM_INPUT_1) {
//             ssid = p->value().c_str();
//             Serial.print("SSID set to: ");
//             Serial.println(ssid);
//             // Write file to save value
//             writeFile(SPIFFS, ssidPath, ssid.c_str());
//           }
//           // HTTP POST pass value
//           if (p->name() == PARAM_INPUT_2) {
//             pass = p->value().c_str();
//             Serial.print("Password set to: ");
//             Serial.println(pass);
//             // Write file to save value
//             writeFile(SPIFFS, passPath, pass.c_str());
//           }
//           // HTTP POST ip value
//           if (p->name() == PARAM_INPUT_3) {
//             ip = p->value().c_str();
//             Serial.print("IP Address set to: ");
//             Serial.println(ip);
//             // Write file to save value
//             writeFile(SPIFFS, ipPath, ip.c_str());
//           }
//           // HTTP POST gateway value
//           if (p->name() == PARAM_INPUT_4) {
//             gateway = p->value().c_str();
//             Serial.print("Gateway set to: ");
//             Serial.println(gateway);
//             // Write file to save value
//             writeFile(SPIFFS, gatewayPath, gateway.c_str());
//           }
//         }
//       }
//       request->send(200, "text/plain", "Done. ESP will restart, connect to your router and go to IP address: " + ip);
//       delay(3000);
//       ESP.restart();
//     });
//     server.begin();
//   }
// }

// void loop() {
//     // Reset-funktion: Tjek om reset-knappen (GPIO 0) er trykket
//   if (digitalRead(resetPin) == LOW) {
//     if (buttonPressTime == 0) {
//       buttonPressTime = millis();  // Start timeren
//     }
//     if (millis() - buttonPressTime > resetDelay) {
//       // Hvis knappen er trykket i 10 sekunder, slet SPIFFS og genstart ESP32
//       Serial.println("Resetting and formatting SPIFFS...");
//       SPIFFS.format();  // Slet SPIFFS filsystem
//       ESP.restart();    // Genstart ESP32
//     }
//   } else {
//     buttonPressTime = 0;  // Nulstil timeren, hvis knappen ikke er trykket
//   }
// }

// #include <WiFi.h>
// #include <WiFiManager.h>
// #include <ESPAsyncWebServer.h>
// #include <AsyncTCP.h>
// #include <SPIFFS.h>
// #include <WiFiUdp.h>
// #include <NTPClient.h>

// #define RESET_PIN 4  // GPIO-pin til reset-knap
// volatile bool resetFlag = false;

// AsyncWebServer server(80);
// AsyncWebSocket ws("/ws");
// WiFiUDP ntpUDP;
// NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000);

// // Interrupt funktion til reset-knappen
// void IRAM_ATTR handleInterrupt() {
//   resetFlag = true;
// }

// // WebSocket event handler
// void setupWebSocket() {
//     ws.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
//         if (type == WS_EVT_CONNECT) {
//             client->text("Connected to Energy Monitor");
//         }
//     });
//     server.addHandler(&ws);
// }

// // Gem data til SPIFFS
// void saveData(String data) {
//   File file = SPIFFS.open("/data.txt", FILE_APPEND);
//   if (file) {
//     file.println(data);
//     file.close();
//   } else {
//     Serial.println("Failed to open file for writing");
//   }
// }

// // Send energidata til klienten
// void sendData() {
//     String timeStamp = timeClient.getFormattedTime();
//     String dataString = "Time: " + timeStamp + " - Power: " + String(random(100, 500)) + "W";  // Eksempel på data
//     ws.textAll(dataString);
//     saveData(dataString); // Log data til SPIFFS
// }

// // Setup-funktionen
// void setup() {
//     Serial.begin(115200);
//     pinMode(RESET_PIN, INPUT_PULLUP);
//     attachInterrupt(digitalPinToInterrupt(RESET_PIN), handleInterrupt, FALLING);

//     // Initialiser SPIFFS
//     if (!SPIFFS.begin(true)) {
//         Serial.println("SPIFFS mount failed");
//         return;
//     }

//     // Wi-Fi opsætning med Wi-Fi Manager
//     WiFiManager wm;
//     if (!wm.autoConnect("EnergyMonitorAP")) {
//         Serial.println("WiFi connection failed");
//         ESP.restart();
//     }
//     Serial.print("Connected to WiFi with IP: ");
//     Serial.println(WiFi.localIP());

//     // NTP setup for tidsstempler
//     timeClient.begin();
//     timeClient.update();

//     // WebServer og WebSocket opsætning
//     setupWebSocket();
//     server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
//         request->send(SPIFFS, "/index.html", "text/html");
//     });

//     server.begin();
// }

// void loop() {
//     if (resetFlag) {
//         Serial.println("Reset triggered!");
//         SPIFFS.format();  // Slet lagret data
//         ESP.restart();
//     }

//     timeClient.update();  // Opdater NTP-tid
//     sendData();           // Send data via WebSocket hvert sekund
//     delay(1000);
//     ws.cleanupClients();
// }