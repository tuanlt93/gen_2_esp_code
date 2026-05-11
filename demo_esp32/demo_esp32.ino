#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

/**
 * CẤU HÌNH HỆ THỐNG (MULTI-TASK PER PIN):
 * - Sửa lỗi Blink khi interval = 0.
 * - Tối ưu hóa logic Fade In / Fade Out và Color Change.
 * - Board Core: ESP32 v2.0.17 | NeoPixelBus v2.7.6
 */

String BOARD_ID = ""; // Sẽ được khởi tạo dựa trên ID phần cứng

// Cấu hình Relay
#define RELAY_ON    HIGH
#define RELAY_OFF   LOW

// Định nghĩa các chân Input (Dùng để feedback mỗi giây)
const int inputPins[] = {18, 19, 34}; 
const int inputCount = 3;

// Định nghĩa các chân Output
const int outputPins[] = {2, 12, 13, 14, 15, 26, 27 };
const int outputCount = 6;

// Quản lý tác vụ Blink
struct BlinkTask {
    int pin;
    unsigned long interval;
    unsigned long duration;
    unsigned long startTime;
    unsigned long lastToggle;
    bool active;
    bool state;
};
BlinkTask blinker = {0, 0, 0, 0, 0, false, false};

unsigned long lastFeedbackTime = 0;

void initWatchdog() {
    esp_task_wdt_init(8, true);
    esp_task_wdt_add(NULL);
}

/**
 * HÀM HỖ TRỢ: Lấy ID duy nhất của chip ESP32 (Không đổi)
 */
String getChipID() {
    uint64_t chipid = ESP.getEfuseMac(); // 64-bit ID duy nhất từ eFuse
    char idStr[13];
    // Định dạng thành chuỗi Hex (6 byte MAC)
    snprintf(idStr, sizeof(idStr), "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
    return String("ESP32_") + String(idStr);
}

/**
 * HÀM HỖ TRỢ: Chuyển tên chân "Dxx" thành số nguyên GPIO
 */
int getPinNumber(const char* pinStr) {
    if (pinStr[0] == 'D' || pinStr[0] == 'd') {
        return atoi(&pinStr[1]);
    }
    return -1;
}

/**
 * LỆNH: Gửi thông tin cấu hình Board
 */
void sendInfo() {
    JsonDocument doc; 
    doc["cmd"] = "info";
    JsonObject data = doc.createNestedObject("data");
    
    // Sử dụng BOARD_ID duy nhất của thiết bị
    data["name"] = BOARD_ID; 

    JsonArray inputs = data.createNestedArray("input");
    for (int i = 0; i < inputCount; i++) {
        // SỬA LỖI: Sử dụng inputPins[i] thay vì outputPins[i]
        int p = inputPins[i];
        inputs.add((p >= 32) ? "A" + String(p) : "D" + String(p));
    }

    JsonArray outputs = data.createNestedArray("output");
    for (int i = 0; i < outputCount; i++) {
        outputs.add("D" + String(outputPins[i]));
    }

    serializeJson(doc, Serial);
    Serial.println();
}

/**
 * LỆNH: Gửi dữ liệu phản hồi
 */
void sendFeedback() {
    JsonDocument doc;
    doc["cmd"] = "feedback";
    JsonObject dataObj = doc.createNestedObject("data");
    
    JsonArray inputData = dataObj.createNestedArray("input");
    for (int i = 0; i < inputCount; i++) {
        if (inputPins[i] >= 32) { 
            inputData.add(analogRead(inputPins[i]));
        } else {
            inputData.add(digitalRead(inputPins[i]));
        }
    }

    JsonArray outputData = dataObj.createNestedArray("output");
    for (int i = 0; i < outputCount; i++) {
        outputData.add(digitalRead(outputPins[i]));
    }

    serializeJson(doc, Serial);
    Serial.println();
}

/**
 * XỬ LÝ DỮ LIỆU JSON NHẬN ĐƯỢC
 */
void handleJsonCommand(String input) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, input);
    if (error) return;

    const char* cmd = doc["cmd"];

    if (cmd && strcmp(cmd, "io") == 0) {
        JsonObject data = doc["data"];
        for (JsonPair kv : data) {
            int pin = getPinNumber(kv.key().c_str());
            int val = kv.value().as<int>();
            if (pin != -1) {
                pinMode(pin, OUTPUT);
                digitalWrite(pin, (val) ? RELAY_ON : RELAY_OFF);
            }
        }
    } 
    else if (cmd && strcmp(cmd, "blink") == 0) {
        JsonObject data = doc["data"];
        blinker.pin = getPinNumber(data["io"] | "D2");
        blinker.interval = data["interval"] | 500;
        blinker.duration = data["duration"] | 5000;
        blinker.startTime = millis();
        blinker.lastToggle = 0;
        blinker.active = true;
        pinMode(blinker.pin, OUTPUT);
    } 
    else if (cmd && strcmp(cmd, "info") == 0) {
        sendInfo();
    } 
    else if (cmd && strcmp(cmd, "status") == 0) {
        sendFeedback();
    } 
    else if (cmd && strcmp(cmd, "reset") == 0) {
        Serial.println("{\"status\":\"resetting\"}");
        delay(500);
        ESP.restart();
    }
}

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(75);

    initWatchdog();

    // Lấy ID duy nhất của Board ngay khi khởi động
    BOARD_ID = getChipID();

    // Khởi tạo các chân Input
    for (int i = 0; i < inputCount; i++) {
        pinMode(inputPins[i], INPUT_PULLDOWN);
    }

    // Khởi tạo các chân Output
    for (int i = 0; i < outputCount; i++) {
        pinMode(outputPins[i], OUTPUT);
        digitalWrite(outputPins[i], RELAY_OFF);
    }
}

void loop() {
    if (Serial.available() > 0) {
        String rcv = Serial.readStringUntil('\n');
        handleJsonCommand(rcv);
    }

    if (blinker.active) {
        unsigned long now = millis();
        if (now - blinker.startTime >= blinker.duration) {
            blinker.active = false;
            digitalWrite(blinker.pin, RELAY_OFF); 
        } else {
            if (now - blinker.lastToggle >= blinker.interval) {
                blinker.state = !blinker.state;
                digitalWrite(blinker.pin, blinker.state ? RELAY_ON : RELAY_OFF);
                blinker.lastToggle = now;
            }
        }
    }

    if (millis() - lastFeedbackTime >= 1000) {
        esp_task_wdt_reset();
        lastFeedbackTime = millis();
    }
}