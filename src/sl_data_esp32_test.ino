/*
 * sl_data_esp32_test.ino
 *
 * Единая прошивка ESP32 для управления сигнализацией StarLine по шине SL-Data.
 * Совмещает (без разделения на файлы/окружения):
 *   1) приём 32-битных слов состояния от сигнализации по шине SL-Data
 *      (библиотека SL_Data_ESP32);
 *   2) управление по Serial-терминалу (режим DEBUG, как в оригинале);
 *   3) BLE-мост: Android-приложение шлёт 1 байт команды в GATT-характеристику
 *      CMD, а ESP32 транслирует его в шину SL-Data и шлёт 32-битное слово
 *      состояния обратно по уведомлениям (NOTIFY) характеристики STATUS.
 *
 * Сборка:  pio run            (обычный терминал, env esp32dev)
 * Прошивка: pio run -t upload
 * Монитор:  pio device monitor
 *
 * !!! ESP32 = 3.3В логика. Если SL-Data сигналки работает по 5В TTL, поставьте
 *     делитель на RX (вход ESP32) и убедитесь, что 3.3В с TX читается как "1".
 *
 * --- Адаптация под ESP32-C3 Super Mini ------------------------------------
 * 1) Пины перенесены на GPIO 3-7 и 10 (см. defines ниже) - на C3 просто нет
 *    пинов 25/26/27/32/33/34 из оригинала под классический ESP32.
 * 2) Serial работает через нативный USB (Super Mini не имеет USB-UART
 *    моста) - это включается флагами ARDUINO_USB_MODE / ARDUINO_USB_CDC_ON_BOOT
 *    в platformio.ini, в коде ничего менять не нужно.
 * 3) ESP32-C3 - одноядерный (RISC-V), в отличие от двухъядерного классического
 *    ESP32. Приём слова SL-Data в SL_Data::recv() выполняется прямо в
 *    обработчике прерывания и блокирует его на ~half-bit + 32 бита (при
 *    5000 бод это ~6.6 мс). На двухъядерном ESP32 это почти не задевало BLE-
 *    стек (он крутился на другом ядре), на C3 - оба делят одно ядро, поэтому
 *    при частых пакетах от сигнализации возможны небольшие задержки/джиттер
 *    BLE-уведомлений. Для этого проекта (низкая частота слов состояния)
 *    это не критично, но если заметите просадки BLE - это первая причина,
 *    которую стоит проверить.
 * 4) partitions.csv пересчитан под 4 МБ флеша (чаще всего именно столько на
 *    Super Mini) вместо 8 МБ в оригинале; раздел LittleFS убран, так как
 *    в прошивке не используется.
 */
#include "SL_Data_ESP32.h"

#include "sdkconfig.h"        // CONFIG_BLUEDROID_ENABLED / CONFIG_NIMBLE_ENABLED
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "ble_ota.h"
#include <Preferences.h>      // хранение PIN в NVS (переживает перепрошивку sketch, не erase flash)

// --- пины под ESP32-C3 Super Mini и доп. каналы ----------------------------
// ВНИМАНИЕ: у C3 всего 11 "обычных" GPIO (0-10) + 18-21, из них:
//   GPIO0, GPIO2, GPIO8, GPIO9  - strapping-пины (влияют на режим загрузки,
//                                 GPIO9 на Super Mini ещё и висит на кнопке
//                                 BOOT, GPIO8 часто на встроенном синем LED)
//                                 - не используем их под функциональные сигналы;
//   GPIO18/GPIO19               - заняты нативным USB (Serial/прошивка),
//                                 не выведены как обычные GPIO на Super Mini;
//   GPIO20/GPIO21               - аппаратный UART0, оставлены свободными про запас.
// Поэтому под шину SL-Data и доп. каналы заняты GPIO 3-7 и 10.
#define SLDATA_RX 4   // данные от сигнализации -> ESP32 (правый пин разъёма SL-Data)
#define SLDATA_TX 5   // данные от ESP32 -> сигнализация (левый пин разъёма SL-Data)
                      // средний пин разъёма SL-Data - GND

#define BIT_DURATION  200   // время одного бита в мкс (отправка команды на сигналку)
#define SL_BAUDRATE   5000  // скорость приёма от сигнализации

//#define STOP_BIT   // раскомментировать, если нужен стоп-бит при отправке команды
#define DEBUG        // раскомментировать для отладки по Serial, снять в финальной прошивке

// --- BLE идентификаторы (синхронизированы с Android-приложением) -----------
#define SL_SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define SL_CMD_CHAR_UUID       "beb5483e-36e1-4688-b7f5-ea07361b26a8"  // WRITE
#define SL_STATUS_CHAR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a9"  // NOTIFY
#define SL_BLE_DEVICE_NAME     "StarLineBLE"

// PIN-код по умолчанию для сопряжения (Passkey Entry), используется только
// при первом запуске, если в NVS ещё ничего не сохранено. Ровно 6 цифр
// (000000-999999). Реальный рабочий PIN хранится в NVS (Preferences) и
// может быть изменён без перепрошивки — см. функции loadBlePin/saveBlePin
// и команду 'P' в debug_readcommand().
#define SL_BLE_PIN_DEFAULT  123456
#define BLE_PIN_NVS_NAMESPACE "sl_ble"
#define BLE_PIN_NVS_KEY       "pin"


SL_Data sl_data(SLDATA_RX, SLDATA_TX, 1); // (RX, TX, 1==invert_logic)

// --- Хранение PIN-кода BLE в NVS (энергонезависимая память) ---------------
Preferences blePrefs;
uint32_t currentBlePin = SL_BLE_PIN_DEFAULT; // актуальное значение, заполняется в setup() из NVS

// команды управления сигнализацией
const uint8_t ZAPROS     = 0x42;
const uint8_t START_ENG  = 0x21;
const uint8_t STOP_ENG   = 0x20;
const uint8_t OHRANA_ON  = 0x11;
const uint8_t OHRANA_OFF = 0x10;
const uint8_t VALET_OFF  = 0x50;
const uint8_t VALET_ON   = 0x51;
const uint8_t TRUNK_CMD  = 0x23;
const uint8_t HORN_CMD   = 0x24;
const uint8_t CMD_GET_PIN = 0x60;  // запрос текущего PIN-кода BLE
const uint8_t CMD_SET_PIN = 0x61;  // установка нового PIN-кода BLE (5 байт: 0x61 + PIN uint32 LE)

// переменные состояния сигнализации
bool Doors        = 0;
bool Locks        = 0;
bool Ohrana       = 0;
bool Trunk        = 0;
bool Hood         = 0;
bool Ignition     = 0;
bool Brake        = 0;
bool Engine       = 0;
bool Trevoga      = 0;
bool Tiltsensor   = 0;
bool Shocksensor  = 0;
bool Valet        = 0; // режим Valet: бит 15, подтверждено экспериментально

uint32_t lastStatusWord = 0; // последнее валидное слово для отправки по BLE

// --- пины дополнительных каналов (багажник, сигнал) и датчиков -----------------
#define TRUNK_PIN 6
#define HORN_PIN  7
#define PULSE_DURATION_MS 1000UL
#define DS18B20_PIN 10
// ADC_PIN обязательно должен быть в диапазоне GPIO0-4 - это единственные
// пины, подключённые к ADC1 на ESP32-C3 (ADC2 на C3 есть, но Arduino-core
// его не отдаёт под analogRead из-за конфликта с Wi-Fi/BLE). GPIO0/2 - это
// strapping-пины, поэтому под делитель напряжения взят GPIO3.
#define ADC_PIN 3

// --- делитель напряжения (R1=510k, R2=100k) ---
const float R1 = 510000.0;
const float R2 = 100000.0;
const float maxInputVoltage = 20.0;
const float maxADCVoltage = 3.3;

// --- DS18B20 ---
OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);
static float lastTemperature = -127.0f;
static uint32_t tempReadDue = 0;
static bool tempConversionStarted = false;

// --- напряжение (через делитель) ---
static float lastVoltage = -1.0f;
static uint32_t voltageReadDue = 0;

// переменные для неблокирующего управления GPIO
static uint32_t trunkPulseEnd = 0;
static uint32_t hornPulseEnd  = 0;

// --- машина состояний для багажника (снять с охраны -> подождать -> открыть) ---
enum TrunkState : uint8_t {
  TRUNK_IDLE,
  TRUNK_DISARM_SENT,      // отправили OHRANA_OFF, ждём подтверждения Ohrana=0
  TRUNK_WAIT_BEFORE_PULSE // охрана снята, ждём 1 сек перед импульсом
};
static TrunkState trunkState = TRUNK_IDLE;
static uint32_t trunkTimerEnd = 0;

// --- BLE объекты и флаги ---------------------------------------------------
BLEServer         *bleServer        = nullptr;
BLECharacteristic *bleCmdChar       = nullptr;
BLECharacteristic *bleStatusChar    = nullptr;
bool               deviceConnected  = false;

// --- Защита BLE: Bonding + PIN (Passkey Entry) -----------------------------
// Любой телефон, не прошедший сопряжение (bonding) с вводом правильного
// PIN-кода (хранится в NVS, см. currentBlePin), не сможет записать командную
// характеристику — канал не будет зашифрован. Bonding в BLE всегда шифрует
// канал, а режим Passkey Entry дополнительно требует знания PIN для
// установления пары.

class SecurityCallbacks : public BLESecurityCallbacks
{
  // ESP32 не имеет дисплея, поэтому вместо реального отображения PIN
  // используется заранее заданный статический passkey (currentBlePin,
  // хранится в NVS), который пользователь должен ввести на телефоне при
  // сопряжении.
  uint32_t onPassKeyRequest() override
  {
    return currentBlePin;
  }

  void onPassKeyNotify(uint32_t pass_key) override
  {
    #ifdef DEBUG
    Serial.print(F("BLE: passkey requested, showing: "));
    Serial.println(pass_key);
    #endif
  }

  bool onConfirmPIN(uint32_t pin) override
  {
    return true;
  }

  bool onSecurityRequest() override
  {
    return true;
  }

  // Сигнатура onAuthenticationComplete() у базового класса BLESecurityCallbacks
  // отличается между backend'ами: Bluedroid передаёт esp_ble_auth_cmpl_t
  // (с полями success/fail_reason), NimBLE - указатель ble_gap_conn_desc*
  // (без fail_reason, есть только сам факт коннекта). ESP32-C3 в этой сборке
  // собирается на NimBLE, поэтому оставлен вариант под оба backend'а через
  // #if, чтобы override совпадал с реально скомпилированным базовым классом.
#if defined(CONFIG_BLUEDROID_ENABLED)
  void onAuthenticationComplete(esp_ble_auth_cmpl_t auth_cmpl) override
  {
    if (auth_cmpl.success)
    {
      #ifdef DEBUG
      Serial.println(F("BLE: pairing (bonding) successful"));
      #endif
    }
    else
    {
      #ifdef DEBUG
      Serial.print(F("BLE: pairing failed, code="));
      Serial.println(auth_cmpl.fail_reason);
      #endif
    }
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override
  {
    #ifdef DEBUG
    if (desc != nullptr)
      Serial.println(F("BLE: pairing (bonding) successful"));
    else
      Serial.println(F("BLE: pairing failed"));
    #endif
  }
#endif
};

// Обмен с BLE-callback'ами через флаги, чтобы не делать долгие delay()
// внутри BLE-задачи (onWrite) и не блокировать стек.
volatile uint8_t  bleCmd            = 0;
volatile bool     bleCmdPending     = false;
volatile bool     bleMultiCmd       = false;   // true если команда мультибайтовая (CMD_SET_PIN)
volatile uint8_t  bleCmdData[5]     = {0};     // буфер для мультибайтовой команды (включая cmd byte)
volatile uint8_t  bleCmdDataLen     = 0;       // длина данных в bleCmdData
volatile bool     statusDirty       = false;

// --- предварительные объявления --------------------------------------------
void sendword(uint8_t &inbyte);
void sendcommand(uint8_t comm);
void sendStatusBLE(uint32_t data);
void handlePulseOutputs();
void triggerOutput(uint8_t pin, uint32_t durationMs);
void handleTrunkState();
void trunkRequest();
void handleTemperature();
void handleVoltage();
void sendStatusBLE(uint32_t data, float temp, float voltage);
void sendStatusBLE(uint32_t data, float temp);
uint32_t loadBlePin();
bool saveBlePin(uint32_t newPin);
void handleBlePinCommand();
void sendPinResponse(uint8_t type, uint32_t pin);

// ---------------------------------------------------------------------------
// BLE callbacks
// ---------------------------------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *pServer) override
  {
    deviceConnected = true;
  }

  void onDisconnect(BLEServer *pServer) override
  {
    deviceConnected = false;
    // перезапускаем advertising, чтобы устройство снова было видно
    pServer->getAdvertising()->start();
  }
};

class CmdCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic) override
  {
    // В Bluedroid запись в характеристику с правами
    // ESP_GATT_PERM_WRITE_ENCRYPTED уже отклоняется стеком, если канал
    // не зашифрован (нет bonding). Дополнительная проверка не нужна.
    String value = pCharacteristic->getValue();
    uint8_t len = value.length();
    if (len >= 1)
    {
      bleCmd = (uint8_t)value[0];
      // Для CMD_SET_PIN ожидаем 5 байт: 0x61 + PIN(uint32 LE)
      if (bleCmd == CMD_SET_PIN && len == 5)
      {
        bleMultiCmd = true;
        bleCmdDataLen = len;
        for (uint8_t i = 0; i < len && i < 5; i++)
          bleCmdData[i] = (uint8_t)value[i];
      }
      else
      {
        bleMultiCmd = false;
        bleCmdData[0] = bleCmd;
        bleCmdDataLen = 1;
      }
      bleCmdPending = true;
    }
  }
};

// ---------------------------------------------------------------------------
// Отладочный вывод (DEBUG)
// ---------------------------------------------------------------------------
#ifdef DEBUG
void printdata(const uint32_t &data, const bool &CSgood)
{
  Serial.print(F("Receive:  "));
  Serial.print(F("    0x")); Serial.print(data, HEX); Serial.print(F("     BIN:  "));
  for (int i = 0; i < 32; i++) { Serial.print(bitRead(data, 31 - i)); if (i == 7 || i == 15 || i == 23) Serial.print(" "); }
  Serial.println();
  Serial.print(" Door: "); Serial.print(Doors);   Serial.print("  Ign: "); Serial.print(Ignition);
  Serial.print("  Brake: "); Serial.print(Brake); Serial.print("  Trunk: "); Serial.print(Trunk);
  Serial.print("  Hood: "); Serial.print(Hood);   Serial.print("  Locks: "); Serial.print(Locks);
  Serial.print("  Ohrana: "); Serial.print(Ohrana); Serial.print("  Engine: "); Serial.print(Engine);
  Serial.print("  Trevoga: "); Serial.print(Trevoga);
  Serial.print("  Shock: "); Serial.print(Shocksensor); Serial.print("  Tilt: "); Serial.println(Tiltsensor);
  Serial.print("  Valet: "); Serial.println(Valet);
  if (CSgood) Serial.println("Checksumm is good!!!"); else Serial.println("Checksumm is Error!!!");
  Serial.println();
}
#endif

void GoodResponse_received_from_starline(const uint32_t &data);

void sl_data_read()
{
  // получаем данные (слово 32 bit) от сигнализации
  if (sl_data.available())
  {
    uint32_t data = sl_data.reaD();
    if (data != 0)
    {
      // ВНИМАНИЕ: массив намеренно оставлен размером 3, как в оригинале
      // автора - запись в answer[3] выходит за границы массива, но по
      // требованию сохраняется без изменений.
      byte answer[3] = {0};
      answer[0] = data;
      answer[1] = (data & 0x0000FF00) >> 8;
      answer[2] = (data & 0x00FF0000) >> 16;
      answer[3] = (data & 0xFF000000) >> 24;
      byte CS = 0;
      CS = ~(answer[0] + answer[1] + answer[2] + 2);
      bool CSgood = (CS == answer[3]);
      if (CSgood) { GoodResponse_received_from_starline(data); }
#ifdef DEBUG
      printdata(data, CSgood);
#endif
    }
  }
}

void sendword(uint8_t &inbyte)
{
  digitalWrite(SLDATA_TX, 1);    // старт-бит
  delayMicroseconds(BIT_DURATION);
  for (byte g = 0; g < 2; g++)
  {
    for (int i = 0; i < 8; i++)
    {
      if (!g) digitalWrite(SLDATA_TX, bitRead(inbyte, i));
      else    digitalWrite(SLDATA_TX, !bitRead(inbyte, i)); // прямой + инверсный проход
      delayMicroseconds(BIT_DURATION);
    }
  }
#ifdef STOP_BIT
  digitalWrite(SLDATA_TX, 0); // стоп-бит
  delayMicroseconds(BIT_DURATION);
#endif
}

void sendcommand(uint8_t comm)
{
  for (int g = 0; g < 5; g++)
  {
    digitalWrite(SLDATA_TX, 0);
    delay(10);
    sendword(comm);
  }
  digitalWrite(SLDATA_TX, 1);
}

// Отправка 32-битного слова состояния + температура + напряжение (8 байт, LE)
void sendStatusBLE(uint32_t data)
{
  sendStatusBLE(data, lastTemperature, lastVoltage);
}

void sendStatusBLE(uint32_t data, float temp, float voltage)
{
  if (!deviceConnected || !bleStatusChar) return;

  uint8_t buf[8];
  buf[0] = data & 0xFF;
  buf[1] = (data >> 8) & 0xFF;
  buf[2] = (data >> 16) & 0xFF;
  buf[3] = (data >> 24) & 0xFF;
  // Температура в int16, десятые градуса (град * 10)
  int16_t tempInt = (temp < -55.0f || temp > 125.0f) ? (int16_t)0x8000 : (int16_t)(temp * 10.0f + 0.5f);
  buf[4] = tempInt & 0xFF;
  buf[5] = (tempInt >> 8) & 0xFF;
  // Напряжение в int16, десятые вольта (в * 10)
  int16_t voltInt = (voltage < 0.0f || voltage > 25.0f) ? (int16_t)0x8000 : (int16_t)(voltage * 10.0f + 0.5f);
  buf[6] = voltInt & 0xFF;
  buf[7] = (voltInt >> 8) & 0xFF;

  bleStatusChar->setValue(buf, 8);
  bleStatusChar->notify();
}

void sendStatusBLE(uint32_t data, float temp)
{
  sendStatusBLE(data, temp, lastVoltage);
}

// --- Хранение PIN-кода в NVS (Preferences) ---------------------------------
// Позволяет менять PIN сопряжения без перепрошивки устройства.
// Значение хранится в отдельном NVS-namespace, переживает re-flash sketch
// (стирается только при полном erase flash или explicit-очистке NVS).

// Читает сохранённый PIN из NVS. Если ничего не сохранено (первый запуск)
// или значение некорректно (>999999) — возвращает и сохраняет PIN по умолчанию.
uint32_t loadBlePin()
{
  blePrefs.begin(BLE_PIN_NVS_NAMESPACE, true); // read-only
  bool exists = blePrefs.isKey(BLE_PIN_NVS_KEY);
  uint32_t pin = blePrefs.getUInt(BLE_PIN_NVS_KEY, SL_BLE_PIN_DEFAULT);
  blePrefs.end();

  if (!exists || pin > 999999UL)
  {
    pin = SL_BLE_PIN_DEFAULT;
    saveBlePin(pin); // инициализируем NVS значением по умолчанию
  }
  return pin;
}

// Сохраняет новый PIN в NVS. Возвращает true при успехе.
bool saveBlePin(uint32_t newPin)
{
  if (newPin > 999999UL) return false; // PIN должен быть 6 цифр (000000-999999)

  blePrefs.begin(BLE_PIN_NVS_NAMESPACE, false); // read-write
  size_t written = blePrefs.putUInt(BLE_PIN_NVS_KEY, newPin);
  blePrefs.end();

  return written > 0;
}

#ifdef DEBUG
void debug_readcommand()
{
  if (!Serial.available()) return;

  // Если первый пришедший байт похож на начало HEX-ввода ('0' перед 'x'/'X'
  // или сразу шестнадцатеричная цифра) - трактуем всю строку как HEX-команду.
  // Иначе - старое поведение с однобуквенными командами (z/s/p/a/d).
  int first = Serial.peek();

  if (first == '0' || isHexadecimalDigit((char)first))
  {
    // ждём перевод строки (терминал должен слать с "New line" / "\n")
    String s = Serial.readStringUntil('\n');
    s.trim();
    if (s.length() == 0) return;

    // strtol сам понимает префикс "0x"/"0X"; если префикса нет - тоже
    // считаем строку HEX-ом (база 16), чтобы можно было писать просто "10"
    uint8_t val = (uint8_t) strtol(s.c_str(), nullptr, 16);

    sendcommand(val);
    Serial.print(F(" Send raw HEX command: 0x"));
    if (val < 0x10) Serial.print('0'); // leading zero for readability
    Serial.print(val, HEX);
    Serial.println();
    return;
  }

  byte inbyte = Serial.read();
  if (inbyte == 'z')      { sendcommand(ZAPROS);     Serial.print(F(" Send Zapros Sostoyanya: "));  Serial.print(ZAPROS, HEX); }
  else if (inbyte == 's') { sendcommand(START_ENG);  Serial.print(F(" Send Start Engine: "));       Serial.print(START_ENG, HEX); }
  else if (inbyte == 'p') { sendcommand(STOP_ENG);   Serial.print(F(" Send Stop Engine: "));        Serial.print(STOP_ENG, HEX); }
  else if (inbyte == 'j') { sendcommand(OHRANA_ON);  Serial.print(F(" Send Ohrana ON: "));          Serial.print(OHRANA_ON, HEX); }
  else if (inbyte == 'k') { sendcommand(OHRANA_OFF); Serial.print(F(" Send Ohrana OFF: "));         Serial.print(OHRANA_OFF, HEX); }
  else if (inbyte == 'v') { sendcommand(VALET_ON);   Serial.print(F(" Send Valet ON: "));           Serial.print(VALET_ON, HEX); }
  else if (inbyte == 'n') { sendcommand(VALET_OFF);  Serial.print(F(" Send Valet OFF: "));          Serial.print(VALET_OFF, HEX); }
  else if (inbyte == 't') { trunkRequest(); Serial.println(F(" Trunk request (disarm if needed)")); }
  else if (inbyte == 'h') { triggerOutput(HORN_PIN, PULSE_DURATION_MS);  Serial.println(F(" Horn output 1s")); }
  else if (inbyte == 'P')
  {
    // Смена PIN-кода BLE-сопряжения без перепрошивки: "P123456" + Enter
    String s = Serial.readStringUntil('\n');
    s.trim();
    bool onlyDigits = s.length() > 0 && s.length() <= 6;
    for (size_t i = 0; onlyDigits && i < s.length(); i++) if (!isDigit(s[i])) onlyDigits = false;

    if (onlyDigits)
    {
      uint32_t newPin = (uint32_t) s.toInt();
      if (saveBlePin(newPin))
      {
        currentBlePin = newPin;
        // apply immediately, no reboot needed — takes effect on next pairing
        // setPassKey() работает и с Bluedroid, и с NimBLE backend (в отличие
        // от прямого esp_ble_gap_set_security_param, доступного только в Bluedroid)
        BLESecurity::setPassKey(true, currentBlePin);
        Serial.print(F(" New PIN saved to NVS: "));
        Serial.println(currentBlePin);
        Serial.println(F(" (already-bonded devices don't need to re-pair; new PIN applies on next pairing)"));
      }
      else
      {
        Serial.println(F(" Error writing PIN to NVS"));
      }
    }
    else
    {
      Serial.println(F(" Invalid format. Example: P123456 (up to 6 digits)"));
    }
  }
  Serial.println();
}
#endif

// обновление переменных при получении ответа с верной контрольной суммой
void GoodResponse_received_from_starline(const uint32_t &data)
{
  Doors    = !(bool(((uint32_t)1 << 0)  & data));
  Trunk    = !(bool(((uint32_t)1 << 2)  & data));
  Hood     = !(bool(((uint32_t)1 << 3)  & data));
  Ignition = !(bool(((uint32_t)1 << 19) & data));
  Brake    = !(bool(((uint32_t)1 << 18) & data));
  Engine   = !(bool(((uint32_t)1 << 7)  & data));
  Locks    = !(bool(((uint32_t)1 << 5)  & data));
  Ohrana   = !(bool(((uint32_t)1 << 6)  & data));

  // Если ждали снятия охраны для багажника — переходим к ожиданию импульса
  if (trunkState == TRUNK_DISARM_SENT && !Ohrana)
  {
    trunkState = TRUNK_WAIT_BEFORE_PULSE;
    trunkTimerEnd = millis() + 1000UL; // ждём 1 сек перед открытием
  }
  Trevoga  = !(bool(((uint32_t)1 << 12) & data));
  Shocksensor = !(bool(((uint32_t)1 << 8) & data));
  Valet       = !(bool(((uint32_t)1 << 15) & data));

  // Tiltsensor = !(bool(((uint32_t)1 << ????) & data)); // бит датчика наклона пока не определён
  // 23-й бит - тип КПП: 1 - АКПП, 0 - МКПП

  lastStatusWord = data;
  statusDirty = true; // отправим по BLE в loop()
}

void setup()
{
#ifdef DEBUG
  Serial.begin(115200);
#endif
  pinMode(SLDATA_TX, OUTPUT);
  digitalWrite(SLDATA_TX, 1);
  pinMode(TRUNK_PIN, OUTPUT);
  pinMode(HORN_PIN, OUTPUT);
  digitalWrite(TRUNK_PIN, LOW);
  digitalWrite(HORN_PIN, LOW);

  sl_data.begin(SL_BAUDRATE);

  ds18b20.begin();
  tempReadDue = millis() + 1000; // первый замер через 1 сек после старта
  voltageReadDue = millis() + 2000; // первый замер напряжения через 2 сек
  analogReadResolution(12); // 12 бит (0-4095)
  analogSetAttenuation(ADC_11db); // измеряем до ~3.9V (нужен для делителя 20V)

  // --- BLE инициализация ---
  BLEDevice::init(SL_BLE_DEVICE_NAME);

  // --- Настройка безопасности: Bonding + PIN (Passkey Entry) ---
  // Bonding в BLE всегда подразумевает шифрование канала (требование
  // спецификации). Без сопряжения запись в CMD-характеристику будет
  // отклонена стеком. Режим ESP_IO_CAP_OUT ("Display Only") заставляет
  // телефон запросить у пользователя ввод PIN-кода при сопряжении;
  // так как у ESP32 нет дисплея, используем заранее заданный статический
  // passkey (хранится в NVS, см. loadBlePin/saveBlePin) вместо реальной
  // генерации/отображения.
  //
  // ВАЖНО про ESP32-C3: setEncryptionLevel() и прямой вызов
  // esp_ble_gap_set_security_param() существуют только в Bluedroid-варианте
  // BLE-стека. Часть сборок под C3 (в т.ч. текущая — pioarduino) по
  // умолчанию собирает BLE на NimBLE, где этих символов просто нет
  // (отсюда ошибка "esp_gap_ble_api.h: No such file"). Поэтому вместо них
  // используется BLESecurity::setPassKey() — единый метод, работающий
  // одинаково и на Bluedroid, и на NimBLE.
  currentBlePin = loadBlePin(); // читаем сохранённый PIN (или инициализируем значением по умолчанию)
  BLESecurity::setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND); // bonding + Secure Connection
  BLESecurity::setCapability(ESP_IO_CAP_OUT);                  // Display Only: требуется ввод PIN
  BLESecurity::setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());

  // Задаём статический PIN-код (загруженный из NVS), который стек BLE
  // будет использовать как passkey при сопряжении.
  BLESecurity::setPassKey(true, currentBlePin); // true = статический (не случайный) passkey

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  // BLE OTA-сервис (обновление прошивки без приложения/Wi-Fi).
  // Защищён bonding'ом — доступен только после сопряжения.
  initBleOta(bleServer);

  BLEService *service = bleServer->createService(SL_SERVICE_UUID);

  // Командная характеристика: клиент пишет 1 байт команды.
  // В Bluedroid требование шифрования задаётся правами доступа, а не
  // флагом в свойствах. Без bonding (зашифрованного канала) запись
  // будет отклонена стеком BLE.
  bleCmdChar = service->createCharacteristic(
      SL_CMD_CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE);
  bleCmdChar->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  bleCmdChar->setCallbacks(new CmdCallbacks());

  // Статусная характеристика: ESP32 шлёт уведомления (8 байт LE)
  bleStatusChar = service->createCharacteristic(
      SL_STATUS_CHAR_UUID,
      BLECharacteristic::PROPERTY_NOTIFY);
  bleStatusChar->addDescriptor(new BLE2902());

  service->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SL_SERVICE_UUID);
  adv->addServiceUUID(OTA_SERVICE_UUID);
  adv->start();

#ifdef DEBUG
  Serial.println(F("StarLine SL-Data + BLE bridge ready!!!"));
  Serial.print(F("BLE pairing PIN: "));
  Serial.println(currentBlePin);
  Serial.println();
  Serial.println(F("Commands:"));
  Serial.println(F("  z - request status"));
  Serial.println(F("  s - start engine"));
  Serial.println(F("  p - stop engine"));
  Serial.println(F("  j - arm ON"));
  Serial.println(F("  k - arm OFF"));
  Serial.println(F("  v - Valet ON"));
  Serial.println(F("  n - Valet OFF"));
  Serial.println(F("  t - open trunk"));
  Serial.println(F("  h - horn (1s)"));
  Serial.println(F("  Change PIN: send 'P' + new PIN (up to 6 digits) e.g. P654321"));
  Serial.println(F("  <hex> - send raw HEX command, e.g. 42"));
#endif
}

void handleTemperature()
{
  uint32_t now = millis();
  if (tempConversionStarted && now >= tempReadDue)
  {
    ds18b20.requestTemperatures();
    float t = ds18b20.getTempCByIndex(0);
    if (t != DEVICE_DISCONNECTED_C && t != lastTemperature)
    {
      lastTemperature = t;
      statusDirty = true;
    }
    tempConversionStarted = false;
    tempReadDue = now + 5000;
  }
  if (!tempConversionStarted && now >= tempReadDue)
  {
    ds18b20.requestTemperatures();
    tempConversionStarted = true;
    tempReadDue = now + 1000;
  }
}

void handleVoltage()
{
  uint32_t now = millis();
  if (now >= voltageReadDue)
  {
    int adcValue = analogRead(ADC_PIN);
    float voltageOut = (adcValue / 4095.0) * maxADCVoltage;
    float v = voltageOut * (R1 + R2) / R2;
    // Всегда обновляем при первом измерении (lastVoltage == -1.0f)
    // или при изменении значения
    bool firstRead = (lastVoltage < 0.0f);
    if (firstRead || abs(v - lastVoltage) > 0.1f) {
      lastVoltage = v;
      statusDirty = true;
    }
    voltageReadDue = now + 10000; // следующая проверка через 10 сек
  }
}

void handlePulseOutputs()
{
  uint32_t now = millis();
  if (trunkPulseEnd != 0 && now >= trunkPulseEnd)
  {
    digitalWrite(TRUNK_PIN, LOW);
    trunkPulseEnd = 0;
  }
  if (hornPulseEnd != 0 && now >= hornPulseEnd)
  {
    digitalWrite(HORN_PIN, LOW);
    hornPulseEnd = 0;
  }
}

void triggerOutput(uint8_t pin, uint32_t durationMs)
{
  digitalWrite(pin, HIGH);
  if (pin == TRUNK_PIN) trunkPulseEnd = millis() + durationMs;
  if (pin == HORN_PIN)  hornPulseEnd  = millis() + durationMs;
}

// Машина состояний для отпирания багажника с авто-снятием охраны
void handleTrunkState()
{
  uint32_t now = millis();
  switch (trunkState)
  {
    case TRUNK_IDLE:
      break;

    case TRUNK_DISARM_SENT:
      // ждём обновления Ohrana в GoodResponse_received_from_starline()
      break;

    case TRUNK_WAIT_BEFORE_PULSE:
      if (now >= trunkTimerEnd)
      {
        // Отправляем команду багажника в шину SL-Data
        sendcommand(TRUNK_CMD);
        // И активируем GPIO
        triggerOutput(TRUNK_PIN, PULSE_DURATION_MS);
        trunkState = TRUNK_IDLE;
      }
      break;
  }
}

// Инициировать открытие багажника (с учётом состояния охраны)
void trunkRequest()
{
  if (Ohrana)
  {
    // Сначала снимаем с охраны
    sendcommand(OHRANA_OFF);
    trunkState = TRUNK_DISARM_SENT;
  }
  else
  {
    // Охрана не активна — открываем сразу
    sendcommand(TRUNK_CMD);
    triggerOutput(TRUNK_PIN, PULSE_DURATION_MS);
  }
}

// Обработка BLE-команд, связанных с PIN-кодом сопряжения
// CMD_GET_PIN (0x60): возвращает текущий PIN через статусную характеристику
// CMD_SET_PIN (0x61): устанавливает новый PIN (5 байт: 0x61 + PIN uint32 LE)
void handleBlePinCommand()
{
  if (bleCmd == CMD_GET_PIN)
  {
    // Отправляем текущий PIN как ответ по статусной характеристике
    // Используем специальный формат: type=0x01 (response), затем PIN
    #ifdef DEBUG
    Serial.println(F("BLE: CMD_GET_PIN received, sending current PIN"));
    #endif
    sendPinResponse(0x01, currentBlePin);
  }
  else if (bleCmd == CMD_SET_PIN && bleMultiCmd && bleCmdDataLen == 5)
  {
    // Извлекаем новый PIN из буфера (LE uint32)
    uint32_t newPin = ((uint32_t)bleCmdData[1])
                    | ((uint32_t)bleCmdData[2] << 8)
                    | ((uint32_t)bleCmdData[3] << 16)
                    | ((uint32_t)bleCmdData[4] << 24);
    
    #ifdef DEBUG
    Serial.print(F("BLE: CMD_SET_PIN received, new PIN = "));
    Serial.println(newPin);
    #endif

    if (newPin > 999999UL)
    {
      #ifdef DEBUG
      Serial.println(F("BLE: PIN > 999999, rejecting"));
      #endif
      // type=0x02=error
      sendPinResponse(0x02, currentBlePin);
      return;
    }

    if (saveBlePin(newPin))
    {
      currentBlePin = newPin;
      // Применяем немедленно: новый PIN будет использоваться при следующем сопряжении
      BLESecurity::setPassKey(true, currentBlePin);
      #ifdef DEBUG
      Serial.println(F("BLE: PIN saved successfully"));
      #endif
      // type=0x01=success, отправляем новый PIN для подтверждения
      sendPinResponse(0x01, currentBlePin);
    }
    else
    {
      #ifdef DEBUG
      Serial.println(F("BLE: PIN save failed"));
      #endif
      // type=0x02=error
      sendPinResponse(0x02, currentBlePin);
    }
  }
}

// Отправка ответа на PIN-запрос через статусную характеристику.
// Формат: 6 байт [0x70=pin_response, type, pin_LE...]
// type: 0x01=success (содержит актуальный PIN), 0x02=error
void sendPinResponse(uint8_t type, uint32_t pin)
{
  if (!deviceConnected || !bleStatusChar) return;

  uint8_t buf[6];
  buf[0] = 0x70; // маркер PIN-ответа
  buf[1] = type; // тип: success/error
  buf[2] = pin & 0xFF;
  buf[3] = (pin >> 8) & 0xFF;
  buf[4] = (pin >> 16) & 0xFF;
  buf[5] = (pin >> 24) & 0xFF;

  bleStatusChar->setValue(buf, 6);
  bleStatusChar->notify();
}

void loop()
{
#ifdef DEBUG
  debug_readcommand();
#endif

  sl_data_read(); // приём состояния от сигнализации
  handleTemperature(); // неблокирующий опрос DS18B20
  handleVoltage(); // неблокирующее измерение напряжения
  handlePulseOutputs(); // неблокирующее управление доп. каналами
  handleTrunkState(); // машина состояний багажника

  // Команда, пришедшая по BLE, выполняется здесь (вне BLE-callback).
  // ВСЕ команды (включая ZAPROS 0x42) отправляем в шину SL-Data —
  // сигнализация ответит словом состояния, которое дальше уйдёт по BLE.
  if (bleCmdPending)
  {
    bleCmdPending = false;
    if (bleCmd == CMD_GET_PIN || bleCmd == CMD_SET_PIN)
    {
      handleBlePinCommand();
    }
    else if (bleCmd == TRUNK_CMD)
    {
      trunkRequest();
    }
    else if (bleCmd == HORN_CMD)
    {
      triggerOutput(HORN_PIN, PULSE_DURATION_MS);
    }
    else
    {
      sendcommand(bleCmd);
    }
  }

  // Автоматическая отправка статуса по BLE при ЛЮБОМ изменении значения.
  // statusDirty устанавливается в GoodResponse_received_from_starline()
  // при получении валидного слова от сигнализации (то есть при каждом
  // реальном обновлении состояния по шине SL-Data).
  // Также отправляем статус с датчиками каждые 10 секунд независимо.
  static uint32_t lastStatusSend = 0;
  uint32_t now = millis();
  if (deviceConnected && (statusDirty || (now - lastStatusSend >= 10000)))
  {
    statusDirty = false;
    lastStatusSend = now;
    sendStatusBLE(lastStatusWord);
  }

  delay(10);
}