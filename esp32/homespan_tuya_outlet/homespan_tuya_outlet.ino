#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_system.h>
#include <time.h>
#include <vector>

#include "HomeSpan.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"

namespace {

constexpr uint16_t TUYA_PORT = 6668;
constexpr uint16_t TUYA_ALT_PORT = 6669;
constexpr uint16_t ADMIN_PORT = 8080;
constexpr uint16_t DNS_PORT = 53;
constexpr char SETUP_AP_SSID[] = "TuyaHomeKit-Setup";
constexpr char SETUP_AP_PASSWORD_PREFIX[] = "THK";
constexpr char DEFAULT_HOSTNAME[] = "tuya-homekit";
constexpr char FIRMWARE_VERSION[] = "2.3.0";
constexpr char PREF_NAMESPACE[] = "tuya-hk";
constexpr uint8_t RESET_CONFIG_PIN = 0;
constexpr uint8_t STATUS_LED_PIN = 2;
constexpr uint8_t STATUS_LED_ON = HIGH;
constexpr uint8_t STATUS_LED_OFF = LOW;
constexpr uint32_t STATUS_LED_PWM_HZ = 5000;
constexpr uint8_t STATUS_LED_PWM_BITS = 8;
constexpr uint8_t STATUS_LED_FULL_DUTY = 255;
constexpr uint8_t STATUS_LED_DIM_DUTY = 3;
constexpr uint8_t WIFI_CONNECT_ATTEMPTS = 3;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t SETUP_WIFI_RETRY_INTERVAL_MS = 60000;
constexpr uint32_t SETUP_MODE_HOLD_MS = 5000;
constexpr uint32_t FACTORY_RESET_HOLD_MS = 15000;
constexpr uint32_t SETUP_LED_BLINK_MS = 120;
constexpr uint8_t WIFI_CONNECTED_BLINKS = 10;
constexpr uint32_t WIFI_CONNECTED_LED_ON_MS = 400;
constexpr uint32_t WIFI_CONNECTED_LED_OFF_MS = 400;
constexpr uint32_t PREFIX_55AA = 0x000055AA;
constexpr uint32_t SUFFIX_55AA = 0x0000AA55;
constexpr uint8_t CMD_SESS_KEY_NEG_START = 0x03;
constexpr uint8_t CMD_SESS_KEY_NEG_RESP = 0x04;
constexpr uint8_t CMD_SESS_KEY_NEG_FINISH = 0x05;
constexpr uint8_t CMD_CONTROL_NEW = 0x0D;
constexpr uint8_t CMD_DP_QUERY_NEW = 0x10;
constexpr size_t AES_BLOCK_SIZE = 16;
constexpr size_t HMAC_SIZE = 32;
constexpr uint32_t DEFAULT_POLL_INTERVAL_SECONDS = 30;
constexpr size_t DIAGNOSTIC_LOG_SIZE = 16;
constexpr uint8_t LAN_SCAN_MAX_RESULTS = 16;
constexpr uint16_t LAN_SCAN_CONNECT_TIMEOUT_MS = 35;
constexpr uint32_t HEALTH_LOW_HEAP_WARNING = 50000;
constexpr uint32_t HEALTH_CRITICAL_HEAP = 25000;
constexpr int HEALTH_WEAK_RSSI = -75;
constexpr uint32_t HEALTH_HIGH_LATENCY_MS = 1500;
constexpr uint32_t HEALTH_WARNING_FAILED_POLLS = 3;
constexpr uint32_t HEALTH_ERROR_FAILED_POLLS = 5;

const uint8_t LOCAL_NONCE[AES_BLOCK_SIZE] = {
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
};

struct BridgeConfig {
  String wifi_ssid;
  String wifi_password;
  String tuya_ip;
  String tuya_device_id;
  String tuya_local_key;
  String tuya_protocol_version = "3.4";
  String tuya_relay_dps = "1";
  String homekit_accessory_name = "Tuya HomeKit Outlet";
  String homekit_service_type = "outlet";
  String homekit_pairing_code;
  String homekit_manufacturer = "Tuya Local Bridge";
  String homekit_model = "Tuya Plug via ESP32";
  String device_hostname = DEFAULT_HOSTNAME;
  uint32_t poll_interval_seconds = DEFAULT_POLL_INTERVAL_SECONDS;
};

struct TuyaResponse {
  uint32_t seq = 0;
  uint32_t cmd = 0;
  uint32_t retcode = 0;
  std::vector<uint8_t> payload;
  bool hmac_ok = false;
};

struct DpsEntry {
  String key;
  String value;
  String label;
  bool is_boolean = false;
};

struct TuyaDiagnostics {
  bool ip_reachable = false;
  bool port_6668_open = false;
  bool port_6669_open = false;
  bool auth_ok = false;
  bool relay_dps_found = false;
  bool relay_state_known = false;
  bool relay_state = false;
  uint32_t latency_ms = 0;
  String protocol_version;
  String payload_json;
  String error;
  String suggestions;
};

struct RuntimeStatus {
  bool relay_state_known = false;
  bool relay_state = false;
  String last_tuya_status = "Not tested yet";
  uint32_t last_latency_ms = 0;
  uint32_t failed_poll_count = 0;
  uint32_t successful_poll_count = 0;
};

struct HealthStatus {
  String state = "Healthy";
  String reason = "All systems responding.";
  String suggested_fix = "No action needed.";
  String updated_at;
};

BridgeConfig config;
Preferences preferences;
WebServer setup_server(80);
WebServer admin_server(ADMIN_PORT);
WebServer* active_server = &setup_server;
DNSServer dns_server;
WiFiClient client;
uint32_t sequence_number = 1;
uint8_t real_key[AES_BLOCK_SIZE] = {0};
uint8_t session_key[AES_BLOCK_SIZE] = {0};
bool session_ready = false;
bool setup_mode = false;
bool homekit_started = false;
bool setup_retry_saved_wifi = false;
bool reset_button_was_pressed = false;
bool factory_reset_started = false;
bool setup_mode_button_started = false;
bool homekit_paired = false;
bool tuya_error = false;
bool setup_led_state = false;
unsigned long last_setup_wifi_retry_ms = 0;
unsigned long reset_button_pressed_ms = 0;
unsigned long last_setup_led_toggle_ms = 0;
unsigned long last_status_led_update_ms = 0;
uint8_t sos_step = 0;
bool sos_led_on = false;
bool captive_dns_started = false;
bool mdns_started = false;
bool mdns_failed = false;

String setup_ap_password;
RuntimeStatus runtime_status;
String diagnostic_log[DIAGNOSTIC_LOG_SIZE];
uint8_t diagnostic_log_start = 0;
uint8_t diagnostic_log_count = 0;

bool connectConfiguredWiFi(const BridgeConfig& candidate, bool keep_ap);
String setupPage(const String& message);
void startSetupMode(const String& reason, bool retry_saved_wifi = false);

void addDiagnosticLog(const String& event) {
  const uint8_t index =
      (diagnostic_log_start + diagnostic_log_count) % DIAGNOSTIC_LOG_SIZE;
  diagnostic_log[index] = String(millis() / 1000) + "s: " + event;
  if (diagnostic_log_count < DIAGNOSTIC_LOG_SIZE) {
    diagnostic_log_count++;
    return;
  }
  diagnostic_log_start = (diagnostic_log_start + 1) % DIAGNOSTIC_LOG_SIZE;
}

void writeStatusLedDuty(uint8_t duty) {
  if (STATUS_LED_ON == HIGH) {
    ledcWrite(STATUS_LED_PIN, duty);
  } else {
    ledcWrite(STATUS_LED_PIN, STATUS_LED_FULL_DUTY - duty);
  }
}

void setStatusLed(bool on) {
  writeStatusLedDuty(on ? STATUS_LED_FULL_DUTY : 0);
}

void setDimStatusLed(bool on) {
  writeStatusLedDuty(on ? STATUS_LED_DIM_DUTY : 0);
}

void updateSetupLed() {
  if (millis() - last_setup_led_toggle_ms < SETUP_LED_BLINK_MS) {
    return;
  }
  last_setup_led_toggle_ms = millis();
  setup_led_state = !setup_led_state;
  setStatusLed(setup_led_state);
}

void resetSosLed() {
  sos_step = 0;
  sos_led_on = false;
  last_status_led_update_ms = 0;
}

void updateSosLed() {
  static const uint16_t durations_ms[] = {
      200, 200, 200, 200, 200, 600,
      600, 200, 600, 200, 600, 600,
      200, 200, 200, 200, 200, 1600,
  };
  const unsigned long now = millis();
  if (last_status_led_update_ms != 0 &&
      now - last_status_led_update_ms < durations_ms[sos_step]) {
    return;
  }
  last_status_led_update_ms = now;
  sos_led_on = !sos_led_on;
  setDimStatusLed(sos_led_on);
  sos_step = (sos_step + 1) % (sizeof(durations_ms) / sizeof(durations_ms[0]));
}

void updateNormalStatusLed() {
  const bool wifi_error = WiFi.status() != WL_CONNECTED;
  if (wifi_error || tuya_error) {
    updateSosLed();
    return;
  }
  resetSosLed();
  setDimStatusLed(homekit_paired);
}

void blinkWiFiConnectedLed() {
  Serial.println("Wi-Fi connected; blinking status LED 10 times.");
  for (uint8_t i = 0; i < WIFI_CONNECTED_BLINKS; i++) {
    setStatusLed(true);
    delay(WIFI_CONNECTED_LED_ON_MS);
    setStatusLed(false);
    delay(WIFI_CONNECTED_LED_OFF_MS);
  }
}

void setTuyaError(bool error) {
  if (tuya_error == error) {
    return;
  }
  tuya_error = error;
  Serial.print("Tuya status LED error state: ");
  Serial.println(tuya_error ? "ON" : "OFF");
  resetSosLed();
}

void refreshHomeKitPairingState() {
  const bool paired =
      homeSpan.controllerListBegin() != homeSpan.controllerListEnd();
  if (homekit_paired == paired) {
    return;
  }
  homekit_paired = paired;
  Serial.print("HomeKit paired state: ");
  Serial.println(homekit_paired ? "paired" : "not paired");
  resetSosLed();
}

void handleHomeKitPairingChange(boolean paired) {
  homekit_paired = paired;
  Serial.print("HomeKit pairing changed: ");
  Serial.println(homekit_paired ? "paired" : "not paired");
  resetSosLed();
}

void appendU32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back((value >> 24) & 0xFF);
  out.push_back((value >> 16) & 0xFF);
  out.push_back((value >> 8) & 0xFF);
  out.push_back(value & 0xFF);
}

uint32_t readU32(const uint8_t* data) {
  return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
         (uint32_t(data[2]) << 8) | uint32_t(data[3]);
}

void appendBytes(std::vector<uint8_t>& out, const uint8_t* data, size_t length) {
  out.insert(out.end(), data, data + length);
}

std::vector<uint8_t> pkcs7Pad(const uint8_t* data, size_t length) {
  std::vector<uint8_t> out(data, data + length);
  uint8_t pad = AES_BLOCK_SIZE - (length % AES_BLOCK_SIZE);
  if (pad == 0) {
    pad = AES_BLOCK_SIZE;
  }
  out.insert(out.end(), pad, pad);
  return out;
}

bool pkcs7Unpad(std::vector<uint8_t>& data) {
  if (data.empty()) {
    return false;
  }
  uint8_t pad = data.back();
  if (pad == 0 || pad > AES_BLOCK_SIZE || pad > data.size()) {
    return false;
  }
  for (size_t i = data.size() - pad; i < data.size(); i++) {
    if (data[i] != pad) {
      return false;
    }
  }
  data.resize(data.size() - pad);
  return true;
}

bool aesEcbEncrypt(const uint8_t key[AES_BLOCK_SIZE], const uint8_t* data,
                   size_t length, bool pad, std::vector<uint8_t>& out) {
  std::vector<uint8_t> input;
  if (pad) {
    input = pkcs7Pad(data, length);
  } else {
    if (length % AES_BLOCK_SIZE != 0) {
      Serial.println("AES encrypt input is not block aligned.");
      return false;
    }
    input.assign(data, data + length);
  }

  out.assign(input.size(), 0);
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  int rc = mbedtls_aes_setkey_enc(&ctx, key, 128);
  if (rc == 0) {
    for (size_t offset = 0; offset < input.size(); offset += AES_BLOCK_SIZE) {
      rc = mbedtls_aes_crypt_ecb(
          &ctx, MBEDTLS_AES_ENCRYPT, input.data() + offset, out.data() + offset);
      if (rc != 0) {
        break;
      }
    }
  }
  mbedtls_aes_free(&ctx);
  return rc == 0;
}

bool aesEcbDecrypt(const uint8_t key[AES_BLOCK_SIZE], const uint8_t* data,
                   size_t length, bool unpad, std::vector<uint8_t>& out) {
  if (length % AES_BLOCK_SIZE != 0) {
    Serial.println("AES decrypt input is not block aligned.");
    return false;
  }

  out.assign(length, 0);
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  int rc = mbedtls_aes_setkey_dec(&ctx, key, 128);
  if (rc == 0) {
    for (size_t offset = 0; offset < length; offset += AES_BLOCK_SIZE) {
      rc = mbedtls_aes_crypt_ecb(
          &ctx, MBEDTLS_AES_DECRYPT, data + offset, out.data() + offset);
      if (rc != 0) {
        break;
      }
    }
  }
  mbedtls_aes_free(&ctx);
  if (rc != 0) {
    return false;
  }
  return !unpad || pkcs7Unpad(out);
}

bool hmacSha256(const uint8_t* key, size_t key_len, const uint8_t* data,
                size_t data_len, uint8_t out[HMAC_SIZE]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) {
    return false;
  }
  return mbedtls_md_hmac(info, key, key_len, data, data_len, out) == 0;
}

String htmlEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length());
  for (size_t i = 0; i < value.length(); i++) {
    const char ch = value.charAt(i);
    switch (ch) {
      case '&':
        escaped += F("&amp;");
        break;
      case '<':
        escaped += F("&lt;");
        break;
      case '>':
        escaped += F("&gt;");
        break;
      case '"':
        escaped += F("&quot;");
        break;
      case '\'':
        escaped += F("&#39;");
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return escaped;
}

String yesNo(bool value) {
  return value ? "yes" : "no";
}

String onOff(bool value) {
  return value ? "on" : "off";
}

String homeKitServiceLabel() {
  if (config.homekit_service_type == "light") {
    return "Light";
  }
  if (config.homekit_service_type == "switch") {
    return "Switch";
  }
  return "Outlet";
}

String localDashboardUrl() {
  return String("http://") + config.device_hostname + ".local:" +
         String(ADMIN_PORT) + "/";
}

String ipDashboardUrl() {
  if (WiFi.status() != WL_CONNECTED) {
    return "";
  }
  return String("http://") + WiFi.localIP().toString() + ":" +
         String(ADMIN_PORT) + "/";
}

String formatDuration(unsigned long seconds) {
  const unsigned long days = seconds / 86400;
  seconds %= 86400;
  const unsigned long hours = seconds / 3600;
  seconds %= 3600;
  const unsigned long minutes = seconds / 60;
  seconds %= 60;
  String out;
  if (days > 0) {
    out += String(days) + "d ";
  }
  if (hours > 0 || !out.isEmpty()) {
    out += String(hours) + "h ";
  }
  if (minutes > 0 || !out.isEmpty()) {
    out += String(minutes) + "m ";
  }
  out += String(seconds) + "s";
  return out;
}

String currentTimestampText() {
  time_t now = time(nullptr);
  if (now > 1700000000) {
    char buffer[32] = {0};
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    return String(buffer);
  }
  return String("uptime ") + formatDuration(millis() / 1000);
}

void addMetricRow(String& page, const String& label, const String& value) {
  page += F("<tr><th>");
  page += htmlEscape(label);
  page += F("</th><td>");
  page += htmlEscape(value);
  page += F("</td></tr>");
}

String jsonEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    const char ch = value.charAt(i);
    switch (ch) {
      case '"':
        escaped += F("\\\"");
        break;
      case '\\':
        escaped += F("\\\\");
        break;
      case '\n':
        escaped += F("\\n");
        break;
      case '\r':
        escaped += F("\\r");
        break;
      case '\t':
        escaped += F("\\t");
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return escaped;
}

void addJsonString(String& json, const String& key, const String& value,
                   bool& first) {
  if (!first) {
    json += F(",");
  }
  first = false;
  json += F("\"");
  json += jsonEscape(key);
  json += F("\":\"");
  json += jsonEscape(value);
  json += F("\"");
}

void addJsonNumber(String& json, const String& key, uint32_t value,
                   bool& first) {
  if (!first) {
    json += F(",");
  }
  first = false;
  json += F("\"");
  json += jsonEscape(key);
  json += F("\":");
  json += String(value);
}

void addJsonBool(String& json, const String& key, bool value, bool& first) {
  if (!first) {
    json += F(",");
  }
  first = false;
  json += F("\"");
  json += jsonEscape(key);
  json += F("\":");
  json += value ? F("true") : F("false");
}

bool parseTuyaIp(IPAddress& ip) {
  return ip.fromString(config.tuya_ip);
}

uint32_t normalizedPollInterval(uint32_t seconds) {
  if (seconds < 5) {
    return 5;
  }
  if (seconds > 3600) {
    return 3600;
  }
  return seconds;
}

bool isRelayDpsValid(const String& dps) {
  if (dps.isEmpty() || dps.length() > 3) {
    return false;
  }
  for (size_t i = 0; i < dps.length(); i++) {
    if (!isDigit(dps.charAt(i))) {
      return false;
    }
  }
  return true;
}

bool isHomeKitServiceTypeValid(const String& service_type) {
  return service_type == "outlet" || service_type == "light" ||
         service_type == "switch";
}

bool isHomeKitPairingCodeValid(const String& code) {
  if (code.isEmpty()) {
    return true;
  }
  if (code.length() != 8) {
    return false;
  }
  for (size_t i = 0; i < code.length(); i++) {
    if (!isDigit(code.charAt(i))) {
      return false;
    }
  }
  return code != "00000000" && code != "11111111" && code != "22222222" &&
         code != "33333333" && code != "44444444" && code != "55555555" &&
         code != "66666666" && code != "77777777" && code != "88888888" &&
         code != "99999999" && code != "12345678" && code != "87654321";
}

String formattedHomeKitCode(const String& code) {
  if (code.length() != 8) {
    return "";
  }
  return code.substring(0, 3) + "-" + code.substring(3, 5) + "-" +
         code.substring(5);
}

Category homeKitCategory() {
  if (config.homekit_service_type == "light") {
    return Category::Lighting;
  }
  if (config.homekit_service_type == "switch") {
    return Category::Switches;
  }
  return Category::Outlets;
}

String optionSelected(const String& value, const String& expected) {
  return value == expected ? " selected" : "";
}

bool isPrintableAscii(const String& value) {
  for (size_t i = 0; i < value.length(); i++) {
    const char ch = value.charAt(i);
    if (ch < 32 || ch > 126) {
      return false;
    }
  }
  return true;
}

bool isAlphaNumericString(const String& value) {
  if (value.isEmpty()) {
    return false;
  }
  for (size_t i = 0; i < value.length(); i++) {
    const char ch = value.charAt(i);
    if (!isAlphaNumeric(ch)) {
      return false;
    }
  }
  return true;
}

String normalizedHostname(String value) {
  value.trim();
  value.toLowerCase();
  String out;
  out.reserve(value.length());
  bool last_dash = false;
  for (size_t i = 0; i < value.length(); i++) {
    const char ch = value.charAt(i);
    const bool allowed = isAlphaNumeric(ch) || ch == '-';
    if (!allowed) {
      continue;
    }
    if (ch == '-') {
      if (out.isEmpty() || last_dash) {
        continue;
      }
      last_dash = true;
    } else {
      last_dash = false;
    }
    out += ch;
  }
  while (out.endsWith("-")) {
    out.remove(out.length() - 1);
  }
  if (out.isEmpty()) {
    out = DEFAULT_HOSTNAME;
  }
  return out;
}

bool isHostnameValid(const String& hostname) {
  if (hostname.isEmpty() || hostname.length() > 32 ||
      hostname.startsWith("-") || hostname.endsWith("-")) {
    return false;
  }
  for (size_t i = 0; i < hostname.length(); i++) {
    const char ch = hostname.charAt(i);
    if (!isAlphaNumeric(ch) && ch != '-') {
      return false;
    }
  }
  return true;
}

int findJsonKey(const String& json, const String& key) {
  return json.indexOf(String("\"") + key + "\"");
}

bool extractJsonString(const String& json, const String& key, String& out) {
  const int key_pos = findJsonKey(json, key);
  if (key_pos < 0) {
    return false;
  }
  int colon = json.indexOf(':', key_pos);
  if (colon < 0) {
    return false;
  }
  int start = json.indexOf('"', colon + 1);
  if (start < 0) {
    return false;
  }
  String value;
  bool escaped = false;
  for (int i = start + 1; i < json.length(); i++) {
    const char ch = json.charAt(i);
    if (escaped) {
      switch (ch) {
        case 'n':
          value += '\n';
          break;
        case 'r':
          value += '\r';
          break;
        case 't':
          value += '\t';
          break;
        default:
          value += ch;
          break;
      }
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      out = value;
      return true;
    }
    value += ch;
  }
  return false;
}

bool extractJsonUInt(const String& json, const String& key, uint32_t& out) {
  const int key_pos = findJsonKey(json, key);
  if (key_pos < 0) {
    return false;
  }
  int colon = json.indexOf(':', key_pos);
  if (colon < 0) {
    return false;
  }
  int start = colon + 1;
  while (start < json.length() && isSpace(json.charAt(start))) {
    start++;
  }
  int end = start;
  while (end < json.length() && isDigit(json.charAt(end))) {
    end++;
  }
  if (end == start) {
    return false;
  }
  out = uint32_t(json.substring(start, end).toInt());
  return true;
}

bool jsonHasKey(const String& json, const String& key) {
  return findJsonKey(json, key) >= 0;
}

String setupPasswordFromRandom() {
  char password[16] = {0};
  snprintf(password, sizeof(password), "%s%08lX%02lX",
           SETUP_AP_PASSWORD_PREFIX, uint32_t(esp_random()),
           uint32_t(esp_random() & 0xFF));
  return String(password);
}

bool validateConfig(const BridgeConfig& candidate, String& error) {
  IPAddress ip;
  if (!isHostnameValid(candidate.device_hostname)) {
    error = "Hostname must be 1-32 characters using only letters, numbers, and hyphens. It cannot start or end with a hyphen.";
    return false;
  }
  if (candidate.wifi_ssid.isEmpty()) {
    error = "Wi-Fi SSID is required.";
    return false;
  }
  if (candidate.wifi_ssid.length() > 32 || !isPrintableAscii(candidate.wifi_ssid)) {
    error = "Wi-Fi SSID must be printable ASCII and at most 32 characters.";
    return false;
  }
  if (!candidate.wifi_password.isEmpty() &&
      (candidate.wifi_password.length() < 8 ||
       candidate.wifi_password.length() > 63 ||
       !isPrintableAscii(candidate.wifi_password))) {
    error = "Wi-Fi password must be empty for open Wi-Fi or 8-63 printable ASCII characters.";
    return false;
  }
  if (!ip.fromString(candidate.tuya_ip)) {
    error = "Tuya plug IP address is invalid.";
    return false;
  }
  if (candidate.tuya_device_id.isEmpty()) {
    error = "Tuya device ID is required.";
    return false;
  }
  if (candidate.tuya_device_id.length() > 32 ||
      !isAlphaNumericString(candidate.tuya_device_id)) {
    error = "Tuya device ID must be alphanumeric and at most 32 characters.";
    return false;
  }
  if (candidate.tuya_local_key.length() != AES_BLOCK_SIZE) {
    error = "Tuya local key must be exactly 16 characters.";
    return false;
  }
  if (!isPrintableAscii(candidate.tuya_local_key)) {
    error = "Tuya local key must use printable ASCII characters.";
    return false;
  }
  if (candidate.tuya_protocol_version != "3.4") {
    error = "This first wizard version only supports Tuya protocol 3.4.";
    return false;
  }
  if (!isRelayDpsValid(candidate.tuya_relay_dps)) {
    error = "Relay DPS must be a numeric value up to 3 digits.";
    return false;
  }
  if (candidate.homekit_accessory_name.isEmpty()) {
    error = "HomeKit accessory name is required.";
    return false;
  }
  if (candidate.homekit_accessory_name.length() > 40 ||
      !isPrintableAscii(candidate.homekit_accessory_name)) {
    error = "HomeKit accessory name must be printable ASCII and at most 40 characters.";
    return false;
  }
  if (!isHomeKitServiceTypeValid(candidate.homekit_service_type)) {
    error = "HomeKit type must be outlet, light, or switch.";
    return false;
  }
  if (!isHomeKitPairingCodeValid(candidate.homekit_pairing_code)) {
    error = "HomeKit pairing code must be empty or a valid 8-digit code. Avoid simple codes like 11111111 or 12345678.";
    return false;
  }
  return true;
}

bool loadConfig() {
  if (!preferences.begin(PREF_NAMESPACE, true)) {
    Serial.println("Could not open Preferences for reading.");
    return false;
  }
  const bool configured = preferences.getBool("configured", false);
  if (!configured) {
    preferences.end();
    return false;
  }

  config.wifi_ssid = preferences.getString("wifi_ssid", "");
  config.wifi_password = preferences.getString("wifi_pass", "");
  config.tuya_ip = preferences.getString("tuya_ip", "");
  config.tuya_device_id = preferences.getString("tuya_id", "");
  config.tuya_local_key = preferences.getString("tuya_key", "");
  config.tuya_protocol_version = preferences.getString("tuya_ver", "3.4");
  config.tuya_relay_dps = preferences.getString("relay_dps", "1");
  config.homekit_accessory_name =
      preferences.getString("hk_name", "Tuya HomeKit Outlet");
  config.homekit_service_type = preferences.getString("hk_service", "outlet");
  config.homekit_pairing_code = preferences.getString("hk_pair", "");
  config.homekit_manufacturer =
      preferences.getString("hk_mfr", "Tuya Local Bridge");
  config.homekit_model =
      preferences.getString("hk_model", "Tuya Plug via ESP32");
  config.device_hostname =
      normalizedHostname(preferences.getString("hostname", DEFAULT_HOSTNAME));
  config.poll_interval_seconds =
      normalizedPollInterval(preferences.getUInt("poll_sec",
                                                 DEFAULT_POLL_INTERVAL_SECONDS));
  preferences.end();

  String error;
  if (!validateConfig(config, error)) {
    Serial.print("Saved configuration is invalid: ");
    Serial.println(error);
    return false;
  }
  return true;
}

bool saveConfig(const BridgeConfig& candidate, String& error) {
  BridgeConfig normalized = candidate;
  normalized.device_hostname = normalizedHostname(normalized.device_hostname);
  normalized.poll_interval_seconds =
      normalizedPollInterval(normalized.poll_interval_seconds);
  if (!validateConfig(normalized, error)) {
    return false;
  }

  if (!preferences.begin(PREF_NAMESPACE, false)) {
    error = "Could not open ESP32 Preferences for writing.";
    return false;
  }
  preferences.putString("wifi_ssid", normalized.wifi_ssid);
  preferences.putString("wifi_pass", normalized.wifi_password);
  preferences.putString("tuya_ip", normalized.tuya_ip);
  preferences.putString("tuya_id", normalized.tuya_device_id);
  preferences.putString("tuya_key", normalized.tuya_local_key);
  preferences.putString("tuya_ver", normalized.tuya_protocol_version);
  preferences.putString("relay_dps", normalized.tuya_relay_dps);
  preferences.putString("hk_name", normalized.homekit_accessory_name);
  preferences.putString("hk_service", normalized.homekit_service_type);
  if (!normalized.homekit_pairing_code.isEmpty()) {
  preferences.putString("hk_pair", normalized.homekit_pairing_code);
  }
  preferences.putString("hk_mfr", normalized.homekit_manufacturer);
  preferences.putString("hk_model", normalized.homekit_model);
  preferences.putString("hostname", normalized.device_hostname);
  preferences.putUInt("poll_sec", normalized.poll_interval_seconds);
  preferences.putBool("configured", true);
  preferences.end();
  config = normalized;
  return true;
}

void clearConfig() {
  if (!preferences.begin(PREF_NAMESPACE, false)) {
    Serial.println("Could not open Preferences for clearing.");
    return;
  }
  preferences.clear();
  preferences.end();
  Serial.println("Saved configuration cleared.");
}

BridgeConfig configFromRequest() {
  BridgeConfig candidate;
  candidate.wifi_ssid = active_server->arg("wifi_ssid");
  candidate.wifi_password = active_server->arg("wifi_password");
  candidate.tuya_ip = active_server->arg("tuya_ip");
  candidate.tuya_device_id = active_server->arg("tuya_device_id");
  candidate.tuya_local_key = active_server->arg("tuya_local_key");
  candidate.tuya_protocol_version = active_server->arg("tuya_protocol_version");
  candidate.tuya_relay_dps = active_server->arg("tuya_relay_dps");
  candidate.homekit_accessory_name = active_server->arg("homekit_accessory_name");
  candidate.homekit_service_type = active_server->arg("homekit_service_type");
  candidate.homekit_pairing_code = active_server->arg("homekit_pairing_code");
  candidate.homekit_manufacturer = "Tuya Local Bridge";
  candidate.homekit_model = "Tuya Plug via ESP32";
  candidate.device_hostname =
      normalizedHostname(active_server->arg("device_hostname"));
  candidate.poll_interval_seconds =
      uint32_t(active_server->arg("poll_interval_seconds").toInt());
  if (candidate.wifi_password.isEmpty() && !config.wifi_password.isEmpty()) {
    candidate.wifi_password = config.wifi_password;
  }
  if (candidate.tuya_local_key.isEmpty() && !config.tuya_local_key.isEmpty()) {
    candidate.tuya_local_key = config.tuya_local_key;
  }
  if (candidate.tuya_protocol_version.isEmpty()) {
    candidate.tuya_protocol_version = "3.4";
  }
  if (candidate.tuya_relay_dps.isEmpty()) {
    candidate.tuya_relay_dps = "1";
  }
  if (candidate.homekit_accessory_name.isEmpty()) {
    candidate.homekit_accessory_name = "Tuya HomeKit Outlet";
  }
  if (candidate.homekit_service_type.isEmpty()) {
    candidate.homekit_service_type = "outlet";
  }
  if (candidate.device_hostname.isEmpty()) {
    candidate.device_hostname = DEFAULT_HOSTNAME;
  }
  if (candidate.poll_interval_seconds == 0) {
    candidate.poll_interval_seconds = DEFAULT_POLL_INTERVAL_SECONDS;
  }
  return candidate;
}

void clearPendingHomeKitPairingCode() {
  if (!preferences.begin(PREF_NAMESPACE, false)) {
    Serial.println("Could not open Preferences for clearing HomeKit code.");
    return;
  }
  preferences.remove("hk_pair");
  preferences.end();
  config.homekit_pairing_code = "";
}

WebServer& requestServer() {
  return *active_server;
}

bool parseLocalKey() {
  const char* key = config.tuya_local_key.c_str();
  size_t length = strlen(key);
  if (length != AES_BLOCK_SIZE) {
    Serial.printf("LOCAL_KEY must be 16 bytes, got %u.\n", unsigned(length));
    return false;
  }
  memcpy(real_key, key, AES_BLOCK_SIZE);
  return true;
}

bool connectTuyaPort(uint16_t port, uint32_t timeout_ms) {
  if (client.connected()) {
    return true;
  }
  IPAddress tuya_ip;
  if (!parseTuyaIp(tuya_ip)) {
    Serial.print("Invalid Tuya plug IP address: ");
    Serial.println(config.tuya_ip);
    return false;
  }
  client.stop();
  Serial.print("Connecting to Tuya plug ");
  Serial.print(tuya_ip);
  Serial.print(":");
  Serial.println(port);
  if (!client.connect(tuya_ip, port, timeout_ms)) {
    Serial.println("TCP connection failed.");
    return false;
  }
  client.setTimeout(5000);
  return true;
}

bool connectTuya() {
  return connectTuyaPort(TUYA_PORT, 5000);
}

bool testTcpPort(const IPAddress& ip, uint16_t port, uint32_t timeout_ms,
                 uint32_t* latency_ms) {
  WiFiClient probe;
  const unsigned long start = millis();
  const bool connected = probe.connect(ip, port, timeout_ms);
  const uint32_t elapsed = uint32_t(millis() - start);
  probe.stop();
  if (latency_ms) {
    *latency_ms = elapsed;
  }
  return connected;
}

bool readExact(uint8_t* data, size_t length) {
  size_t got = 0;
  unsigned long start = millis();
  while (got < length && millis() - start < 5000) {
    int available = client.available();
    if (available <= 0) {
      delay(5);
      continue;
    }
    int read_len = client.read(data + got, length - got);
    if (read_len <= 0) {
      delay(5);
      continue;
    }
    got += size_t(read_len);
  }
  return got == length;
}

std::vector<uint8_t> packMessage(uint32_t cmd, const std::vector<uint8_t>& payload,
                                 const uint8_t hmac_key[AES_BLOCK_SIZE]) {
  std::vector<uint8_t> out;
  uint32_t length = payload.size() + HMAC_SIZE + 4;
  appendU32(out, PREFIX_55AA);
  appendU32(out, sequence_number++);
  appendU32(out, cmd);
  appendU32(out, length);
  appendBytes(out, payload.data(), payload.size());

  uint8_t digest[HMAC_SIZE] = {0};
  hmacSha256(hmac_key, AES_BLOCK_SIZE, out.data(), out.size(), digest);
  appendBytes(out, digest, HMAC_SIZE);
  appendU32(out, SUFFIX_55AA);
  return out;
}

bool readResponse(const uint8_t hmac_key[AES_BLOCK_SIZE], TuyaResponse& response) {
  uint8_t header[16] = {0};
  if (!readExact(header, sizeof(header))) {
    Serial.println("Timed out reading Tuya header.");
    return false;
  }

  uint32_t prefix = readU32(header);
  if (prefix != PREFIX_55AA) {
    Serial.printf("Unexpected Tuya prefix: 0x%08lx\n", prefix);
    return false;
  }

  response.seq = readU32(header + 4);
  response.cmd = readU32(header + 8);
  uint32_t length = readU32(header + 12);
  if (length < 4 + HMAC_SIZE + 4 || length > 1024) {
    Serial.printf("Unexpected Tuya packet length: %lu\n", length);
    return false;
  }

  std::vector<uint8_t> rest(length);
  if (!readExact(rest.data(), rest.size())) {
    Serial.println("Timed out reading Tuya payload.");
    return false;
  }

  uint32_t suffix = readU32(rest.data() + rest.size() - 4);
  if (suffix != SUFFIX_55AA) {
    Serial.printf("Unexpected Tuya suffix: 0x%08lx\n", suffix);
    return false;
  }

  std::vector<uint8_t> signed_data(header, header + sizeof(header));
  signed_data.insert(signed_data.end(), rest.begin(), rest.end() - HMAC_SIZE - 4);
  uint8_t expected[HMAC_SIZE] = {0};
  hmacSha256(hmac_key, AES_BLOCK_SIZE, signed_data.data(), signed_data.size(), expected);
  const uint8_t* actual = rest.data() + rest.size() - HMAC_SIZE - 4;
  response.hmac_ok = memcmp(expected, actual, HMAC_SIZE) == 0;
  if (!response.hmac_ok) {
    Serial.println("Tuya response HMAC check failed.");
    return false;
  }

  response.retcode = readU32(rest.data());
  size_t payload_len = length - 4 - HMAC_SIZE - 4;
  response.payload.assign(rest.begin() + 4, rest.begin() + 4 + payload_len);
  return true;
}

bool sendEncrypted(uint32_t cmd, const uint8_t* clear, size_t clear_len,
                   const uint8_t key[AES_BLOCK_SIZE], bool prepend_version,
                   TuyaResponse* response) {
  std::vector<uint8_t> message_clear;
  if (prepend_version) {
    uint8_t version_header[15] = {0};
    size_t version_len = config.tuya_protocol_version.length();
    if (version_len > 3) {
      version_len = 3;
    }
    memcpy(version_header, config.tuya_protocol_version.c_str(), version_len);
    appendBytes(message_clear, version_header, sizeof(version_header));
  }
  appendBytes(message_clear, clear, clear_len);

  std::vector<uint8_t> encrypted;
  if (!aesEcbEncrypt(key, message_clear.data(), message_clear.size(), true, encrypted)) {
    Serial.println("AES encrypt failed.");
    return false;
  }

  std::vector<uint8_t> packet = packMessage(cmd, encrypted, key);
  if (client.write(packet.data(), packet.size()) != packet.size()) {
    Serial.println("Failed to write Tuya packet.");
    return false;
  }

  if (!response) {
    return true;
  }
  return readResponse(key, *response);
}

bool decryptPayload(const uint8_t key[AES_BLOCK_SIZE], const TuyaResponse& response,
                    std::vector<uint8_t>& clear) {
  return aesEcbDecrypt(key, response.payload.data(), response.payload.size(), true, clear);
}

bool negotiateSession() {
  if (!connectTuya()) {
    return false;
  }

  std::vector<uint8_t> encrypted_nonce;
  if (!aesEcbEncrypt(real_key, LOCAL_NONCE, AES_BLOCK_SIZE, true, encrypted_nonce)) {
    Serial.println("Failed to encrypt local nonce.");
    return false;
  }

  std::vector<uint8_t> step1 = packMessage(CMD_SESS_KEY_NEG_START, encrypted_nonce, real_key);
  if (client.write(step1.data(), step1.size()) != step1.size()) {
    Serial.println("Failed to send session negotiation step 1.");
    return false;
  }

  TuyaResponse step2;
  if (!readResponse(real_key, step2) || step2.cmd != CMD_SESS_KEY_NEG_RESP) {
    Serial.println("Session negotiation step 2 failed.");
    return false;
  }

  std::vector<uint8_t> clear_step2;
  if (!decryptPayload(real_key, step2, clear_step2) || clear_step2.size() < 48) {
    Serial.println("Could not decrypt session negotiation response.");
    return false;
  }

  const uint8_t* remote_nonce = clear_step2.data();
  const uint8_t* remote_hmac = clear_step2.data() + AES_BLOCK_SIZE;
  uint8_t expected_hmac[HMAC_SIZE] = {0};
  hmacSha256(real_key, AES_BLOCK_SIZE, LOCAL_NONCE, AES_BLOCK_SIZE, expected_hmac);
  if (memcmp(expected_hmac, remote_hmac, HMAC_SIZE) != 0) {
    Serial.println("Device nonce HMAC verification failed.");
    return false;
  }

  uint8_t finish_hmac[HMAC_SIZE] = {0};
  hmacSha256(real_key, AES_BLOCK_SIZE, remote_nonce, AES_BLOCK_SIZE, finish_hmac);
  std::vector<uint8_t> encrypted_finish;
  if (!aesEcbEncrypt(real_key, finish_hmac, HMAC_SIZE, true, encrypted_finish)) {
    Serial.println("Failed to encrypt session negotiation finish.");
    return false;
  }
  std::vector<uint8_t> step3 = packMessage(CMD_SESS_KEY_NEG_FINISH, encrypted_finish, real_key);
  if (client.write(step3.data(), step3.size()) != step3.size()) {
    Serial.println("Failed to send session negotiation step 3.");
    return false;
  }

  uint8_t xor_key[AES_BLOCK_SIZE] = {0};
  for (size_t i = 0; i < AES_BLOCK_SIZE; i++) {
    xor_key[i] = LOCAL_NONCE[i] ^ remote_nonce[i];
  }
  std::vector<uint8_t> derived;
  if (!aesEcbEncrypt(real_key, xor_key, AES_BLOCK_SIZE, false, derived)) {
    Serial.println("Failed to derive session key.");
    return false;
  }
  memcpy(session_key, derived.data(), AES_BLOCK_SIZE);
  session_ready = true;
  Serial.println("Tuya session key negotiated.");
  return true;
}

bool ensureSession() {
  if (session_ready && client.connected()) {
    return true;
  }
  session_ready = false;
  client.stop();
  sequence_number = 1;
  return negotiateSession();
}

void resetTuyaSession() {
  client.stop();
  session_ready = false;
}

String payloadJsonText(const std::vector<uint8_t>& payload) {
  String text;
  text.reserve(payload.size() + 1);
  for (uint8_t byte : payload) {
    text += char(byte);
  }

  int json_start = text.indexOf('{');
  if (json_start > 0) {
    text = text.substring(json_start);
  }
  return text;
}

void printClearPayload(const std::vector<uint8_t>& payload) {
  String text = payloadJsonText(payload);
  Serial.println("Tuya clear payload:");
  Serial.println(text);
}

bool parseRelayStateFromText(String text, const String& dps, bool& is_on) {
  String true_pattern = String("\"") + dps + "\":true";
  String false_pattern = String("\"") + dps + "\":false";
  text.replace(" ", "");
  if (text.indexOf(true_pattern) >= 0) {
    is_on = true;
    return true;
  }
  if (text.indexOf(false_pattern) >= 0) {
    is_on = false;
    return true;
  }
  return false;
}

String dpsHeuristicLabel(const String& value, bool is_boolean) {
  if (is_boolean) {
    return "boolean value; possible relay/switch candidate";
  }
  bool numeric = !value.isEmpty();
  for (size_t i = 0; i < value.length(); i++) {
    const char ch = value.charAt(i);
    if (!isDigit(ch) && ch != '-' && ch != '.') {
      numeric = false;
      break;
    }
  }
  if (numeric) {
    return "numeric value; possible sensor/energy/power value";
  }
  return "unknown value type";
}

std::vector<DpsEntry> parseDpsEntries(const String& payload) {
  std::vector<DpsEntry> entries;
  int dps_pos = payload.indexOf("\"dps\"");
  if (dps_pos < 0) {
    dps_pos = payload.indexOf("'dps'");
  }
  if (dps_pos < 0) {
    return entries;
  }
  int object_start = payload.indexOf('{', dps_pos);
  if (object_start < 0) {
    return entries;
  }
  int i = object_start + 1;
  while (i < payload.length()) {
    while (i < payload.length() &&
           (payload.charAt(i) == ' ' || payload.charAt(i) == ',' ||
            payload.charAt(i) == '\n' || payload.charAt(i) == '\r')) {
      i++;
    }
    if (i >= payload.length() || payload.charAt(i) == '}') {
      break;
    }
    if (payload.charAt(i) != '"' && payload.charAt(i) != '\'') {
      i++;
      continue;
    }
    const char quote = payload.charAt(i++);
    const int key_start = i;
    while (i < payload.length() && payload.charAt(i) != quote) {
      i++;
    }
    if (i >= payload.length()) {
      break;
    }
    DpsEntry entry;
    entry.key = payload.substring(key_start, i++);
    while (i < payload.length() &&
           (payload.charAt(i) == ' ' || payload.charAt(i) == ':')) {
      i++;
    }
    const int value_start = i;
    int depth = 0;
    bool in_string = false;
    char string_quote = 0;
    while (i < payload.length()) {
      const char ch = payload.charAt(i);
      if (in_string) {
        if (ch == string_quote && payload.charAt(i - 1) != '\\') {
          in_string = false;
        }
      } else if (ch == '"' || ch == '\'') {
        in_string = true;
        string_quote = ch;
      } else if (ch == '{' || ch == '[') {
        depth++;
      } else if (ch == '}' || ch == ']') {
        if (depth == 0) {
          break;
        }
        depth--;
      } else if (ch == ',' && depth == 0) {
        break;
      }
      i++;
    }
    entry.value = payload.substring(value_start, i);
    entry.value.trim();
    entry.is_boolean = entry.value == "true" || entry.value == "false";
    entry.label = dpsHeuristicLabel(entry.value, entry.is_boolean);
    entries.push_back(entry);
    if (i < payload.length() && payload.charAt(i) == '}') {
      break;
    }
  }
  return entries;
}

bool tuyaQueryStatusPayload(String& payload_json, uint32_t* latency_ms,
                            String* error) {
  const unsigned long start = millis();
  if (!ensureSession()) {
    setTuyaError(true);
    if (error) {
      *error = "Could not negotiate Tuya session. Check IP, local key, and protocol version.";
    }
    return false;
  }
  const char payload[] = "{}";
  TuyaResponse response;
  if (!sendEncrypted(CMD_DP_QUERY_NEW, reinterpret_cast<const uint8_t*>(payload),
                     strlen(payload), session_key, false, &response)) {
    Serial.println("Status command failed.");
    resetTuyaSession();
    setTuyaError(true);
    if (error) {
      *error = "Status command failed or timed out.";
    }
    return false;
  }
  std::vector<uint8_t> clear;
  if (!decryptPayload(session_key, response, clear)) {
    Serial.println("Could not decrypt status response.");
    resetTuyaSession();
    setTuyaError(true);
    if (error) {
      *error = "Status response could not be decrypted. The local key or protocol version may be wrong.";
    }
    return false;
  }
  payload_json = payloadJsonText(clear);
  if (latency_ms) {
    *latency_ms = uint32_t(millis() - start);
  }
  resetTuyaSession();
  setTuyaError(false);
  return true;
}

bool tuyaStatus() {
  String payload_json;
  uint32_t latency_ms = 0;
  String error;
  if (!tuyaQueryStatusPayload(payload_json, &latency_ms, &error)) {
    runtime_status.last_tuya_status = error;
    runtime_status.failed_poll_count++;
    addDiagnosticLog("Tuya poll failed: " + error);
    return false;
  }
  Serial.println("Tuya clear payload:");
  Serial.println(payload_json);
  runtime_status.last_tuya_status = "OK";
  runtime_status.last_latency_ms = latency_ms;
  runtime_status.successful_poll_count++;
  addDiagnosticLog("Tuya poll OK");
  return true;
}

bool tuyaReadPower(bool& is_on) {
  String payload_json;
  uint32_t latency_ms = 0;
  String error;
  if (!tuyaQueryStatusPayload(payload_json, &latency_ms, &error)) {
    runtime_status.last_tuya_status = error;
    runtime_status.failed_poll_count++;
    addDiagnosticLog("Tuya poll failed: " + error);
    return false;
  }

  if (parseRelayStateFromText(payload_json, config.tuya_relay_dps, is_on)) {
    runtime_status.last_tuya_status = "OK";
    runtime_status.last_latency_ms = latency_ms;
    runtime_status.relay_state_known = true;
    runtime_status.relay_state = is_on;
    runtime_status.successful_poll_count++;
    addDiagnosticLog(String("Tuya poll OK; relay ") + onOff(is_on));
    return true;
  }

  Serial.println("Relay DPS was not found in status response.");
  Serial.println(payload_json);
  runtime_status.last_tuya_status = "Relay DPS not found in status response";
  runtime_status.failed_poll_count++;
  runtime_status.relay_state_known = false;
  addDiagnosticLog("Tuya poll failed: relay DPS not found");
  setTuyaError(true);
  return false;
}

bool tuyaSwitch(bool on) {
  if (!ensureSession()) {
    setTuyaError(true);
    return false;
  }
  time_t now = time(nullptr);
  if (now < 1700000000) {
    now = millis() / 1000;
  }

  char payload[160] = {0};
  const int payload_len =
      snprintf(payload, sizeof(payload),
               "{\"protocol\":5,\"t\":%ld,\"data\":{\"dps\":{\"%s\":%s}}}",
               long(now), config.tuya_relay_dps.c_str(), on ? "true" : "false");
  if (payload_len < 0 || size_t(payload_len) >= sizeof(payload)) {
    Serial.println("Tuya switch payload was too long.");
    resetTuyaSession();
    setTuyaError(true);
    return false;
  }

  Serial.print("Sending local Tuya command: ");
  Serial.println(on ? "ON" : "OFF");
  TuyaResponse response;
  if (!sendEncrypted(CMD_CONTROL_NEW, reinterpret_cast<const uint8_t*>(payload),
                     strlen(payload), session_key, true, &response)) {
    Serial.println("Switch command failed.");
    resetTuyaSession();
    setTuyaError(true);
    return false;
  }

  std::vector<uint8_t> clear;
  if (response.payload.empty()) {
    Serial.println("Switch command acknowledged with empty payload.");
    resetTuyaSession();
    setTuyaError(false);
    return true;
  }
  if (decryptPayload(session_key, response, clear)) {
    printClearPayload(clear);
  }
  resetTuyaSession();
  setTuyaError(false);
  runtime_status.relay_state_known = true;
  runtime_status.relay_state = on;
  runtime_status.last_tuya_status = "Switch command OK";
  addDiagnosticLog(String("Tuya switch command OK; relay ") + onOff(on));
  return true;
}

TuyaDiagnostics runTuyaDiagnostics() {
  TuyaDiagnostics diagnostics;
  diagnostics.protocol_version = config.tuya_protocol_version;

  IPAddress tuya_ip;
  if (!parseTuyaIp(tuya_ip)) {
    diagnostics.error = "Tuya IP address is invalid.";
    diagnostics.suggestions = "Enter the plug IP address from your router.";
    return diagnostics;
  }

  uint32_t port_latency = 0;
  diagnostics.port_6668_open =
      testTcpPort(tuya_ip, TUYA_PORT, 900, &port_latency);
  diagnostics.port_6669_open =
      testTcpPort(tuya_ip, TUYA_ALT_PORT, 500, nullptr);
  diagnostics.ip_reachable =
      diagnostics.port_6668_open || diagnostics.port_6669_open;
  if (!diagnostics.port_6668_open) {
    diagnostics.latency_ms = port_latency;
    diagnostics.error = diagnostics.port_6669_open
                            ? "Port 6669 is open, but this firmware currently talks on 6668."
                            : "No Tuya LAN port responded.";
    diagnostics.suggestions =
        "Check that the device is online, on the same LAN/VLAN, and that the IP address is correct.";
    return diagnostics;
  }

  String error;
  if (!tuyaQueryStatusPayload(diagnostics.payload_json, &diagnostics.latency_ms,
                              &error)) {
    diagnostics.error = error;
    diagnostics.suggestions =
        "If the IP and LAN are correct, verify the local key, protocol version, and device ID.";
    return diagnostics;
  }

  diagnostics.auth_ok = true;
  diagnostics.relay_dps_found = parseRelayStateFromText(
      diagnostics.payload_json, config.tuya_relay_dps, diagnostics.relay_state);
  diagnostics.relay_state_known = diagnostics.relay_dps_found;
  if (!diagnostics.relay_dps_found) {
    diagnostics.error = "Connection works, but configured relay DPS was not found.";
    diagnostics.suggestions =
        "Run Scan DPS and choose a boolean datapoint that represents the relay/switch.";
    return diagnostics;
  }

  diagnostics.error = "OK";
  diagnostics.suggestions = "No action needed.";
  return diagnostics;
}

String diagnosticsHtml(const TuyaDiagnostics& diagnostics) {
  String html;
  html.reserve(3000);
  html += F("<div class=\"result-card\"><h3>Tuya connection diagnostics</h3><table>");
  addMetricRow(html, "IP reachable", yesNo(diagnostics.ip_reachable));
  addMetricRow(html, "Tuya port 6668 open", yesNo(diagnostics.port_6668_open));
  addMetricRow(html, "Tuya port 6669 open", yesNo(diagnostics.port_6669_open));
  addMetricRow(html, "Protocol version used", diagnostics.protocol_version);
  addMetricRow(html, "Authentication/local key", diagnostics.auth_ok ? "OK" : "failed or not confirmed");
  addMetricRow(html, "Relay DPS found", yesNo(diagnostics.relay_dps_found));
  addMetricRow(html, "Current relay state",
               diagnostics.relay_state_known ? onOff(diagnostics.relay_state) : "unknown");
  addMetricRow(html, "Response latency", String(diagnostics.latency_ms) + " ms");
  addMetricRow(html, "Summary", diagnostics.error);
  html += F("</table><p class=\"help\"><strong>Suggestion:</strong> ");
  html += htmlEscape(diagnostics.suggestions);
  html += F("</p></div>");
  return html;
}

String dpsInspectorHtml(const String& payload_json) {
  const std::vector<DpsEntry> entries = parseDpsEntries(payload_json);
  String html;
  html.reserve(5000);
  html += F("<div class=\"result-card\"><h3>Experimental DPS inspector</h3>");
  html += F("<p class=\"help\">Read-only scan. It does not toggle unknown datapoints.</p>");
  if (entries.empty()) {
    html += F("<p>No DPS values were found in the response.</p></div>");
    return html;
  }
  html += F("<table><tr><th>DPS</th><th>Value</th><th>Heuristic label</th><th>Action</th></tr>");
  for (const DpsEntry& entry : entries) {
    html += F("<tr><td>");
    html += htmlEscape(entry.key);
    html += F("</td><td><code>");
    html += htmlEscape(entry.value);
    html += F("</code></td><td>");
    html += htmlEscape(entry.label);
    html += F("</td><td>");
    if (entry.is_boolean) {
      html += F("<button type=\"button\" class=\"small\" onclick=\"useDps('");
      html += htmlEscape(entry.key);
      html += F("')\">Use as relay DPS</button>");
    } else {
      html += F("<span class=\"muted\">read only</span>");
    }
    html += F("</td></tr>");
  }
  html += F("</table></div>");
  return html;
}

String lanScanHtml(const BridgeConfig& candidate) {
  String html;
  html.reserve(6000);
  const bool already_on_wifi = WiFi.status() == WL_CONNECTED;
  if (!already_on_wifi && candidate.wifi_ssid.isEmpty()) {
    return F("<div class=\"result-card error\">Enter Wi-Fi SSID first. The ESP32 must join the LAN before it can scan it.</div>");
  }

  if (!already_on_wifi && !connectConfiguredWiFi(candidate, true)) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);
    return F("<div class=\"result-card error\">Wi-Fi connection failed. Check SSID/password before scanning.</div>");
  }

  IPAddress local_ip = WiFi.localIP();
  IPAddress gateway = WiFi.gatewayIP();
  IPAddress probe_ip(local_ip[0], local_ip[1], local_ip[2], 1);
  uint8_t found = 0;
  html += F("<div class=\"result-card\"><h3>Experimental LAN scan</h3>");
  html += F("<p class=\"help\">Results are candidates only. Tuya detection cannot be guaranteed from an open TCP port alone.</p>");
  html += F("<table><tr><th>IP address</th><th>Open port</th><th>Label</th><th>Action</th></tr>");

  for (uint16_t host = 1; host <= 254 && found < LAN_SCAN_MAX_RESULTS; host++) {
    probe_ip[3] = uint8_t(host);
    if (probe_ip == local_ip || probe_ip == gateway) {
      continue;
    }
    uint32_t latency_ms = 0;
    bool open = testTcpPort(probe_ip, TUYA_PORT, LAN_SCAN_CONNECT_TIMEOUT_MS,
                            &latency_ms);
    uint16_t open_port = TUYA_PORT;
    String label = "likely Tuya device";
    if (!open) {
      open = testTcpPort(probe_ip, TUYA_ALT_PORT, LAN_SCAN_CONNECT_TIMEOUT_MS,
                         &latency_ms);
      open_port = TUYA_ALT_PORT;
      label = "possible Tuya device";
    }
    if (!open) {
      continue;
    }
    found++;
    html += F("<tr><td>");
    html += probe_ip.toString();
    html += F("</td><td>");
    html += String(open_port);
    html += F("</td><td>");
    html += label;
    html += F(" (");
    html += String(latency_ms);
    html += F(" ms)</td><td><button type=\"button\" class=\"small\" onclick=\"useIp('");
    html += probe_ip.toString();
    html += F("')\">Use this IP</button></td></tr>");
  }
  if (found == 0) {
    html += F("<tr><td colspan=\"4\">No candidates found. The plug may be offline, on another subnet/VLAN, or blocking Tuya LAN ports.</td></tr>");
  }
  html += F("</table></div>");

  if (!already_on_wifi) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);
  }
  addDiagnosticLog(String("LAN scan finished; candidates found: ") + found);
  return html;
}

bool startMdnsResponder() {
  if (mdns_started) {
    return true;
  }
  if (config.device_hostname.isEmpty()) {
    config.device_hostname = DEFAULT_HOSTNAME;
  }
  if (!MDNS.begin(config.device_hostname.c_str())) {
    Serial.print("mDNS failed for hostname: ");
    Serial.println(config.device_hostname);
    addDiagnosticLog("mDNS failed: " + config.device_hostname);
    mdns_failed = true;
    return false;
  }
  MDNS.addService("http", "tcp", ADMIN_PORT);
  MDNS.addServiceTxt("http", "tcp", "path", "/");
  MDNS.addServiceTxt("http", "tcp", "name", config.homekit_accessory_name);
  mdns_started = true;
  mdns_failed = false;
  Serial.print("mDNS URL: ");
  Serial.println(localDashboardUrl());
  addDiagnosticLog("mDNS started: " + localDashboardUrl());
  return true;
}

String appIconSvg() {
  return F("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 512 512\"><rect width=\"512\" height=\"512\" rx=\"96\" fill=\"#f6f8fa\"/><path d=\"M156 360V226l100-82 100 82v134\" fill=\"none\" stroke=\"#00a6a6\" stroke-width=\"28\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/><path d=\"M132 378h248\" stroke=\"#14212b\" stroke-width=\"32\" stroke-linecap=\"round\"/><path d=\"M256 216v74\" stroke=\"#00a6a6\" stroke-width=\"28\" stroke-linecap=\"round\"/><path d=\"M218 270a56 56 0 1 0 76 0\" fill=\"none\" stroke=\"#00a6a6\" stroke-width=\"28\" stroke-linecap=\"round\"/><path d=\"M148 174V92h216v82\" fill=\"none\" stroke=\"#14212b\" stroke-width=\"28\" stroke-linejoin=\"round\"/><path d=\"M108 176h48M108 236h48M108 296h48M356 176h48M356 236h48M356 296h48\" stroke=\"#14212b\" stroke-width=\"24\" stroke-linecap=\"round\"/></svg>");
}

String webManifestJson() {
  String json;
  json.reserve(700);
  json += F("{\"name\":\"Tuya HomeKit Bridge\",\"short_name\":\"Tuya Bridge\",");
  json += F("\"description\":\"Local ESP32 dashboard for Tuya HomeKit Bridge\",");
  json += F("\"start_url\":\"/\",\"scope\":\"/\",\"display\":\"standalone\",");
  json += F("\"background_color\":\"#f6f8fa\",\"theme_color\":\"#008b8b\",");
  json += F("\"icons\":[{\"src\":\"/icon.svg\",\"sizes\":\"any\",\"type\":\"image/svg+xml\",\"purpose\":\"any maskable\"}],");
  json += F("\"shortcuts\":[{\"name\":\"Dashboard\",\"url\":\"/\",\"description\":\"Open local dashboard\"}]}");
  return json;
}

String exportConfigJson(bool include_sensitive) {
  String json;
  json.reserve(include_sensitive ? 1100 : 850);
  bool first = true;
  json += F("{");
  addJsonString(json, "firmware_version", FIRMWARE_VERSION, first);
  addJsonString(json, "hostname", config.device_hostname, first);
  addJsonString(json, "homekit_accessory_name",
                config.homekit_accessory_name, first);
  addJsonString(json, "homekit_device_type", config.homekit_service_type,
                first);
  addJsonString(json, "tuya_ip", config.tuya_ip, first);
  addJsonString(json, "tuya_device_id", config.tuya_device_id, first);
  addJsonString(json, "tuya_protocol_version",
                config.tuya_protocol_version, first);
  addJsonString(json, "relay_dps", config.tuya_relay_dps, first);
  addJsonNumber(json, "polling_interval", config.poll_interval_seconds, first);
  addJsonNumber(json, "dashboard_port", ADMIN_PORT, first);
  addJsonBool(json, "mdns_enabled", mdns_started, first);
  addJsonString(json, "exported_at", currentTimestampText(), first);
  if (include_sensitive) {
    addJsonString(json, "wifi_ssid", config.wifi_ssid, first);
    addJsonString(json, "wifi_password", config.wifi_password, first);
    addJsonString(json, "tuya_local_key", config.tuya_local_key, first);
  }
  json += F("}\n");
  return json;
}

bool configFromJson(const String& json, BridgeConfig& candidate,
                    String& warnings, String& error) {
  candidate = config;
  String value;
  uint32_t number = 0;

  if (extractJsonString(json, "hostname", value)) {
    if (value.endsWith(".local")) {
      error = "Hostname must not include .local.";
      return false;
    }
    candidate.device_hostname = normalizedHostname(value);
  }
  if (extractJsonString(json, "homekit_accessory_name", value)) {
    candidate.homekit_accessory_name = value;
  }
  if (extractJsonString(json, "homekit_device_type", value)) {
    candidate.homekit_service_type = value;
  }
  if (extractJsonString(json, "tuya_ip", value)) {
    candidate.tuya_ip = value;
  }
  if (extractJsonString(json, "tuya_device_id", value)) {
    candidate.tuya_device_id = value;
  }
  if (extractJsonString(json, "tuya_protocol_version", value)) {
    candidate.tuya_protocol_version = value;
  }
  if (extractJsonString(json, "relay_dps", value)) {
    candidate.tuya_relay_dps = value;
  }
  if (extractJsonUInt(json, "polling_interval", number)) {
    candidate.poll_interval_seconds = normalizedPollInterval(number);
  }
  if (extractJsonString(json, "wifi_ssid", value)) {
    candidate.wifi_ssid = value;
  }
  if (extractJsonString(json, "wifi_password", value)) {
    candidate.wifi_password = value;
  } else if (candidate.wifi_password.isEmpty()) {
    warnings += "Wi-Fi password is missing; enter it before applying if no saved password exists. ";
  }
  if (extractJsonString(json, "tuya_local_key", value)) {
    candidate.tuya_local_key = value;
  } else if (candidate.tuya_local_key.isEmpty()) {
    warnings += "Tuya local key is missing; enter it before applying if no saved key exists. ";
  }

  if (jsonHasKey(json, "dashboard_port")) {
    warnings += "dashboard_port is fixed by this firmware and will be ignored. ";
  }
  if (jsonHasKey(json, "mdns_enabled")) {
    warnings += "mdns_enabled is managed by this firmware and will be ignored. ";
  }
  if (warnings.isEmpty()) {
    warnings = "Unknown fields, if any, were ignored.";
  }
  return validateConfig(candidate, error);
}

String importPreviewHtml(const BridgeConfig& candidate, const String& warnings,
                         bool has_secrets) {
  String html;
  html.reserve(4500);
  html += F("<div class=\"result-card\"><h3>Import preview</h3><table>");
  addMetricRow(html, "Hostname", candidate.device_hostname + ".local");
  addMetricRow(html, "HomeKit accessory name",
               candidate.homekit_accessory_name);
  addMetricRow(html, "HomeKit type", candidate.homekit_service_type);
  addMetricRow(html, "Tuya IP", candidate.tuya_ip);
  addMetricRow(html, "Tuya device ID", candidate.tuya_device_id);
  addMetricRow(html, "Tuya protocol version",
               candidate.tuya_protocol_version);
  addMetricRow(html, "Relay DPS", candidate.tuya_relay_dps);
  addMetricRow(html, "Polling interval",
               String(candidate.poll_interval_seconds) + " seconds");
  addMetricRow(html, "Contains secrets", yesNo(has_secrets));
  html += F("</table><p class=\"help\"><strong>Warnings:</strong> ");
  html += htmlEscape(warnings);
  html += F("</p><p class=\"help\">Review these values. Use Apply import only when they are correct.</p></div>");
  return html;
}

HealthStatus currentHealthStatus() {
  HealthStatus health;
  health.updated_at = currentTimestampText();
  if (WiFi.status() != WL_CONNECTED) {
    health.state = "Error";
    health.reason = "Wi-Fi disconnected.";
    health.suggested_fix = "Check Wi-Fi or hold BOOT for 5s to enter Setup Mode.";
    return health;
  }
  if (!homekit_started && !setup_mode) {
    health.state = "Error";
    health.reason = "HomeKit is not initialized.";
    health.suggested_fix = "Restart the ESP32 or re-save configuration.";
    return health;
  }
  if (ESP.getFreeHeap() < HEALTH_CRITICAL_HEAP) {
    health.state = "Error";
    health.reason = "Critical low memory.";
    health.suggested_fix = "Restart the ESP32 and report the issue.";
    return health;
  }
  if (runtime_status.failed_poll_count >= HEALTH_ERROR_FAILED_POLLS || tuya_error) {
    health.state = "Error";
    health.reason = "Tuya device unreachable.";
    health.suggested_fix = "Check plug power, IP address, local key, and LAN.";
    return health;
  }
  if (WiFi.RSSI() < HEALTH_WEAK_RSSI) {
    health.state = "Warning";
    health.reason = "Weak Wi-Fi signal.";
    health.suggested_fix = "Move ESP32 closer to Wi-Fi or improve coverage.";
    return health;
  }
  if (runtime_status.failed_poll_count >= HEALTH_WARNING_FAILED_POLLS) {
    health.state = "Warning";
    health.reason = String("Tuya device missed ") +
                    runtime_status.failed_poll_count + " polls.";
    health.suggested_fix = "Check plug reachability and consider increasing polling interval.";
    return health;
  }
  if (runtime_status.last_latency_ms > HEALTH_HIGH_LATENCY_MS) {
    health.state = "Warning";
    health.reason = "Tuya response latency is high.";
    health.suggested_fix = "Check Wi-Fi quality and plug responsiveness.";
    return health;
  }
  if (mdns_failed) {
    health.state = "Warning";
    health.reason = "mDNS friendly URL failed to start.";
    health.suggested_fix = "Use the fallback IP URL from Serial Monitor.";
    return health;
  }
  if (ESP.getFreeHeap() < HEALTH_LOW_HEAP_WARNING) {
    health.state = "Warning";
    health.reason = "Free heap is low.";
    health.suggested_fix = "Restart the ESP32 if behavior becomes unstable.";
    return health;
  }
  return health;
}

void handleManifest() {
  requestServer().send(200, "application/manifest+json", webManifestJson());
}

void handleIcon() {
  requestServer().send(200, "image/svg+xml", appIconSvg());
}

void handleCaptivePortalProbe() {
  requestServer().sendHeader("Cache-Control", "no-store");
  requestServer().send(200, "text/html", setupPage(""));
}

String setupPage(const String& message) {
  String page;
  page.reserve(30000);
  page += F("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">");
  page += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  page += F("<meta name=\"theme-color\" content=\"#008b8b\">");
  page += F("<meta name=\"apple-mobile-web-app-capable\" content=\"yes\">");
  page += F("<meta name=\"apple-mobile-web-app-title\" content=\"Tuya Bridge\">");
  page += F("<link rel=\"manifest\" href=\"/manifest.webmanifest\">");
  page += F("<link rel=\"icon\" href=\"/icon.svg\" type=\"image/svg+xml\">");
  page += F("<title>Tuya HomeKit Bridge</title><style>");
  page += F(":root{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;color:#14212b;background:#f6f8fa;line-height:1.45}");
  page += F("body{margin:0;padding:18px}.wrap{max-width:980px;margin:0 auto}.panel{background:white;border:1px solid #d8dee4;border-radius:8px;padding:20px;margin:0 0 16px}");
  page += F("h1{margin:0 0 6px;font-size:28px}h2{font-size:18px;margin:0 0 12px}h3{font-size:16px;margin:0 0 10px}.hint,.help,.muted{color:#57606a;margin-top:0}.help{font-size:13px;line-height:1.35}");
  page += F(".steps{display:flex;gap:8px;overflow:auto;margin:16px 0}.step-dot{border:1px solid #d0d7de;background:#fff;color:#24292f;border-radius:999px;padding:8px 10px;white-space:nowrap;font-size:13px}.step-dot.active{background:#008b8b;color:white;border-color:#008b8b}");
  page += F(".step{display:none}.step.active{display:block}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:14px}label{display:grid;gap:6px;font-weight:600}");
  page += F("input,select{font:inherit;padding:10px;border:1px solid #d0d7de;border-radius:6px;background:white;min-width:0}button{font:inherit;border:0;border-radius:6px;padding:10px 14px;background:#008b8b;color:white;font-weight:700;cursor:pointer}");
  page += F("button.secondary{background:#24292f}button.danger{background:#b42318}button.small{padding:6px 9px;font-size:13px}.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:18px}");
  page += F(".msg{margin:16px 0;padding:12px;border-radius:6px;background:#ddf4ff;border:1px solid #54aeef}.error{background:#ffebe9;border-color:#ff8182}.result-card{margin-top:14px;padding:12px;border:1px solid #d8dee4;border-radius:6px;background:#f6f8fa}");
  page += F(".badge{display:inline-block;border-radius:999px;padding:4px 10px;font-weight:700}.Healthy{background:#dafbe1;color:#116329}.Warning{background:#fff8c5;color:#7d4e00}.Error{background:#ffebe9;color:#b42318}.simple .advanced-only{display:none}");
  page += F("table{border-collapse:collapse;width:100%;font-size:14px}th,td{text-align:left;border-bottom:1px solid #d8dee4;padding:8px;vertical-align:top}th{width:34%;color:#57606a;font-weight:600}code{word-break:break-all}.log{max-height:220px;overflow:auto;background:#0d1117;color:#c9d1d9;border-radius:6px;padding:10px;font:12px ui-monospace,SFMono-Regular,Menlo,monospace}");
  page += F("@media(max-width:640px){body{padding:10px}.panel{padding:14px}h1{font-size:23px}th,td{display:block;width:auto}.steps{padding-bottom:4px}}");
  page += F("</style></head><body><main class=\"wrap\"><section class=\"panel\"><h1>");
  page += homekit_started ? F("Tuya HomeKit Dashboard") : F("Tuya HomeKit Setup");
  page += F("</h1><p class=\"hint\">");
  page += homekit_started
              ? F("Local admin dashboard for configuration, diagnostics and reset actions. This page has no login; use it only on a trusted LAN.")
              : F("Step-by-step setup for Wi-Fi, Tuya LAN credentials and HomeKit. Secrets are stored only in ESP32 flash memory.");
  page += F("</p>");
  if (!message.isEmpty()) {
    page += F("<div class=\"msg\">");
    page += htmlEscape(message);
    page += F("</div>");
  }
  if (homekit_started) {
    const HealthStatus health = currentHealthStatus();
    page += F("<div class=\"result-card\"><h2>Health</h2><p><span class=\"badge ");
    page += htmlEscape(health.state);
    page += F("\">");
    page += htmlEscape(health.state);
    page += F("</span></p><table>");
    addMetricRow(page, "Reason", health.reason);
    addMetricRow(page, "Last update", health.updated_at);
    addMetricRow(page, "Suggested fix", health.suggested_fix);
    page += F("</table></div>");
    page += F("<div class=\"grid\"><div><h2>Device / HomeKit</h2><table>");
    addMetricRow(page, "Accessory name", config.homekit_accessory_name);
    addMetricRow(page, "HomeKit type", homeKitServiceLabel());
    addMetricRow(page, "Current relay state",
                 runtime_status.relay_state_known ? onOff(runtime_status.relay_state)
                                                  : "unknown");
    addMetricRow(page, "Polling interval", String(config.poll_interval_seconds) + " seconds");
    page += F("</table></div><div><h2>Tuya</h2><table>");
    addMetricRow(page, "Tuya IP", config.tuya_ip);
    addMetricRow(page, "Protocol version", config.tuya_protocol_version);
    addMetricRow(page, "Relay DPS", config.tuya_relay_dps);
    addMetricRow(page, "Last status", runtime_status.last_tuya_status);
    addMetricRow(page, "Last latency", String(runtime_status.last_latency_ms) + " ms");
    addMetricRow(page, "Failed poll count", String(runtime_status.failed_poll_count));
    page += F("</table></div><div><h2>Network</h2><table>");
    addMetricRow(page, "Wi-Fi SSID", WiFi.SSID());
    addMetricRow(page, "ESP32 IP address", WiFi.localIP().toString());
    addMetricRow(page, "Local hostname", config.device_hostname + ".local");
    addMetricRow(page, "Dashboard URL", localDashboardUrl());
    addMetricRow(page, "RSSI", String(WiFi.RSSI()) + " dBm");
    addMetricRow(page, "Uptime", formatDuration(millis() / 1000));
    addMetricRow(page, "Free heap", String(ESP.getFreeHeap()) + " bytes");
    page += F("</table></div></div><div class=\"actions\">");
    page += F("<button class=\"secondary\" type=\"button\" id=\"dashTest\">Test Tuya connection</button>");
    page += F("<button class=\"secondary\" type=\"button\" id=\"dashDps\">Scan DPS</button>");
    page += F("<button class=\"secondary\" type=\"button\" id=\"restart\">Restart ESP32</button>");
    page += F("<button class=\"danger\" type=\"button\" id=\"resetDash\">Reset configuration</button>");
    page += F("<button class=\"danger\" type=\"button\" id=\"unpairDash\">Reset HomeKit pairing</button>");
    page += F("</div><div id=\"dashboardResult\"></div><h2 style=\"margin-top:18px\">Diagnostics log</h2><div class=\"log\">");
    if (diagnostic_log_count == 0) {
      page += F("No diagnostic events yet.");
    } else {
      for (uint8_t i = 0; i < diagnostic_log_count; i++) {
        const uint8_t index = (diagnostic_log_start + i) % DIAGNOSTIC_LOG_SIZE;
        page += htmlEscape(diagnostic_log[index]);
        page += F("<br>");
      }
    }
    page += F("</div>");
    page += F("<div class=\"result-card\"><h3>Management URL</h3><p class=\"help\">Use this bookmark after setup instead of searching for the ESP32 IP address.</p><p><a href=\"");
    page += htmlEscape(localDashboardUrl());
    page += F("\">");
    page += htmlEscape(localDashboardUrl());
    page += F("</a></p>");
    const String ip_url = ipDashboardUrl();
    if (!ip_url.isEmpty()) {
      page += F("<p class=\"help\">Fallback IP URL: <a href=\"");
      page += htmlEscape(ip_url);
      page += F("\">");
      page += htmlEscape(ip_url);
      page += F("</a></p>");
    }
    page += F("</div>");
  }
  page += F("</section><section class=\"panel\"><h2>");
  page += homekit_started ? F("Edit configuration") : F("Setup wizard");
  page += F("</h2><form id=\"setup\" method=\"post\" action=\"/save\">");
  page += F("<div class=\"actions\"><label><input type=\"radio\" name=\"wizard_mode\" value=\"simple\" checked> Simple</label><label><input type=\"radio\" name=\"wizard_mode\" value=\"advanced\"> Advanced</label></div>");
  page += F("<div class=\"steps\"><button type=\"button\" class=\"step-dot active\" data-step-button=\"0\">1 Wi-Fi</button><button type=\"button\" class=\"step-dot\" data-step-button=\"1\">2 Find Tuya</button><button type=\"button\" class=\"step-dot\" data-step-button=\"2\">3 Credentials</button><button type=\"button\" class=\"step-dot\" data-step-button=\"3\">4 Test</button><button type=\"button\" class=\"step-dot\" data-step-button=\"4\">5 HomeKit</button><button type=\"button\" class=\"step-dot\" data-step-button=\"5\">6 Save</button></div>");

  page += F("<div class=\"step active\" data-step=\"0\"><div class=\"grid\">");
  page += F("<label>Wi-Fi SSID<input name=\"wifi_ssid\" required value=\"");
  page += htmlEscape(config.wifi_ssid);
  page += F("\"><span class=\"help\">Name of your home, IoT, or guest Wi-Fi network.</span></label>");
  page += F("<label>Wi-Fi password<input name=\"wifi_password\" type=\"password\" value=\"");
  page += F("\" placeholder=\"Leave blank to keep saved password\"><span class=\"help\">Leave blank when editing if the saved Wi-Fi password should stay unchanged.</span></label>");
  page += F("<label class=\"advanced-only\">Local dashboard hostname<input name=\"device_hostname\" required maxlength=\"32\" pattern=\"[a-zA-Z0-9-]{1,32}\" value=\"");
  page += htmlEscape(config.device_hostname);
  page += F("\"><span class=\"help\">Bookmark http://");
  page += htmlEscape(config.device_hostname);
  page += F(".local:");
  page += String(ADMIN_PORT);
  page += F("/ after setup. Default is tuya-homekit.local. Use only letters, numbers, and hyphens.</span></label>");
  page += F("</div></div>");

  page += F("<div class=\"step\" data-step=\"1\"><p class=\"help\">Experimental LAN scan checks common Tuya ports on the ESP32 subnet. Results are only candidates.</p><div class=\"actions\"><button class=\"secondary\" type=\"button\" id=\"scan\">Find Tuya devices</button></div><div id=\"scanResult\"></div>");
  page += F("<label>Tuya plug IP address<input name=\"tuya_ip\" required placeholder=\"192.168.1.123\" value=\"");
  page += htmlEscape(config.tuya_ip);
  page += F("\"><span class=\"help\">Local IP address of the Tuya / Smart Life plug on your router.</span></label>");
  page += F("</div>");

  page += F("<div class=\"step\" data-step=\"2\"><div class=\"grid\">");
  page += F("<label>Tuya device ID<input name=\"tuya_device_id\" required value=\"");
  page += htmlEscape(config.tuya_device_id);
  page += F("\"><span class=\"help\">Device ID from Tuya / Smart Life developer data. This is not the IP address.</span></label>");
  page += F("<label>Tuya local key<input name=\"tuya_local_key\" maxlength=\"16\" value=\"");
  page += F("\" placeholder=\"Leave blank to keep saved key\"><span class=\"help\">16-character local key used by the ESP32 to talk directly to the plug. Leave blank when editing to keep the saved key.</span></label>");
  page += F("<label class=\"advanced-only\">Tuya protocol version<input name=\"tuya_protocol_version\" required value=\"");
  page += htmlEscape(config.tuya_protocol_version);
  page += F("\"><span class=\"help\">Keep 3.4 for the tested plug. Other versions are not supported by this first wizard.</span></label>");
  page += F("<label class=\"advanced-only\">Relay DPS<input name=\"tuya_relay_dps\" required value=\"");
  page += htmlEscape(config.tuya_relay_dps);
  page += F("\"><span class=\"help\">DPS means Tuya datapoint. It is the number Tuya uses for a value inside the plug. For the tested socket, relay on/off is DPS 1.</span></label>");
  page += F("</div></div>");

  page += F("<div class=\"step\" data-step=\"3\"><p class=\"help\">Run Test Tuya connection first. If it succeeds, Advanced mode can scan returned DPS datapoints without toggling anything.</p><div class=\"actions\"><button class=\"secondary\" type=\"button\" id=\"test\">Test Tuya connection</button><button class=\"secondary advanced-only\" type=\"button\" id=\"dps\">Scan DPS</button></div><div id=\"result\"></div></div>");

  page += F("<div class=\"step\" data-step=\"4\"><div class=\"grid\">");
  page += F("<label>HomeKit accessory name<input name=\"homekit_accessory_name\" required value=\"");
  page += htmlEscape(config.homekit_accessory_name);
  page += F("\"><span class=\"help\">Name shown in Apple Home. Changing it later may require removing and adding the accessory again.</span></label>");
  page += F("<label>HomeKit type<select name=\"homekit_service_type\">");
  page += F("<option value=\"outlet\"");
  page += optionSelected(config.homekit_service_type, "outlet");
  page += F(">Outlet</option><option value=\"light\"");
  page += optionSelected(config.homekit_service_type, "light");
  page += F(">Light</option><option value=\"switch\"");
  page += optionSelected(config.homekit_service_type, "switch");
  page += F(">Switch</option></select><span class=\"help\">Outlet is best for a physical smart plug. Use Light only if the plug controls a lamp. Changing type usually needs HomeKit unpair and re-pair.</span></label>");
  page += F("<label>HomeKit pairing code<input name=\"homekit_pairing_code\" inputmode=\"numeric\" pattern=\"[0-9]{8}\" maxlength=\"8\" placeholder=\"Optional, 8 digits\"><span class=\"help\">Code used when adding the ESP32 in Apple Home. Enter your own 8-digit code and write it down. If blank and never changed, HomeSpan uses default 466-37-726. If forgotten, enter a new code and save.</span></label>");
  page += F("</div><div class=\"result-card\"><h3>HomeKit pairing</h3><p class=\"help\">Apple Home asks for a HomeKit setup code when adding the accessory. The code is not your Wi-Fi password and it is not the temporary setup AP password. Use the code you entered above, or the HomeSpan default 466-37-726 if you never changed it.</p></div></div>");

  page += F("<div class=\"step\" data-step=\"5\"><div class=\"grid\">");
  page += F("<label class=\"advanced-only\">Polling interval, seconds<input name=\"poll_interval_seconds\" type=\"number\" min=\"5\" max=\"3600\" value=\"");
  page += String(config.poll_interval_seconds);
  page += F("\"><span class=\"help\">How often the ESP32 checks the plug state in the background. 30 seconds is a safe default.</span></label></div><p class=\"help\">Saving restarts the ESP32. After restart, use Apple Home to pair with the HomeKit code above.</p>");
  page += F("</div><div class=\"actions\">");
  page += F("<button class=\"secondary\" type=\"button\" id=\"prev\">Back</button><button class=\"secondary\" type=\"button\" id=\"next\">Next</button>");
  page += F("<button type=\"submit\">Save and restart</button>");
  page += F("<button class=\"danger\" type=\"button\" id=\"reset\">Clear saved config</button>");
  if (homekit_started) {
    page += F("<button class=\"danger\" type=\"button\" id=\"unpair\">Clear HomeKit pairing</button>");
  }
  page += F("</div></form><div class=\"result-card\"><h3>Backup and restore</h3><p class=\"help\">Exports exclude Wi-Fi password and Tuya local key by default.</p><label><input type=\"checkbox\" id=\"includeSecrets\"> Include sensitive values</label><div class=\"actions\"><button class=\"secondary\" type=\"button\" id=\"exportCfg\">Export config</button></div><label class=\"advanced-only\">Import JSON config<textarea id=\"importJson\" name=\"import_json\" rows=\"8\" style=\"font:inherit;width:100%;box-sizing:border-box\"></textarea><span class=\"help\">Unknown fields are ignored. Values are validated before saving.</span></label><input class=\"advanced-only\" type=\"file\" id=\"importFile\" accept=\"application/json,.json\"><div class=\"actions advanced-only\"><button class=\"secondary\" type=\"button\" id=\"previewImport\">Preview import</button><button class=\"danger\" type=\"button\" id=\"applyImport\">Apply import and restart</button></div><div id=\"importResult\"></div></div><p class=\"help\">Setup AP: ");
  page += SETUP_AP_SSID;
  page += F(" · In setup mode open http://192.168.4.1/ if the captive portal does not appear automatically · In normal mode open ");
  page += htmlEscape(localDashboardUrl());
  page += F(" or the ESP32 IP with port ");
  page += String(ADMIN_PORT);
  page += F(".</p></section>");
  page += F("<script>");
  page += F("const form=document.getElementById('setup');const steps=[...document.querySelectorAll('.step')];const dots=[...document.querySelectorAll('.step-dot')];let step=0;");
  page += F("function setMode(mode){document.body.classList.toggle('simple',mode==='simple');}document.querySelectorAll('input[name=\"wizard_mode\"]').forEach(r=>r.onchange=()=>setMode(r.value));setMode('simple');");
  page += F("function showStep(n){step=Math.max(0,Math.min(steps.length-1,n));steps.forEach((el,i)=>el.classList.toggle('active',i===step));dots.forEach((el,i)=>el.classList.toggle('active',i===step));}");
  page += F("dots.forEach((b,i)=>b.onclick=()=>showStep(i));document.getElementById('next').onclick=()=>showStep(step+1);document.getElementById('prev').onclick=()=>showStep(step-1);");
  page += F("function target(id){return document.getElementById(id)||document.getElementById('dashboardResult')||document.getElementById('result');}");
  page += F("async function postHtml(url,outId,msg){const out=target(outId);out.innerHTML='<div class=\"result-card\">'+msg+'</div>';try{const r=await fetch(url,{method:'POST',body:new FormData(form)});out.innerHTML=await r.text();}catch(e){out.innerHTML='<div class=\"result-card error\">Request failed.</div>';}}");
  page += F("document.getElementById('test').onclick=()=>postHtml('/test','result','Testing Tuya connection...');document.getElementById('dps').onclick=()=>postHtml('/dps','result','Scanning DPS...');document.getElementById('scan').onclick=()=>postHtml('/scan','scanResult','Scanning LAN. This can take a few seconds...');");
  page += F("const dt=document.getElementById('dashTest');if(dt)dt.onclick=()=>postHtml('/test','dashboardResult','Testing Tuya connection...');const dd=document.getElementById('dashDps');if(dd)dd.onclick=()=>postHtml('/dps','dashboardResult','Scanning DPS...');");
  page += F("window.useIp=ip=>{form.tuya_ip.value=ip;showStep(2)};window.useDps=dps=>{form.tuya_relay_dps.value=dps;showStep(2)};");
  page += F("async function postPlain(url,msg){const out=target('dashboardResult');out.innerHTML='<div class=\"result-card\">'+msg+'</div>';const r=await fetch(url,{method:'POST'});out.innerHTML='<div class=\"result-card\">'+await r.text()+'</div>';}");
  page += F("document.getElementById('exportCfg').onclick=()=>{const s=document.getElementById('includeSecrets').checked;if(s&&!confirm('This file contains secrets. Anyone with this file may be able to access your Wi-Fi or control your Tuya device. Continue?'))return;location.href='/export?include_sensitive='+(s?'1':'0')};");
  page += F("const file=document.getElementById('importFile');if(file)file.onchange=async()=>{const f=file.files[0];if(f)document.getElementById('importJson').value=await f.text()};");
  page += F("function importData(){const fd=new FormData();fd.append('import_json',document.getElementById('importJson').value);return fd}document.getElementById('previewImport').onclick=async()=>{const out=document.getElementById('importResult');out.innerHTML='<div class=\"result-card\">Validating import...</div>';const r=await fetch('/import/preview',{method:'POST',body:importData()});out.innerHTML=await r.text()};");
  page += F("document.getElementById('applyImport').onclick=async()=>{if(!confirm('Apply imported configuration and restart ESP32?'))return;const out=document.getElementById('importResult');out.innerHTML='<div class=\"result-card\">Applying import...</div>';const r=await fetch('/import/apply',{method:'POST',body:importData()});out.innerHTML='<div class=\"result-card\">'+await r.text()+'</div>'};");
  page += F("document.getElementById('reset').onclick=()=>{if(confirm('Clear saved configuration and restart setup mode?'))postPlain('/reset','Resetting...')};const rd=document.getElementById('resetDash');if(rd)rd.onclick=()=>{if(confirm('Clear saved configuration and restart setup mode?'))postPlain('/reset','Resetting...')};");
  page += F("const unpair=document.getElementById('unpair');if(unpair)unpair.onclick=()=>{if(confirm('Clear HomeKit pairing on the ESP32? Also remove this accessory in Apple Home.'))postPlain('/unpair','Clearing HomeKit pairing...')};const ud=document.getElementById('unpairDash');if(ud)ud.onclick=()=>{if(confirm('Clear HomeKit pairing on the ESP32? Also remove this accessory in Apple Home.'))postPlain('/unpair','Clearing HomeKit pairing...')};");
  page += F("const restart=document.getElementById('restart');if(restart)restart.onclick=()=>{if(confirm('Restart ESP32 now?'))postPlain('/restart','Restarting...')};");
  page += F("</script></main></body></html>");
  return page;
}

bool connectConfiguredWiFi(const BridgeConfig& candidate, bool keep_ap) {
  WiFi.mode(keep_ap ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(candidate.device_hostname.c_str());
  WiFi.begin(candidate.wifi_ssid.c_str(), candidate.wifi_password.c_str());
  Serial.print("Connecting to Wi-Fi");
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection failed.");
    addDiagnosticLog("Wi-Fi connection failed");
    return false;
  }
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
  addDiagnosticLog("Wi-Fi connected: " + WiFi.localIP().toString());
  return true;
}

bool connectConfiguredWiFiWithRetries() {
  for (uint8_t attempt = 1; attempt <= WIFI_CONNECT_ATTEMPTS; attempt++) {
    Serial.printf("Wi-Fi attempt %u of %u.\n", attempt, WIFI_CONNECT_ATTEMPTS);
    if (connectConfiguredWiFi(config, false)) {
      return true;
    }
    WiFi.disconnect(true);
    delay(1000);
  }
  return false;
}

void handleSetupRoot() {
  requestServer().send(200, "text/html", setupPage(""));
}

void handleSaveConfig() {
  const BridgeConfig candidate = configFromRequest();
  String error;
  if (!saveConfig(candidate, error)) {
    requestServer().send(400, "text/html", setupPage(error));
    return;
  }
  String message = "Configuration saved. ESP32 is restarting.";
  if (!candidate.homekit_pairing_code.isEmpty()) {
    message += " HomeKit pairing code: ";
    message += formattedHomeKitCode(candidate.homekit_pairing_code);
    message += ". Write it down before pairing.";
  }
  requestServer().send(200, "text/html", setupPage(message));
  delay(candidate.homekit_pairing_code.isEmpty() ? 700 : 2500);
  ESP.restart();
}

void handleResetConfig() {
  clearConfig();
  addDiagnosticLog("Configuration reset from web UI");
  requestServer().send(200, "text/plain", "Configuration cleared. Restarting...");
  delay(700);
  ESP.restart();
}

void handleRestart() {
  addDiagnosticLog("Restart requested from dashboard");
  requestServer().send(200, "text/plain", "ESP32 is restarting...");
  delay(700);
  ESP.restart();
}

void handleExportConfig() {
  const bool include_sensitive = requestServer().arg("include_sensitive") == "1";
  requestServer().sendHeader(
      "Content-Disposition",
      "attachment; filename=\"tuya-homekit-bridge-config.json\"");
  requestServer().send(200, "application/json",
                       exportConfigJson(include_sensitive));
  addDiagnosticLog(include_sensitive ? "Config exported with secrets"
                                     : "Config exported without secrets");
}

void handleImportPreview() {
  const String json = requestServer().arg("import_json");
  if (json.isEmpty()) {
    requestServer().send(400, "text/html",
                         F("<div class=\"result-card error\">Paste or upload a JSON config first.</div>"));
    return;
  }
  BridgeConfig candidate;
  String warnings;
  String error;
  if (!configFromJson(json, candidate, warnings, error)) {
    requestServer().send(400, "text/html",
                         String("<div class=\"result-card error\">Import validation failed: ") +
                             htmlEscape(error) + "</div>");
    return;
  }
  const bool has_secrets = jsonHasKey(json, "wifi_password") ||
                           jsonHasKey(json, "tuya_local_key");
  requestServer().send(200, "text/html",
                       importPreviewHtml(candidate, warnings, has_secrets));
}

void handleImportApply() {
  const String json = requestServer().arg("import_json");
  if (json.isEmpty()) {
    requestServer().send(400, "text/plain", "Paste or upload a JSON config first.");
    return;
  }
  BridgeConfig candidate;
  String warnings;
  String error;
  if (!configFromJson(json, candidate, warnings, error)) {
    requestServer().send(400, "text/plain",
                         String("Import validation failed: ") + error);
    return;
  }
  if (!saveConfig(candidate, error)) {
    requestServer().send(400, "text/plain",
                         String("Import save failed: ") + error);
    return;
  }
  addDiagnosticLog("Configuration imported from JSON");
  requestServer().send(200, "text/plain",
                       "Configuration imported. ESP32 is restarting...");
  delay(1200);
  ESP.restart();
}

void handleUnpairHomeKit() {
  if (!homekit_started) {
    requestServer().send(409, "text/plain",
                      "HomeKit is not running in setup mode.");
    return;
  }
  requestServer().send(200, "text/plain",
                    "HomeKit pairing cleared on the ESP32. Remove the accessory in Apple Home too. Restarting...");
  delay(200);
  homeSpan.processSerialCommand("U");
  delay(700);
  ESP.restart();
}

void performFactoryReset(const char* reason) {
  if (factory_reset_started) {
    return;
  }
  factory_reset_started = true;
  Serial.println();
  Serial.print("Factory reset requested");
  if (reason && strlen(reason) > 0) {
    Serial.print(": ");
    Serial.print(reason);
  }
  Serial.println();
  clearConfig();
  Serial.println("Clearing HomeKit device ID and pairing data.");
  homeSpan.processSerialCommand("H");
  delay(1000);
  Serial.println("Restarting into setup mode.");
  delay(500);
  ESP.restart();
}

void handleResetButton() {
  const bool pressed = digitalRead(RESET_CONFIG_PIN) == LOW;
  if (pressed && !reset_button_was_pressed) {
    reset_button_pressed_ms = millis();
    reset_button_was_pressed = true;
    setup_mode_button_started = false;
    Serial.println("BOOT button pressed. Hold 5s for Setup Mode or 15s for Factory Reset.");
  }
  if (!pressed) {
    if (reset_button_was_pressed && !factory_reset_started) {
      Serial.println("BOOT button released.");
    }
    reset_button_was_pressed = false;
    setup_mode_button_started = false;
    reset_button_pressed_ms = 0;
    return;
  }
  if (!setup_mode_button_started && !setup_mode &&
      millis() - reset_button_pressed_ms >= SETUP_MODE_HOLD_MS) {
    setup_mode_button_started = true;
    Serial.println("BOOT held for 5 seconds: entering Setup Mode without erasing configuration.");
    startSetupMode("BOOT/GPIO0 held for 5 seconds");
  }
  if (!factory_reset_started &&
      millis() - reset_button_pressed_ms >= FACTORY_RESET_HOLD_MS) {
    performFactoryReset("BOOT/GPIO0 held for 15 seconds");
  }
}

void handleTestConfig() {
  const BridgeConfig candidate = configFromRequest();
  String error;
  if (!validateConfig(candidate, error)) {
    requestServer().send(400, "text/html",
                         String("<div class=\"result-card error\">") +
                             htmlEscape(error) + "</div>");
    return;
  }

  const BridgeConfig previous = config;
  config = candidate;
  if (!parseLocalKey()) {
    config = previous;
    requestServer().send(400, "text/html",
                         F("<div class=\"result-card error\">Tuya local key is invalid.</div>"));
    return;
  }

  const bool already_on_wifi = homekit_started && WiFi.status() == WL_CONNECTED;
  if (!already_on_wifi) {
    if (!connectConfiguredWiFi(candidate, true)) {
      WiFi.disconnect(false);
      config = previous;
      requestServer().send(408, "text/html",
                           F("<div class=\"result-card error\">Wi-Fi connection failed. Check SSID and password.</div>"));
      return;
    }
  }
  const TuyaDiagnostics diagnostics = runTuyaDiagnostics();
  resetTuyaSession();
  if (!already_on_wifi) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);
  }
  config = previous;
  if (diagnostics.error == "OK") {
    runtime_status.last_tuya_status = "Manual test OK";
    runtime_status.last_latency_ms = diagnostics.latency_ms;
    runtime_status.relay_state_known = diagnostics.relay_state_known;
    runtime_status.relay_state = diagnostics.relay_state;
    addDiagnosticLog("Test connection OK");
  } else {
    runtime_status.last_tuya_status = diagnostics.error;
    runtime_status.failed_poll_count++;
    addDiagnosticLog("Test connection failed: " + diagnostics.error);
  }
  requestServer().send(diagnostics.error == "OK" ? 200 : 502, "text/html",
                       diagnosticsHtml(diagnostics));
}

void handleLanScan() {
  const BridgeConfig candidate = configFromRequest();
  requestServer().send(200, "text/html", lanScanHtml(candidate));
}

void handleDpsScan() {
  const BridgeConfig candidate = configFromRequest();
  String error;
  if (!validateConfig(candidate, error)) {
    requestServer().send(400, "text/html",
                         String("<div class=\"result-card error\">") +
                             htmlEscape(error) + "</div>");
    return;
  }

  const BridgeConfig previous = config;
  config = candidate;
  if (!parseLocalKey()) {
    config = previous;
    requestServer().send(400, "text/html",
                         F("<div class=\"result-card error\">Tuya local key is invalid.</div>"));
    return;
  }

  const bool already_on_wifi = homekit_started && WiFi.status() == WL_CONNECTED;
  if (!already_on_wifi && !connectConfiguredWiFi(candidate, true)) {
    WiFi.disconnect(false);
    config = previous;
    requestServer().send(408, "text/html",
                         F("<div class=\"result-card error\">Wi-Fi connection failed. Check SSID and password.</div>"));
    return;
  }

  String payload_json;
  uint32_t latency_ms = 0;
  String query_error;
  const bool ok = tuyaQueryStatusPayload(payload_json, &latency_ms, &query_error);
  resetTuyaSession();
  if (!already_on_wifi) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);
  }
  config = previous;

  if (!ok) {
    runtime_status.last_tuya_status = query_error;
    runtime_status.failed_poll_count++;
    addDiagnosticLog("DPS scan failed: " + query_error);
    requestServer().send(502, "text/html",
                         String("<div class=\"result-card error\">DPS scan failed: ") +
                             htmlEscape(query_error) + "</div>");
    return;
  }

  runtime_status.last_tuya_status = "DPS scan OK";
  runtime_status.last_latency_ms = latency_ms;
  addDiagnosticLog("DPS scan OK");
  requestServer().send(200, "text/html", dpsInspectorHtml(payload_json));
}

void startSetupMode(const String& reason, bool retry_saved_wifi) {
  setup_mode = true;
  setup_retry_saved_wifi = retry_saved_wifi;
  last_setup_wifi_retry_ms = millis();
  last_setup_led_toggle_ms = 0;
  setup_led_state = false;
  setStatusLed(false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  setup_ap_password = setupPasswordFromRandom();
  const bool ap_started =
      WiFi.softAP(SETUP_AP_SSID, setup_ap_password.c_str());
  Serial.println();
  Serial.println("Starting setup mode.");
  if (!reason.isEmpty()) {
    Serial.print("Reason: ");
    Serial.println(reason);
  }
  if (!ap_started) {
    Serial.println("Failed to start setup access point.");
    return;
  }
  Serial.print("Setup AP SSID: ");
  Serial.println(SETUP_AP_SSID);
  Serial.print("Setup AP password: ");
  Serial.println(setup_ap_password);
  Serial.print("Setup URL: http://");
  Serial.println(WiFi.softAPIP());
  captive_dns_started = dns_server.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.print("Captive portal DNS: ");
  Serial.println(captive_dns_started ? "started" : "failed");

  setup_server.on("/", HTTP_GET, handleSetupRoot);
  setup_server.on("/manifest.webmanifest", HTTP_GET, handleManifest);
  setup_server.on("/icon.svg", HTTP_GET, handleIcon);
  setup_server.on("/export", HTTP_GET, handleExportConfig);
  setup_server.on("/import/preview", HTTP_POST, handleImportPreview);
  setup_server.on("/import/apply", HTTP_POST, handleImportApply);
  setup_server.on("/generate_204", HTTP_GET, handleCaptivePortalProbe);
  setup_server.on("/gen_204", HTTP_GET, handleCaptivePortalProbe);
  setup_server.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortalProbe);
  setup_server.on("/library/test/success.html", HTTP_GET, handleCaptivePortalProbe);
  setup_server.on("/connecttest.txt", HTTP_GET, handleCaptivePortalProbe);
  setup_server.on("/fwlink", HTTP_GET, handleCaptivePortalProbe);
  setup_server.on("/save", HTTP_POST, handleSaveConfig);
  setup_server.on("/reset", HTTP_POST, handleResetConfig);
  setup_server.on("/test", HTTP_POST, handleTestConfig);
  setup_server.on("/scan", HTTP_POST, handleLanScan);
  setup_server.on("/dps", HTTP_POST, handleDpsScan);
  setup_server.on("/unpair", HTTP_POST, handleUnpairHomeKit);
  setup_server.on("/restart", HTTP_POST, handleRestart);
  setup_server.onNotFound(handleSetupRoot);
  setup_server.begin();
  addDiagnosticLog("Setup mode started");
}

void startAdminServer() {
  admin_server.on("/", HTTP_GET, handleSetupRoot);
  admin_server.on("/manifest.webmanifest", HTTP_GET, handleManifest);
  admin_server.on("/icon.svg", HTTP_GET, handleIcon);
  admin_server.on("/export", HTTP_GET, handleExportConfig);
  admin_server.on("/import/preview", HTTP_POST, handleImportPreview);
  admin_server.on("/import/apply", HTTP_POST, handleImportApply);
  admin_server.on("/save", HTTP_POST, handleSaveConfig);
  admin_server.on("/reset", HTTP_POST, handleResetConfig);
  admin_server.on("/test", HTTP_POST, handleTestConfig);
  admin_server.on("/scan", HTTP_POST, handleLanScan);
  admin_server.on("/dps", HTTP_POST, handleDpsScan);
  admin_server.on("/unpair", HTTP_POST, handleUnpairHomeKit);
  admin_server.on("/restart", HTTP_POST, handleRestart);
  admin_server.onNotFound(handleSetupRoot);
  admin_server.begin();
  Serial.print("Admin URL: http://");
  Serial.print(WiFi.localIP());
  Serial.print(":");
  Serial.println(ADMIN_PORT);
  Serial.print("Friendly admin URL: ");
  Serial.println(localDashboardUrl());
  addDiagnosticLog("Admin dashboard started");
}

void maybeRetrySavedWiFiFromSetup() {
  if (!setup_retry_saved_wifi ||
      millis() - last_setup_wifi_retry_ms < SETUP_WIFI_RETRY_INTERVAL_MS) {
    return;
  }
  last_setup_wifi_retry_ms = millis();
  Serial.println("Retrying saved Wi-Fi from setup mode.");
  if (!connectConfiguredWiFi(config, true)) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);
    Serial.println("Saved Wi-Fi still unavailable; setup mode remains active.");
    return;
  }
  Serial.println("Saved Wi-Fi is available again; restarting normal mode.");
  delay(500);
  ESP.restart();
}

bool handleBootButtonHold() {
  if (digitalRead(RESET_CONFIG_PIN) != LOW) {
    return false;
  }
  Serial.println("BOOT held during boot.");
  Serial.println("Release after 5s for Setup Mode, keep holding 15s for Factory Reset.");
  const unsigned long start = millis();
  bool setup_threshold_logged = false;
  while (digitalRead(RESET_CONFIG_PIN) == LOW) {
    const unsigned long held_ms = millis() - start;
    if (!setup_threshold_logged && held_ms >= SETUP_MODE_HOLD_MS) {
      setup_threshold_logged = true;
      Serial.println("5s reached: release now for Setup Mode, keep holding for Factory Reset.");
    }
    if (held_ms >= FACTORY_RESET_HOLD_MS) {
      performFactoryReset("BOOT/GPIO0 held for 15 seconds during boot");
      return true;
    }
    setStatusLed((held_ms / 250) % 2 == 0);
    delay(50);
  }
  setStatusLed(false);
  if (millis() - start >= SETUP_MODE_HOLD_MS) {
    Serial.println("Entering Setup Mode without erasing saved configuration.");
    loadConfig();
    startSetupMode("BOOT/GPIO0 held for 5 seconds during boot");
    return true;
  }
  Serial.println("BOOT released before 5 seconds; continuing normal boot.");
  return false;
}

void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for time");
  for (int i = 0; i < 20; i++) {
    time_t now = time(nullptr);
    if (now > 1700000000) {
      Serial.println();
      Serial.printf("Unix time: %ld\n", long(now));
      return;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("Time sync not ready; commands will use uptime fallback.");
}

String readCommand() {
  static String line;
  while (Serial.available() > 0) {
    char ch = char(Serial.read());
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      String command = line;
      line = "";
      command.trim();
      command.toLowerCase();
      return command;
    }
    line += ch;
  }
  return "";
}

void printHelp() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  status");
  Serial.println("  on");
  Serial.println("  off");
  Serial.println();
}

struct HomeKitTuyaOutlet : Service::Outlet {
  SpanCharacteristic* power;
  SpanCharacteristic* outlet_in_use;
  unsigned long last_poll_ms = 0;
  bool last_known_power = false;

  HomeKitTuyaOutlet() : Service::Outlet() {
    bool initial_power = false;
    if (tuyaReadPower(initial_power)) {
      last_known_power = initial_power;
    }
    power = new Characteristic::On(last_known_power);
    outlet_in_use = new Characteristic::OutletInUse(last_known_power);
  }

  boolean update() override {
    if (!power->updated()) {
      return true;
    }

    bool requested_power = power->getNewVal<bool>();
    Serial.print("HomeKit requested outlet ");
    Serial.println(requested_power ? "ON" : "OFF");

    if (!tuyaSwitch(requested_power)) {
      Serial.println("Tuya command failed; rejecting HomeKit update.");
      return false;
    }

    last_known_power = requested_power;
    outlet_in_use->setVal(requested_power);
    last_poll_ms = millis();
    return true;
  }

  void loop() override {
    const uint32_t poll_interval_ms =
        normalizedPollInterval(config.poll_interval_seconds) * 1000;
    if (millis() - last_poll_ms < poll_interval_ms) {
      return;
    }
    last_poll_ms = millis();

    bool current_power = last_known_power;
    if (!tuyaReadPower(current_power)) {
      Serial.println("Periodic Tuya status poll failed.");
      return;
    }

    if (current_power != power->getVal<bool>()) {
      Serial.print("External outlet state changed to ");
      Serial.println(current_power ? "ON" : "OFF");
      power->setVal(current_power);
    }
    if (current_power != outlet_in_use->getVal<bool>()) {
      outlet_in_use->setVal(current_power);
    }
    last_known_power = current_power;
  }
};

struct HomeKitTuyaLight : Service::LightBulb {
  SpanCharacteristic* power;
  unsigned long last_poll_ms = 0;
  bool last_known_power = false;

  HomeKitTuyaLight() : Service::LightBulb() {
    bool initial_power = false;
    if (tuyaReadPower(initial_power)) {
      last_known_power = initial_power;
    }
    power = new Characteristic::On(last_known_power);
  }

  boolean update() override {
    if (!power->updated()) {
      return true;
    }

    const bool requested_power = power->getNewVal<bool>();
    Serial.print("HomeKit requested light ");
    Serial.println(requested_power ? "ON" : "OFF");

    if (!tuyaSwitch(requested_power)) {
      Serial.println("Tuya command failed; rejecting HomeKit update.");
      return false;
    }

    last_known_power = requested_power;
    last_poll_ms = millis();
    return true;
  }

  void loop() override {
    const uint32_t poll_interval_ms =
        normalizedPollInterval(config.poll_interval_seconds) * 1000;
    if (millis() - last_poll_ms < poll_interval_ms) {
      return;
    }
    last_poll_ms = millis();

    bool current_power = last_known_power;
    if (!tuyaReadPower(current_power)) {
      Serial.println("Periodic Tuya status poll failed.");
      return;
    }

    if (current_power != power->getVal<bool>()) {
      Serial.print("External light state changed to ");
      Serial.println(current_power ? "ON" : "OFF");
      power->setVal(current_power);
    }
    last_known_power = current_power;
  }
};

struct HomeKitTuyaSwitch : Service::Switch {
  SpanCharacteristic* power;
  unsigned long last_poll_ms = 0;
  bool last_known_power = false;

  HomeKitTuyaSwitch() : Service::Switch() {
    bool initial_power = false;
    if (tuyaReadPower(initial_power)) {
      last_known_power = initial_power;
    }
    power = new Characteristic::On(last_known_power);
  }

  boolean update() override {
    if (!power->updated()) {
      return true;
    }

    const bool requested_power = power->getNewVal<bool>();
    Serial.print("HomeKit requested switch ");
    Serial.println(requested_power ? "ON" : "OFF");

    if (!tuyaSwitch(requested_power)) {
      Serial.println("Tuya command failed; rejecting HomeKit update.");
      return false;
    }

    last_known_power = requested_power;
    last_poll_ms = millis();
    return true;
  }

  void loop() override {
    const uint32_t poll_interval_ms =
        normalizedPollInterval(config.poll_interval_seconds) * 1000;
    if (millis() - last_poll_ms < poll_interval_ms) {
      return;
    }
    last_poll_ms = millis();

    bool current_power = last_known_power;
    if (!tuyaReadPower(current_power)) {
      Serial.println("Periodic Tuya status poll failed.");
      return;
    }

    if (current_power != power->getVal<bool>()) {
      Serial.print("External switch state changed to ");
      Serial.println(current_power ? "ON" : "OFF");
      power->setVal(current_power);
    }
    last_known_power = current_power;
  }
};

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("ESP32 HomeSpan Tuya Outlet");

  pinMode(RESET_CONFIG_PIN, INPUT_PULLUP);
  ledcAttach(STATUS_LED_PIN, STATUS_LED_PWM_HZ, STATUS_LED_PWM_BITS);
  setStatusLed(false);
  if (handleBootButtonHold()) {
    return;
  }

  if (!loadConfig()) {
    startSetupMode("no saved configuration");
    return;
  }

  if (!parseLocalKey()) {
    startSetupMode("saved local key is invalid");
    return;
  }

  if (!connectConfiguredWiFiWithRetries()) {
    startSetupMode("Wi-Fi connection failed repeatedly", true);
    return;
  }
  blinkWiFiConnectedLed();
  startMdnsResponder();

  homeSpan.setWifiCredentials(config.wifi_ssid.c_str(),
                              config.wifi_password.c_str());
  if (!config.homekit_pairing_code.isEmpty()) {
    Serial.print("Setting HomeKit pairing code: ");
    Serial.println(formattedHomeKitCode(config.homekit_pairing_code));
    homeSpan.setPairingCode(config.homekit_pairing_code.c_str(), false);
    clearPendingHomeKitPairingCode();
  } else {
    Serial.println("HomeKit pairing code: use saved HomeSpan code, or default 466-37-726 if never changed.");
  }
  homeSpan.setLogLevel(1);
  homeSpan.setPairCallback(handleHomeKitPairingChange);
  homeSpan.setControllerCallback(refreshHomeKitPairingState);
  homeSpan.begin(homeKitCategory(), config.homekit_accessory_name.c_str());
  refreshHomeKitPairingState();
  addDiagnosticLog("HomeSpan started");

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name(config.homekit_accessory_name.c_str());
  new Characteristic::Manufacturer(config.homekit_manufacturer.c_str());
  new Characteristic::Model(config.homekit_model.c_str());
  new Characteristic::SerialNumber(config.tuya_device_id.c_str());
  new Characteristic::FirmwareRevision(FIRMWARE_VERSION);
  if (config.homekit_service_type == "light") {
    new HomeKitTuyaLight();
  } else if (config.homekit_service_type == "switch") {
    new HomeKitTuyaSwitch();
  } else {
    new HomeKitTuyaOutlet();
  }
  homekit_started = true;
  startAdminServer();
}

void loop() {
  handleResetButton();
  if (setup_mode) {
    active_server = &setup_server;
    if (captive_dns_started) {
      dns_server.processNextRequest();
    }
    setup_server.handleClient();
    updateSetupLed();
    maybeRetrySavedWiFiFromSetup();
    return;
  }
  if (!homekit_started) {
    delay(100);
    return;
  }
  static bool time_sync_started = false;
  if (!time_sync_started && WiFi.status() == WL_CONNECTED) {
    time_sync_started = true;
    syncTime();
  }
  active_server = &admin_server;
  admin_server.handleClient();
  homeSpan.poll();
  refreshHomeKitPairingState();
  updateNormalStatusLed();
}
