#include <Arduino.h>
#include <EEPROM.h>

// ==================== АДАПТАЦИЯ ПОД ESP32 (PlatformIO) ====================
// Что изменено относительно оригинала под AVR/Arduino Uno:
//
// 1. Пины SLDATA_RX/SLDATA_TX перенесены с 3/2 на 18/19.
//    На Uno пины 2/3 были жёстко зашиты, т.к. код напрямую дёргал регистр
//    PORTD (биты 2 и 3) и использовал "старую" нумерацию внеш. прерывания
//    attachInterrupt(1, ...). На ESP32 такой номер прерывания не имеет
//    смысла (там прерывание вешается на конкретный GPIO), а пины 1 и 3 —
//    это аппаратный UART0 (Serial), который у нас занят под отладку.
//    Поэтому пины теперь можно назначить почти на любой GPIO
//    (кроме input-only 34-39 для TX и strapping-пинов 0/2/5/12/15,
//    которых лучше избегать). При необходимости поменяйте ниже.
//
// 2. Прямая работа с PORTD/PIND заменена на digitalRead()/digitalWrite().
//    Скорости digitalWrite на ESP32 (240 МГц) с запасом хватает для
//    протокола с шагом 100 мкс.
//
// 3. Таймер2 (TCCR2A/TCCR2B/OCR2A/TIMSK2, ISR(TIMER2_COMPA_vect)) заменён
//    на аппаратный hw_timer_t ESP32 с тем же периодом 100 мкс.
//
// 4. attachInterrupt(1, ...) / detachInterrupt(1) заменены на
//    attachInterrupt(digitalPinToInterrupt(SLDATA_RX), ...) — так пины
//    можно менять свободно, без привязки к номерам "старых" INTx.
//
// 5. Обработчики прерываний (по таймеру и по внешнему GPIO) помечены
//    IRAM_ATTR — на ESP32 код прерываний должен быть в IRAM, иначе
//    возможен краш при обращении к флэшу во время прерывания.
//
// 6. EEPROM на ESP32 эмулируется во флэше: добавлен EEPROM.begin(size)
//    в setup() и EEPROM.commit() после каждой записи (без commit()
//    данные не сохранятся).
//
// ВАЖНО про тайминги: на голом AVR прерывания были детерминированы.
// На ESP32 крутится FreeRTOS + (потенциально) Wi-Fi/BT стек, поэтому
// возможен небольшой джиттер обработки прерываний. В этом проекте
// Wi-Fi/BT не используется, так что на практике всё должно тактироваться
// стабильно, но если увидите "CLOCKING from Starline is not end" или
// ошибки CS чаще, чем на Uno — проверьте осциллографом реальные тайминги.
// ============================================================================

#define SLDATA_RX 18  // пин ESP32, роль RX оригинального GSM — подключать к TX сигнализации
#define SLDATA_TX 19  // пин ESP32, роль TX оригинального GSM — подключать к RX сигнализации

#define SL_DEBUG // раскомментировать/закомментировать, если нужна отладка в терминал.

volatile byte SL_dataRX[10] = {0}; // буфер приема данных от сигналки
enum SL_statesRX {WAIT_FALL, WAITSTROB, CLOCKING_DETECTED};    // возможные состояния приемника
volatile byte SL_stateRX = WAIT_FALL ;  // переменная состояния приемника
volatile byte SL_bitNumberRX  = 0 ; 
volatile byte SL_byteNumberRX = 0 ;  
volatile bool SLmessage_received  = 0;   // флаг что принято новое сообщение от сигналки

volatile byte SL_dataTX[10]= {0}; // буфер передачи данных на сигналку (пакет байт команды)
enum SL_statesTX {WAIT_CLOCKING, PROPUSK, TXbyCLOCK, TXtoIDLE}; //возможные состояния передатчика 
volatile byte SL_stateTX = WAIT_CLOCKING ;   // переменная состояния передатчика
volatile byte SL_bitNumberTX  = 0 ; 
volatile byte SL_byteNumberTX = 0 ;  

byte SL_key[2]={0}; // байты ключа сигнализации

// таймеры возврата в bus-Idle в случае неответов от сигналки
volatile bool Timer_waitCLOCKING_from_Starline    = 0; 
uint32_t prev_waitCLOCKING_from_Starline = 0;
enum protectTimer_states {TIMER_OFF, TIMER_WAIT_RESPONSE, TIMER_PROTECT, TIMER_LASTSTATE};
byte Timer_waitRESPONSE_from_Starline    = 0; 
uint32_t prev_waitRESPONSE_from_Starline = 0;

byte SL_Timer_needRepeat          = 0; 
uint32_t prev_SL_Timer_needRepeat = 0;
volatile bool SL_needPropusk = 1 ;
byte SL_RepeatCommand = 0 ; 
byte SL_CountRepeat = 0 ; 

// ---- Замена аппаратного Timer2 на ESP32 hw_timer ----
hw_timer_t *SL_timer = NULL;
volatile bool TX_pin_state = HIGH; // текущее состояние линии TX (для программного тоггла)

void onTimerISR(); // предварительное объявление обработчика таймера (IRAM_ATTR ставится только на определении ниже)

#define CLOCK_ON  timerStart(SL_timer)
#define CLOCK_OFF timerStop(SL_timer)

// ---- Прототипы функций ----
// В PlatformIO файл main.cpp (в отличие от .ino в Arduino IDE) не проходит
// через авто-генерацию прототипов, поэтому объявляем их явно.
void SL_Setup();
void SL_Data();
void send_CLOCKING_to_Starline();
void endRX();
void processing_of_received_SLmessages();
#ifdef SL_DEBUG
void Command_to_Starline_from_Terminal();
#endif
void SL_sendcommand(const byte &SLcommand);
void Accept_clocking_and_transmit_bits(); // (IRAM_ATTR ставится только на определении ниже)
void endTX();
void timers_to_protect_against_failures();
void Reset_SL_GSM();
void Registration_SL_GSM();

const uint8_t SL_ZAPROS     = 0x42;
const uint8_t SL_START_ENG  = 0x21;
const uint8_t SL_STOP_ENG   = 0x20;
const uint8_t SL_OHRANA_ON  = 0x11;
const uint8_t SL_OHRANA_OFF = 0x10;

bool Doors    = 0 ; 
bool Locks    = 0 ;
bool Ohrana   = 0 ;
bool Trunk    = 0 ; 
bool Hood     = 0 ; 
bool Ignition = 0 ; 
bool Brake    = 0 ; 
bool Engine   = 0 ; 
bool Trevoga  = 0 ; 
bool Tiltsensor   = 0 ; 
bool Shocksensor  = 0 ; 

 

void setup() 
{
 SL_Setup();

}

void loop() 
{
 SL_Data(); // работа протокола SL_DATA
}


void SL_Setup(){
   pinMode (SLDATA_TX, OUTPUT);       // линию TX на выход 
   digitalWrite (SLDATA_TX, 1);       // и ставим в busIdle
   TX_pin_state = HIGH;
   pinMode (SLDATA_RX, INPUT_PULLUP); // линию RX на вход с подтяжкой

#ifdef SL_DEBUG
Serial.begin(115200); Serial.println(); Serial.println(F("ESP32 Restart"));
Serial.print(F("Starline Key:  "));
#endif

EEPROM.begin(512); // на ESP32 обязательно перед чтением/записью EEPROM

for (byte i=0; i<2; i++)
{
    SL_key[i] = EEPROM.read(i+200); 
   #ifdef SL_DEBUG
    if (SL_key[i]<0xF){Serial.print("0");} Serial.print(SL_key[i], HEX); Serial.print(F(" "));
   #endif
}
 #ifdef SL_DEBUG
 Serial.println();
 #endif

//---------настройка аппаратного таймера ESP32 для тактирования сигналки
// (обработчик будет вызываться раз в 100 мкс, аналогично Timer2 на AVR).
// Новый API Arduino-ESP32 core 3.x: timerBegin принимает частоту в Гц.
SL_timer = timerBegin(1000000);           // частота тика таймера 1 МГц => 1 тик = 1 мкс
timerAttachInterrupt(SL_timer, &onTimerISR);
timerAlarm(SL_timer, 100, true, 0);       // период 100 мкс, автоперезапуск, без лимита срабатываний
timerStop(SL_timer); // таймер пока НЕ включаем (как и в оригинале) — включится по CLOCK_ON
  
  }


void SL_Data()
{
//таймеры сброса тактирования или сброса линии TX при передаче в случае неответов от оппонента
 timers_to_protect_against_failures();

 #ifdef SL_DEBUG
 //иницирование отправки команд на сигналку через терминал
 Command_to_Starline_from_Terminal();
 #endif
 
 //обработка принятых сообщений от сигналки
 processing_of_received_SLmessages();
 
 //отправка сигнала тактирования на сигналку 
 send_CLOCKING_to_Starline();
}

void send_CLOCKING_to_Starline(){

if (Timer_waitRESPONSE_from_Starline!=TIMER_PROTECT){ // тактируем, если не включена защитная задержка глюков связи
  
bool curRX = digitalRead(SLDATA_RX); 
static bool lastRX = curRX;
 if (lastRX != curRX){ // если состояние линии RX изменилось
 if (SL_stateTX==WAIT_CLOCKING && SL_stateRX == WAIT_FALL) // если статус приемника и передатчика "ждем спад"
  {
  if (lastRX && !curRX){ // если увидели спад на RX, значит сигналка хочет передавать, ниже отправляем ей тактирвание
  SL_stateRX = WAITSTROB;
  delayMicroseconds (100);
  if (digitalRead(SLDATA_RX)) {SL_stateRX = WAIT_FALL; return;} // если было ложное срабатывание RX пина, выходим. 
  //ниже включение таймера защиты от возможных глюков при неответах
  if (Timer_waitRESPONSE_from_Starline==TIMER_OFF) {Timer_waitRESPONSE_from_Starline = TIMER_WAIT_RESPONSE; prev_waitRESPONSE_from_Starline = millis();}
  digitalWrite (SLDATA_TX,0); TX_pin_state = LOW;  // короткий строб в ноль, делаем всё по снифферской осциллограмме с ори обмена данными
  delayMicroseconds (40); // длина строба
  CLOCK_ON; // включаем тактирование для оппонента. 
  }
  }
 lastRX = curRX;
 }
                                                  }
}

// обработчик вылазит раз в 100мкс (аппаратный таймер ESP32),
// тактируем для сигналки и читаем очередной бит от неё
void IRAM_ATTR onTimerISR()
{
       if (SL_stateRX == WAITSTROB) {SL_stateRX = CLOCKING_DETECTED; TX_pin_state = !TX_pin_state; digitalWrite(SLDATA_TX, TX_pin_state);} 
  else if (SL_stateRX == CLOCKING_DETECTED) 
      {
      if (digitalRead(SLDATA_RX)){SL_dataRX[SL_byteNumberRX]|= (1<<(SL_bitNumberRX));}// читаем 
      else {SL_dataRX[SL_byteNumberRX]&=~(1<<(SL_bitNumberRX));}                     // очередной бит 
      TX_pin_state = !TX_pin_state; digitalWrite(SLDATA_TX, TX_pin_state); // тактируем, изменив состояние пина TX на противоположное 
      if (SL_bitNumberRX==7 && SL_byteNumberRX == 9) {endRX(); SLmessage_received = 1; return;} // сообщение полностью принято
      SL_bitNumberRX = SL_bitNumberRX + 1; if (SL_bitNumberRX==8){SL_bitNumberRX=0; SL_byteNumberRX = SL_byteNumberRX + 1;} // рост счётчиков бит и байт
       }
}
void endRX(){CLOCK_OFF; SL_bitNumberRX=0; SL_byteNumberRX=0; SL_stateRX = WAIT_FALL;}

void processing_of_received_SLmessages()

{if (SLmessage_received){
  #ifdef SL_DEBUG
    Serial.print ("Starline Response:    ");
  #endif 
    byte CS = 0 ;
    for (int i=0; i<10; i++) 
{
    if (i<9)CS+=SL_dataRX[i]; // подсчёт CS
    #ifdef SL_DEBUG
    if (SL_dataRX[i]<=0xF)Serial.print("0"); Serial.print(SL_dataRX[i], HEX); Serial.print(" "); 
    #endif
    if (i==9) 
     {
      #ifdef SL_DEBUG
      Serial.print("    CS is "); 
      #endif 
            if (CS==SL_dataRX[i]) // если получен валидный ответ от сигналки
            { 
              Timer_waitCLOCKING_from_Starline = 0;         // сбрасываем таймер ожидания тактирования от сигналки
              Timer_waitRESPONSE_from_Starline = TIMER_OFF; // сбрасываем таймер ожидания ответа от сигналки
              SL_Timer_needRepeat = 0 ; SL_CountRepeat = 0 ; 
              #ifdef SL_DEBUG
              Serial.println("OK!   "); 
              #endif
              if (SL_dataRX[0]==0x0B && SL_dataRX[4]==0x00) // принята процедура привязки GSM, запоминаем ключ Starline
              {
                 Registration_SL_GSM();
                #ifdef SL_DEBUG 
                Serial.println(F("Registration GSM successfully completed!")); 
                #endif
              }
else {
              
Doors       = (bool((1 << 0) & SL_dataRX[1]));
Trunk       = (bool((1 << 2) & SL_dataRX[1]));
Hood        = (bool((1 << 3) & SL_dataRX[1]));
Ignition    = (bool((1 << 3) & SL_dataRX[3]));
Engine      = (bool((1 << 3) & SL_dataRX[7]));
if ((bool((1 << 2) & SL_dataRX[3])) || (bool((1 << 2) & SL_dataRX[2])))Brake = 1; else Brake = 0;
Locks       = (bool((1 << 5) & SL_dataRX[1]));
Ohrana      = (bool((1 << 6) & SL_dataRX[1]));
Trevoga     = (bool((1 << 4) & SL_dataRX[2])); 
Shocksensor = (bool((1 << 1) & SL_dataRX[5])); 
Tiltsensor  = (bool((1 << 0) & SL_dataRX[6])); 
// 7 бит третьего байта это тип КПП , 1-МКПП, 0-АКПП

#ifdef SL_DEBUG
 Serial.print (" Door: "); Serial.print (Doors);  Serial.print ("  Ign: "); Serial.print (Ignition);
 Serial.print ("  Brake: "); Serial.print (Brake); Serial.print ("  Trunk: "); Serial.print (Trunk);
 Serial.print ("  Hood: "); Serial.print (Hood); Serial.print ("  Locks: "); Serial.print (Locks);
 Serial.print ("  Ohrana: "); Serial.print (Ohrana); Serial.print ("  Engine: "); Serial.print (Engine);
 Serial.print ("  Trevoga: "); Serial.print (Trevoga);
 Serial.print ("  Shock: "); Serial.print (Shocksensor); Serial.print ("  Tilt: "); Serial.println (Tiltsensor);
#endif
}
              
            }
            else  // Если CS неправильная
              {
              #ifdef SL_DEBUG
              Serial.println("Error!!!");
              #endif
              }  
      }
}
 
  SLmessage_received = 0 ; 
  #ifdef SL_DEBUG
  Serial.println();
  #endif
 }
}

#ifdef SL_DEBUG
void Command_to_Starline_from_Terminal() // инициирование отправки команд на сигналку с терминала
{
  if (Serial.available())
  { 
      Serial.println();
      byte inbyte = Serial.read();
       if (inbyte == 'z') {Serial.println (F(" Send SMS SL_ZAPROS Sostoyanya: ")); SL_sendcommand (SL_ZAPROS);}
  else if (inbyte == '1') {Serial.println (F(" Send SMS Ohrana ON: "));         SL_sendcommand (SL_OHRANA_ON);}
  else if (inbyte == '0') {Serial.println (F(" Send SMS Ohrana OFF: "));        SL_sendcommand (SL_OHRANA_OFF);}
  else if (inbyte == 's') {Serial.println (F(" Send SMS Dvig ON: "));         SL_sendcommand (SL_START_ENG);}
  else if (inbyte == 't') {Serial.println (F(" Send SMS Dvig OFF: "));        SL_sendcommand (SL_STOP_ENG);}
  else if (inbyte == 'r') {Serial.println (F(" GSM is reset!  "));    Reset_SL_GSM(); }
  else if (inbyte == 'p') {SL_needPropusk=!SL_needPropusk;  Serial.print  (F("Propusk "));  Serial.println (SL_needPropusk);}
  
  }
}
#endif

void SL_sendcommand(const byte &SLcommand)  // процедура отправки команды на сигналку
{
  if (SL_stateRX > WAIT_FALL) {delay (200);} 
  SL_RepeatCommand = SLcommand;
  SL_dataTX[0]=0x0A;
  SL_dataTX[1]=millis();
  SL_dataTX[2]=(SL_dataTX[1]^SLcommand)^SL_key[0];
  SL_dataTX[3]=(SL_dataTX[1]^SLcommand)^SL_key[1];
  SL_dataTX[4]=0xFF;
  byte CS = 0 ;
  for (byte i=0; i<5;i++) {CS+=SL_dataTX[i];}
  SL_dataTX[9]=CS;
  #ifdef SL_DEBUG
  Serial.print (" Send to Starline:  ");
  for (byte i=0; i<10;i++) {if (SL_dataTX[i]<=0xF)Serial.print("0"); Serial.print(SL_dataTX[i], HEX); Serial.print(" ");}
  Serial.println();
  #endif
  digitalWrite(SLDATA_TX, LOW); TX_pin_state = LOW; // садим в 0 линию передачи , инициируя желание передавать данные
  //Timer_waitCLOCKING_from_Starline = 1; prev_waitCLOCKING_from_Starline = millis(); // включаем таймер защиты  
  SL_Timer_needRepeat = 1; prev_SL_Timer_needRepeat =  millis();
  attachInterrupt(digitalPinToInterrupt(SLDATA_RX), Accept_clocking_and_transmit_bits, FALLING); // и ждем тактирования от сигналки (принимать тактирование будет внеш прерывание)
}


void IRAM_ATTR Accept_clocking_and_transmit_bits () //обработчик внешнего прерывания, выступает как приёмник тактирования от сигналки для выполнения нашей ей передачки. 
{
 
 if (SL_stateTX==WAIT_CLOCKING){if (SL_needPropusk) SL_stateTX=PROPUSK; else SL_stateTX=TXbyCLOCK; attachInterrupt(digitalPinToInterrupt(SLDATA_RX), Accept_clocking_and_transmit_bits, CHANGE);return;} 
//--------------ниже передача команды на шину по тактированию от сигналки-----------
 else if (SL_needPropusk && SL_stateTX==PROPUSK){SL_stateTX=TXbyCLOCK;}
 else if (SL_stateTX==TXbyCLOCK) // принимаем тактирование от сигналки и по нему отправляем побитно команду
 {
   if (SL_bitNumberTX==0 && SL_byteNumberTX==0) {Timer_waitCLOCKING_from_Starline = 1; prev_waitCLOCKING_from_Starline = millis();} // включаем таймер защиты  
   if (bool((1 << (SL_bitNumberTX)) & SL_dataTX[SL_byteNumberTX])) { digitalWrite(SLDATA_TX, HIGH); TX_pin_state = HIGH; } // читаем очередной бит из команды 
   else {digitalWrite(SLDATA_TX, LOW); TX_pin_state = LOW; }                                                   // и выставляем его на пине TX 
//----------------------------------------------------------------------------------    
    if (SL_byteNumberTX==9 && SL_bitNumberTX == 7)  { Timer_waitCLOCKING_from_Starline=0; endTX(); return;}// если все 80 бит переданы, выходим , сбросив переменные и откл внеш прерывание 
        SL_bitNumberTX = SL_bitNumberTX + 1; if (SL_bitNumberTX == 8) {SL_bitNumberTX = 0 ; SL_byteNumberTX = SL_byteNumberTX + 1;}
 }
}

void endTX() {detachInterrupt(digitalPinToInterrupt(SLDATA_RX)); SL_stateTX=TXtoIDLE; SL_bitNumberTX=0;  SL_byteNumberTX=0;}

void timers_to_protect_against_failures()
{
  if (SL_stateTX==TXtoIDLE){delayMicroseconds(100); digitalWrite(SLDATA_TX,1); TX_pin_state = HIGH; SL_stateTX=WAIT_CLOCKING;}
 
  if (Timer_waitCLOCKING_from_Starline && millis()- prev_waitCLOCKING_from_Starline>=200)
     {
      Timer_waitCLOCKING_from_Starline=0;
      endTX();
      #ifdef SL_DEBUG
  Serial.println ("      CLOCKING from Starline is not end!!!  ");
      #endif
     }

   if (SL_Timer_needRepeat && millis() - prev_SL_Timer_needRepeat>=3500) 
      {
        SL_CountRepeat++; if  (SL_CountRepeat>=3) SL_CountRepeat = 3 ; 
        SL_Timer_needRepeat = 0 ; 
        SL_needPropusk=!SL_needPropusk;
        if (SL_CountRepeat<3){SL_sendcommand (SL_RepeatCommand);}
      }

     
  if (Timer_waitRESPONSE_from_Starline>0 && millis() - prev_waitRESPONSE_from_Starline>=4000) 
     {
      
      Timer_waitRESPONSE_from_Starline++; 
       if (Timer_waitRESPONSE_from_Starline==TIMER_PROTECT) 
       {
         endRX(); 
         digitalWrite(SLDATA_TX,1); TX_pin_state = HIGH;
          #ifdef SL_DEBUG
          Serial.println (F("We are clocking , but we have not received a valid response from the Starline!!!"));
          #endif
       }
       else if (Timer_waitRESPONSE_from_Starline>=TIMER_LASTSTATE){Timer_waitRESPONSE_from_Starline = TIMER_OFF;}
       prev_waitRESPONSE_from_Starline = millis();
      
     }     
}

void Reset_SL_GSM () {SL_key[0]=0xFF; SL_key[1]=0xFF; EEPROM.write(200,0xFF);EEPROM.write(201,0xFF); EEPROM.commit();}

void Registration_SL_GSM () {SL_key[0]=SL_dataRX[1]; SL_key[1]=SL_dataRX[2]; EEPROM.write(200,SL_dataRX[1]);EEPROM.write(201,SL_dataRX[2]); EEPROM.commit();}