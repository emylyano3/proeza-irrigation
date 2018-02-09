#include <FS.h>                   //this needs to be first, or it all crashes and burns...

#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino

#include <ESP8266WebServer.h>
#include <WiFiManager.h>

#include <WiFiClient.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <DHT.h>

#include <PubSubClient.h>
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

#define PARAM_LENGTH 15

const char* CONFIG_FILE     = "/config.json";
const uint8_t MAX_IRR_LINES = 5;

struct ControlStruct {
  uint8_t irrLinesState[MAX_IRR_LINES];
  uint8_t pumpState;
  uint8_t irrLinesPins[MAX_IRR_LINES];
  uint8_t pumpPin;
  uint8_t sensorPin;
} control = {
  {0,0,0,0,0},
  0,
#ifdef NODEMCUV2
  {D2,D3,D4,D5,D12},
  D13,
  D14
#else
  {2,3,4,5,12},
  13,
  14
#endif
};

// DH_TYPE must be defined via build flags
DHT sensor(control.sensorPin, DH_TYPE, 16);

char stationName[PARAM_LENGTH * 3 + 4];
char topicBase[PARAM_LENGTH * 3 + 4];

WiFiClient espClient;
PubSubClient mqttClient(espClient);

ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

long nextBrokerConnAtte = 0;

WiFiManagerParameter mqttServerParam("server", "MQTT Server", "192.168.0.105", 16);
WiFiManagerParameter mqttPortParam("port", "MQTT Port", "1", 6);
WiFiManagerParameter locationParam("location", "Module location", "room", PARAM_LENGTH);
WiFiManagerParameter typeParam("type", "Module type", "light", PARAM_LENGTH);
WiFiManagerParameter nameParam("name", "Module name", "ceiling", PARAM_LENGTH);

template <class T> void log (T text) {
  if (LOGGING) {
    Serial.print("*SW: ");
    Serial.println(text);
  }
}

template <class T, class U> void log (T key, U value) {
  if (LOGGING) {
    Serial.print("*SW: ");
    Serial.print(key);
    Serial.print(": ");
    Serial.println(value);
  }
}
void setup() {
  Serial.begin(115200);
  // Pin settings
  for (uint8_t i = 0; i < sizeof(control.irrLinesPins); ++i) {
    pinMode(control.irrLinesPins[i], OUTPUT);
  }
  pinMode(control.pumpPin, OUTPUT);
  pinMode(control.sensorPin, INPUT);
  
  // Load module config
  loadConfig();

  // WiFi Manager Config  
  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setStationNameCallback(buildStationName);
  wifiManager.setMinimumSignalQuality(30);
  wifiManager.addParameter(&mqttServerParam);
  wifiManager.addParameter(&mqttPortParam);
  wifiManager.addParameter(&locationParam);
  wifiManager.addParameter(&typeParam);
  wifiManager.addParameter(&nameParam);
  if (!wifiManager.autoConnect(("ESP_" + String(ESP.getChipId())).c_str(), "12345678")) {
    log(F("Failed to connect and hit timeout"));
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }
  log(F("Connected to wifi network. Local IP"), WiFi.localIP());
  log(F("Configuring MQTT broker"));
  String port = String(mqttPortParam.getValue());
  log(F("Port"), port);
  log(F("Server"), mqttServerParam.getValue());
  mqttClient.setServer(mqttServerParam.getValue(), (uint16_t) port.toInt());
  mqttClient.setCallback(mqttCallback);
  
  // Pin settings
  for (uint8_t i = 0; i < sizeof(control.irrLinesPins); ++i) {
    pinMode(control.irrLinesPins[i], OUTPUT);
  }
  pinMode(control.pumpPin, OUTPUT);
  pinMode(control.sensorPin, INPUT);

  // Building topics base
  String buff = String(locationParam.getValue()) + String(F("/")) + String(typeParam.getValue()) + String(F("/")) + String(nameParam.getValue()) + String(F("/"));
  buff.toCharArray(topicBase, buff.length() + 1);
  log(F("Topics Base"), topicBase);

  // OTA Update Stuff
  WiFi.mode(WIFI_AP_STA);
  MDNS.begin(getStationName());
  httpUpdater.setup(&httpServer);
  httpServer.begin();
  MDNS.addService("http", "tcp", 80);
  Serial.print(F("HTTPUpdateServer ready! Open http://"));
  Serial.print(WiFi.localIP().toString());
  Serial.println(F("/update in your browser"));
}

void loop() {
  httpServer.handleClient();
  if (!mqttClient.connected()) {
    connectBroker();
  }
  mqttClient.loop();
}

void connectBroker() {
  if (nextBrokerConnAtte <= millis()) {
    nextBrokerConnAtte = millis() + 5000;
    log(F("Connecting MQTT broker as"), getStationName());
    if (mqttClient.connect(getStationName())) {
      log(F("Connected"));
    } else {
      //TODO Suscribe to topics
      log(F("Failed. RC:"), mqttClient.state());
    }
  }
}

void loadConfig() {
//read configuration from FS json
  if (SPIFFS.begin()) {
    if (SPIFFS.exists(CONFIG_FILE)) {
      //file exists, reading and loading
      File configFile = SPIFFS.open(CONFIG_FILE, "r");
      if (configFile) {
        size_t size = configFile.size();
        if (size > 0) {
          // Allocate a buffer to store contents of the file.
          std::unique_ptr<char[]> buf(new char[size]);
          configFile.readBytes(buf.get(), size);
          DynamicJsonBuffer jsonBuffer;
          JsonObject& json = jsonBuffer.parseObject(buf.get());
          json.printTo(Serial);
          if (json.success()) {
            mqttServerParam.update(json["mqtt_server"]);
            mqttPortParam.update(json["mqtt_port"]);
            nameParam.update(json["name"]);
            locationParam.update(json["location"]);
            typeParam.update(json["type"]);
          } else {
            log(F("Failed to load json config"));
          }
        } else {
          log(F("Config file empty"));
        }
      } else {
        log(F("No config file found"));
      }
    } else {
      log(F("No config file found"));
    }
  } else {
    log(F("Failed to mount FS"));
  }
}

void mqttCallback(char* topic, unsigned char* payload, unsigned int length) {
  log(F("mqtt message"), topic);
  // TODO Process callbacks
}

/** callback notifying the need to save config */
void saveConfigCallback () {
  DynamicJsonBuffer jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["mqtt_server"] = mqttServerParam.getValue();
  json["mqtt_port"] = mqttPortParam.getValue();
  json["name"] = nameParam.getValue();
  json["location"] = locationParam.getValue();
  json["type"] = typeParam.getValue();
  File configFile = SPIFFS.open(CONFIG_FILE, "w");
  if (configFile) {
    //json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
  } else {
    log(F("Failed to open config file for writing"));
  }
}

void hardReset () {
  log(F("Doing a module hard reset"));
  SPIFFS.format();
  delay(200);
  reset();
}

void reset () {
  log(F("Reseting module configuration"));
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  delay(200);
  restart();
}

void restart () {
  log(F("Restarting module"));
  ESP.restart();
  delay(2000);
}

uint8_t getTopicLength(const char* wich) {
  return strlen(topicBase) + strlen(wich);
}

char* getTopic(char* topic, const char* wich) {
  String buff = topicBase + String(wich);
  buff.toCharArray(topic, buff.length() + 1);
  return topic;
}

char* buildStationName () {
  String buff = String(locationParam.getValue()) + String(F("_")) + String(typeParam.getValue()) + String(F("_")) + String(nameParam.getValue());
  buff.toCharArray(stationName, buff.length() + 1);
  log(F("Station name"), stationName);
  return stationName;
}

char* getStationName () {
  return !stationName || strlen(stationName) == 0 ? buildStationName() : stationName;
}