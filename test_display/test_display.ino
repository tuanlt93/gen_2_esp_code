

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define OLED_SDA 5
#define OLED_SCL 4
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// Danh sách các chân ADC khả nghi trên board có đế pin
const int testPins[] = {32, 33, 25, 26, 27, 14};
const int numPins = sizeof(testPins) / sizeof(testPins[0]);

void setup() {
  Serial.begin(115200);
  Wire.begin(OLED_SDA, OLED_SCL);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) for(;;);
  
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
}

void loop() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("--- SCANNING ADC ---");
  
  for (int i = 0; i < numPins; i++) {
    int val = analogRead(testPins[i]);
    
    // In ra Serial để theo dõi kỹ hơn
    Serial.printf("Pin %d: %d | ", testPins[i], val);
    
    // Hiển thị lên OLED theo cột
    display.setCursor(0, 12 + (i * 8));
    display.print("Pin "); display.print(testPins[i]);
    display.print(": "); display.print(val);
    
    // Đánh dấu chân có khả năng là chân Pin (thường > 1500 khi có pin)
    if(val > 1500 && val < 3000) display.print(" <--"); 
  }
  Serial.println();
  
  display.display();
  delay(1000); // Cập nhật mỗi giây
}