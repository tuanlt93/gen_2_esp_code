#include <RCSwitch.h>

RCSwitch mySwitch = RCSwitch();

// Định nghĩa chân DATA là 13 theo yêu cầu của bạn
const int RF_TRANSMIT_PIN = 13; 

void setup() {
  // Chỉnh Baudrate chuẩn 115200 để tránh lỗi font chữ trên Serial Monitor
  Serial.begin(115200);
  delay(1000); // Đợi Serial ổn định

  // Cấu hình chân phát
  mySwitch.enableTransmit(RF_TRANSMIT_PIN);

  /* TÙY CHỈNH CHO CORE 2.0.17:
     Tăng số lần gửi lặp lại để bên thu dễ bắt được ngắt (interrupt)
  */
  mySwitch.setRepeatTransmit(20); 
  
  // Độ rộng xung mặc định thường là 320, bạn có thể thử 350 nếu bên thu khó nhận
  mySwitch.setPulseLength(320);

  // Thử dùng Protocol 1 (phổ biến nhất)
  mySwitch.setProtocol(1);

  Serial.println("--- He thong phat RF433 bat dau (Pin 13) ---");
}

void loop() {
  // Mã số bạn muốn phát (ví dụ: 556677)
  unsigned long codeToSend = 556677; 
  
  Serial.print("Dang phat ma: ");
  Serial.println(codeToSend);

  // Phát mã 24-bit (độ dài chuẩn của hầu hết Remote hiện nay)
  mySwitch.send(codeToSend, 24);

  // Đợi 3 giây phát lại một lần để tránh làm nghẹt sóng
  delay(3000);
}