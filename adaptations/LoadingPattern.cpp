#include "LoadingPattern.h"
#include <ArduinoJson.h>
#include <math.h>

#include "utils/PatternMath.h"

LoadingPattern::LoadingPattern() {
    dotCount = 5;
    secondsPerRotation = 1.2;
    dotWidth = 0.12;
    trailExponent = 2.5;
    minimumBrightness = 0.05;
    rotation = 0.0;
    lastMillis = 0;
}

LoadingPattern::LoadingPattern(String configPath) {
    this->configFile = configPath;
    dotCount = 5;
    secondsPerRotation = 1.2;
    dotWidth = 0.12;
    trailExponent = 2.5;
    minimumBrightness = 0.05;
    rotation = 0.0;
    lastMillis = 0;
}

void LoadingPattern::updateFromJson(DynamicJsonDocument *json) {
    AbstractPattern::updateFromJson(json);
    DynamicJsonDocument doc = *json;

    if (doc.containsKey("dot_count")) {
        dotCount = doc["dot_count"].as<int>();
    }
    if (dotCount < 1) {
        dotCount = 1;
    }

    if (doc.containsKey("seconds_per_rotation")) {
        secondsPerRotation = doc["seconds_per_rotation"].as<double>();
    }
    if (secondsPerRotation <= 0.0) {
        secondsPerRotation = 1.0;
    }

    if (doc.containsKey("dot_width")) {
        dotWidth = doc["dot_width"].as<double>();
    }
    if (dotWidth <= 0.0) {
        dotWidth = 0.01;
    }

    if (doc.containsKey("trail_exponent")) {
        trailExponent = doc["trail_exponent"].as<double>();
    }
    if (trailExponent < 1.0) {
        trailExponent = 1.0;
    }

    if (doc.containsKey("minimum_brightness")) {
        minimumBrightness = doc["minimum_brightness"].as<double>();
    }
    minimumBrightness = clamp01(minimumBrightness);
}

void LoadingPattern::loop(long millis, LinkedList<LedRGB*> &leds) {
    AbstractPattern::loop(millis, leds);

    int ledCount = leds.size();
    if (ledCount <= 0) {
        return;
    }

    long delta = lastMillis == 0 ? 0 : (millis - lastMillis);
    lastMillis = millis;

    double deltaSeconds = (double)delta / 1000.0;
    double cyclesPerSecond = 1.0 / secondsPerRotation;
    rotation += deltaSeconds * cyclesPerSecond;

    rotation = fmod(rotation, 1.0);
    if (rotation < 0.0) {
        rotation += 1.0;
    }

    for (int i = 0; i < ledCount; i++) {
        LedRGB* led = leds.get(i);
        if (!led) {
            continue;
        }

        double position = (double)i / (double)ledCount;
        double brightness = minimumBrightness;

        for (int dotIndex = 0; dotIndex < dotCount; dotIndex++) {
            double dotPosition = rotation + ((double)dotIndex / (double)dotCount);
            dotPosition = fmod(dotPosition, 1.0);

            double distance = fabs(position - dotPosition);
            if (distance > 0.5) {
                distance = 1.0 - distance;
            }

            double normalized = 0.0;
            if (dotWidth > 0.0) {
                normalized = distance / dotWidth;
            }

            if (normalized < 1.0) {
                double intensity = pow(1.0 - normalized, trailExponent);
                double dotBrightness = minimumBrightness + (1.0 - minimumBrightness) * intensity;
                if (dotBrightness > brightness) {
                    brightness = dotBrightness;
                }
            }
        }

        brightness = clamp01(brightness);

        led->r = (int)(baseColorR * brightness);
        led->g = (int)(baseColorG * brightness);
        led->b = (int)(baseColorB * brightness);
    }
}

String LoadingPattern::getBaseJson() const {
    StaticJsonDocument<256> doc;
    doc["name"] = "Loading Pattern";
    doc["type"] = "Loading";
    doc["active"] = 1;
    doc["dot_count"] = 5;
    doc["seconds_per_rotation"] = 1.2;
    doc["dot_width"] = 0.12;
    doc["trail_exponent"] = 2.5;
    doc["minimum_brightness"] = 0.15;

    String output;
    serializeJson(doc, output);
    return output;
}
