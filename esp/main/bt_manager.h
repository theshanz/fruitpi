#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "sci_28d.h"
#include "fruit_store.h"

#define SERVICE_UUID             "4fa10001-2241-4cf5-9988-34824317f012"
#define CHAR_MODEL_TRANSFER_UUID "4fa10002-2241-4cf5-9988-34824317f012"
#define CHAR_SCAN_CONFIG_UUID    "4fa10003-2241-4cf5-9988-34824317f012"
#define CHAR_SCAN_RESULTS_UUID   "4fa10004-2241-4cf5-9988-34824317f012"
#define CHAR_RAW_STREAM_UUID     "4fa10005-2241-4cf5-9988-34824317f012"

enum SystemMode {
    MODE_INFERENCE = 0,
    MODE_DATA_COLLECTION = 1
};

struct ScanConfig {
    char target_fruit[32];
    float override_volume_cm3;
    bool use_volume_override;
};

class BTManager : public NimBLEServerCallbacks, public NimBLECharacteristicCallbacks {
private:
    NimBLEServer* pServer;
    NimBLECharacteristic* pCharModelTransfer;
    NimBLECharacteristic* pCharScanConfig;
    NimBLECharacteristic* pCharScanResults;
    NimBLECharacteristic* pCharRawStream;

    FruitStore* store_ref;
    bool device_connected;
    SystemMode current_mode;
    ScanConfig current_config;

    // Command Flags matching bt_manager.cpp
    bool capture_image_requested;
    bool arm_acoustic_requested;
    bool cancel_requested;
    bool threshold_updated;
    float new_threshold_val;

    uint8_t model_rx_buffer[sizeof(Fruit28D)];
    size_t model_rx_bytes;

    void process_incoming_model();
    void send_chunked_data(const uint8_t* data, size_t len, uint8_t packet_type);

public:
    BTManager();
    bool init(const char* device_name, FruitStore* store);

    void onConnect(NimBLEServer* pServer) override;
    void onDisconnect(NimBLEServer* pServer) override;
    void onWrite(NimBLECharacteristic* pCharacteristic) override;

    void notify_scan_result(const BiologicalStatus& result);
    void notify_status_change(const char* status_msg);
    void send_raw_jpeg_stream(const uint8_t* jpeg_buf, size_t jpeg_len);
    void send_raw_acoustic_waveform(const uint16_t raw_adc[512], uint16_t peak_adc);

    bool check_capture_image_request() {
        if (capture_image_requested) { capture_image_requested = false; return true; }
        return false;
    }

    bool check_arm_acoustic_request() {
        if (arm_acoustic_requested) { arm_acoustic_requested = false; return true; }
        return false;
    }

    bool check_and_clear_cancel_request() {
        if (cancel_requested) { cancel_requested = false; return true; }
        return false;
    }

    bool has_threshold_update() const { return threshold_updated; }
    float get_updated_threshold() {
        threshold_updated = false;
        return new_threshold_val;
    }

    SystemMode get_mode() const { return current_mode; }
    const ScanConfig& get_config() const { return current_config; }
    bool is_connected() const { return device_connected; }
};
