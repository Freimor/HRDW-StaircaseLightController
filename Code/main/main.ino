#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <cstring>
#include <algorithm>

// ==============================================================================
// 🔧 КОНФИГУРАЦИЯ
// ==============================================================================
#define LED_NUM         32    // Общее количество ступеней
#define GROUP_SIZE      3     // 🔥 Размер волны (активных каналов одновременно)
#define PAUSE_RATIO     0.5f  // 🆕 Коэффициент паузы: pause_time = led_time * PAUSE_RATIO
                              // 0.5f = пауза = 50% времени fade (плавная волна)
                              // 1.0f = пауза = времени fade (чёткие шаги)
                              // 0.0f = без паузы (непрерывный поток)

#define ENV_BRIGHTNESS  1 // Порог освещённости (ADC 0-65535)
#define LED_TEST        0     // 1 = тест анимации, 0 = рабочий режим
#define SENSOR_TEST     0     // 1 = диагностика датчиков, 0 = рабочий режим

// ==============================================================================
// 🔌 АППАРАТНАЯ КОНФИГУРАЦИЯ
// ==============================================================================
const uint8_t PCA_ADDRS[] = {0x40, 0x41};
const int NUM_DRIVERS = sizeof(PCA_ADDRS) / sizeof(PCA_ADDRS[0]);

const uint8_t PIR1_PIN       = 11;
const uint8_t PIR2_PIN       = 12;
const uint8_t LDR_PIN        = 26;
const uint8_t POT_TIME_PIN   = 27;
const uint8_t POT_BRIGHT_PIN = 28;
const uint8_t OE_PIN         = 10;
const uint8_t I2C_SDA        = 8;
const uint8_t I2C_SCL        = 9;

// ==============================================================================
// 🏗️ КЛАСС КОНТРОЛЛЕРА
// ==============================================================================
class StairLightController {
private:
    Adafruit_PWMServoDriver* drivers[NUM_DRIVERS];
    
    // Состояние волны
    unsigned long start_times[LED_NUM];
    int head_ch = 0;              // Физический канал-"голова" волны
    int next_sched_ch = 0;        // Следующий канал для запуска
    int direction = 1;            // 1 = вверх, -1 = вниз
    bool is_animating = false;

    // Тесты и диагностика
    unsigned long test_last_ms = 0;
    int test_pir_sel = 1;
    unsigned long sensor_print_ms = 0;

    // Параметры (обновляются в цикле)
    uint16_t led_brightness = 0;
    uint32_t led_time = 0;

    // Гамма-коррекция 2.2
    uint16_t applyGamma(uint16_t val) const {
        if (val <= 0) return 0;
        if (val >= 4095) return 4095;
        float f = (float)val / 4095.0f;
        return (uint16_t)(powf(f, 2.2f) * 4095.0f);
    }

    void setLedPWM(int ch, uint16_t brightness) {
        if (ch < 0 || ch >= LED_NUM) return;
        int drv = ch / 16;
        int local = ch % 16;
        if (drv < NUM_DRIVERS) {
            drivers[drv]->setPWM(local, 0, applyGamma(brightness));
        }
    }

    void turnOffAll() {
        for (int i = 0; i < NUM_DRIVERS; i++) {
            for (int ch = 0; ch < 16; ch++) {
                drivers[i]->setPWM(ch, 0, 0);
            }
        }
    }

    // Запуск волны принимает рассчитанный интервал
    void startWaveAnimation(int dir, uint32_t step_interval) {
        direction = dir;
        memset(start_times, 0, sizeof(start_times));
        
        if (dir == 1) { // Снизу вверх
            head_ch = 0;
            next_sched_ch = 1;
            start_times[0] = millis();
        } else {        // Сверху вниз
            head_ch = LED_NUM - 1;
            next_sched_ch = LED_NUM - 2;
            start_times[LED_NUM - 1] = millis();
        }
        
        next_trigger_ms = millis() + step_interval;
        is_animating = true;
        Serial.println("🌊 Волна запущена");
    }

public:
    unsigned long next_trigger_ms = 0;

    void begin() {
        Serial.begin(115200);
        while (!Serial) delay(10);

        pinMode(OE_PIN, OUTPUT);
        digitalWrite(OE_PIN, LOW);

        Wire.setSDA(I2C_SDA);
        Wire.setSCL(I2C_SCL);
        Wire.begin();

        for (int i = 0; i < NUM_DRIVERS; i++) {
            drivers[i] = new Adafruit_PWMServoDriver(PCA_ADDRS[i], Wire);
            drivers[i]->begin();
            drivers[i]->setPWMFreq(1000);
            Serial.printf("✅ PCA@0x%02X инициализирован\n", PCA_ADDRS[i]);
        }

        analogReadResolution(16);
        turnOffAll();

        #if SENSOR_TEST == 1
            Serial.println("🔍 Режим ДИАГНОСТИКИ датчиков");
        #elif LED_TEST == 1
            Serial.println("🟢 Режим ТЕСТА анимации");
        #else
            Serial.println("🌍 РАБОЧИЙ режим");
        #endif
    }

    void update() {
        // 📊 РЕЖИМ ДИАГНОСТИКИ
        #if SENSOR_TEST == 1
            if (millis() - sensor_print_ms >= 500) {
                sensor_print_ms = millis();
                Serial.printf("📊 DIAG | PIR1: %d | PIR2: %d | LDR: %u\n", 
                              digitalRead(PIR1_PIN), digitalRead(PIR2_PIN), analogRead(LDR_PIN));
            }
            return;
        #endif

        // 🌍 СЧИТЫВАНИЕ ДАТЧИКОВ И ПОТЕНЦИОМЕТРОВ
        uint16_t ldr_val = analogRead(LDR_PIN);
        uint16_t pot_b_val = analogRead(POT_BRIGHT_PIN);
        uint16_t pot_t_val = analogRead(POT_TIME_PIN);

        // Динамические параметры
        led_brightness = (uint16_t)((pot_b_val / 65535.0f) * 4095.0f);
        led_time = 500 + (uint32_t)((pot_t_val / 65535.0f) * 4500.0f);
        
        // 🆕 Пропорциональная пауза
        uint32_t pause_time = (uint32_t)(led_time * PAUSE_RATIO);
        uint32_t step_interval = led_time + (pause_time / 4);
        uint32_t full_cycle = 2UL * led_time + pause_time;

        // Сигналы PIR
        uint8_t pir1_sig = 0, pir2_sig = 0;
        #if LED_TEST == 1
            unsigned long now = millis();
            if (!is_animating && (now - test_last_ms >= 1000)) {
                test_last_ms = now;
                test_pir_sel = (test_pir_sel == 1) ? 2 : 1;
                pir1_sig = (test_pir_sel == 1);
                pir2_sig = (test_pir_sel == 2);
                Serial.printf("🧪 ТЕСТ: PIR %d\n", test_pir_sel);
            }
        #else
            pir1_sig = digitalRead(PIR1_PIN);
            pir2_sig = digitalRead(PIR2_PIN);
        #endif

        // 🔒 БЛОКИРОВКА ВХОДОВ ВО ВРЕМЯ АНИМАЦИИ
        if (!is_animating) {
            if (ldr_val >= ENV_BRIGHTNESS) {
                if (pir1_sig) startWaveAnimation(1, step_interval);
                else if (pir2_sig) startWaveAnimation(-1, step_interval);
            }
        }

        // 🎬 ЛОГИКА ВОЛНЫ
        if (is_animating) {
            unsigned long now = millis();

            // 1. Запуск следующего канала в волне
            bool in_range = false;
            if (direction == 1) in_range = (next_sched_ch <= std::min(LED_NUM - 1, head_ch + GROUP_SIZE - 1));
            else                in_range = (next_sched_ch >= std::max(0, head_ch - GROUP_SIZE + 1));

            if (in_range && now >= next_trigger_ms) {
                start_times[next_sched_ch] = now;
                next_trigger_ms = now + step_interval; // Используем текущий шаг
                Serial.printf("➡️ Канал %d начинает розжиг\n", next_sched_ch);
                next_sched_ch += direction;
            }

            // 2. Обновление ШИМ для всех каналов
            for (int i = 0; i < LED_NUM; i++) {
                if (start_times[i] == 0) continue;
                unsigned long elapsed = now - start_times[i];
                uint16_t val = 0;

                if (elapsed < led_time) {
                    val = (uint16_t)((float)elapsed / led_time * led_brightness);
                } else if (elapsed < led_time + pause_time) {
                    val = led_brightness;
                } else if (elapsed < full_cycle) {
                    val = (uint16_t)((1.0f - (float)(elapsed - (led_time + pause_time)) / led_time) * led_brightness);
                } else {
                    val = 0;
                }
                setLedPWM(i, val);
            }

            // 3. Сдвиг "головы" волны, когда текущий канал завершил цикл
            if (start_times[head_ch] > 0) {
                unsigned long head_elapsed = now - start_times[head_ch];
                if (head_elapsed >= full_cycle) {
                    start_times[head_ch] = 0; // Очистка памяти
                    head_ch += direction;

                    bool finished = (direction == 1 && head_ch >= LED_NUM) || 
                                    (direction == -1 && head_ch < 0);
                    if (finished) {
                        is_animating = false;
                        turnOffAll();
                        Serial.println("✅ Волна завершена. Готов к новому срабатыванию.");
                    }
                }
            }
        }
    }
};

// ==============================================================================
// 🚀 ЗАПУСК
// ==============================================================================
StairLightController controller;

void setup() {
    controller.begin();
}

void loop() {
    controller.update();
    delay(5); // Стабилизация I2C
}