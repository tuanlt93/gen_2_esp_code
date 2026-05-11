#include <Arduino.h>
#include <ArduinoJson.h>
#include <NeoPixelBus.h>
#include <esp_task_wdt.h>

/**
 * CẤU HÌNH HỆ THỐNG PIXEL (FIX LỖI BIÊN DỊCH):
 * - Board Core: ESP32 v2.0.17
 * - NeoPixelBus: v2.7.6
 */

#define STRIP_COUNT          8
#define NUM_CHANNELS         8
#define MAX_TASKS_PER_PIN    10   
#define MAX_LEDS             500
#define COLOR_FEATURE        NeoBgrFeature 

String BOARD_ID = "";

// Cấu trúc để ánh xạ Chân và Tên Kênh
struct StripConfig {
    uint8_t pin;
    const char* label;
};

// Khai báo bảng ánh xạ (Theo yêu cầu của bạn)
// Thứ tự trong mảng tương ứng với Index 0 -> CH1, 1 -> CH2, ...
const StripConfig STRIP_MAP[STRIP_COUNT] = {
    {25, "CH1"}, 
    {26, "CH2"}, 
    {27, "CH3"}, 
    {23, "CH4"},
    {18, "CH5"}, 
    {19, "CH6"}, 
    {21, "CH7"}, 
    {22, "CH8"}
};

// Khai báo các dải LED RMT riêng biệt (vì mỗi kênh là một kiểu dữ liệu khác nhau)
NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt0Ws2812xMethod>* strip0 = NULL;
NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt1Ws2812xMethod>* strip1 = NULL;
NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt2Ws2812xMethod>* strip2 = NULL;
NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt3Ws2812xMethod>* strip3 = NULL;
NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt4Ws2812xMethod>* strip4 = NULL;
NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt5Ws2812xMethod>* strip5 = NULL;
NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt6Ws2812xMethod>* strip6 = NULL;
NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt7Ws2812xMethod>* strip7 = NULL;

struct LedTask {
    int start_led;
    int end_led;
    int speed;
    RgbColor c1;
    RgbColor c2;
    bool color_change;
    uint32_t time_change;
    bool blink;
    int interval;
    uint32_t time_blink;
    uint32_t duration;
    uint32_t fade_in;
    uint32_t fade_out;
    uint8_t brightness;
    uint32_t start_time;
    bool active;
};

LedTask g_tasks[STRIP_COUNT][MAX_TASKS_PER_PIN];
unsigned long lastFeedbackTime = 0;

/**
 * HÀM HỖ TRỢ
 */
RgbColor hexToRgb(const char* hex) {
    if (!hex) return RgbColor(0);
    long number = strtol(hex, NULL, 16);
    return RgbColor((uint8_t)(number >> 16), (uint8_t)(number >> 8), (uint8_t)(number & 0xFF));
}

String getChipID() {
    uint64_t chipid = ESP.getEfuseMac();
    char idStr[13];
    snprintf(idStr, sizeof(idStr), "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);
    return String("PIXEL_") + String(idStr);
}

/**
 * HÀM LẤY INDEX KÊNH (Dựa trên chuỗi "CH1", "CH2"...)
 */
int getChannelIndex(const char* chanStr) {
    if (!chanStr || strlen(chanStr) < 3) return -1;
    // Chấp nhận cả "CH1", "ch1", "CH01"...
    int idx = atoi(&chanStr[2]) - 1;
    if (idx >= 0 && idx < NUM_CHANNELS) return idx;
    return -1;
}

/**
 * CÁC HÀM WRAPPER ĐỂ TRUY CẬP LED THEO INDEX
 */
void showStrip(int idx) {
    switch(idx) {
        case 0: if(strip0) strip0->Show(); break;
        case 1: if(strip1) strip1->Show(); break;
        case 2: if(strip2) strip2->Show(); break;
        case 3: if(strip3) strip3->Show(); break;
        case 4: if(strip4) strip4->Show(); break;
        case 5: if(strip5) strip5->Show(); break;
        case 6: if(strip6) strip6->Show(); break;
        case 7: if(strip7) strip7->Show(); break;
    }
}

void setPixel(int idx, uint16_t index, RgbColor color) {
    if (index >= MAX_LEDS) return;
    switch(idx) {
        case 0: if(strip0) strip0->SetPixelColor(index, color); break;
        case 1: if(strip1) strip1->SetPixelColor(index, color); break;
        case 2: if(strip2) strip2->SetPixelColor(index, color); break;
        case 3: if(strip3) strip3->SetPixelColor(index, color); break;
        case 4: if(strip4) strip4->SetPixelColor(index, color); break;
        case 5: if(strip5) strip5->SetPixelColor(index, color); break;
        case 6: if(strip6) strip6->SetPixelColor(index, color); break;
        case 7: if(strip7) strip7->SetPixelColor(index, color); break;
    }
}

void clearStrip(int idx) {
    switch(idx) {
        case 0: if(strip0) strip0->ClearTo(RgbColor(0)); break;
        case 1: if(strip1) strip1->ClearTo(RgbColor(0)); break;
        case 2: if(strip2) strip2->ClearTo(RgbColor(0)); break;
        case 3: if(strip3) strip3->ClearTo(RgbColor(0)); break;
        case 4: if(strip4) strip4->ClearTo(RgbColor(0)); break;
        case 5: if(strip5) strip5->ClearTo(RgbColor(0)); break;
        case 6: if(strip6) strip6->ClearTo(RgbColor(0)); break;
        case 7: if(strip7) strip7->ClearTo(RgbColor(0)); break;
    }
}

void sendInfo() {
    JsonDocument doc;
    doc["cmd"] = "info";
    JsonObject data = doc.createNestedObject("data");
    data["name"] = BOARD_ID;
    JsonArray outputs = data.createNestedArray("output");
    // Sử dụng label từ STRIP_MAP
    for (int i = 0; i < STRIP_COUNT; i++) {
        outputs.add(STRIP_MAP[i].label);
    }
    serializeJson(doc, Serial);
    Serial.println();
}

void sendFeedback() {
    JsonDocument doc;
    doc["cmd"] = "feedback";
    JsonObject msg = doc.createNestedObject("msg");
    JsonObject dataObj = msg.createNestedObject("data");
    JsonObject outputs = dataObj.createNestedObject("output");

    for (int i = 0; i < STRIP_COUNT; i++) {
        bool active = false;
        for (int s = 0; s < MAX_TASKS_PER_PIN; s++) {
            if (g_tasks[i][s].active) { active = true; break; }
        }
        outputs[STRIP_MAP[i].label] = active ? 1 : 0;
    }
    serializeJson(doc, Serial);
    Serial.println();
}

void handleJsonInput(String jsonInput) {
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, jsonInput);
    if (error) return;

    if (doc.containsKey("cmd")) {
        const char* cmd = doc["cmd"];
        if (strcmp(cmd, "info") == 0) sendInfo();
        // else if (strcmp(cmd, "status") == 0) sendFeedback();
        else if (strcmp(cmd, "reset") == 0) ESP.restart();
        return;
    }

    if (doc.containsKey("c")) {
        int cmd = doc["c"];
        const char* ch_label = doc["ch"]; // Chỉ sử dụng phím "ch"
        int idx = getChannelIndex(ch_label);
        if (idx == -1) return;

        if (cmd == 1) { // Start Task
            int slot = -1;
            for (int i = 0; i < MAX_TASKS_PER_PIN; i++) {
                if (!g_tasks[idx][i].active) { slot = i; break; }
            }
            if (slot == -1) slot = 0; 

            LedTask *t = &g_tasks[idx][slot];
            t->active = true;
            t->start_time = millis();
            t->start_led = doc["r"][0];
            t->end_led = doc["r"][1];
            t->speed = doc["spd"];
            t->c1 = hexToRgb(doc["c1"]);
            t->c2 = hexToRgb(doc["c2"]);
            t->color_change = (doc["cg"] == 1);
            t->duration = doc["dur"];
            t->time_change = doc.containsKey("t_cg") ? doc["t_cg"] : t->duration;
            t->blink = (doc["blk"] == 1);
            
            int intervalVal = doc.containsKey("int") ? doc["int"] : 0;
            if (t->blink && intervalVal <= 0) intervalVal = 200; 
            t->interval = intervalVal;

            t->time_blink = doc.containsKey("tb") ? doc["tb"] : t->duration;
            t->fade_in = doc.containsKey("fi") ? doc["fi"] : 0;
            t->fade_out = doc.containsKey("fo") ? doc["fo"] : 0;
            t->brightness = doc.containsKey("b") ? doc["b"] : 150;
        } 
        else if (cmd == 0) { // Stop Task
            for (int i = 0; i < MAX_TASKS_PER_PIN; i++) g_tasks[idx][i].active = false;
            clearStrip(idx); showStrip(idx);
        }
    }
}

void update_led_effects() {
    uint32_t now = millis();
    for (int idx = 0; idx < STRIP_COUNT; idx++) {
        bool has_active = false;
        for (int s = 0; s < MAX_TASKS_PER_PIN; s++) {
            if (g_tasks[idx][s].active) { has_active = true; break; }
        }
        if (!has_active) continue;

        clearStrip(idx); 

        for (int s = 0; s < MAX_TASKS_PER_PIN; s++) {
            LedTask *t = &g_tasks[idx][s];
            if (!t->active) continue;

            uint32_t elapsed = now - t->start_time;
            if (elapsed > t->duration) { t->active = false; continue; }

            int direction = (t->end_led >= t->start_led) ? 1 : -1;
            int num_leds = (elapsed * t->speed) / 1000;
            int current_end = t->start_led + (num_leds * direction);

            if (direction == 1 && current_end > t->end_led) current_end = t->end_led;
            if (direction == -1 && current_end < t->end_led) current_end = t->end_led;

            RgbColor color = t->c2;
            if (t->color_change && t->time_change > 0) {
                float f = (float)elapsed / t->time_change;
                if (f > 1.0f) f = 1.0f;
                color = RgbColor::LinearBlend(t->c1, t->c2, f);
            }

            float fadeFactor = 1.0f;
            if (t->fade_in > 0 && elapsed < t->fade_in) {
                fadeFactor = (float)elapsed / t->fade_in;
            } else if (t->fade_out > 0 && elapsed > (t->duration - t->fade_out)) {
                fadeFactor = (float)(t->duration - elapsed) / t->fade_out;
            }
            fadeFactor = constrain(fadeFactor, 0.0f, 1.0f);

            uint8_t finalBrightness = (uint8_t)(t->brightness * fadeFactor);
            if (finalBrightness < 255) color = color.Dim(finalBrightness);

            bool is_off = false;
            if (t->blink && t->interval > 0 && elapsed < t->time_blink) {
                is_off = (now / t->interval) % 2 == 0;
            }

            if (!is_off) {
                int loop_start = min(t->start_led, current_end);
                int loop_end = max(t->start_led, current_end);
                for (int i = loop_start; i <= loop_end; i++) {
                    setPixel(idx, i, color);
                }
            }
        }
        showStrip(idx); 
    }
}

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(50);
    
    BOARD_ID = getChipID();
    esp_task_wdt_init(8, true);
    esp_task_wdt_add(NULL);

    // Khởi tạo các dải LED RMT dựa trên bảng cấu hình STRIP_MAP
    strip0 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt0Ws2812xMethod>(MAX_LEDS, STRIP_MAP[0].pin); // CH1
    strip1 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt1Ws2812xMethod>(MAX_LEDS, STRIP_MAP[1].pin); // CH2
    strip2 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt2Ws2812xMethod>(MAX_LEDS, STRIP_MAP[2].pin); // CH3
    strip3 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt3Ws2812xMethod>(MAX_LEDS, STRIP_MAP[3].pin); // CH4
    strip4 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt4Ws2812xMethod>(MAX_LEDS, STRIP_MAP[4].pin); // CH5
    strip5 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt5Ws2812xMethod>(MAX_LEDS, STRIP_MAP[5].pin); // CH6
    strip6 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt6Ws2812xMethod>(MAX_LEDS, STRIP_MAP[6].pin); // CH7
    strip7 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt7Ws2812xMethod>(MAX_LEDS, STRIP_MAP[7].pin); // CH8

    if(strip0) strip0->Begin(); if(strip1) strip1->Begin();
    if(strip2) strip2->Begin(); if(strip3) strip3->Begin();
    if(strip4) strip4->Begin(); if(strip5) strip5->Begin();
    if(strip6) strip6->Begin(); if(strip7) strip7->Begin();

    Serial.println("ESP32 PIXEL CHANNEL CONTROLLER READY.");
}

void loop() {
    if (Serial.available()) {
        handleJsonInput(Serial.readStringUntil('\n'));
    }

    unsigned long now = millis();
    update_led_effects();

    if (now - lastFeedbackTime >= 1000) {
        // sendFeedback();
        esp_task_wdt_reset();
        lastFeedbackTime = now;
    }
}