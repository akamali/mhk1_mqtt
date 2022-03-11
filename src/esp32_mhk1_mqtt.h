
//#define ESP32
#define OTA
//const char* ota_password = "<YOUR OTA PASSWORD GOES HERE>";

// wifi settings
const char* ssid = "SSID";
const char* password = "PASSWORD";

// mqtt server settings
const char* mqtt_server   = "192.168.1.37";
const int mqtt_port       = 1883;
const char* mqtt_username = "mqtt";
const char* mqtt_password = "PASSWORD";

// mqtt client settings
// Note PubSubClient.h has a MQTT_MAX_PACKET_SIZE of 128 defined, so either raise it to 256 or use short topics
const char* client_id                   = "heatpump"; // Must be unique on the MQTT network
const char* heatpump_topic              = "heatpump";
const char* heatpump_set_topic          = "heatpump/set";
const char* heatpump_status_topic       = "heatpump/status";
const char* heatpump_timers_topic       = "heatpump/timers";
const char* heatpump_override_topic     = "heatpump/override";
const char* heatpump_override_set_topic = "heatpump/override/set";

const char* heatpump_debug_topic        = "heatpump/debug";
const char* heatpump_debug_set_topic    = "heatpump/debug/set";
const char* heatpump_info_topic         = "heatpump/info";

const char* heatpump_functions_topic          = "heatpump/functions";
const char* heatpump_functions_command_topic  = "heatpump/functions/command";
