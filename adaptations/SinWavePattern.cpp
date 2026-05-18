#include "SinWavePattern.h"
#include <ArduinoJson.h>
#include <math.h>

SinWavePattern::SinWavePattern() {
    amplitude = 0.5;
    offset = 0.5;
    wavelength = 10.0;
    speed = 1.0;
    direction = 1;
    phase = 0.0;
    lastMillis = 0;
}

SinWavePattern::SinWavePattern(String configPath) {
    this->configFile = configPath;
    amplitude = 0.5;
    offset = 0.5;
    wavelength = 10.0;
    speed = 1.0;
    direction = 1;
    phase = 0.0;
    lastMillis = 0;
}

void SinWavePattern::updateFromJson(DynamicJsonDocument *json) {
    AbstractPattern::updateFromJson(json);
    DynamicJsonDocument doc = *json;

    if (doc.containsKey("amplitude")) {
        amplitude = doc["amplitude"].as<double>();
    }
    if (amplitude < 0) amplitude = 0;
    if (amplitude > 1) amplitude = 1;

    if (doc.containsKey("offset")) {
        offset = doc["offset"].as<double>();
    }
    if (offset < 0) offset = 0;
    if (offset > 1) offset = 1;

    if (doc.containsKey("wavelength")) {
        wavelength = doc["wavelength"].as<double>();
    }
    if (wavelength <= 0) {
        wavelength = 1.0;
    }

    if (doc.containsKey("speed")) {
        speed = doc["speed"].as<double>();
    }
    if (speed < 0) {
        speed = -speed;
    }

    if (doc.containsKey("phase")) {
        phase = doc["phase"].as<double>();
    }

    if (doc.containsKey("direction")) {
        int d = doc["direction"].as<int>();
        direction = (d == 0) ? 1 : -1;
    }
}

void SinWavePattern::loop(long millis, LinkedList<LedRGB*> &leds) {
    AbstractPattern::loop(millis, leds);

    int ledCount = leds.size();
    if (ledCount == 0) {
        return;
    }

    long delta = lastMillis == 0 ? 0 : (millis - lastMillis);
    lastMillis = millis;

    double deltaSeconds = (double)delta / 1000.0;
    double angularVelocity = speed * 2.0 * PI;
    phase += direction * angularVelocity * deltaSeconds;

    const double twoPi = 2.0 * PI;
    while (phase > twoPi) {
        phase -= twoPi;
    }
    while (phase < -twoPi) {
        phase += twoPi;
    }

    for (int i = 0; i < ledCount; i++) {
        LedRGB *led = leds.get(i);
        if (!led) {
            continue;
        }

        double positionPhase = ((double)i / wavelength) * 2.0 * PI;
        double brightness = offset + amplitude * sin(positionPhase + phase);

        if (brightness < 0.0) brightness = 0.0;
        if (brightness > 1.0) brightness = 1.0;

        led->r = (int)(baseColorR * brightness);
        led->g = (int)(baseColorG * brightness);
        led->b = (int)(baseColorB * brightness);
    }
}

String SinWavePattern::getBaseJson() const {
    StaticJsonDocument<256> doc;
    doc["name"] = "Sin Wave Pattern";
    doc["type"] = "SinWave";
    doc["active"] = 1;
    doc["amplitude"] = 0.5;
    doc["offset"] = 0.5;
    doc["wavelength"] = 6;
    doc["speed"] = 0.5;
    doc["direction"] = 0;
    doc["phase"] = 0;

    String output;
    serializeJson(doc, output);
    return output;
}
