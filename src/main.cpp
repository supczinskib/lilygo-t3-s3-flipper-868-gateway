#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SPI.h>
#include <SSD1306Wire.h>
#include <OLEDDisplayFonts.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_system.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <vector>

#include "flipper_parser.h"

namespace {

constexpr char FIRMWARE_VERSION[] = "1.0.2";
constexpr int8_t TX_POWER_DBM = 20;
// Preserve the polarity of Flipper asynchronous 2-FSK captures.
constexpr bool FLIPPER_FSK_DATA_INVERTED = false;
constexpr char HOSTNAME[] = "lilygo-868";
constexpr char AP_SSID[] = "LILYGO-868-SETUP";
constexpr char AP_PASSWORD[] = "lilygo868";
constexpr char DEFAULT_WEB_USERNAME[] = "admin";
constexpr char DEFAULT_WEB_PASSWORD[] = "lilygo868";
constexpr char WEB_REALM[] = "LILYGO 868 Gateway";
constexpr char SESSION_COOKIE_NAME[] = "lilygo_session";
constexpr uint32_t SESSION_COOKIE_MAX_AGE = 604800;
constexpr char LITTLEFS_LABEL[] = "littlefs";
constexpr uint8_t SLOT_COUNT = 16;
constexpr uint8_t DEFAULT_REPEATS = 1;
constexpr uint16_t DEFAULT_GAP_MS = 100;
constexpr uint32_t DISPLAY_TIMEOUT_MS = 60000;
constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 15000;
constexpr uint32_t WIFI_INTERFACE_RESTART_AFTER_MS = 60000;
constexpr uint32_t WIFI_DEVICE_RESTART_AFTER_MS = 300000;
constexpr size_t MAX_UPLOAD_BYTES = 65536;

// Official LILYGO T3-S3 v1.2/v1.3 SX1276 pin map.
constexpr uint8_t RADIO_SCK = 5;
constexpr uint8_t RADIO_MISO = 3;
constexpr uint8_t RADIO_MOSI = 6;
constexpr uint8_t RADIO_CS = 7;
constexpr uint8_t RADIO_RESET = 8;
constexpr uint8_t RADIO_DIO0 = 9;
constexpr uint8_t RADIO_DIO1 = 33;
constexpr uint8_t RADIO_DIO2 = 34;
constexpr uint8_t OLED_SDA = 18;
constexpr uint8_t OLED_SCL = 17;
constexpr uint8_t BUTTON_PIN = 0;
constexpr uint8_t BOARD_LED = 37;

SPIClass radio_spi(FSPI);
SPISettings radio_spi_settings(4000000, MSBFIRST, SPI_MODE0);
Module radio_module(RADIO_CS, RADIO_DIO0, RADIO_RESET, RADIO_DIO1,
                    radio_spi, radio_spi_settings);
SX1276 radio(&radio_module);
// The board uses a 128x64 SSD1306 display on a 400 kHz I2C bus.
SSD1306Wire display(0x3C, OLED_SDA, OLED_SCL, GEOMETRY_128_64, I2C_ONE,
                    400000);
WebServer server(80);
DNSServer dns_server;
Preferences preferences;

bool radio_ready = false;
bool storage_ready = false;
bool access_point_active = false;
bool display_enabled = true;
// Optional rotation relative to the board-mounting orientation.
bool display_rotated = false;
bool display_awake = false;
bool update_failed = false;
bool update_authorized = false;
bool upload_failed = false;
bool upload_authorized = false;
bool mdns_active = false;
bool web_server_restart_pending = false;
uint8_t upload_slot = 0;
String upload_name;
String upload_error;
std::vector<uint8_t> upload_buffer;
String web_username = DEFAULT_WEB_USERNAME;
String web_password = DEFAULT_WEB_PASSWORD;
String web_session_token;
uint8_t selected_slot = 0;
uint32_t display_last_activity = 0;
uint32_t wifi_disconnected_since = 0;
uint32_t wifi_last_reconnect_attempt = 0;
bool wifi_interface_restarted = false;
String last_result = "Ready";

bool button_down = false;
uint32_t button_down_since = 0;
uint32_t button_last_change = 0;

String slot_path(uint8_t slot) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "/slots/%02u.sub", slot);
  return String(buffer);
}

String slot_name_key(uint8_t slot) {
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "n%02u", slot);
  return String(buffer);
}

bool slot_exists(uint8_t slot) {
  return storage_ready && slot < SLOT_COUNT &&
         LittleFS.exists(slot_path(slot));
}

String slot_name(uint8_t slot) {
  preferences.begin("slots", true);
  String value = preferences.getString(slot_name_key(slot).c_str(), "");
  preferences.end();
  if (value.isEmpty() && slot_exists(slot)) value = "Signal " + String(slot);
  return value;
}

void save_slot_name(uint8_t slot, const String &name) {
  preferences.begin("slots", false);
  preferences.putString(slot_name_key(slot).c_str(), name);
  preferences.end();
}

String json_escape(const String &input) {
  String result;
  result.reserve(input.length() + 12);
  for (size_t index = 0; index < input.length(); ++index) {
    const char character = input[index];
    switch (character) {
      case '\\': result += "\\\\"; break;
      case '"': result += "\\\""; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<uint8_t>(character) >= 0x20) result += character;
        break;
    }
  }
  return result;
}

String clean_name(String value) {
  value.trim();
  String cleaned;
  cleaned.reserve(std::min<size_t>(value.length(), 40));
  for (size_t index = 0; index < value.length() && cleaned.length() < 40;
       ++index) {
    const char character = value[index];
    if (static_cast<uint8_t>(character) >= 0x20 && character != '<' &&
        character != '>' && character != '"') {
      cleaned += character;
    }
  }
  return cleaned;
}

void load_web_credentials() {
  preferences.begin("security", true);
  web_username = preferences.getString("username", DEFAULT_WEB_USERNAME);
  web_password = preferences.getString("password", DEFAULT_WEB_PASSWORD);
  preferences.end();
  if (web_username.isEmpty()) web_username = DEFAULT_WEB_USERNAME;
  if (web_password.length() < 8) web_password = DEFAULT_WEB_PASSWORD;
}

bool save_web_credentials(const String &username, const String &password) {
  preferences.begin("security", false);
  const bool username_ok =
      preferences.putString("username", username) == username.length();
  const bool password_ok =
      preferences.putString("password", password) == password.length();
  preferences.end();
  if (username_ok && password_ok) {
    web_username = username;
    web_password = password;
    return true;
  }
  return false;
}

String create_session_token() {
  char token[33];
  for (uint8_t part = 0; part < 4; ++part) {
    snprintf(token + part * 8, 9, "%08lx",
             static_cast<unsigned long>(esp_random()));
  }
  return String(token);
}

void rotate_web_session() {
  web_session_token = create_session_token();
}

bool has_valid_session_cookie() {
  if (web_session_token.isEmpty() || !server.hasHeader("Cookie")) return false;
  String cookies = server.header("Cookie");
  const String expected =
      String(SESSION_COOKIE_NAME) + "=" + web_session_token;
  int start = 0;
  while (start < cookies.length()) {
    int end = cookies.indexOf(';', start);
    if (end < 0) end = cookies.length();
    String cookie = cookies.substring(start, end);
    cookie.trim();
    if (cookie == expected) return true;
    start = end + 1;
  }
  return false;
}

void issue_session_cookie() {
  server.sendHeader(
      "Set-Cookie",
      String(SESSION_COOKIE_NAME) + "=" + web_session_token +
          "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
          String(SESSION_COOKIE_MAX_AGE));
  server.sendHeader("Cache-Control", "no-store");
}

void clear_session_cookie() {
  server.sendHeader(
      "Set-Cookie", String(SESSION_COOKIE_NAME) +
                        "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
  server.sendHeader("Cache-Control", "no-store");
}

bool valid_web_username(const String &username) {
  if (username.isEmpty() || username.length() > 32) return false;
  for (size_t index = 0; index < username.length(); ++index) {
    const char character = username[index];
    if (!isalnum(static_cast<unsigned char>(character)) && character != '-' &&
        character != '_' && character != '.') {
      return false;
    }
  }
  return true;
}

bool initialize_storage() {
  // Select the custom LittleFS partition label from partitions.csv.
  if (!LittleFS.begin(true, "/littlefs", 10, LITTLEFS_LABEL)) return false;
  if (!LittleFS.exists("/slots") && !LittleFS.mkdir("/slots")) return false;

  File probe = LittleFS.open("/.storage-check", FILE_WRITE);
  if (!probe) return false;
  const bool written = probe.print("ok") == 2;
  probe.close();
  LittleFS.remove("/.storage-check");
  return written;
}

void persist_runtime_settings() {
  preferences.begin("runtime", false);
  preferences.putBool("display", display_enabled);
  preferences.putBool("rotate_ui", display_rotated);
  preferences.putUChar("slot", selected_slot);
  preferences.end();
}

void apply_display_orientation() {
  display.resetOrientation();
  if (!display_rotated) display.flipScreenVertically();
  display.clear();
  display.display();
  if (!display_enabled) display.displayOff();
}

void display_power(bool on) {
  if (!display_enabled) on = false;
  if (on) {
    display.displayOn();
  } else {
    display.displayOff();
  }
  display_awake = on;
  if (on) display_last_activity = millis();
}

String fit_display_line(String value, uint16_t max_width = 127) {
  value.replace('\n', ' ');
  value.replace('\r', ' ');
  if (display.getStringWidth(value) <= max_width) return value;

  while (!value.isEmpty() &&
         display.getStringWidth(value + "..") > max_width) {
    value.remove(value.length() - 1);
  }
  return value + "..";
}

void display_lines(const String &line1, const String &line2,
                   const String &line3 = "", const String &line4 = "") {
  if (!display_enabled) return;
  display_power(true);
  display.clear();
  display.setColor(WHITE);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_16);
  display.drawString(0, 0, fit_display_line(line1));
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 21, fit_display_line(line2));
  if (!line3.isEmpty()) display.drawString(0, 35, fit_display_line(line3));
  if (!line4.isEmpty()) display.drawString(0, 49, fit_display_line(line4));
  display.display();
}

void show_selected_slot() {
  String name = slot_name(selected_slot);
  if (name.isEmpty()) name = "Empty";
  const String address = access_point_active
                             ? WiFi.softAPIP().toString()
                             : WiFi.localIP().toString();
  display_lines("LILYGO 868", "Slot " + String(selected_slot) + ": " + name,
                address, "Short: next  Long: TX");
}

void show_network_setup() {
  display_lines(AP_SSID, "Password: " + String(AP_PASSWORD),
                "Open: 192.168.4.1", "Configure home Wi-Fi");
}

void show_info() {
  const String network = access_point_active ? String(AP_SSID) : WiFi.SSID();
  const String address = access_point_active
                             ? WiFi.softAPIP().toString()
                             : WiFi.localIP().toString();
  display_lines("Version " + String(FIRMWARE_VERSION), network, address,
                radio_ready ? "SX1276 ready" : "SX1276 ERROR");
}

// Protect short timings from task jitter without blocking long frame gaps.
void write_direct_signal(const std::vector<int32_t> &timings, bool invert) {
  constexpr uint32_t LONG_GAP_US = 5000;
  bool interrupts_blocked = false;

  for (const int32_t signed_duration : timings) {
    const uint32_t duration_us =
        static_cast<uint32_t>(labs(signed_duration));
    const bool level = (signed_duration > 0) ^ invert;

    if (duration_us >= LONG_GAP_US) {
      if (!interrupts_blocked) {
        noInterrupts();
      }
      gpio_set_level(static_cast<gpio_num_t>(RADIO_DIO2), level);
      interrupts();
      interrupts_blocked = false;
      delayMicroseconds(duration_us);
      continue;
    }

    if (!interrupts_blocked) {
      noInterrupts();
      interrupts_blocked = true;
    }
    gpio_set_level(static_cast<gpio_num_t>(RADIO_DIO2), level);
    delayMicroseconds(duration_us);
  }

  gpio_set_level(static_cast<gpio_num_t>(RADIO_DIO2), LOW);
  if (interrupts_blocked) interrupts();
}

bool initialize_radio_for_signal(const flipper::RawSignal &signal,
                                 String &error) {
  const float frequency_mhz = signal.frequency_hz / 1000000.0F;
  int16_t state = radio.beginFSK(frequency_mhz, 4.8, 47.6, 125.0,
                                 TX_POWER_DBM, 16,
                                 false);
  if (state != RADIOLIB_ERR_NONE) {
    error = "SX1276 initialization failed (code " + String(state) + ")";
    radio_ready = false;
    return false;
  }
  // Use PA_BOOST at +20 dBm with the required protected current limit.
  state = radio.setCurrentLimit(140);
  if (state == RADIOLIB_ERR_NONE) state = radio.setOutputPower(TX_POWER_DBM);
  if (state == RADIOLIB_ERR_NONE) {
    state = radio.setDataShaping(RADIOLIB_SHAPING_NONE);
  }
  // Asynchronous Flipper RAW requires direct DATA input without bit sync.
  if (state == RADIOLIB_ERR_NONE) state = radio.disableBitSync();
  if (state != RADIOLIB_ERR_NONE) {
    error = "SX1276 high-power setup failed (code " + String(state) + ")";
    radio_ready = false;
    return false;
  }
  radio_ready = true;
  return true;
}

bool transmit_slot(uint8_t slot, uint8_t repeats, uint16_t gap_ms,
                   bool invert, String &result) {
  if (slot >= SLOT_COUNT || !slot_exists(slot)) {
    result = "Selected slot is empty";
    return false;
  }

  flipper::RawSignal signal;
  if (!flipper::parse_file(LittleFS, slot_path(slot), signal, result)) {
    return false;
  }
  if (!initialize_radio_for_signal(signal, result)) return false;

  if (signal.timings.empty()) {
    result = "Signal does not contain transmit data";
    return false;
  }

  pinMode(RADIO_DIO2, OUTPUT);
  digitalWrite(RADIO_DIO2, LOW);
  display_lines("TRANSMITTING", "Slot " + String(slot), slot_name(slot),
                String(signal.frequency_hz / 1000000.0F, 3) + " MHz");
  digitalWrite(BOARD_LED, HIGH);

  bool sent = true;
  for (uint8_t repeat = 0; repeat < repeats; ++repeat) {
    const int16_t direct_state = radio.transmitDirect();
    if (direct_state != RADIOLIB_ERR_NONE) {
      result = "SX1276 direct TX failed (code " + String(direct_state) + ")";
      sent = false;
      break;
    }
    delayMicroseconds(250);
    write_direct_signal(signal.timings, invert);
    radio.standby();
    digitalWrite(RADIO_DIO2, LOW);
    if (repeat + 1U < repeats && gap_ms > 0) delay(gap_ms);
  }

  digitalWrite(BOARD_LED, LOW);
  pinMode(RADIO_DIO2, OUTPUT);
  digitalWrite(RADIO_DIO2, LOW);

  if (!sent) {
    display_lines("TX FAILED", "Slot " + String(slot), result);
    return false;
  }

  result = "Sent slot " + String(slot) + " (" + slot_name(slot) +
           ") x" + String(repeats);
  display_lines("SENT", "Slot " + String(slot), slot_name(slot),
                "Repeat x" + String(repeats));
  return true;
}

String status_json() {
  String json;
  json.reserve(5000);
  json = "{\"status\":\"" + json_escape(last_result) + "\",\"version\":\"" +
         String(FIRMWARE_VERSION) + "\",\"radio\":" +
         (radio_ready ? "true" : "false") + ",\"tx_power\":" +
         String(TX_POWER_DBM) + ",\"display\":" +
         (display_enabled ? "true" : "false") + ",\"display_rotated\":" +
         (display_rotated ? "true" : "false") + ",\"selected\":" +
         String(selected_slot) + ",\"storage\":" +
         (storage_ready ? "true" : "false") + ",\"storage_total\":" +
         String(storage_ready ? LittleFS.totalBytes() : 0) +
         ",\"storage_used\":" +
         String(storage_ready ? LittleFS.usedBytes() : 0) + ",\"ap\":" +
         (access_point_active ? "true" : "false") + ",\"ip\":\"" +
         (access_point_active ? WiFi.softAPIP().toString()
                              : WiFi.localIP().toString()) +
         "\",\"slots\":[";

  for (uint8_t slot = 0; slot < SLOT_COUNT; ++slot) {
    if (slot > 0) json += ',';
    json += "{\"slot\":" + String(slot) + ",\"used\":" +
            (slot_exists(slot) ? "true" : "false") + ",\"name\":\"" +
            json_escape(slot_name(slot)) + "\"";
    if (slot_exists(slot)) {
      flipper::RawSignal signal;
      String error;
      if (flipper::parse_file(LittleFS, slot_path(slot), signal, error)) {
        json += ",\"frequency\":" + String(signal.frequency_hz) +
                ",\"timings\":" + String(signal.timings.size());
      } else {
        json += ",\"error\":\"" + json_escape(error) + "\"";
      }
    }
    json += '}';
  }
  json += "]}";
  return json;
}

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>LILYGO 868 Gateway</title><style>
:root{color-scheme:light dark;--blue:#03a9e8;--card:#fff;--bg:#f6f7f8;--line:#d8dadd;--text:#202124;--muted:#6c7177}
@media(prefers-color-scheme:dark){:root{--card:#202124;--bg:#111315;--line:#45484d;--text:#f2f3f4;--muted:#aeb3b8}}
*{box-sizing:border-box}body{font-family:Arial,sans-serif;font-size:16px;line-height:1.45;background:var(--bg);color:var(--text);max-width:1100px;margin:0 auto;padding:24px 18px}h1{font-size:32px;line-height:1.2;text-align:center;margin:8px 0 4px}h2{font-size:20px;line-height:1.3;margin:0 0 18px}.sub{text-align:center;color:var(--muted);font-size:15px;margin-bottom:22px}.nav{display:flex;gap:22px;flex-wrap:wrap;margin-bottom:18px}.nav a{font-size:15px;color:var(--blue);text-decoration:none}.nav a:hover{text-decoration:underline}.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:22px;margin:16px 0}.grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:16px}label{display:block;font-size:14px;font-weight:600;color:var(--muted);margin:0 0 6px}select,input{width:100%;height:46px;font:inherit;font-size:16px;padding:10px 12px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--text)}input[type=file]{padding:8px}button,.button{border:0;border-radius:8px;padding:11px 17px;font-size:15px;font-weight:700;cursor:pointer}.primary{background:var(--blue);color:white}.danger{background:#d32f2f;color:white}.secondary{background:#e5e7eb;color:#202124}.actions{display:flex;gap:14px;align-items:center;flex-wrap:wrap}.status{border-left:4px solid var(--blue)}.status h2{margin-bottom:8px}.status p{font-size:17px;margin:5px 0 3px}table{width:100%;border-collapse:collapse;font-size:15px}th,td{text-align:left;padding:12px 8px;border-bottom:1px solid var(--line)}th{font-size:13px;color:var(--muted)}.muted{color:var(--muted);font-size:14px}.switch{display:flex;align-items:center;gap:8px;font-size:15px;font-weight:400;color:var(--text);margin:0}.switch input{width:20px;height:20px;margin:0}.slot-actions{display:flex;gap:7px;flex-wrap:wrap}@media(max-width:720px){body{font-size:15px;padding:14px 8px}h1{font-size:27px}h2{font-size:19px}.card{padding:17px}.grid{grid-template-columns:1fr}.nav{gap:16px}select,input{font-size:16px}th:nth-child(3),td:nth-child(3),th:nth-child(4),td:nth-child(4){display:none}}
</style></head><body>
<div class="nav"><a href="/">Control</a><a href="/network">Wi-Fi</a><a href="/security">Login &amp; password</a><a href="/update">Firmware update</a></div>
<h1>LILYGO 868 Gateway</h1><div class="sub">T3-S3 · SX1276 · Flipper RAW signal slots</div>
<div class="card status"><h2>LIVE DEVICE STATUS</h2><p id="status">Loading…</p><span class="muted" id="details"></span></div>
<div class="card"><h2>Send stored signal</h2><div class="grid">
<div><label>Slot</label><select id="sendSlot"></select></div><div><label>File repetitions</label><input id="repeats" type="number" min="1" max="10" value="1"></div><div><label>Gap (ms)</label><input id="gap" type="number" min="0" max="2000" value="100"></div>
</div><div class="actions" style="margin-top:16px"><button class="primary" onclick="sendSelected()">SEND</button><label class="switch"><input id="display" type="checkbox" onchange="setDisplay(this.checked)"> OLED enabled</label><label class="switch"><input id="displayRotation" type="checkbox" onchange="setDisplayRotation(this.checked)"> Rotate OLED 180&deg;</label></div></div>
<div class="card"><h2>Import Flipper signal</h2><div class="grid"><div><label>Target slot</label><select id="importSlot"></select></div><div><label>Signal name</label><input id="signalName" maxlength="40" placeholder="e.g. Pool LED ON"></div><div><label>Flipper .sub file</label><input id="signalFile" type="file" accept=".sub,text/plain"></div></div><div class="actions" style="margin-top:12px"><button class="primary" onclick="importSignal(false)">IMPORT</button><button class="secondary" onclick="importSignal(true)">IMPORT & TEST</button></div></div>
<div class="card"><h2>Stored slots</h2><table><thead><tr><th>Slot</th><th>Name</th><th>Frequency</th><th>RAW timings</th><th>Actions</th></tr></thead><tbody id="slots"></tbody></table></div>
<script>
let state=null;const $=id=>document.getElementById(id);function esc(s){const d=document.createElement('div');d.textContent=s||'';return d.innerHTML}
async function load(){try{const r=await fetch('/api/status',{cache:'no-store'});state=await r.json();$('status').textContent=state.status;$('details').textContent=`Firmware ${state.version} · IP ${state.ip} · SX1276 ${state.radio?'ready':'error'} · storage ${state.storage?'ready':'ERROR'} · TX power +${state.tx_power} dBm`;$('display').checked=state.display;$('displayRotation').checked=state.display_rotated;render()}catch(e){$('status').textContent='Device connection failed'}}
function render(){for(const id of ['sendSlot','importSlot']){const s=$(id),old=s.value;s.innerHTML='';state.slots.forEach(x=>{const o=document.createElement('option');o.value=x.slot;o.textContent=`Signal ${x.slot}${x.used?' — '+x.name:' — Empty'}`;s.appendChild(o)});s.value=old||String(state.selected)}const body=$('slots');body.innerHTML='';state.slots.forEach(x=>{const tr=document.createElement('tr');tr.innerHTML=`<td>${x.slot}</td><td>${x.used?esc(x.name):'<span class=muted>Empty</span>'}</td><td>${x.frequency?(x.frequency/1e6).toFixed(3)+' MHz':'—'}</td><td>${x.timings||'—'}</td><td><div class=slot-actions>${x.used?`<button class=primary onclick="sendSlot(${x.slot})">SEND</button><button class=danger onclick="clearSlot(${x.slot})">CLEAR</button>`:'—'}</div></td>`;body.appendChild(tr)})}
async function api(url,options){$('status').textContent='Working…';const r=await fetch(url,options);const t=await r.text();$('status').textContent=t;if(!r.ok)throw new Error(t);await load();return t}
async function sendSlot(slot){try{await api(`/api/send?slot=${slot}&repeats=${$('repeats').value}&gap_ms=${$('gap').value}`,{method:'POST'})}catch(e){}}
function sendSelected(){sendSlot(Number($('sendSlot').value))}
async function setDisplay(enabled){try{await api(`/api/display?enabled=${enabled?1:0}`,{method:'POST'})}catch(e){}}
async function setDisplayRotation(rotated){try{await api(`/api/display-rotation?rotated=${rotated?1:0}`,{method:'POST'})}catch(e){}}
async function clearSlot(slot){if(!confirm(`Clear signal slot ${slot}?`))return;try{await api(`/api/clear?slot=${slot}`,{method:'POST'})}catch(e){}}
async function importSignal(test){const f=$('signalFile').files[0];if(!f){alert('Choose a Flipper .sub file first.');return}const slot=$('importSlot').value,name=encodeURIComponent($('signalName').value);const form=new FormData();form.append('file',f);try{await api(`/api/import?slot=${slot}&name=${name}`,{method:'POST',body:form});$('signalFile').value='';if(test)await sendSlot(Number(slot))}catch(e){alert(e.message)}}
load();setInterval(load,10000);
</script></body></html>
)HTML";

String page_header(const char *title) {
  String page;
  page.reserve(1800);
  page += F("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>");
  page += title;
  page += F("</title><style>*{box-sizing:border-box}body{font-family:Arial,sans-serif;font-size:16px;line-height:1.45;max-width:820px;margin:28px auto;padding:0 18px;background:#f6f7f8;color:#202124}h1{font-size:30px;line-height:1.2}.card{background:white;border:1px solid #ddd;padding:22px;border-radius:12px;margin:16px 0}label{display:block;font-size:14px;font-weight:600;color:#666;margin-top:7px}input{width:100%;height:46px;font:inherit;font-size:16px;padding:10px 12px;margin:5px 0 16px;border:1px solid #aaa;border-radius:8px}button{background:#03a9e8;color:white;border:0;border-radius:8px;padding:11px 17px;font-size:15px;font-weight:bold}.nav{display:flex;gap:22px;flex-wrap:wrap}.nav a{font-size:15px;color:#0288d1;text-decoration:none}@media(prefers-color-scheme:dark){body{background:#111315;color:#f2f3f4}.card,input{background:#202124;color:#f2f3f4;border-color:#555}label{color:#bbb}}</style></head><body><div class=nav><a href='/'>Control</a><a href='/network'>Wi-Fi</a><a href='/security'>Login &amp; password</a><a href='/update'>Firmware update</a></div>");
  return page;
}

String network_page(const String &message = "") {
  String page = page_header("LILYGO 868 Wi-Fi");
  page += F("<h1>Wi-Fi configuration</h1><div class=card>");
  if (!message.isEmpty()) page += "<p>" + message + "</p>";
  page += F("<form method=post action='/network'><label>Home Wi-Fi name (SSID)</label><input name=ssid maxlength=32 required><label>Password</label><input name=password type=password maxlength=63><button type=submit>Save and restart</button></form><p>The device will restart and connect to the selected network. If it cannot connect, the setup access point will return.</p></div></body></html>");
  return page;
}

String update_page() {
  String page = page_header("LILYGO 868 firmware update");
  page += F("<h1>Firmware update</h1><div class=card><form method=post action='/update' enctype='multipart/form-data'><input type=file name=firmware accept='.bin,application/octet-stream' required><button type=submit>Install firmware</button></form><p>Use the OTA application file, not the factory image. Keep power connected until the device restarts.</p></div></body></html>");
  return page;
}

String security_page(const String &message = "") {
  String page = page_header("LILYGO 868 security");
  page += F("<h1>Web interface security</h1><div class=card>");
  if (!message.isEmpty()) page += "<p>" + message + "</p>";
  page += F("<form method=post action='/security'><label>Login</label><input name=username autocomplete=username maxlength=32 required value='");
  page += web_username;
  page += F("'><label>New password</label><input name=password type=password autocomplete=new-password minlength=8 maxlength=64 required><label>Repeat new password</label><input name=confirm type=password autocomplete=new-password minlength=8 maxlength=64 required><button type=submit>Save credentials</button></form><p>The new credentials protect the control, import and firmware-update pages. If they are ever forgotten, connect to the LILYGO setup access point and open this page again.</p></div></body></html>");
  return page;
}

bool read_wifi_credentials(String &ssid, String &password) {
  preferences.begin("network", true);
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");
  preferences.end();
  return !ssid.isEmpty();
}

bool save_wifi_credentials(const String &ssid, const String &password) {
  preferences.begin("network", false);
  const bool ok = preferences.putString("ssid", ssid) > 0;
  preferences.putString("password", password);
  preferences.end();
  return ok;
}

bool connect_wifi() {
  String ssid;
  String password;
  if (!read_wifi_credentials(ssid, password)) return false;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect(true);
  return false;
}

void restart_wifi_interface() {
  String ssid;
  String password;
  if (!read_wifi_credentials(ssid, password)) return;

  if (mdns_active) {
    MDNS.end();
    mdns_active = false;
  }
  server.stop();
  web_server_restart_pending = true;

  WiFi.disconnect(true, false);
  delay(100);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());
}

void maintain_wifi() {
  const uint32_t now = millis();
  if (access_point_active) {
    if (WiFi.status() == WL_CONNECTED) {
      dns_server.stop();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      access_point_active = false;
      wifi_last_reconnect_attempt = 0;
      mdns_active = MDNS.begin(HOSTNAME);
      show_selected_slot();
      return;
    }

    if (now - wifi_last_reconnect_attempt >= WIFI_RECONNECT_INTERVAL_MS) {
      wifi_last_reconnect_attempt = now;
      String ssid;
      String password;
      if (read_wifi_credentials(ssid, password)) {
        WiFi.setAutoReconnect(true);
        WiFi.begin(ssid.c_str(), password.c_str());
      }
    }
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifi_disconnected_since = 0;
    wifi_last_reconnect_attempt = 0;
    wifi_interface_restarted = false;
    if (web_server_restart_pending) {
      server.begin();
      web_server_restart_pending = false;
    }
    if (!mdns_active) mdns_active = MDNS.begin(HOSTNAME);
    return;
  }

  if (wifi_disconnected_since == 0) {
    wifi_disconnected_since = now;
    wifi_last_reconnect_attempt = now;
  }
  const uint32_t disconnected_for = now - wifi_disconnected_since;

  if (disconnected_for >= WIFI_DEVICE_RESTART_AFTER_MS) {
    ESP.restart();
  }

  if (!wifi_interface_restarted &&
      disconnected_for >= WIFI_INTERFACE_RESTART_AFTER_MS) {
    wifi_interface_restarted = true;
    wifi_last_reconnect_attempt = now;
    restart_wifi_interface();
    return;
  }

  if (now - wifi_last_reconnect_attempt >= WIFI_RECONNECT_INTERVAL_MS) {
    wifi_last_reconnect_attempt = now;
    if (!WiFi.reconnect()) {
      String ssid;
      String password;
      if (read_wifi_credentials(ssid, password)) {
        WiFi.begin(ssid.c_str(), password.c_str());
      }
    }
  }
}

bool request_is_authorized() {
  if (access_point_active || has_valid_session_cookie()) return true;
  if (!server.authenticate(web_username.c_str(), web_password.c_str())) {
    return false;
  }
  issue_session_cookie();
  return true;
}

bool require_authentication() {
  if (request_is_authorized()) return true;
  server.requestAuthentication(DIGEST_AUTH, WEB_REALM,
                               "Authentication required");
  return false;
}

void handle_send() {
  if (!require_authentication()) return;
  const int slot = server.arg("slot").toInt();
  int repeats = server.hasArg("repeats") ? server.arg("repeats").toInt()
                                          : DEFAULT_REPEATS;
  int gap_ms = server.hasArg("gap_ms") ? server.arg("gap_ms").toInt()
                                        : DEFAULT_GAP_MS;
  bool invert = FLIPPER_FSK_DATA_INVERTED;
  if (server.hasArg("invert")) {
    invert = server.arg("invert") == "1" || server.arg("invert") == "true";
  }
  if (slot < 0 || slot >= SLOT_COUNT || repeats < 1 || repeats > 10 ||
      gap_ms < 0 || gap_ms > 2000) {
    server.send(400, "text/plain", "Invalid transmission settings");
    return;
  }
  selected_slot = static_cast<uint8_t>(slot);
  persist_runtime_settings();
  String result;
  const bool ok = transmit_slot(selected_slot, static_cast<uint8_t>(repeats),
                                static_cast<uint16_t>(gap_ms), invert, result);
  last_result = result;
  server.send(ok ? 200 : 500, "text/plain", result);
}

void begin_upload() {
  upload_authorized = request_is_authorized();
  upload_failed = false;
  upload_error = "";
  upload_buffer.clear();
  upload_buffer.reserve(4096);
}

void handle_upload_data() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    begin_upload();
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!upload_authorized || upload_failed) return;
    if (upload_buffer.size() + upload.currentSize > MAX_UPLOAD_BYTES) {
      upload_failed = true;
      upload_error = "Signal file exceeds the 64 KiB limit";
      return;
    }
    upload_buffer.insert(upload_buffer.end(), upload.buf,
                         upload.buf + upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    upload_failed = true;
    upload_error = "Signal upload was interrupted";
  }
}

void finish_upload() {
  if (!require_authentication()) return;
  if (!upload_authorized) {
    server.send(401, "text/plain", "Authentication required");
    return;
  }
  if (!server.hasArg("slot")) {
    server.send(400, "text/plain", "Missing target slot");
    return;
  }
  const int requested_slot = server.arg("slot").toInt();
  if (requested_slot < 0 || requested_slot >= SLOT_COUNT) {
    server.send(400, "text/plain", "Target slot must be between 0 and 15");
    return;
  }
  upload_slot = static_cast<uint8_t>(requested_slot);
  upload_name = clean_name(server.arg("name"));

  if (upload_failed) {
    const String message = upload_error.isEmpty()
                               ? "Signal upload failed"
                               : upload_error;
    upload_buffer.clear();
    server.send(400, "text/plain", message);
    return;
  }
  if (upload_buffer.empty()) {
    server.send(400, "text/plain", "Uploaded signal file is empty");
    return;
  }

  flipper::RawSignal signal;
  String error;
  if (!flipper::parse_buffer(upload_buffer.data(), upload_buffer.size(),
                             signal, error)) {
    upload_buffer.clear();
    server.send(400, "text/plain", error);
    return;
  }

  if (!storage_ready) {
    upload_buffer.clear();
    server.send(500, "text/plain",
                "Signal storage is not mounted. Restart the device and check "
                "the storage status.");
    return;
  }

  const String destination = slot_path(upload_slot);
  const String staged = destination + ".new";
  const String backup = destination + ".bak";
  LittleFS.remove(staged);
  File slot_file = LittleFS.open(staged, FILE_WRITE);
  if (!slot_file) {
    upload_buffer.clear();
    server.send(500, "text/plain", "Could not create the signal slot file");
    return;
  }
  const size_t expected_size = upload_buffer.size();
  const size_t written = slot_file.write(upload_buffer.data(), expected_size);
  slot_file.flush();
  slot_file.close();
  upload_buffer.clear();
  if (written != expected_size || !LittleFS.exists(staged)) {
    LittleFS.remove(staged);
    server.send(500, "text/plain", "Could not store the signal slot file");
    return;
  }

  LittleFS.remove(backup);
  const bool had_previous = LittleFS.exists(destination);
  if (had_previous && !LittleFS.rename(destination, backup)) {
    LittleFS.remove(staged);
    server.send(500, "text/plain", "Could not prepare the existing slot");
    return;
  }
  if (!LittleFS.rename(staged, destination)) {
    if (had_previous) LittleFS.rename(backup, destination);
    LittleFS.remove(staged);
    server.send(500, "text/plain", "Could not activate the signal slot");
    return;
  }
  LittleFS.remove(backup);
  if (upload_name.isEmpty()) upload_name = "Signal " + String(upload_slot);
  save_slot_name(upload_slot, upload_name);
  selected_slot = upload_slot;
  persist_runtime_settings();
  last_result = "Imported slot " + String(upload_slot) + " (" +
                upload_name + ")";
  show_selected_slot();
  server.send(200, "text/plain", last_result);
}

void setup_routes() {
  const char *tracked_headers[] = {"Cookie"};
  server.collectHeaders(tracked_headers, 1);

  server.on("/", HTTP_GET, [] {
    if (!require_authentication()) return;
    if (access_point_active) {
      server.send(200, "text/html; charset=utf-8", network_page());
    } else {
      server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
    }
  });
  server.on("/api/status", HTTP_GET, [] {
    if (!require_authentication()) return;
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", status_json());
  });
  server.on("/api/send", HTTP_ANY, handle_send);
  server.on("/api/display", HTTP_ANY, [] {
    if (!require_authentication()) return;
    if (!server.hasArg("enabled")) {
      server.send(400, "text/plain", "Missing enabled setting");
      return;
    }
    display_enabled = server.arg("enabled") == "1" ||
                      server.arg("enabled") == "true";
    persist_runtime_settings();
    if (display_enabled) {
      show_selected_slot();
      last_result = "OLED enabled";
    } else {
      display_power(false);
      last_result = "OLED disabled";
    }
    server.send(200, "text/plain", last_result);
  });
  server.on("/api/display-rotation", HTTP_ANY, [] {
    if (!require_authentication()) return;
    if (!server.hasArg("rotated")) {
      server.send(400, "text/plain", "Missing rotated setting");
      return;
    }
    display_rotated = server.arg("rotated") == "1" ||
                      server.arg("rotated") == "true";
    persist_runtime_settings();
    apply_display_orientation();
    if (display_enabled) show_selected_slot();
    last_result = display_rotated ? "OLED rotated 180 degrees"
                                  : "OLED standard orientation";
    server.send(200, "text/plain", last_result);
  });
  server.on("/api/clear", HTTP_ANY, [] {
    if (!require_authentication()) return;
    if (!storage_ready) {
      server.send(500, "text/plain", "Signal storage is not mounted");
      return;
    }
    const int slot = server.arg("slot").toInt();
    if (slot < 0 || slot >= SLOT_COUNT) {
      server.send(400, "text/plain", "Invalid slot");
      return;
    }
    LittleFS.remove(slot_path(static_cast<uint8_t>(slot)));
    save_slot_name(static_cast<uint8_t>(slot), "");
    last_result = "Cleared slot " + String(slot);
    server.send(200, "text/plain", last_result);
  });
  server.on("/api/import", HTTP_POST, finish_upload, handle_upload_data);

  server.on("/network", HTTP_GET, [] {
    if (!require_authentication()) return;
    server.send(200, "text/html; charset=utf-8", network_page());
  });
  server.on("/network", HTTP_POST, [] {
    if (!require_authentication()) return;
    const String ssid = server.arg("ssid");
    const String password = server.arg("password");
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63) {
      server.send(400, "text/html; charset=utf-8",
                  network_page("Invalid Wi-Fi name or password."));
      return;
    }
    if (!save_wifi_credentials(ssid, password)) {
      server.send(500, "text/html; charset=utf-8",
                  network_page("Could not save Wi-Fi settings."));
      return;
    }
    server.send(200, "text/html; charset=utf-8",
                page_header("Wi-Fi saved") +
                    "<h1>Wi-Fi saved</h1><div class=card>The device is "
                    "restarting.</div></body></html>");
    delay(700);
    ESP.restart();
  });

  server.on("/security", HTTP_GET, [] {
    if (!require_authentication()) return;
    server.send(200, "text/html; charset=utf-8", security_page());
  });
  server.on("/security", HTTP_POST, [] {
    if (!require_authentication()) return;
    const String username = server.arg("username");
    const String password = server.arg("password");
    const String confirmation = server.arg("confirm");
    if (!valid_web_username(username)) {
      server.send(400, "text/html; charset=utf-8",
                  security_page("Use 1-32 letters, digits, dots, dashes or "
                                "underscores for the login."));
      return;
    }
    if (password.length() < 8 || password.length() > 64) {
      server.send(400, "text/html; charset=utf-8",
                  security_page("The password must contain 8-64 characters."));
      return;
    }
    if (password != confirmation) {
      server.send(400, "text/html; charset=utf-8",
                  security_page("The repeated password does not match."));
      return;
    }
    if (!save_web_credentials(username, password)) {
      server.send(500, "text/html; charset=utf-8",
                  security_page("Could not save the new credentials."));
      return;
    }
    rotate_web_session();
    clear_session_cookie();
    server.send(200, "text/html; charset=utf-8",
                page_header("Credentials saved") +
                    "<h1>Credentials saved</h1><div class=card>The new login "
                    "and password are active. Close this browser tab and open "
                    "the device page again to sign in.</div></body></html>");
  });

  const auto redirect_to_setup = [] {
    server.sendHeader("Location", "http://192.168.4.1/network", true);
    server.send(302, "text/plain", "Open Wi-Fi configuration");
  };
  server.on("/generate_204", HTTP_ANY, redirect_to_setup);
  server.on("/hotspot-detect.html", HTTP_ANY, redirect_to_setup);
  server.on("/connecttest.txt", HTTP_ANY, redirect_to_setup);
  server.on("/ncsi.txt", HTTP_ANY, redirect_to_setup);

  server.on("/update", HTTP_GET, [] {
    if (!require_authentication()) return;
    server.send(200, "text/html; charset=utf-8", update_page());
  });
  server.on(
      "/update", HTTP_POST,
      [] {
        if (!require_authentication()) return;
        if (!update_authorized) {
          server.send(401, "text/plain", "Authentication required");
          return;
        }
        const bool success = !update_failed && !Update.hasError();
        server.sendHeader("Connection", "close");
        if (success) {
          server.send(200, "text/html; charset=utf-8",
                      page_header("Update complete") +
                          "<h1>Update complete</h1><div class=card>The device "
                          "is restarting.</div></body></html>");
          delay(700);
          ESP.restart();
        } else {
          server.send(500, "text/plain", "Firmware update failed");
        }
      },
      [] {
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
          update_authorized = request_is_authorized();
          update_failed = !update_authorized;
          if (update_authorized) {
            update_failed = !Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
          }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (update_authorized && !update_failed &&
              Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            update_failed = true;
          }
        } else if (upload.status == UPLOAD_FILE_END) {
          if (update_authorized && !update_failed && !Update.end(true)) {
            update_failed = true;
          }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
          if (update_authorized) Update.abort();
          update_failed = true;
        }
      });

  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

uint8_t next_used_slot(uint8_t current) {
  for (uint8_t offset = 1; offset <= SLOT_COUNT; ++offset) {
    const uint8_t candidate = (current + offset) % SLOT_COUNT;
    if (slot_exists(candidate)) return candidate;
  }
  return (current + 1U) % SLOT_COUNT;
}

void handle_button() {
  const bool pressed = digitalRead(BUTTON_PIN) == LOW;
  const uint32_t now = millis();
  if (pressed != button_down && now - button_last_change >= 30) {
    button_last_change = now;
    button_down = pressed;
    if (pressed) {
      button_down_since = now;
    } else {
      const uint32_t held = now - button_down_since;
      if (held < 700) {
        selected_slot = next_used_slot(selected_slot);
        persist_runtime_settings();
        show_selected_slot();
      } else if (held < 3000) {
        String result;
        const bool ok = transmit_slot(selected_slot, DEFAULT_REPEATS,
                                      DEFAULT_GAP_MS,
                                      FLIPPER_FSK_DATA_INVERTED, result);
        last_result = result;
        if (!ok) display_lines("TX FAILED", result);
      } else {
        show_info();
      }
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.printf("\nLILYGO T3-S3 Flipper 868 Gateway %s\n", FIRMWARE_VERSION);

  pinMode(BOARD_LED, OUTPUT);
  digitalWrite(BOARD_LED, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(RADIO_DIO2, OUTPUT);
  digitalWrite(RADIO_DIO2, LOW);

  if (!display.init()) {
    Serial.println("OLED initialization failed");
  } else {
    // Clear controller RAM left by earlier firmware.
    display.resetDisplay();
    display.normalDisplay();
    display.clear();
    display.display();
  }

  preferences.begin("runtime", true);
  display_enabled = preferences.getBool("display", true);
  // The saved flag is relative to the board-mounting orientation.
  display_rotated = preferences.getBool("rotate_ui", false);
  selected_slot = preferences.getUChar("slot", 0);
  preferences.end();
  if (selected_slot >= SLOT_COUNT) selected_slot = 0;
  load_web_credentials();
  rotate_web_session();
  apply_display_orientation();
  display_power(display_enabled);

  storage_ready = initialize_storage();
  if (!storage_ready) {
    last_result = "LittleFS initialization failed";
    display_lines("STORAGE ERROR", last_result);
  }

  radio_spi.begin(RADIO_SCK, RADIO_MISO, RADIO_MOSI, RADIO_CS);
  flipper::RawSignal probe;
  probe.frequency_hz = 868320000UL;
  String radio_error;
  radio_ready = initialize_radio_for_signal(probe, radio_error);
  radio.standby();
  if (!radio_ready) last_result = radio_error;

  if (!connect_wifi()) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    access_point_active = true;
    dns_server.start(53, "*", WiFi.softAPIP());
    show_network_setup();
  } else {
    access_point_active = false;
    mdns_active = MDNS.begin(HOSTNAME);
    show_selected_slot();
  }

  setup_routes();
  Serial.printf("Web interface: http://%s/\n",
                access_point_active ? WiFi.softAPIP().toString().c_str()
                                    : WiFi.localIP().toString().c_str());
}

void loop() {
  if (access_point_active) dns_server.processNextRequest();
  server.handleClient();
  maintain_wifi();
  handle_button();
  if (display_awake && millis() - display_last_activity > DISPLAY_TIMEOUT_MS) {
    display_power(false);
  }
  delay(2);
}
