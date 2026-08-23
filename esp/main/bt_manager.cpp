#include "bt_manager.h"
#include <ArduinoJson.h>
#include <cmath>

#include "esp_heap_caps.h"

BTManager::BTManager() : pServer(nullptr), pCharModelTransfer(nullptr), pCharScanConfig(nullptr),
                         pCharScanResults(nullptr), pCharRawStream(nullptr), store_ref(nullptr),
                         device_connected(false), current_mode(MODE_INFERENCE),
                         capture_image_requested(false), arm_acoustic_requested(false),
                         raw_capture_requested(false), raw_capture_trigger_mode(false), raw_capture_windows(20), cancel_requested(false),
                         threshold_updated(false), new_threshold_val(0.15f),
                         tx_buf(nullptr), tx_len(0), tx_id(0), tx_type(0), tx_chunks(0), tx_active(false),
                         tx_last_activity_ms(0), tx_last_chunk_ms(0), tx_next_seq(0),
                         tx_resend_range_idx(0), tx_resend_seq(0), rx_buf(nullptr), rx_len(0), rx_id(0), rx_type(0),
                         rx_chunks(0), rx_received_count(0), rx_active(false), rx_last_activity_ms(0),
                         rx_next_progress_bytes(0), inference_requested(false), ms_scan_requested(false),
                         ms_debug_requested(false), ms_config_requested(false),
                         ms_cfg_gain_r(1.0f), ms_cfg_gain_g(1.0f), ms_cfg_gain_b(1.0f), ms_cfg_aec(0),
                         ms_cfg_ambient(true)
{
    memset(&current_config, 0, sizeof(ScanConfig));
    strncpy(current_config.target_fruit, "Mango", sizeof(current_config.target_fruit) - 1);
    memset(model_rx_buffer, 0, sizeof(model_rx_buffer));
}

uint16_t BTManager::next_transfer_id()
{
    static uint16_t counter = 0;
    if (++counter == 0)
        counter = 1;
    return counter;
}

bool BTManager::init(const char *device_name, FruitStore *store)
{
    store_ref = store;

    NimBLEDevice::init(device_name);
    NimBLEDevice::setMTU(512);

    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(this);

    NimBLEService *pService = pServer->createService(SERVICE_UUID);

    pCharModelTransfer = pService->createCharacteristic(
        CHAR_MODEL_TRANSFER_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    pCharModelTransfer->setCallbacks(this);

    pCharScanConfig = pService->createCharacteristic(
        CHAR_SCAN_CONFIG_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    pCharScanConfig->setCallbacks(this);

    pCharScanResults = pService->createCharacteristic(
        CHAR_SCAN_RESULTS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    pCharRawStream = pService->createCharacteristic(
        CHAR_RAW_STREAM_UUID,
        NIMBLE_PROPERTY::NOTIFY);

    pService->start();

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06); // iPhone connection optimization
    pAdvertising->setMaxPreferred(0x12);
    NimBLEDevice::startAdvertising();

    Serial.println("[BTManager] BLE Subsystem Advertising.");
    return true;
}
/////////////////////////// transfer stuff //////////////////////////
// ─── BLE Connection Callbacks ─────────────────────────────────────────
void BTManager::onConnect(NimBLEServer *pServer, ble_gap_conn_desc *desc)
{
    device_connected = true;
    Serial.println("[BTManager] Mobile App Connected.");
    if (desc)
    {
        // Request a faster connection interval (min 15ms, max 30ms) and
        // larger data length for high-throughput image streaming.
        pServer->updateConnParams(desc->conn_handle, 12, 24, 0, 400);
        pServer->setDataLen(desc->conn_handle, 251);
    }
}

void BTManager::onDisconnect(NimBLEServer *pServer, ble_gap_conn_desc *desc)
{
    device_connected = false;
    // Abort any in-flight transfer so stale data is never reused.
    free_tx();
    free_rx();
    Serial.println("[BTManager] Mobile App Disconnected. Restarting Advertising...");
    NimBLEDevice::startAdvertising();
}

// ─── BLE Write Handlers (Model Upload & App Commands) ───────────────
void BTManager::onWrite(NimBLECharacteristic *pCharacteristic)
{
    std::string rx_data = pCharacteristic->getValue();
    const uint8_t *d = (const uint8_t *)rx_data.data();
    size_t len = rx_data.length();

    if (len == 0)
        return;

    // A. Incoming Transfer (upload direction: laptop -> ESP)
    if (pCharacteristic == pCharModelTransfer)
    {
        if (len >= 8 && d[0] == PKT_TYPE_HEADER)
        {
            uint16_t id = (d[1] << 8) | d[2];
            size_t total = ((size_t)d[3] << 24) | ((size_t)d[4] << 16) |
                           ((size_t)d[5] << 8) | d[6];
            uint8_t type = d[7];
            start_rx(id, total, type);
        }
        else if (len >= 6)
        {
            uint8_t type = d[0];
            uint16_t id = (d[1] << 8) | d[2];
            uint16_t seq = (d[3] << 8) | d[4];
            bool end = (d[5] == 0x01);
            on_rx_chunk(id, seq, end, d + 6, len - 6);
        }
        return;
    }

    // B. Scan Config & Mode Selection (+ retransmission commands)
    else if (pCharacteristic == pCharScanConfig)
    {
        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, rx_data.c_str());

        if (!err)
        {

            if (doc.containsKey("command"))
            {
                const char *cmd = doc["command"];
                if (strcmp(cmd, "list_models") == 0)
                {
                    // Send list of installed models back to GUI
                    StaticJsonDocument<256> resp;
                    JsonArray arr = resp.createNestedArray("models");

                    // Query active model name or NVS Flash list
                    if (store_ref->has_model())
                    {
                        arr.add(store_ref->get_loaded_fruit_name());
                    }

                    char output[256];
                    size_t len = serializeJson(resp, output);
                    pCharScanResults->setValue((uint8_t *)output, len);
                    pCharScanResults->notify();
                }
                else if (strcmp(cmd, "delete_model") == 0 && doc.containsKey("fruit"))
                {
                    const char *target_fruit = doc["fruit"];
                    store_ref->delete_model_from_flash(target_fruit);
                    notify_status_change("model_deleted");
                }
                else if (strcmp(cmd, "resend") == 0 && doc.containsKey("id") && doc.containsKey("ranges"))
                {
                    uint16_t id = doc["id"];
                    std::vector<TransferRange> ranges;
                    JsonArray arr = doc["ranges"].as<JsonArray>();
                    for (JsonVariant r : arr)
                    {
                        TransferRange tr;
                        tr.start = r[0].as<uint16_t>();
                        tr.end = r[1].as<uint16_t>();
                        ranges.push_back(tr);
                    }
                    handle_resend(id, ranges);
                }
                else if (strcmp(cmd, "transfer_done") == 0 && doc.containsKey("id"))
                {
                    handle_transfer_done(doc["id"]);
                }
                else if (strcmp(cmd, "capture_image") == 0)
                {
                    capture_image_requested = true;
                }
                else if (strcmp(cmd, "arm_acoustic") == 0)
                {
                    arm_acoustic_requested = true;
                }
                else if (strcmp(cmd, "raw_capture") == 0)
                {
                    raw_capture_requested = true;
                    raw_capture_trigger_mode = false;
                    raw_capture_windows = 20;
                    if (doc.containsKey("trigger") && doc["trigger"].as<bool>()) {
                        raw_capture_trigger_mode = true;
                    }
                    if (doc.containsKey("windows")) {
                        raw_capture_windows = doc["windows"].as<uint8_t>();
                        if (raw_capture_windows < 1) raw_capture_windows = 1;
                        if (raw_capture_windows > 20) raw_capture_windows = 20;
                    }
                }
                else if (strcmp(cmd, "inference_request") == 0)
                {
                    inference_requested = true;
                }
                else if (strcmp(cmd, "ms_capture") == 0)
                {
                    ms_scan_requested = true;
                }
                else if (strcmp(cmd, "ms_debug") == 0)
                {
                    ms_debug_requested = true;
                }
                else if (strcmp(cmd, "ms_config") == 0 &&
                         doc.containsKey("gain_r") && doc.containsKey("gain_g") && doc.containsKey("gain_b"))
                {
                    ms_cfg_gain_r = doc["gain_r"];
                    ms_cfg_gain_g = doc["gain_g"];
                    ms_cfg_gain_b = doc["gain_b"];
                    ms_cfg_aec = doc.containsKey("aec") ? doc["aec"].as<int>() : 0;
                    ms_cfg_ambient = doc.containsKey("ambient") ? doc["ambient"].as<bool>() : true;
                    ms_config_requested = true;
                    Serial.printf("[BTManager] ms_config: gain=(%.2f,%.2f,%.2f) aec=%d ambient=%d\n",
                                  ms_cfg_gain_r, ms_cfg_gain_g, ms_cfg_gain_b, ms_cfg_aec,
                                  ms_cfg_ambient ? 1 : 0);
                }
                else if (strcmp(cmd, "cancel") == 0)
                {
                    cancel_requested = true;
                }
                else if (strcmp(cmd, "set_threshold") == 0 && doc.containsKey("threshold"))
                {
                    new_threshold_val = doc["threshold"];
                    threshold_updated = true;
                }
            }
            if (doc.containsKey("fruit"))
            {
                strncpy(current_config.target_fruit, doc["fruit"], sizeof(current_config.target_fruit) - 1);
                // Load model into RAM immediately upon switching fruit selection
                store_ref->load_model_to_ram(current_config.target_fruit);
            }

            if (doc.containsKey("volume_cm3"))
            {
                current_config.override_volume_cm3 = doc["volume_cm3"];
                current_config.use_volume_override = true;
            }

            if (doc.containsKey("mode"))
            {
                current_mode = (doc["mode"] == "data_collection") ? MODE_DATA_COLLECTION : MODE_INFERENCE;
                Serial.printf("[BTManager] Mode set to: %s\n",
                              (current_mode == MODE_DATA_COLLECTION) ? "DATA COLLECTION" : "INFERENCE");
            }
        }
    }
}

// ─── TransferEngine: Sender (downloads) ──────────────────────────────
bool BTManager::begin_transfer(uint8_t payload_type, const uint8_t *data, size_t len)
{
    if (!device_connected)
        return false;
    if (len == 0)
        return false;

    free_tx(); // never let a stale transfer buffer leak into a new one

    tx_buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!tx_buf)
    {
        Serial.printf("[BTManager] OOM allocating %u byte transfer buffer!\n", (unsigned)len);
        return false;
    }
    memcpy(tx_buf, data, len);
    tx_len = len;
    tx_id = next_transfer_id();
    tx_type = payload_type;
    tx_chunks = (len + CHUNK_SIZE - 1) / CHUNK_SIZE;
    tx_active = true;
    tx_next_seq = 0;
    tx_resend_range_idx = 0;
    tx_resend_seq = 0;
    tx_last_activity_ms = millis();
    tx_last_chunk_ms = tx_last_activity_ms;

    // Header
    uint8_t hdr[8];
    hdr[0] = PKT_TYPE_HEADER;
    hdr[1] = tx_id >> 8;
    hdr[2] = tx_id & 0xFF;
    hdr[3] = (tx_len >> 24) & 0xFF;
    hdr[4] = (tx_len >> 16) & 0xFF;
    hdr[5] = (tx_len >> 8) & 0xFF;
    hdr[6] = tx_len & 0xFF;
    hdr[7] = tx_type;
    pCharRawStream->setValue(hdr, sizeof(hdr));
    pCharRawStream->notify();

    // Non-blocking: the chunk stream is driven from service_transfer() so the
    // main loop keeps serving arm/status/raw-capture commands mid-transfer.
    Serial.printf("[BTManager] TX id:%u start: %u chunks (%u payload bytes), %lu ms/chunk.\n",
                  tx_id, (unsigned)tx_chunks, (unsigned)tx_len, (unsigned long)inter_chunk_delay_ms());
    return true;
}

void BTManager::send_chunk(uint16_t seq, bool end_flag)
{
    if (!tx_active)
        return;
    size_t offset = (size_t)seq * CHUNK_SIZE;
    if (offset >= tx_len)
        return;
    size_t n = min(CHUNK_SIZE, tx_len - offset);

    uint8_t pkt[CHUNK_SIZE + 6];
    pkt[0] = tx_type;
    pkt[1] = tx_id >> 8;
    pkt[2] = tx_id & 0xFF;
    pkt[3] = seq >> 8;
    pkt[4] = seq & 0xFF;
    pkt[5] = end_flag ? 0x01 : 0x00;
    memcpy(pkt + 6, tx_buf + offset, n);

    pCharRawStream->setValue(pkt, n + 6);
    pCharRawStream->notify();
}

void BTManager::send_pass_done()
{
    if (!tx_active)
        return;
    uint8_t pkt[3] = {PKT_TYPE_PASS_DONE, (uint8_t)(tx_id >> 8), (uint8_t)(tx_id & 0xFF)};
    pCharRawStream->setValue(pkt, sizeof(pkt));
    pCharRawStream->notify();
}

void BTManager::handle_resend(uint16_t id, const std::vector<TransferRange> &ranges)
{
    if (!tx_active || id != tx_id)
    {
        Serial.printf("[BTManager] Ignoring resend for inactive/stale id %u.\n", id);
        return;
    }
    tx_pending_resend = ranges;
    tx_resend_range_idx = 0;
    tx_resend_seq = ranges.empty() ? 0 : ranges[0].start;
    tx_last_activity_ms = millis();
}

void BTManager::handle_transfer_done(uint16_t id)
{
    if (!tx_active || id != tx_id)
        return;
    Serial.printf("[BTManager] TX id:%u acknowledged, releasing buffer.\n", id);
    free_tx();
}

void BTManager::service_transfer()
{
    uint32_t now = millis();
    if (!tx_active)
    {
        // Receiver timeout: upload stalled mid-way.
        if (rx_active && (now - rx_last_activity_ms > TRANSFER_TIMEOUT_MS))
        {
            Serial.println("[BTManager] RX transfer timed out, releasing buffer.");
            free_rx();
        }
        return;
    }

    // Sender timeout: receiver went away without acknowledging.
    if (now - tx_last_activity_ms > TRANSFER_TIMEOUT_MS)
    {
        Serial.println("[BTManager] TX transfer timed out, releasing buffer.");
        free_tx();
        return;
    }

    // Pace to the negotiated BLE connection interval; loop is free between chunks.
    if (now - tx_last_chunk_ms < inter_chunk_delay_ms())
        return;

    bool sent = false;
    // Resend pass has priority over the initial pass.
    if (!tx_pending_resend.empty())
    {
        while (tx_resend_range_idx < tx_pending_resend.size())
        {
            const TransferRange &r = tx_pending_resend[tx_resend_range_idx];
            if (tx_resend_seq <= r.end && tx_resend_seq < tx_chunks)
            {
                send_chunk(tx_resend_seq, (tx_resend_seq == tx_chunks - 1));
                tx_resend_seq++;
                sent = true;
                break;
            }
            tx_resend_range_idx++;
            tx_resend_seq = (tx_resend_range_idx < tx_pending_resend.size())
                               ? tx_pending_resend[tx_resend_range_idx].start : 0;
        }
        if (tx_resend_range_idx >= tx_pending_resend.size())
        {
            tx_pending_resend.clear();
            tx_resend_range_idx = 0;
            tx_resend_seq = 0;
            send_pass_done();
            Serial.println("[BTManager] TX resend pass complete.");
        }
    }
    else if (tx_next_seq < tx_chunks)
    {
        send_chunk(tx_next_seq, (tx_next_seq == tx_chunks - 1));
        tx_next_seq++;
        sent = true;
        if (tx_chunks >= 25 && (tx_next_seq % 25 == 0 || tx_next_seq == tx_chunks))
        {
            Serial.printf("[BTManager] TX id:%u %u/%u (%u%%)\n",
                          tx_id, (unsigned)tx_next_seq, (unsigned)tx_chunks,
                          (unsigned)(tx_next_seq * 100 / tx_chunks));
        }
        if (tx_next_seq >= tx_chunks)
        {
            send_pass_done();
            Serial.printf("[BTManager] TX id:%u streamed %u chunks.\n",
                          tx_id, (unsigned)tx_chunks);
        }
    }

    if (sent)
    {
        tx_last_chunk_ms = now;
        tx_last_activity_ms = now;
    }

    // Receiver timeout: upload stalled mid-way.
    if (rx_active && (now - rx_last_activity_ms > TRANSFER_TIMEOUT_MS))
    {
        Serial.println("[BTManager] RX transfer timed out, releasing buffer.");
        free_rx();
    }
}

void BTManager::free_tx()
{
    if (tx_buf)
    {
        heap_caps_free(tx_buf);
        tx_buf = nullptr;
    }
    tx_len = 0;
    tx_id = 0;
    tx_type = 0;
    tx_chunks = 0;
    tx_active = false;
    tx_next_seq = 0;
    tx_resend_range_idx = 0;
    tx_resend_seq = 0;
    tx_pending_resend.clear();
}

// ─── TransferEngine: Receiver (uploads) ──────────────────────────────
void BTManager::start_rx(uint16_t id, size_t total, uint8_t type)
{
    free_rx();
    if (total == 0 || total > sizeof(Fruit28D))
    {
        Serial.printf("[BTManager] RX reject: bad total %u.\n", (unsigned)total);
        return;
    }
    rx_id = id;
    rx_type = type;
    rx_len = total;
    rx_chunks = (total + CHUNK_SIZE - 1) / CHUNK_SIZE;
    rx_received_count = 0;
    rx_active = true;
    rx_last_activity_ms = millis();
    rx_next_progress_bytes = 0;
    rx_received.assign(rx_chunks, false);
    rx_buf = model_rx_buffer; // MODEL transfers land directly in the store buffer

    Serial.printf("[BTManager] RX id:%u start: %u bytes (%u chunks).\n",
                  id, (unsigned)total, (unsigned)rx_chunks);
}

void BTManager::on_rx_chunk(uint16_t id, uint16_t seq, bool end_flag,
                            const uint8_t *payload, size_t len)
{
    if (!rx_active || id != rx_id)
        return;
    if (seq >= rx_chunks)
        return;

    rx_last_activity_ms = millis();

    if (!rx_received[seq])
    {
        size_t offset = (size_t)seq * CHUNK_SIZE;
        if (offset + len > rx_len)
            return;
        memcpy(rx_buf + offset, payload, len);
        rx_received[seq] = true;
        rx_received_count++;

        if (rx_received_count * CHUNK_SIZE >= rx_next_progress_bytes + PROGRESS_NOTIFY_BYTES ||
            rx_received_count == rx_chunks)
        {
            notify_rx_progress();
        }
    }

    if (rx_received_count == rx_chunks)
    {
        Serial.printf("[BTManager] RX id:%u complete (%u bytes).\n", id, (unsigned)rx_len);
        if (rx_type == PKT_TYPE_MODEL)
        {
            process_incoming_model();
        }
        free_rx();
    }
}

void BTManager::notify_rx_progress()
{
    size_t received = (rx_received_count > rx_chunks) ? rx_len : rx_received_count * CHUNK_SIZE;
    if (received > rx_len)
        received = rx_len;

    StaticJsonDocument<128> doc;
    doc["status"] = "transfer_progress";
    doc["id"] = rx_id;
    doc["received"] = (unsigned long)received;
    doc["total"] = (unsigned long)rx_len;

    char output[128];
    size_t n = serializeJson(doc, output);
    pCharScanResults->setValue((uint8_t *)output, n);
    pCharScanResults->notify();

    Serial.printf("[BTManager] RX id:%u %u/%u (%u%%)\n",
                  rx_id, (unsigned)received, (unsigned)rx_len,
                  (unsigned)((received * 100) / rx_len));
    rx_next_progress_bytes = received;
}

void BTManager::free_rx()
{
    rx_buf = nullptr;
    rx_len = 0;
    rx_id = 0;
    rx_type = 0;
    rx_chunks = 0;
    rx_received_count = 0;
    rx_active = false;
    rx_received.clear();
}

uint32_t BTManager::inter_chunk_delay_ms()
{
    uint16_t conn_itvl = 0;
    NimBLEServer *server = NimBLEDevice::getServer();
    if (server && !server->getPeerDevices().empty())
    {
        conn_itvl = server->getPeerInfo(0).getConnInterval(); // units of 1.25 ms
    }
    uint32_t d = (conn_itvl > 0) ? (uint32_t)(conn_itvl * 1.25f * 1.2f) : 30;
    if (d < 10)
        d = 10;
    return d;
}

// ─── Save Received Model to NVS ──────────────────────────────────────
void BTManager::process_incoming_model()
{
    Fruit28D incoming_model;
    memcpy(&incoming_model, model_rx_buffer, sizeof(Fruit28D));

    Serial.printf("[BTManager] Completed Binary Model Upload: '%s'\n", incoming_model.fruit_name);

    if (store_ref && store_ref->save_model(incoming_model))
    {
        // Automatically activate newly saved model into RAM
        store_ref->load_model_to_ram(incoming_model.fruit_name);
        notify_status_change("model_saved");
        Serial.println("[BTManager] New Model Saved to Flash & Loaded to RAM!");
    }
    else
    {
        notify_status_change("model_error");
        Serial.println("[BTManager] Failed to save new model to NVS.");
    }
}

// ─── Send Inference Results to Mobile App ───────────────────────────
void BTManager::notify_scan_result(const BiologicalStatus &result)
{
    if (!device_connected)
        return;

    StaticJsonDocument<384> doc;
    doc["decision"] = result.primary_decision;
    doc["ripeness_index"] = result.ripeness_index;
    doc["confidence"] = result.confidence;
    doc["entropy"] = result.transition_entropy;
    doc["is_anomaly"] = result.is_anomaly;

    JsonObject probs = doc.createNestedObject("probabilities");
    probs["unripe"] = result.probabilities[0];
    probs["ripe"] = result.probabilities[1];
    probs["overripe"] = result.probabilities[2];
    probs["rotten"] = result.probabilities[3];
    probs["artificially_ripened"] = result.probabilities[4];

    char output[384];
    size_t len = serializeJson(doc, output);

    pCharScanResults->setValue((uint8_t *)output, len);
    pCharScanResults->notify();
}

void BTManager::notify_ms_features(const ColorFeatures &f)
{
    if (!device_connected)
        return;

    float sum = f.raw_rgb_means[0] + f.raw_rgb_means[1] + f.raw_rgb_means[2];
    float inv = sum > 1e-6f ? 1.0f / sum : 0.0f;

    StaticJsonDocument<448> doc;
    doc["status"] = "ms_captured";

    JsonArray hist = doc.createNestedArray("hue_histogram");
    for (int i = 0; i < 8; i++) hist.add(f.hue_histogram[i]);
    doc["chromatic_dispersion"] = f.chromatic_dispersion;
    doc["volume_cm3"] = f.volume_cm3;

    JsonArray means = doc.createNestedArray("raw_rgb_means");
    for (int i = 0; i < 3; i++) means.add(f.raw_rgb_means[i]);

    JsonArray ratios = doc.createNestedArray("rgb_ratios");
    for (int i = 0; i < 3; i++) ratios.add(f.raw_rgb_means[i] * inv);

    JsonArray ambient = doc.createNestedArray("ambient_rgb_means");
    for (int i = 0; i < 3; i++) ambient.add(f.ambient_rgb_means[i]);

    char output[448];
    size_t len = serializeJson(doc, output);

    pCharScanResults->setValue((uint8_t *)output, len);
    pCharScanResults->notify();
}

void BTManager::notify_status_change(const char *status_msg)
{
    if (!device_connected)
        return;

    StaticJsonDocument<128> doc;
    doc["status"] = status_msg;

    char output[128];
    size_t len = serializeJson(doc, output);
    pCharScanResults->setValue((uint8_t *)output, len);
    pCharScanResults->notify();
}

// ─── Public stream entry points (thin wrappers over the engine) ──────
void BTManager::send_raw_jpeg_stream(const uint8_t *jpeg_buf, size_t jpeg_len)
{
    if (current_mode != MODE_DATA_COLLECTION)
        return;
    Serial.printf("[BTManager] Streaming %u byte High-Res JPEG over BLE...\n", (unsigned)jpeg_len);
    begin_transfer(PKT_TYPE_JPEG, jpeg_buf, jpeg_len);
}

void BTManager::send_raw_acoustic_waveform(const uint16_t raw_adc[512], uint16_t peak_adc)
{
    if (current_mode != MODE_DATA_COLLECTION)
        return;
    Serial.println("[BTManager] Streaming Raw Acoustic Impact Waveform over BLE...");
    begin_transfer(PKT_TYPE_RAW_WAVEFORM, (const uint8_t *)raw_adc, 512 * sizeof(uint16_t));
}

void BTManager::send_ms_debug_jpeg(const uint8_t *jpeg_buf, size_t jpeg_len)
{
    Serial.printf("[BTManager] ms_debug: streaming %u byte flash JPEG over BLE...\n", (unsigned)jpeg_len);
    begin_transfer(PKT_TYPE_JPEG, jpeg_buf, jpeg_len);
}

bool BTManager::check_inference_request()
{
    if(inference_requested){ inference_requested = false; return true; }
    return false;
}
