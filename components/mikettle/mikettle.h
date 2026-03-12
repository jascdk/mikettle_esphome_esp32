#pragma once

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#ifdef USE_ESP32

#include <array>
#include <vector>

namespace esphome {
namespace mikettle {

namespace espbt = esphome::esp32_ble_tracker;

// ── Service UUIDs ─────────────────────────────────────────────────────────────
// Auth / Xiaomi proprietary service (16-bit shorthand: 0xFE95)
static const espbt::ESPBTUUID MI_SERVICE_AUTH_UUID = espbt::ESPBTUUID::from_uint16(0xFE95);

// Data service (128-bit)  "01344736-0000-1000-8000-262837236156"
// Stored in ESP32 little-endian byte order (reversed from the string form)
static const uint8_t DATA_SVC_UUID_BYTES[16] = {
    0x56, 0x61, 0x23, 0x37, 0x28, 0x26, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x36, 0x47, 0x34, 0x01};
static const espbt::ESPBTUUID MI_SERVICE_DATA_UUID =
    espbt::ESPBTUUID::from_raw(DATA_SVC_UUID_BYTES);

// ── Auth service characteristic UUIDs (16-bit) ───────────────────────────────
static const espbt::ESPBTUUID MI_CHAR_AUTH_INIT_UUID = espbt::ESPBTUUID::from_uint16(0x0010);
static const espbt::ESPBTUUID MI_CHAR_AUTH_UUID      = espbt::ESPBTUUID::from_uint16(0x0001);
static const espbt::ESPBTUUID MI_CHAR_VER_UUID       = espbt::ESPBTUUID::from_uint16(0x0004);

// ── Data service characteristic UUIDs (128-bit) ──────────────────────────────
// "0000aa02-0000-1000-8000-262837236156" – Status (notifications)
static const uint8_t STATUS_CHAR_UUID_BYTES[16] = {
    0x56, 0x61, 0x23, 0x37, 0x28, 0x26, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x02, 0xAA, 0x00, 0x00};
static const espbt::ESPBTUUID MI_CHAR_STATUS_UUID =
    espbt::ESPBTUUID::from_raw(STATUS_CHAR_UUID_BYTES);

// "0000aa01-0000-1000-8000-262837236156" – Keep-warm setup (R/W)
static const uint8_t SETUP_CHAR_UUID_BYTES[16] = {
    0x56, 0x61, 0x23, 0x37, 0x28, 0x26, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x01, 0xAA, 0x00, 0x00};
static const espbt::ESPBTUUID MI_CHAR_SETUP_UUID =
    espbt::ESPBTUUID::from_raw(SETUP_CHAR_UUID_BYTES);

// "0000aa04-0000-1000-8000-262837236156" – Keep-warm time limit (R/W)
static const uint8_t TIME_CHAR_UUID_BYTES[16] = {
    0x56, 0x61, 0x23, 0x37, 0x28, 0x26, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x04, 0xAA, 0x00, 0x00};
static const espbt::ESPBTUUID MI_CHAR_TIME_UUID =
    espbt::ESPBTUUID::from_raw(TIME_CHAR_UUID_BYTES);

// "0000aa05-0000-1000-8000-262837236156" – Boil-mode flag (R/W)
static const uint8_t BOIL_CHAR_UUID_BYTES[16] = {
    0x56, 0x61, 0x23, 0x37, 0x28, 0x26, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x05, 0xAA, 0x00, 0x00};
static const espbt::ESPBTUUID MI_CHAR_BOIL_UUID =
    espbt::ESPBTUUID::from_raw(BOIL_CHAR_UUID_BYTES);

// Client Characteristic Configuration Descriptor (0x2902)
static const espbt::ESPBTUUID CCCD_UUID = espbt::ESPBTUUID::from_uint16(0x2902);

// ── Authentication constants ──────────────────────────────────────────────────
static const uint8_t MI_KEY1[4]  = {0x90, 0xCA, 0x85, 0xDE};
static const uint8_t MI_KEY2[4]  = {0x92, 0xAB, 0x54, 0xFA};

static constexpr size_t MI_TOKEN_LEN = 12;

// ── Authentication state machine ─────────────────────────────────────────────
enum class MiKettleState : uint8_t {
  IDLE = 0,
  WRITING_AUTH_INIT,    // writing KEY1 to auth-init characteristic
  SUBSCRIBING_AUTH,     // writing CCCD 0x01,0x00 to auth characteristic
  WRITING_AUTH_TOKEN,   // writing cipher(mixA, token) to auth characteristic
  WAITING_AUTH_NOTIFY,  // waiting for auth notification
  WRITING_AUTH_KEY2,    // writing cipher(token, KEY2) to auth characteristic
  READING_VERSION,      // reading version characteristic to complete auth
  SUBSCRIBING_STATUS,   // writing CCCD to status characteristic
  READY,                // authenticated and receiving status notifications
};

// ── Component ─────────────────────────────────────────────────────────────────
class MiKettleComponent : public ble_client::BLEClientNode, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Called for every ESP-IDF GATT client event forwarded by ble_client
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  // ── Configuration setters (called from generated code) ──────────────────
  void set_product_id(uint16_t product_id) { product_id_ = product_id; }
  void set_token(std::array<uint8_t, MI_TOKEN_LEN> token) {
    memcpy(token_, token.data(), MI_TOKEN_LEN);
  }

  // ── Sensor / text-sensor registration ──────────────────────────────────
  void set_current_temperature_sensor(sensor::Sensor *s) { current_temperature_ = s; }
  void set_set_temperature_sensor(sensor::Sensor *s)     { set_temperature_ = s; }
  void set_action_sensor(text_sensor::TextSensor *s)     { action_sensor_ = s; }
  void set_mode_sensor(text_sensor::TextSensor *s)       { mode_sensor_ = s; }
  void set_keep_warm_type_sensor(text_sensor::TextSensor *s) { keep_warm_type_sensor_ = s; }
  void set_keep_warm_time_sensor(sensor::Sensor *s)      { keep_warm_time_sensor_ = s; }

 protected:
  // ── RC4-derived cipher used for authentication ──────────────────────────
  static std::vector<uint8_t> cipher_init(const uint8_t *key, size_t key_len);
  static std::vector<uint8_t> cipher_crypt(const uint8_t *input, size_t input_len,
                                            std::vector<uint8_t> perm);
  static std::vector<uint8_t> cipher(const uint8_t *key, size_t key_len,
                                      const uint8_t *input, size_t input_len);

  // Key-mixing functions (depend on device MAC and product-ID)
  static std::vector<uint8_t> mix_a(const uint8_t *mac, uint16_t product_id);
  static std::vector<uint8_t> mix_b(const uint8_t *mac, uint16_t product_id);

  // Helper: find CCCD handle for a characteristic
  uint16_t get_cccd_handle_(espbt::ESPBTUUID service_uuid, espbt::ESPBTUUID char_uuid);

  // Resolve status_handle_ and status_cccd_handle_; tries ESPHome abstraction first,
  // then falls back to querying the ESP-IDF GATT cache directly by characteristic UUID.
  void resolve_status_handles_();

  // Parse the 8+ byte status notification payload
  void parse_status_(const uint8_t *data, uint16_t len);

  // Write CCCD to enable notifications on the given descriptor handle
  bool write_cccd_(uint16_t cccd_handle);

  // ── Runtime state ────────────────────────────────────────────────────────
  MiKettleState state_{MiKettleState::IDLE};
  uint16_t product_id_{275};

  // Per-device authentication token (12 bytes, obtained from Mi Home)
  uint8_t token_[MI_TOKEN_LEN]{
      0x01, 0x5C, 0xCB, 0xA8, 0x80, 0x0A, 0xBD, 0xC1, 0x2E, 0xB8, 0xED, 0x82};

  // Remote Bluetooth device address (populated on connect, needed for register_for_notify)
  uint8_t remote_bda_[6]{};

  // Reversed MAC address (computed once on connect, used for auth cipher mixing)
  uint8_t reversed_mac_[6]{};

  // GATT attribute handles (populated after service discovery)
  uint16_t auth_init_handle_{0};
  uint16_t auth_handle_{0};
  uint16_t auth_cccd_handle_{0};
  uint16_t ver_handle_{0};
  uint16_t status_handle_{0};
  uint16_t status_cccd_handle_{0};

  // ── Sensors ─────────────────────────────────────────────────────────────
  sensor::Sensor          *current_temperature_{nullptr};
  sensor::Sensor          *set_temperature_{nullptr};
  text_sensor::TextSensor *action_sensor_{nullptr};
  text_sensor::TextSensor *mode_sensor_{nullptr};
  text_sensor::TextSensor *keep_warm_type_sensor_{nullptr};
  sensor::Sensor          *keep_warm_time_sensor_{nullptr};
};

}  // namespace mikettle
}  // namespace esphome

#endif  // USE_ESP32
