#include "Win8Pattern.h"
#include <ArduinoJson.h>
#include <math.h>

#include "utils/PatternMath.h"

namespace {
    double easeInOutCubic(double t) {
        if (t < 0.5) {
            return 4.0 * t * t * t;
        }
        double f = -2.0 * t + 2.0;
        return 1.0 - (f * f * f) / 2.0;
    }
}

Win8Pattern::Win8Pattern() {
    dotCount = 5;
    secondsPerCycle = 1.8;
    dotRadius = 0.09;
    trailPower = 2.4;
    minimumBrightness = 0.05;
    tailBrightness = 0.35;
    phase = 0.0;
    startOffset = 0.0;
    offsetMoveSpeed = 0.05;
    lastMillis = 0;
}

Win8Pattern::Win8Pattern(String configPath) {
    this->configFile = configPath;
    dotCount = 5;
    secondsPerCycle = 1.8;
    dotRadius = 0.09;
    trailPower = 2.4;
    minimumBrightness = 0.05;
    tailBrightness = 0.35;
    phase = 0.0;
    startOffset = 0.0;
    offsetMoveSpeed = 0.05;
    lastMillis = 0;
}

void Win8Pattern::updateFromJson(DynamicJsonDocument *json) {
    AbstractPattern::updateFromJson(json);
    DynamicJsonDocument doc = *json;

    if (doc.containsKey("dot_count")) {
        dotCount = doc["dot_count"].as<int>();
    }
    if (dotCount < 1) {
        dotCount = 1;
    }

    if (doc.containsKey("seconds_per_cycle")) {
        secondsPerCycle = doc["seconds_per_cycle"].as<double>();
    }
    if (secondsPerCycle <= 0.0) {
        secondsPerCycle = 1.0;
    }

    if (doc.containsKey("dot_radius")) {
        dotRadius = doc["dot_radius"].as<double>();
    }
    if (dotRadius <= 0.0) {
        dotRadius = 0.01;
    }

    if (doc.containsKey("trail_power")) {
        trailPower = doc["trail_power"].as<double>();
    }
    if (trailPower < 1.0) {
        trailPower = 1.0;
    }

    if (doc.containsKey("minimum_brightness")) {
        minimumBrightness = doc["minimum_brightness"].as<double>();
    }
    minimumBrightness = clamp01(minimumBrightness);

    if (doc.containsKey("tail_brightness")) {
        tailBrightness = doc["tail_brightness"].as<double>();
    }
    tailBrightness = clamp01(tailBrightness);

    if (doc.containsKey("offset_move_speed")) {
        offsetMoveSpeed = doc["offset_move_speed"].as<double>();
    }
    if (offsetMoveSpeed < 0.0) {
        offsetMoveSpeed = 0.0;
    }
}

void Win8Pattern::loop(long millis, LinkedList<LedRGB*> &leds) {
    AbstractPattern::loop(millis, leds);

    int ledCount = leds.size();
    if (ledCount <= 0) {
        return;
    }

    long delta = lastMillis == 0 ? 0 : (millis - lastMillis);
    lastMillis = millis;

    double deltaSeconds = (double)delta / 1000.0;
    double cyclesPerSecond = 1.0 / secondsPerCycle;
    phase += deltaSeconds * cyclesPerSecond;
    phase = fmod(phase, 1.0);
    if (phase < 0.0) {
        phase += 1.0;
    }

    startOffset += deltaSeconds * offsetMoveSpeed;
    startOffset = fmod(startOffset, 1.0);
    if (startOffset < 0.0) {
        startOffset += 1.0;
    }

    for (int i = 0; i < ledCount; i++) {
        LedRGB *led = leds.get(i);
        if (!led) {
            continue;
        }

        double position = (double)i / (double)ledCount;
        double brightness = minimumBrightness;

        for (int dotIndex = 0; dotIndex < dotCount; dotIndex++) {
            double dotProgress = fmod(phase + (double)dotIndex / (double)dotCount, 1.0);
            double easedPosition = easeInOutCubic(dotProgress);
            double dotPosition = fmod(easedPosition + startOffset, 1.0);
            if (dotPosition < 0.0) {
                dotPosition += 1.0;
            }

            double distance = fabs(position - dotPosition);
            if (distance > 0.5) {
                distance = 1.0 - distance;
            }

            double normalized = dotRadius > 0.0 ? distance / dotRadius : distance;
            if (normalized < 1.0) {
                double intensity = pow(1.0 - normalized, trailPower);

                double dotStrength = 1.0;
                if (dotCount > 1) {
                    double fraction = (double)dotIndex / (double)(dotCount - 1);
                    dotStrength = tailBrightness + (1.0 - tailBrightness) * (1.0 - fraction);
                }

                double dotBrightness = minimumBrightness + (1.0 - minimumBrightness) * intensity * dotStrength;
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

String Win8Pattern::getBaseJson() const {
    StaticJsonDocument<256> doc;
    doc["name"] = "Win8 Loading Pattern";
    doc["type"] = "Win8";
    doc["active"] = 1;
    doc["dot_count"] = 5;
    doc["seconds_per_cycle"] = 1.8;
    doc["dot_radius"] = 0.09;
    doc["trail_power"] = 2.4;
    doc["minimum_brightness"] = 0.05;
    doc["tail_brightness"] = 0.35;
    doc["offset_move_speed"] = 0.05;

    String output;
    serializeJson(doc, output);
    return output;
}
