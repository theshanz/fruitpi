#pragma once

#include "sci_32d.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

constexpr size_t MAX_MODEL_NAME_LEN = 32;
constexpr uint8_t MAX_CACHED_MODELS = 12;

class FruitStore {
private:
#if defined(_MSC_VER)
    __declspec(align(16)) Fruit32D active_model_ram;
#else
    alignas(16) Fruit32D active_model_ram;
#endif

    bool is_model_loaded_in_ram;
    char loaded_fruit_name[MAX_MODEL_NAME_LEN];

    // ── Browse cache: NVS entry iteration is NOT thread-safe vs the
    //    NimBLE task touching NVS concurrently (LoadProhibited crash),
    //    so we enumerate exactly ONCE at init and maintain in RAM. ──
    char cached_names[MAX_CACHED_MODELS][MAX_MODEL_NAME_LEN];
    uint8_t cached_count;
    SemaphoreHandle_t store_mutex;

    void get_nvs_key(const char* fruit_name, char key_out[16]) const;
    void rebuild_cache_locked();
    void cache_remove_locked(const char* nvs_key);
    void cache_add_locked(const char* fruit_name);

public:
    FruitStore();

    bool init();
    bool save_model(const Fruit32D& fruit);
    bool get_model_wire(const char* fruit_name, uint8_t out[MODEL_WIRE_BYTES]);
    bool load_model_to_ram(const char* fruit_name);
    void unload_active_model();
    bool delete_model_from_flash(const char* fruit_name);

    const Fruit32D* get_active_model_ptr() const {
        return is_model_loaded_in_ram ? &active_model_ram : nullptr;
    }

    // Both helper aliases defined to prevent missing member errors
    bool is_loaded() const { return is_model_loaded_in_ram; }
    bool has_model() const { return is_model_loaded_in_ram; }

    const char* get_loaded_fruit_name() const { return loaded_fruit_name; }

    // ── Model browser (on-device selector) ────────────────────────
    // Served from the RAM cache; stable order within one boot.
    uint8_t model_count();
    bool    model_name_at(uint8_t idx, char out[MAX_MODEL_NAME_LEN]);
    // Activates (loads to RAM) and persists the choice across reboots.
    bool    activate_by_index(uint8_t idx);
    void    clear_active();               // forget persisted choice + unload
    uint8_t active_index();               // 0xFF when none persists
};
