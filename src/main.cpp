#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>

class MyTFT : public MCUFRIEND_kbv {
public:
    void fixDims() {
        WIDTH = 320;
        HEIGHT = 400;
        _width = (rotation & 1) ? HEIGHT : WIDTH;
        _height = (rotation & 1) ? WIDTH : HEIGHT;
    }
};

MyTFT tft;

void setup() {
    tft.begin(0x9481);
    tft.setRotation(1);
    tft.fixDims();               // panel is 400px wide in landscape
    tft.invertDisplay(true);     // correct inverted color polarity
    tft.setTextWrap(false);
    tft.fillScreen(0x0000);

    tft.drawRect(0, 0, tft.width(), tft.height(), 0x07E0);

    const char *title = "EDGE TO EDGE!";
    tft.setTextColor(0x07E0);
    tft.setTextSize(3);
    int16_t x1, y1;
    uint16_t tw, th;
    tft.getTextBounds(title, 0, 0, &x1, &y1, &tw, &th);
    tft.setCursor((tft.width() - tw) / 2, 40);
    tft.println(title);

    char buf[24];
    snprintf(buf, sizeof(buf), "%d x %d", tft.width(), tft.height());
    tft.setTextSize(2);
    tft.getTextBounds(buf, 0, 0, &x1, &y1, &tw, &th);
    tft.setCursor((tft.width() - tw) / 2, 90);
    tft.print(buf);
}

void loop() {
}
