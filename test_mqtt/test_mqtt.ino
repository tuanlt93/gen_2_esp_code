#include <WiFi.h>
#include <PubSubClient.h>

// Cấu hình WiFi
const char* ssid = "WIFI_XFILES2";
const char* password = "joybit2025";

// Cấu hình MQTT Broker
const char* mqtt_server = "192.168.1.5";
const int mqtt_port = 1883;
const char* mqtt_topic = "/topic/info";

WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastMsg = 0;
const long interval = 2000; // Gửi dữ liệu mỗi 2 giây (2000ms)

// Hàm khởi tạo kết nối WiFi
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

// Hàm kết nối lại MQTT Broker khi bị mất kết nối
void reconnect() {
  // Lặp cho đến khi kết nối lại được
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    // Tạo một Client ID ngẫu nhiên cho ESP32-S3
    String clientId = "ESP32S3Client-";
    clientId += String(random(0xffff), HEX);
    
    // Thử kết nối
    if (client.connect(clientId.c_str())) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Đợi 5 giây trước khi thử lại
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  // Khởi tạo WiFi
  setup_wifi();
  
  // Cấu hình MQTT server và port
  client.setServer(mqtt_server, mqtt_port);
  
  // Khởi tạo hàm random bằng cách đọc nhiễu từ chân ADC trống
  randomSeed(analogRead(0));
}

void loop() {
  // Kiểm tra kết nối MQTT
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  // Gửi dữ liệu theo chu kỳ không dùng delay() để tránh block loop
  unsigned long now = millis();
  if (now - lastMsg >= interval) {
    lastMsg = now;

    // Tạo giá trị random (ví dụ từ 0 đến 100)
    int random_value = random(0, 101);

    // Chuyển giá trị số sang chuỗi để publish
    char msg[50];
    snprintf(msg, sizeof(msg), "%d", random_value);

    Serial.print("Publish message: ");
    Serial.println(msg);
    
    // Publish lên topic
    client.publish(mqtt_topic, msg);
  }
}