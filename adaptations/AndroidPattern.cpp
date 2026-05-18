#include "AndroidPattern.h"
#include <ArduinoJson.h>

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "utils/PatternMath.h"

namespace {
    double smoothstep(double edge0, double edge1, double x) {
        if (edge0 == edge1) {
            return x < edge0 ? 0.0 : 1.0;
        }
        double t = (x - edge0) / (edge1 - edge0);
        t = clamp01(t);
        return t * t * (3.0 - 2.0 * t);
    }

    double wrap01(double value) {
        value = fmod(value, 1.0);
        if (value < 0.0) {
            value += 1.0;
        }
        return value;
    }
}

AndroidPattern::AndroidPattern() {
    speed = 1.0;
    minArcLength = 0.08;
    maxArcLength = 0.45;
    rotation = 0.0;
    growthPhase = 0.0;
    lastMillis = 0;
}

AndroidPattern::AndroidPattern(String configPath) {
    this->configFile = configPath;
    speed = 1.0;
    minArcLength = 0.08;
    maxArcLength = 0.45;
    rotation = 0.0;
    growthPhase = 0.0;
    lastMillis = 0;
}

void AndroidPattern::updateFromJson(DynamicJsonDocument *json) {
    AbstractPattern::updateFromJson(json);
    DynamicJsonDocument doc = *json;

    if (doc.containsKey("speed")) {
        speed = doc["speed"].as<double>();
    }
    if (speed <= 0.0) {
        speed = 0.1;
    }

    if (doc.containsKey("min_arc")) {
        minArcLength = doc["min_arc"].as<double>();
    }
    if (minArcLength < 0.01) {
        minArcLength = 0.01;
    }

    if (doc.containsKey("max_arc")) {
        maxArcLength = doc["max_arc"].as<double>();
    }
    if (maxArcLength > 1.0) {
        maxArcLength = 1.0;
    }

    if (maxArcLength < minArcLength) {
        maxArcLength = minArcLength;
    }
}

void AndroidPattern::loop(long millis, LinkedList<LedRGB *> &leds) {
    AbstractPattern::loop(millis, leds);

    int ledCount = leds.size();
    if (ledCount <= 0) {
        return;
    }

    long delta = lastMillis == 0 ? 0 : (millis - lastMillis);
    lastMillis = millis;

    double deltaSeconds = (double)delta / 1000.0;
    double rotationSpeed = speed;
    double growthSpeed = speed * 0.75;

    rotation = wrap01(rotation + deltaSeconds * rotationSpeed);
    growthPhase = wrap01(growthPhase + deltaSeconds * growthSpeed);

    double arcProgress = (sin(2.0 * M_PI * growthPhase) + 1.0) * 0.5;
    double arcLength = minArcLength + (maxArcLength - minArcLength) * arcProgress;
    arcLength = clamp01(arcLength);

    if (arcLength <= 0.0) {
        for (int i = 0; i < ledCount; i++) {
            LedRGB* led = leds.get(i);
            if (!led) {
                continue;
            }
            led->r = 0;
            led->g = 0;
            led->b = 0;
        }
        return;
    }

    double start = rotation;
    double length = arcLength;

    double edgeFeather = length * 0.25;
    if (edgeFeather < 0.02) {
        edgeFeather = 0.02;
    }
    double maxFeather = length * 0.45;
    if (edgeFeather > maxFeather) {
        edgeFeather = maxFeather;
    }
    double featherRatio = 0.0;
    if (length > 0.0) {
        featherRatio = edgeFeather / length;
    }

    for (int i = 0; i < ledCount; i++) {
        LedRGB* led = leds.get(i);
        if (!led) {
            continue;
        }

        double position = (double)i / (double)ledCount;
        double relative = position - start;
        if (relative < 0.0) {
            relative += 1.0;
        }

        double brightness = 0.0;
        if (relative <= length) {
            double normalized = relative / length;
            double tail = smoothstep(0.0, featherRatio, normalized);
            double head = 1.0 - smoothstep(1.0 - featherRatio, 1.0, normalized);
            brightness = clamp01(tail * head);
        }

        led->r = (int)(baseColorR * brightness);
        led->g = (int)(baseColorG * brightness);
        led->b = (int)(baseColorB * brightness);
    }
}

String AndroidPattern::getBaseJson() const {
    StaticJsonDocument<192> doc;
    doc["name"] = "Android Progress Pattern";
    doc["type"] = "Android";
    doc["active"] = 1;
    doc["speed"] = 0.75;
    doc["min_arc"] = 0.05;
    doc["max_arc"] = 0.75;

    String output;
    serializeJson(doc, output);
    return output;
}
