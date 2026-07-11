#include <Arduino.h>
#include "sensor.h"

#define LED_PIN 47

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    if (psramInit()) {
        Serial.println("PSRAM initialized successfully!");
    }
}

void loop() {
    digitalWrite(LED_PIN, HIGH);

    SensorReading r = readPiezoSensor();
    printReading(r);

    delay(1000);
    digitalWrite(LED_PIN, LOW);
    delay(1000);
}
