#include "ShootingStarPattern.h"
#include <ArduinoJson.h>
#include <Arduino.h>
#include <math.h>

ShootingStarPattern::ShootingStarPattern() {
    lastMillis = 0;
    nextSpawnMillis = 0;
    maxStars = 3;
    tailLength = 8.0;
    tailExponent = 2.0;
    minSpeed = 12.0;
    maxSpeed = 28.0;
    minCircles = 1;
    maxCircles = 3;
    minSpawnIntervalMs = 500;
    maxSpawnIntervalMs = 1500;
}

ShootingStarPattern::ShootingStarPattern(String configPath) {
    this->configFile = configPath;
    lastMillis = 0;
    nextSpawnMillis = 0;
    maxStars = 3;
    tailLength = 8.0;
    tailExponent = 2.0;
    minSpeed = 12.0;
    maxSpeed = 28.0;
    minCircles = 1;
    maxCircles = 3;
    minSpawnIntervalMs = 500;
    maxSpawnIntervalMs = 1500;
}

void ShootingStarPattern::updateFromJson(DynamicJsonDocument *json) {
    AbstractPattern::updateFromJson(json);
    DynamicJsonDocument doc = *json;

    if (doc.containsKey("max_stars")) {
        maxStars = doc["max_stars"].as<int>();
        if (maxStars < 1) {
            maxStars = 1;
        }
    }

    if (doc.containsKey("tail_length")) {
        tailLength = doc["tail_length"].as<double>();
        if (tailLength < 0.0) {
            tailLength = 0.0;
        }
    }

    if (doc.containsKey("tail_exponent")) {
        tailExponent = doc["tail_exponent"].as<double>();
        if (tailExponent < 1.0) {
            tailExponent = 1.0;
        }
    }

    if (doc.containsKey("min_speed")) {
        minSpeed = doc["min_speed"].as<double>();
        if (minSpeed < 0.1) {
            minSpeed = 0.1;
        }
    }

    if (doc.containsKey("max_speed")) {
        maxSpeed = doc["max_speed"].as<double>();
    }
    if (maxSpeed < minSpeed) {
        maxSpeed = minSpeed;
    }

    if (doc.containsKey("min_circles")) {
        minCircles = doc["min_circles"].as<int>();
        if (minCircles < 1) {
            minCircles = 1;
        }
    }

    if (doc.containsKey("max_circles")) {
        maxCircles = doc["max_circles"].as<int>();
    }
    if (maxCircles < minCircles) {
        maxCircles = minCircles;
    }

    if (doc.containsKey("min_spawn_interval_ms")) {
        minSpawnIntervalMs = doc["min_spawn_interval_ms"].as<long>();
        if (minSpawnIntervalMs < 1) {
            minSpawnIntervalMs = 1;
        }
    }

    if (doc.containsKey("max_spawn_interval_ms")) {
        maxSpawnIntervalMs = doc["max_spawn_interval_ms"].as<long>();
    }
    if (maxSpawnIntervalMs < minSpawnIntervalMs) {
        maxSpawnIntervalMs = minSpawnIntervalMs;
    }

    stars.clear();
    lastMillis = 0;
    nextSpawnMillis = 0;
}

void ShootingStarPattern::loop(long millis, LinkedList<LedRGB *> &leds) {
    AbstractPattern::loop(millis, leds);

    int ledCount = leds.size();
    if (ledCount <= 0) {
        stars.clear();
        scheduleNextSpawn(millis);
        lastMillis = millis;
        return;
    }

    for (int i = 0; i < ledCount; i++) {
        LedRGB *led = leds.get(i);
        if (!led) {
            continue;
        }
        led->r = 0;
        led->g = 0;
        led->b = 0;
    }

    if (nextSpawnMillis == 0) {
        nextSpawnMillis = millis;
    }

    while (stars.size() < (size_t)maxStars && millis >= nextSpawnMillis) {
        spawnStar(millis, ledCount);
    }

    long delta = lastMillis == 0 ? 0 : (millis - lastMillis);
    lastMillis = millis;
    if (delta < 0) {
        delta = 0;
    }

    std::vector<ShootingStar> activeStars;
    activeStars.reserve(stars.size());

    for (auto &star : stars) {
        double distance = star.speedPerMs * (double)delta;
        star.position += (double)star.direction * distance;

        bool keep = true;
        while (star.position >= (double)ledCount) {
            star.position -= (double)ledCount;
            star.loopsCompleted++;
            if (star.loopsCompleted >= star.loopsTarget) {
                keep = false;
                break;
            }
        }
        while (keep && star.position < 0.0) {
            star.position += (double)ledCount;
            star.loopsCompleted++;
            if (star.loopsCompleted >= star.loopsTarget) {
                keep = false;
                break;
            }
        }

        if (keep) {
            activeStars.push_back(star);
        }
    }

    stars.swap(activeStars);

    if (stars.empty()) {
        spawnStar(millis, ledCount);
    }

    if (stars.empty()) {
        return;
    }

    int tailSteps = (int)ceil(tailLength);
    if (tailSteps < 0) {
        tailSteps = 0;
    }

    for (auto &star : stars) {
        double pos = star.position;
        if (pos < 0.0) {
            pos = fmod(pos, (double)ledCount);
            if (pos < 0.0) {
                pos += ledCount;
            }
        } else if (pos >= ledCount) {
            pos = fmod(pos, (double)ledCount);
        }

        int headIndex = (int)floor(pos + 0.5);
        headIndex %= ledCount;

        applyColor(leds.get(headIndex), 1.0);

        for (int step = 1; step <= tailSteps; ++step) {
            double normalized = 1.0 - ((double)step / (tailLength + 1.0));
            if (normalized < 0.0) {
                normalized = 0.0;
            }
            double brightness = pow(normalized, tailExponent);
            if (brightness <= 0.0) {
                continue;
            }

            int index = headIndex - star.direction * step;
            index %= ledCount;
            if (index < 0) {
                index += ledCount;
            }

            applyColor(leds.get(index), brightness);
        }
    }
}

void ShootingStarPattern::scheduleNextSpawn(long currentMillis) {
    long minInterval = minSpawnIntervalMs <= 0 ? 1 : minSpawnIntervalMs;
    long maxInterval = maxSpawnIntervalMs <= 0 ? minInterval : maxSpawnIntervalMs;
    if (maxInterval < minInterval) {
        maxInterval = minInterval;
    }
    long interval = random(minInterval, maxInterval + 1);
    nextSpawnMillis = currentMillis + interval;
}

void ShootingStarPattern::spawnStar(long currentMillis, int ledCount) {
    if (ledCount <= 0) {
        scheduleNextSpawn(currentMillis);
        return;
    }

    ShootingStar star{};
    star.position = randomStartPosition(ledCount);
    double speedPerSecond = randomDouble(minSpeed, maxSpeed);
    if (speedPerSecond <= 0.0) {
        speedPerSecond = minSpeed;
        if (speedPerSecond <= 0.0) {
            speedPerSecond = 1.0;
        }
    }
    star.speedPerMs = speedPerSecond / 1000.0;
    star.direction = random(0, 2) == 0 ? 1 : -1;
    star.loopsTarget = random(minCircles, maxCircles + 1);
    if (star.loopsTarget < 1) {
        star.loopsTarget = 1;
    }
    star.loopsCompleted = 0;

    stars.push_back(star);
    scheduleNextSpawn(currentMillis);
}

double ShootingStarPattern::randomDouble(double minValue, double maxValue) {
    if (maxValue <= minValue) {
        return minValue;
    }
    long scaledMin = (long)floor(minValue * 1000.0);
    long scaledMax = (long)ceil(maxValue * 1000.0);
    if (scaledMax <= scaledMin) {
        scaledMax = scaledMin + 1;
    }
    long value = random(scaledMin, scaledMax);
    return (double)value / 1000.0;
}

double ShootingStarPattern::randomStartPosition(int ledCount) {
    if (ledCount <= 0) {
        return 0.0;
    }

    double discreteIndex = (double)random(0, ledCount);
    double fractionalOffset = randomDouble(0.0, 1.0);
    double position = discreteIndex + fractionalOffset;

    double maxPosition = (double)ledCount;
    if (position >= maxPosition) {
        position -= maxPosition;
    }

    return position;
}

void ShootingStarPattern::applyColor(LedRGB *led, double brightness) {
    if (!led) {
        return;
    }
    if (brightness <= 0.0) {
        return;
    }
    if (brightness > 1.0) {
        brightness = 1.0;
    }

    int addR = (int)(baseColorR * brightness + 0.5);
    int addG = (int)(baseColorG * brightness + 0.5);
    int addB = (int)(baseColorB * brightness + 0.5);

    int r = led->r + addR;
    int g = led->g + addG;
    int b = led->b + addB;

    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;

    led->r = r;
    led->g = g;
    led->b = b;
}

String ShootingStarPattern::getBaseJson() const {
    StaticJsonDocument<256> doc;
    doc["name"] = "Shooting Star Pattern";
    doc["type"] = "ShootingStar";
    doc["active"] = 1;
    doc["max_stars"] = 3;
    doc["tail_length"] = 8.0;
    doc["tail_exponent"] = 2.0;
    doc["min_speed"] = 12.0;
    doc["max_speed"] = 28.0;
    doc["min_circles"] = 1;
    doc["max_circles"] = 3;
    doc["min_spawn_interval_ms"] = 500;
    doc["max_spawn_interval_ms"] = 1500;

    String output;
    serializeJson(doc, output);
    return output;
}
