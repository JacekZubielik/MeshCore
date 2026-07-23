#include "NetServices.h"
#include "NetHooks.h"
#include <SPIFFS.h>
#include <time.h>
#include <esp_sntp.h>

#ifndef TCP_CLI_PORT
  #define TCP_CLI_PORT 5000
#endif
#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

// Home Assistant MQTT Discovery metric map. Fields match publishState() JSON keys.
// opt: pointer-to-member of an optional (NAN-able) sensor field, or nullptr for
// metrics that are always published. Discovery skips entries whose opt field is
// NAN in a sampled Telemetry, so absent sensors never create "unavailable" entities.
struct MetricDef {
  const char* name;
  const char* field;
  const char* unit;      // nullptr => omit
  const char* dev_cla;   // nullptr => omit
  const char* stat_cla;
  float Telemetry::* opt; // nullptr => always publish
};
static const MetricDef METRICS[] = {
  {"Battery Voltage", "batt_v",     "V",   "voltage",         "measurement",      nullptr},
  {"Battery",         "batt_pct",   "%",   "battery",         "measurement",      nullptr},
  {"Uptime",          "uptime_s",   "s",   "duration",        "total_increasing", nullptr},
  {"Free Heap",       "free_heap",  "B",   "data_size",       "measurement",      nullptr},
  {"Neighbours",      "neighbours", nullptr, nullptr,         "measurement",      nullptr},
  {"RX Packets",      "rx_count",   nullptr, nullptr,         "total_increasing", nullptr},
  {"TX Packets",      "tx_count",   nullptr, nullptr,         "total_increasing", nullptr},
  {"Last RSSI",       "last_rssi",  "dBm", "signal_strength", "measurement",      nullptr},
  {"Last SNR",        "last_snr",   "dB",  nullptr,           "measurement",      nullptr},
  // Optional environment sensors — advertised only when detected at boot.
  {"Temperature",     "sens_temp",  "°C",  "temperature",     "measurement",      &Telemetry::sens_temp},
  {"Humidity",        "sens_hum",   "%",   "humidity",        "measurement",      &Telemetry::sens_hum},
  {"Pressure",        "sens_pres",  "hPa", "pressure",        "measurement",      &Telemetry::sens_pres},
  // Optional INA3221 current-monitor channels (panel / charge / load).
  {"Panel Voltage",   "panel_v",    "V",   "voltage",         "measurement",      &Telemetry::panel_v},
  {"Panel Current",   "panel_a",    "A",   "current",         "measurement",      &Telemetry::panel_a},
  {"Panel Power",     "panel_w",    "W",   "power",           "measurement",      &Telemetry::panel_w},
  {"Charge Voltage",  "chg_v",      "V",   "voltage",         "measurement",      &Telemetry::chg_v},
  {"Charge Current",  "chg_a",      "A",   "current",         "measurement",      &Telemetry::chg_a},
  {"Charge Power",    "chg_w",      "W",   "power",           "measurement",      &Telemetry::chg_w},
  {"Load Voltage",    "load_v",     "V",   "voltage",         "measurement",      &Telemetry::load_v},
  {"Load Current",    "load_a",     "A",   "current",         "measurement",      &Telemetry::load_a},
  {"Load Power",      "load_w",     "W",   "power",           "measurement",      &Telemetry::load_w},
};

static NetServices* g_net = nullptr;

bool net_handle_cli(const char* command, char* reply, size_t n) {
  if (!g_net) return false;
  return g_net->handleCli(command, reply, n);
}

static wifi_power_t dbmToPower(int d) {
  if (d >= 19) return WIFI_POWER_19_5dBm;
  if (d >= 18) return WIFI_POWER_18_5dBm;
  if (d >= 17) return WIFI_POWER_17dBm;
  if (d >= 15) return WIFI_POWER_15dBm;
  if (d >= 13) return WIFI_POWER_13dBm;
  if (d >= 11) return WIFI_POWER_11dBm;
  if (d >= 8)  return WIFI_POWER_8_5dBm;
  if (d >= 7)  return WIFI_POWER_7dBm;
  return WIFI_POWER_5dBm;
}

// ---- lifecycle ----
void NetServices::begin(const char* node_id, CmdFn cmd, TelemFn telem, SetTimeFn set_time) {
  strncpy(_node_id, node_id, sizeof(_node_id) - 1);
  _cmd = cmd;
  _telem = telem;
  _set_time = set_time;
  g_net = this;
  _cfg.load();                      // stored config, per-key fallback to build defaults
  Serial.printf("NetServices: ntp interval default=%ds active=%ds\n", NTP_INTERVAL_SECS, _cfg.ntp_interval_s);
  _wifi_enabled = _cfg.wifi_enabled;
  applyWifiState();
}

void NetServices::loop() {
  applyWifiState();
  if (!_wifi_enabled) return;

  if (WiFi.status() != WL_CONNECTED) {
    if (_ntp_state != NTP_IDLE) { sntp_stop(); _ntp_state = NTP_IDLE; }   // re-arm on reconnect
    unsigned long now = millis();
    if (now - _last_wifi_retry > 10000UL) {
      _last_wifi_retry = now;
      WiFi.reconnect();
    }
    return;
  }

  cliLoop();
  mqttLoop();
  ntpLoop();
}

// ---- WiFi ----
void NetServices::applyWifiPower() {
  if (_cfg.wifi_power > 0) WiFi.setTxPower(dbmToPower(_cfg.wifi_power));
}

void NetServices::applyWifiState() {
  if (_wifi_enabled && !_wifi_started) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(_cfg.wifi_ssid, _cfg.wifi_pwd);
    applyWifiPower();
    _cli_server.begin(TCP_CLI_PORT);
    _wifi_started = true;
    Serial.printf("NetServices: WiFi enabled (ssid=%s)\n", _cfg.wifi_ssid);
  } else if (!_wifi_enabled && _wifi_started) {
    if (_mqtt.connected()) {
      char st[80];
      topicStatus(st, sizeof(st));
      _mqtt.publish(st, "offline", true);
      _mqtt.disconnect();
    }
    _cli_server.stop();
    cliReset();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    _wifi_started = false;
    _discovery_sent = false;
    Serial.println("NetServices: WiFi disabled");
  }
}

void NetServices::setWifiEnabled(bool enabled) {
  _wifi_enabled = enabled;
  _cfg.wifi_enabled = enabled;
  _cfg.save();
  applyWifiState();
}

void NetServices::applyConfig() {
  // Tear WiFi down (if up) so new ssid/pwd/power/broker take effect, then reapply.
  if (_wifi_started) {
    if (_mqtt.connected()) {
      char st[80];
      topicStatus(st, sizeof(st));
      _mqtt.publish(st, "offline", true);
      _mqtt.disconnect();
    }
    _cli_server.stop();
    cliReset();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    _wifi_started = false;
    _discovery_sent = false;
  }
  _wifi_enabled = _cfg.wifi_enabled;
  applyWifiState();
}

// Non-blocking SNTP. Runs only while WiFi is connected. loop()'s disconnect
// branch calls sntp_stop() + resets to IDLE, so each (re)connection re-arms.
void NetServices::ntpLoop() {
  if (!_cfg.ntp_enabled || !_set_time) return;   // NTP off, or board exposes no RTC sink
  unsigned long now = millis();

  if (_ntp_state == NTP_IDLE) {
    configTime(0, 0, _cfg.ntp_server);            // UTC; starts SNTP (poll mode)
    _ntp_state = NTP_WAITING;
    _ntp_wait_start_ms = now;
    _ntp_retries = 0;
    return;
  }

  if (_ntp_state == NTP_WAITING) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED || time(nullptr) > 1600000000) {
      _set_time((uint32_t)time(nullptr));
      _ntp_state = NTP_SYNCED;
      _last_ntp_apply_ms = now;
      Serial.println("NetServices: NTP synced");
    } else if (now - _ntp_wait_start_ms > 60000UL && _ntp_retries < 10) {
      sntp_stop();
      configTime(0, 0, _cfg.ntp_server);          // retry (DNS may not have been ready)
      _ntp_wait_start_ms = now;
      _ntp_retries++;
    }
    return;
  }

  // NTP_SYNCED: re-apply the auto-refreshed system time to the RTC periodically.
  if (now - _last_ntp_apply_ms >= (unsigned long)_cfg.ntp_interval_s * 1000UL) {
    _set_time((uint32_t)time(nullptr));
    _last_ntp_apply_ms = now;
  }
}

String NetServices::ipString() const {
  if (_wifi_started && WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return String("-");
}

// ---- TCP CLI (raw text, password-gated) ----
void NetServices::cliReset() {
  if (_cli_client) _cli_client.stop();
  _cli_authed = false;
  _cli_len = 0;
}

void NetServices::cliLoop() {
  if (!_cli_client || !_cli_client.connected()) {
    WiFiClient nc = _cli_server.available();
    if (nc) {
      _cli_client = nc;
      _cli_authed = false;
      _cli_len = 0;
      _cli_last_activity = millis();
      _cli_client.print("Password: ");
    }
    return;
  }

  if (millis() - _cli_last_activity > 120000UL) {
    cliReset();
    return;
  }

  while (_cli_client.available()) {
    char c = _cli_client.read();
    _cli_last_activity = millis();
    if (c == '\r') continue;
    if (c == '\n') {
      _cli_line[_cli_len] = 0;
      if (!_cli_authed) {
        if (strcmp(_cli_line, ADMIN_PASSWORD) == 0) {
          _cli_authed = true;
          _cli_client.print("OK\r\n> ");
        } else {
          _cli_client.print("Denied\r\n");
          cliReset();
          return;
        }
      } else if (_cli_len > 0) {
        char reply[160];
        reply[0] = 0;
        if (_cmd) _cmd(_cli_line, reply, sizeof(reply));
        if (reply[0]) {
          _cli_client.print("  -> ");
          _cli_client.print(reply);
          _cli_client.print("\r\n");
        }
        _cli_client.print("> ");
      }
      _cli_len = 0;
    } else if (_cli_len < (int)sizeof(_cli_line) - 1) {
      _cli_line[_cli_len++] = c;
    }
  }
}

// ---- net-config CLI (wifi.* / mqtt.* / net.*) ----
bool NetServices::handleCli(const char* command, char* reply, size_t n) {
  if (!strcmp(command, "net status")) {
    const char* ntp = !_cfg.ntp_enabled ? "off"
                    : (_ntp_state == NTP_SYNCED ? "synced"
                       : (_ntp_state == NTP_WAITING ? "waiting" : "idle"));
    snprintf(reply, n, "wifi:%s ip:%s rssi:%ddBm mqtt:%s ntp:%s",
             _wifi_enabled ? (isWifiConnected() ? "connected" : "connecting") : "off",
             ipString().c_str(),
             isWifiConnected() ? (int)WiFi.RSSI() : 0,
             _mqtt.connected() ? "connected" : "down",
             ntp);
    return true;
  }
  if (!strcmp(command, "net apply")) {
    applyConfig();
    snprintf(reply, n, "OK - net applied");
    return true;
  }
  if (!strcmp(command, "net help")) {
    snprintf(reply, n, "set wifi.ssid/pwd/power/enabled | set mqtt.host/port/user/pwd/topic/enabled | set net.telemetry | get <key> | net status|apply");
    return true;
  }

  // legacy alias: set wifi 0/1
  if (!strcmp(command, "set wifi 0") || !strcmp(command, "set wifi 1")) {
    bool en = (command[9] == '1');
    setWifiEnabled(en);
    snprintf(reply, n, "OK - wifi %s", en ? "on" : "off");
    return true;
  }

  if (!strncmp(command, "set ", 4)) {
    const char* rest = command + 4;
    const char* sp = strchr(rest, ' ');
    if (!sp) return false;                       // 'set foo' with no value -> not ours
    char key[24];
    size_t klen = sp - rest;
    if (klen >= sizeof(key)) return false;
    memcpy(key, rest, klen);
    key[klen] = 0;
    if (!_cfg.isOwnedKey(key)) return false;     // e.g. 'set tx' -> MeshCore CLI
    bool handled = _cfg.set(key, sp + 1, reply, n);
    if (handled && !strcmp(key, "wifi.enabled")) setWifiEnabled(atoi(sp + 1) != 0);
    return handled;
  }

  if (!strncmp(command, "get ", 4)) {
    const char* key = command + 4;
    if (!_cfg.isOwnedKey(key)) return false;
    return _cfg.get(key, reply, n);
  }

  return false;
}

// ---- MQTT ----
bool NetServices::isMqttConnected() { return _mqtt.connected(); }

void NetServices::topicState(char* out, size_t n)  { snprintf(out, n, "%s/%s/state",  _cfg.mqtt_topic, _node_id); }
void NetServices::topicStatus(char* out, size_t n) { snprintf(out, n, "%s/%s/status", _cfg.mqtt_topic, _node_id); }

bool NetServices::mqttConnect() {
  _mqtt.setServer(_cfg.mqtt_host, _cfg.mqtt_port);
  _mqtt.setBufferSize(768);
  _mqtt.setSocketTimeout(2);
  _mqtt.setKeepAlive(30);

  char status_topic[80];
  topicStatus(status_topic, sizeof(status_topic));
  char client_id[40];
  snprintf(client_id, sizeof(client_id), "mc_%s", _node_id);

  const char* user = _cfg.mqtt_user[0] ? _cfg.mqtt_user : nullptr;
  const char* pass = _cfg.mqtt_pwd[0]  ? _cfg.mqtt_pwd  : nullptr;
  bool ok = _mqtt.connect(client_id, user, pass, status_topic, 0, true, "offline");
  if (ok) {
    _mqtt.publish(status_topic, "online", true);
    _discovery_sent = false;
  }
  return ok;
}

void NetServices::mqttLoop() {
  if (!_cfg.mqtt_enabled) return;

  if (!_mqtt.connected()) {
    unsigned long now = millis();
    if (now - _last_mqtt_retry < 5000UL) return;
    _last_mqtt_retry = now;
    if (!mqttConnect()) return;
  }
  _mqtt.loop();

  if (!_discovery_sent) {
    publishDiscovery();
    _discovery_sent = true;
  }

  unsigned long now = millis();
  if (now - _last_publish >= (unsigned long)_cfg.telemetry_s * 1000UL) {
    _last_publish = now;
    Telemetry t;
    if (_telem) _telem(t);
    publishState(t);
  }
}

void NetServices::publishState(const Telemetry& t) {
  char topic[80];
  topicState(topic, sizeof(topic));
  char payload[640];
  int len = snprintf(payload, sizeof(payload),
    "{\"batt_v\":%.2f,\"batt_pct\":%.0f,\"uptime_s\":%u,\"free_heap\":%u,"
    "\"neighbours\":%u,\"rx_count\":%u,\"tx_count\":%u,\"last_rssi\":%d,\"last_snr\":%.1f",
    t.batt_v, t.batt_pct, (unsigned)t.uptime_s, (unsigned)t.free_heap,
    (unsigned)t.neighbours, (unsigned)t.rx_count, (unsigned)t.tx_count,
    (int)t.last_rssi, t.last_snr);
  if (len < 0 || len >= (int)sizeof(payload)) return;

  // Append only sensor fields that were actually decoded (NAN = absent).
  struct { float v; const char* fmt; } opt[] = {
    {t.sens_temp, ",\"sens_temp\":%.1f"},
    {t.sens_hum,  ",\"sens_hum\":%.1f"},
    {t.sens_pres, ",\"sens_pres\":%.1f"},
    {t.panel_v,   ",\"panel_v\":%.2f"},
    {t.panel_a,   ",\"panel_a\":%.3f"},
    {t.panel_w,   ",\"panel_w\":%.2f"},
    {t.chg_v,     ",\"chg_v\":%.2f"},
    {t.chg_a,     ",\"chg_a\":%.3f"},
    {t.chg_w,     ",\"chg_w\":%.2f"},
    {t.load_v,    ",\"load_v\":%.2f"},
    {t.load_a,    ",\"load_a\":%.3f"},
    {t.load_w,    ",\"load_w\":%.2f"},
  };
  for (auto& o : opt) {
    if (isnan(o.v) || len >= (int)sizeof(payload) - 1) continue;
    int w = snprintf(payload + len, sizeof(payload) - len, o.fmt, o.v);
    if (w > 0 && w < (int)sizeof(payload) - len) len += w;
  }

  if (len < (int)sizeof(payload) - 1) { payload[len++] = '}'; payload[len] = 0; }
  _mqtt.publish(topic, payload, true);
}

void NetServices::publishDiscovery() {
  char state_topic[80];
  topicState(state_topic, sizeof(state_topic));
  char status_topic[80];
  topicStatus(status_topic, sizeof(status_topic));

  // Sample telemetry once so we only advertise sensor entities present at boot.
  Telemetry sample;
  if (_telem) _telem(sample);

  for (const MetricDef& m : METRICS) {
    if (m.opt && isnan(sample.*(m.opt))) continue;  // optional sensor not detected

    char cfg_topic[110];
    snprintf(cfg_topic, sizeof(cfg_topic),
             "homeassistant/sensor/mc_%s_%s/config", _node_id, m.field);

    char payload[640];
    int len = snprintf(payload, sizeof(payload),
      "{\"name\":\"%s\",\"uniq_id\":\"mc_%s_%s\",\"stat_t\":\"%s\","
      "\"val_tpl\":\"{{ value_json.%s }}\",\"stat_cla\":\"%s\","
      "\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
      "\"dev\":{\"ids\":[\"mc_%s\"],\"name\":\"MeshCore %s\",\"mf\":\"MeshCore\",\"mdl\":\"Repeater\"}",
      m.name, _node_id, m.field, state_topic, m.field, m.stat_cla,
      status_topic, _node_id, _node_id);
    if (len < 0 || len >= (int)sizeof(payload)) continue;
    if (m.unit)
      len += snprintf(payload + len, sizeof(payload) - len, ",\"unit_of_meas\":\"%s\"", m.unit);
    if (len < (int)sizeof(payload) - 1 && m.dev_cla)
      len += snprintf(payload + len, sizeof(payload) - len, ",\"dev_cla\":\"%s\"", m.dev_cla);
    if (len < (int)sizeof(payload) - 1)
      snprintf(payload + len, sizeof(payload) - len, "}");

    _mqtt.publish(cfg_topic, payload, true);
    _mqtt.loop();
  }
}
