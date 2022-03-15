#include <WiFi.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Preferences.h>

#include "HeatPump.h"
#include "esp32_mhk1_mqtt.h"

#ifdef OTA
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#endif

WiFiClient espClient;
PubSubClient mqtt_client(espClient);

HeatPump hp;

#ifdef CONFIG_IDF_TARGET_ESP32C3
HardwareSerial& heatpumpSerial = Serial;
HardwareSerial& mhk1 = Serial1;
#else
HardwareSerial& mhk1 = Serial2;
HardwareSerial& heatpumpSerial = Serial1;
#endif

#define HPSerial 0
#define MHK1Serial 1

const int SERIAL_READ_TIMEOUT = 250;
const int SERIAL_BUFFER_SIZE = 30;
byte mhk1ReadBuffer[SERIAL_BUFFER_SIZE];
byte hpReadBuffer[SERIAL_BUFFER_SIZE];
int numberOfConnectRequests = 0;

/**
 * If two sets come from MHK1 under this time then we reset the override flag.
 */
const int MHK1_RESET_OVERRIDE_PERIOD = 1 * 60000;
const int WIFI_RECONNECT_DELAY = 5*60000; // time to wait before retrying to connect to WiFi. Increase so that MHK1 doesn't lose connectivity often if you have unstable wifi

Preferences preferences;
bool _debugMode = false;
bool _debugToSerial = false;
bool wifiConnected = false;
bool hpConnected = false;
double remoteTemperature = 0.0;
bool remoteTemperatureSet = false;
long timeSinceLastWifiConnect = 0;
long timeSinceLastMqttConnect = 0;
long timeSinceRemoteTempSet = 0;

/**
 * When this flag is set we ignore changes to the settings from MHK1.
 * 
 * This can be set to false by sending two consecutive sets from MHK1 under MHK1_RESET_OVERRIDE_PERIOD
 */
bool overrideGenericSettings = false;
long lastMhk1SetSettingPacket = -1;

bool overrideGenericSettings_NewSettings = false;
bool sendGetFunctions = false;
bool sendSetFunctions = false;
int setFunctionCodes[MAX_FUNCTION_CODE_COUNT];
int setFunctionValues[MAX_FUNCTION_CODE_COUNT];

long lastGetFreqTs = 0;
bool initialSettingsReadFromHP = false;

void(* resetFunc) (void) = 0;

#ifdef CONFIG_IDF_TARGET_ESP32C3
// RGB colors for ESP-C3-32s
const int RED_LAMP = 3; // No packets from MHK1
const int GREEN_LAMP = 4; // Settings Override
const int BLUE_LAMP = 5; // No packets from HP
const int WARM_LAMP = 18; // WIFI Connected
const int COOL_LAMP = 19; // HP Connected
bool lampVals[COOL_LAMP + 1];
#endif

long timeSinceLastPacketFromMHK1 = 0;
long timeSinceLastPacketFromHP = 0;


const int PACKET_LEN = 22;
const int IDX_RSP_SETTINGS = 0;
const int IDX_RSP_STANDBY = 1;
const int IDX_RSP_ROOM_TEMP = 2;
const int IDX_RSP_ERRORS = 3;
const int IDX_RSP_STATUS = 4;
byte cachedResponses[5][PACKET_LEN];

enum InjectionStates {
  State_Idle, 
  State_GetFreq, 
  State_GetFreqResponse, 
  State_SetSettings, 
  State_SetSettingsResponse, 
  State_GetFunctions1, 
  State_GetFunctions1Response, 
  State_GetFunctions2, 
  State_GetFunctions2Response,
  State_SetFunctions1,
  State_SetFunctions1Response,
  State_SetFunctions2,
  State_SetFunctions2Response,
};

InjectionStates currentState = State_Idle;
int injectionReseponseCacheIdx = -1;

void initLamps() {
#ifdef CONFIG_IDF_TARGET_ESP32C3
  pinMode(RED_LAMP, OUTPUT);
  pinMode(GREEN_LAMP, OUTPUT);
  pinMode(BLUE_LAMP, OUTPUT);
  pinMode(WARM_LAMP, OUTPUT);
  pinMode(COOL_LAMP, OUTPUT);
  
  digitalWrite(RED_LAMP, LOW);
  digitalWrite(GREEN_LAMP, LOW);
  digitalWrite(BLUE_LAMP, LOW);
  digitalWrite(WARM_LAMP, LOW);
  digitalWrite(COOL_LAMP, LOW);

  memset(lampVals, 0, sizeof(lampVals));
#endif
}

void setLamp(int const lampId, bool val) {
#ifdef CONFIG_IDF_TARGET_ESP32C3
  digitalWrite(lampId, val);
  lampVals[lampId] = val;
#endif
}

void flipLamp(int const lampId) {
#ifdef CONFIG_IDF_TARGET_ESP32C3
  digitalWrite(lampId, lampVals[lampId]);
  lampVals[lampId] = !lampVals[lampId];
#endif
}

void debugLog(const char* msg) {
  if (_debugMode) {
    if (_debugToSerial) {
      Serial.println(msg);
    }
    if (wifiConnected) {
      mqtt_client.publish(heatpump_debug_topic, msg);
    }
  }
}

void debugLog(const String& msg) {
  debugLog(msg.c_str());
}

void infoLog(const char* msg) {
  if (_debugToSerial) {
    Serial.println(msg);
  }
  if (wifiConnected) {
    mqtt_client.publish(heatpump_info_topic, msg);
  }
}

void infoLog(const String& msg) {
  infoLog(msg.c_str());
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      
      timeSinceLastMqttConnect = 0;
      mqtt_client.disconnect();
      delay(10);
    }
    return;
  }

  wifiConnected = false;

  if (timeSinceLastWifiConnect != 0 && millis() - timeSinceLastWifiConnect < WIFI_RECONNECT_DELAY)
    return;
    
  WiFi.disconnect();
  delay(100);
  
  WiFi.hostname(client_id);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int const flipDelay = 250;
  int const wifiConnectWaitTime = 30 * 1000;
  int tries = wifiConnectWaitTime / flipDelay;
  while (WiFi.status() != WL_CONNECTED && tries > 0) {
    flipLamp(WARM_LAMP);
    delay(flipDelay);
    tries -= 1;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    timeSinceLastMqttConnect = 0;
    mqtt_client.disconnect();
    delay(10);
    infoLog("Wifi Connected");
  }

  timeSinceLastWifiConnect = millis();
}

void setup() {
  if (_debugToSerial) {
    Serial.begin(115200);
  }
  infoLog("Starting up");
  
  initLamps();

  memset(cachedResponses, 0, sizeof(cachedResponses));

  infoLog("Connecting to Wifi...");

  connectWifi();
  
  // startup mqtt connection
  mqtt_client.setServer(mqtt_server, mqtt_port);
  mqtt_client.setCallback(mqttCallback);
  mqttConnect();

  preferences.begin("esp32-hp", false);
  if (!_debugMode) {
    _debugMode = preferences.getBool("debug", false);
  }
  
  hp.setSettingsChangedCallback(hpSettingsChanged);
  hp.setStatusChangedCallback(hpStatusChanged);
  publishOverrideStatus(true);

#ifdef OTA
  ArduinoOTA.setHostname(client_id);
  //ArduinoOTA.setPassword(ota_password);
  ArduinoOTA.begin();
  infoLog("OTA started");
#endif

  //heatpumpSerial.begin(2400, SERIAL_8E1, 16, 17);
  //mhk1.begin(2400, SERIAL_8E1, 32, 33);
  heatpumpSerial.begin(2400, SERIAL_8E1, 9, 10);
  mhk1.begin(2400, SERIAL_8E1, 1, 2);
  
  delay(2000);
  discardSerialBuffers();
}

void hpSettingsChanged(heatpumpSettings const& currentSettings) {
  const size_t bufferSize = JSON_OBJECT_SIZE(6);
  DynamicJsonDocument root(bufferSize);

  root["power"]       = currentSettings.power;
  root["mode"]        = strcmp(currentSettings.power, "OFF") == 0 ? "OFF" : strcmp(currentSettings.mode, "FAN") == 0 ? "FAN_ONLY" : currentSettings.mode;
  root["temperature"] = currentSettings.temperature;
  root["fan"]         = currentSettings.fan;
  root["vane"]        = currentSettings.vane;
  root["wideVane"]    = currentSettings.wideVane;
  //root["iSee"]        = currentSettings.iSee;

  char buffer[512];
  serializeJson(root, buffer);

  bool retain = true;
  if (!mqtt_client.publish(heatpump_topic, buffer, retain)) {
    infoLog("failed to publish to heatpump topic");
  }
}

void hpStatusChanged(heatpumpStatus const& currentStatus) {
  // send room temp and operating info
  const size_t bufferSizeInfo = JSON_OBJECT_SIZE(5);
  DynamicJsonDocument rootInfo(bufferSizeInfo);

  rootInfo["roomTemperature"]     = currentStatus.roomTemperature;
  rootInfo["operating"]           = currentStatus.operating;
  rootInfo["compressorFrequency"] = currentStatus.compressorFrequency;
  rootInfo["compressorState"]     = currentStatus.compressorState;
  rootInfo["fanMode"]             = currentStatus.fanMode;

  char bufferInfo[512];
  serializeJson(rootInfo, bufferInfo);

  if (!mqtt_client.publish(heatpump_status_topic, bufferInfo, true)) {
    infoLog("failed to publish to room temp and operation status to heatpump/status topic");
  }
}

void handleMqttSetTopic(const char* message) {
  if (!initialSettingsReadFromHP) {
    // needed so that the hp object is initialized properly
    infoLog("Cannot set anything unless settings are first read from HP");
    return;
  }
  
  const size_t bufferSize = JSON_OBJECT_SIZE(6);
  DynamicJsonDocument root(bufferSize);
  DeserializationError error = deserializeJson(root, message);

  if (error) {
    infoLog("!root.success(): invalid JSON on heatpump_set_topic...");
    return;
  }

  if (root.containsKey("mode")) {
    const char* mode = root["mode"];
    const char* convertedMode = strcmp(mode, "FAN_ONLY") == 0 ? "FAN" : mode;
    if (strcmp(mode, "OFF") == 0) {
      hp.setPowerSetting("OFF");
    } else {
      hp.setPowerSetting("ON");
      hp.setModeSetting(convertedMode);
    }
  }

  if (root.containsKey("temperature")) {
    float temperature = root["temperature"];
    hp.setTemperature(temperature);
  }

  if (root.containsKey("fan")) {
    const char* fan = root["fan"];
    hp.setFanSpeed(fan);
  }

  if (root.containsKey("vane")) {
    const char* vane = root["vane"];
    hp.setVaneSetting(vane);
  }

  if (root.containsKey("wideVane")) {
    const char* wideVane = root["wideVane"];
    hp.setWideVaneSetting(wideVane);
  }

  overrideGenericSettings_NewSettings = true;
  if (!overrideGenericSettings) {
    overrideGenericSettings = true;
    publishOverrideStatus(true);
  }
}

long lastOverridePublish = 0;

void publishOverrideStatus(bool forceUpdate) {
  if (forceUpdate || millis() - lastOverridePublish > MHK1_RESET_OVERRIDE_PERIOD) {
    if (!mqtt_client.publish(heatpump_override_topic, overrideGenericSettings ? "ON" : "OFF", true)) {
      infoLog("failed to publish override flag");
    }
    
    lastOverridePublish = millis();
  }
}

long lastHPSettingsPublish = 0;
void publishHPSettings() {
  // update settings periodically in case the change notification was lost
  if(millis() - lastHPSettingsPublish > 60000) {
    hpSettingsChanged(hp.getSettings());
    hpStatusChanged(hp.getStatus());
    lastHPSettingsPublish = millis();
  }
}

void handleMqttDebugTopic(const char* message) {
  // this gets persisted in case we need to debug stuff on reboot
  if (strcmp(message, "on") == 0) {
    _debugMode = true;
    preferences.putBool("debug", true);
    debugLog("debug mode enabled");
  } else if (strcmp(message, "off") == 0) {
    _debugMode = false;
    preferences.putBool("debug", false);
    debugLog("debug mode disabled");
  }  
}

void handleMqttFunctionsTopic(const char* message) {
  const size_t bufferSize = JSON_OBJECT_SIZE(6);
  DynamicJsonDocument root(bufferSize);
  DeserializationError error = deserializeJson(root, message);

  if (error) {
    infoLog("!root.success(): invalid JSON on heatpump_functions_command_topic...");
    return;
  }
  
  if (root.containsKey("set")) {
    JsonObject setCodes = root["set"];
    memset(setFunctionCodes, 0, sizeof(setFunctionCodes));
    memset(setFunctionValues, 0, sizeof(setFunctionValues));

    int idx = 0;
    for (JsonObject::iterator it = setCodes.begin(); it != setCodes.end(); ++it, ++idx) {
      int code = String(it->key().c_str()).toInt();
      int value = it->value().as<int>();
      setFunctionCodes[idx] = code;
      setFunctionValues[idx] = value;
    }
    sendSetFunctions = true;
  } else if (root.containsKey("get")) {
    sendGetFunctions = true;
  }
}

void publishFunctionValues() {
  heatpumpFunctions const& functions = hp.getFunctions();
  heatpumpFunctionCodes codes = functions.getAllCodes();
  const size_t bufferSize = JSON_OBJECT_SIZE(MAX_FUNCTION_CODE_COUNT*2);
  DynamicJsonDocument root(bufferSize);

  bool isAnyValid = false;
  for (int i = 0; i < MAX_FUNCTION_CODE_COUNT; ++i) {
    if (codes.valid[i]) {
      auto cc = String("") + codes.code[i];
      root[cc] = functions.getValue(codes.code[i]);
      isAnyValid = true;
    }
  }
  if (!isAnyValid) infoLog("No function code is valid");

  String buffer("");
  serializeJson(root, buffer);
  mqtt_client.publish(heatpump_functions_topic, buffer.c_str());
}

void handleMqttOverrideTopic(const char* message) {
  bool newOverrideVal = strcmp(message, "ON") == 0;
  if (newOverrideVal != overrideGenericSettings) {
    overrideGenericSettings = newOverrideVal;
    publishOverrideStatus(true);
    infoLog(String("New override flag: ") + overrideGenericSettings);
  }
}

void handleMqttRemoteTempTopic(const char* message) {
  double temperature = String(message).toDouble();
  bool disable = false;
  if (isnan(temperature)) {
    disable = true;
    infoLog("Remote temperature is not a number");
  } else if (temperature < 0.0001) {
    disable = true;
    infoLog("Remote temperature is <= 0");
  }

  timeSinceRemoteTempSet = millis();
  remoteTemperatureSet = !disable;
  remoteTemperature = temperature;
}

void handleMqttRebootTopic(const char* message) {
  if (strcmp(message, "1") == 0 || strcmp(message, "true") == 0) {
    rebootMe("Reboot because MQTT topic");
  }
}

void handleRemoteTempTimeout() {
  if (remoteTemperatureSet && millis() - timeSinceRemoteTempSet > 5 * 60000) {
    remoteTemperatureSet = false;
    infoLog("Disabling remote temp since no temperature has been set lately.");
    remoteTemperature = 0;
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Copy payload into message buffer
  char message[length + 1];
  for (int i = 0; i < length; i++) {
    message[i] = (char)payload[i];
  }
  message[length] = '\0';

  if (strcmp(topic, heatpump_set_topic) == 0) { //if the incoming message is on the heatpump_set_topic topic...
    handleMqttSetTopic(message);
  } else if (strcmp(topic, heatpump_debug_set_topic) == 0) { //if the incoming message is on the heatpump_debug_set_topic topic...
    handleMqttDebugTopic(message);
  } else if (strcmp(topic, heatpump_functions_command_topic) == 0) {
    handleMqttFunctionsTopic(message);
  } else if (strcmp(topic, heatpump_override_set_topic) == 0) {
    handleMqttOverrideTopic(message);
  } else if (strcmp(topic, heatpump_remote_temp_topic) == 0) {
    handleMqttRemoteTempTopic(message);
  } else if (strcmp(topic, heatpump_reboot_topic) == 0) {
    handleMqttRebootTopic(message);
  } else {//should never get called, as that would mean something went wrong with subscribe
    mqtt_client.publish(heatpump_debug_topic, "heatpump: wrong topic received");
  }
}

void mqttConnect() {
  if (wifiConnected && millis() - timeSinceLastMqttConnect > 2*60*1000 && !mqtt_client.connected()) {
    if (mqtt_client.connect(client_id, mqtt_username, mqtt_password)) {
      // Attempt to connect
      mqtt_client.subscribe(heatpump_set_topic);
      mqtt_client.subscribe(heatpump_functions_command_topic);
      mqtt_client.subscribe(heatpump_override_set_topic);
      mqtt_client.subscribe(heatpump_remote_temp_topic);
      mqtt_client.subscribe(heatpump_reboot_topic);
      mqtt_client.subscribe(heatpump_debug_set_topic);
      mqtt_client.setSocketTimeout(1); // 1 second timeout
      infoLog("mqtt Connected");
    } else {
      infoLog("mqttConnect failed");
    }
    
    timeSinceLastMqttConnect = millis();
  }
}

void setLamps() {
  if (wifiConnected) {
    setLamp(WARM_LAMP, true);
  } else {
    setLamp(WARM_LAMP, false);
  }

  if (hpConnected) {
    setLamp(COOL_LAMP, true);
  } else {
    setLamp(COOL_LAMP, false);
  }
  
  if (millis() - timeSinceLastPacketFromMHK1 > 10000) { 
    setLamp(GREEN_LAMP, false);
    setLamp(BLUE_LAMP, false);
    setLamp(RED_LAMP, true);
    setLamp(COOL_LAMP, false);
    return;
  } else {
    setLamp(RED_LAMP, false);
  }

  if (millis() - timeSinceLastPacketFromHP > 10000) {
    setLamp(GREEN_LAMP, false);
    setLamp(BLUE_LAMP, true);
    setLamp(COOL_LAMP, false);
    return;
  } else {
    setLamp(BLUE_LAMP, false);
  }

  if (overrideGenericSettings) {
    setLamp(GREEN_LAMP, true);
  } else {
    setLamp(GREEN_LAMP, false);
  }
}

void loop() {
  connectWifi();
  mqttConnect();
  setLamps();
  handleRemoteTempTimeout();

  publishOverrideStatus(false);
  publishHPSettings();
  handleIdleState();

  // read MHK1
  {
    int packetLen = readSerial(mhk1, String("MHK1"), mhk1ReadBuffer, SERIAL_BUFFER_SIZE);
    if(packetLen > 0) {
      timeSinceLastPacketFromMHK1 = millis();
      handleMhk1Packet(mhk1ReadBuffer, packetLen);
    }
  }

  // read HP
  {
    int packetLen = readSerial(heatpumpSerial, String("HP"), hpReadBuffer, SERIAL_BUFFER_SIZE);
    if(packetLen > 0) {
      timeSinceLastPacketFromHP = millis();
      handleHeatpumpPacket(hpReadBuffer, packetLen);
    }
  }


  mqtt_client.loop();
  
#ifdef OTA
  ArduinoOTA.handle();
#endif
}

/**
 * Returns true if there is a pending request received from MQTT that has not yet been delivered to the HP
 */
bool isPacketReadyForInjection() {
  switch (currentState) {
    case State_Idle:
      return false;
      
    case State_GetFreq:
    case State_SetSettings:
    case State_GetFunctions1:
    case State_GetFunctions2:
    case State_SetFunctions1:
    case State_SetFunctions2:
      return true;

    case State_GetFreqResponse:
    case State_SetSettingsResponse:
    case State_GetFunctions1Response:
    case State_GetFunctions2Response:
    case State_SetFunctions1Response:
    case State_SetFunctions2Response:
      rebootMe("Asking for injection when expecting a response");
      return false;
  }
  return false;
}

bool isExpectingInjectionResponse() {
  switch (currentState) {
    case State_Idle:
      return false;
      
    case State_GetFreq:
    case State_SetSettings:
    case State_GetFunctions1:
    case State_GetFunctions2:
    case State_SetFunctions1:
    case State_SetFunctions2:
      return false;

    case State_GetFreqResponse:
    case State_SetSettingsResponse:
    case State_GetFunctions1Response:
    case State_GetFunctions2Response:
    case State_SetFunctions1Response:
    case State_SetFunctions2Response:
      return true;
  }
  return false;
}

/**
 * Gets called to send a packet to HP in order to serve the MQTT request
 */
void handleInjectedPacket() {
  switch (currentState) {
    case State_Idle:
      rebootMe("Handling injection in idle state?");
      
    case State_GetFreq: {
      byte const getFreqPacket[] = {0xFC, 0x42, 0x01, 0x30, 0x10, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x77};
      writeToSerial(HPSerial, getFreqPacket, sizeof(getFreqPacket));
      
      infoLog("Writing GetFreq to HP");
      currentState = State_GetFreqResponse;
      break;
    }
      
    case State_SetSettings: {
      byte setSettingsPacket[22] = {};
      memset(setSettingsPacket, 0, sizeof(setSettingsPacket));
      hp.update(setSettingsPacket);
      
      writeToSerial(HPSerial, setSettingsPacket, sizeof(setSettingsPacket));
      infoLog("Writing SetSettings to HP");
      currentState = State_SetSettingsResponse;
      break;
    }

    case State_GetFunctions1: {
      hp.clearFunctions();
      
      byte const getFunctions1Packet[] = {0xFC, 0x42, 0x01, 0x30, 0x10, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5D};
      writeToSerial(HPSerial, getFunctions1Packet, sizeof(getFunctions1Packet));
      
      infoLog("Writing GetFunction1 to HP");
      currentState = State_GetFunctions1Response;
      break;
    }

    case State_GetFunctions2: {
      byte const getFunctions2Packet[] = {0xFC, 0x42, 0x01, 0x30, 0x10, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5B};
      writeToSerial(HPSerial, getFunctions2Packet, sizeof(getFunctions2Packet));
      
      infoLog("Writing GetFunctions2 to HP");
      currentState = State_GetFunctions2Response;
      break;
    }
  
    case State_SetFunctions1: {
      byte setFunctions1[22] = {};
      memset(setFunctions1, 0, sizeof(setFunctions1));
      
      if (!hp.setFunctions1(setFunctions1)) {
        infoLog("hp.setFunctions1 failed");
        rebootMe("hp.setFunctions1 failed");
      }
      writeToSerial(HPSerial, setFunctions1, sizeof(setFunctions1));

      infoLog("Writing SetFunctions1 to HP");
      currentState = State_SetFunctions1Response;
      break;
    }
  
    case State_SetFunctions2: {
      byte setFunctions2[22] = {};
      memset(setFunctions2, 0, sizeof(setFunctions2));
      
      if (!hp.setFunctions2(setFunctions2)) {
        infoLog("hp.setFunctions2 failed");
        rebootMe("hp.setFunctions2 failed");
      }
      writeToSerial(HPSerial, setFunctions2, sizeof(setFunctions2));
      
      infoLog("Writing SetFunctions2 to HP");
      currentState = State_SetFunctions2Response;
      break;
    }
  
    case State_GetFreqResponse:
    case State_SetSettingsResponse:
    case State_GetFunctions1Response:
    case State_GetFunctions2Response:
    case State_SetFunctions1Response:
    case State_SetFunctions2Response:
      rebootMe("Handling injection in response states?");
  }
}

void handleInjectionResponse(byte const* packet, int length) {
  switch (currentState) {
    case State_Idle:
      rebootMe("Handling injectionResponse in idle state?");
      
    case State_GetFreq:
    case State_SetSettings:
    case State_GetFunctions1:
    case State_GetFunctions2:
    case State_SetFunctions1:
    case State_SetFunctions2:
      rebootMe("Handling injectionResponse in get states?");  

    case State_GetFreqResponse:
      currentState = State_Idle;
      infoLog("Changing state to idle");
      break;

    case State_SetSettingsResponse:
      // publishing the state to MQTT is handled as part of the settings update callback
      currentState = State_Idle;
      infoLog("Changing state to idle");
      lastMhk1SetSettingPacket = -1; // MHK1 sends a SET packet whenever it sees an external entity change settings, we want to ignore that SET packet and not count it towards the two RESET packets
      break;
      
    case State_GetFunctions1Response:
      currentState = State_GetFunctions2;
      infoLog("Changing state to GetFunctions2");
      break;
      
    case State_GetFunctions2Response:
      if (sendSetFunctions) {
        sendSetFunctions = false;

        heatpumpFunctions& functions = hp.getFunctions();
        bool hasError = false;
        
        for (int idx = 0; idx < MAX_FUNCTION_CODE_COUNT && !hasError; ++idx) {
          if (setFunctionCodes[idx] != 0) {
            if (!functions.setValue(setFunctionCodes[idx], setFunctionValues[idx])) {
              String errorMsg = String("Unable to set value for ") + setFunctionCodes[idx];
              infoLog(errorMsg);
              mqtt_client.publish(heatpump_functions_topic, errorMsg.c_str());
              hasError = true;
            }
          }
        }

        if (!hasError) {
          currentState = State_SetFunctions1;
          infoLog("Changing state to SetFunctions1");
        } else {
          mqtt_client.publish(heatpump_functions_topic, "Had errors during set.");
          currentState = State_Idle;
        }
      } else {
        publishFunctionValues();
        infoLog("Changing state to idle");
        currentState = State_Idle;
      }
      
      memset(setFunctionCodes, 0, sizeof(setFunctionCodes));
      memset(setFunctionValues, 0, sizeof(setFunctionValues));
      break;
      
    case State_SetFunctions1Response:
      currentState = State_SetFunctions2;
      infoLog("Changing state to State_SetFunctions2");
      break;
      
    case State_SetFunctions2Response:
      infoLog("Functions are set, moving to idle");
      currentState = State_Idle;
      mqtt_client.publish(heatpump_functions_topic, "Finished sending the set commands");
      break;
  }
}

void handleIdleState() {
  if (currentState != State_Idle)
    return;

  if (overrideGenericSettings_NewSettings) {
    overrideGenericSettings_NewSettings = false;
    currentState = State_SetSettings;
    infoLog("Changing state to State_SetSettings");
    return;
  }

  if (sendGetFunctions) {
    sendGetFunctions = false;
    currentState = State_GetFunctions1;
    infoLog("Changing state to State_GetFunctions1");
    return;
  }

  if (sendSetFunctions) {
    currentState = State_GetFunctions1;
    infoLog("Changing state to State_GetFunctions1 because sendSetFunctions is set");
    return;
  }

  if (millis() - lastGetFreqTs > 30000) {
    lastGetFreqTs = millis();
    currentState = State_GetFreq;
    infoLog("Changing state to State_GetFreq");
    return;
  }
}

void cacheHeatpumpResponse(byte const* packet, int length) {
  if (length == PACKET_LEN && packet[0] == 0xFC && packet[1] == 0x62 && packet[2] == 0x01 && packet[3] == 0x30) {
    int idx = -1;
    switch (packet[5]) {
      case 0x02: {
        idx = IDX_RSP_SETTINGS;
        initialSettingsReadFromHP = true;
        
        if (!overrideGenericSettings) {
          hp.resetWantedSettings();
        }
        
        break;
      }
      case 0x09: idx = IDX_RSP_STANDBY; break;
      case 0x03: idx = IDX_RSP_ROOM_TEMP; break;
      case 0x04: idx = IDX_RSP_ERRORS; break;
      case 0x06: idx = IDX_RSP_STATUS; break;
      default: break;
    }
    
    if (idx >= 0) {
      memcpy(cachedResponses[idx], packet, length);
    }
  }
}

void handleHeatpumpPacket(byte const* hpReadBuffer, int packetLen) {
  hp.readPacket(hpReadBuffer, packetLen);

  if (isExpectingInjectionResponse()) {
    handleInjectionResponse(hpReadBuffer, packetLen);

    writeToSerial(MHK1Serial, cachedResponses[injectionReseponseCacheIdx], PACKET_LEN);

    infoLog("Cached response was sent to MHK1");
    injectionReseponseCacheIdx = -1;
  } else {
    cacheHeatpumpResponse(hpReadBuffer, packetLen);
    writeToSerial(MHK1Serial, hpReadBuffer, packetLen);
    
    byte const HP_CONNECTED[] = {0xFC, 0x7A, 0x01, 0x30, 0x01, 0x00, 0x54};
    if (packetLen == sizeof(HP_CONNECTED) && memcmp(HP_CONNECTED, hpReadBuffer, sizeof(HP_CONNECTED)) == 0) {
      hpConnected = true;
    }

    debugLog("Wrote heatpump response to MHK1");
  }
}

void handleMhl1Get(byte const* mhk1ReadBuffer, int packetLen) {
  int idx = -1;
  switch(mhk1ReadBuffer[5]) {
    case 0x02: idx = IDX_RSP_SETTINGS; break;
    case 0x09: idx = IDX_RSP_STANDBY; break;
    case 0x03: idx = IDX_RSP_ROOM_TEMP; break;
    case 0x04: idx = IDX_RSP_ERRORS; break;
    case 0x06: idx = IDX_RSP_STATUS; break;
    default: break;
  }

  if (!isPacketReadyForInjection()) {
    writeToSerial(HPSerial, mhk1ReadBuffer, packetLen);

    debugLog("Wrote get packet to heatpump normal workflow");
  } else if (idx >= 0 && cachedResponses[idx][0] != 0) {
    handleInjectedPacket();

    injectionReseponseCacheIdx = idx; // save this for later to reply to mhk1
    //mhk1.write(cachedResponses[idx], PACKET_LEN);
    infoLog("MHK1 packet intercepted and not sent to HP");
    return;
  } else if (idx >= 0) {
    writeToSerial(HPSerial, mhk1ReadBuffer, packetLen);

    debugLog("Wrote packet to heatpump because value not found in cache");
  } else {
    writeToSerial(HPSerial, mhk1ReadBuffer, packetLen);

    debugLog("Wrote packet to heatpump because no cache index was found");
  }
}

void resetCachedResponses() {
  memset(cachedResponses, 0, sizeof(cachedResponses));
}

void handleMhl1Set(byte const* mhk1ReadBuffer, int packetLen) {
  if (mhk1ReadBuffer[5] == 0x7) {
    // on set remote temp let's only invalidate the relevant cache line
    memset(cachedResponses[IDX_RSP_ROOM_TEMP], 0, PACKET_LEN);

    if (!remoteTemperatureSet) {
      writeToSerial(HPSerial, mhk1ReadBuffer, packetLen);
      debugLog("Setting remote temp from MHK1");
    } else {
      byte setTempPacket[PACKET_LEN];
      int setPacketLen = 0;
      hp.setRemoteTemperature(remoteTemperature, setTempPacket, setPacketLen);
      writeToSerial(HPSerial, setTempPacket, setPacketLen);
      debugLog(String("Overriding remote temp with ") + remoteTemperature);
    }

  } else if (mhk1ReadBuffer[5] == 0x1 && overrideGenericSettings) {
    if (lastMhk1SetSettingPacket == -1 || millis() - lastMhk1SetSettingPacket > MHK1_RESET_OVERRIDE_PERIOD) {
      // we'll just ignore the call and tell MHK1 that set was successful, on the next get call MHK1 will figure out that it's command was overridden externally
      const byte genericSetOkResponse[] = {0xFC, 0x61, 0x01, 0x30, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5E};
      delay(10);
      writeToSerial(MHK1Serial, genericSetOkResponse, sizeof(genericSetOkResponse));
      
      infoLog("Ignoring set generic settings from MHK1");
    } else {
      if (overrideGenericSettings) {
        overrideGenericSettings = false;
        publishOverrideStatus(true);
        infoLog("Override flag was set to false because MHK1 is setting values multiple times");
      }
      
      writeToSerial(HPSerial, mhk1ReadBuffer, packetLen);
    }
    
    lastMhk1SetSettingPacket = millis();
    return;
  } else {
    writeToSerial(HPSerial, mhk1ReadBuffer, packetLen);

    // on set calls we need to invalidate caches so that MHK1 can get an up to date status
    resetCachedResponses();
    infoLog("Other set packet or no override");
  }
}

void discardSerialBuffers() {
  while(mhk1.available()) mhk1.read();
  while(heatpumpSerial.available()) heatpumpSerial.read();
}

void rebootMe(char const* message) {
  infoLog(String("Rebooting in 10s: ") + message);
  delay(10000);
  discardSerialBuffers();
  resetFunc();
}

void handleMhk1Packet(byte const* mhk1ReadBuffer, int packetLen) {
  const byte SIG_CONNECT[] = {0xFC, 0x5A, 0x01, 0x30, 0x02, 0xCA, 0x01, 0xA8};
  const byte SIG_GET[] = {0xFC, 0x42, 0x01, 0x30, 0x10};
  const byte SIG_SET[] = {0xFC, 0x41, 0x01, 0x30, 0x10};
  const byte SIG_GET_RSP[] = {0xFC, 0x62, 0x01, 0x30, 0x10};
  const byte SIG_SET_RSP[] = {0xFC, 0x61, 0x01, 0x30, 0x10};

  if (memcmp(mhk1ReadBuffer, SIG_CONNECT, sizeof(SIG_CONNECT)) == 0) {
    hpConnected = false;
    numberOfConnectRequests += 1;
    if (numberOfConnectRequests >= 10) {
      // restart every 10 connect requests in case we are in a bad state
      rebootMe("Too many connection requests");
    }
  }
  
  if (packetLen != PACKET_LEN) {
    writeToSerial(HPSerial, mhk1ReadBuffer, packetLen);

    debugLog("Wrote generic packet from MHK1 to heatpump");
    return;
  }

  byte const computedChkSum = HeatPump::checkSum(mhk1ReadBuffer, packetLen - 1);
  byte const chkSumFromPacket = packetLen > 0 ? mhk1ReadBuffer[packetLen - 1] : 0;
  if (computedChkSum != chkSumFromPacket) {
    writeToSerial(HPSerial, mhk1ReadBuffer, packetLen);

    debugLog("Wrote packet with bad checksum from MHK1 to heatpump");
    return;
  }

  if (memcmp(mhk1ReadBuffer, SIG_GET, sizeof(SIG_GET)) == 0) {
    handleMhl1Get(mhk1ReadBuffer, packetLen);
  } else if(memcmp(mhk1ReadBuffer, SIG_SET, sizeof(SIG_SET)) == 0) {
    handleMhl1Set(mhk1ReadBuffer, packetLen);
  } else {
    writeToSerial(HPSerial, mhk1ReadBuffer, packetLen);

    debugLog("Wrote not get&set packet from MHK1 to heatpump");
  }

  return;
}

int readSerial(HardwareSerial& serialIn, const String& serialId, byte* buffer, int expectedPacketLen) {
  long startMillis = millis();
  
  if (serialIn.available() <= 0) {
    return -1;
  }

  memset(buffer, 0, expectedPacketLen * sizeof(byte));
  bool finished = false;
  int bufferPointer = 0;

  // assume we can read everything once a byte is available 
  while (!finished && bufferPointer < expectedPacketLen) {
    buffer[bufferPointer] = serialIn.read();
    
    if (bufferPointer == 0 && buffer[bufferPointer] != 0xFC) {
      return -1;
    }
    if (bufferPointer == 2 && buffer[bufferPointer] != 0x01) {
      return -1;
    }
    if (bufferPointer == 3 && buffer[bufferPointer] != 0x30) {
      return -1;
    }
    if (bufferPointer == 4 && buffer[bufferPointer] > 0x10) { // we don't expect packets that have more than 0x10 bytes of data
      return -1;
    }
    else {
      expectedPacketLen = buffer[4] + 5 + 1; // account for header size and checksum
    }
    
    bufferPointer += 1;

    if (bufferPointer == expectedPacketLen)
      break;
      
    while (serialIn.available() <= 0 && millis() - startMillis < SERIAL_READ_TIMEOUT) { delay(1); }
    if (serialIn.available() <= 0) {
      infoLog("Timed out while waiting for the rest of the packet");
      return -1;
    }
  }

  debugLog(serialId + " sent " + expectedPacketLen + " bytes: " + toHex(buffer, expectedPacketLen));
  if (bufferPointer != expectedPacketLen) {
    return -1;
  }

  byte checksum = HeatPump::checkSum(buffer, expectedPacketLen - 1);
  if (checksum != buffer[expectedPacketLen - 1]) {
    infoLog("Packet has bad checksum");
    return -1;
  }

  return expectedPacketLen;
}

String toHex(byte const* packet, int len) {
  String message("");
  for (int idx = 0; idx < len; idx++) {
    if (idx != 0)
      message += String(" ");
    if (packet[idx] < 16) {
      message += "0";
    }
    message += String(packet[idx], HEX);
  }
  
  return message;
}

void writeToSerial(int serialId, byte const* packet, int length) {
  HardwareSerial& serial = serialId == HPSerial ? heatpumpSerial : mhk1;
  char const* name = serialId == HPSerial ? "HP" : "MHK1";
  String h = toHex(packet, length);
  serial.write(packet, length);
  debugLog(String("To ") + name + String(": ") + h);
}
