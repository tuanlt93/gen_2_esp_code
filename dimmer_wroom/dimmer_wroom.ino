#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_dmx.h>
#include <esp_task_wdt.h>

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
#define PHYSICAL_MIN 5
#define PHYSICAL_MAX 255

// Ngưỡng thời gian tối thiểu để thực hiện hiệu ứng Fade (ms)
#define MIN_FADE_DURATION 800

String BOARD_ID = "";
uint8_t dmx_data[DMX_PACKET_SIZE];
unsigned long lastFeedbackTime = 0;

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

/**
 * PHẢN HỒI THÔNG TIN (INFO)
 */
void sendInfo() {
    JsonDocument doc;
    doc["cmd"] = "info";
    JsonObject data = doc.createNestedObject("data");
    data["name"] = BOARD_ID;
    
    JsonArray outputs = data.createNestedArray("output");
    for (int i = 1; i <= NUM_CHANNELS; i++) {
        outputs.add("CH" + String(i));
    }
    
    serializeJson(doc, Serial);
    Serial.println();
}

/**
 * PHẢN HỒI TRẠNG THÁI (FEEDBACK)
 */
void sendFeedback() {
    JsonDocument doc;
    doc["cmd"] = "feedback";
    JsonObject msg = doc.createNestedObject("msg");
    JsonObject dataObj = msg.createNestedObject("data");
    JsonObject outputs = dataObj.createNestedObject("output");

    for (int i = 0; i < NUM_CHANNELS; i++) {
        String label = "CH" + String(i + 1);
        outputs[label] = fadeTasks[i].currentVal;
    }
    
    serializeJson(doc, Serial);
    Serial.println();
}

/**
 * XỬ LÝ LỆNH JSON
 */
void handleJsonCommand(String input) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, input);
    if (err) return;

    const char* cmd = doc["cmd"];
    if (!cmd) return;

    if (strcmp(cmd, "dimmer") == 0) {
        JsonObject msg = doc["msg"];
        int idx = getChannelIndex(msg["channel"] | "");
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

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(50);

    BOARD_ID = "DIMER_BI_MAT";
    // BOARD_ID = getChipID();

    esp_task_wdt_init(8, true);
    esp_task_wdt_add(NULL);

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

    if (now - lastFeedbackTime >= 1000) {
        // sendFeedback();
        esp_task_wdt_reset();
        lastFeedbackTime = now;
    }

    delay(25); 
}