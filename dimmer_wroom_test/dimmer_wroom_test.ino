#include <Arduino.h>
#include <esp_dmx.h>

#define DMX_TX_PIN 17
#define DMX_EN_PIN 4

uint8_t dmx_data[DMX_PACKET_SIZE];

// fade control
int startValue = 0;
int targetValue = 0;
unsigned long fadeStartTime = 0;
unsigned long fadeDuration = 0;
bool isFading = false;

void setup() {
  Serial.begin(115200);

  pinMode(DMX_EN_PIN, OUTPUT);
  digitalWrite(DMX_EN_PIN, HIGH);

  dmx_config_t config = DMX_CONFIG_DEFAULT;
  dmx_driver_install(DMX_NUM_1, &config, NULL, 0);

  dmx_set_pin(DMX_NUM_1, DMX_TX_PIN, -1, DMX_EN_PIN);

  memset(dmx_data, 0, DMX_PACKET_SIZE);

  Serial.println("Nhap dang: [start,end,time_ms]");
}

void loop() {

  // ===== nhận input =====
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');

    int s, e, t;
    if (sscanf(input.c_str(), "[%d,%d,%d]", &s, &e, &t) == 3) {

      startValue = constrain(s, 0, 255);
      targetValue = constrain(e, 0, 255);
      fadeDuration = t;

      fadeStartTime = millis();
      isFading = true;

      Serial.printf("Fade %d -> %d in %lu ms\n", startValue, targetValue, fadeDuration);
    }
  }

  // ===== xử lý fade =====
  if (isFading) {
    unsigned long now = millis();
    unsigned long elapsed = now - fadeStartTime;

    if (elapsed >= fadeDuration) {
      dmx_data[1] = targetValue;
      isFading = false;
    } else {
      float progress = (float)elapsed / fadeDuration;
      int currentValue = startValue + (targetValue - startValue) * progress;
      dmx_data[1] = currentValue;
    }
  }

  // ===== gửi DMX =====
  dmx_write(DMX_NUM_1, dmx_data, DMX_PACKET_SIZE);
  dmx_send(DMX_NUM_1);

  delay(25);
}


