#include <RCSwitch.h>

#include <RCSwitch.h>

RCSwitch mySwitch = RCSwitch();

// Thử dùng chân GPIO 13 hoặc 27 (Tránh các chân 0, 2, 5, 12 lúc khởi động)
const int RF_RECEIVER_PIN = 13; 

void setup() {
  Serial.begin(115200);
  
  // Đảm bảo chân DATA là Input
  pinMode(RF_RECEIVER_PIN, INPUT);

  // Core 2.x đôi khi cần xác định rõ số Interrupt
  // digitalPinToInterrupt(13) thường trả về đúng số GPIO trên ESP32
  mySwitch.enableReceive(digitalPinToInterrupt(RF_RECEIVER_PIN));
  
  Serial.println("--- San sang nhan tin hieu (Core 2.0.17) ---");
}

void loop() {
  if (mySwitch.available()) {
    Serial.print("Da nhan: ");
    Serial.println(mySwitch.getReceivedValue());
    mySwitch.resetAvailable();
  }
}