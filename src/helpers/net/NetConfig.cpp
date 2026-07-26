#include "NetConfig.h"
#include <SPIFFS.h>

#define NETCFG_FILE "/netcfg"

static bool assignKey(NetConfig& c, const char* k, const char* v) {
  if      (!strcmp(k, "wifi.enabled"))  c.wifi_enabled = (atoi(v) != 0);
  else if (!strcmp(k, "wifi.ssid"))     strlcpy(c.wifi_ssid, v, sizeof(c.wifi_ssid));
  else if (!strcmp(k, "wifi.pwd"))      strlcpy(c.wifi_pwd, v, sizeof(c.wifi_pwd));
  else if (!strcmp(k, "wifi.power"))    c.wifi_power = atoi(v);
  else if (!strcmp(k, "mqtt.enabled"))  c.mqtt_enabled = (atoi(v) != 0);
  else if (!strcmp(k, "mqtt.host"))     strlcpy(c.mqtt_host, v, sizeof(c.mqtt_host));
  else if (!strcmp(k, "mqtt.port"))     c.mqtt_port = atoi(v);
  else if (!strcmp(k, "mqtt.user"))     strlcpy(c.mqtt_user, v, sizeof(c.mqtt_user));
  else if (!strcmp(k, "mqtt.pwd"))      strlcpy(c.mqtt_pwd, v, sizeof(c.mqtt_pwd));
  else if (!strcmp(k, "mqtt.topic"))    strlcpy(c.mqtt_topic, v, sizeof(c.mqtt_topic));
  else if (!strcmp(k, "net.telemetry")) c.telemetry_s = atoi(v);
  else if (!strcmp(k, "ntp.enabled"))   c.ntp_enabled = (atoi(v) != 0);
  else if (!strcmp(k, "ntp.server"))    strlcpy(c.ntp_server, v, sizeof(c.ntp_server));
  else if (!strcmp(k, "ntp.interval"))  c.ntp_interval_s = atoi(v);
  else return false;
  return true;
}

void NetConfig::loadDefaults() {
  // Off by default: the shipped credentials are placeholders, so a freshly
  // flashed node would otherwise sit in a reconnect loop against a network
  // named "changeme". Enable it once configured: `wifi.enabled 1`.
  // Nodes with a stored netcfg keep their setting - load() applies defaults
  // per key, then overrides with whatever the file contains.
  wifi_enabled = false;
  strlcpy(wifi_ssid, WIFI_SSID, sizeof(wifi_ssid));
  strlcpy(wifi_pwd,  WIFI_PWD,  sizeof(wifi_pwd));
  wifi_power = WIFI_TX_POWER_DBM;
  mqtt_enabled = true;
  strlcpy(mqtt_host, MQTT_HOST, sizeof(mqtt_host));
  mqtt_port = MQTT_PORT;
  strlcpy(mqtt_user, MQTT_USER, sizeof(mqtt_user));
  strlcpy(mqtt_pwd,  MQTT_PWD,  sizeof(mqtt_pwd));
  strlcpy(mqtt_topic, MQTT_BASE_TOPIC, sizeof(mqtt_topic));
  telemetry_s = TELEMETRY_INTERVAL_SECS;
  ntp_enabled = NTP_ENABLED;
  strlcpy(ntp_server, NTP_SERVER, sizeof(ntp_server));
  ntp_interval_s = NTP_INTERVAL_SECS;
}

void NetConfig::load() {
  loadDefaults();                       // per-key fallback if not stored
  File f = SPIFFS.open(NETCFG_FILE, FILE_READ);
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;
    int eq = line.indexOf('=');
    if (eq < 1) continue;
    String k = line.substring(0, eq);
    String v = line.substring(eq + 1);
    assignKey(*this, k.c_str(), v.c_str());
  }
  f.close();
}

void NetConfig::save() {
  File f = SPIFFS.open(NETCFG_FILE, FILE_WRITE);
  if (!f) return;
  f.printf("wifi.enabled=%d\n", wifi_enabled ? 1 : 0);
  f.printf("wifi.ssid=%s\n", wifi_ssid);
  f.printf("wifi.pwd=%s\n", wifi_pwd);
  f.printf("wifi.power=%d\n", wifi_power);
  f.printf("mqtt.enabled=%d\n", mqtt_enabled ? 1 : 0);
  f.printf("mqtt.host=%s\n", mqtt_host);
  f.printf("mqtt.port=%d\n", mqtt_port);
  f.printf("mqtt.user=%s\n", mqtt_user);
  f.printf("mqtt.pwd=%s\n", mqtt_pwd);
  f.printf("mqtt.topic=%s\n", mqtt_topic);
  f.printf("net.telemetry=%d\n", telemetry_s);
  f.printf("ntp.enabled=%d\n", ntp_enabled ? 1 : 0);
  f.printf("ntp.server=%s\n", ntp_server);
  f.printf("ntp.interval=%d\n", ntp_interval_s);
  f.close();
}

bool NetConfig::isOwnedKey(const char* key) const {
  return !strncmp(key, "wifi.", 5) || !strncmp(key, "mqtt.", 5)
      || !strncmp(key, "net.", 4)  || !strncmp(key, "ntp.", 4);
}

bool NetConfig::set(const char* key, const char* val, char* reply, size_t n) {
  if (!isOwnedKey(key)) return false;               // let MeshCore CLI handle it
  if (!assignKey(*this, key, val)) {
    snprintf(reply, n, "ERR unknown key %s", key);
    return true;
  }
  save();
  snprintf(reply, n, "OK %s set (run 'net apply')", key);
  return true;
}

bool NetConfig::get(const char* key, char* reply, size_t n) {
  if (!isOwnedKey(key)) return false;
  if      (!strcmp(key, "wifi.enabled"))  snprintf(reply, n, "wifi.enabled=%d", wifi_enabled ? 1 : 0);
  else if (!strcmp(key, "wifi.ssid"))     snprintf(reply, n, "wifi.ssid=%s", wifi_ssid);
  else if (!strcmp(key, "wifi.pwd"))      snprintf(reply, n, "wifi.pwd=%s", wifi_pwd[0] ? "***" : "(empty)");
  else if (!strcmp(key, "wifi.power"))    snprintf(reply, n, "wifi.power=%d%s", wifi_power, wifi_power ? " dBm" : " (default)");
  else if (!strcmp(key, "mqtt.enabled"))  snprintf(reply, n, "mqtt.enabled=%d", mqtt_enabled ? 1 : 0);
  else if (!strcmp(key, "mqtt.host"))     snprintf(reply, n, "mqtt.host=%s", mqtt_host);
  else if (!strcmp(key, "mqtt.port"))     snprintf(reply, n, "mqtt.port=%d", mqtt_port);
  else if (!strcmp(key, "mqtt.user"))     snprintf(reply, n, "mqtt.user=%s", mqtt_user);
  else if (!strcmp(key, "mqtt.pwd"))      snprintf(reply, n, "mqtt.pwd=%s", mqtt_pwd[0] ? "***" : "(empty)");
  else if (!strcmp(key, "mqtt.topic"))    snprintf(reply, n, "mqtt.topic=%s", mqtt_topic);
  else if (!strcmp(key, "net.telemetry")) snprintf(reply, n, "net.telemetry=%d s", telemetry_s);
  else if (!strcmp(key, "ntp.enabled"))  snprintf(reply, n, "ntp.enabled=%d", ntp_enabled ? 1 : 0);
  else if (!strcmp(key, "ntp.server"))   snprintf(reply, n, "ntp.server=%s", ntp_server);
  else if (!strcmp(key, "ntp.interval")) snprintf(reply, n, "ntp.interval=%d s", ntp_interval_s);
  else snprintf(reply, n, "ERR unknown key %s", key);
  return true;
}
