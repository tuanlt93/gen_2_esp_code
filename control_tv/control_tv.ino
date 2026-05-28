// Định nghĩa chân điều khiển là chân số 8
const int controlPin = 8;

void setup() {
  // Cấu hình chân số 8 là OUTPUT (ngõ ra)
  pinMode(controlPin, OUTPUT);
  
  // 1. Ngay khi có nguồn khởi động, bật chân 8 lên mức HIGH
  digitalWrite(controlPin, HIGH);
  
  // 2. Chờ trong 3 giây (3000 mili-giây)
  delay(2000);
  
  // 3. Sau 3 giây, chuyển chân 8 về mức LOW
  digitalWrite(controlPin, LOW);
}

void loop() {
  // Hàm loop để trống vì chân 8 sẽ luôn giữ mức LOW 
  // và không cần thực hiện thêm hành động nào khác.
  delay(20);
}