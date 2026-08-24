#include "fruit_store.h"
#include <Arduino.h>
#include <cstring>
#include <cctype>

constexpr const char* NVS_NAMESPACE = "fruit_models";
constexpr const char* ACTIVE_NAME_KEY = "active_name";

FruitStore::FruitStore() : is_model_loaded_in_ram(false) {
    memset(&active_model_ram, 0, sizeof(Fruit28D));
    memset(loaded_fruit_name, 0, sizeof(loaded_fruit_name));
}

void FruitStore::get_nvs_key(const char* fruit_name, char key_out[16]) const {
    // NVS keys MUST be <= 15 chars. Format: "m_<lowercase_name>"
    memset(key_out, 0, 16);
    key_out[0] = 'm';
    key_out[1] = '_';

    size_t idx = 2;
    for (size_t i = 0; fruit_name[i] != '\0' && idx < 15; i++) {
        if (isalnum((unsigned char)fruit_name[i])) {
            key_out[idx++] = tolower((unsigned char)fruit_name[i]);
        }
    }
}

bool FruitStore::init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        Serial.printf("[FruitStore] Error: NVS Flash initialization failed (0x%X)\n", err);
        return false;
    }

    // Restore last user-selected model so a reboot lands ready-to-scan.
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        char name[MAX_MODEL_NAME_LEN] = {0};
        size_t len = sizeof(name);
        if (nvs_get_str(nvs, ACTIVE_NAME_KEY, name, &len) == ESP_OK &&
            strlen(name) > 0) {
            if (!load_model_to_ram(name)) {
                Serial.printf("[FruitStore] Persisted model '%s' missing.\n", name);
            }
        }
        nvs_close(nvs);
    }

    Serial.println("[FruitStore] NVS Multi-Fruit Storage Subsystem Ready.");
    return true;
}

bool FruitStore::save_model(const Fruit28D& fruit) {
    if (fruit.fruit_name[0] == '\0') {
        Serial.println("[FruitStore] Refusing to save unnamed model.");
        return false;
    }

    char key[16];
    get_nvs_key(fruit.fruit_name, key);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        Serial.println("[FruitStore] Error: cannot open NVS namespace.");
        return false;
    }
    esp_err_t err = nvs_set_blob(nvs, key, &fruit, sizeof(Fruit28D));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        Serial.printf("[FruitStore] Save failed (0x%X)\n", err);
        return false;
    }
    Serial.printf("[FruitStore] Saved model '%s' (%u bytes)\n",
                  fruit.fruit_name, (unsigned)sizeof(Fruit28D));
    return true;
}

bool FruitStore::load_model_to_ram(const char* fruit_name) {
    if (fruit_name == nullptr || fruit_name[0] == '\0') return false;

    char key[16];
    get_nvs_key(fruit_name, key);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    Fruit28D tmp{};
    size_t len = sizeof(tmp);
    esp_err_t err = nvs_get_blob(nvs, key, &tmp, &len);
    nvs_close(nvs);

    // Accept new (mask-carrying) and legacy 612-byte blobs alike; the
    // zeroed tail leaves active_class_mask = 0 -> firmware default classes.
    // A truncated/corrupt entry must never become the active classifier.
    if (err != ESP_OK || len < MODEL_WIRE_BYTES_LEGACY) {
        Serial.printf("[FruitStore] Load '%s' failed (0x%X, %u/%u bytes)\n",
                      fruit_name, err, (unsigned)len,
                      (unsigned)sizeof(Fruit28D));
        return false;
    }

    active_model_ram = tmp;
    strncpy(loaded_fruit_name, tmp.fruit_name, MAX_MODEL_NAME_LEN - 1);
    loaded_fruit_name[MAX_MODEL_NAME_LEN - 1] = '\0';
    is_model_loaded_in_ram = true;
    Serial.printf("[FruitStore] Loaded '%s' into RAM\n", loaded_fruit_name);
    return true;
}

void FruitStore::unload_active_model() {
    memset(&active_model_ram, 0, sizeof(Fruit28D));
    memset(loaded_fruit_name, 0, sizeof(loaded_fruit_name));
    is_model_loaded_in_ram = false;
}

bool FruitStore::delete_model_from_flash(const char* fruit_name) {
    if (fruit_name == nullptr || fruit_name[0] == '\0') return false;

    char key[16];
    get_nvs_key(fruit_name, key);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;

    esp_err_t err = nvs_erase_key(nvs, key);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        Serial.printf("[FruitStore] Delete '%s' failed (0x%X)\n",
                      fruit_name, err);
        return false;
    }

    // Dropping the active model leaves the device with nothing to classify.
    if (is_model_loaded_in_ram && strcmp(loaded_fruit_name, fruit_name) == 0) {
        unload_active_model();
    }
    Serial.printf("[FruitStore] Deleted '%s'\n", fruit_name);
    return true;
}

// ─── Model browser ────────────────────────────────────────────────
// Keys follow get_nvs_key(): "m_<name>". The active-name key is a string,
// so a BLOB-type entry scan naturally lists models only.

uint8_t FruitStore::model_count() {
    uint8_t n = 0;
    // Old arduino-esp32 core API: iterators are values; NULL ends the walk.
    nvs_iterator_t it = nvs_entry_find(nullptr, NVS_NAMESPACE, NVS_TYPE_BLOB);
    while (it != nullptr) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, "m_", 2) == 0 && n < 255) n++;
        it = nvs_entry_next(it);
    }
    nvs_release_iterator(it);
    return n;
}

bool FruitStore::model_name_at(uint8_t idx, char out[MAX_MODEL_NAME_LEN]) {
    nvs_iterator_t it = nvs_entry_find(nullptr, NVS_NAMESPACE, NVS_TYPE_BLOB);
    while (it != nullptr) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, "m_", 2) == 0 && idx == 0) {
            bool found = false;
            nvs_handle_t nvs;
            if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
                Fruit28D tmp{};
                size_t len = sizeof(tmp);
                if (nvs_get_blob(nvs, info.key, &tmp, &len) == ESP_OK) {
                    strncpy(out, tmp.fruit_name, MAX_MODEL_NAME_LEN - 1);
                    out[MAX_MODEL_NAME_LEN - 1] = '\0';
                    found = strlen(out) > 0;
                }
                nvs_close(nvs);
            }
            nvs_release_iterator(it);
            return found;
        }
        if (strncmp(info.key, "m_", 2) == 0) idx--;
        it = nvs_entry_next(it);
    }
    nvs_release_iterator(it);
    return false;
}

bool FruitStore::activate_by_index(uint8_t idx) {
    char name[MAX_MODEL_NAME_LEN];
    if (!model_name_at(idx, name)) return false;
    if (!load_model_to_ram(name)) return false;

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, ACTIVE_NAME_KEY, name);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    Serial.printf("[FruitStore] Active model: %s\n", name);
    return true;
}

void FruitStore::clear_active() {
    unload_active_model();
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, ACTIVE_NAME_KEY);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    Serial.println("[FruitStore] Active selection cleared.");
}

uint8_t FruitStore::active_index() {
    char active[MAX_MODEL_NAME_LEN] = {0};
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(active);
        nvs_get_str(nvs, ACTIVE_NAME_KEY, active, &len);
        nvs_close(nvs);
    }
    if (strlen(active) == 0) return 0xFF;

    uint8_t n = model_count();
    for (uint8_t i = 0; i < n; i++) {
        char name[MAX_MODEL_NAME_LEN];
        if (model_name_at(i, name) && strcmp(name, active) == 0) return i;
    }
    return 0xFF;
}
