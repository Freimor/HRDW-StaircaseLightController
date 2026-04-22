#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

// ==============================================================================
// 🔧 НАСТРОЙКИ АНИМАЦИИ
// ==============================================================================
#define LED_NUM_TOTAL     32    // Общее количество ступеней на лестнице
#define GROUP_SIZE        3     // 🔥 Размер волны (количество одновременно активных каналов)
#define MAX_BRIGHTNESS    4095  // Максимальное значение ШИМ (0-4095)
#define LED_TIME          700  // Длительность плавного перехода (мс)
#define PAUSE_TIME        700   // Пауза на максимальной яркости (мс)

// ==============================================================================
// 🔌 АППАРАТНАЯ КОНФИГУРАЦИЯ
// ==============================================================================
const uint8_t PCA_ADDRS[] = {0x40, 0x41};
const int NUM_DRIVERS = sizeof(PCA_ADDRS) / sizeof(PCA_ADDRS[0]);
const uint8_t OE_PIN  = 10;
const uint8_t I2C_SDA = 8;
const uint8_t I2C_SCL = 9;

Adafruit_PWMServoDriver* drivers[NUM_DRIVERS];

// ==============================================================================
// 📊 СОСТОЯНИЕ АНИМАЦИИ
// ==============================================================================
unsigned long start_times[LED_NUM_TOTAL]; // Время старта каждого канала
int next_to_schedule = 0;                 // Следующий канал для запуска
int head_channel = 0;                     // Первый канал в текущей волне
unsigned long next_trigger_ms = 0;        // Время следующего запуска

// Расчётные константы
const unsigned long STEP_INTERVAL = LED_TIME + (PAUSE_TIME / 4);
const unsigned long FULL_CYCLE_TIME = 2UL * LED_TIME + PAUSE_TIME;

// ==============================================================================
// 🧮 ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ==============================================================================
uint16_t applyGamma(uint16_t val) {
    if (val <= 0) return 0;
    if (val >= 4095) return 4095;
    float f = (float)val / 4095.0f;
    return (uint16_t)(powf(f, 2.2f) * 4095.0f);
}

void setLedPWM(int ch, uint16_t brightness) {
    if (ch < 0 || ch >= LED_NUM_TOTAL) return;
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

// ==============================================================================
// 🛠️ ИНИЦИАЛИЗАЦИЯ
// ==============================================================================
void setup() {
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

    turnOffAll();
    memset(start_times, 0, sizeof(start_times));
    
    // Запуск волны
    start_times[0] = millis();
    next_trigger_ms = millis() + STEP_INTERVAL;
    next_to_schedule = 1;
    
    Serial.printf("🌊 Волна из %d каналов. fade=%dмс, pause=%dмс\n", 
                  GROUP_SIZE, LED_TIME, PAUSE_TIME);
}

// ==============================================================================
// 🔁 ОСНОВНОЙ ЦИКЛ (неблокирующий)
// ==============================================================================
void loop() {
    unsigned long now = millis();

    // 1️⃣ Запуск следующего канала, если пришёл срок и не вышли за пределы волны
    if (next_to_schedule < LED_NUM_TOTAL && 
        next_to_schedule < head_channel + GROUP_SIZE && 
        now >= next_trigger_ms) {
        
        start_times[next_to_schedule] = now;
        next_trigger_ms = now + STEP_INTERVAL;
        Serial.printf("➡️ Канал %d начинает розжиг\n", next_to_schedule);
        next_to_schedule++;
    }

    // 2️⃣ Обновление ШИМ для всех каналов независимо
    for (int i = 0; i < LED_NUM_TOTAL; i++) {
        if (start_times[i] == 0) continue;

        unsigned long elapsed = now - start_times[i];
        uint16_t val = 0;

        if (elapsed < LED_TIME) {
            // 🔼 Розжиг
            float p = (float)elapsed / LED_TIME;
            val = (uint16_t)(p * MAX_BRIGHTNESS);
        } else if (elapsed < LED_TIME + PAUSE_TIME) {
            // ⏸️ Пауза на максимуме
            val = MAX_BRIGHTNESS;
        } else if (elapsed < FULL_CYCLE_TIME) {
            // 🔽 Затухание
            float p = (float)(elapsed - (LED_TIME + PAUSE_TIME)) / LED_TIME;
            val = (uint16_t)((1.0f - p) * MAX_BRIGHTNESS);
        } else {
            // ✅ Завершено
            val = 0;
        }
        setLedPWM(i, val);
    }

    // 3️⃣ Сдвиг окна волны, когда головной канал завершил полный цикл
    if (start_times[head_channel] > 0) {
        unsigned long head_elapsed = now - start_times[head_channel];
        if (head_elapsed >= FULL_CYCLE_TIME) {
            start_times[head_channel] = 0; // Очищаем память о завершённом канале
            head_channel++;
            
            if (head_channel >= LED_NUM_TOTAL) {
                // Цикл по всей лестнице завершён
                head_channel = 0;
                next_to_schedule = 0;
                memset(start_times, 0, sizeof(start_times));
                start_times[0] = millis();
                next_trigger_ms = millis() + STEP_INTERVAL;
                Serial.println("🔄 Полная лестница пройдена. Перезапуск волны...\n");
            } else {
                Serial.printf("📏 Окно сдвинуто. Активные: %d-%d\n", 
                              head_channel, min(head_channel + GROUP_SIZE - 1, LED_NUM_TOTAL - 1));
            }
        }
    }

    delay(5); // Стабилизация I2C без влияния на плавность
}