#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <time.h>
#include <vector>

#include "HomeSpan.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "secrets.h"

namespace {

constexpr uint16_t TUYA_PORT = 6668;
constexpr uint32_t PREFIX_55AA = 0x000055AA;
constexpr uint32_t SUFFIX_55AA = 0x0000AA55;
constexpr uint8_t CMD_SESS_KEY_NEG_START = 0x03;
constexpr uint8_t CMD_SESS_KEY_NEG_RESP = 0x04;
constexpr uint8_t CMD_SESS_KEY_NEG_FINISH = 0x05;
constexpr uint8_t CMD_CONTROL_NEW = 0x0D;
constexpr uint8_t CMD_DP_QUERY_NEW = 0x10;
constexpr size_t AES_BLOCK_SIZE = 16;
constexpr size_t HMAC_SIZE = 32;

const uint8_t LOCAL_NONCE[AES_BLOCK_SIZE] = {
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
};

struct TuyaResponse {
  uint32_t seq = 0;
  uint32_t cmd = 0;
  uint32_t retcode = 0;
  std::vector<uint8_t> payload;
  bool hmac_ok = false;
};

WiFiClient client;
uint32_t sequence_number = 1;
uint8_t real_key[AES_BLOCK_SIZE] = {0};
uint8_t session_key[AES_BLOCK_SIZE] = {0};
bool session_ready = false;

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

bool parseLocalKey() {
  const char* key = TUYA_LOCAL_KEY;
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
  client.stop();
  Serial.print("Connecting to Tuya plug ");
  Serial.print(TUYA_DEVICE_IP);
  Serial.print(":");
  Serial.println(TUYA_PORT);
  if (!client.connect(TUYA_DEVICE_IP, TUYA_PORT, 5000)) {
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
    const uint8_t version_header[15] = {
        '3', '.', '4', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
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
  String true_pattern = String("\"") + TUYA_RELAY_DPS + "\":true";
  String false_pattern = String("\"") + TUYA_RELAY_DPS + "\":false";
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
  snprintf(payload, sizeof(payload),
           "{\"protocol\":5,\"t\":%ld,\"data\":{\"dps\":{\"%s\":%s}}}",
           long(now), TUYA_RELAY_DPS, on ? "true" : "false");

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

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("ESP32 IP: ");
  Serial.println(WiFi.localIP());
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
    if (millis() - last_poll_ms < 30000) {
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

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("ESP32 HomeSpan Tuya Outlet");

  if (!parseLocalKey()) {
    Serial.println("Fix secrets.h before continuing.");
    return;
  }

  homeSpan.setWifiCredentials(WIFI_SSID, WIFI_PASSWORD);
  homeSpan.setLogLevel(1);
  homeSpan.begin(Category::Outlets, HOMEKIT_ACCESSORY_NAME);

  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name(HOMEKIT_ACCESSORY_NAME);
  new Characteristic::Manufacturer(HOMEKIT_MANUFACTURER);
  new Characteristic::Model(HOMEKIT_MODEL);
  new Characteristic::SerialNumber(TUYA_DEVICE_ID);
  new Characteristic::FirmwareRevision("0.2.0");
  new HomeKitTuyaOutlet();
}

void loop() {
  static bool time_sync_started = false;
  if (!time_sync_started && WiFi.status() == WL_CONNECTED) {
    time_sync_started = true;
    syncTime();
  }
  homeSpan.poll();
}
