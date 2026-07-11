#include "tap_detector.h"
#include "camera_handler.h"

// ─── State ──────────────────────────────────────────────────────────
enum AppState { IDLE, ARMED, WAIT_CAPTURE };
static AppState app_state = IDLE;

static TapDetector tap;
static String serial_buf;
static bool done_sent;

// ─── Helpers ────────────────────────────────────────────────────────
static void ledBlink(int count, int ms = 100) {
    for (int i = 0; i < count; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(ms / 2);
        digitalWrite(LED_PIN, LOW);
        delay(ms / 2);
    }
}

static void sendJSON(const String& json) {
    Serial.println(json);
}

static void sendTapJSON(uint8_t tap_num, const uint16_t* samples, uint16_t count) {
    String msg;
    msg.reserve(7 + 6 * count);
    msg += "{\"tap\":";
    msg += String(tap_num);
    msg += ",\"samples\":[";
    for (uint16_t i = 0; i < count; i++) {
        if (i > 0) msg += ',';
        msg += String(samples[i]);
    }
    msg += "]}";
    sendJSON(msg);
}

static void handleCommand(const String& cmd) {
    if (cmd == "PING") {
        sendJSON("{\"status\":\"pong\"}");

    } else if (cmd == "ARM") {
        tap.arm();
        app_state = ARMED;
        done_sent = false;
        digitalWrite(LED_PIN, HIGH);
        sendJSON("{\"status\":\"armed\"}");

    } else if (cmd == "CAPTURE") {
        if (app_state == WAIT_CAPTURE) {
            String b64 = captureBase64();
            if (b64.length() > 0) {
                sendJSON("{\"photo\":\"" + b64 + "\"}");
            } else {
                sendJSON("{\"error\":\"capture_failed\"}");
            }
        }
    }
}

// ─── Setup / Loop ───────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    done_sent = false;

    tap.begin();

    if (!initCamera()) {
        sendJSON("{\"error\":\"camera_init_failed\"}");
    }

    sendJSON("{\"status\":\"ready\"}");
    ledBlink(2, 80);
}

void loop() {
    // ── Serial RX ──
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

    // ── Tap detection ──
    if (app_state == ARMED) {
        tap.update();

        // Send each newly-completed tap immediately
        while (tap.tapReady()) {
            uint8_t idx = tap.getTapCount() - 1;
            sendTapJSON(idx + 1, tap.getTap(idx), SAMPLE_COUNT);
        }

        // Once all taps are captured and sent, signal done
        if (!done_sent && tap.getTapCount() >= MAX_TAPS) {
            sendJSON("{\"status\":\"done\",\"tap_count\":" + String(MAX_TAPS) + "}");
            app_state = WAIT_CAPTURE;
            done_sent = true;
            digitalWrite(LED_PIN, LOW);
        }
    }
}
