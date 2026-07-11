#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>

struct SensorReading {
    float frequency;
    float amplitude;
    uint32_t timestamp;
};

SensorReading readPiezoSensor();
void printReading(const SensorReading &r);

#endif
