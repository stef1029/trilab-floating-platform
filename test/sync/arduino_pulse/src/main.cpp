#include <Arduino.h>

constexpr uint8_t EVENT_PIN = 4;

constexpr uint32_t MIN_INTERVAL_MS = 500;
constexpr uint32_t MAX_INTERVAL_MS = 3000;
constexpr uint32_t PULSE_WIDTH_US = 250;

static uint32_t event_sequence = 0;

void setup()
{
    Serial.begin(115200);

    pinMode(EVENT_PIN, OUTPUT);
    digitalWrite(EVENT_PIN, LOW);

    delay(1000);

    randomSeed(
        static_cast<unsigned long>(analogRead(A7)) ^
        static_cast<unsigned long>(micros())
    );

    Serial.println("EVENT_GENERATOR_READY");
}

void loop()
{
    const uint32_t wait_ms =
        static_cast<uint32_t>(
            random(
                MIN_INTERVAL_MS,
                MAX_INTERVAL_MS + 1
            )
        );

    delay(wait_ms);

    digitalWrite(EVENT_PIN, HIGH);

    event_sequence++;

    const uint32_t generated_us = micros();

    delayMicroseconds(PULSE_WIDTH_US);
    digitalWrite(EVENT_PIN, LOW);

    Serial.print("EVENT_GEN,");
    Serial.print(event_sequence);
    Serial.print(",");
    Serial.print(generated_us);
    Serial.print(",");
    Serial.println(wait_ms);
}