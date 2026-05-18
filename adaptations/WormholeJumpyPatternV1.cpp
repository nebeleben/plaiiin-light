#include "WormholeJumpyPatternV1.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>

namespace {
    constexpr float MIN_JUMP_DISTANCE_FALLBACK = 1.0f;
    constexpr float MIN_TAIL_DECAY = 0.0f;
    constexpr float MAX_TAIL_DECAY = 1.0f;

}

WormholeJumpyPatternV1::WormholeJumpyPatternV1() : AbstractWormholePatternV1() {
    enforceFixedConfiguration();
}

WormholeJumpyPatternV1::WormholeJumpyPatternV1(String configPath) : AbstractWormholePatternV1(configPath) {
    enforceFixedConfiguration();
}

void WormholeJumpyPatternV1::updateFromJson(DynamicJsonDocument *json) {
    if (!json) {
        return;
    }

    AbstractPattern::updateFromJson(json);
    DynamicJsonDocument doc = *json;

    enforceFixedConfiguration();

    resetDirection();
    applyDirectionFromJson(doc);

    resetLedDirection();
    resetStartOffset();
    applyLedDirectionFromJson(doc);
    applyStartOffsetFromJson(doc);

    if (doc.containsKey("speed")) {
        float uniformSpeed = doc["speed"].as<float>();
        minSpeed = uniformSpeed;
        maxSpeed = uniformSpeed;
    }
    if (doc.containsKey("minSpeed")) {
        minSpeed = doc["minSpeed"].as<float>();
    }
    if (doc.containsKey("maxSpeed")) {
        maxSpeed = doc["maxSpeed"].as<float>();
    }
    if (doc.containsKey("minSpeedChangeTime")) {
        minSpeedChangeTime = doc["minSpeedChangeTime"].as<float>();
    }
    if (doc.containsKey("maxSpeedChangeTime")) {
        maxSpeedChangeTime = doc["maxSpeedChangeTime"].as<float>();
    }
    if (doc.containsKey("minWidthBeforeJump")) {
        minWidthBeforeJump = doc["minWidthBeforeJump"].as<float>();
    } else if (doc.containsKey("minWidth")) {
        minWidthBeforeJump = doc["minWidth"].as<float>();
    }
    if (doc.containsKey("maxWidthBeforeJump")) {
        maxWidthBeforeJump = doc["maxWidthBeforeJump"].as<float>();
    } else if (doc.containsKey("maxWidth")) {
        maxWidthBeforeJump = doc["maxWidth"].as<float>();
    }
    if (doc.containsKey("tailLength")) {
        tailLength = doc["tailLength"].as<float>();
    }
    if (doc.containsKey("tailDecay")) {
        tailDecay = doc["tailDecay"].as<float>();
    }

    if (minSpeed < 0.0f) {
        minSpeed = 0.0f;
    }
    if (maxSpeed < minSpeed) {
        maxSpeed = minSpeed;
    }
    if (minSpeedChangeTime < 0.0f) {
        minSpeedChangeTime = 0.0f;
    }
    if (maxSpeedChangeTime < minSpeedChangeTime) {
        maxSpeedChangeTime = minSpeedChangeTime;
    }

    if (minWidthBeforeJump < 0.0f) {
        minWidthBeforeJump = 0.0f;
    }
    if (maxWidthBeforeJump < minWidthBeforeJump) {
        maxWidthBeforeJump = minWidthBeforeJump;
    }

    if (tailLength < 0.0f) {
        tailLength = 0.0f;
    }

    if (tailDecay < MIN_TAIL_DECAY) {
        tailDecay = MIN_TAIL_DECAY;
    }
    if (tailDecay > MAX_TAIL_DECAY) {
        tailDecay = MAX_TAIL_DECAY;
    }

    resetState();
}

void WormholeJumpyPatternV1::loop(long millis, LinkedList<LedRGB*> &leds) {
    AbstractPattern::loop(millis, leds);

    int total = leds.size();
    if (total <= 0) {
        return;
    }

    AbstractWormholePatternV1::clearLedList(leds);

    int firstCount = total / 2;
    int secondCount = total - firstCount;

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
    if (virtualLedCount <= 0) {
        return;
    }

    for (int i = 0; i < virtualLedCount; ++i) {
        virtualLedBuffer[i].r = 0;
        virtualLedBuffer[i].g = 0;
        virtualLedBuffer[i].b = 0;
    }

    if (lastMillis < 0) {
        lastMillis = millis;
    }
    long delta = millis - lastMillis;
    if (delta < 0) {
        delta = 0;
    }
    lastMillis = millis;

    float deltaSeconds = static_cast<float>(delta) / 1000.0f;

    int availableFirst = firstCount;
    int availableSecond = secondCount;

    if (mappingType != TYPE_LINE) {
        availableFirst = virtualLedCount;
        availableSecond = 0;
    }

    int currentRingCount = (activeRing == 0) ? availableFirst : availableSecond;
    if (initialPlacementPending) {
        if (availableFirst > 0) {
            activeRing = 0;
            currentRingCount = availableFirst;
        } else if (availableSecond > 0) {
            activeRing = 1;
            currentRingCount = availableSecond;
        }
        headPosition = 0.0f;
        nextJumpDistance = pickNextJumpDistance();
        if (nextJumpDistance < MIN_JUMP_DISTANCE_FALLBACK) {
            nextJumpDistance = MIN_JUMP_DISTANCE_FALLBACK;
        }
        distanceSinceJump = 0.0f;
        initialPlacementPending = false;

        for (int i = 0; i < 2; ++i) {
            ringStates[i].headPosition = 0.0f;
            ringStates[i].visibleTailLength = 0.0f;
            ringStates[i].includeHead = false;
            ringStates[i].hasTail = false;
            ringStates[i].justBecameInactive = false;
        }
    }

    if (currentRingCount <= 0) {
        int alternateCount = (activeRing == 0) ? availableSecond : availableFirst;
        if (alternateCount > 0) {
            activeRing = (activeRing == 0) ? 1 : 0;
            currentRingCount = alternateCount;
        } else {
            mapLineMode(leds, firstCount, secondCount, firstCount);
            return;
        }
    }

    float travel = computeTravel(deltaSeconds);
    if (travel < 0.0f) {
        travel = 0.0f;
    }

    distanceSinceJump += travel;
    headPosition += travel;

    if (currentRingCount > 0) {
        while (headPosition >= static_cast<float>(currentRingCount)) {
            headPosition -= static_cast<float>(currentRingCount);
        }
        while (headPosition < 0.0f) {
            headPosition += static_cast<float>(currentRingCount);
        }
    }

    while (distanceSinceJump >= nextJumpDistance) {
        float leftover = distanceSinceJump - nextJumpDistance;
        int oldRing = activeRing;
        int oldCount = currentRingCount;
        int newRing = (activeRing == 0) ? 1 : 0;
        int newCount = (newRing == 0) ? availableFirst : availableSecond;

        if (newCount <= 0) {
            distanceSinceJump = 0.0f;
            nextJumpDistance = pickNextJumpDistance();
            if (nextJumpDistance < MIN_JUMP_DISTANCE_FALLBACK) {
                nextJumpDistance = MIN_JUMP_DISTANCE_FALLBACK;
            }
            break;
        }

        float normalized = 0.0f;
        if (oldCount > 0) {
            normalized = headPosition / static_cast<float>(oldCount);
            if (normalized < 0.0f) {
                normalized = 0.0f;
            }
            if (normalized > 1.0f) {
                normalized = 1.0f;
            }
        }

        if (oldCount > 0) {
            RingState &oldState = ringStates[oldRing];
            oldState.headPosition = normalized * static_cast<float>(oldCount);
            oldState.includeHead = false;
            oldState.visibleTailLength = tailLength;
            if (oldState.visibleTailLength > 0.0f) {
                oldState.hasTail = true;
                if (leftover > 0.0f) {
                    oldState.visibleTailLength -= leftover;
                    if (oldState.visibleTailLength < 0.0f) {
                        oldState.visibleTailLength = 0.0f;
                        oldState.hasTail = false;
                        oldState.justBecameInactive = false;
                    } else {
                        oldState.justBecameInactive = true;
                    }
                } else {
                    oldState.justBecameInactive = true;
                }
            } else {
                oldState.visibleTailLength = 0.0f;
                oldState.hasTail = false;
                oldState.justBecameInactive = false;
            }
        }

        activeRing = newRing;
        currentRingCount = newCount;
        headPosition = normalized * static_cast<float>(currentRingCount);
        headPosition += leftover;
        if (currentRingCount > 0) {
            while (headPosition >= static_cast<float>(currentRingCount)) {
                headPosition -= static_cast<float>(currentRingCount);
            }
            while (headPosition < 0.0f) {
                headPosition += static_cast<float>(currentRingCount);
            }
        }

        distanceSinceJump = leftover;
        nextJumpDistance = pickNextJumpDistance();
        if (nextJumpDistance < MIN_JUMP_DISTANCE_FALLBACK) {
            nextJumpDistance = MIN_JUMP_DISTANCE_FALLBACK;
        }
    }

    if (currentRingCount > 0) {
        RingState &activeState = ringStates[activeRing];
        activeState.headPosition = headPosition;
        activeState.includeHead = true;
        activeState.visibleTailLength = tailLength;
        activeState.hasTail = true;
        activeState.justBecameInactive = false;
    } else {
        RingState &activeState = ringStates[activeRing];
        activeState.includeHead = false;
        activeState.hasTail = false;
        activeState.visibleTailLength = 0.0f;
        activeState.justBecameInactive = false;
    }

    for (int ring = 0; ring < 2; ++ring) {
        if (ring == activeRing) {
            continue;
        }
        RingState &state = ringStates[ring];
        state.includeHead = false;
        if (!state.hasTail) {
            state.visibleTailLength = 0.0f;
            state.justBecameInactive = false;
            continue;
        }
        if (state.justBecameInactive) {
            state.justBecameInactive = false;
            continue;
        }
        if (travel > 0.0f) {
            state.visibleTailLength -= travel;
            if (state.visibleTailLength <= 0.0f) {
                state.visibleTailLength = 0.0f;
                state.hasTail = false;
                state.justBecameInactive = false;
            }
        }
    }

    int ringCounts[2] = {availableFirst, availableSecond};
    for (int ring = 0; ring < 2; ++ring) {
        int count = ringCounts[ring];
        if (count <= 0) {
            continue;
        }
        RingState &state = ringStates[ring];
        if (!state.includeHead && (!state.hasTail || state.visibleTailLength <= 0.0f)) {
            continue;
        }
        int offset = (ring == 0) ? 0 : availableFirst;
        renderRingState(offset, count, state);
    }

    mapLineMode(leds, firstCount, secondCount, firstCount);
}

void WormholeJumpyPatternV1::enforceFixedConfiguration() {
    resetType();
    setType(TYPE_LINE);
}

void WormholeJumpyPatternV1::resetState() {
    headPosition = 0.0f;
    distanceSinceJump = 0.0f;
    nextJumpDistance = pickNextJumpDistance();
    if (nextJumpDistance < MIN_JUMP_DISTANCE_FALLBACK) {
        nextJumpDistance = MIN_JUMP_DISTANCE_FALLBACK;
    }
    initializeSpeedState();
    lastMillis = -1;
    initialPlacementPending = true;
    for (int i = 0; i < 2; ++i) {
        ringStates[i].headPosition = 0.0f;
        ringStates[i].visibleTailLength = 0.0f;
        ringStates[i].includeHead = false;
        ringStates[i].hasTail = false;
        ringStates[i].justBecameInactive = false;
    }
}

void WormholeJumpyPatternV1::ensureIntensityBuffer(int count) {
    if (count < 0) {
        count = 0;
    }
    if (static_cast<int>(intensityBuffer.size()) != count) {
        intensityBuffer.assign(count, 0.0f);
    } else {
        std::fill(intensityBuffer.begin(), intensityBuffer.end(), 0.0f);
    }
}

float WormholeJumpyPatternV1::pickNextJumpDistance() const {
    float distance = randomFloat(minWidthBeforeJump, maxWidthBeforeJump);
    if (distance < MIN_JUMP_DISTANCE_FALLBACK) {
        return minWidthBeforeJump > 0.0f ? minWidthBeforeJump : MIN_JUMP_DISTANCE_FALLBACK;
    }
    return distance;
}

float WormholeJumpyPatternV1::computeTravel(float deltaSeconds) {
    if (deltaSeconds <= 0.0f) {
        return 0.0f;
    }

    float travel = 0.0f;
    float remaining = deltaSeconds;

    while (remaining > 0.0f) {
        if (speedChangeRemaining <= 0.0f) {
            currentSpeed = targetSpeed;
            currentAcceleration = 0.0f;
            scheduleNextSpeedChange(false);
            if (speedChangeRemaining <= 0.0f) {
                travel += currentSpeed * remaining;
                return travel;
            }
        }

        float step = remaining;
        if (speedChangeRemaining > 0.0f && step > speedChangeRemaining) {
            step = speedChangeRemaining;
        }

        float startSpeed = currentSpeed;
        float acceleration = currentAcceleration;
        travel += startSpeed * step + 0.5f * acceleration * step * step;
        currentSpeed = startSpeed + acceleration * step;
        if (currentSpeed < 0.0f) {
            currentSpeed = 0.0f;
        }
        speedChangeRemaining -= step;
        remaining -= step;

        if (speedChangeRemaining <= 0.0f) {
            currentSpeed = targetSpeed;
            speedChangeRemaining = 0.0f;
            currentAcceleration = 0.0f;
        }
    }

    return travel;
}

void WormholeJumpyPatternV1::initializeSpeedState() {
    scheduleNextSpeedChange(true);
}

void WormholeJumpyPatternV1::scheduleNextSpeedChange(bool initial) {
    float usedMinSpeed = std::max(minSpeed, 0.0f);
    float usedMaxSpeed = std::max(maxSpeed, usedMinSpeed);
    float newTarget = randomFloat(usedMinSpeed, usedMaxSpeed);
    if (newTarget < 0.0f) {
        newTarget = 0.0f;
    }

    float usedMinTime = std::max(minSpeedChangeTime, 0.0f);
    float usedMaxTime = std::max(maxSpeedChangeTime, usedMinTime);
    float duration = randomFloat(usedMinTime, usedMaxTime);
    if (duration < 0.0f) {
        duration = 0.0f;
    }

    targetSpeed = newTarget;
    if (initial) {
        currentSpeed = targetSpeed;
        speedChangeRemaining = 0.0f;
        currentAcceleration = 0.0f;
        return;
    }

    if (duration <= 0.0f) {
        currentSpeed = targetSpeed;
        speedChangeRemaining = 0.0f;
        currentAcceleration = 0.0f;
        return;
    }

    currentAcceleration = (targetSpeed - currentSpeed) / duration;
    speedChangeRemaining = duration;
}

float WormholeJumpyPatternV1::randomFloat(float minValue, float maxValue) {
    if (maxValue < minValue) {
        float tmp = maxValue;
        maxValue = minValue;
        minValue = tmp;
    }

    long scaledMin = static_cast<long>(std::floor(minValue * 1000.0f));
    long scaledMax = static_cast<long>(std::floor(maxValue * 1000.0f));
    if (scaledMax <= scaledMin) {
        return minValue;
    }
    long value = random(scaledMin, scaledMax + 1);
    return static_cast<float>(value) / 1000.0f;
}

void WormholeJumpyPatternV1::renderRingState(int offset, int count, const RingState &state) {
    if (count <= 0 || offset < 0) {
        return;
    }

    if (!state.includeHead && (!state.hasTail || state.visibleTailLength <= 0.0f)) {
        return;
    }

    ensureIntensityBuffer(count);

    float ringLength = static_cast<float>(count);
    if (ringLength <= 0.0f) {
        return;
    }

    float currentHead = state.headPosition;
    while (currentHead >= ringLength) {
        currentHead -= ringLength;
    }
    while (currentHead < 0.0f) {
        currentHead += ringLength;
    }

    float effectiveTailLength = state.visibleTailLength;
    if (effectiveTailLength < 0.0f) {
        effectiveTailLength = 0.0f;
    }

    int maxSteps = 0;
    if (state.includeHead || effectiveTailLength > 0.0f) {
        maxSteps = static_cast<int>(std::ceil(effectiveTailLength));
    }
    if (maxSteps < 0) {
        maxSteps = 0;
    }

    float clampedDecay = tailDecay;
    if (clampedDecay < MIN_TAIL_DECAY) {
        clampedDecay = MIN_TAIL_DECAY;
    }
    if (clampedDecay > MAX_TAIL_DECAY) {
        clampedDecay = MAX_TAIL_DECAY;
    }

    for (int step = 0; step <= maxSteps; ++step) {
        if (step == 0 && !state.includeHead) {
            continue;
        }

        float distance = static_cast<float>(step);
        if (distance > 0.0f && distance > effectiveTailLength) {
            break;
        }

        float baseBrightness = 1.0f;
        if (distance > 0.0f) {
            if (effectiveTailLength > 0.0f) {
                baseBrightness = 1.0f - (distance / effectiveTailLength);
                if (baseBrightness < 0.0f) {
                    baseBrightness = 0.0f;
                }
            } else {
                baseBrightness = 0.0f;
            }
        }

        float decayBrightness = 1.0f;
        if (distance > 0.0f) {
            if (clampedDecay <= 0.0f) {
                decayBrightness = 0.0f;
            } else {
                decayBrightness = std::pow(clampedDecay, distance);
            }
        }

        float brightness = baseBrightness * decayBrightness;
        if (step == 0 && state.includeHead) {
            brightness = 1.0f;
        }
        if (brightness <= 0.0f) {
            continue;
        }

        float samplePosition = currentHead - distance;
        while (samplePosition < 0.0f) {
            samplePosition += ringLength;
        }
        while (samplePosition >= ringLength) {
            samplePosition -= ringLength;
        }

        int lowerIndex = static_cast<int>(std::floor(samplePosition));
        float fraction = samplePosition - static_cast<float>(lowerIndex);
        int upperIndex = lowerIndex + 1;
        if (upperIndex >= count) {
            upperIndex -= count;
        }

        float primaryContribution = brightness * (1.0f - fraction);
        float secondaryContribution = brightness * fraction;

        if (primaryContribution > 0.0f) {
            intensityBuffer[lowerIndex] += primaryContribution;
        }
        if (secondaryContribution > 0.0f) {
            intensityBuffer[upperIndex] += secondaryContribution;
        }
    }

    for (int i = 0; i < count; ++i) {
        float brightness = intensityBuffer[i];
        if (brightness > 1.0f) {
            brightness = 1.0f;
        }
        if (brightness < 0.0f) {
            brightness = 0.0f;
        }

        int r = static_cast<int>(std::round(static_cast<float>(baseColorR) * brightness));
        int g = static_cast<int>(std::round(static_cast<float>(baseColorG) * brightness));
        int b = static_cast<int>(std::round(static_cast<float>(baseColorB) * brightness));

        if (r < 0) {
            r = 0;
        } else if (r > 255) {
            r = 255;
        }
        if (g < 0) {
            g = 0;
        } else if (g > 255) {
            g = 255;
        }
        if (b < 0) {
            b = 0;
        } else if (b > 255) {
            b = 255;
        }

        LedRGB &dest = virtualLedBuffer[offset + i];
        dest.r = static_cast<uint8_t>(r);
        dest.g = static_cast<uint8_t>(g);
        dest.b = static_cast<uint8_t>(b);
    }
}

String WormholeJumpyPatternV1::getBaseJson() const {
    StaticJsonDocument<256> doc;
    doc["name"] = "Jumpy Pattern";
    doc["type"] = "WormholeJumpyV1";
    doc["active"] = 1;
    doc["direction"] = "same";
    doc["ledDirection"] = "right";
    doc["startOffset"] = 0;
    doc["minSpeed"] = 12.0;
    doc["maxSpeed"] = 64.0;
    doc["minSpeedChangeTime"] = 6.0;
    doc["maxSpeedChangeTime"] = 24.0;
    doc["minWidthBeforeJump"] = 12.0;
    doc["maxWidthBeforeJump"] = 36.0;
    doc["tailLength"] = 12.0;
    doc["tailDecay"] = 0.65;

    String output;
    serializeJson(doc, output);
    return output;
}
