#include "tap_detector.h"

void TapDetector::begin() {
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);
    pinMode(PIEZO_PIN, INPUT);
    reset();
}

void TapDetector::arm() {
    reset();
    _state = TAP_MONITORING;
}

void TapDetector::reset() {
    _state = TAP_IDLE;
    _sample_index = 0;
    _tap_count = 0;
    _tap_sent = 0;
    _cooldown_start = 0;
    memset(_tap_buffer, 0, sizeof(_tap_buffer));
    memset(_noise_buf, 0, sizeof(_noise_buf));
    _noise_idx = 0;
    _noise_sum = 0;
}

uint16_t TapDetector::_readADC() {
    return (uint16_t)analogRead(PIEZO_PIN);
}

uint16_t TapDetector::_noiseFloor() const {
    return (uint16_t)(_noise_sum / NOISE_WINDOW);
}

void TapDetector::_pushNoise(uint16_t sample) {
    _noise_sum -= _noise_buf[_noise_idx];
    _noise_buf[_noise_idx] = sample;
    _noise_sum += sample;
    _noise_idx = (_noise_idx + 1) % NOISE_WINDOW;
}

bool TapDetector::tapReady() {
    if (_tap_sent < _tap_count) {
        _tap_sent++;
        return true;
    }
    return false;
}

void TapDetector::update() {
    switch (_state) {
        case TAP_IDLE:
            break;

        case TAP_COOLDOWN:
            if (millis() - _cooldown_start >= COOLDOWN_MS) {
                if (_tap_count >= MAX_TAPS) {
                    _state = TAP_IDLE;
                } else {
                    _state = TAP_MONITORING;
                }
            }
            break;

        case TAP_MONITORING: {
            uint16_t raw = _readADC();
            _pushNoise(raw);

            int32_t baseline = (int32_t)_noiseFloor();
            int32_t deviation = (int32_t)raw - baseline;
            if (deviation < 0) deviation = -deviation;

            if (deviation > TAP_THRESHOLD) {
                _sample_index = 0;
                _state = TAP_CAPTURING;
            }
            break;
        }

        case TAP_CAPTURING: {
            uint32_t start_us = micros();

            while (_sample_index < SAMPLE_COUNT) {
                _tap_buffer[_tap_count][_sample_index] = _readADC();
                _sample_index++;

                uint32_t target = _sample_index * SAMPLE_DELAY_US;
                uint32_t elapsed = micros() - start_us;
                if (elapsed < target) {
                    delayMicroseconds(target - elapsed);
                }
            }

            _tap_count++;
            _cooldown_start = millis();
            _state = TAP_COOLDOWN;
            break;
        }
    }
}
