#include <Arduino.h>
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
constexpr uint16_t ADMIN_PORT = 8080;
constexpr char SETUP_AP_SSID[] = "TuyaHomeKit-Setup";
constexpr char SETUP_AP_PASSWORD_PREFIX[] = "THK";
constexpr char PREF_NAMESPACE[] = "tuya-hk";
constexpr uint8_t RESET_CONFIG_PIN = 0;
constexpr uint8_t STATUS_LED_PIN = 2;
constexpr uint8_t STATUS_LED_ON = HIGH;
constexpr uint8_t STATUS_LED_OFF = LOW;
constexpr uint8_t WIFI_CONNECT_ATTEMPTS = 3;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t SETUP_WIFI_RETRY_INTERVAL_MS = 60000;
constexpr uint32_t FACTORY_RESET_HOLD_MS = 8000;
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
  uint32_t poll_interval_seconds = DEFAULT_POLL_INTERVAL_SECONDS;
};

struct TuyaResponse {
  uint32_t seq = 0;
  uint32_t cmd = 0;
  uint32_t retcode = 0;
  std::vector<uint8_t> payload;
  bool hmac_ok = false;
};

BridgeConfig config;
Preferences preferences;
WebServer setup_server(80);
WebServer admin_server(ADMIN_PORT);
WebServer* active_server = &setup_server;
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
bool setup_led_state = false;
unsigned long last_setup_wifi_retry_ms = 0;
unsigned long reset_button_pressed_ms = 0;
unsigned long last_setup_led_toggle_ms = 0;

String setup_ap_password;

void setStatusLed(bool on) {
  digitalWrite(STATUS_LED_PIN, on ? STATUS_LED_ON : STATUS_LED_OFF);
}

void updateSetupLed() {
  if (millis() - last_setup_led_toggle_ms < SETUP_LED_BLINK_MS) {
    return;
  }
  last_setup_led_toggle_ms = millis();
  setup_led_state = !setup_led_state;
  setStatusLed(setup_led_state);
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

String setupPasswordFromRandom() {
  char password[16] = {0};
  snprintf(password, sizeof(password), "%s%08lX%02lX",
           SETUP_AP_PASSWORD_PREFIX, uint32_t(esp_random()),
           uint32_t(esp_random() & 0xFF));
  return String(password);
}

bool validateConfig(const BridgeConfig& candidate, String& error) {
  IPAddress ip;
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

bool connectTuya() {
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
  Serial.println(TUYA_PORT);
  if (!client.connect(tuya_ip, TUYA_PORT, 5000)) {
    Serial.println("TCP connection failed.");
    return false;
  }
  client.setTimeout(5000);
  return true;
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

bool tuyaStatus() {
  if (!ensureSession()) {
    return false;
  }
  const char payload[] = "{}";
  TuyaResponse response;
  if (!sendEncrypted(CMD_DP_QUERY_NEW, reinterpret_cast<const uint8_t*>(payload),
                     strlen(payload), session_key, false, &response)) {
    Serial.println("Status command failed.");
    resetTuyaSession();
    return false;
  }
  std::vector<uint8_t> clear;
  if (!decryptPayload(session_key, response, clear)) {
    Serial.println("Could not decrypt status response.");
    resetTuyaSession();
    return false;
  }
  printClearPayload(clear);
  resetTuyaSession();
  return true;
}

bool tuyaReadPower(bool& is_on) {
  if (!ensureSession()) {
    return false;
  }
  const char payload[] = "{}";
  TuyaResponse response;
  if (!sendEncrypted(CMD_DP_QUERY_NEW, reinterpret_cast<const uint8_t*>(payload),
                     strlen(payload), session_key, false, &response)) {
    Serial.println("Status command failed.");
    resetTuyaSession();
    return false;
  }

  std::vector<uint8_t> clear;
  if (!decryptPayload(session_key, response, clear)) {
    Serial.println("Could not decrypt status response.");
    resetTuyaSession();
    return false;
  }

  String text = payloadJsonText(clear);
  String true_pattern = String("\"") + config.tuya_relay_dps + "\":true";
  String false_pattern = String("\"") + config.tuya_relay_dps + "\":false";
  text.replace(" ", "");
  if (text.indexOf(true_pattern) >= 0) {
    is_on = true;
    resetTuyaSession();
    return true;
  }
  if (text.indexOf(false_pattern) >= 0) {
    is_on = false;
    resetTuyaSession();
    return true;
  }

  Serial.println("Relay DPS was not found in status response.");
  printClearPayload(clear);
  resetTuyaSession();
  return false;
}

bool tuyaSwitch(bool on) {
  if (!ensureSession()) {
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
    return false;
  }

  Serial.print("Sending local Tuya command: ");
  Serial.println(on ? "ON" : "OFF");
  TuyaResponse response;
  if (!sendEncrypted(CMD_CONTROL_NEW, reinterpret_cast<const uint8_t*>(payload),
                     strlen(payload), session_key, true, &response)) {
    Serial.println("Switch command failed.");
    resetTuyaSession();
    return false;
  }

  std::vector<uint8_t> clear;
  if (response.payload.empty()) {
    Serial.println("Switch command acknowledged with empty payload.");
    resetTuyaSession();
    return true;
  }
  if (decryptPayload(session_key, response, clear)) {
    printClearPayload(clear);
  }
  resetTuyaSession();
  return true;
}

String setupPage(const String& message) {
  String page;
  page.reserve(14000);
  page += F("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">");
  page += F("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  page += F("<title>Tuya HomeKit Setup</title><style>");
  page += F(":root{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;color:#14212b;background:#f6f8fa}");
  page += F("body{margin:0;padding:24px}.wrap{max-width:760px;margin:0 auto;background:white;border:1px solid #d8dee4;border-radius:8px;padding:24px}");
  page += F("h1{margin:0 0 8px;font-size:28px}.hint,.help{color:#57606a;margin-top:0}.help{font-size:13px;line-height:1.35}");
  page += F(".grid{display:grid;gap:14px}label{display:grid;gap:6px;font-weight:600}");
  page += F("input,select{font:inherit;padding:10px;border:1px solid #d0d7de;border-radius:6px;background:white}");
  page += F("button{font:inherit;border:0;border-radius:6px;padding:10px 14px;background:#008b8b;color:white;font-weight:700;cursor:pointer}");
  page += F("button.secondary{background:#24292f}button.danger{background:#b42318}.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:18px}");
  page += F(".msg{margin:16px 0;padding:12px;border-radius:6px;background:#ddf4ff;border:1px solid #54aeef}.foot{margin-top:18px;color:#57606a;font-size:14px}");
  page += F(".box{margin-top:18px;padding:12px;border:1px solid #d8dee4;border-radius:6px;background:#f6f8fa}.box h2{font-size:16px;margin:0 0 8px}");
  page += F("</style></head><body><main class=\"wrap\"><h1>Tuya HomeKit Setup</h1>");
  page += F("<p class=\"hint\">Enter the local Tuya plug values. Secrets are stored only in ESP32 flash memory and are not shown back after saving. This local page has no login, so use it only on a trusted network.</p>");
  if (!message.isEmpty()) {
    page += F("<div class=\"msg\">");
    page += htmlEscape(message);
    page += F("</div>");
  }
  page += F("<form id=\"setup\" method=\"post\" action=\"/save\"><div class=\"grid\">");
  page += F("<label>Wi-Fi SSID<input name=\"wifi_ssid\" required value=\"");
  page += htmlEscape(config.wifi_ssid);
  page += F("\"><span class=\"help\">Name of your home, IoT, or guest Wi-Fi network.</span></label>");
  page += F("<label>Wi-Fi password<input name=\"wifi_password\" type=\"password\" value=\"");
  page += F("\" placeholder=\"Leave blank to keep saved password\"><span class=\"help\">Leave blank when editing if the saved Wi-Fi password should stay unchanged.</span></label>");
  page += F("<label>Tuya plug IP address<input name=\"tuya_ip\" required placeholder=\"192.168.1.123\" value=\"");
  page += htmlEscape(config.tuya_ip);
  page += F("\"><span class=\"help\">Local IP address of the Tuya / Smart Life plug on your router.</span></label>");
  page += F("<label>Tuya device ID<input name=\"tuya_device_id\" required value=\"");
  page += htmlEscape(config.tuya_device_id);
  page += F("\"><span class=\"help\">Device ID from Tuya / Smart Life developer data. This is not the IP address.</span></label>");
  page += F("<label>Tuya local key<input name=\"tuya_local_key\" maxlength=\"16\" value=\"");
  page += F("\" placeholder=\"Leave blank to keep saved key\"><span class=\"help\">16-character local key used by the ESP32 to talk directly to the plug. Leave blank when editing to keep the saved key.</span></label>");
  page += F("<label>Tuya protocol version<input name=\"tuya_protocol_version\" required value=\"");
  page += htmlEscape(config.tuya_protocol_version);
  page += F("\"><span class=\"help\">Keep 3.4 for the tested plug. Other versions are not supported by this first wizard.</span></label>");
  page += F("<label>Relay DPS<input name=\"tuya_relay_dps\" required value=\"");
  page += htmlEscape(config.tuya_relay_dps);
  page += F("\"><span class=\"help\">DPS means Tuya datapoint. It is the number Tuya uses for a value inside the plug. For the tested socket, relay on/off is DPS 1.</span></label>");
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
  page += F("<label>Polling interval, seconds<input name=\"poll_interval_seconds\" type=\"number\" min=\"5\" max=\"3600\" value=\"");
  page += String(config.poll_interval_seconds);
  page += F("\"><span class=\"help\">How often the ESP32 checks the plug state in the background. 30 seconds is a safe default.</span></label></div><div class=\"actions\">");
  page += F("<button type=\"submit\">Save and restart</button>");
  page += F("<button class=\"secondary\" type=\"button\" id=\"test\">Test Tuya connection</button>");
  page += F("<button class=\"danger\" type=\"button\" id=\"reset\">Clear saved config</button>");
  if (homekit_started) {
    page += F("<button class=\"danger\" type=\"button\" id=\"unpair\">Clear HomeKit pairing</button>");
  }
  page += F("</div></form><div class=\"box\"><h2>HomeKit pairing</h2><p class=\"help\">Apple Home asks for a HomeKit setup code when adding the accessory. The code is not your Wi-Fi password and it is not the temporary setup AP password. Use the code you entered above, or the HomeSpan default 466-37-726 if you never changed it.</p></div>");
  page += F("<p class=\"foot\" id=\"result\">Setup AP: ");
  page += SETUP_AP_SSID;
  page += F(" · In setup mode open http://192.168.4.1/ · In normal mode open the ESP32 IP with port ");
  page += String(ADMIN_PORT);
  page += F(" printed in Serial Monitor.</p>");
  page += F("<script>const form=document.getElementById('setup');const result=document.getElementById('result');");
  page += F("document.getElementById('test').onclick=async()=>{result.textContent='Testing...';try{const r=await fetch('/test',{method:'POST',body:new FormData(form)});result.textContent=await r.text();}catch(e){result.textContent='Test request failed.'}};");
  page += F("document.getElementById('reset').onclick=async()=>{if(confirm('Clear saved configuration and restart setup mode?')){const r=await fetch('/reset',{method:'POST'});result.textContent=await r.text();}};");
  page += F("const unpair=document.getElementById('unpair');if(unpair)unpair.onclick=async()=>{if(confirm('Clear HomeKit pairing on the ESP32? Also remove this accessory in Apple Home.')){const r=await fetch('/unpair',{method:'POST'});result.textContent=await r.text();}};");
  page += F("</script></main></body></html>");
  return page;
}

bool connectConfiguredWiFi(const BridgeConfig& candidate, bool keep_ap) {
  WiFi.mode(keep_ap ? WIFI_AP_STA : WIFI_STA);
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
    return false;
  }
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
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
  requestServer().send(200, "text/plain", "Configuration cleared. Restarting...");
  delay(700);
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
    Serial.println("Reset button pressed. Hold for 8 seconds for factory reset.");
  }
  if (!pressed) {
    if (reset_button_was_pressed && !factory_reset_started) {
      Serial.println("Reset button released before factory reset.");
    }
    reset_button_was_pressed = false;
    reset_button_pressed_ms = 0;
    return;
  }
  if (!factory_reset_started &&
      millis() - reset_button_pressed_ms >= FACTORY_RESET_HOLD_MS) {
    performFactoryReset("BOOT/GPIO0 held for 8 seconds");
  }
}

void handleTestConfig() {
  const BridgeConfig candidate = configFromRequest();
  String error;
  if (!validateConfig(candidate, error)) {
    requestServer().send(400, "text/plain", error);
    return;
  }

  const BridgeConfig previous = config;
  config = candidate;
  if (!parseLocalKey()) {
    config = previous;
    requestServer().send(400, "text/plain", "Tuya local key is invalid.");
    return;
  }

  const bool already_on_wifi = homekit_started && WiFi.status() == WL_CONNECTED;
  if (!already_on_wifi) {
    if (!connectConfiguredWiFi(candidate, true)) {
      WiFi.disconnect(false);
      config = previous;
      requestServer().send(408, "text/plain",
                        "Wi-Fi connection failed. Check SSID and password.");
      return;
    }
  }
  const bool ok = tuyaStatus();
  resetTuyaSession();
  if (!already_on_wifi) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_AP);
  }
  config = previous;
  requestServer().send(ok ? 200 : 502, "text/plain",
                    ok ? "Tuya connection test succeeded."
                       : "Tuya connection test failed. Check IP, local key, protocol version, and LAN reachability.");
}

void startSetupMode(const String& reason, bool retry_saved_wifi = false) {
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

  setup_server.on("/", HTTP_GET, handleSetupRoot);
  setup_server.on("/save", HTTP_POST, handleSaveConfig);
  setup_server.on("/reset", HTTP_POST, handleResetConfig);
  setup_server.on("/test", HTTP_POST, handleTestConfig);
  setup_server.on("/unpair", HTTP_POST, handleUnpairHomeKit);
  setup_server.onNotFound(handleSetupRoot);
  setup_server.begin();
}

void startAdminServer() {
  admin_server.on("/", HTTP_GET, handleSetupRoot);
  admin_server.on("/save", HTTP_POST, handleSaveConfig);
  admin_server.on("/reset", HTTP_POST, handleResetConfig);
  admin_server.on("/test", HTTP_POST, handleTestConfig);
  admin_server.on("/unpair", HTTP_POST, handleUnpairHomeKit);
  admin_server.onNotFound(handleSetupRoot);
  admin_server.begin();
  Serial.print("Admin URL: http://");
  Serial.print(WiFi.localIP());
  Serial.print(":");
  Serial.println(ADMIN_PORT);
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
  pinMode(STATUS_LED_PIN, OUTPUT);
  setStatusLed(false);
  if (digitalRead(RESET_CONFIG_PIN) == LOW) {
    clearConfig();
    startSetupMode("reset button held during boot");
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
  homeSpan.begin(homeKitCategory(), config.homekit_accessory_name.c_str());

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name(config.homekit_accessory_name.c_str());
  new Characteristic::Manufacturer(config.homekit_manufacturer.c_str());
  new Characteristic::Model(config.homekit_model.c_str());
  new Characteristic::SerialNumber(config.tuya_device_id.c_str());
  new Characteristic::FirmwareRevision("0.2.0");
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
}
