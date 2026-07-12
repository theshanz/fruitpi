#include "adc_capture.h"
#include "camera_handler.h"
#include <Wire.h>

static ADCCapture adc;
static String serial_buf;

static uint32_t threshold = 300;

static void sendJSON(const String& json) {
    Serial.println(json);
}

static String toBase64(const uint16_t* data, uint32_t count) {
    static const char B64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String out;
    out.reserve(((count * 2 + 2) / 3) * 4);

    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t byte_len = count * 2;

    for (uint32_t i = 0; i < byte_len; i += 3) {
        uint32_t n = (uint32_t)bytes[i] << 16;
        if (i + 1 < byte_len) n |= (uint32_t)bytes[i + 1] << 8;
        if (i + 2 < byte_len) n |= bytes[i + 2];
        out += B64[(n >> 18) & 0x3F];
        out += B64[(n >> 12) & 0x3F];
        out += (i + 1 < byte_len) ? B64[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < byte_len) ? B64[n & 0x3F] : '=';
    }
    return out;
}

static void scanI2C() {
    Wire.begin(CAM_PIN_SIOD, CAM_PIN_SIOC);
    Serial.println("{\"i2c_scan\":\"start\"}");
    uint8_t found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.println("{\"i2c_device\":" + String(addr) + "}");
            found++;
        }
    }
    Serial.println("{\"i2c_scan\":\"done\",\"count\":" + String(found) + "}");
}

static void handleCommand(const String& cmd) {
    if (cmd == "PING") {
        sendJSON("{\"status\":\"pong\"}");

    } else if (cmd == "ARM") {
        digitalWrite(LED_PIN, HIGH);
        sendJSON("{\"status\":\"armed\"}");

        uint16_t* buf = (uint16_t*)ps_malloc(SAMPLE_COUNT * sizeof(uint16_t));
        if (!buf) {
            sendJSON("{\"error\":\"alloc_failed\"}");
            digitalWrite(LED_PIN, LOW);
            return;
        }

        // Phase 1: monitor for rising-edge threshold crossing
        uint16_t prev = (uint16_t)adc1_get_raw(PIEZO_CHANNEL);
        bool triggered = false;
        uint32_t timeout = SAMPLE_RATE_HZ * 10;  // 10s max wait
        uint32_t elapsed = 0;

        while (!triggered && elapsed < timeout) {
            uint16_t cur = (uint16_t)adc1_get_raw(PIEZO_CHANNEL);
            elapsed++;
            if (prev < threshold && cur >= threshold) {
                triggered = true;
                buf[0] = cur;
            }
            prev = cur;
        }

        if (!triggered) {
            free(buf);
            sendJSON("{\"error\":\"timeout\"}");
            digitalWrite(LED_PIN, LOW);
            return;
        }

        // Phase 2: capture remaining samples
        uint32_t n = adc.capture(buf, SAMPLE_COUNT);
        (void)n;

        digitalWrite(LED_PIN, LOW);

        String b64 = toBase64(buf, SAMPLE_COUNT);
        float rate = adc.lastActualRate();
        sendJSON("{\"samples_b64\":\"" + b64 + "\",\"actual_rate\":" + String(rate, 0) + "}");
        free(buf);

    } else if (cmd.startsWith("THRESHOLD ")) {
        int32_t val = cmd.substring(10).toInt();
        if (val > 0 && val < 4096) {
            threshold = val;
            sendJSON("{\"status\":\"threshold_set\",\"threshold\":" + String(threshold) + "}");
        } else {
            sendJSON("{\"error\":\"invalid_threshold\"}");
        }

    } else if (cmd == "CAPTURE") {
        String b64 = captureBase64();
        if (b64.length() > 0) {
            sendJSON("{\"photo\":\"" + b64 + "\"}");
        } else {
            sendJSON("{\"error\":\"capture_failed\"}");
        }
    }
}

void setup() {
    Serial.begin(921600);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    if (!adc.begin()) {
        sendJSON("{\"error\":\"adc_init_failed\"}");
    }

    scanI2C();

    if (!initCamera()) {
        sendJSON("{\"error\":\"camera_init_failed\"}");
    }

    sendJSON("{\"status\":\"ready\"}");
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
}

void loop() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (serial_buf.length() > 0) {
                serial_buf.trim();
                handleCommand(serial_buf);
                serial_buf = "";
            }
        } else {
            serial_buf += c;
            if (serial_buf.length() > 64) serial_buf = "";
        }
    }
}
