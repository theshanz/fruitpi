#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <vector>

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


constexpr uint8_t PKT_TYPE_HEADER       = 0x03;
constexpr uint8_t PKT_TYPE_JPEG         = 0x01;
constexpr uint8_t PKT_TYPE_RAW_WAVEFORM = 0x02;
constexpr uint8_t PKT_TYPE_MODEL        = 0x04;
constexpr uint8_t PKT_TYPE_PASS_DONE    = 0x05;

constexpr size_t  CHUNK_SIZE            = 500;
constexpr uint32_t TRANSFER_TIMEOUT_MS  = 20000;
constexpr uint32_t PROGRESS_NOTIFY_BYTES = 2000;

struct TransferRange {
    uint16_t start;
    uint16_t end;
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
    bool raw_capture_requested;
    bool raw_capture_trigger_mode;   // true = trigger-based, false = continuous
    uint8_t raw_capture_windows;     // number of capture windows (default 20)
    bool cancel_requested;
    bool threshold_updated;
    float new_threshold_val;
    bool inference_requested;
    bool ms_scan_requested;
    bool ms_debug_requested;
    bool ms_config_requested;
    float ms_cfg_gain_r, ms_cfg_gain_g, ms_cfg_gain_b;
    int   ms_cfg_aec;
    bool  ms_cfg_ambient;

    // Destination buffer for received MODEL transfers
    uint8_t model_rx_buffer[sizeof(Fruit28D)];

    // ─── TransferEngine: Sender state (downloads) ───
    uint8_t*  tx_buf;
    size_t    tx_len;
    uint16_t  tx_id;
    uint8_t   tx_type;
    size_t    tx_chunks;
    bool      tx_active;
    uint32_t  tx_last_activity_ms;
    uint32_t  tx_last_chunk_ms;
    uint16_t  tx_next_seq;            // next chunk of the initial pass
    size_t    tx_resend_range_idx;    // cursor into tx_pending_resend
    uint16_t  tx_resend_seq;          // current chunk inside the range
    std::vector<TransferRange> tx_pending_resend;

    // ─── TransferEngine: Receiver state (uploads) ───
    uint8_t*  rx_buf;
    size_t    rx_len;
    uint16_t  rx_id;
    uint8_t   rx_type;
    size_t    rx_chunks;
    size_t    rx_received_count;
    bool      rx_active;
    uint32_t  rx_last_activity_ms;
    uint32_t  rx_next_progress_bytes;
    std::vector<bool> rx_received;

    uint16_t next_transfer_id();

    // Sender
    void send_chunk(uint16_t seq, bool end_flag);
    void send_pass_done();
    bool begin_transfer(uint8_t payload_type, const uint8_t* data, size_t len);
    void handle_resend(uint16_t id, const std::vector<TransferRange>& ranges);
    void handle_transfer_done(uint16_t id);
    void free_tx();

    // Receiver
    void start_rx(uint16_t id, size_t total, uint8_t type);
    void on_rx_chunk(uint16_t id, uint16_t seq, bool end_flag,
                     const uint8_t* payload, size_t len);
    void notify_rx_progress();
    void free_rx();

    uint32_t inter_chunk_delay_ms();

    void process_incoming_model();

public:
    BTManager();
    bool init(const char* device_name, FruitStore* store);

    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override;
    void onDisconnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override;
    void onWrite(NimBLECharacteristic* pCharacteristic) override;

    void notify_scan_result(const BiologicalStatus& result);
    void notify_ms_features(const ColorFeatures& f);
    void notify_status_change(const char* status_msg);
    void send_raw_jpeg_stream(const uint8_t* jpeg_buf, size_t jpeg_len);
    void send_raw_acoustic_waveform(const uint16_t raw_adc[512], uint16_t peak_adc);
    void send_ms_debug_jpeg(const uint8_t* jpeg_buf, size_t jpeg_len);
    void service_transfer();
    bool is_transfer_active() const { return tx_active; }
    bool check_inference_request();

    bool check_capture_image_request() {
        if (capture_image_requested) { capture_image_requested = false; return true; }
        return false;
    }

    bool check_ms_scan_request() {
        if (ms_scan_requested) { ms_scan_requested = false; return true; }
        return false;
    }

    bool check_ms_debug_request() {
        if (ms_debug_requested) { ms_debug_requested = false; return true; }
        return false;
    }

    bool check_ms_config_request(float* gain_r, float* gain_g, float* gain_b, int* aec, bool* ambient) {
        if (!ms_config_requested) return false;
        ms_config_requested = false;
        *gain_r = ms_cfg_gain_r;
        *gain_g = ms_cfg_gain_g;
        *gain_b = ms_cfg_gain_b;
        *aec = ms_cfg_aec;
        *ambient = ms_cfg_ambient;
        return true;
    }

    bool check_arm_acoustic_request() {
        if (arm_acoustic_requested) { arm_acoustic_requested = false; return true; }
        return false;
    }

    bool check_raw_capture_request() {
        if (raw_capture_requested) { raw_capture_requested = false; return true; }
        return false;
    }

    bool is_raw_capture_trigger_mode() const { return raw_capture_trigger_mode; }
    uint8_t get_raw_capture_windows() const { return raw_capture_windows; }

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
