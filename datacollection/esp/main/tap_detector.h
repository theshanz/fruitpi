#ifndef TAP_DETECTOR_H
#define TAP_DETECTOR_H

#include <Arduino.h>

#define PIEZO_PIN        1     // ADC1_CH0 (GPIO 1) — change to match wiring
#define SAMPLE_COUNT     1024
#define SAMPLE_RATE_HZ   16000
#define SAMPLE_DELAY_US  62    // ~16kHz (1/16000 = 62.5us)
#define TAP_THRESHOLD    100   // ADC counts above/below noise floor
#define NOISE_WINDOW     64    // Sliding window for noise floor
#define COOLDOWN_MS      300   // Debounce dead-time after each tap
#define MAX_TAPS         3

enum TapState {
    TAP_IDLE,
    TAP_MONITORING,
    TAP_CAPTURING,
    TAP_COOLDOWN
};

class TapDetector {
public:
    void begin();
    void arm();
    void update();

    bool isArmed()        const { return _state == TAP_MONITORING; }
    uint8_t getTapCount() const { return _tap_count; }

    // Returns true once per newly-completed tap.  Caller should
    // read getTap(newTapCount()-1) after this returns true.
    bool tapReady();

    const uint16_t* getTap(uint8_t idx) const {
        return (idx < MAX_TAPS) ? _tap_buffer[idx] : nullptr;
    }

    void reset();

private:
    TapState _state;
    uint16_t _tap_buffer[MAX_TAPS][SAMPLE_COUNT];
    uint32_t _sample_index;
    uint8_t  _tap_count;
    uint8_t  _tap_sent;       // how many taps already reported via tapReady()
    uint32_t _cooldown_start;

    uint16_t _noise_buf[NOISE_WINDOW];
    uint8_t  _noise_idx;
    uint32_t _noise_sum;

    uint16_t _readADC();
    uint16_t _noiseFloor() const;
    void     _pushNoise(uint16_t sample);
};

#endif
