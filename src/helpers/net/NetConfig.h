#pragma once

// Runtime network configuration for a MeshCore repeater, persisted to SPIFFS.
// Build flags provide DEFAULTS only; real values are set at runtime over any CLI
// transport (LoRa / USB / TCP) and stored on-device — so no secrets in the build.

#include <Arduino.h>

// ---- defaults (overridable via build flags) ----
#ifndef WIFI_SSID
  #define WIFI_SSID "changeme"
#endif
#ifndef WIFI_PWD
  #define WIFI_PWD "changeme"
#endif
#ifndef WIFI_TX_POWER_DBM
  #define WIFI_TX_POWER_DBM 0        // 0 = core default (~19.5 dBm)
#endif
#ifndef MQTT_HOST
  #define MQTT_HOST "192.168.1.10"
#endif
#ifndef MQTT_PORT
  #define MQTT_PORT 1883
#endif
#ifndef MQTT_USER
  #define MQTT_USER "mesh"
#endif
#ifndef MQTT_PWD
  #define MQTT_PWD "changeme"
#endif
#ifndef MQTT_BASE_TOPIC
  #define MQTT_BASE_TOPIC "meshcore"
#endif
#ifndef TELEMETRY_INTERVAL_SECS
  #define TELEMETRY_INTERVAL_SECS 60
#endif
#ifndef NTP_SERVER
  #define NTP_SERVER "pool.ntp.org"
#endif
#ifndef NTP_ENABLED
  #define NTP_ENABLED 1
#endif
#ifndef NTP_INTERVAL_SECS
  #define NTP_INTERVAL_SECS 90000   // re-apply system time to RTC every 25h
#endif

struct NetConfig {
  bool wifi_enabled = true;
  char wifi_ssid[33] = {0};
  char wifi_pwd[65]  = {0};
  int  wifi_power    = 0;        // dBm; 0 = core default/max

  bool mqtt_enabled  = true;
  char mqtt_host[64] = {0};
  int  mqtt_port     = 1883;
  char mqtt_user[33] = {0};
  char mqtt_pwd[65]  = {0};
  char mqtt_topic[24] = {0};

  int  telemetry_s   = 60;

  bool ntp_enabled    = true;
  char ntp_server[64] = {0};
  int  ntp_interval_s = 90000;

  void loadDefaults();                 // populate from build-flag macros
  void load();                         // read /netcfg, per-key fallback to defaults
  void save();                         // write /netcfg

  // CLI helpers. Return true if `key` is one we own (i.e. command consumed).
  // set() also persists. get() masks passwords.
  bool set(const char* key, const char* val, char* reply, size_t reply_len);
  bool get(const char* key, char* reply, size_t reply_len);
  bool isOwnedKey(const char* key) const;
};
