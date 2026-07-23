#pragma once

// NetServices: board- and mesh-agnostic network layer for a MeshCore repeater.
//  - always-on WiFi STA (non-blocking reconnect), TX power configurable
//  - raw TCP CLI console (telnet/nc), password-gated, feeds the repeater CLI
//  - MQTT telemetry publisher with Home Assistant MQTT Discovery + LWT availability
//  - runtime config (WiFi/MQTT) via CLI over any transport, persisted on-device
//
// Deliberately does NOT include Mesh.h — it receives plain callbacks so it can be
// unit-tested and reused across boards.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <math.h>            // NAN / isnan for optional sensor fields
#include "NetConfig.h"

// Snapshot of node telemetry, filled by the example via a callback.
// Optional I2C sensor fields default to NAN ("not present"); publishState and
// publishDiscovery skip any field that is still NAN at sample time.
struct Telemetry {
  float    batt_v      = 0.0f;
  float    batt_pct    = 0.0f;
  uint32_t uptime_s    = 0;
  uint32_t free_heap   = 0;
  uint16_t neighbours  = 0;
  uint32_t rx_count    = 0;
  uint32_t tx_count    = 0;
  int16_t  last_rssi   = 0;
  float    last_snr    = 0.0f;
  // Optional environment sensor telemetry (decoded from querySensors); NAN = absent.
  float    sens_temp   = NAN;  // °C
  float    sens_hum    = NAN;  // %
  float    sens_pres   = NAN;  // hPa
  // Optional multi-channel current-monitor (INA3221) telemetry. The INA3221's
  // enabled channels are decoded positionally by ascending LPP channel:
  // 0 -> panel, 1 -> charge (CN3791->battery), 2 -> load. NAN = channel absent.
  float    panel_v     = NAN;  // V   INA3221 CH1 bus voltage (panel side)
  float    panel_a     = NAN;  // A   panel current
  float    panel_w     = NAN;  // W   panel power (V*I)
  float    chg_v       = NAN;  // V   INA3221 CH2 bus voltage (battery side)
  float    chg_a       = NAN;  // A   charge current
  float    chg_w       = NAN;  // W   charge power
  float    load_v      = NAN;  // V   INA3221 CH3 bus voltage (spare/load)
  float    load_a      = NAN;  // A   load current
  float    load_w      = NAN;  // W   load power
};

// Runs the repeater CLI for a single text line; fills reply (may be empty).
typedef void (*CmdFn)(const char* cmd, char* reply, size_t reply_len);
// Fills a Telemetry snapshot from board/mesh state.
typedef void (*TelemFn)(Telemetry& out);
// Sets the authoritative RTC clock to an absolute UTC epoch (seconds).
typedef void (*SetTimeFn)(uint32_t epoch);

class NetServices {
public:
  // node_id: short stable hex id (used in MQTT topics / HA unique_id). Copied.
  void begin(const char* node_id, CmdFn cmd, TelemFn telem, SetTimeFn set_time = nullptr);

  // Call from the main loop() only. Non-blocking; never stalls mesh routing.
  void loop();

  // Runtime WiFi on/off toggle (persisted). Default ON.
  void setWifiEnabled(bool enabled);
  bool isWifiEnabled() const { return _wifi_enabled; }

  // Handle a net-config CLI line (wifi.* / mqtt.* / net.*). True if consumed.
  bool handleCli(const char* command, char* reply, size_t reply_len);

  // Status.
  bool isWifiConnected() const { return WiFi.status() == WL_CONNECTED; }
  bool isMqttConnected();
  String ipString() const;

private:
  char _node_id[24] = {0};
  CmdFn   _cmd   = nullptr;
  TelemFn _telem = nullptr;

  NetConfig _cfg;

  // ---- WiFi ----
  bool _wifi_enabled = true;
  bool _wifi_started = false;
  unsigned long _last_wifi_retry = 0;

  // ---- NTP / SNTP ----
  SetTimeFn _set_time = nullptr;
  enum NtpState { NTP_IDLE, NTP_WAITING, NTP_SYNCED };
  NtpState _ntp_state = NTP_IDLE;
  unsigned long _ntp_wait_start_ms = 0;
  unsigned long _last_ntp_apply_ms = 0;
  unsigned int  _ntp_retries = 0;
  void ntpLoop();

  void applyWifiState();      // bring STA up or fully off to match _wifi_enabled
  void applyWifiPower();      // set TX power from config (if non-default)
  void applyConfig();         // 'net apply': restart WiFi/MQTT with current config

  // ---- TCP CLI ----
  WiFiServer _cli_server;
  WiFiClient _cli_client;
  bool  _cli_authed = false;
  char  _cli_line[160];
  int   _cli_len = 0;
  unsigned long _cli_last_activity = 0;
  void cliLoop();
  void cliReset();

  // ---- MQTT ----
  WiFiClient    _mqtt_wifi;
  PubSubClient  _mqtt{_mqtt_wifi};
  unsigned long _last_mqtt_retry = 0;
  unsigned long _last_publish = 0;
  bool          _discovery_sent = false;

  void mqttLoop();
  bool mqttConnect();
  void publishDiscovery();
  void publishState(const Telemetry& t);
  void topicState(char* out, size_t n);
  void topicStatus(char* out, size_t n);
};
