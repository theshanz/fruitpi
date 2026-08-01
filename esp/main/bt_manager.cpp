#include "bt_manager.h"
#include <ArduinoJson.h>

constexpr uint8_t PKT_TYPE_JPEG = 0x01;
constexpr uint8_t PKT_TYPE_RAW_WAVEFORM = 0x02;

BTManager::BTManager() :
pServer(nullptr), pCharModelTransfer(nullptr), pCharScanConfig(nullptr),
pCharScanResults(nullptr), pCharRawStream(nullptr), store_ref(nullptr),
device_connected(false), current_mode(MODE_INFERENCE),
capture_image_requested(false), arm_acoustic_requested(false),
cancel_requested(false), threshold_updated(false), new_threshold_val(0.15f),
model_rx_bytes(0)
{
    memset(&current_config, 0, sizeof(ScanConfig));
    strncpy(current_config.target_fruit, "Mango", sizeof(current_config.target_fruit) - 1);
}

bool BTManager::init(const char* device_name, FruitStore* store){
    store_ref = store;

    NimBLEDevice::init(device_name);
    NimBLEDevice::setMTU(512);

    pServer = NimBLEDevice::createServer();
    pServer -> setCallbacks(this);

    NimBLEService* pService = pServer->createService(SERVICE_UUID);

    pCharModelTransfer = pService->createCharacteristic(
        CHAR_MODEL_TRANSFER_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pCharModelTransfer->setCallbacks(this);

    pCharScanConfig = pService->createCharacteristic(
        CHAR_SCAN_CONFIG_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
    );
    pCharScanConfig->setCallbacks(this);

    pCharScanResults = pService->createCharacteristic(
        CHAR_SCAN_RESULTS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pCharRawStream = pService->createCharacteristic(
        CHAR_RAW_STREAM_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );

    pService->start();

    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
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
void BTManager::onConnect(NimBLEServer* pServer) {
    device_connected = true;
    Serial.println("[BTManager] Mobile App Connected.");
}

void BTManager::onDisconnect(NimBLEServer* pServer) {
    device_connected = false;
    Serial.println("[BTManager] Mobile App Disconnected. Restarting Advertising...");
    NimBLEDevice::startAdvertising();
}

// ─── BLE Write Handlers (Model Upload & App Commands) ───────────────
void BTManager::onWrite(NimBLECharacteristic* pCharacteristic) {
    std::string rx_data = pCharacteristic->getValue();
    size_t len = rx_data.length();

    if (len == 0) return;

    // A. Model Transfer Ingestion
    if (pCharacteristic == pCharModelTransfer) {
        if (model_rx_bytes + len <= sizeof(Fruit28D)) {
            memcpy(model_rx_buffer + model_rx_bytes, rx_data.data(), len);
            model_rx_bytes += len;

            Serial.printf("[BTManager] Received Model Chunk (%d bytes). Total: %d / %d\n",
                          len, model_rx_bytes, sizeof(Fruit28D));

            if (model_rx_bytes == sizeof(Fruit28D)) {
                process_incoming_model();
            }
        } else {
            Serial.println("[BTManager] Error: Model payload overflow! Resetting buffer.");
            model_rx_bytes = 0;
        }
    }

    // B. Scan Config & Mode Selection
    else if (pCharacteristic == pCharScanConfig) {
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, rx_data.c_str());

        if (!err) {

            if (doc.containsKey("command")) {
                           const char* cmd = doc["command"];
                           if (strcmp(cmd, "list_models") == 0) {
                                  // Send list of installed models back to GUI
                                  StaticJsonDocument<256> resp;
                                  JsonArray arr = resp.createNestedArray("models");

                                  // Query active model name or NVS Flash list
                                  if (store_ref->has_model()) {
                                      arr.add(store_ref->get_loaded_fruit_name());
                                  }

                                  char output[256];
                                  size_t len = serializeJson(resp, output);
                                  pCharScanResults->setValue((uint8_t*)output, len);
                                  pCharScanResults->notify();
                              }
                              else if (strcmp(cmd, "delete_model") == 0 && doc.containsKey("fruit")) {
                                  const char* target_fruit = doc["fruit"];
                                  store_ref->delete_model_from_flash(target_fruit);
                                  notify_status_change("model_deleted");
                              }
                           if (strcmp(cmd, "capture_image") == 0) {
                               capture_image_requested = true;
                           } else if (strcmp(cmd, "arm_acoustic") == 0) {
                               arm_acoustic_requested = true;
                           } else if (strcmp(cmd, "arm_full") == 0) {
                               capture_image_requested = true;
                               arm_acoustic_requested = true;
                           } else if (strcmp(cmd, "cancel") == 0) {
                               cancel_requested = true;
                           } else if (strcmp(cmd, "set_threshold") == 0 && doc.containsKey("threshold")) {
                               new_threshold_val = doc["threshold"];
                               threshold_updated = true;
                           }
                       }
            if (doc.containsKey("fruit")) {
                strncpy(current_config.target_fruit, doc["fruit"], sizeof(current_config.target_fruit) - 1);
                // Load model into RAM immediately upon switching fruit selection
                store_ref->load_model_to_ram(current_config.target_fruit);
            }

            if (doc.containsKey("volume_cm3")) {
                current_config.override_volume_cm3 = doc["volume_cm3"];
                current_config.use_volume_override = true;
            }

            if (doc.containsKey("mode")) {
                current_mode = (doc["mode"] == "data_collection") ? MODE_DATA_COLLECTION : MODE_INFERENCE;
                Serial.printf("[BTManager] Mode set to: %s\n",
                              (current_mode == MODE_DATA_COLLECTION) ? "DATA COLLECTION" : "INFERENCE");
            }
        }
    }
}

// ─── Save Received Model to NVS ──────────────────────────────────────
void BTManager::process_incoming_model() {
    Fruit28D incoming_model;
    memcpy(&incoming_model, model_rx_buffer, sizeof(Fruit28D));

    Serial.printf("[BTManager] Completed Binary Model Upload: '%s'\n", incoming_model.fruit_name);

    if (store_ref && store_ref->save_model(incoming_model)) {
        // Automatically activate newly saved model into RAM
        store_ref->load_model_to_ram(incoming_model.fruit_name);
        Serial.println("[BTManager] New Model Saved to Flash & Loaded to RAM!");
    } else {
        Serial.println("[BTManager] Failed to save new model to NVS.");
    }

    model_rx_bytes = 0; // Reset receive buffer
}

// ─── Send Inference Results to Mobile App ───────────────────────────
void BTManager::notify_scan_result(const BiologicalStatus& result) {
    if (!device_connected) return;

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

    pCharScanResults->setValue((uint8_t*)output, len);
    pCharScanResults->notify();
}

// ─── Chunked Data Streamer (Data Collection Mode) ────────────────────
void BTManager::send_chunked_data(const uint8_t* data, size_t len, uint8_t packet_type) {
    if (!device_connected) return;

    constexpr size_t CHUNK_SIZE = 200; // Fits BLE MTU
    uint8_t packet[CHUNK_SIZE + 4];
    size_t offset = 0;
    uint16_t seq = 0;
    size_t chunks_sent = 0;

    // Pace one notification per connection event so the link never overflows.
    // (NimBLE silently drops notifications when the host/controller is busy.)
    uint16_t conn_itvl = 0;
    NimBLEServer* server = NimBLEDevice::getServer();
    if (server && !server->getPeerDevices().empty()) {
        conn_itvl = server->getPeerInfo(0).getConnInterval(); // units of 1.25 ms
    }
    uint32_t inter_chunk_delay_ms = (conn_itvl > 0) ? (uint32_t)(conn_itvl * 1.25f * 1.2f) : 30;
    if (inter_chunk_delay_ms < 10) inter_chunk_delay_ms = 10;

    while (offset < len) {
        size_t bytes_to_send = min(CHUNK_SIZE, len - offset);

        packet[0] = packet_type;
        packet[1] = (uint8_t)(seq >> 8);
        packet[2] = (uint8_t)(seq & 0xFF);
        packet[3] = (offset + bytes_to_send >= len) ? 0x01 : 0x00; // End-of-frame flag

        memcpy(packet + 4, data + offset, bytes_to_send);

        pCharRawStream->setValue(packet, bytes_to_send + 4);
        pCharRawStream->notify();

        offset += bytes_to_send;
        seq++;
        chunks_sent++;
        delay(inter_chunk_delay_ms);
    }

    Serial.printf("[BTManager] Streamed %zu chunks (%zu payload bytes), %lu ms/chunk.\n",
                  chunks_sent, offset, (unsigned long)inter_chunk_delay_ms);
}
void BTManager::notify_status_change(const char* status_msg) {
    if (!device_connected) return;

    StaticJsonDocument<128> doc;
    doc["status"] = status_msg;

    char output[128];
    size_t len = serializeJson(doc, output);
    pCharScanResults->setValue((uint8_t*)output, len);
    pCharScanResults->notify();
}
void BTManager::send_raw_jpeg_stream(const uint8_t* jpeg_buf, size_t jpeg_len) {
    if (current_mode != MODE_DATA_COLLECTION) return;
    Serial.printf("[BTManager] Streaming %d byte High-Res JPEG over BLE...\n", jpeg_len);
    send_chunked_data(jpeg_buf, jpeg_len, PKT_TYPE_JPEG);
}

void BTManager::send_raw_acoustic_waveform(const uint16_t raw_adc[512], uint16_t peak_adc) {
    if (current_mode != MODE_DATA_COLLECTION) return;
    Serial.println("[BTManager] Streaming Raw Acoustic Impact Waveform over BLE...");
    send_chunked_data((const uint8_t*)raw_adc, 512 * sizeof(uint16_t), PKT_TYPE_RAW_WAVEFORM);
}

//////////////////////////////////////////////////////
