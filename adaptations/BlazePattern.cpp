#include "BlazePattern.h"
#include <ArduinoJson.h>

BlazePattern::BlazePattern() {
    lastUpdateMillis = 0;
    deviationMin = 0;
    deviationMax = 15;
    fadeSpeedMin = 500;
    fadeSpeedMax = 2000;
}

BlazePattern::BlazePattern(String configPath) {
    this->configFile = configPath;
    lastUpdateMillis = 0;
    deviationMin = 0;
    deviationMax = 15;
    fadeSpeedMin = 500;
    fadeSpeedMax = 2000;
}

void BlazePattern::ensureStateSize(int size) {
    int currentSize = states.size();
    if (currentSize < size) {
        states.resize(size);
        for (int i = currentSize; i < size; i++) {
            states[i].initialized = false;
            states[i].elapsedMs = 0;
            states[i].durationMs = 1;
            states[i].startR = baseColorR;
            states[i].startG = baseColorG;
            states[i].startB = baseColorB;
            states[i].targetR = baseColorR;
            states[i].targetG = baseColorG;
            states[i].targetB = baseColorB;
        }
    }
}

String BlazePattern::getBaseJson() const {
    StaticJsonDocument<192> doc;
    doc["name"] = "Blaze Pattern";
    doc["type"] = "Blaze";
    doc["active"] = 0;
    doc["deviationMin"] = 5;
    doc["deviationMax"] = 40;
    doc["fadeSpeedMin"] = 500;
    doc["fadeSpeedMax"] = 2500;

    String output;
    serializeJson(doc, output);
    return output;
}

void BlazePattern::resetState(LedState &state) {
    state.startR = baseColorR;
    state.startG = baseColorG;
    state.startB = baseColorB;
    state.targetR = baseColorR;
    state.targetG = baseColorG;
    state.targetB = baseColorB;
    state.durationMs = 1;
    state.elapsedMs = 0;
    state.initialized = true;
    pickNewTarget(state);
}

int BlazePattern::randomDeviation() const {
    if (deviationMax < deviationMin) {
        return deviationMin;
    }
    if (deviationMax == deviationMin) {
        return deviationMin;
    }
    return random(deviationMin, deviationMax + 1);
}

int BlazePattern::clampColorComponent(int value) const {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return value;
}

void BlazePattern::pickNewTarget(LedState &state) {
    int deviation = randomDeviation();
    int rOffset = deviation == 0 ? 0 : random(-deviation, deviation + 1);
    int gOffset = deviation == 0 ? 0 : random(-deviation, deviation + 1);
    int bOffset = deviation == 0 ? 0 : random(-deviation, deviation + 1);

    state.startR = state.targetR;
    state.startG = state.targetG;
    state.startB = state.targetB;

    state.targetR = clampColorComponent(baseColorR + rOffset);
    state.targetG = clampColorComponent(baseColorG + gOffset);
    state.targetB = clampColorComponent(baseColorB + bOffset);

    state.durationMs = random(fadeSpeedMin, fadeSpeedMax + 1);
    if (state.durationMs <= 0) {
        state.durationMs = 1;
    }
    state.elapsedMs = 0;
}

void BlazePattern::updateFromJson(DynamicJsonDocument *json) {
    AbstractPattern::updateFromJson(json);
    DynamicJsonDocument doc = *json;
    deviationMin = doc["deviationMin"].as<int>();
    deviationMax = doc["deviationMax"].as<int>();
    fadeSpeedMin = doc["fadeSpeedMin"].as<int>();
    fadeSpeedMax = doc["fadeSpeedMax"].as<int>();

    if (deviationMin < 0) {
        deviationMin = 0;
    }
    if (deviationMax < 0) {
        deviationMax = 0;
    }
    if (deviationMin > 255) {
        deviationMin = 255;
    }
    if (deviationMax > 255) {
        deviationMax = 255;
    }
    if (deviationMax < deviationMin) {
        int tmp = deviationMin;
        deviationMin = deviationMax;
        deviationMax = tmp;
    }

    if (fadeSpeedMin < 1) {
        fadeSpeedMin = 1;
    }
    if (fadeSpeedMax < 1) {
        fadeSpeedMax = 1;
    }
    if (fadeSpeedMax < fadeSpeedMin) {
        int tmp = fadeSpeedMin;
        fadeSpeedMin = fadeSpeedMax;
        fadeSpeedMax = tmp;
    }

    for (auto &state : states) {
        state.initialized = false;
    }
}

void BlazePattern::loop(long millis, LinkedList<LedRGB*> &leds) {
    AbstractPattern::loop(millis, leds);
    int size = leds.size();
    ensureStateSize(size);

    if (lastUpdateMillis == 0) {
        lastUpdateMillis = millis;
    }
    long delta = millis - lastUpdateMillis;
    if (delta < 0) {
        delta = 0;
    }

    for (int i = 0; i < size; i++) {
        LedRGB *led = leds.get(i);
        if (!led) {
            continue;
        }

        LedState &state = states[i];
        if (!state.initialized) {
            resetState(state);
        }

        state.elapsedMs += delta;
        if (state.elapsedMs > state.durationMs) {
            state.elapsedMs = state.durationMs;
        }

        float progress = state.durationMs > 0 ? (float)state.elapsedMs / (float)state.durationMs : 1.0f;
        if (progress > 1.0f) {
            progress = 1.0f;
        }

        float currentR = state.startR + (state.targetR - state.startR) * progress;
        float currentG = state.startG + (state.targetG - state.startG) * progress;
        float currentB = state.startB + (state.targetB - state.startB) * progress;

        led->r = (int)currentR;
        led->g = (int)currentG;
        led->b = (int)currentB;

        if (progress >= 1.0f) {
            pickNewTarget(state);
        }
    }

    lastUpdateMillis = millis;
}
