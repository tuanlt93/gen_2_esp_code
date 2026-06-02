#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

/**
 * CẤU HÌNH HỆ THỐNG CHO ESP32-S3
 */

String BOARD_ID = "";

// Logic Relay
#define RELAY_ON    LOW
#define RELAY_OFF   HIGH

// Trạng thái logic phản hồi
#define IO_ACTIVE   1
#define IO_INACTIVE 0

// Cấu trúc quản lý Input (Digital/Analog & Pull mode)
struct InputConfig {
    int pin;
    char type;    // 'D': Digital, 'A': Analog
    int pinMode;  // INPUT_PULLUP, INPUT_PULLDOWN, hoặc INPUT
};

const InputConfig inputConfigs[] = {
    {4, 'D', INPUT_PULLUP},   
    {5, 'D', INPUT_PULLUP},
    {6, 'D', INPUT_PULLUP},
    {7, 'D', INPUT_PULLUP},
    {15, 'D', INPUT_PULLUP},
    {16, 'D', INPUT_PULLUP},
    {17, 'D', INPUT_PULLUP},
    {18, 'D', INPUT_PULLUP},
    // {41, 'D', INPUT_PULLUP},

};
const int inputCount = sizeof(inputConfigs) / sizeof(inputConfigs[0]);

// Cấu trúc quản lý Output
const int outputPins[] = {3, 8, 9, 10};
// const int outputPins[] = {3, 8, 9, 10, 11, 12, 13, 14, 21, 35, 36, 37, 38};
// const int outputPins[] = {3, 8, 9, 10, 11, 12, 13, 14, 21, 35, 36, 37, 38, 39, 40};
// const int outputPins[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 35, 36, 37, 38, 39, 40};
const int outputCount = sizeof(outputPins) / sizeof(outputPins[0]);

// Quản lý thời gian Active cho Feedback (Chỉ dùng cho Input)
unsigned long inputActiveStartTime[inputCount] = {0};

// Cấu trúc quản lý tác vụ Blink
struct BlinkTask {
    int pin;
    unsigned long interval;
    unsigned long duration;
    unsigned long startTime;
    unsigned long lastToggle;
    bool active;
    bool paused;
    bool state;
};

BlinkTask blinkTasks[outputCount];
unsigned long lastFeedbackTime = 0;

/**
 * CÁC HÀM HỖ TRỢ
 */
int findTaskIndex(int pin) {
    for (int i = 0; i < outputCount; i++) {
        if (outputPins[i] == pin) return i;
    }
    return -1;
}

int getPinNumber(const char* pinStr) {
    if (!pinStr || strlen(pinStr) < 2) return -1;
    return atoi(&pinStr[1]);
}

String getPinLabel(InputConfig config) {
    return String(config.type) + String(config.pin);
}

String getOutputLabel(int pin) {
    return "D" + String(pin);
}

String getChipID() {
    uint64_t chipid = ESP.getEfuseMac();
    char idStr[13];
    snprintf(idStr, sizeof(idStr), "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
    return String("S3_") + String(idStr);
}

int getLogicalValue(InputConfig config) {
    if (config.type == 'A') return analogRead(config.pin);
    int raw = digitalRead(config.pin);
    if (config.pinMode == INPUT_PULLUP) {
        return (raw == LOW) ? IO_ACTIVE : IO_INACTIVE;
    } else {
        return (raw == HIGH) ? IO_ACTIVE : IO_INACTIVE;
    }
}

unsigned long calculateTimeHold(int logicalVal, unsigned long &startTime, unsigned long now) {
    if (logicalVal == IO_ACTIVE) {
        if (startTime == 0) startTime = now;
        return now - startTime;
    } else {
        startTime = 0;
        return 0;
    }
}

/**
 * GỬI THÔNG TIN THIẾT BỊ (INFO)
 */
void sendInfo() {
    JsonDocument doc; 
    doc["cmd"] = "info";
    JsonObject data = doc.createNestedObject("data");
    data["name"] = BOARD_ID; 

    JsonArray inputs = data.createNestedArray("input");
    for (int i = 0; i < inputCount; i++) {
        inputs.add(getPinLabel(inputConfigs[i]));
    }

    JsonArray outputs = data.createNestedArray("output");
    for (int i = 0; i < outputCount; i++) {
        outputs.add(getOutputLabel(outputPins[i]));
    }

    serializeJson(doc, Serial);
    Serial.println();
}

/**
 * GỬI DỮ LIỆU PHẢN HỒI (FEEDBACK)
 */
void sendFeedback() {
    unsigned long now = millis();
    JsonDocument doc;
    doc["cmd"] = "feedback";
    JsonObject msg = doc.createNestedObject("msg");
    JsonObject dataObj = msg.createNestedObject("data");
    JsonObject timeHoldObj = msg.createNestedObject("time_hold");

    // Input - Data và Time Hold
    JsonObject inputData = dataObj.createNestedObject("input");
    JsonObject inputTime = timeHoldObj.createNestedObject("input");
    for (int i = 0; i < inputCount; i++) {
        String label = getPinLabel(inputConfigs[i]);
        int logicalVal = getLogicalValue(inputConfigs[i]);
        inputData[label] = logicalVal;
        if (inputConfigs[i].type == 'D') {
            inputTime[label] = calculateTimeHold(logicalVal, inputActiveStartTime[i], now);
        } else {
            inputTime[label] = 0;
        }
    }

    // Output - Chỉ gửi Data, không gửi Time Hold
    JsonObject outputData = dataObj.createNestedObject("output");
    for (int i = 0; i < outputCount; i++) {
        int pin = outputPins[i];
        String label = getOutputLabel(pin);
        int logicalVal = (digitalRead(pin) == RELAY_ON) ? IO_ACTIVE : IO_INACTIVE;
        outputData[label] = logicalVal;
    }

    serializeJson(doc, Serial);
    Serial.println();
}

/**
 * XỬ LÝ LỆNH JSON
 */
void handleJsonCommand(String input) {
    JsonDocument doc;
    if (deserializeJson(doc, input)) return;

    const char* cmd = doc["cmd"];
    if (!cmd) return;

    if (strcmp(cmd, "io") == 0) {
        JsonObject msg = doc["msg"];
        for (JsonPair kv : msg) {
            int pin = getPinNumber(kv.key().c_str());
            int val = kv.value().as<int>();
            int idx = findTaskIndex(pin);
            if (idx != -1) {
                blinkTasks[idx].active = false; 
                digitalWrite(pin, (val == IO_ACTIVE) ? RELAY_ON : RELAY_OFF);
            }
        }
    } 
    else if (strcmp(cmd, "blink") == 0) {
        JsonObject msg = doc["msg"];
        int pin = getPinNumber(msg["io"] | "");
        int idx = findTaskIndex(pin);
        if (idx != -1) {
            blinkTasks[idx].interval = msg["interval"] | 500;
            blinkTasks[idx].duration = msg["duration"] | 5000;
            blinkTasks[idx].startTime = millis();
            blinkTasks[idx].lastToggle = 0;
            blinkTasks[idx].active = true;
            blinkTasks[idx].paused = false;
        }
    }
    else if (strcmp(cmd, "blink_pause") == 0) {
        int pin = getPinNumber(doc["msg"]["io"] | "");
        int idx = findTaskIndex(pin);
        if (idx != -1) blinkTasks[idx].paused = true;
    }
    else if (strcmp(cmd, "blink_resume") == 0) {
        int pin = getPinNumber(doc["msg"]["io"] | "");
        int idx = findTaskIndex(pin);
        if (idx != -1) blinkTasks[idx].paused = false;
    }
    else if (strcmp(cmd, "blink_stop") == 0) {
        int pin = getPinNumber(doc["msg"]["io"] | "");
        int idx = findTaskIndex(pin);
        if (idx != -1) {
            blinkTasks[idx].active = false;
            digitalWrite(pin, RELAY_OFF);
        }
    }
    else if (strcmp(cmd, "status") == 0) {
        sendFeedback();
    }
    else if (strcmp(cmd, "info") == 0) {
        sendInfo();
    }
    else if (strcmp(cmd, "reset") == 0) {
        ESP.restart();
    }
}

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(50);
    
    BOARD_ID = "S3_SDD";
    // BOARD_ID = getChipID();

    esp_task_wdt_init(8, true);
    esp_task_wdt_add(NULL);

    for (int i = 0; i < inputCount; i++) {
        pinMode(inputConfigs[i].pin, inputConfigs[i].pinMode);
    }

    for (int i = 0; i < outputCount; i++) {
        pinMode(outputPins[i], OUTPUT);
        digitalWrite(outputPins[i], RELAY_OFF);
        blinkTasks[i].pin = outputPins[i];
        blinkTasks[i].active = false;
        blinkTasks[i].paused = false;
    }
}

void loop() {
    if (Serial.available() > 0) {
        handleJsonCommand(Serial.readStringUntil('\n'));
    }

    unsigned long now = millis();

    for (int i = 0; i < outputCount; i++) {
        if (blinkTasks[i].active) {
            if (now - blinkTasks[i].startTime >= blinkTasks[i].duration) {
                blinkTasks[i].active = false;
                digitalWrite(blinkTasks[i].pin, RELAY_OFF);
            } 
            else if (!blinkTasks[i].paused) {
                if (now - blinkTasks[i].lastToggle >= blinkTasks[i].interval) {
                    blinkTasks[i].state = !blinkTasks[i].state;
                    digitalWrite(blinkTasks[i].pin, blinkTasks[i].state ? RELAY_ON : RELAY_OFF);
                    blinkTasks[i].lastToggle = now;
                }
            }
        }
    }

    if (now - lastFeedbackTime >= 1000) {
        esp_task_wdt_reset();
        lastFeedbackTime = now;
    }
}