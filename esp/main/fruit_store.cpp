#include "fruit_store.h"
#include <Arduino.h>
#include <cstring>
#include <cctype>

constexpr const char* NVS_NAMESPACE = "fruit_models";
constexpr const char* ACTIVE_NAME_KEY = "active_name";
constexpr const char* STORE_FORMAT_KEY = "mmd_fmt";

// NVS format marker: magic "2FRT" + wire-format version. Verified on boot; a
// mismatch means the flash holds pre-852 (616/848-byte) models and must be
// re-initialized before the menu can trust any listing.
struct StoreFormatMarker {
    uint32_t magic;
    uint8_t version;
};

FruitStore::FruitStore()
    : is_model_loaded_in_ram(false), cached_count(0), store_mutex(nullptr) {
    memset(&active_model_ram, 0, sizeof(Fruit32D));
    memset(loaded_fruit_name, 0, sizeof(loaded_fruit_name));
    memset(cached_names, 0, sizeof(cached_names));
}

// ─── Browse cache internals ───────────────────────────────────────
// Enumerates NVS entries exactly once. Only ever called from init()
// with the mutex held — never races the NimBLE task afterwards.
void FruitStore::rebuild_cache_locked() {
    cached_count = 0;
    nvs_iterator_t it = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NAMESPACE,
                                       NVS_TYPE_BLOB);
    if (it == nullptr) return;
    nvs_handle_t nvs;
    const bool can_write =
        (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK);
    while (it != nullptr && cached_count < MAX_CACHED_MODELS) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, "m_", 2) == 0 && can_write) {
            // decode real fruit name from the blob's first bytes; a
            // corrupt/legacy blob must never surface in the menu
            Fruit32D tmp{};
            size_t len = sizeof(tmp);
            const bool usable =
                nvs_get_blob(nvs, info.key, &tmp, &len) == ESP_OK &&
                len == MODEL_WIRE_BYTES &&
                model_blob_is_valid(tmp);
            if (usable) {
                strncpy(cached_names[cached_count], tmp.fruit_name,
                        MAX_MODEL_NAME_LEN - 1);
                cached_names[cached_count][MAX_MODEL_NAME_LEN - 1] = '\0';
                cached_count++;
            } else {
                Serial.printf("[FruitStore] Dropped corrupt/legacy entry '%s'\n",
                              info.key);
                nvs_erase_key(nvs, info.key);
                nvs_commit(nvs);
            }
        }
        it = nvs_entry_next(it);
    }
    if (it != nullptr) nvs_release_iterator(it);
    if (can_write) nvs_close(nvs);
}

void FruitStore::cache_remove_locked(const char* nvs_key) {
    for (uint8_t i = 0; i < cached_count; i++) {
        char key[16];
        get_nvs_key(cached_names[i], key);
        if (strcmp(key, nvs_key) == 0) {
            for (uint8_t j = i; j + 1 < cached_count; j++) {
                memcpy(cached_names[j], cached_names[j + 1],
                       MAX_MODEL_NAME_LEN);
            }
            cached_count--;
            memset(cached_names[cached_count], 0, MAX_MODEL_NAME_LEN);
            return;
        }
    }
}

void FruitStore::cache_add_locked(const char* fruit_name) {
    for (uint8_t i = 0; i < cached_count; i++) {
        if (strcmp(cached_names[i], fruit_name) == 0) return; // overwrite
    }
    if (cached_count < MAX_CACHED_MODELS) {
        strncpy(cached_names[cached_count], fruit_name,
                MAX_MODEL_NAME_LEN - 1);
        cached_names[cached_count][MAX_MODEL_NAME_LEN - 1] = '\0';
        cached_count++;
    }
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

// Erases every model blob (keys "m_*") from the namespace. Used on first boot
// after a format bump to drop pre-852 legacy entries (616-byte 28D / 848-byte
// old-wire) that NVS would otherwise surface as selectable "models".
static size_t wipe_all_model_blobs(nvs_handle_t nvs) {
    size_t wiped = 0;
    nvs_iterator_t it = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NAMESPACE,
                                       NVS_TYPE_BLOB);
    while (it != nullptr) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, "m_", 2) == 0 &&
            nvs_erase_key(nvs, info.key) == ESP_OK) {
            wiped++;
            Serial.printf("[FruitStore] Wiped legacy entry '%s'\n", info.key);
        }
        it = nvs_entry_next(it);
    }
    if (it != nullptr) nvs_release_iterator(it);
    if (wiped > 0) nvs_commit(nvs);
    return wiped;
}

bool FruitStore::init() {
    if (store_mutex == nullptr) {
        store_mutex = xSemaphoreCreateMutex();
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }

    if (err != ESP_OK) {
        Serial.printf("[FruitStore] Error: NVS Flash initialization failed (0x%X)\n", err);
        return false;
    }

    // ── Format version gate ─────────────────────────────────────────
    // The 852-byte ManifoldModel32D is wire-locked. Boot verifies the store's
    // format marker (magic "2FRT" + v2) and wipes any legacy model blobs when
    // it is absent or stale, so the menu only ever lists blobs the classifier
    // can actually load. Individual corrupt entries are dropped during
    // rebuild_cache_locked() below.
    nvs_handle_t fmt;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &fmt) == ESP_OK) {
        StoreFormatMarker marker{};
        size_t mlen = sizeof(marker);
        const bool matches =
            nvs_get_blob(fmt, STORE_FORMAT_KEY, &marker, &mlen) == ESP_OK &&
            mlen == sizeof(marker) &&
            marker.magic == FRUIT32D_MAGIC &&
            marker.version == MODEL_STORE_VERSION;
        if (!matches) {
            const size_t wiped = wipe_all_model_blobs(fmt);
            nvs_erase_key(fmt, ACTIVE_NAME_KEY);
            marker.magic = FRUIT32D_MAGIC;
            marker.version = MODEL_STORE_VERSION;
            nvs_set_blob(fmt, STORE_FORMAT_KEY, &marker, sizeof(marker));
            nvs_commit(fmt);
            Serial.printf("[FruitStore] Store format v%u ('2FRT') initialized "
                          "(wiped %u legacy blob%s).\n",
                          (unsigned)MODEL_STORE_VERSION, (unsigned)wiped,
                          wiped == 1 ? "" : "s");
        }
        nvs_close(fmt);
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

    rebuild_cache_locked();   // one-time enumeration, mutex held
    Serial.printf("[FruitStore] NVS Multi-Fruit Storage Subsystem Ready "
                  "(%u model%s cached).\n",
                  cached_count, cached_count == 1 ? "" : "s");
    return true;
}

bool FruitStore::save_model(const Fruit32D& fruit) {
    if (fruit.fruit_name[0] == '\0') {
        Serial.println("[FruitStore] Refusing to save unnamed model.");
        return false;
    }
    if (!model_blob_is_valid(fruit)) {
        Serial.println("[FruitStore] Refusing to save malformed model blob.");
        return false;
    }

    char key[16];
    get_nvs_key(fruit.fruit_name, key);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        Serial.println("[FruitStore] Error: cannot open NVS namespace.");
        return false;
    }
    esp_err_t err = nvs_set_blob(nvs, key, &fruit, sizeof(Fruit32D));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        Serial.printf("[FruitStore] Save failed (0x%X)\n", err);
        return false;
    }
    Serial.printf("[FruitStore] Saved model '%s' (%u bytes)\n",
                  fruit.fruit_name, (unsigned)sizeof(Fruit32D));
    if (store_mutex) xSemaphoreTake(store_mutex, portMAX_DELAY);
    cache_add_locked(fruit.fruit_name);
    if (store_mutex) xSemaphoreGive(store_mutex);
    return true;
}

// Reads one model back in 852-byte wire format (struct padding excluded).
// Does NOT touch the active-model RAM slot.
bool FruitStore::get_model_wire(const char* fruit_name, uint8_t out[MODEL_WIRE_BYTES]) {
    if (fruit_name == nullptr || fruit_name[0] == '\0') return false;
    if (store_mutex) xSemaphoreTake(store_mutex, portMAX_DELAY);

    char key[16];
    get_nvs_key(fruit_name, key);

    nvs_handle_t nvs;
    bool ok = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        Fruit32D tmp{};
        size_t len = sizeof(tmp);
        if (nvs_get_blob(nvs, key, &tmp, &len) == ESP_OK &&
            len == MODEL_WIRE_BYTES && model_blob_is_valid(tmp)) {
            // blob head is wire-identical: name[32] | header | W | b
            memcpy(out, &tmp, MODEL_WIRE_BYTES);
            ok = true;
        }
        nvs_close(nvs);
    }
    if (store_mutex) xSemaphoreGive(store_mutex);
    return ok;
}

bool FruitStore::load_model_to_ram(const char* fruit_name) {
    if (fruit_name == nullptr || fruit_name[0] == '\0') return false;
    if (store_mutex) xSemaphoreTake(store_mutex, portMAX_DELAY);

    char key[16];
    get_nvs_key(fruit_name, key);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        if (store_mutex) xSemaphoreGive(store_mutex);
        return false;
    }

    Fruit32D tmp{};
    size_t len = sizeof(tmp);
    esp_err_t err = nvs_get_blob(nvs, key, &tmp, &len);
    nvs_close(nvs);
    // NOTE: mutex released on all exits below via helper lambda-free pattern

    // The 852-byte blob must be complete and structurally valid; a
    // truncated/corrupt/legacy entry must never become the active classifier.
    if (err != ESP_OK || len != MODEL_WIRE_BYTES ||
        !model_blob_is_valid(tmp)) {
        Serial.printf("[FruitStore] Load '%s' failed (0x%X, %u/%u bytes)\n",
                      fruit_name, err, (unsigned)len,
                      (unsigned)sizeof(Fruit32D));
        if (store_mutex) xSemaphoreGive(store_mutex);
        return false;
    }

    active_model_ram = tmp;
    strncpy(loaded_fruit_name, tmp.fruit_name, MAX_MODEL_NAME_LEN - 1);
    loaded_fruit_name[MAX_MODEL_NAME_LEN - 1] = '\0';
    is_model_loaded_in_ram = true;
    Serial.printf("[FruitStore] Loaded '%s' into RAM\n", loaded_fruit_name);
    if (store_mutex) xSemaphoreGive(store_mutex);
    return true;
}

void FruitStore::unload_active_model() {
    memset(&active_model_ram, 0, sizeof(Fruit32D));
    memset(loaded_fruit_name, 0, sizeof(loaded_fruit_name));
    is_model_loaded_in_ram = false;
}

bool FruitStore::delete_model_from_flash(const char* fruit_name) {
    if (fruit_name == nullptr || fruit_name[0] == '\0') return false;
    if (store_mutex) xSemaphoreTake(store_mutex, portMAX_DELAY);

    char key[16];
    get_nvs_key(fruit_name, key);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        if (store_mutex) xSemaphoreGive(store_mutex);
        return false;
    }

    esp_err_t err = nvs_erase_key(nvs, key);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        Serial.printf("[FruitStore] Delete '%s' failed (0x%X)\n",
                      fruit_name, err);
        if (store_mutex) xSemaphoreGive(store_mutex);
        return false;
    }
    cache_remove_locked(key);

    // Dropping the active model leaves the device with nothing to classify.
    if (is_model_loaded_in_ram && strcmp(loaded_fruit_name, fruit_name) == 0) {
        unload_active_model();
    }
    if (store_mutex) xSemaphoreGive(store_mutex);
    Serial.printf("[FruitStore] Deleted '%s'\n", fruit_name);
    return true;
}

// ─── Model browser (cache-backed — no NVS iteration at runtime) ───
// Keys follow get_nvs_key(): "m_<name>". The active-name key is a string,
// so a BLOB-type entry scan naturally lists models only.

uint8_t FruitStore::model_count() {
    if (store_mutex) xSemaphoreTake(store_mutex, portMAX_DELAY);
    uint8_t n = cached_count;
    if (store_mutex) xSemaphoreGive(store_mutex);
    return n;
}

bool FruitStore::model_name_at(uint8_t idx, char out[MAX_MODEL_NAME_LEN]) {
    if (store_mutex) xSemaphoreTake(store_mutex, portMAX_DELAY);
    bool ok = false;
    if (idx < cached_count) {
        strncpy(out, cached_names[idx], MAX_MODEL_NAME_LEN - 1);
        out[MAX_MODEL_NAME_LEN - 1] = '\0';
        ok = true;
    }
    if (store_mutex) xSemaphoreGive(store_mutex);
    return ok;
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

    for (uint8_t i = 0; i < model_count(); i++) {
        char name[MAX_MODEL_NAME_LEN];
        if (model_name_at(i, name) && strcmp(name, active) == 0) return i;
    }
    return 0xFF;
}
