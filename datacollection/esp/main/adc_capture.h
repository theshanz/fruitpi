#ifndef ADC_CAPTURE_H
#define ADC_CAPTURE_H

#include <Arduino.h>
#include "driver/adc.h"

#define PIEZO_CHANNEL    ADC1_CHANNEL_0   // GPIO1 on ESP32-S3
#define SAMPLE_RATE_HZ   80000   // target 80 kHz
#define SAMPLE_COUNT     4096    // 51.2 ms window

class ADCCapture {
public:
    bool begin();
    uint32_t capture(uint16_t* buffer, uint32_t count = SAMPLE_COUNT);
    float lastActualRate() const { return _actualRate; }

private:
    bool _ok;
    float _actualRate = 0;
};

#endif
