#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <cmath>
#include <cstring>

// ==============================================================================
// 🔧 КОНФИГУРАЦИЯ
// ==============================================================================
#define ACTIVE_MODE         1     // 0 = NIGHT_FADE, 1 = ASYMMETRIC
#define WAITING_ANIMATION   1     // 0 = ВЫКЛ, 1 = ВКЛ (BREATHING)
#define DIAG_MODE           0     // 🔍 1 = ТОЛЬКО диагностика (Serial), 0 = рабочий режим
#define LDR_BLOCKING        0     // 0 = PIR срабатывает ВСЕГДА, 1 = PIR только в темноте
#define LDR_DARK_IS_LOW     1     // 1 = темнота = низкий ADC, 0 = темнота = высокий ADC
#define ENV_BRIGHTNESS      30000 // Порог срабатывания LDR
#define LED_NUM             32    // Количество ступеней

#define NIGHT_FADE_MS       1500UL
#define BREATH_MIN_RATIO    0.03f
#define BREATH_MAX_RATIO    0.20f
#define BREATH_PERIOD_MS    4000UL
#define BREATH_RECOVER_MS   300UL
#define PIR_DEBOUNCE_MS     500UL

// ==============================================================================
// 🔌 АППАРАТНАЯ КОНФИГУРАЦИЯ
// ==============================================================================
const uint8_t PCA_ADDRS[] = {0x40, 0x41};
const int NUM_DRIVERS = sizeof(PCA_ADDRS) / sizeof(PCA_ADDRS[0]);
const uint8_t PIR1_PIN = 11, PIR2_PIN = 12, LDR_PIN = 26, POT_TIME_PIN = 27, POT_BRIGHT_PIN = 28;
const uint8_t OE_PIN = 10, I2C_SDA = 8, I2C_SCL = 9;

// ==============================================================================
// 🏗️ КЛАСС КОНТРОЛЛЕРА
// ==============================================================================
class StairLightController {
private:
    Adafruit_PWMServoDriver* drivers[NUM_DRIVERS];
    
    enum State { IDLE, PRE_ACTIVE, ACTIVE, POST_ACTIVE, TO_IDLE };
    State state = IDLE;
    unsigned long state_start = 0;
    
    unsigned long start_times[LED_NUM] = {0};
    int anim_dir = 1, next_ch = 0;
    unsigned long last_schedule_ms = 0;
    int pending_dir = 0;
    unsigned long last_pir_ms = 0;
    
    uint16_t led_brightness = 0, led_time = 0;
    unsigned long breath_start = 0;
    unsigned long trans_start = 0, trans_duration = 0;
    uint16_t trans_from = 0, trans_to = 0;

    // 🔍 Программное усреднение LDR (убирает случайные скачки АЦП)
    uint16_t readLDR_Average() const {
        uint32_t sum = 0;
        for (uint8_t i = 0; i < 8; i++) {
            sum += analogRead(LDR_PIN);
            delayMicroseconds(300); // Небольшая пауза для стабилизации входа АЦП
        }
        return sum >> 3; // Деление на 8
    }

    uint16_t applyGamma(uint16_t val) const {
        if (val <= 0) return 0;
        if (val >= 4095) return 4095;
        return (uint16_t)(powf((float)val / 4095.0f, 2.2f) * 4095.0f);
    }

    void setLedPWM(int ch, uint16_t brightness) {
        if (ch < 0 || ch >= LED_NUM) return;
        drivers[ch / 16]->setPWM(ch % 16, 0, applyGamma(brightness));
    }

    void turnOffAll() {
        for (int i = 0; i < NUM_DRIVERS; i++)
            for (int ch = 0; ch < 16; ch++) drivers[i]->setPWM(ch, 0, 0);
    }

    void setState(State newState) {
        state = newState;
        state_start = millis();
    }

    void startTransition(uint16_t from, uint16_t to, unsigned long dur) {
        trans_start = millis();
        trans_duration = (dur < 50) ? 50 : dur;
        trans_from = from; trans_to = to;
    }

    // 🔥 Гарантированный старт с 0% яркости
    void startWave(int dir) {
        turnOffAll(); // Сбрасываем все каналы в 0 перед запуском волны
        anim_dir = dir;
        memset(start_times, 0, sizeof(start_times));
        int first = (dir == 1) ? 0 : LED_NUM - 1;
        next_ch = first + dir;
        start_times[first] = millis();
        last_schedule_ms = millis();
    }

    // 🔹 РАСЧЁТ ТАЙМИНГОВ
    void calcTimings(uint32_t &up, uint32_t &pause, uint32_t &down) {
        if (ACTIVE_MODE == 1) { // ASYMMETRIC
            const float SLOW_FACTOR = 2.4f; // 🐌 Замедление на 140% относительно базы
            up    = (uint32_t)(led_time * 0.2f * SLOW_FACTOR);
            pause = 0;
            down  = (uint32_t)(led_time * 0.8f * SLOW_FACTOR);
        } else { // NIGHT_FADE
            up    = (uint32_t)(led_time * 0.35f);
            pause = (uint32_t)(led_time * 0.3f);
            down  = (uint32_t)(led_time * 0.35f);
        }
    }

    uint16_t calcPhase(unsigned long elapsed, uint32_t up, uint32_t pause, uint32_t down) {
        if (elapsed < up) return (uint16_t)((float)elapsed / up * led_brightness);
        if (elapsed < up + pause) return led_brightness;
        if (elapsed < up + pause + down) return (uint16_t)((1.0f - (float)(elapsed - up - pause) / down) * led_brightness);
        return 0;
    }

    uint16_t getCurrentBreath() const {
        float phase = (float)(millis() - breath_start) / BREATH_PERIOD_MS;
        float wave = 0.5f * (1.0f - cosf(2.0f * 3.14159f * phase));
        return (uint16_t)(led_brightness * (BREATH_MIN_RATIO + (BREATH_MAX_RATIO - BREATH_MIN_RATIO) * wave));
    }

public:
    void begin() {
        Serial.begin(115200);
        for (int i = 0; i < 150; i++) { if (Serial) break; delay(10); }

        pinMode(PIR1_PIN, INPUT_PULLDOWN);
        pinMode(PIR2_PIN, INPUT_PULLDOWN);
        pinMode(OE_PIN, OUTPUT);
        digitalWrite(OE_PIN, LOW);

        Wire.setSDA(I2C_SDA); Wire.setSCL(I2C_SCL); Wire.begin();
        analogReadResolution(16);

        for (int i = 0; i < NUM_DRIVERS; i++) {
            drivers[i] = new Adafruit_PWMServoDriver(PCA_ADDRS[i], Wire);
            drivers[i]->begin(); drivers[i]->setPWMFreq(1000);
        }

        turnOffAll();
        breath_start = millis();
        
        #if DIAG_MODE == 1
            Serial.println("=== DIAG MODE ACTIVE ===");
            Serial.println("LDR: <фото> | POT_B: <яркость> | POT_T: <время> | PIR1: <0/1> | PIR2: <0/1>");
        #endif
    }

    void update() {
        // 🔍 РЕЖИМ ДИАГНОСТИКИ
        #if DIAG_MODE == 1
            static unsigned long last_diag = 0;
            if (millis() - last_diag >= 500) {
                last_diag = millis();
                uint16_t ldr = readLDR_Average(); // ✅ Усреднённое чтение
                uint16_t pot_b = analogRead(POT_BRIGHT_PIN);
                uint16_t pot_t = analogRead(POT_TIME_PIN);
                uint8_t p1 = digitalRead(PIR1_PIN);
                uint8_t p2 = digitalRead(PIR2_PIN);
                Serial.printf("LDR: %5u | POT_B: %5u | POT_T: %5u | PIR1: %d | PIR2: %d\n", ldr, pot_b, pot_t, p1, p2);
            }
            return;
        #endif

        uint16_t ldr = readLDR_Average(); // ✅ Усреднённое чтение
        uint16_t pot_b = analogRead(POT_BRIGHT_PIN);
        uint16_t pot_t = analogRead(POT_TIME_PIN);
        
        led_brightness = (uint16_t)((pot_b / 65535.0f) * 4095.0f);
        if (led_brightness < 200) led_brightness = 200;
        led_time = 500 + (uint32_t)((pot_t / 65535.0f) * 4500.0f);

        unsigned long now = millis();
        unsigned long elapsed = now - state_start;

        if (now - last_pir_ms > PIR_DEBOUNCE_MS) {
            bool p1 = digitalRead(PIR1_PIN);
            bool p2 = digitalRead(PIR2_PIN);
            if (p1 || p2) {
                last_pir_ms = now;
                bool is_dark = true;
                if (LDR_BLOCKING) {
                    is_dark = LDR_DARK_IS_LOW ? (ldr < ENV_BRIGHTNESS) : (ldr >= ENV_BRIGHTNESS);
                }
                if (is_dark && state == IDLE && pending_dir == 0) {
                    pending_dir = p1 ? 1 : -1;
                }
            }
        }

        switch (state) {
            case IDLE:
                if (WAITING_ANIMATION) {
                    uint16_t v = getCurrentBreath();
                    for(int i=0; i<LED_NUM; i++) setLedPWM(i, v);
                }
                if (pending_dir != 0) {
                    if (WAITING_ANIMATION) {
                        // 🔥 Целевая яркость перехода теперь 0%, а не BREATH_MIN_RATIO
                        startTransition(getCurrentBreath(), 0, (led_time > 600 ? led_time/3 : 200));
                        setState(PRE_ACTIVE);
                    } else {
                        startWave(pending_dir);
                        pending_dir = 0;
                        setState(ACTIVE);
                    }
                }
                break;

            case PRE_ACTIVE:
                if (elapsed >= trans_duration) {
                    startWave(pending_dir);
                    pending_dir = 0;
                    setState(ACTIVE);
                } else {
                    float p = (float)elapsed / trans_duration;
                    uint16_t v = trans_from - (uint16_t)((trans_from - trans_to) * p);
                    for(int i=0; i<LED_NUM; i++) setLedPWM(i, v);
                }
                break;

            case ACTIVE: {
                uint32_t up, pause, down;
                calcTimings(up, pause, down);
                uint32_t step = up + (pause / 2);

                bool in_bounds = (anim_dir == 1) ? (next_ch < LED_NUM) : (next_ch >= 0);
                if (in_bounds && now - last_schedule_ms >= step) {
                    start_times[next_ch] = now;
                    next_ch += anim_dir;
                    last_schedule_ms = now;
                }

                for (int i = 0; i < LED_NUM; i++) {
                    if (start_times[i] == 0) continue;
                    setLedPWM(i, calcPhase(now - start_times[i], up, pause, down));
                }

                int last = next_ch - anim_dir;
                if (last >= 0 && last < LED_NUM && start_times[last] > 0) {
                    if (now - start_times[last] >= up + pause + down) {
                        turnOffAll();
                        setState(POST_ACTIVE);
                    }
                }
                break;
            }

            case POST_ACTIVE:
                if (elapsed >= NIGHT_FADE_MS) {
                    if (WAITING_ANIMATION) {
                        startTransition(0, led_brightness * BREATH_MIN_RATIO, BREATH_RECOVER_MS);
                        setState(TO_IDLE);
                    } else {
                        setState(IDLE);
                    }
                }
                break;

            case TO_IDLE:
                if (elapsed >= trans_duration) {
                    breath_start = millis();
                    setState(IDLE);
                } else {
                    float p = (float)elapsed / trans_duration;
                    uint16_t v = (uint16_t)(trans_to * p);
                    for(int i=0; i<LED_NUM; i++) setLedPWM(i, v);
                }
                break;
        }

        delay(5);
    }
};

StairLightController ctrl;
void setup() { ctrl.begin(); }
void loop()  { ctrl.update(); }