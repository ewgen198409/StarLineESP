/*
SL_Data_ESP32.h

Порт библиотеки SL_Data (софт-приёмник протокола StarLine SL-Data,
исходно написан как форк SoftwareSerial под AVR/PCINT) на ESP32.

Оригинальная AVR-версия читала данные через Pin Change Interrupt (PCINT)
и цикло-точные задержки _delay_loop_2 — оба механизма жёстко привязаны
к архитектуре AVR и на ESP32 не существуют. Здесь та же логика (детект
старт-бита по фронту, выборка 32 бит с фиксированным периодом, финальная
инверсия всего слова при invert_logic) реализована через:
  - attachInterrupt(..., CHANGE)  вместо PCINT
  - delayMicroseconds()           вместо _delay_loop_2

ВАЖНО про уровни сигналов:
  ESP32 GPIO — это 3.3В логика и НЕ толерантны к 5В. Если родная плата
  StarLine (SLDATA_RX/TX) работает на 5В TTL — обязательно добавьте
  делитель напряжения (или транзисторный ключ) на линию RX перед ESP32,
  и на линию TX либо делитель/буфер, либо резистор + проверку, что 3.3В
  уверенно читается стороной StarLine как логическая единица. Без этого
  есть риск повредить вход ESP32 или получить нестабильный приём/передачу.

TX в этой библиотеке НЕ реализован (как и в оригинале) — передача
командного слова в скетче делается вручную через digitalWrite/
delayMicroseconds (SLDATA_TX), это не AVR-специфичный код и переносится
без изменений.
*/

#ifndef SL_Data_ESP32_h
#define SL_Data_ESP32_h

#include <Arduino.h>

class SL_Data
{
public:
  // rxPin       - пин приёма (должен поддерживать attachInterrupt; на ESP32
  //               это практически любой GPIO, включая input-only 34-39,
  //               но НЕ они же для TX)
  // txPin       - сохраняется для совместимости API, самим классом не
  //               используется (см. комментарий выше)
  // invertLogic - как в оригинале: true, если сигнал инвертирован
  SL_Data(uint8_t rxPin, uint8_t txPin, bool invertLogic = false);
  ~SL_Data();

  void begin(uint32_t baud);
  void end();

  bool available();

  // Оставлены оба имени, как в оригинальной библиотеке (read()/reaD()
  // использовались как синонимы для 32-битного слова)
  uint32_t reaD();
  int32_t  read();

  bool isListening() const { return _active == this; }

private:
  static void IRAM_ATTR isrTrampoline();
  void recv();

  uint8_t  _rxPin;
  uint8_t  _txPin;
  bool     _invert;

  uint32_t _bitPeriodUs;
  uint32_t _halfBitUs;

  static const uint8_t BUF_SIZE = 8; // размер буфера в 32-битных словах

  static volatile uint32_t _buffer[BUF_SIZE];
  static volatile uint8_t  _head;
  static volatile uint8_t  _tail;

  static SL_Data *_active; // поддерживается один активный приёмник, как в оригинале
};

#endif