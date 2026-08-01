#include "fruit_store.h"
#include <Arduino.h>
#include <cstring>
#include <cctype>

constexpr const char* NVS_NAMESPACE = "fruit_models";

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

    Serial.println("[FruitStore] NVS Multi-Model Storage Subsystem Ready.");
    return true;
}

bool FruitStore::save_model(const Fruit28D& fruit) {
    if (strlen(fruit.fruit_name) == 0) {
        Serial.println("[FruitStore] Error: Cannot save model with empty name!");
        return false;
    }

    char key[16];
    get_nvs_key(fruit.fruit_name, key);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        Serial.printf("[FruitStore] NVS Open failed: 0x%X\n", err);
        return false;
    }

    err = nvs_set_blob(nvs, key, &fruit, sizeof(Fruit28D));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
        Serial.printf("[FruitStore] Successfully saved model '%s' to NVS (key: %s)\n", fruit.fruit_name, key);
    } else {
        Serial.printf("[FruitStore] Save failed: 0x%X\n", err);
    }

    nvs_close(nvs);
    return (err == ESP_OK);
}

bool FruitStore::load_model_to_ram(const char* fruit_name) {
    char key[16];
    get_nvs_key(fruit_name, key);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        Serial.printf("[FruitStore] Model '%s' not found in NVS.\n", fruit_name);
        return false;
    }

    size_t required_size = sizeof(Fruit28D);

    // Direct read into aligned RAM buffer
    err = nvs_get_blob(nvs, key, &active_model_ram, &required_size);
    nvs_close(nvs);

    if (err == ESP_OK && required_size == sizeof(Fruit28D)) {
        is_model_loaded_in_ram = true;
        strncpy(loaded_fruit_name, active_model_ram.fruit_name, MAX_MODEL_NAME_LEN - 1);
        Serial.printf("[FruitStore] Loaded model '%s' into aligned RAM [Ptr: %p]\n",
                      loaded_fruit_name, (void*)&active_model_ram);
        return true;
    }

    Serial.printf("[FruitStore] Failed to load model '%s' (Error: 0x%X)\n", fruit_name, err);
    is_model_loaded_in_ram = false;
    return false;
}

void FruitStore::unload_active_model() {
    memset(&active_model_ram, 0, sizeof(Fruit28D));
    memset(loaded_fruit_name, 0, sizeof(loaded_fruit_name));
    is_model_loaded_in_ram = false;
    Serial.println("[FruitStore] Active model unloaded from RAM.");
}

bool FruitStore::delete_model_from_flash(const char* fruit_name) {
    char key[16];
    get_nvs_key(fruit_name, key);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return false;

    err = nvs_erase_key(nvs, key);
    if (err == ESP_OK) {
        nvs_commit(nvs);
        Serial.printf("[FruitStore] Deleted model '%s' from Flash.\n", fruit_name);

        // If the deleted model was currently in RAM, unload it
        if (is_model_loaded_in_ram && strcasecmp(loaded_fruit_name, fruit_name) == 0) {
            unload_active_model();
        }
    }

    nvs_close(nvs);
    return (err == ESP_OK);
}
