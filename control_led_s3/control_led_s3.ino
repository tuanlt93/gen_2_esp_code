#include <Arduino.h>
#include <ArduinoJson.h>
#include <NeoPixelBus.h>
#include <esp_task_wdt.h>

/**
 * CẤU HÌNH HỆ THỐNG CHO ESP32-S3 (4 RMT)
 * - Sửa lỗi Blink khi interval = 0.
 * - Tối ưu hóa logic Fade In / Fade Out và Color Change.
 * - Board Core: ESP32-S3 v2.0.17 | NeoPixelBus v2.7.6
 */

#define STRIP_COUNT          4          // Chỉ dùng 4 RMT trên ESP32-S3
#define MAX_TASKS_PER_PIN    10   
#define MAX_LEDS             500

// CẤU HÌNH THỨ TỰ MÀU (COLOR ORDER):
// Đổi NeoGrbFeature thành NeoRgbFeature, NeoBgrFeature hoặc NeoBrgFeature tùy theo loại LED.
#define COLOR_FEATURE        NeoBgrFeature

// Chọn 4 chân GPIO cho 4 dải LED (có thể thay đổi)
const uint8_t LED_GPIO_PINS[STRIP_COUNT] = {18, 19, 20, 21};

unsigned long lastFeedbackTime = 0;

// Khai báo các dải LED RMT cho ESP32-S3 (RMT 0..3)
NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt0Ws2812xMethod>* strip0 = NULL;
NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt1Ws2812xMethod>* strip1 = NULL;
NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt2Ws2812xMethod>* strip2 = NULL;
NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt3Ws2812xMethod>* strip3 = NULL;

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

RgbColor hexToRgb(const char* hex) {
    if (!hex) return RgbColor(0);
    long number = strtol(hex, NULL, 16);
    return RgbColor((uint8_t)(number >> 16), (uint8_t)(number >> 8), (uint8_t)(number & 0xFF));
}

void initWatchdog() {
    esp_task_wdt_init(8, true);
    esp_task_wdt_add(NULL);
}

int get_strip_index(int gpio_pin) {
    for (int i = 0; i < STRIP_COUNT; i++) {
        if (LED_GPIO_PINS[i] == gpio_pin) return i;
    }
    return -1;
}

void showStrip(int idx) {
    switch(idx) {
        case 0: if(strip0) strip0->Show(); break;
        case 1: if(strip1) strip1->Show(); break;
        case 2: if(strip2) strip2->Show(); break;
        case 3: if(strip3) strip3->Show(); break;
    }
}

void setPixel(int idx, uint16_t index, RgbColor color) {
    if (index >= MAX_LEDS) return;
    switch(idx) {
        case 0: strip0->SetPixelColor(index, color); break;
        case 1: strip1->SetPixelColor(index, color); break;
        case 2: strip2->SetPixelColor(index, color); break;
        case 3: strip3->SetPixelColor(index, color); break;
    }
}

void clearStrip(int idx) {
    switch(idx) {
        case 0: if(strip0) strip0->ClearTo(RgbColor(0)); break;
        case 1: if(strip1) strip1->ClearTo(RgbColor(0)); break;
        case 2: if(strip2) strip2->ClearTo(RgbColor(0)); break;
        case 3: if(strip3) strip3->ClearTo(RgbColor(0)); break;
    }
}

void process_json(String jsonInput) {
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, jsonInput);
    if (error) return;

    int cmd = doc["c"];
    int pin = doc["p"];
    int idx = get_strip_index(pin);
    if (idx == -1) return;

    if (cmd == 1) {
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
        t->time_change = doc.containsKey("t_cg") ? doc["t_cg"] : doc["dur"];
        t->blink = (doc["blk"] == 1);
        
        // Sửa lỗi Blink: Nếu blk = 1 nhưng int = 0 thì mặc định 200ms
        int intervalVal = doc.containsKey("int") ? doc["int"] : 0;
        if (t->blink && intervalVal <= 0) intervalVal = 200; 
        t->interval = intervalVal;

        // Thêm tham số time_blink (tb), mặc định là toàn bộ duration nếu không gửi
        t->time_blink = doc.containsKey("tb") ? doc["tb"] : t->duration;

        t->duration = doc["dur"];
        t->fade_in = doc.containsKey("fi") ? doc["fi"] : 0;
        t->fade_out = doc.containsKey("fo") ? doc["fo"] : 0;
        t->brightness = doc.containsKey("b") ? doc["b"] : 150;
    } 
    else if (cmd == 0) {
        for (int i = 0; i < MAX_TASKS_PER_PIN; i++) g_tasks[idx][i].active = false;
        clearStrip(idx); showStrip(idx);
    }
}

void update_led_effects() {
    uint32_t now = millis();
    
    for (int idx = 0; idx < STRIP_COUNT; idx++) {
        bool pin_has_active_task = false;
        for (int s = 0; s < MAX_TASKS_PER_PIN; s++) {
            if (g_tasks[idx][s].active) { pin_has_active_task = true; break; }
        }
        if (!pin_has_active_task) continue;

        clearStrip(idx); 

        for (int s = 0; s < MAX_TASKS_PER_PIN; s++) {
            LedTask *t = &g_tasks[idx][s];
            if (!t->active) continue;

            uint32_t elapsed = now - t->start_time;
            if (elapsed > t->duration) { t->active = false; continue; }

            // 1. Vùng hiển thị
            int direction = (t->end_led >= t->start_led) ? 1 : -1;
            int num_leds = (elapsed * t->speed) / 1000;
            int current_end = t->start_led + (num_leds * direction);

            if (direction == 1 && current_end > t->end_led) current_end = t->end_led;
            if (direction == -1 && current_end < t->end_led) current_end = t->end_led;

            // 2. Màu sắc (Linear Blend)
            RgbColor color = t->c2;
            if (t->color_change && t->time_change > 0) {
                float f = (float)elapsed / t->time_change;
                if (f > 1.0f) f = 1.0f;
                color = RgbColor::LinearBlend(t->c1, t->c2, f);
            }

            // 3. Fade In / Fade Out (Brightness ramp)
            float fadeFactor = 1.0f;
            if (t->fade_in > 0 && elapsed < t->fade_in) {
                fadeFactor = (float)elapsed / t->fade_in;
            } else if (t->fade_out > 0 && elapsed > (t->duration - t->fade_out)) {
                fadeFactor = (float)(t->duration - elapsed) / t->fade_out;
            }
            if (fadeFactor < 0.0f) fadeFactor = 0.0f;
            if (fadeFactor > 1.0f) fadeFactor = 1.0f;

            // 4. Áp dụng Brightness
            uint8_t finalBrightness = (uint8_t)(t->brightness * fadeFactor);
            if (finalBrightness < 255) color = color.Dim(finalBrightness);

            // 5. Blink logic có giới hạn thời gian tb
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

    initWatchdog();

    strip0 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt0Ws2812xMethod>(MAX_LEDS, LED_GPIO_PINS[0]);
    strip1 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt1Ws2812xMethod>(MAX_LEDS, LED_GPIO_PINS[1]);
    strip2 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt2Ws2812xMethod>(MAX_LEDS, LED_GPIO_PINS[2]);
    strip3 = new NeoPixelBus<COLOR_FEATURE, NeoEsp32Rmt3Ws2812xMethod>(MAX_LEDS, LED_GPIO_PINS[3]);

    if(strip0) strip0->Begin(); if(strip1) strip1->Begin(); 
    if(strip2) strip2->Begin(); if(strip3) strip3->Begin();

    Serial.println("ESP32-S3 MULTI-TASK LED (4 RMT) Ready.");
}

void loop() {
    if (millis() - lastFeedbackTime >= 1000) {
        esp_task_wdt_reset();
        lastFeedbackTime = millis();
    }

    if (Serial.available()) {
        String input = Serial.readStringUntil('\n');
        process_json(input);
    }
    update_led_effects();
    delay(1); 
}