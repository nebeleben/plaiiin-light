#include "FirePattern.h"
#include "../ColorConversion.h"
#include <ArduinoJson.h>

FirePattern::FirePattern() {

}

FirePattern::FirePattern(String configPath) {
    this->configFile = configPath;
    baseColorMode = true;
}

void FirePattern::updateFromJson(DynamicJsonDocument *json) {
    AbstractPattern::updateFromJson(json);
    DynamicJsonDocument doc = *json;

    hueData = doc["hue_mask"].as<String>();
    valueData = doc["value_mask"].as<String>();
    int cmode = doc["base_mode"].as<int>();
    int rmode = doc["reverse_mode"].as<int>();

    baseColorMode = cmode == 1;
    reversedMode = rmode == 1;

    width = doc["width"].as<int>();
    height = doc["height"].as<int>();

    line = new IntegerValueRow(width);
    hueMask.clear();
    valueMask.clear();
    matrixValue.clear();

    for (int i = 0; i < height; i++) {
        hueMask.add(new IntegerValueRow(width));
        valueMask.add(new IntegerValueRow(width));
        matrixValue.add(new IntegerValueRow(width));
    }
    // fill values to masks
    fillMasks();
    randomSeed(analogRead(0));
    generateLine();
}

void FirePattern::loop(long millis, LinkedList<LedRGB*> &leds) {
    if (pcnt >= 100) {
        shiftUp();
        generateLine();
        pcnt = 0;
    }
    drawFrame(pcnt, leds);
    pcnt += 30;
}

void FirePattern::generateLine() {
    for (uint8_t x = 0; x < width; x++) {
        line->set(x, random(64, 255));
    }
}

void FirePattern::shiftUp() {
    for (uint8_t y = height - 1; y > 0; y--) {
        matrixValue.get(y)->copyData(matrixValue.get(y - 1));
    }
    matrixValue.set(0, line);
}

int FirePattern::calculateHueValue(int hueMaskValue) {
    if (baseColorMode) {
        double h = 0;
        double s = 0;
        double v = 0;
        rgbToHsv(baseColorR, baseColorG, baseColorB, h, s, v);
        int usedBase = h * 255;
        if (usedBase + hueMaskValue > 255) {
            return (usedBase - hueMaskValue) - 255;
        }
        return usedBase + hueMaskValue;
    }
    return hueMaskValue;
}

void FirePattern::drawFrame(int pcnt, LinkedList<LedRGB *> &leds) {
    int nextv;

    //each row interpolates with the one before it
    for (unsigned char y = height - 1; y > 0; y--) {
        for (unsigned char x = 0; x < width; x++) {
            nextv =
                    (((100.0 - pcnt) * matrixValue.get(y)->get(x)
                      + pcnt * matrixValue.get(y - 1)->get(x)) / 100.0)
                    - valueMask.get(y)->get(x);
            if (x < width) {
                // draw only when inside of pattern range
                // mirroring y axis
                int translatedY = (height - 1) - y;
                int index = translatedY * width + x;

                int reversedIndex = 0;
                if (reversedMode) {
                    reversedIndex = leds.size() - (index + 1);
                } else {
                    reversedIndex = index;
                }

                LedRGB *led = leds.get(reversedIndex);
                HSVtoRGB(calculateHueValue(hueMask.get(y)->get(x)), 255, (uint8_t) max(0, nextv), led);
            }
        }
    }
    //first row interpolates with the "next" line
    for (unsigned char x = 0; x < width; x++) {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;

        if (x < width) {
            int translatedX = (height - 1) * width + x;

            int reversedIndex = 0;

            if (reversedMode) {
                reversedIndex = leds.size() - (translatedX + 1);
            } else {
                reversedIndex = translatedX;
            }

            LedRGB *led = leds.get(reversedIndex);
            HSVtoRGB(calculateHueValue(hueMask.get(0)->get(x)), 255,
                     (uint8_t)(((100.0 - pcnt) * matrixValue.get(0)->get(x) + pcnt * line->get(x)) / 100.0), led);
        }
    }
}

void FirePattern::fillMasks() {
    int lastPosition = 0;
    for (int i = 0; i < height; i++) {
        int e = hueData.indexOf(";", lastPosition);
        if (e == -1) {
            e = hueData.length();
        }
        String row = hueData.substring(lastPosition, e);

        int lastRowPos = 0;
        for (int j = 0; j < width; j++) {
            int s = row.indexOf(",", lastRowPos);
            if (s == -1) {
                s = row.length();
            }
            String v = row.substring(lastRowPos, s);
            lastRowPos = s + 1;

            hueMask.get(i)->set(j, v.toInt());
        }
        lastPosition = e + 1;
    }

    // fill valueMask
    lastPosition = 0;
    for (int i = 0; i < height; i++) {
        int e = valueData.indexOf(";", lastPosition);
        if (e == -1) {
            e = valueData.length();
        }
        String row = valueData.substring(lastPosition, e);

        int lastRowPos = 0;
        for (int j = 0; j < width; j++) {
            int s = row.indexOf(",", lastRowPos);
            if (s == -1) {
                s = row.length();
            }
            String v = row.substring(lastRowPos, s);
            lastRowPos = s + 1;

            valueMask.get(i)->set(j, v.toInt());
        }
        lastPosition = e + 1;
    }
}

String FirePattern::getBaseJson() const {
    static const char *DEFAULT_HUE_MASK = R"(
  1,11,19,25,25,22,11,1;
  1, 8,13,19,25,19, 8,1;
  1, 8,13,16,19,16, 8,1;
  1, 5,11,13,13,13, 5,1;
  1, 5,11,11,11,11, 5,1;
  0, 1, 5, 8, 8, 5, 1,0;
  0, 0, 1, 5, 5, 1, 0,0;
  0, 0, 0, 1, 1, 0, 0,0;)";

    static const char *DEFAULT_VALUE_MASK =
            " 32,  0,  0,  0,  0,  0,  0, 32;"
			" 64,  0,  0,  0,  0,  0,  0, 64;"
			" 96, 32,  0,  0,  0,  0, 32, 96;"
            "128, 64, 32,  0,  0, 32, 64,128;"
			"160, 96, 64, 32, 32, 64, 96,160;"
			"192,128, 96, 64, 64, 96,128,192;"
            "255,160,128, 96, 96,128,160,255;"
			"255,192,160,128,128,160,192,255;";

    StaticJsonDocument<2048> doc;
    doc["name"] = "Fire Pattern";
    doc["type"] = "Fire";
    doc["active"] = 1;
    doc["width"] = 8;
    doc["height"] = 8;
    doc["hue_mask"] = DEFAULT_HUE_MASK;
    doc["value_mask"] = DEFAULT_VALUE_MASK;
    doc["base_mode"] = 1;
    doc["reverse_mode"] = 0;

    String output;
    serializeJson(doc, output);
    return output;
}
void FirePattern::HSVtoRGB(uint8_t ih, uint8_t is, uint8_t iv, LedRGB *led) {
    float r, g, b, h, s, v; //this function works with floats between 0 and 1
    float f, p, q, t;
    int i;

    h = (float) (ih / 256.0);
    s = (float) (is / 256.0);
    v = (float) (iv / 256.0);

    //if saturation is 0, the color is a shade of grey
    if (s == 0.0) {
        b = v;
        g = b;
        r = g;
    }
        //if saturation > 0, more complex calculations are needed
    else {
        h *= 6.0; //to bring hue to a number between 0 and 6, better for the calculations
        i = (int) (floor(h)); //e.g. 2.7 becomes 2 and 3.01 becomes 3 or 4.9999 becomes 4
        f = h - i;//the fractional part of h

        p = (float) (v * (1.0 - s));
        q = (float) (v * (1.0 - (s * f)));
        t = (float) (v * (1.0 - (s * (1.0 - f))));

        switch (i) {
            case 0:
                r = v;
                g = t;
                b = p;
                break;
            case 1:
                r = q;
                g = v;
                b = p;
                break;
            case 2:
                r = p;
                g = v;
                b = t;
                break;
            case 3:
                r = p;
                g = q;
                b = v;
                break;
            case 4:
                r = t;
                g = p;
                b = v;
                break;
            case 5:
                r = v;
                g = p;
                b = q;
                break;
            default:
                r = g = b = 0;
                break;
        }
    }
    led->r = r*255;
    led->g = g*255;
    led->b = b*255;
}
