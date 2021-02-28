#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <PubSubClient.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <math.h>
#include <ESP8266mDNS.h>

#ifndef NODE_NAME
#define NODE_NAME "esp8266aq"
#endif

#ifndef MQTT_SERVER
#define MQTT_SERVER ""
#endif

#ifndef MQTT_PORT
#define MQTT_PORT 1833
#endif

#ifndef MQTT_PREFIX
#define MQTT_PREFIX "esp8266aq"
#endif

#define LED_PIN 2
#define G2_PIN 14
#define G3_PIN 12

Adafruit_BME280 bme;

WiFiClient espClient;
PubSubClient client(espClient);
ESP8266WebServer server(80);

struct Config {
  String name;

  String mqtt_server;
  unsigned int mqtt_port;
  String mqtt_prefix;
  IPAddress address;
};

Config config;

struct Measurement {
  const char *name;
  const int scale;
  Config * cfg;

  unsigned long last_measured_at;
  const char * last_prefix;
  int last_value;
  char full_topic[256];

  Measurement(const char *name, int scale = 0, Config * cfg = &config):
    name(name),
    scale(scale),
    cfg(cfg),
    last_measured_at(0),
    last_prefix(NULL),
    last_value(0) {};

  void record(int value) {
    last_value = value;
    last_measured_at = millis();
  }

  const char * topic(void) {
    const char * prefix = cfg->mqtt_prefix.c_str();
    if (last_prefix != prefix) {
      last_prefix = prefix;
      memset(full_topic, 0, 256);
      sprintf(full_topic, "%s/%s", last_prefix, name);
    }

    return full_topic;
  }

  void publish_int(PubSubClient * client) {
    char formatted_value[256];
    sprintf(formatted_value, "%u", last_value);
    publish(client, formatted_value);
  }

  void publish_int(HardwareSerial * serial) {
    char formatted_value[256];
    sprintf(formatted_value, "%u", last_value);
    publish(serial, formatted_value);
  }

  void publish_float(PubSubClient * client) {
    char formatted_value[256];
    sprintf(formatted_value, "%u.%.2u", last_value / 100, last_value % 100);
    publish(client, formatted_value);
  }

  void publish_float(HardwareSerial * serial) {
    char formatted_value[256];
    sprintf(formatted_value, "%u.%.2u", last_value / 100, last_value % 100);
    publish(serial, formatted_value);
  }

  void publish(PubSubClient * client, char * formatted_value) {
    client->publish(topic(), formatted_value);
  }

  void publish(HardwareSerial * serial, char * formatted_value) {
    serial->print(topic());
    serial->print(": ");
    serial->println(formatted_value);
  }
};

Measurement measurement_temperature("temperature", 100);
Measurement measurement_humidity("humidity", 100);
Measurement measurement_dewpoint("dewpoint", 100);

#define NUM_MEASUREMENTS_PMS5003 12
Measurement measurements_pms5003[NUM_MEASUREMENTS_PMS5003] = {
  Measurement("pm1_0_standard"),
  Measurement("pm2_5_standard"),
  Measurement("pm10_standard"),
  Measurement("pm1_0_env"),
  Measurement("pm2_5_env"),
  Measurement("concentration_unit"),
  Measurement("particles_03um"),
  Measurement("particles_05um"),
  Measurement("particles_10um"),
  Measurement("particles_25um"),
  Measurement("particles_50um"),
  Measurement("particles_100um")
};

struct PMS5003 {
  byte input_string[32];
  int input_idx;

  PMS5003() : input_idx(0) {}

  void process_byte(byte in) {
    if(input_idx == 32) {
      input_idx = 0;
    }

    input_string[input_idx++] = in;
    if (input_idx == 1 && input_string[0] != 0x42) {
      input_idx = 0;
    } else if (input_idx == 2 && input_string[1] != 0x4d) {
      input_idx = 0;
    }
  }

  void reset_input() {
    input_idx = 0;
  }

  unsigned int get_value(int idx) {
    byte high = input_string[4 + idx * 2];
    byte low  = input_string[4 + idx * 2 + 1];
    return (high << 8) | low;
  }

  /* Are we a full, valid packet */
  bool is_valid_packet() {
    if (input_idx != 32)
      return false;

    /* TODO: check crc */

    return true;
  }
};

PMS5003 pms5003;

typedef StaticJsonDocument<512> ConfigJsonDocument;

static void loadConfigurationFromDoc(Config &config, ConfigJsonDocument &doc) {
  if (doc["mqtt_server"])
    config.mqtt_server = doc["mqtt_server"].as<String>();

  if (doc["mqtt_port"])
    config.mqtt_port = doc["mqtt_port"].as<String>().toInt();

  if (doc["mqtt_prefix"])
    config.mqtt_prefix = doc["mqtt_prefix"].as<String>();
}

void loadConfiguration(Config &config) {
  /* Load defaults */
  config.name = NODE_NAME;
  config.mqtt_server = MQTT_SERVER;
  config.mqtt_port = MQTT_PORT;
  config.mqtt_prefix = MQTT_PREFIX;

  if (LittleFS.exists("config.json")) {
    File file = LittleFS.open("config.json", "r");

    ConfigJsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);

    if (!error) {
      loadConfigurationFromDoc(config, doc);
      return;
    }

    file.close();
  }

}

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
}

String getContentType(String filename) {
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".json")) return "application/json";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  return "text/plain";
}

bool handleFileRead(String path) {
  if (path.endsWith("/"))
    path += "index.html";

  String contentType = getContentType(path);
  if (LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    server.streamFile(file, contentType);
    file.close();
    return true;
  }

  return false;
}

void addMeasurementJson(String &json, Measurement &m) {
  json += "{\"name\": \"";
  json += m.name;
  json += "\", \"value\": ";
  if (m.scale) {
    json += float(m.last_value) / m.scale;
  } else {
    json += m.last_value;
  }
  json += ", \"last_measured_at\": ";
  json += m.last_measured_at;
  json += "}, ";
}

void webHandleStatus() {
  String json;
  json.reserve(1024);
  json += "{\"measurements\": [";

  addMeasurementJson(json, measurement_temperature);
  addMeasurementJson(json, measurement_humidity);
  for(int i = 0; i < NUM_MEASUREMENTS_PMS5003; i++) {
    addMeasurementJson(json, measurements_pms5003[i]);
  }
  json.remove(json.lastIndexOf(",")); // remove trailing comma

  json += "], \"current_time\": ";
  json += millis();
  json += ", \"mqtt\": {\"connected\": ";
  json += client.connected() ? "true" : "false";
  json += ", \"server\": \"";
  json += config.mqtt_server;
  json += "\", \"port\": ";
  json += config.mqtt_port;
  json += "}, \"esp8266\": {\"heap_free\":";
  json += ESP.getFreeHeap();
  json += ", \"heap_fragmentation\": ";
  json += ESP.getHeapFragmentation();
  json += ", \"heap_max_free_block_size\": ";
  json += ESP.getMaxFreeBlockSize();
  json += "}}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void webHandleUpdateConfig() {
  String body = server.arg("plain");

  File file = LittleFS.open("config.json", "w");
  file.print(body);
  file.close();

  server.send(200, "text/json", "{\"success\":true}" );

  ESP.restart();
}

void webHandleReadConfig() {
  String json;
  json.reserve(1024);

  json += "{\"name\": \"";
  json += config.name;
  json += "\", \"mqtt_server\": \"";
  json += config.mqtt_server;
  json += "\", \"mqtt_port\": \"";
  json += config.mqtt_port;
  json += "\", \"mqtt_prefix\": \"";
  json += config.mqtt_prefix;
  json += "\"}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void setup() {
  WiFiManager wifiManager;

  pinMode(G2_PIN, INPUT);
  pinMode(G3_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(2, HIGH);

  Serial.begin(9600);
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.autoConnect();

  randomSeed(micros());

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  if (!bme.begin(0x76)) {
    Serial.println("Couldn't find BME280 sensor");
    delay(50);
  }
  LittleFS.begin();
  loadConfiguration(config);
  Serial.print("Node name: ");
  Serial.println(config.name);
  Serial.print("MQTT Server: ");
  Serial.println(config.mqtt_server);
  Serial.print("MQTT Port: ");
  Serial.println(config.mqtt_port);
  Serial.print("MQTT Prefix: ");
  Serial.println(config.mqtt_prefix);
  Serial.print("GPIO: ");
  Serial.print(digitalRead(G2_PIN));
  Serial.println(digitalRead(G3_PIN));

  MDNS.begin(config.name);

  ArduinoOTA.begin();

  Serial.flush();
  Serial.swap(); // Switch to sensor

  server.on("/config.json", HTTP_POST, webHandleUpdateConfig);
  server.on("/config.json", HTTP_GET, webHandleReadConfig);
  server.on("/status.json", HTTP_GET, webHandleStatus);
  server.onNotFound([]() {
    if (!handleFileRead(server.uri()))
      server.send(404, "text/plain", "404: Not Found");
  });

  server.begin();
}

void mqtt_reconnect() {
  static unsigned long last_attempt = 0;
  if (millis() - last_attempt < 5000) {
    return;
  }

  last_attempt = millis();
  Serial.swap();
  if (config.mqtt_server.length() && config.mqtt_port) {
    if (WiFi.hostByName(config.mqtt_server.c_str(), config.address)) {
      client.setServer(config.address, config.mqtt_port);
      String clientId = config.name;
      if(client.connect(clientId.c_str())) {
        Serial.println("Connected\n");
      } else {
        Serial.println(clientId);
        Serial.println(config.address);
        Serial.print("failed, rc=");
        Serial.println(client.state());
      }
    } else {
      Serial.print("Couldn't find address: ");
      Serial.println(config.mqtt_server);
    }

  }
  Serial.flush();
  Serial.swap();
}

void loop() {
  if (client.connected()) {
    client.loop();
  } else {
    mqtt_reconnect();
  }

  if (client.connected()) {
    digitalWrite(LED_PIN, LOW); // LOW == ON
  } else {
    digitalWrite(LED_PIN, HIGH);
  }

  server.handleClient();
  ArduinoOTA.handle();

  /* If there is more than one packet in the buffer we only want the most recent */
  while (Serial.available() > 32) {
    Serial.read();
    pms5003.reset_input();
  }

  while (Serial.available()) {
    pms5003.process_byte(Serial.read());

    if (pms5003.is_valid_packet()) {
      if (!client.connected()) {
        Serial.swap();
        Serial.println("MQTT client is not connected");
        Serial.flush();
        Serial.swap();
      }

      for(int i = 0; i < NUM_MEASUREMENTS_PMS5003; i++) {
        unsigned int value = pms5003.get_value(i);

        measurements_pms5003[i].record(value);
        measurements_pms5003[i].publish_int(&client);
      }

      // Publish to the serial client if G2 pin is HIGH
      if (digitalRead(G2_PIN) == HIGH) {
        Serial.flush();
        Serial.swap();
        for(int i = 0; i < NUM_MEASUREMENTS_PMS5003; i++) {
          measurements_pms5003[i].publish_int(&Serial);
        }
        Serial.flush();
        Serial.swap();
      }
    }
  }

  static unsigned long last_time_measurement = 0;
  if(millis() - last_time_measurement > 1000){
    last_time_measurement = millis();

    float temp = bme.readTemperature();
    float rh = bme.readHumidity();

    // https://bmcnoldy.rsmas.miami.edu/humidity_conversions.pdf
    float x = (17.625 * temp) / (243.04 + temp);
    float l = log(rh / 100.0);
    float dewpoint = 243.04 * (l + x) / (17.625 - l - x);

    int temperature = round(temp * 100);
    int humidity = round(rh * 100);

    measurement_temperature.record(temperature);
    measurement_humidity.record(humidity);
    measurement_dewpoint.record(round(dewpoint * 100));

    measurement_temperature.publish_float(&client);
    measurement_humidity.publish_float(&client);
    measurement_dewpoint.publish_float(&client);

    // Publish to the serial client if G2 pin is HIGH
    if (digitalRead(G2_PIN) == HIGH) {
      Serial.flush();
      Serial.swap();
      measurement_temperature.publish_float(&Serial);
      measurement_humidity.publish_float(&Serial);
      measurement_dewpoint.publish_float(&Serial);
      Serial.flush();
      Serial.swap();
    }
  }
}
