#include "mikettle.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <esp_gattc_api.h>
#include <algorithm>

namespace esphome {
namespace mikettle {

static const char *const TAG = "mikettle";

// ── String constants for state values ────────────────────────────────────────
static const char *const ACTION_IDLE         = "idle";
static const char *const ACTION_HEATING      = "heating";
static const char *const ACTION_COOLING      = "cooling";
static const char *const ACTION_KEEPING_WARM = "keeping warm";

static const char *const MODE_NONE      = "none";
static const char *const MODE_BOIL      = "boil";
static const char *const MODE_KEEP_WARM = "keep warm";

static const char *const KW_TYPE_BOIL_COOL = "boil and cool down";
static const char *const KW_TYPE_HEAT_UP   = "heat up";

// ── Component lifecycle ───────────────────────────────────────────────────────

void MiKettleComponent::setup() {
  // Nothing to initialise at boot; auth starts on BLE connect
}

void MiKettleComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Mi Kettle");
  ESP_LOGCONFIG(TAG, "  Product ID : %u", product_id_);
  ESP_LOGCONFIG(TAG, "  Token      : %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                token_[0], token_[1], token_[2], token_[3],
                token_[4], token_[5], token_[6], token_[7],
                token_[8], token_[9], token_[10], token_[11]);
  LOG_SENSOR("  ", "Current Temperature", current_temperature_);
  LOG_SENSOR("  ", "Set Temperature",     set_temperature_);
  LOG_TEXT_SENSOR("  ", "Action",         action_sensor_);
  LOG_TEXT_SENSOR("  ", "Mode",           mode_sensor_);
  LOG_TEXT_SENSOR("  ", "Keep Warm Type", keep_warm_type_sensor_);
  LOG_SENSOR("  ", "Keep Warm Time",      keep_warm_time_sensor_);
}

// ── GATT event handler ────────────────────────────────────────────────────────

void MiKettleComponent::gattc_event_handler(esp_gattc_cb_event_t event,
                                              esp_gatt_if_t gattc_if,
                                              esp_ble_gattc_cb_param_t *param) {
  switch (event) {

    // ── Connection opened ─────────────────────────────────────────────────
    case ESP_GATTC_OPEN_EVT: {
      if (param->open.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Open failed, status=%d", param->open.status);
        state_ = MiKettleState::IDLE;
        break;
      }
      ESP_LOGD(TAG, "Connected");
      state_ = MiKettleState::IDLE;
      // Compute reversed MAC for cipher mixing from the open event BDA
      const uint8_t *bda = param->open.remote_bda;
      for (int i = 0; i < 6; i++) {
        reversed_mac_[i] = bda[5 - i];
      }
      ESP_LOGD(TAG, "Reversed MAC: %02X:%02X:%02X:%02X:%02X:%02X",
               reversed_mac_[0], reversed_mac_[1], reversed_mac_[2],
               reversed_mac_[3], reversed_mac_[4], reversed_mac_[5]);
      break;
    }

    // ── Service discovery complete ────────────────────────────────────────
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      ESP_LOGD(TAG, "Service discovery complete – resolving handles");

      // ── Auth-init characteristic ──
      auto *auth_init_chr = this->parent_->get_characteristic(
          MI_SERVICE_AUTH_UUID, MI_CHAR_AUTH_INIT_UUID);
      if (auth_init_chr == nullptr) {
        ESP_LOGE(TAG, "Auth-init characteristic not found");
        break;
      }
      auth_init_handle_ = auth_init_chr->handle;

      // ── Auth characteristic + its CCCD ──
      auto *auth_chr = this->parent_->get_characteristic(
          MI_SERVICE_AUTH_UUID, MI_CHAR_AUTH_UUID);
      if (auth_chr == nullptr) {
        ESP_LOGE(TAG, "Auth characteristic not found");
        break;
      }
      auth_handle_      = auth_chr->handle;
      auth_cccd_handle_ = get_cccd_handle_(MI_SERVICE_AUTH_UUID, MI_CHAR_AUTH_UUID);

      // ── Version characteristic ──
      auto *ver_chr = this->parent_->get_characteristic(
          MI_SERVICE_AUTH_UUID, MI_CHAR_VER_UUID);
      if (ver_chr == nullptr) {
        ESP_LOGE(TAG, "Version characteristic not found");
        break;
      }
      ver_handle_ = ver_chr->handle;

      // ── Status characteristic + its CCCD ──
      // Best-effort: resolve now if possible; retried after auth completes.
      // Authentication must not be blocked by a missing status handle here,
      // because some devices only expose the data service after auth begins.
      resolve_status_handles_();

      // ── Step 1: write KEY1 to auth-init characteristic ──
      ESP_LOGD(TAG, "Auth step 1 – writing KEY1");
      state_ = MiKettleState::WRITING_AUTH_INIT;
      auto err = esp_ble_gattc_write_char(
          this->parent_->get_gattc_if(),
          this->parent_->get_conn_id(),
          auth_init_handle_,
          sizeof(MI_KEY1),
          const_cast<uint8_t *>(MI_KEY1),
          ESP_GATT_WRITE_TYPE_RSP,
          ESP_GATT_AUTH_REQ_NONE);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Write KEY1 failed: %d", err);
        state_ = MiKettleState::IDLE;
      }
      break;
    }

    // ── Characteristic write response ─────────────────────────────────────
    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Write char failed: handle=%d status=%d",
                 param->write.handle, param->write.status);
        break;
      }

      if (state_ == MiKettleState::WRITING_AUTH_INIT &&
          param->write.handle == auth_init_handle_) {
        // ── Step 2: subscribe to auth characteristic notifications ──
        ESP_LOGD(TAG, "Auth step 2 – enabling auth notifications");
        state_ = MiKettleState::SUBSCRIBING_AUTH;
        if (!write_cccd_(auth_cccd_handle_)) {
          state_ = MiKettleState::IDLE;
        }

      } else if (state_ == MiKettleState::WRITING_AUTH_TOKEN &&
                 param->write.handle == auth_handle_) {
        // Token sent; now wait silently for the auth notification
        ESP_LOGD(TAG, "Auth step 3 done – waiting for auth notification");
        state_ = MiKettleState::WAITING_AUTH_NOTIFY;

      } else if (state_ == MiKettleState::WRITING_AUTH_KEY2 &&
                 param->write.handle == auth_handle_) {
        // ── Step 6: read version to complete handshake ──
        ESP_LOGD(TAG, "Auth step 6 – reading version");
        state_ = MiKettleState::READING_VERSION;
        auto err = esp_ble_gattc_read_char(
            this->parent_->get_gattc_if(),
            this->parent_->get_conn_id(),
            ver_handle_,
            ESP_GATT_AUTH_REQ_NONE);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "Read version failed: %d", err);
          state_ = MiKettleState::IDLE;
        }
      }
      break;
    }

    // ── Descriptor write response (e.g. CCCD written) ────────────────────
    case ESP_GATTC_WRITE_DESCR_EVT: {
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Write descr failed: handle=%d status=%d",
                 param->write.handle, param->write.status);
        break;
      }

      if (state_ == MiKettleState::SUBSCRIBING_AUTH &&
          param->write.handle == auth_cccd_handle_) {
        // ── Step 3: write cipher(mixA(reversedMac, productID), token) ──
        ESP_LOGD(TAG, "Auth step 3 – writing encrypted token");
        state_ = MiKettleState::WRITING_AUTH_TOKEN;

        auto mix_a_result = mix_a(reversed_mac_, product_id_);
        auto auth_payload = cipher(
            mix_a_result.data(), mix_a_result.size(),
            token_, sizeof(token_));

        auto err = esp_ble_gattc_write_char(
            this->parent_->get_gattc_if(),
            this->parent_->get_conn_id(),
            auth_handle_,
            static_cast<uint16_t>(auth_payload.size()),
            auth_payload.data(),
            ESP_GATT_WRITE_TYPE_RSP,
            ESP_GATT_AUTH_REQ_NONE);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "Write auth token failed: %d", err);
          state_ = MiKettleState::IDLE;
        }

      } else if (state_ == MiKettleState::SUBSCRIBING_STATUS &&
                 param->write.handle == status_cccd_handle_) {
        ESP_LOGI(TAG, "Authentication complete – receiving status updates");
        state_ = MiKettleState::READY;
      }
      break;
    }

    // ── Notification / indication received ───────────────────────────────
    case ESP_GATTC_NOTIFY_EVT: {
      if (state_ == MiKettleState::WAITING_AUTH_NOTIFY &&
          param->notify.handle == auth_handle_) {
        ESP_LOGD(TAG, "Auth step 4 – notification received, verifying");

        // Optional integrity check:
        // cipher(mixB(mac,pid), cipher(mixA(mac,pid), data)) must equal token
        auto mix_a_result = mix_a(reversed_mac_, product_id_);
        auto mix_b_result = mix_b(reversed_mac_, product_id_);
        auto inner = cipher(
            mix_a_result.data(), mix_a_result.size(),
            param->notify.value, param->notify.value_len);
        auto outer = cipher(
            mix_b_result.data(), mix_b_result.size(),
            inner.data(), inner.size());
        if (outer.size() != sizeof(token_) ||
            memcmp(outer.data(), token_, sizeof(token_)) != 0) {
          ESP_LOGW(TAG, "Auth integrity check failed – continuing anyway");
        }

        // ── Step 5: write cipher(token, KEY2) ──
        ESP_LOGD(TAG, "Auth step 5 – writing KEY2");
        state_ = MiKettleState::WRITING_AUTH_KEY2;
        auto key2_payload = cipher(
            token_, sizeof(token_),
            MI_KEY2, sizeof(MI_KEY2));

        auto err = esp_ble_gattc_write_char(
            this->parent_->get_gattc_if(),
            this->parent_->get_conn_id(),
            auth_handle_,
            static_cast<uint16_t>(key2_payload.size()),
            key2_payload.data(),
            ESP_GATT_WRITE_TYPE_RSP,
            ESP_GATT_AUTH_REQ_NONE);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "Write KEY2 failed: %d", err);
          state_ = MiKettleState::IDLE;
        }

      } else if (state_ == MiKettleState::READY &&
                 param->notify.handle == status_handle_) {
        parse_status_(param->notify.value, param->notify.value_len);
      }
      break;
    }

    // ── Characteristic read response ──────────────────────────────────────
    case ESP_GATTC_READ_CHAR_EVT: {
      if (param->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Read char failed: handle=%d status=%d",
                 param->read.handle, param->read.status);
        break;
      }

      if (state_ == MiKettleState::READING_VERSION &&
          param->read.handle == ver_handle_) {
        // ── Step 7: subscribe to status notifications ──
        // If the status handle was not resolved during service discovery,
        // retry now that authentication has completed.
        if (status_handle_ == 0)
          resolve_status_handles_();

        if (status_handle_ == 0) {
          ESP_LOGE(TAG, "Status characteristic not found – cannot receive updates");
          state_ = MiKettleState::IDLE;
          break;
        }

        ESP_LOGD(TAG, "Auth step 7 – subscribing to status notifications");
        state_ = MiKettleState::SUBSCRIBING_STATUS;
        if (!write_cccd_(status_cccd_handle_)) {
          state_ = MiKettleState::IDLE;
        }
      }
      break;
    }

    // ── Disconnection ─────────────────────────────────────────────────────
    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGW(TAG, "Disconnected (reason=%d) – resetting state",
               param->disconnect.reason);
      state_              = MiKettleState::IDLE;
      auth_init_handle_   = 0;
      auth_handle_        = 0;
      auth_cccd_handle_   = 0;
      ver_handle_         = 0;
      status_handle_      = 0;
      status_cccd_handle_ = 0;
      break;
    }

    default:
      break;
  }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

uint16_t MiKettleComponent::get_cccd_handle_(espbt::ESPBTUUID service_uuid,
                                              espbt::ESPBTUUID char_uuid) {
  auto *chr = this->parent_->get_characteristic(service_uuid, char_uuid);
  if (chr == nullptr)
    return 0;
  for (auto *descr : chr->descriptors) {
    if (descr->uuid == CCCD_UUID)
      return descr->handle;
  }
  // Fallback: query the ESP-IDF GATT descriptor cache directly.
  // ESPHome may not populate chr->descriptors in all firmware versions.
  esp_bt_uuid_t cccd_uuid = CCCD_UUID.get_uuid();
  esp_gattc_descr_elem_t descr_elem;
  uint16_t descr_count = 1;  // capacity in; actual count out
  if (esp_ble_gattc_get_descr_by_char_handle(
          this->parent_->get_gattc_if(),
          this->parent_->get_conn_id(),
          chr->handle,
          cccd_uuid, &descr_elem, &descr_count) == ESP_GATT_OK
      && descr_count > 0) {
    return descr_elem.handle;
  }
  return 0;
}

void MiKettleComponent::resolve_status_handles_() {
  // ── Primary path: ESPHome service abstraction ──────────────────────────────
  auto *status_chr = this->parent_->get_characteristic(
      MI_SERVICE_DATA_UUID, MI_CHAR_STATUS_UUID);
  if (status_chr != nullptr) {
    status_handle_      = status_chr->handle;
    status_cccd_handle_ = get_cccd_handle_(MI_SERVICE_DATA_UUID, MI_CHAR_STATUS_UUID);
    ESP_LOGD(TAG, "Status characteristic resolved via service UUID (handle 0x%04X)", status_handle_);
    return;
  }

  // ── Fallback: query the ESP-IDF GATT cache directly ────────────────────────
  // This succeeds even when ESPHome's service-level UUID matching fails
  // (e.g. the data service UUID is not found in services_ despite being
  // present in the underlying ESP-IDF GATT attribute cache).
  ESP_LOGD(TAG, "Service-level lookup failed; falling back to direct GATT cache search");
  esp_bt_uuid_t stat_uuid = MI_CHAR_STATUS_UUID.get_uuid();
  esp_gattc_char_elem_t char_elem;
  uint16_t char_count = 1;
  esp_gatt_status_t gatt_err = esp_ble_gattc_get_char_by_uuid(
      this->parent_->get_gattc_if(),
      this->parent_->get_conn_id(),
      0x0001, 0xFFFF,
      stat_uuid, &char_elem, &char_count);
  if (gatt_err != ESP_GATT_OK || char_count == 0) {
    ESP_LOGD(TAG, "Status characteristic not found in GATT cache (err=%d)", gatt_err);
    return;  // handles remain 0; caller decides whether to fail
  }
  status_handle_ = char_elem.char_handle;
  ESP_LOGD(TAG, "Status characteristic found via direct GATT lookup (handle 0x%04X)", status_handle_);

  // Locate the CCCD descriptor for this characteristic
  esp_bt_uuid_t cccd_uuid = CCCD_UUID.get_uuid();
  esp_gattc_descr_elem_t descr_elem;
  uint16_t descr_count = 1;
  if (esp_ble_gattc_get_descr_by_char_handle(
          this->parent_->get_gattc_if(),
          this->parent_->get_conn_id(),
          char_elem.char_handle,
          cccd_uuid, &descr_elem, &descr_count) == ESP_GATT_OK
      && descr_count > 0) {
    status_cccd_handle_ = descr_elem.handle;
    ESP_LOGD(TAG, "Status CCCD resolved (handle 0x%04X)", status_cccd_handle_);
  } else {
    ESP_LOGW(TAG, "Status CCCD descriptor not found");
  }
}

bool MiKettleComponent::write_cccd_(uint16_t cccd_handle) {
  if (cccd_handle == 0) {
    ESP_LOGE(TAG, "CCCD handle is 0 – cannot subscribe");
    return false;
  }
  static const uint8_t ENABLE_NOTIFY[2] = {0x01, 0x00};
  auto err = esp_ble_gattc_write_char_descr(
      this->parent_->get_gattc_if(),
      this->parent_->get_conn_id(),
      cccd_handle,
      sizeof(ENABLE_NOTIFY),
      const_cast<uint8_t *>(ENABLE_NOTIFY),
      ESP_GATT_WRITE_TYPE_RSP,
      ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Write CCCD failed: %d", err);
    return false;
  }
  return true;
}

void MiKettleComponent::parse_status_(const uint8_t *data, uint16_t len) {
  if (len < 8) {
    ESP_LOGW(TAG, "Status payload too short (%d bytes)", len);
    return;
  }

  ESP_LOGD(TAG, "Status: %02X %02X %02X %02X %02X %02X %02X %02X",
           data[0], data[1], data[2], data[3],
           data[4], data[5], data[6], data[7]);

  // Byte 0 – Action
  if (action_sensor_ != nullptr) {
    const char *action;
    switch (data[0]) {
      case 0:  action = ACTION_IDLE;         break;
      case 1:  action = ACTION_HEATING;      break;
      case 2:  action = ACTION_COOLING;      break;
      case 3:  action = ACTION_KEEPING_WARM; break;
      default: action = "unknown";           break;
    }
    action_sensor_->publish_state(action);
  }

  // Byte 1 – Mode
  if (mode_sensor_ != nullptr) {
    const char *mode;
    switch (data[1]) {
      case 0xFF: mode = MODE_NONE;      break;
      case 1:    mode = MODE_BOIL;      break;
      case 2:    // fall-through (aprosvetova uses 2; drndos uses 3)
      case 3:    mode = MODE_KEEP_WARM; break;
      default:   mode = "unknown";      break;
    }
    mode_sensor_->publish_state(mode);
  }

  // Byte 4 – Keep-warm set temperature (°C)
  if (set_temperature_ != nullptr)
    set_temperature_->publish_state(static_cast<float>(data[4]));

  // Byte 5 – Current temperature (°C)
  if (current_temperature_ != nullptr)
    current_temperature_->publish_state(static_cast<float>(data[5]));

  // Byte 6 – Keep-warm type
  if (keep_warm_type_sensor_ != nullptr) {
    const char *kw_type;
    switch (data[6]) {
      case 0:  kw_type = KW_TYPE_BOIL_COOL; break;
      case 1:  kw_type = KW_TYPE_HEAT_UP;   break;
      default: kw_type = "unknown";          break;
    }
    keep_warm_type_sensor_->publish_state(kw_type);
  }

  // Bytes 7-8 – Keep-warm elapsed time (minutes, little-endian)
  if (keep_warm_time_sensor_ != nullptr && len >= 9) {
    uint16_t kw_min = static_cast<uint16_t>(data[7]) |
                      (static_cast<uint16_t>(data[8]) << 8);
    keep_warm_time_sensor_->publish_state(static_cast<float>(kw_min));
  }
}

// ── RC4-derived cipher ────────────────────────────────────────────────────────

std::vector<uint8_t> MiKettleComponent::cipher_init(const uint8_t *key,
                                                      size_t key_len) {
  std::vector<uint8_t> perm(256);
  for (int i = 0; i < 256; i++)
    perm[i] = static_cast<uint8_t>(i);

  int j = 0;
  for (int i = 0; i < 256; i++) {
    j = (j + perm[i] + key[i % key_len]) & 0xFF;
    std::swap(perm[i], perm[j]);
  }
  return perm;
}

std::vector<uint8_t> MiKettleComponent::cipher_crypt(const uint8_t *input,
                                                       size_t input_len,
                                                       std::vector<uint8_t> perm) {
  std::vector<uint8_t> output;
  output.reserve(input_len);
  int idx1 = 0, idx2 = 0;
  for (size_t i = 0; i < input_len; i++) {
    idx1 = (idx1 + 1) & 0xFF;
    idx2 = (idx2 + perm[idx1]) & 0xFF;
    std::swap(perm[idx1], perm[idx2]);
    int k = (perm[idx1] + perm[idx2]) & 0xFF;
    output.push_back(input[i] ^ perm[k]);
  }
  return output;
}

std::vector<uint8_t> MiKettleComponent::cipher(const uint8_t *key,   size_t key_len,
                                                 const uint8_t *input, size_t input_len) {
  auto perm = cipher_init(key, key_len);
  return cipher_crypt(input, input_len, std::move(perm));
}

// ── Key-mixing functions ──────────────────────────────────────────────────────

std::vector<uint8_t> MiKettleComponent::mix_a(const uint8_t *mac,
                                               uint16_t product_id) {
  return {
      mac[0], mac[2], mac[5],
      static_cast<uint8_t>(product_id & 0xFF),
      static_cast<uint8_t>(product_id & 0xFF),
      mac[4], mac[5], mac[1]};
}

std::vector<uint8_t> MiKettleComponent::mix_b(const uint8_t *mac,
                                               uint16_t product_id) {
  return {
      mac[0], mac[2], mac[5],
      static_cast<uint8_t>((product_id >> 8) & 0xFF),
      mac[4], mac[0], mac[5],
      static_cast<uint8_t>(product_id & 0xFF)};
}

}  // namespace mikettle
}  // namespace esphome

#endif  // USE_ESP32
