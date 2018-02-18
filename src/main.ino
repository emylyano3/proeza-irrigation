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

#define PARAM_LENGTH    15
#define MILLIS          1000

const char* CONFIG_FILE     = "/config.json";

struct Controllable {
  const char* name;
  uint8_t state;
  uint8_t pin;
};

struct Readable {
  const char* name;
  uint8_t state;
  uint8_t pin;
};

struct ControlStruct {
  bool running;
  bool paused;
  uint16_t elapsedTime; // used just when paused
  uint8_t currentLine;
  uint16_t irrTime; // in seconds
  long irrLineStartTime; // in millis
  struct Readable sensor;
  struct Controllable irrLines[IRR_LINES_COUNT];
  struct Controllable pump;
  struct Readable auxLine;
} ctrl = {
  false,  // not running
  false,  // not paused
  0,      // 0 sec elapesd
  0,      // current is first line
  DEFAULT_IRR_TIME, // defined through build flags
  0,      // means not started by default
#ifdef NODEMCUV2
  {"Sensor DHT22",0,D0},
  {{"Línea 1",0,D1},{"Línea 2",0,D2},{"Línea 3",0,D3},{"Línea 4",0,D4},{"Línea 5",0,D5}},
  {"Bomba sumergible",0,D6},
  {"Linea sisterna",0,D7}
#else
  {"Sensor DHT22",0,14},
  {{"Línea 1",0,2},{"Línea 2",0,3},{"Línea 3",0,4},{"Línea 4",0,5},{"Línea 5",0,12}},
  {"Bomba sumergible",0,13},
  {"Linea sisterna",0,15}
#endif
};

template <class T> void log (T text) {
  if (LOGGING) {
    Serial.print("*IRR: ");
    Serial.println(text);
  }
}

template <class T, class U> void log (T key, U value) {
  if (LOGGING) {
    Serial.print("*IRR: ");
    Serial.print(key);
    Serial.print(": ");
    Serial.println(value);
  }
}

// DH_TYPE must be defined through build flags
DHT sensor(ctrl.sensor.pin, DH_TYPE, 16);

char stationName[PARAM_LENGTH * 3 + 4];
char topicBase[PARAM_LENGTH * 3 + 4];

WiFiClient espClient;
PubSubClient mqttClient(espClient);

ESP8266WebServer httpServer(80);
ESP8266HTTPUpdateServer httpUpdater;

long nextBrokerConnAtte = 0;

WiFiManagerParameter mqttServerParam("server", "MQTT Server", "192.168.0.105", 16);
WiFiManagerParameter mqttPortParam("port", "MQTT Port", "1883", 6);
WiFiManagerParameter locationParam("location", "Module location", "room", PARAM_LENGTH);
WiFiManagerParameter typeParam("type", "Module type", "light", PARAM_LENGTH);
WiFiManagerParameter nameParam("name", "Module name", "ceiling", PARAM_LENGTH);

void setup() {
  Serial.begin(115200);
  // Pin settings
  for (int i = 0; i < (IRR_LINES_COUNT - 1) ; ++i) {
    pinMode(ctrl.irrLines[i].pin, OUTPUT);
  }
  pinMode(ctrl.pump.pin, OUTPUT);
  pinMode(ctrl.sensor.pin, INPUT);
  
  // Load module config
  loadConfig();
  
  // WiFi Manager Config
  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  wifiManager.setStationNameCallback(buildStationName);
  wifiManager.setMinimumSignalQuality(30);
  wifiManager.setConnectTimeout(WIFI_CONN_TIMEOUT);
  wifiManager.setMaxConnRetries(WIFI_CONN_RETRIES);
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
  
  // Building topics base
  String buff = String(locationParam.getValue()) + String(F("/")) + String(typeParam.getValue()) + String(F("/")) + String(nameParam.getValue()) + String(F("/"));
  buff.toCharArray(topicBase, buff.length() + 1);
  log(F("Topics Base"), topicBase);

  // OTA Update Stuff
  WiFi.mode(WIFI_STA);
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
  checkSequence();
  mqttClient.loop();
}

void checkSequence() {
  if (ctrl.running) {
    if (ctrl.irrLineStartTime + ctrl.irrTime * MILLIS < millis()) {
      if (ctrl.currentLine == IRR_LINES_COUNT - 1) {
        endSequence();
      } else {
        startNextLine();
      }
    }
  } else {
    // Just to be safe that the pump is not running
    if (ctrl.pump.state == HIGH) {
      setState(ctrl.pump, LOW);
    }
  }
}

void startSequence() {
  if (!ctrl.running) {
    log(F("Starting irrigation sequence"));
    ctrl.running = true;
    ctrl.irrLineStartTime = millis();
    log(F("Opening valve"), getCurrentLine().name);
    setState(getCurrentLine(), HIGH);
    delay(200);
    log(F("Starting"), ctrl.pump.name);
    setState(ctrl.pump, HIGH);
  } else {
    log(F("Irrigation not started, it was already running"));
  }
}

void restartSequence() {
  if (!ctrl.running) {
    log(F("Restarting irrigation sequence at line"), getCurrentLine().name);
    ctrl.running = true;
    ctrl.paused = false;
    ctrl.irrLineStartTime = millis() - ctrl.elapsedTime;
    ctrl.elapsedTime = 0;
    log(F("Opening valve"), getCurrentLine().name);
    setState(getCurrentLine(), HIGH);
    delay(200);
    log(F("Restarting"), ctrl.pump.name);
    setState(ctrl.pump, HIGH);
  } else {
    log(F("Irrigation not started, it was already running"));
  }
}

void endSequence() {
  if (ctrl.running || ctrl.paused) {
    log(F("Ending irrigation sequence"));
    log(F("Stoping"), ctrl.pump.name);
    setState(ctrl.pump, LOW);
    // delay to wait for system presure to go down
    delay(PUMP_DELAY);
    log(F("Closing valve"), getCurrentLine().name);
    setState(getCurrentLine(), LOW);
    ctrl.running = false;
    ctrl.paused = false;
    ctrl.currentLine = 0;
    ctrl.irrLineStartTime = 0;
  } else {
    log(F("Irrigation is not running nor paused"));
  }
}

void pauseSequence() {
   if (ctrl.running) {
    log(F("Pausing irrigation sequence"));
    log(F("Stoping"), ctrl.pump.name);
    ctrl.elapsedTime = millis() - ctrl.irrLineStartTime;
    setState(ctrl.pump, LOW);
    // delay to wait for system presure to go down
    delay(PUMP_DELAY);
    log(F("Closing valve"), getCurrentLine().name);
    setState(getCurrentLine(), LOW);
    ctrl.running = false;
    ctrl.paused = true;
   } else {
    log(F("Irrigation is not running"));
  }
}

void startNextLine () {
  uint8_t prevLine = ctrl.currentLine++;
  log(F("Opening valve"), getCurrentLine().name);
  setState(getCurrentLine(), HIGH);
  ctrl.irrLineStartTime = millis();
  // delay to wait for actuators
  delay(ACTUATOR_DELAY);
  log(F("Closing valve"), ctrl.irrLines[prevLine].name);
  setState(ctrl.irrLines[prevLine], LOW);
}

Controllable getCurrentLine () {
  return ctrl.irrLines[ctrl.currentLine];
}

void setState (Controllable c, uint8_t state) {
  c.state = state;
}

void connectBroker() {
  if (nextBrokerConnAtte <= millis()) {
    nextBrokerConnAtte = millis() + 5000;
    log(F("Connecting MQTT broker as"), getStationName());
    if (mqttClient.connect(getStationName())) {
      log(F("Connected"));
      mqttClient.subscribe(getTopic(new char[getTopicLength("cmd")], "cmd"));
      mqttClient.subscribe(getTopic(new char[getTopicLength("hrst")], "hrst"));
      mqttClient.subscribe(getTopic(new char[getTopicLength("echo")] , "echo"));
    } else {
      log(F("Failed. RC"), mqttClient.state());
    }
  }
}

void mqttCallback(char* topic, unsigned char* payload, unsigned int length) {
  log(F("mqtt message"), topic);
  if (String(topic).equals(getTopic(new char[getTopicLength("cmd")], "cmd"))) {
    processCommand(payload, length);
  } else if (String(topic).equals(String(getTopic(new char[getTopicLength("hrst")], "hrst")))) {
    hardReset();
  } else if (String(topic).equals(String(getTopic(new char[getTopicLength("echo")], "echo")))) {
    publishState();
  } else {
    log(F("Unknown topic"));
  }
}

void processCommand (unsigned char* payload, unsigned int length) {
  String cmd = getCommand(payload, length);
  log(F("Command received"), cmd);
  cmd.toLowerCase();
  if (cmd.equals("start")) {
    if (ctrl.paused) {
      restartSequence();
    } else {
      startSequence();
    }
  } else if (cmd.equals("stop")) {
    endSequence();
  } else if (cmd.equals("pause")) {
    pauseSequence();
  }
}

String getCommand(unsigned char* payload, unsigned int length) {
  std::string val(reinterpret_cast<char*>(payload), length);
  return val.c_str();
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
    json.printTo(configFile);
    configFile.close();
  } else {
    log(F("Failed to open config file for writing"));
  }
}

void publishState () {
  mqttClient.publish(getTopic(new char[getTopicLength("state")], "state"), new char[2]{ctrl.running ? '1' : '0', '\0'});
}

void hardReset () {
  log(F("Doing a module hard reset"));
  SPIFFS.format();
  delay(200);
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  delay(200);
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
  String buff = String(typeParam.getValue()) + String(F("_")) + String(locationParam.getValue()) + String(F("_")) + String(nameParam.getValue());
  buff.toCharArray(stationName, buff.length() + 1);
  log(F("Station name"), stationName);
  return stationName;
}

char* getStationName () {
  return !stationName || strlen(stationName) == 0 ? buildStationName() : stationName;
}