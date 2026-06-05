#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_dmx.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

/**
 * CẤU HÌNH DMX CHO ESP32 WROOM
 * TX: 17, EN: 4, 6 Kênh
 * Hỗ trợ tham số offset_ms (off) để dịch chuyển thời điểm bắt đầu Fade.
 */

#define DMX_TX_PIN 17
#define DMX_EN_PIN 4
#define DMX_PORT   DMX_NUM_1
#define NUM_CHANNELS 6

// Cấu hình Scale vật lý để đèn không nháy ở mức thấp
#define PHYSICAL_MIN 10
#define PHYSICAL_MAX 255

// Ngưỡng thời gian tối thiểu để thực hiện hiệu ứng Fade (ms)
#define MIN_FADE_DURATION 1000

// Logic Input
#define IO_ACTIVE   1
#define IO_INACTIVE 0

String BOARD_ID = "";
uint8_t dmx_data[DMX_PACKET_SIZE];
unsigned long lastFeedbackTime = 0;

SemaphoreHandle_t serialMutex; // Mutex bảo vệ quá trình ghi Serial

// Cấu trúc quản lý Input (Digital/Analog & Pull mode)
struct InputConfig {
    int pin;
    char type;    // 'D': Digital, 'A': Analog
    int pinMode;  // INPUT_PULLUP, INPUT_PULLDOWN, hoặc INPUT
};

// Đã loại bỏ Pin 4 vì trùng với DMX_EN_PIN
const InputConfig inputConfigs[] = {
    {32, 'D', INPUT_PULLUP},
    {33, 'D', INPUT_PULLUP}
};
const int inputCount = sizeof(inputConfigs) / sizeof(inputConfigs[0]);

unsigned long inputActiveStartTime[inputCount] = {0};
int lastInputState[inputCount] = {0};

// Cấu trúc quản lý Fade cho từng kênh
struct FadeTask {
    int currentVal;         // Giá trị người dùng (0-255)
    int startVal;
    int stopVal;
    unsigned long startTime;
    unsigned long duration;
    unsigned long offset_ms; // Tham số dịch chuyển thời gian
    unsigned long elapsedAtPause;
    bool active;            // Đang trong quá trình Fade
    bool paused;
};

FadeTask fadeTasks[NUM_CHANNELS];

/**
 * HÀM HỖ TRỢ
 */
String getChipID() {
    uint64_t chipid = ESP.getEfuseMac();
    char idStr[13];
    snprintf(idStr, sizeof(idStr), "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
    return String("DMX_") + String(idStr);
}

// Map 0-255 sang 20-255 cho phần cứng DMX
uint8_t scaleToHardware(int userVal) {
    if (userVal <= 0) return 0;
    return map(userVal, 0, 255, PHYSICAL_MIN, PHYSICAL_MAX);
}

int getChannelIndex(const char* chanStr) {
    if (!chanStr || strlen(chanStr) < 3) return -1;
    // Chấp nhận cả "CH1", "ch1", "CH01"...
    int idx = atoi(&chanStr[2]) - 1;
    if (idx >= 0 && idx < NUM_CHANNELS) return idx;
    return -1;
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
 * PHẢN HỒI THÔNG TIN (INFO)
 */
void sendInfo() {
    JsonDocument doc;
    doc["cmd"] = "info";
    JsonObject data = doc.createNestedObject("data");
    data["name"] = BOARD_ID;
    
    JsonArray inputs = data.createNestedArray("input");
    for (int i = 0; i < inputCount; i++) {
        inputs.add(String(inputConfigs[i].type) + String(inputConfigs[i].pin));
    }
    
    JsonArray outputs = data.createNestedArray("output");
    for (int i = 1; i <= NUM_CHANNELS; i++) {
        outputs.add("CH" + String(i));
    }
    
    if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
        serializeJson(doc, Serial);
        Serial.println();
        xSemaphoreGive(serialMutex);
    }
}

/**
 * PHẢN HỒI TRẠNG THÁI (FEEDBACK)
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
        String label = String(inputConfigs[i].type) + String(inputConfigs[i].pin);
        int logicalVal = getLogicalValue(inputConfigs[i]);
        inputData[label] = logicalVal;
        if (inputConfigs[i].type == 'D') {
            inputTime[label] = calculateTimeHold(logicalVal, inputActiveStartTime[i], now);
        } else {
            inputTime[label] = 0;
        }
    }

    // Output - Dữ liệu độ sáng hiện tại (0-255)
    JsonObject outputs = dataObj.createNestedObject("output");
    for (int i = 0; i < NUM_CHANNELS; i++) {
        String label = "CH" + String(i + 1);
        outputs[label] = fadeTasks[i].currentVal;
    }
    
    if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
        serializeJson(doc, Serial);
        Serial.println();
        xSemaphoreGive(serialMutex);
    }
}

/**
 * XỬ LÝ LỆNH JSON
 */
void handleJsonCommand(String input) {
    // Kích thước 1024 thường là đủ cho bản tin DMX (chỉ cấu hình 1 kênh mỗi lần)
    StaticJsonDocument<1024> doc; 
    DeserializationError err = deserializeJson(doc, input);
    if (err) return;

    const char* cmd = doc["cmd"];
    if (!cmd) return;

    if (strcmp(cmd, "dimmer") == 0) {
        JsonObject msg = doc["msg"];
        int idx = getChannelIndex(msg["channel"] | ""); // Key channel vẫn là String
        if (idx != -1) {
            fadeTasks[idx].stopVal = constrain(msg["brt_stop"] | 0, 0, 255);
            fadeTasks[idx].duration = msg["duration"] | 0;
            fadeTasks[idx].offset_ms = msg["offset"] | 0; // Nhận tham số off (offset_ms)
            
            // Logic bảo vệ: Nếu duration < 800ms hoặc offset >= duration thì thực hiện ngay lập tức
            if (fadeTasks[idx].duration < MIN_FADE_DURATION || fadeTasks[idx].offset_ms >= fadeTasks[idx].duration) {
                fadeTasks[idx].currentVal = fadeTasks[idx].stopVal;
                fadeTasks[idx].active = false;
                fadeTasks[idx].paused = false;
            } else {
                fadeTasks[idx].startVal = constrain(msg["brt_start"] | 0, 0, 255);
                fadeTasks[idx].startTime = millis();
                fadeTasks[idx].elapsedAtPause = 0;
                fadeTasks[idx].active = true;
                fadeTasks[idx].paused = false;
            }
        }
    }
    else if (strcmp(cmd, "dimmer_pause") == 0) {
        int idx = getChannelIndex(doc["msg"]["channel"] | "");
        if (idx != -1 && fadeTasks[idx].active) {
            fadeTasks[idx].paused = true;
            fadeTasks[idx].elapsedAtPause = millis() - fadeTasks[idx].startTime;
        }
    }
    else if (strcmp(cmd, "dimmer_resume") == 0) {
        int idx = getChannelIndex(doc["msg"]["channel"] | "");
        if (idx != -1 && fadeTasks[idx].active && fadeTasks[idx].paused) {
            fadeTasks[idx].paused = false;
            fadeTasks[idx].startTime = millis() - fadeTasks[idx].elapsedAtPause;
        }
    }
    else if (strcmp(cmd, "dimmer_stop") == 0) {
        int idx = getChannelIndex(doc["msg"]["channel"] | "");
        if (idx != -1) {
            fadeTasks[idx].active = false;
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

// Luồng Task xử lý Input độc lập (Core 0)
void inputProcessingTask(void *pvParameters) {
    unsigned long lastCheckTime = 0;
    for (;;) {
        unsigned long now = millis();
        // Quét sự thay đổi của Input với chu kỳ 20ms (debounce)
        if (now - lastCheckTime >= 20) {
            lastCheckTime = now;
            bool inputChanged = false;
            for (int i = 0; i < inputCount; i++) {
                int currentVal = getLogicalValue(inputConfigs[i]);
                if (currentVal != lastInputState[i]) {
                    lastInputState[i] = currentVal;
                    inputChanged = true;
                }
            }
            // Gửi feedback lập tức khi có sự kiện thay đổi
            if (inputChanged) {
                sendFeedback();
            }
        }
        // Yield giúp nhường quyền sử dụng Core 0 cho các tính năng nền của hệ điều hành
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(50);

    BOARD_ID = "DIMER_BI_MAT";
    // BOARD_ID = getChipID();

    esp_task_wdt_init(8, true);
    esp_task_wdt_add(NULL);

    serialMutex = xSemaphoreCreateMutex(); // Khởi tạo Mutex

    for (int i = 0; i < inputCount; i++) {
        pinMode(inputConfigs[i].pin, inputConfigs[i].pinMode);
        lastInputState[i] = getLogicalValue(inputConfigs[i]); // Lấy trạng thái khởi tạo
    }

    // Gắn Task quét Input vào Core 0
    xTaskCreatePinnedToCore(
        inputProcessingTask, 
        "InputTask", 
        4096, 
        NULL, 
        1, 
        NULL, 
        0 // Core 0
    );

    pinMode(DMX_EN_PIN, OUTPUT);
    digitalWrite(DMX_EN_PIN, HIGH);
    dmx_config_t config = DMX_CONFIG_DEFAULT;
    dmx_driver_install(DMX_PORT, &config, NULL, 0);
    dmx_set_pin(DMX_PORT, DMX_TX_PIN, -1, DMX_EN_PIN);

    memset(dmx_data, 0, DMX_PACKET_SIZE);

    for (int i = 0; i < NUM_CHANNELS; i++) {
        fadeTasks[i].currentVal = 0;
        fadeTasks[i].active = false;
        fadeTasks[i].paused = false;
        fadeTasks[i].offset_ms = 0;
    }
}

void loop() {
    if (Serial.available()) {
        String rcv = Serial.readStringUntil('\n');
        handleJsonCommand(rcv);
    }

    unsigned long now = millis();

    // Cập nhật logic Fading của DMX trên Core 1
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (fadeTasks[i].active && !fadeTasks[i].paused) {
            unsigned long real_elapsed = now - fadeTasks[i].startTime;
            unsigned long effective_elapsed = real_elapsed + fadeTasks[i].offset_ms;
            
            if (effective_elapsed >= fadeTasks[i].duration) {
                fadeTasks[i].currentVal = fadeTasks[i].stopVal;
                fadeTasks[i].active = false;
            } else {
                float progress = (float)effective_elapsed / fadeTasks[i].duration;
                fadeTasks[i].currentVal = fadeTasks[i].startVal + (int)((fadeTasks[i].stopVal - fadeTasks[i].startVal) * progress);
            }
        }
        // Luôn cập nhật buffer DMX dựa trên giá trị đã scale
        dmx_data[i + 1] = scaleToHardware(fadeTasks[i].currentVal);
    }

    dmx_write(DMX_PORT, dmx_data, DMX_PACKET_SIZE);
    dmx_send(DMX_PORT);

    static unsigned long lastWdtTime = 0;
    if (now - lastWdtTime >= 1000) {
        esp_task_wdt_reset();
        lastWdtTime = now;
    }

    delay(25); 
}