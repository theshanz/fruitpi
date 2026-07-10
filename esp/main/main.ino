#include <Arduino.h>
#include <wireless.h>

#define LED_PIN 47

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    if (psramInit()) {
        Serial.println("PSRAM initialized successfully!");
    }
}

void loop() {
    Serial.println("FruitPi System Heartbeat...");
    digitalWrite(LED_PIN, HIGH);
    delay(1000);
    digitalWrite(LED_PIN, LOW);
    delay(1000);
}
