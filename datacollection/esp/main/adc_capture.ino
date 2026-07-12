#include "adc_capture.h"

bool ADCCapture::begin() {
    adc1_config_width(ADC_WIDTH_BIT_12);
    esp_err_t err = adc1_config_channel_atten(PIEZO_CHANNEL, ADC_ATTEN_DB_11);
    _ok = (err == ESP_OK);
    return _ok;
}

uint32_t ADCCapture::capture(uint16_t* buffer, uint32_t count) {
    if (!_ok) return 0;

    uint32_t start = micros();
    uint32_t pos = 0;
    while (pos < count) {
        buffer[pos++] = (uint16_t)adc1_get_raw(PIEZO_CHANNEL);
    }
    uint32_t elapsed = micros() - start;
    _actualRate = (elapsed > 0) ? (float)count / (float)elapsed * 1000000.0f : 0;
    return pos;
}
