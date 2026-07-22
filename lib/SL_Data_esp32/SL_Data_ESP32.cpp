#include "SL_Data_ESP32.h"

// статика
volatile uint32_t SL_Data::_buffer[SL_Data::BUF_SIZE];
volatile uint8_t  SL_Data::_head = 0;
volatile uint8_t  SL_Data::_tail = 0;
SL_Data          *SL_Data::_active = nullptr;

SL_Data::SL_Data(uint8_t rxPin, uint8_t txPin, bool invertLogic)
  : _rxPin(rxPin), _txPin(txPin), _invert(invertLogic),
    _bitPeriodUs(0), _halfBitUs(0)
{
}

SL_Data::~SL_Data()
{
  end();
}

void SL_Data::begin(uint32_t baud)
{
  _bitPeriodUs = 1000000UL / baud;
  _halfBitUs   = _bitPeriodUs / 2;

  _head = 0;
  _tail = 0;

  // Как и в оригинале: подтяжка включается только для неинвертированной
  // логики (в инвертированном варианте линией обычно управляет источник
  // с открытым коллектором/эмиттером в другую сторону — если у вас иначе,
  // поменяйте на INPUT_PULLUP или INPUT_PULLDOWN по месту).
  if (_invert)
    pinMode(_rxPin, INPUT);
  else
    pinMode(_rxPin, INPUT_PULLUP);

  _active = this;
  attachInterrupt(digitalPinToInterrupt(_rxPin), isrTrampoline, CHANGE);
}

void SL_Data::end()
{
  if (_active == this)
  {
    detachInterrupt(digitalPinToInterrupt(_rxPin));
    _active = nullptr;
  }
}

bool SL_Data::available()
{
  return _head != _tail;
}

uint32_t SL_Data::reaD()
{
  if (_head == _tail)
    return 0;

  // Индексы 8-битные, читаются/пишутся атомарно на ESP32, отдельная
  // критическая секция не нужна для одного производителя/потребителя.
  uint32_t d = _buffer[_head];
  _head = (_head + 1) % BUF_SIZE;
  return d;
}

int32_t SL_Data::read()
{
  if (_head == _tail)
    return -1;
  return (int32_t)reaD();
}

/* static */ void SL_Data::isrTrampoline()
{
  if (_active)
    _active->recv();
}

// Обработчик фронта. Логика перенесена из AVR-версии:
//  - при срабатывании прерывания проверяем, действительно ли это старт-бит
//    (учитывая инверсию), иначе выходим (эквивалент "мимо" в оригинале)
//  - ждём половину битового интервала (центрирование выборки)
//  - 32 раза: ждём один битовый интервал, читаем пин, сдвигаем в d
//  - если invert_logic - инвертируем всё слово целиком
//  - кладём в кольцевой буфер
//
// Внимание: это блокирующий обработчик прерывания длительностью
// ~0.5 + 32 битовых интервала (для 4800 бод это ~6.8 мс). На ESP32 это
// приемлемо для одиночного bit-bang протокола на низкой скорости, но
// если в проекте активны Wi-Fi/BT — учитывайте, что на это время
// обработка радиостека на этом ядре будет придерживаться работы ISR.
void SL_Data::recv()
{
  bool level = digitalRead(_rxPin);
  bool isStart = _invert ? level : !level;
  if (!isStart)
    return;

  uint32_t d = 0;

  delayMicroseconds(_halfBitUs);

  for (uint8_t i = 32; i > 0; --i)
  {
    delayMicroseconds(_bitPeriodUs);
    d >>= 1;
    if (digitalRead(_rxPin))
      d |= 0x80000000UL;
  }

  if (_invert)
    d = ~d;

  uint8_t next = (_tail + 1) % BUF_SIZE;
  if (next != _head)
  {
    _buffer[_tail] = d;
    _tail = next;
  }
  // при переполнении буфера (next == _head) слово молча теряется,
  // как и переполнение в оригинале (там тоже просто выставлялся флаг,
  // который в этом скетче не проверялся)
}