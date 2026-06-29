#include <Arduino.h>
#include <TFT_eSPI.h>   // Seeed_GFX presents itself under this name

EPaper epd;             // a full-screen drawing buffer for the panel

void setup() {

  Serial.begin(115200);
  delay(1000);
  
  epd.begin();
  epd.setRotation(0); 

  epd.fillScreen(TFT_WHITE);

  // Text  (drawString args: text, x, y, font number)
  epd.setTextColor(TFT_BLACK);
  epd.drawString("Hello from the TRMNL!", 40, 40, 4);
  epd.drawCentreString("800 x 480 e-ink", 400, 120, 4);  // centred on x=400

  // Shapes (pixel coordinates)
  epd.drawRect(40, 200, 300, 150, TFT_BLACK);   // outline box
  epd.fillRect(360, 200, 300, 150, TFT_BLACK);  // solid box
  epd.drawLine(40, 400, 760, 400, TFT_BLACK);   // horizontal rule
  epd.drawCircle(150, 275, 50, TFT_BLACK);      // outline circle
  epd.fillCircle(510, 275, 50, TFT_WHITE);      // white circle inside black box

  epd.update();   // <-- THIS is what actually paints the physical screen
}

void loop() {
  // e-ink is slow and has a limited number of refresh cycles.
  // Draw once, then idle (or deep-sleep). Do NOT redraw in a tight loop.
}