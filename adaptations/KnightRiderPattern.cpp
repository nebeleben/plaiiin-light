#include "KnightRiderPattern.h"

#include <math.h>
#include <Arduino.h>
#include <ArduinoJson.h>

KnightRiderPattern::KnightRiderPattern() {
    secondsPerRound = 2.5;
    tailLength = 6;
    tailFade = 0.55;
    coreWidth = 1.4;
    trailIntensity = 0.65;
    backgroundLevel = 0.0;
    leadingBoost = 1.2;
    edgePauseMillis = 120;
    bounceMode = true;

    position = 0.0;
    direction = 1;
    cycleDirection = 1;
    lastMillis = 0;
    edgePauseUntil = 0;
}

KnightRiderPattern::KnightRiderPattern(String configPath) {
    this->configFile = configPath;
    secondsPerRound = 2.5;
    tailLength = 6;
    tailFade = 0.55;
    coreWidth = 1.4;
    trailIntensity = 0.65;
    backgroundLevel = 0.0;
    leadingBoost = 1.2;
    edgePauseMillis = 120;
    bounceMode = true;

    position = 0.0;
    direction = 1;
    cycleDirection = 1;
    lastMillis = 0;
    edgePauseUntil = 0;
}

void KnightRiderPattern::updateFromJson(DynamicJsonDocument *json) {
    AbstractPattern::updateFromJson(json);
    DynamicJsonDocument doc = *json;

    if (doc.containsKey("secondsPerRound")) {
        secondsPerRound = doc["secondsPerRound"].as<double>();
    }
    if (doc.containsKey("tailLength")) {
        tailLength = doc["tailLength"].as<int>();
    }
    if (doc.containsKey("tailFade")) {
        tailFade = doc["tailFade"].as<double>();
    }
    if (doc.containsKey("coreWidth")) {
        coreWidth = doc["coreWidth"].as<double>();
    }
    if (doc.containsKey("trailIntensity")) {
        trailIntensity = doc["trailIntensity"].as<double>();
    }
    if (doc.containsKey("backgroundLevel")) {
        backgroundLevel = doc["backgroundLevel"].as<double>();
    }
    if (doc.containsKey("leadingBoost")) {
        leadingBoost = doc["leadingBoost"].as<double>();
    }
    if (doc.containsKey("edgePauseMs")) {
        edgePauseMillis = doc["edgePauseMs"].as<long>();
    }
    if (doc.containsKey("movementMode")) {
        String mode = doc["movementMode"].as<String>();
        if (mode.equalsIgnoreCase("cycle")) {
            bounceMode = false;
        } else if (mode.equalsIgnoreCase("bounce")) {
            bounceMode = true;
        }
    }
    if (doc.containsKey("bounce")) {
        bounceMode = doc["bounce"].as<bool>();
    }
    if (doc.containsKey("cycle")) {
        bool cycleEnabled = doc["cycle"].as<bool>();
        bounceMode = !cycleEnabled;
    }
    if (doc.containsKey("direction")) {
        int configuredDirection = doc["direction"].as<int>();
        if (configuredDirection < 0) {
            cycleDirection = -1;
        } else {
            cycleDirection = 1;
        }
    }

    if (secondsPerRound <= 0) secondsPerRound = 2.5;
    if (tailLength < 0) tailLength = 0;
    if (tailFade < 0.0) tailFade = 0.0;
    if (tailFade > 1.0) tailFade = 1.0;
    if (coreWidth < 0.0) coreWidth = 0.0;
    if (trailIntensity < 0.0) trailIntensity = 0.0;
    if (trailIntensity > 1.0) trailIntensity = 1.0;
    if (backgroundLevel < 0.0) backgroundLevel = 0.0;
    if (backgroundLevel > 1.0) backgroundLevel = 1.0;
    if (leadingBoost < 1.0) leadingBoost = 1.0;
    if (leadingBoost > 1.5) leadingBoost = 1.5;
    if (edgePauseMillis < 0) edgePauseMillis = 0;

    position = 0.0;
    if (cycleDirection == 0) {
        cycleDirection = 1;
    }
    direction = bounceMode ? 1 : cycleDirection;
    lastMillis = 0;
    edgePauseUntil = 0;
}

void KnightRiderPattern::loop(long millis, LinkedList<LedRGB *> &leds) {
    AbstractPattern::loop(millis, leds);

    int totalLeds = leds.size();
    if (totalLeds <= 0) {
        return;
    }

    int dimension = totalLeds;

    long delta = (lastMillis == 0) ? 0 : (millis - lastMillis);
    lastMillis = millis;

    if (edgePauseUntil > 0) {
        if (millis < edgePauseUntil) {
            delta = 0;
        } else {
            edgePauseUntil = 0;
        }
    }

    if (delta != 0) {
        double stepsPerMs;
        if (bounceMode) {
            double cycleSteps = (dimension <= 1) ? 1.0 : (double)(2 * (dimension - 1));
            stepsPerMs = cycleSteps / (secondsPerRound * 1000.0);
        } else {
            double cycleSteps = (dimension <= 0) ? 1.0 : (double)dimension;
            stepsPerMs = cycleSteps / (secondsPerRound * 1000.0);
        }
        if (!bounceMode) {
            if (cycleDirection == 0) {
                cycleDirection = 1;
            }
            direction = cycleDirection;
        }
        position += direction * stepsPerMs * (double)delta;

        if (bounceMode) {
            if (dimension > 1) {
                double maxPos = (double)(dimension - 1);
                if (position >= maxPos) {
                    position = maxPos - (position - maxPos);
                    direction = -1;
                    if (edgePauseMillis > 0) {
                        edgePauseUntil = millis + edgePauseMillis;
                    }
                } else if (position <= 0.0) {
                    position = -position;
                    direction = 1;
                    if (edgePauseMillis > 0) {
                        edgePauseUntil = millis + edgePauseMillis;
                    }
                }
            } else {
                position = 0.0;
            }
        } else {
            if (dimension > 0) {
                double circumference = (double)dimension;
                position = fmod(position, circumference);
                if (position < 0.0) {
                    position += circumference;
                }
            } else {
                position = 0.0;
            }
        }
    }

    for (int i = 0; i < totalLeds; i++) {
        LedRGB *l = leds.get(i);
        if (!l) {
            continue;
        }
        l->r = 0;
        l->g = 0;
        l->b = 0;
    }

    double halfCore = coreWidth * 0.5;
    for (int index = 0; index < totalLeds; ++index) {
        LedRGB *l = leds.get(index);
        if (!l) {
            continue;
        }

        double distance;
        if (!bounceMode && dimension > 0) {
            double direct = fabs((double)index - position);
            double wrapDistance = (double)dimension - direct;
            if (wrapDistance < 0.0) {
                wrapDistance = 0.0;
            }
            distance = (wrapDistance < direct) ? wrapDistance : direct;
        } else {
            distance = fabs((double)index - position);
        }

        double brightness = computeBrightness(distance);
        if (leadingBoost > 1.0 && halfCore > 0.0) {
            double maxAhead = halfCore + 0.5;
            if (direction > 0) {
                double ahead = (double)index - position;
                if (!bounceMode && dimension > 0 && ahead < 0.0) {
                    ahead += (double)dimension;
                }
                if (ahead >= 0.0 && ahead <= maxAhead) {
                    brightness *= leadingBoost;
                }
            } else {
                double ahead = position - (double)index;
                if (!bounceMode && dimension > 0 && ahead < 0.0) {
                    ahead += (double)dimension;
                }
                if (ahead >= 0.0 && ahead <= maxAhead) {
                    brightness *= leadingBoost;
                }
            }
        }

        if (brightness < backgroundLevel) {
            brightness = backgroundLevel;
        }
        if (brightness > 1.0) {
            brightness = 1.0;
        }

        int r = (int)(baseColorR * brightness);
        int g = (int)(baseColorG * brightness);
        int b = (int)(baseColorB * brightness);

        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        if (r < 0) r = 0;
        if (g < 0) g = 0;
        if (b < 0) b = 0;

        l->r = r;
        l->g = g;
        l->b = b;
    }
}

double KnightRiderPattern::computeBrightness(double distance) const {
    double brightness = backgroundLevel;
    double halfCore = coreWidth * 0.5;

    if (distance <= halfCore && halfCore > 0.0) {
        double ratio = distance / halfCore;
        if (ratio < 0.0) ratio = 0.0;
        if (ratio > 1.0) ratio = 1.0;
        brightness = cos(ratio * (PI / 2.0));
    } else if (halfCore == 0.0 && distance == 0.0) {
        brightness = 1.0;
    } else {
        double tailDistance = distance - halfCore;
        if (tailDistance < 0.0) {
            tailDistance = 0.0;
        }
        if (tailDistance <= tailLength) {
            double fade = 0.0;
            if (tailFade <= 0.0) {
                fade = (tailDistance == 0.0) ? 1.0 : 0.0;
            } else if (tailFade >= 1.0) {
                fade = 1.0;
            } else {
                fade = pow(tailFade, tailDistance);
            }
            brightness = fade * trailIntensity;
        }
    }

    if (brightness < backgroundLevel) {
        brightness = backgroundLevel;
    }

    if (brightness > 1.0) {
        brightness = 1.0;
    }

    return brightness;
}

String KnightRiderPattern::getBaseJson() const {
    StaticJsonDocument<256> doc;
    doc["name"] = "Knight Rider Scanner";
    doc["type"] = "KnightRider";
    doc["active"] = 1;
    doc["secondsPerRound"] = 2.2;
    doc["tailLength"] = 7;
    doc["tailFade"] = 0.55;
    doc["coreWidth"] = 1.5;
    doc["trailIntensity"] = 0.65;
    doc["leadingBoost"] = 1.25;
    doc["edgePauseMs"] = 140;
    doc["movementMode"] = "bounce";
    doc["direction"] = 1;

    String output;
    serializeJson(doc, output);
    return output;
}
