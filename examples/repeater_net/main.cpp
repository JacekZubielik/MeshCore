#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "MyMesh.h"

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

#ifdef WITH_NET_SERVICES
#include <helpers/net/NetServices.h>
#include <CayenneLPP.h>
#include <helpers/sensors/LPPDataHelpers.h>   // LPPReader + LPP_* type constants
static NetServices net;
static char node_id[16];

static float batt_pct_from_mv(uint16_t mv) {
  float p = (mv - 3300) / (4200.0f - 3300.0f) * 100.0f;
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return p;
}

// Bridge a network CLI line into the repeater CLI (same handler as USB/LoRa).
static void net_cmd(const char* cmd, char* reply, size_t reply_len) {
  (void)reply_len;
  char buf[160];
  strncpy(buf, cmd, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  reply[0] = 0;
  the_mesh.handleCommand(0, buf, reply);
}

static void net_telem(Telemetry& t) {
  RepeaterStats s;
  the_mesh.getRepeaterStats(s);
  t.batt_v     = s.batt_milli_volts / 1000.0f;
  t.batt_pct   = batt_pct_from_mv(s.batt_milli_volts);
  t.uptime_s   = s.total_up_time_secs;
  t.free_heap  = ESP.getFreeHeap();
  t.neighbours = the_mesh.getNeighbourCount();
  t.rx_count   = s.n_packets_recv;
  t.tx_count   = s.n_packets_sent;
  t.last_rssi  = s.last_rssi;
  t.last_snr   = s.last_snr / 4.0f;   // RepeaterStats stores SNR x4

  // Decode live I2C sensor telemetry via the same path as the 'sensor read' CLI
  // command (querySensors -> CayenneLPP -> LPPReader). Environment quantities map
  // directly by type. Voltage/current/power come from a multi-channel current
  // monitor (INA3221): each enabled channel is a distinct LPP channel carrying a
  // V+I+P triplet, so we accumulate per LPP channel and map positionally by
  // ascending channel number: 0 -> panel, 1 -> charge, 2 -> load. This assumes
  // the INA3221 is the only V/I/P source on the bus (VBAT is read natively).
  CayenneLPP lpp(160);
  lpp.reset();
  sensors.querySensors(0xFF, lpp);
  LPPReader r(lpp.getBuffer(), lpp.getSize());

  struct { uint8_t ch; float v, a, w; } vip[3];
  int nvip = 0;
  auto vipSlot = [&](uint8_t c) -> int {         // find-or-create accumulator for LPP channel c
    for (int i = 0; i < nvip; i++) if (vip[i].ch == c) return i;
    if (nvip < 3) { vip[nvip] = {c, NAN, NAN, NAN}; return nvip++; }
    return -1;                                    // >3 current channels: ignore extras
  };

  uint8_t ch, type;
  int guard = 0;
  while (r.readHeader(ch, type) && guard++ < 32) {
    float a = 0;
    int i;
    switch (type) {
      case LPP_TEMPERATURE:         r.readTemperature(a);      t.sens_temp = a; break;
      case LPP_RELATIVE_HUMIDITY:   r.readRelativeHumidity(a); t.sens_hum  = a; break;
      case LPP_BAROMETRIC_PRESSURE: r.readPressure(a);         t.sens_pres = a; break;
      case LPP_VOLTAGE: r.readVoltage(a); if ((i = vipSlot(ch)) >= 0) vip[i].v = a; break;
      case LPP_CURRENT: r.readCurrent(a); if ((i = vipSlot(ch)) >= 0) vip[i].a = a; break;
      case LPP_POWER:   r.readPower(a);   if ((i = vipSlot(ch)) >= 0) vip[i].w = a; break;
      default:          r.skipData(type);                               break;
    }
  }

  // Sort current-monitor channels by ascending LPP channel, then map by position.
  for (int p = 1; p < nvip; p++) {
    auto key = vip[p]; int q = p - 1;
    while (q >= 0 && vip[q].ch > key.ch) { vip[q + 1] = vip[q]; q--; }
    vip[q + 1] = key;
  }
  float* dst[3][3] = {
    {&t.panel_v, &t.panel_a, &t.panel_w},
    {&t.chg_v,   &t.chg_a,   &t.chg_w},
    {&t.load_v,  &t.load_a,  &t.load_w},
  };
  for (int p = 0; p < nvip && p < 3; p++) {
    *dst[p][0] = vip[p].v; *dst[p][1] = vip[p].a; *dst[p][2] = vip[p].w;
  }
}

static void net_set_time(uint32_t epoch) {
  // getCurrentTimeUnique() (MeshCore.h) is monotonic, so a small backward step
  // never reuses packet timestamps. Apply plausible corrections both directions
  // (fixes a clock that ran fast) + first-boot jump; reject only garbage.
  uint32_t now = rtc_clock.getCurrentTime();
  int32_t delta = (int32_t)(epoch - now);
  if (now < 1600000000UL || (delta > -3600 && delta < 3600)) {
    rtc_clock.setCurrentTime(epoch);
    Serial.printf("NetServices: RTC set %lu -> %lu\n", (unsigned long)now, (unsigned long)epoch);
  } else {
    Serial.printf("NetServices: RTC step rejected (%lu -> %lu)\n", (unsigned long)now, (unsigned long)epoch);
  }
}
#endif

void halt() {
  while (1) ;
}

static char command[160];

// For power saving
unsigned long POWERSAVING_FIRSTSLEEP_SECS = 120; // The first sleep (if enabled) from boot

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
static unsigned long userBtnDownAt = 0;
#define USER_BTN_HOLD_OFF_MILLIS 1500
#endif

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#if defined(MESH_DEBUG) && defined(NRF52_PLATFORM)
  // give some extra time for serial to settle so
  // boot debug messages can be seen on terminal
  delay(5000);
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) {
    MESH_DEBUG_PRINTLN("Radio init failed!");
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Repeater ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();

  the_mesh.begin(fs);

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

#ifdef WITH_NET_SERVICES
  snprintf(node_id, sizeof(node_id), "%02x%02x%02x",
           the_mesh.self_id.pub_key[0], the_mesh.self_id.pub_key[1], the_mesh.self_id.pub_key[2]);
  board.setInhibitSleep(true);   // always-on WiFi: never light-sleep the repeater
  #ifdef PIN_USER_BTN
    pinMode(PIN_USER_BTN, INPUT_PULLUP);
  #endif
  net.begin(node_id, net_cmd, net_telem, net_set_time);
  Serial.print("NetServices node id: "); Serial.println(node_id);
#endif

  board.onBootComplete();
}

void loop() {
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
  // Hold the user button to power off the SenseCAP Solar repeater.
  int btnState = digitalRead(PIN_USER_BTN);
  if (btnState == LOW) {
    if (userBtnDownAt == 0) {
      userBtnDownAt = millis();
    } else if ((unsigned long)(millis() - userBtnDownAt) >= USER_BTN_HOLD_OFF_MILLIS) {
      Serial.println("Powering off...");
      board.powerOff();  // does not return
    }
  } else {
    userBtnDownAt = 0;
  }
#endif

#ifdef WITH_NET_SERVICES
  net.loop();
  #ifdef PIN_USER_BTN
  {
    static int last_btn = HIGH;
    static unsigned long btn_down_at = 0;
    int b = digitalRead(PIN_USER_BTN);
    if (b == LOW && last_btn == HIGH) btn_down_at = millis();
    if (b == HIGH && last_btn == LOW) {
      unsigned long held = millis() - btn_down_at;
      if (held > 40 && held < 1500) {   // short press = toggle WiFi
        net.setWifiEnabled(!net.isWifiEnabled());
        Serial.print("Button: WiFi -> "); Serial.println(net.isWifiEnabled() ? "ON" : "OFF");
      }
    }
    last_btn = b;
  }
  #endif
#endif

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

#ifndef WITH_NET_SERVICES
  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#else
    if (the_mesh.millisHasNowPassed(POWERSAVING_FIRSTSLEEP_SECS * 1000)) { // To check if it is time to sleep
      board.sleep(30); // Sleep. Wake up after a while or when receiving a LoRa packet
    }
#endif
  }
#endif
}
