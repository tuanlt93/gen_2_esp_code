#include <Arduino.h>
#include <ArduinoJson.h>
#include <NeoPixelBus.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

/**
 * CẤU HÌNH HỆ THỐNG PIXEL + INPUT:
 * - Board Core: ESP32 v2.0.17
 * - NeoPixelBus: v2.7.6
 */

#define STRIP_COUNT          8
#define NUM_CHANNELS         8
#define MAX_TASKS_PER_PIN    5
#define MAX_LEDS             200
#define COLOR_FEATURE        NeoGrbFeature

// Giới hạn Frame Rate (Tránh lỗi LED không kịp Latch gây chớp màu sai)
#define FRAME_DELAY          25 // 25ms = 40 FPS.

// Logic IO phản hồi
#define IO_ACTIVE   1
#define IO_INACTIVE 0

String BOARD_ID = "";

// Cấu trúc quản lý Input
struct InputConfig {
    int pin;
    char type;    // 'D': Digital, 'A': Analog
    int pinMode;  // INPUT_PULLUP, INPUT_PULLDOWN, hoặc INPUT
};

const InputConfig inputConfigs[] = {
    {4, 'D', INPUT_PULLUP},   
    {5, 'D', INPUT_PULLUP},
    {13, 'D', INPUT_PULLUP},
    {14, 'D', INPUT_PULLUP},
    {32, 'D', INPUT_PULLUP},
    {33, 'D', INPUT_PULLUP}
};
const int inputCount = sizeof(inputConfigs) / sizeof(inputConfigs[0]);

unsigned long inputActiveStartTime[inputCount] = {0};
int lastInputState[inputCount] = {0};

SemaphoreHandle_t serialMutex; // Mutex bảo vệ quá trình ghi Serial khỏi đụng độ

struct StripConfig {
    uint8_t pin;
    const char* label;
};

const StripConfig STRIP_MAP[STRIP_COUNT] = {
    {25, "CH1"}, {26, "CH2"}, {27, "CH3"}, {23, "CH4"},
    {18, "CH5"}, {19, "CH6"}, {21, "CH7"}, {22, "CH8"}
};

// Sử dụng timing chuẩn mặc định của thư viện NeoPixelBus cho WS2812
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
    float speed;
    RgbColor c1;
    RgbColor c2;
    bool color_change;
    uint32_t time_change;
    bool blink;
    int interval;
    uint32_t time_blink;
    uint32_t duration;
    uint32_t offset_ms;
    uint32_t fade_in;
    uint32_t fade_out;
    uint8_t brightness;
    uint32_t start_time;
    bool active;
    bool is_turning_off;
    uint32_t turn_off_start_time;
    float turn_off_speed;
};

LedTask g_tasks[STRIP_COUNT][MAX_TASKS_PER_PIN];
unsigned long lastFrameTime = 0; 

bool g_anyTaskActive = false;

RgbColor hexToRgb(const char* hex) {
    if (!hex) return RgbColor(0);
    long number = strtol(hex, NULL, 16);
    return RgbColor((uint8_t)(number >> 16), (uint8_t)(number >> 8), (uint8_t)(number & 0xFF));
}

int getChannelIndex(const char* chanStr) {
    if (!chanStr || strlen(chanStr) < 3) return -1;
    int idx = atoi(&chanStr[2]) - 1;
    if (idx >= 0 && idx < NUM_CHANNELS) return idx;
    return -1;
}

String getPinLabel(InputConfig config) {
    return String(config.type) + String(config.pin);
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

// XUẤT TUẦN TỰ: Đợi dải LED trước truyền xong tín hiệu mới gửi tín hiệu dải tiếp theo
void showStrip(int idx) {
    switch(idx) {
        case 0: if(strip0) { strip0->Show(); while(!strip0->CanShow()) yield(); } break;
        case 1: if(strip1) { strip1->Show(); while(!strip1->CanShow()) yield(); } break;
        case 2: if(strip2) { strip2->Show(); while(!strip2->CanShow()) yield(); } break;
        case 3: if(strip3) { strip3->Show(); while(!strip3->CanShow()) yield(); } break;
        case 4: if(strip4) { strip4->Show(); while(!strip4->CanShow()) yield(); } break;
        case 5: if(strip5) { strip5->Show(); while(!strip5->CanShow()) yield(); } break;
        case 6: if(strip6) { strip6->Show(); while(!strip6->CanShow()) yield(); } break;
        case 7: if(strip7) { strip7->Show(); while(!strip7->CanShow()) yield(); } break;
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
    
    JsonArray inputs = data.createNestedArray("input");
    for (int i = 0; i < inputCount; i++) {
        inputs.add(getPinLabel(inputConfigs[i]));
    }
    
    JsonArray outputs = data.createNestedArray("output");
    for (int i = 0; i < STRIP_COUNT; i++) {
        outputs.add(STRIP_MAP[i].label);
    }
    
    if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
        serializeJson(doc, Serial);
        Serial.println();
        xSemaphoreGive(serialMutex);
    }
}

void sendFeedback() {
    unsigned long now = millis();
    JsonDocument doc;
    doc["cmd"] = "feedback";
    JsonObject msg = doc.createNestedObject("msg");
    JsonObject dataObj = msg.createNestedObject("data");
    JsonObject timeHoldObj = msg.createNestedObject("time_hold");

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

    dataObj.createNestedObject("output");

    if (xSemaphoreTake(serialMutex, portMAX_DELAY)) {
        serializeJson(doc, Serial);
        Serial.println();
        xSemaphoreGive(serialMutex);
    }
}

void updateGlobalActiveFlag() {
    bool found = false;
    for (int i = 0; i < STRIP_COUNT; i++) {
        for (int s = 0; s < MAX_TASKS_PER_PIN; s++) {
            if (g_tasks[i][s].active) {
                found = true;
                break;
            }
        }
        if (found) break;
    }
    g_anyTaskActive = found;
}

void handleJsonInput(String jsonInput) {
    // Tăng kích thước document lên 2048 để xử lý JSON Array thoải mái
    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, jsonInput);
    if (error) return;

    if (doc.containsKey("cmd")) {
        const char* cmd = doc["cmd"];
        if (strcmp(cmd, "info") == 0) sendInfo();
        else if (strcmp(cmd, "status") == 0) sendFeedback();
        else if (strcmp(cmd, "reset") == 0) ESP.restart();
        return;
    }

    if (doc.containsKey("c")) {
        int cmd = doc["c"];
        
        // Bắt buộc phải có key "ch"
        if (!doc.containsKey("ch")) return;

        // "ch" nay luôn được truyền vào dưới dạng mảng (ví dụ: ["CH5", "CH6"]) để tăng tốc độ xử lý
        JsonArray ch_array = doc["ch"].as<JsonArray>();
        if (ch_array.isNull()) return; // Đảm bảo an toàn, thoát nếu "ch" không phải là mảng
        
        int loop_count = ch_array.size();

        // Lặp qua tất cả các channel được yêu cầu
        for (int k = 0; k < loop_count; k++) {
            const char* ch_label = ch_array[k].as<const char*>();
            int idx = getChannelIndex(ch_label);
            
            // Bỏ qua nếu channel không hợp lệ
            if (idx == -1) continue;

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
                t->speed = doc["spd"].as<float>();
                t->c1 = hexToRgb(doc["c1"]);
                t->c2 = hexToRgb(doc["c2"]);
                t->color_change = (doc["cg"] == 1);
                t->duration = doc["dur"];
                t->offset_ms = doc.containsKey("off") ? doc["off"] : 0; 
                t->time_change = doc.containsKey("t_cg") ? doc["t_cg"] : t->duration;
                t->blink = (doc["blk"] == 1);
                
                int intervalVal = doc.containsKey("int") ? doc["int"] : 0;
                if (t->blink && intervalVal <= 0) intervalVal = 200; 
                t->interval = intervalVal;

                t->time_blink = doc.containsKey("tb") ? doc["tb"] : t->duration;
                t->fade_in = doc.containsKey("fi") ? doc["fi"] : 0;
                t->fade_out = doc.containsKey("fo") ? doc["fo"] : 0;
                t->brightness = doc.containsKey("b") ? doc["b"] : 150;

                t->is_turning_off = false;
                t->turn_off_start_time = 0;
                t->turn_off_speed = 0.0f;

                g_anyTaskActive = true;
            } 
            else if (cmd == 0) { // Stop Task
                bool has_spd = doc.containsKey("spd");
                float spd = has_spd ? doc["spd"].as<float>() : 0.0f;

                if (doc.containsKey("r")) {
                    int r_start = doc["r"][0];
                    int r_end = doc["r"][1];
                    int min_r = min(r_start, r_end);
                    int max_r = max(r_start, r_end);

                    for (int i = 0; i < MAX_TASKS_PER_PIN; i++) {
                        if (g_tasks[idx][i].active) {
                            int t_min = min(g_tasks[idx][i].start_led, g_tasks[idx][i].end_led);
                            int t_max = max(g_tasks[idx][i].start_led, g_tasks[idx][i].end_led);
                            
                            if (t_min <= max_r && t_max >= min_r) {
                                if (has_spd && spd > 0.0f) {
                                    g_tasks[idx][i].is_turning_off = true;
                                    g_tasks[idx][i].turn_off_start_time = millis();
                                    g_tasks[idx][i].turn_off_speed = spd;
                                } else {
                                    g_tasks[idx][i].active = false;
                                }
                            }
                        }
                    }

                    if (!has_spd || spd <= 0.0f) {
                        for (int i = min_r; i <= max_r; i++) {
                            setPixel(idx, i, RgbColor(0));
                        }
                        showStrip(idx); // Được gọi tuần tự nhờ hàm showStrip()
                    }
                    updateGlobalActiveFlag();
                } else {
                    for (int i = 0; i < MAX_TASKS_PER_PIN; i++) {
                        if (g_tasks[idx][i].active) {
                            if (has_spd && spd > 0.0f) {
                                g_tasks[idx][i].is_turning_off = true;
                                g_tasks[idx][i].turn_off_start_time = millis();
                                g_tasks[idx][i].turn_off_speed = spd;
                            } else {
                                g_tasks[idx][i].active = false;
                            }
                        }
                    }
                    if (!has_spd || spd <= 0.0f) {
                        clearStrip(idx); 
                        showStrip(idx); // Được gọi tuần tự nhờ hàm showStrip()
                    }
                    updateGlobalActiveFlag();
                }
            }
        }
    }
}

void update_led_effects() {
    uint32_t now = millis();
    bool foundActive = false;  

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

            foundActive = true;

            uint32_t real_elapsed = now - t->start_time;
            uint32_t effective_elapsed = real_elapsed + t->offset_ms;

            if (!t->is_turning_off && effective_elapsed >= t->duration) { 
                t->active = false; 
                continue; 
            }

            int direction = (t->end_led >= t->start_led) ? 1 : -1;
            int num_leds;
            
            if (t->speed <= 0.0f) {
                num_leds = abs(t->end_led - t->start_led) + 1;
            } else {
                num_leds = (int)((effective_elapsed * t->speed) / 1000.0f);
            }

            int current_end = t->start_led + (num_leds * direction);

            if (direction == 1 && current_end > t->end_led) current_end = t->end_led;
            if (direction == -1 && current_end < t->end_led) current_end = t->end_led;

            int current_start = t->start_led;
            
            if (t->is_turning_off) {
                uint32_t off_elapsed = now - t->turn_off_start_time;
                int off_num_leds = (int)((off_elapsed * t->turn_off_speed) / 1000.0f);
                current_start = t->start_led + (off_num_leds * direction);

                if ((direction == 1 && current_start > current_end) || 
                    (direction == -1 && current_start < current_end)) {
                    t->active = false;
                    continue;
                }
            }

            RgbColor color = t->c2;
            if (t->color_change && t->time_change > 0) {
                float f = (float)effective_elapsed / t->time_change;
                if (f > 1.0f) f = 1.0f;
                color = RgbColor::LinearBlend(t->c1, t->c2, f);
            }

            float fadeFactor = 1.0f;
            if (t->fade_in > 0 && effective_elapsed < t->fade_in) {
                fadeFactor = (float)effective_elapsed / t->fade_in;
            } else if (t->fade_out > 0 && effective_elapsed > (t->duration - t->fade_out)) {
                fadeFactor = (float)(t->duration - effective_elapsed) / t->fade_out;
            }
            fadeFactor = constrain(fadeFactor, 0.0f, 1.0f);

            uint8_t finalBrightness = (uint8_t)(t->brightness * fadeFactor);
            if (finalBrightness < 255) color = color.Dim(finalBrightness);

            bool is_off = false;
            if (t->blink && t->interval > 0 && effective_elapsed < t->time_blink) {
                is_off = (now / t->interval) % 2 == 0;
            }

            if (!is_off) {
                int loop_start = min(current_start, current_end);
                int loop_end = max(current_start, current_end);
                for (int i = loop_start; i <= loop_end; i++) {
                    setPixel(idx, i, color);
                }
            }
        }
        showStrip(idx); // Xuất an toàn, tuần tự
    }

    g_anyTaskActive = foundActive;
}

// Luồng Task xử lý Input chạy độc lập trên Core 0
void inputProcessingTask(void *pvParameters) {
    unsigned long lastCheckTime = 0;
    for (;;) {
        unsigned long now = millis();
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
            if (inputChanged) {
                sendFeedback();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup() {
    Serial.begin(115200);
    Serial.setTimeout(50);

    BOARD_ID = "GAME_SDD";
    esp_task_wdt_init(8, true);
    esp_task_wdt_add(NULL);

    serialMutex = xSemaphoreCreateMutex(); 

    for (int i = 0; i < inputCount; i++) {
        pinMode(inputConfigs[i].pin, inputConfigs[i].pinMode);
        lastInputState[i] = getLogicalValue(inputConfigs[i]); 
    }

    xTaskCreatePinnedToCore(
        inputProcessingTask, 
        "InputTask",         
        4096,                
        NULL,                
        1,                   
        NULL,                
        0                    
    );

    strip0 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt0Ws2812xMethod>(MAX_LEDS, STRIP_MAP[0].pin);
    strip1 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt1Ws2812xMethod>(MAX_LEDS, STRIP_MAP[1].pin);
    strip2 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt2Ws2812xMethod>(MAX_LEDS, STRIP_MAP[2].pin);
    strip3 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt3Ws2812xMethod>(MAX_LEDS, STRIP_MAP[3].pin);
    strip4 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt4Ws2812xMethod>(MAX_LEDS, STRIP_MAP[4].pin);
    strip5 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt5Ws2812xMethod>(MAX_LEDS, STRIP_MAP[5].pin);
    strip6 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt6Ws2812xMethod>(MAX_LEDS, STRIP_MAP[6].pin);
    strip7 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt7Ws2812xMethod>(MAX_LEDS, STRIP_MAP[7].pin);

    if(strip0) { strip0->Begin(); clearStrip(0); showStrip(0); }
    if(strip1) { strip1->Begin(); clearStrip(1); showStrip(1); }
    if(strip2) { strip2->Begin(); clearStrip(2); showStrip(2); }
    if(strip3) { strip3->Begin(); clearStrip(3); showStrip(3); }
    if(strip4) { strip4->Begin(); clearStrip(4); showStrip(4); }
    if(strip5) { strip5->Begin(); clearStrip(5); showStrip(5); }
    if(strip6) { strip6->Begin(); clearStrip(6); showStrip(6); }
    if(strip7) { strip7->Begin(); clearStrip(7); showStrip(7); }

    Serial.println("ESP32 PIXEL CHANNEL CONTROLLER READY.");
}

void loop() {
    if (Serial.available()) {
        handleJsonInput(Serial.readStringUntil('\n'));
    }

    unsigned long now = millis();

    // Cập nhật LED với thời gian trễ chuẩn chống bóp nghẹt Frame
    if (g_anyTaskActive && (now - lastFrameTime >= FRAME_DELAY)) {
        update_led_effects();
        // Lấy thời gian hiện tại SAU KHI đã xuất xong LED để đảm bảo có thời gian nghỉ
        lastFrameTime = millis(); 
    }

    static unsigned long lastWdtTime = 0;
    if (now - lastWdtTime >= 1000) {
        esp_task_wdt_reset(); 
        lastWdtTime = now;
    }
}