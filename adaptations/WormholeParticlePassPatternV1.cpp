#include "WormholeParticlePassPatternV1.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>

namespace {
    constexpr float TWO_PI_F_PARTICLE_PASS = 6.28318530718f;
    constexpr float MIN_SPEED_FALLBACK = 0.05f;
    constexpr float MIN_SIZE_FALLBACK = 0.5f;
    constexpr float MIN_APPROACH_RATE = 0.35f;
    constexpr float MIN_FADE_RATE = 0.1f;
    constexpr float APPROACH_RATE_MULTIPLIER = 2.0f;
    constexpr float REMOVAL_EPSILON = 0.001f;

    void clearVirtualBuffer(LedRGB *buffer, int count) {
        if (!buffer || count <= 0) {
            return;
        }

        for (int i = 0; i < count; ++i) {
            buffer[i].r = 0;
            buffer[i].g = 0;
            buffer[i].b = 0;
        }
    }
}

WormholeParticlePassPatternV1::WormholeParticlePassPatternV1() : AbstractWormholePatternV1() {
    enforceFixedConfiguration();
}

WormholeParticlePassPatternV1::WormholeParticlePassPatternV1(String configPath) : AbstractWormholePatternV1(configPath) {
    enforceFixedConfiguration();
}

void WormholeParticlePassPatternV1::updateFromJson(DynamicJsonDocument *json) {
    if (!json) {
        return;
    }

    AbstractPattern::updateFromJson(json);
    DynamicJsonDocument doc = *json;

    // This pattern only supports the mirror/opposite layout. Allowing
    // configuration to override these values produces broken visuals, so the
    // configuration is enforced every time we refresh.
    enforceFixedConfiguration();

    resetLedDirection();
    resetStartOffset();
    applyLedDirectionFromJson(doc);
    applyStartOffsetFromJson(doc);

    if (doc.containsKey("density")) {
        density = doc["density"].as<float>();
    }
    if (doc.containsKey("minSpeed")) {
        minSpeed = doc["minSpeed"].as<float>();
    }
    if (doc.containsKey("maxSpeed")) {
        maxSpeed = doc["maxSpeed"].as<float>();
    }
    if (doc.containsKey("minSize")) {
        minSize = doc["minSize"].as<float>();
    }
    if (doc.containsKey("maxSize")) {
        maxSize = doc["maxSize"].as<float>();
    }
    if (doc.containsKey("minAngle")) {
        minAngle = doc["minAngle"].as<float>();
    }
    if (doc.containsKey("maxAngle")) {
        maxAngle = doc["maxAngle"].as<float>();
    }

    if (density < 0.0f) {
        density = 0.0f;
    }

    if (minSpeed < 0.0f) {
        minSpeed = 0.0f;
    }
    if (maxSpeed < minSpeed) {
        maxSpeed = minSpeed;
    }

    if (minSize < 0.0f) {
        minSize = 0.0f;
    }
    if (maxSize < minSize) {
        maxSize = minSize;
    }

    if (maxAngle < minAngle) {
        float tmp = maxAngle;
        maxAngle = minAngle;
        minAngle = tmp;
    }

    resetState();
}

void WormholeParticlePassPatternV1::enforceFixedConfiguration() {
    resetType();
    setType(TYPE_LINE);
    direction = DIRECTION_OPPOSITE;
}

void WormholeParticlePassPatternV1::loop(long millis, LinkedList<LedRGB*> &leds) {
    AbstractPattern::loop(millis, leds);

    int total = leds.size();
    if (total <= 0) {
        return;
    }

    AbstractWormholePatternV1::clearLedList(leds);

    int firstCount = total / 2;
    int secondCount = total - firstCount;
    int secondStart = firstCount;
    WormholeType mappingType = getType();
    int virtualCount;
    if (mappingType == TYPE_LINE) {
        virtualCount = firstCount + secondCount;
    } else {
        virtualCount = firstCount;
        if (secondCount < virtualCount) {
            virtualCount = secondCount;
        }
    }

    ensureVirtualBuffer(virtualCount);
    ensureAuxBuffers(virtualLedCount);
    if (virtualLedCount <= 0) {
        return;
    }

    clearVirtualBuffer(virtualLedBuffer, virtualLedCount);

    if (lastMillis < 0) {
        lastMillis = millis;
    }
    long deltaMs = millis - lastMillis;
    if (deltaMs < 0) {
        deltaMs = 0;
    }
    lastMillis = millis;

    float deltaSeconds = static_cast<float>(deltaMs) / 1000.0f;
    updateParticles(deltaSeconds, virtualLedCount);

    int desiredCount = static_cast<int>(density);
    float fractional = density - static_cast<float>(desiredCount);
    if (fractional > 0.0f) {
        long roll = random(0, 1000);
        if (roll < static_cast<long>(fractional * 1000.0f)) {
            desiredCount += 1;
        }
    }
    if (desiredCount < 0) {
        desiredCount = 0;
    }

    while (static_cast<int>(particles.size()) < desiredCount) {
        spawnParticle(virtualLedCount);
        if (particles.size() > 1000) {
            break;
        }
    }
    while (static_cast<int>(particles.size()) > desiredCount && !particles.empty()) {
        particles.erase(particles.begin());
    }

    renderParticles();
    if (mappingType == TYPE_LINE) {
        mapLineMode(leds, firstCount, secondCount, secondStart);
    } else {
        mapMirrorMode(leds, firstCount, secondCount, secondStart);
        applyLowerRing(leds, firstCount, secondCount, secondStart);
    }
}

void WormholeParticlePassPatternV1::resetState() {
    particles.clear();
    lastMillis = -1;
    upperBrightnessLevels.clear();
    lowerBrightnessLevels.clear();
    lowerLedBuffer.clear();
}

void WormholeParticlePassPatternV1::spawnParticle(int totalCount) {
    if (totalCount <= 0) {
        return;
    }

    Particle particle{};
    float usedMinSpeed = std::max(minSpeed, 0.0f);
    float usedMaxSpeed = std::max(maxSpeed, usedMinSpeed);
    particle.speed = randomFloat(usedMinSpeed, usedMaxSpeed);
    if (particle.speed < MIN_SPEED_FALLBACK) {
        particle.speed = MIN_SPEED_FALLBACK;
    }

    float usedMinSize = std::max(minSize, 0.0f);
    float usedMaxSize = std::max(maxSize, usedMinSize);
    particle.size = randomFloat(usedMinSize, usedMaxSize);
    if (particle.size < MIN_SIZE_FALLBACK) {
        particle.size = MIN_SIZE_FALLBACK;
    }

    float usedMinAngle = minAngle;
    float usedMaxAngle = maxAngle;
    if (usedMaxAngle < usedMinAngle) {
        usedMaxAngle = usedMinAngle;
    }
    float chosenAngle = randomFloat(usedMinAngle, usedMaxAngle);
    float normalizedAngle = std::fmod(chosenAngle, TWO_PI_F_PARTICLE_PASS);
    if (normalizedAngle < 0.0f) {
        normalizedAngle += TWO_PI_F_PARTICLE_PASS;
    }
    float positionFactor = (TWO_PI_F_PARTICLE_PASS <= 0.0f)
        ? 0.0f
        : (normalizedAngle / TWO_PI_F_PARTICLE_PASS);
    int ledIndex = static_cast<int>(std::floor(positionFactor * static_cast<float>(totalCount)));
    if (ledIndex < 0) {
        ledIndex = 0;
    }
    if (ledIndex >= totalCount) {
        ledIndex = totalCount - 1;
    }
    particle.ledIndex = ledIndex;

    particle.verticalDirection = (random(0, 2) == 0) ? 1 : -1;
    particle.approachProgress = 0.0f;
    particle.fadeProgress = 0.0f;
    particle.secondaryFadeProgress = 0.0f;
    particle.reachedPeak = false;
    particle.secondaryFadeStarted = false;
    particle.upperIntensity = 0.0f;
    particle.lowerIntensity = 0.0f;

    particles.push_back(particle);
}

void WormholeParticlePassPatternV1::updateParticles(float deltaSeconds, int totalCount) {
    if (deltaSeconds <= 0.0f || totalCount <= 0) {
        return;
    }

    auto it = particles.begin();
    while (it != particles.end()) {
        Particle &particle = *it;
        if (particle.ledIndex < 0 || particle.ledIndex >= totalCount) {
            it = particles.erase(it);
            continue;
        }

        if (!particle.reachedPeak) {
            float approachRate = particle.speed * APPROACH_RATE_MULTIPLIER;
            if (approachRate < MIN_APPROACH_RATE) {
                approachRate = MIN_APPROACH_RATE;
            }
            particle.approachProgress += approachRate * deltaSeconds;
            float intensity = particle.approachProgress;
            if (intensity > 1.0f) {
                intensity = 1.0f;
            }

            float secondaryIntensity = 0.0f;
            if (intensity >= 0.5f) {
                secondaryIntensity = (intensity - 0.5f) * 2.0f;
                if (secondaryIntensity > 1.0f) {
                    secondaryIntensity = 1.0f;
                }
            }

            if (particle.verticalDirection >= 0) {
                particle.upperIntensity = intensity;
                particle.lowerIntensity = secondaryIntensity;
            } else {
                particle.lowerIntensity = intensity;
                particle.upperIntensity = secondaryIntensity;
            }

            if (particle.approachProgress >= 1.0f) {
                particle.reachedPeak = true;
                particle.approachProgress = 1.0f;
                particle.fadeProgress = 0.0f;
                particle.secondaryFadeProgress = 0.0f;
                particle.secondaryFadeStarted = false;
                if (particle.verticalDirection >= 0) {
                    particle.upperIntensity = 1.0f;
                    particle.lowerIntensity = 1.0f;
                } else {
                    particle.lowerIntensity = 1.0f;
                    particle.upperIntensity = 1.0f;
                }
            }
        }

        if (particle.reachedPeak) {
            float fadeRate = particle.speed * 0.5f;
            if (particle.size > 0.0f) {
                fadeRate /= particle.size;
            }
            if (fadeRate < MIN_FADE_RATE) {
                fadeRate = MIN_FADE_RATE;
            }
            particle.fadeProgress += fadeRate * deltaSeconds;
            float primaryIntensity = 1.0f - particle.fadeProgress;
            if (primaryIntensity < 0.0f) {
                primaryIntensity = 0.0f;
            }

            if (particle.verticalDirection >= 0) {
                particle.upperIntensity = primaryIntensity;
            } else {
                particle.lowerIntensity = primaryIntensity;
            }

            if (!particle.secondaryFadeStarted && primaryIntensity <= 0.5f) {
                particle.secondaryFadeStarted = true;
                particle.secondaryFadeProgress = 0.0f;
            }

            if (particle.secondaryFadeStarted) {
                particle.secondaryFadeProgress += fadeRate * deltaSeconds;
                float secondaryIntensity = 1.0f - particle.secondaryFadeProgress;
                if (secondaryIntensity < 0.0f) {
                    secondaryIntensity = 0.0f;
                }
                if (particle.verticalDirection >= 0) {
                    particle.lowerIntensity = secondaryIntensity;
                } else {
                    particle.upperIntensity = secondaryIntensity;
                }
            }

            if (particle.upperIntensity <= REMOVAL_EPSILON && particle.lowerIntensity <= REMOVAL_EPSILON) {
                it = particles.erase(it);
                continue;
            }
        }

        ++it;
    }
}

void WormholeParticlePassPatternV1::renderParticles() {
    if (virtualLedCount <= 0) {
        return;
    }

    int count = virtualLedCount;
    if (static_cast<int>(upperBrightnessLevels.size()) != count) {
        upperBrightnessLevels.assign(count, 0.0f);
    } else {
        std::fill(upperBrightnessLevels.begin(), upperBrightnessLevels.end(), 0.0f);
    }

    if (static_cast<int>(lowerBrightnessLevels.size()) != count) {
        lowerBrightnessLevels.assign(count, 0.0f);
    } else {
        std::fill(lowerBrightnessLevels.begin(), lowerBrightnessLevels.end(), 0.0f);
    }

    if (static_cast<int>(lowerLedBuffer.size()) != count) {
        lowerLedBuffer.resize(count);
    }

    for (const Particle &particle : particles) {
        if (particle.ledIndex < 0 || particle.ledIndex >= count) {
            continue;
        }

        float upperValue = particle.upperIntensity;
        if (upperValue < 0.0f) {
            upperValue = 0.0f;
        }
        if (upperValue > 1.0f) {
            upperValue = 1.0f;
        }
        float lowerValue = particle.lowerIntensity;
        if (lowerValue < 0.0f) {
            lowerValue = 0.0f;
        }
        if (lowerValue > 1.0f) {
            lowerValue = 1.0f;
        }

        float &upperSlot = upperBrightnessLevels[particle.ledIndex];
        if (upperValue > upperSlot) {
            upperSlot = upperValue;
        }
        float &lowerSlot = lowerBrightnessLevels[particle.ledIndex];
        if (lowerValue > lowerSlot) {
            lowerSlot = lowerValue;
        }
    }

    for (int i = 0; i < count; ++i) {
        float upperValue = upperBrightnessLevels[i];
        if (upperValue < 0.0f) {
            upperValue = 0.0f;
        }
        if (upperValue > 1.0f) {
            upperValue = 1.0f;
        }

        int targetR = static_cast<int>(baseColorR + (255 - baseColorR) * upperValue);
        int targetG = static_cast<int>(baseColorG + (255 - baseColorG) * upperValue);
        int targetB = static_cast<int>(baseColorB + (255 - baseColorB) * upperValue);

        LedRGB &upperDest = virtualLedBuffer[i];
        upperDest.r = static_cast<uint8_t>(std::max(0, std::min(255, targetR)));
        upperDest.g = static_cast<uint8_t>(std::max(0, std::min(255, targetG)));
        upperDest.b = static_cast<uint8_t>(std::max(0, std::min(255, targetB)));

        float lowerValue = lowerBrightnessLevels[i];
        if (lowerValue < 0.0f) {
            lowerValue = 0.0f;
        }
        if (lowerValue > 1.0f) {
            lowerValue = 1.0f;
        }

        int lowerR = static_cast<int>(baseColorR + (255 - baseColorR) * lowerValue);
        int lowerG = static_cast<int>(baseColorG + (255 - baseColorG) * lowerValue);
        int lowerB = static_cast<int>(baseColorB + (255 - baseColorB) * lowerValue);

        LedRGB &lowerDest = lowerLedBuffer[i];
        lowerDest.r = static_cast<uint8_t>(std::max(0, std::min(255, lowerR)));
        lowerDest.g = static_cast<uint8_t>(std::max(0, std::min(255, lowerG)));
        lowerDest.b = static_cast<uint8_t>(std::max(0, std::min(255, lowerB)));
    }
}

float WormholeParticlePassPatternV1::randomFloat(float minValue, float maxValue) {
    if (maxValue < minValue) {
        float tmp = maxValue;
        maxValue = minValue;
        minValue = tmp;
    }

    long scaledMin = static_cast<long>(minValue * 1000.0f);
    long scaledMax = static_cast<long>(maxValue * 1000.0f);
    if (scaledMax < scaledMin) {
        long tmp = scaledMax;
        scaledMax = scaledMin;
        scaledMin = tmp;
    }

    long value = random(scaledMin, scaledMax + 1);
    return static_cast<float>(value) / 1000.0f;
}

void WormholeParticlePassPatternV1::ensureAuxBuffers(int count) {
    if (count < 0) {
        count = 0;
    }

    if (static_cast<int>(upperBrightnessLevels.size()) != count) {
        upperBrightnessLevels.assign(count, 0.0f);
    }
    if (static_cast<int>(lowerBrightnessLevels.size()) != count) {
        lowerBrightnessLevels.assign(count, 0.0f);
    }
    if (static_cast<int>(lowerLedBuffer.size()) != count) {
        lowerLedBuffer.resize(count);
    }
}

void WormholeParticlePassPatternV1::applyLowerRing(LinkedList<LedRGB*> &leds, int firstCount, int secondCount, int secondStart) {
    int pairCount = virtualLedCount;
    if (pairCount > firstCount) {
        pairCount = firstCount;
    }
    if (pairCount > secondCount) {
        pairCount = secondCount;
    }
    if (pairCount <= 0) {
        return;
    }

    for (int i = 0; i < pairCount; ++i) {
        int targetIndex = i;
        if (direction == DIRECTION_OPPOSITE) {
            targetIndex = pairCount - 1 - i;
        }
        if (targetIndex < 0 || targetIndex >= static_cast<int>(lowerLedBuffer.size())) {
            continue;
        }

        LedRGB *led = leds.get(secondStart + i);
        if (!led) {
            continue;
        }

        LedRGB &src = lowerLedBuffer[targetIndex];
        led->r = src.r;
        led->g = src.g;
        led->b = src.b;
    }
}

String WormholeParticlePassPatternV1::getBaseJson() const {
    StaticJsonDocument<256> doc;
    doc["name"] = "Particle Pass Pattern";
    doc["type"] = "WormholeParticlePassV1";
    doc["active"] = 1;
    doc["direction"] = "opposite";
    doc["ledDirection"] = "right";
    doc["startOffset"] = 0;
    doc["density"] = 5.0;
    doc["minSpeed"] = 0.35;
    doc["maxSpeed"] = 0.85;
    doc["minSize"] = 1.5;
    doc["maxSize"] = 4.0;
    doc["minAngle"] = 0.0;
    doc["maxAngle"] = 6.2831853;

    String output;
    serializeJson(doc, output);
    return output;
}
