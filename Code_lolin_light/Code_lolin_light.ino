#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h> // Bạn cần cài đặt thư viện ArduinoJson (phiên bản 6.x hoặc 7.x) trong Library Manager
#include <esp_task_wdt.h>

// ==========================================
// CẤU HÌNH WIFI & MQTT
// ==========================================
const char* ssid = "WIFI_XFILES2";
const char* password = "joybit2025";
const char* mqtt_server = "192.168.1.5";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// ==========================================
// CẤU HÌNH TOPIC & CLIENT ID
// ==========================================
String BOARD_ID = ""; // Sẽ lấy theo địa chỉ MAC của WiFi
String CLIENT_ID = ""; 

String TOPIC_CHECK_INFO = "/topic/check/info";
String TOPIC_INFO = "/topic/info";
String TOPIC_DEVICE = "";     // Sẽ format dạng /topic/device/{CLIENT_ID}
String TOPIC_DEVICE_STT = ""; // Sẽ format dạng /topic/device/status/{CLIENT_ID}

// ==========================================
// LOGIC RELAY & IO
// ==========================================
#define RELAY_ON    HIGH
#define RELAY_OFF   LOW
#define IO_ACTIVE   1
#define IO_INACTIVE 0

// Cấu trúc quản lý Input
struct InputConfig {
    int pin;
    char type;    // 'D': Digital, 'A': Analog
    int pinMode;  
};

// Cấu hình chân theo yêu cầu: D26 là input, D25 là output
const InputConfig inputConfigs[] = {
    {26, 'D', INPUT_PULLUP}
};
const int inputCount = sizeof(inputConfigs) / sizeof(inputConfigs[0]);

const int outputPins[] = {25};
const int outputCount = sizeof(outputPins) / sizeof(outputPins[0]);

// ==========================================
// QUẢN LÝ TRẠNG THÁI (HOLD TIME & BLINKING)
// ==========================================
unsigned long lastFeedbackTime = 0; // Biến đếm thời gian reset Watchdog Timer

// Biến lưu thời gian thay đổi trạng thái cuối cùng (để tính time_hold)
unsigned long lastInputStateChange[inputCount] = {0};
int lastInputState[inputCount] = {0};

unsigned long lastOutputStateChange[outputCount] = {0};
int currentOutputState[outputCount] = {0};

// Cấu trúc quản lý trạng thái Blink cho từng chân Output
struct BlinkState {
    bool isBlinking = false;
    bool isPaused = false;
    unsigned long interval = 0;
    unsigned long duration = 0;
    unsigned long startTime = 0;
    unsigned long lastToggleTime = 0;
    unsigned long runTimeBeforePause = 0; // Lưu thời gian đã chạy nếu bị pause
};
BlinkState blinkStates[outputCount];

// ==========================================
// HÀM BỔ TRỢ (HELPER FUNCTIONS)
// ==========================================

// Trích xuất số pin từ chuỗi (VD: "D25" -> 25)
int getPinFromStr(const char* pinStr) {
    if (pinStr[0] == 'D') {
        return atoi(pinStr + 1);
    }
    return -1;
}

// Tìm index của pin trong mảng output
int getOutputIndex(int pin) {
    for (int i = 0; i < outputCount; i++) {
        if (outputPins[i] == pin) return i;
    }
    return -1;
}

// Gửi bản tin Info
void sendInfo() {
    StaticJsonDocument<512> doc;
    doc["cmd"] = "info";
    
    JsonObject msg = doc.createNestedObject("msg");
    msg["serial_number"] = CLIENT_ID;
    msg["type"] = "serial";
    
    JsonObject data = msg.createNestedObject("data");
    data["name"] = "LOLIN_CONTROL_LIGHT";
    
    JsonArray inputArr = data.createNestedArray("input");
    for (int i = 0; i < inputCount; i++) {
        inputArr.add("D" + String(inputConfigs[i].pin));
    }
    
    JsonArray outputArr = data.createNestedArray("output");
    for (int i = 0; i < outputCount; i++) {
        outputArr.add("D" + String(outputPins[i]));
    }
    
    JsonObject params = msg.createNestedObject("params");
    params["controller"] = "microprocessor";

    char buffer[512];
    serializeJson(doc, buffer);
    client.publish(TOPIC_INFO.c_str(), buffer);
    Serial.println("Published Info to: " + TOPIC_INFO);
}

// Gửi bản tin Status
void sendStatus() {
    StaticJsonDocument<512> doc;
    doc["cmd"] = "feedback";
    
    JsonObject msg = doc.createNestedObject("msg");
    JsonObject data = msg.createNestedObject("data");
    JsonObject inputObj = data.createNestedObject("input");
    JsonObject outputObj = data.createNestedObject("output");
    
    JsonObject timeHoldObj = msg.createNestedObject("time_hold");
    JsonObject timeHoldInputObj = timeHoldObj.createNestedObject("input");

    unsigned long now = millis();

    // Lấy trạng thái và time_hold của Input
    for (int i = 0; i < inputCount; i++) {
        int pin = inputConfigs[i].pin;
        String pinName = "D" + String(pin);
        int state = digitalRead(pin);
        
        // Đảo ngược trạng thái nếu dùng INPUT_PULLUP (nhấn = LOW, nhả = HIGH)
        int logicState = (inputConfigs[i].pinMode == INPUT_PULLUP) ? !state : state;
        
        inputObj[pinName] = logicState;
        
        // Nếu input đang ACTIVE (kéo mát) thì mới tính time_hold, ngược lại trả về 0
        timeHoldInputObj[pinName] = (logicState == IO_ACTIVE) ? (now - lastInputStateChange[i]) : 0;
    }

    // Lấy trạng thái của Output
    for (int i = 0; i < outputCount; i++) {
        int pin = outputPins[i];
        String pinName = "D" + String(pin);
        int state = digitalRead(pin);
        
        outputObj[pinName] = state;
    }

    char buffer[512];
    serializeJson(doc, buffer);
    client.publish(TOPIC_DEVICE_STT.c_str(), buffer);
    Serial.println("Published Status to: " + TOPIC_DEVICE_STT);
}

// ==========================================
// HÀM SETUP & MQTT XỬ LÝ
// ==========================================
void setup_wifi() {
    delay(10);
    Serial.println("\nConnecting to " + String(ssid));
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("\nWiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // Tạo MAC address làm BOARD_ID và CLIENT_ID
    BOARD_ID = WiFi.macAddress();
    BOARD_ID.replace(":", "");
    CLIENT_ID = "LOLIN32_" + BOARD_ID; // Theo code của bạn là ESP32-S3, với wemos lolin32 là esp32 thường

    // Khởi tạo các topic động
    TOPIC_DEVICE = "/topic/device/" + CLIENT_ID;
    TOPIC_DEVICE_STT = "/topic/device/status/" + CLIENT_ID;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    Serial.print("Message arrived [");
    Serial.print(topic);
    Serial.print("] ");

    // Chuyển payload thành String để dễ log (nếu cần)
    String messageTemp;
    for (int i = 0; i < length; i++) {
        messageTemp += (char)payload[i];
    }
    Serial.println(messageTemp);

    // Parse JSON
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
        return;
    }

    String cmd = doc["cmd"].as<String>();
    String topicStr = String(topic);

    // XỬ LÝ: TOPIC_CHECK_INFO
    if (topicStr == TOPIC_CHECK_INFO) {
        if (cmd == "check_info") {
            String type = doc["msg"]["type"].as<String>();
            if (type == "serial") {
                sendInfo();
            }
        }
    } 
    // XỬ LÝ: TOPIC_DEVICE
    else if (topicStr == TOPIC_DEVICE) {
        
        if (cmd == "status") {
            sendStatus();
        } 
        else if (cmd == "info") {
            sendInfo();
        }
        else if (cmd == "reset") {
            Serial.println("Rebooting...");
            delay(1000);
            ESP.restart();
        }
        else if (cmd == "io") {
            JsonObject msgObj = doc["msg"].as<JsonObject>();
            for (JsonPair kv : msgObj) {
                int pin = getPinFromStr(kv.key().c_str());
                int val = kv.value().as<int>();
                int idx = getOutputIndex(pin);
                
                if (idx != -1) {
                    digitalWrite(pin, val);
                    currentOutputState[idx] = val;
                    lastOutputStateChange[idx] = millis();
                    // Tắt blink nếu đang chạy để io ghi đè
                    blinkStates[idx].isBlinking = false; 
                    Serial.printf("Set IO %d to %d\n", pin, val);
                }
            }
            sendStatus(); // Phản hồi lại trạng thái sau khi thay đổi
        }
        else if (cmd == "blink") {
            const char* pinStr = doc["msg"]["io"];
            int pin = getPinFromStr(pinStr);
            int idx = getOutputIndex(pin);
            
            if (idx != -1) {
                blinkStates[idx].isBlinking = true;
                blinkStates[idx].isPaused = false;
                blinkStates[idx].interval = doc["msg"]["interval"] | 500;
                blinkStates[idx].duration = doc["msg"]["duration"] | 0;
                blinkStates[idx].startTime = millis();
                blinkStates[idx].lastToggleTime = millis();
                blinkStates[idx].runTimeBeforePause = 0;
                
                // Bật pin ngay lập tức khi bắt đầu blink
                digitalWrite(pin, RELAY_ON);
                currentOutputState[idx] = RELAY_ON;
                lastOutputStateChange[idx] = millis();
                Serial.printf("Start blink IO %d\n", pin);
            }
        }
        else if (cmd == "blink_pause") {
            const char* pinStr = doc["msg"]["io"];
            int pin = getPinFromStr(pinStr);
            int idx = getOutputIndex(pin);
            
            if (idx != -1 && blinkStates[idx].isBlinking) {
                blinkStates[idx].isPaused = true;
                // Tính thời gian đã chạy
                blinkStates[idx].runTimeBeforePause += (millis() - blinkStates[idx].startTime);
                // Tắt đèn khi pause
                digitalWrite(pin, RELAY_OFF);
                currentOutputState[idx] = RELAY_OFF;
                Serial.printf("Paused blink IO %d\n", pin);
            }
        }
        else if (cmd == "blink_resume") {
            const char* pinStr = doc["msg"]["io"];
            int pin = getPinFromStr(pinStr);
            int idx = getOutputIndex(pin);
            
            if (idx != -1 && blinkStates[idx].isBlinking && blinkStates[idx].isPaused) {
                blinkStates[idx].isPaused = false;
                // Cập nhật lại startTime để trừ đi thời gian đã chạy
                blinkStates[idx].startTime = millis() - blinkStates[idx].runTimeBeforePause;
                blinkStates[idx].lastToggleTime = millis();
                Serial.printf("Resumed blink IO %d\n", pin);
            }
        }
        else if (cmd == "blink_stop") {
            const char* pinStr = doc["msg"]["io"];
            int pin = getPinFromStr(pinStr);
            int idx = getOutputIndex(pin);
            
            if (idx != -1) {
                blinkStates[idx].isBlinking = false;
                digitalWrite(pin, RELAY_OFF);
                currentOutputState[idx] = RELAY_OFF;
                lastOutputStateChange[idx] = millis();
                Serial.printf("Stopped blink IO %d\n", pin);
            }
        }
    }
}

void reconnect() {
    while (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        if (client.connect(CLIENT_ID.c_str())) {
            Serial.println("connected");
            
            // Subscribe các topic sau khi kết nối thành công
            client.subscribe(TOPIC_CHECK_INFO.c_str());
            client.subscribe(TOPIC_DEVICE.c_str());
            
            Serial.println("Subscribed to: " + TOPIC_CHECK_INFO);
            Serial.println("Subscribed to: " + TOPIC_DEVICE);
            
            // Tự động gửi info lúc mới khởi động (Tùy chọn)
            // sendInfo();
        } else {
            Serial.print("failed, rc=");
            Serial.print(client.state());
            Serial.println(" try again in 5 seconds");
            delay(5000);
        }
    }
}

void setup() {
    Serial.begin(115200);

    // Khởi tạo Watchdog Timer (8 giây, true = cho phép panic/reset ESP khi timeout)
    esp_task_wdt_init(8, true);
    esp_task_wdt_add(NULL);

    // Khởi tạo IO
    for (int i = 0; i < inputCount; i++) {
        pinMode(inputConfigs[i].pin, inputConfigs[i].pinMode);
        lastInputState[i] = digitalRead(inputConfigs[i].pin);
        lastInputStateChange[i] = millis();
    }

    for (int i = 0; i < outputCount; i++) {
        pinMode(outputPins[i], OUTPUT);
        digitalWrite(outputPins[i], RELAY_OFF);
        currentOutputState[i] = RELAY_OFF;
        lastOutputStateChange[i] = millis();
    }

    setup_wifi();

    // Cấu hình MQTT
    // Tăng kích thước buffer vì chuỗi JSON response Status/Info khá dài
    client.setBufferSize(1024); 
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(mqttCallback);
}

void loop() {
    if (!client.connected()) {
        reconnect();
    }
    client.loop();

    unsigned long currentMillis = millis();

    // Reset Watchdog Timer mỗi 1000ms
    if (currentMillis - lastFeedbackTime >= 1000) {
        esp_task_wdt_reset();
        lastFeedbackTime = currentMillis;
    }

    // 1. Logic xử lý Input Hold Time (Phát hiện sự thay đổi trạng thái chân Input)
    for (int i = 0; i < inputCount; i++) {
        int pin = inputConfigs[i].pin;
        int currentState = digitalRead(pin);
        
        // Nếu có sự thay đổi logic
        if (currentState != lastInputState[i]) {
            lastInputState[i] = currentState;
            lastInputStateChange[i] = currentMillis;
            
            // Tùy chọn: tự động gửi MQTT báo trạng thái khi có nút bấm thay đổi
            // sendStatus(); 
        }
    }

    // 2. Logic xử lý Blinking Non-blocking
    for (int i = 0; i < outputCount; i++) {
        if (blinkStates[i].isBlinking && !blinkStates[i].isPaused) {
            
            // Kiểm tra thời lượng (duration) - nếu duration > 0 thì kiểm tra xem đã hết giờ chưa
            if (blinkStates[i].duration > 0 && (currentMillis - blinkStates[i].startTime >= blinkStates[i].duration)) {
                // Hết thời gian blink -> Stop
                blinkStates[i].isBlinking = false;
                digitalWrite(outputPins[i], RELAY_OFF);
                currentOutputState[i] = RELAY_OFF;
                lastOutputStateChange[i] = currentMillis;
                Serial.printf("Finished blink IO %d\n", outputPins[i]);
                continue;
            }

            // Xử lý đảo trạng thái (toggle) theo interval
            if (currentMillis - blinkStates[i].lastToggleTime >= blinkStates[i].interval) {
                blinkStates[i].lastToggleTime = currentMillis;
                
                int pin = outputPins[i];
                int state = !digitalRead(pin); // Đảo trạng thái
                digitalWrite(pin, state);
                currentOutputState[i] = state;
            }
        }
    }
}