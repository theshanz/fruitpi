SensorReading readPiezoSensor() {
    SensorReading r;
    r.frequency = analogRead(4) * 1.0;
    r.amplitude = analogRead(5) * 1.0;
    r.timestamp = millis();
    return r;
}

void printReading(const SensorReading &r) {
    Serial.printf("Freq: %.1f Hz  Amp: %.1f  t=%lu\n",
                  r.frequency, r.amplitude, r.timestamp);
}
