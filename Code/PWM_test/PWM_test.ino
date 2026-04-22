#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>

// ==============================================================================
// 🔧 КОНФИГУРАЦИЯ
// ==============================================================================
const uint8_t PCA_ADDRESSES[] = {0x40, 0x41};
#define NUM_DRIVERS (sizeof(PCA_ADDRESSES) / sizeof(PCA_ADDRESSES[0]))
#define TOTAL_CHANNELS (NUM_DRIVERS * 16)

#define SDA_PIN 8
#define SCL_PIN 9
#define OE_PIN  10
#define DEFAULT_FREQ 1000

// Используем указатели для корректной передачи адреса и Wire в конструктор
Adafruit_PWMServoDriver* drivers[NUM_DRIVERS];
uint16_t channelState[TOTAL_CHANNELS];

// ==============================================================================
// 🛠️ ИНИЦИАЛИЗАЦИЯ
// ==============================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  pinMode(OE_PIN, OUTPUT);
  digitalWrite(OE_PIN, LOW); // OE активный низкий: разрешаем выходы PCA9685

  // Настройка I2C под RP2040 (выполняется ДО инициализации драйверов)
  Wire.setSDA(SDA_PIN);
  Wire.setSCL(SCL_PIN);
  Wire.begin();

  // Инициализация драйверов с передачей адреса и Wire в конструктор
  for (int i = 0; i < NUM_DRIVERS; i++) {
    drivers[i] = new Adafruit_PWMServoDriver(PCA_ADDRESSES[i], Wire);
    drivers[i]->begin();            // В v2.x begin() вызывается БЕЗ аргументов
    drivers[i]->setPWMFreq(DEFAULT_FREQ);
    Serial.printf("✅ PCA@0x%02X инициализирован\n", PCA_ADDRESSES[i]);
  }

  memset(channelState, 0, sizeof(channelState));
  printHelp();
}

// ==============================================================================
// 🔁 ОСНОВНОЙ ЦИКЛ
// ==============================================================================
void loop() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() == 0) return;
    input.toUpperCase();

    char cmd[10] = {0};
    int arg1 = -1, arg2 = -1;
    sscanf(input.c_str(), "%s %d %d", cmd, &arg1, &arg2);

    if (strcmp(cmd, "SET") == 0 && arg1 >= 0 && arg2 >= 0) {
      setChannel(arg1, arg2);
    } else if (strcmp(cmd, "ALL") == 0 && arg1 >= 0) {
      setAllChannels(arg1);
    } else if (strcmp(cmd, "FREQ") == 0 && arg1 > 0) {
      setFrequency(arg1);
    } else if (strcmp(cmd, "STATUS") == 0) {
      printStatus();
    } else if (strcmp(cmd, "CLEAR") == 0) {
      setAllChannels(0);
    } else if (strcmp(cmd, "HELP") == 0) {
      printHelp();
    } else {
      Serial.println("⚠️ Неизвестная команда. Введите HELP");
    }
  }
}

// ==============================================================================
// 📦 ФУНКЦИИ УПРАВЛЕНИЯ
// ==============================================================================
void setChannel(int ch, int val) {
  if (ch < 0 || ch >= TOTAL_CHANNELS) {
    Serial.printf("❌ Ошибка: канал должен быть 0-%d\n", TOTAL_CHANNELS - 1);
    return;
  }
  val = constrain(val, 0, 4095);
  
  int drv = ch / 16;
  int local = ch % 16;
  drivers[drv]->setPWM(local, 0, val);
  channelState[ch] = val;
  Serial.printf("🟢 Канал %2d -> %4d (12-bit PWM)\n", ch, val);
}

void setAllChannels(int val) {
  val = constrain(val, 0, 4095);
  Serial.printf("🟡 Установка всех %d каналов на %d...\n", TOTAL_CHANNELS, val);
  for (int i = 0; i < TOTAL_CHANNELS; i++) {
    setChannel(i, val);
  }
}

void setFrequency(int hz) {
  for (int i = 0; i < NUM_DRIVERS; i++) {
    drivers[i]->setPWMFreq(hz);
  }
  Serial.printf("⚙️ Частота ШИМ изменена на %d Гц\n", hz);
}

void printStatus() {
  Serial.println("\n📊 ТЕКУЩЕЕ СОСТОЯНИЕ КАНАЛОВ:");
  for (int i = 0; i < NUM_DRIVERS; i++) {
    Serial.printf("  PCA@0x%02X: [", PCA_ADDRESSES[i]);
    for (int j = 0; j < 16; j++) {
      Serial.printf("%4d", channelState[i * 16 + j]);
      if (j < 15) Serial.print(", ");
    }
    Serial.println("]");
  }
  Serial.println();
}

void printHelp() {
  Serial.println("\n📖 ДОСТУПНЫЕ КОМАНДЫ (регистр не важен):");
  Serial.printf("  SET <ch> <val>  - Установить канал (0-%d) значение 0-4095\n", TOTAL_CHANNELS - 1);
  Serial.println("  ALL <val>       - Все каналы сразу");
  Serial.println("  FREQ <hz>       - Частота ШИМ (по умолч. 1000 Гц)");
  Serial.println("  STATUS          - Вывести текущие значения");
  Serial.println("  CLEAR           - Выключить все каналы");
  Serial.println("  HELP            - Показать эту справку\n");
}