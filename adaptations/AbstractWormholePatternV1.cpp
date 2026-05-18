#include "AbstractWormholePatternV1.h"

#include <cmath>

AbstractWormholePatternV1::AbstractWormholePatternV1() {
    resetDirection();
    resetType();
    resetLedDirection();
    resetStartOffset();
}

AbstractWormholePatternV1::AbstractWormholePatternV1(String configPath) {
    this->configFile = configPath;
    resetDirection();
    resetType();
    resetLedDirection();
    resetStartOffset();
}

AbstractWormholePatternV1::~AbstractWormholePatternV1() {
    ensureVirtualBuffer(0);
}

void AbstractWormholePatternV1::resetDirection() {
    direction = DIRECTION_OPPOSITE;
}

void AbstractWormholePatternV1::applyDirectionFromJson(DynamicJsonDocument &doc) {
    if (!doc.containsKey("direction")) {
        return;
    }

    String directionValue = doc["direction"].as<String>();
    directionValue.toLowerCase();
    if (directionValue == "same") {
        direction = DIRECTION_OPPOSITE;
    } else if (directionValue == "opposite") {
        direction = DIRECTION_SAME;
    }
}

void AbstractWormholePatternV1::resetType() {
    type = TYPE_MIRROR;
}

void AbstractWormholePatternV1::applyTypeFromJson(DynamicJsonDocument &doc) {
    if (doc.containsKey("wormholeType")) {
        String value = doc["wormholeType"].as<String>();
        value.toLowerCase();
        if (value == "line") {
            type = TYPE_LINE;
        } else {
            type = TYPE_MIRROR;
        }
        return;
    }

    if (!doc.containsKey("mode")) {
        return;
    }

    String modeValue = doc["mode"].as<String>();
    modeValue.toLowerCase();
    if (modeValue == "line") {
        type = TYPE_LINE;
    } else if (modeValue == "mirror") {
        type = TYPE_MIRROR;
    }
}

void AbstractWormholePatternV1::setType(WormholeType newType) {
    type = newType;
}

AbstractWormholePatternV1::WormholeType AbstractWormholePatternV1::getType() const {
    return type;
}

void AbstractWormholePatternV1::resetLedDirection() {
    ledDirection = LED_DIRECTION_RIGHT;
}

void AbstractWormholePatternV1::applyLedDirectionFromJson(DynamicJsonDocument &doc) {
    if (!doc.containsKey("ledDirection")) {
        return;
    }

    String directionValue = doc["ledDirection"].as<String>();
    directionValue.toLowerCase();
    if (directionValue == "left") {
        ledDirection = LED_DIRECTION_LEFT;
    } else if (directionValue == "right") {
        ledDirection = LED_DIRECTION_RIGHT;
    }
}

void AbstractWormholePatternV1::resetStartOffset() {
    startOffset = 0;
}

void AbstractWormholePatternV1::applyStartOffsetFromJson(DynamicJsonDocument &doc) {
    if (!doc.containsKey("startOffset")) {
        return;
    }

    JsonVariant offsetVariant = doc["startOffset"];
    if (offsetVariant.is<int>()) {
        startOffset = offsetVariant.as<int>();
    } else if (offsetVariant.is<long>()) {
        startOffset = static_cast<int>(offsetVariant.as<long>());
    } else if (offsetVariant.is<float>()) {
        startOffset = static_cast<int>(offsetVariant.as<float>());
    } else if (offsetVariant.is<double>()) {
        startOffset = static_cast<int>(offsetVariant.as<double>());
    }
}

void AbstractWormholePatternV1::ensureVirtualBuffer(int count) {
    if (count < 0) {
        count = 0;
    }

    if (count == virtualLedCount) {
        if (count == 0) {
            innerLeds.clear();
        } else if (innerLeds.size() != virtualLedCount) {
            innerLeds.clear();
            for (int i = 0; i < virtualLedCount; ++i) {
                innerLeds.add(&virtualLedBuffer[i]);
            }
        }
        return;
    }

    if (virtualLedBuffer) {
        delete[] virtualLedBuffer;
        virtualLedBuffer = nullptr;
    }
    virtualLedCount = count;
    innerLeds.clear();

    if (virtualLedCount > 0) {
        virtualLedBuffer = new LedRGB[virtualLedCount];
        for (int i = 0; i < virtualLedCount; ++i) {
            innerLeds.add(&virtualLedBuffer[i]);
        }
    }
}

void AbstractWormholePatternV1::fillWithBaseColor(LinkedList<LedRGB*> &leds) {
    int total = leds.size();
    for (int i = 0; i < total; ++i) {
        LedRGB *led = leds.get(i);
        if (!led) {
            continue;
        }
        led->r = baseColorR;
        led->g = baseColorG;
        led->b = baseColorB;
    }
}

void AbstractWormholePatternV1::mapMirrorMode(LinkedList<LedRGB*> &leds, int firstCount, int secondCount, int secondStart) {
    int pairCount = virtualLedCount;
    if (pairCount > firstCount) {
        pairCount = firstCount;
    }
    if (pairCount > secondCount) {
        pairCount = secondCount;
    }

    for (int i = 0; i < pairCount; ++i) {
        LedRGB *left = leds.get(i);
        if (left) {
            int sourceIndex = normalizeIndexWithOffset(i, pairCount);
            LedRGB &source = virtualLedBuffer[sourceIndex];
            left->r = source.r;
            left->g = source.g;
            left->b = source.b;
        }

        LedRGB *right = leds.get(secondStart + i);
        if (!right) {
            continue;
        }

        int rightIndex = i;
        if (direction == DIRECTION_OPPOSITE) {
            rightIndex = pairCount - 1 - i;
        }
        if (rightIndex < 0 || rightIndex >= virtualLedCount) {
            continue;
        }
        int sourceIndex = normalizeIndexWithOffset(rightIndex, pairCount);
        LedRGB &rightSource = virtualLedBuffer[sourceIndex];
        right->r = rightSource.r;
        right->g = rightSource.g;
        right->b = rightSource.b;
    }
}

void AbstractWormholePatternV1::clearLedList(LinkedList<LedRGB*> &leds) {
    int total = leds.size();
    for (int i = 0; i < total; ++i) {
        LedRGB *led = leds.get(i);
        if (!led) {
            continue;
        }
        led->r = 0;
        led->g = 0;
        led->b = 0;
    }
}

void AbstractWormholePatternV1::mapLineMode(LinkedList<LedRGB*> &leds, int firstCount, int secondCount, int secondStart) {
    int availableFirst = firstCount;
    if (availableFirst > virtualLedCount) {
        availableFirst = virtualLedCount;
    }

    for (int i = 0; i < availableFirst; ++i) {
        LedRGB *led = leds.get(i);
        if (!led) {
            continue;
        }
        int sourceIndex = normalizeIndexWithOffset(i, virtualLedCount);
        LedRGB &src = virtualLedBuffer[sourceIndex];
        led->r = src.r;
        led->g = src.g;
        led->b = src.b;
    }

    for (int i = 0; i < secondCount; ++i) {
        LedRGB *led = leds.get(secondStart + i);
        if (!led) {
            continue;
        }
        int srcIndex;
        if (direction == DIRECTION_SAME) {
            srcIndex = firstCount + i;
        } else {
            srcIndex = firstCount + (secondCount - 1 - i);
        }
        if (srcIndex < 0 || srcIndex >= virtualLedCount) {
            continue;
        }
        int sourceIndex = normalizeIndexWithOffset(srcIndex, virtualLedCount);
        LedRGB &src = virtualLedBuffer[sourceIndex];
        led->r = src.r;
        led->g = src.g;
        led->b = src.b;
    }
}

int AbstractWormholePatternV1::normalizeIndexWithOffset(int baseIndex, int totalCount) const {
    if (totalCount <= 0) {
        return 0;
    }

    int normalizedOffset = startOffset % totalCount;
    if (normalizedOffset < 0) {
        normalizedOffset += totalCount;
    }

    long adjustedIndex;
    if (ledDirection == LED_DIRECTION_LEFT) {
        adjustedIndex = static_cast<long>(normalizedOffset) - static_cast<long>(baseIndex);
    } else {
        adjustedIndex = static_cast<long>(normalizedOffset) + static_cast<long>(baseIndex);
    }

    int wrappedIndex = static_cast<int>(adjustedIndex % totalCount);
    if (wrappedIndex < 0) {
        wrappedIndex += totalCount;
    }
    return wrappedIndex;
}
