#include <SL_Data32.h>

#define SLDATA_RX 7 // выбираем пин RX ардуино на sl-data, на меге это пины, имеющие pcint 
                    // данные идут от сигнализации на этот пин (подключить к правому пину разъема SLdata)
                    // средний пин разъема SL-Data сигналки это GND

#define SLDATA_TX 6 //выбираем пин TX ардуино на sl-data, данные идут с этого пина на сигнализацию (на левый пин разъема SLdata)

#define BIT_DURATION  200  //время одного бита в микросекундах (для отправки данных на сигналку)
#define SL_BAUDRATE   4800 //скорость soft uart (для получения данных от сигналки)

//#define STOP_BIT  // закоментировать, для отсутствия стоп бита в отправке команды на старлайн
#define DEBUG     // раскоменнтировать если нужна отладка. закоментировать в итоговом скетче проекта 


SL_Data sl_data(SLDATA_RX, 22, 1); // (RX, TX, 1==invert_logic)  // пины правленного софт сериал

// команды управления сигнализацией
const uint8_t ZAPROS     = 0x42;
const uint8_t START_ENG  = 0x21;
const uint8_t STOP_ENG   = 0x20;
const uint8_t OHRANA_ON  = 0x11;
const uint8_t OHRANA_OFF = 0x10;

// переменные работы сигнализации 
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

#ifdef DEBUG
void printdata (const uint32_t &data, const bool &CSgood) 
{
 Serial.print (F("Receive:  "));
 Serial.print(F("    0x")); Serial.print(data, HEX); Serial.print(F("     BIN:  ")); 
 for  (int i =0; i<32; i++) {Serial.print(bitRead(data, 31-i)); if (i==7||i==15||i==23)Serial.print(" ");}
      Serial.println(); 
 Serial.print (" Door: "); Serial.print (Doors);  Serial.print ("  Ign: "); Serial.print (Ignition);
 Serial.print ("  Brake: "); Serial.print (Brake); Serial.print ("  Trunk: "); Serial.print (Trunk);
 Serial.print ("  Hood: "); Serial.print (Hood); Serial.print ("  Locks: "); Serial.print (Locks);
 Serial.print ("  Ohrana: "); Serial.print (Ohrana); Serial.print ("  Engine: "); Serial.print (Engine);
 Serial.print ("  Trevoga: "); Serial.print (Trevoga);
 Serial.print ("  Shock: "); Serial.print (Shocksensor); Serial.print ("  Tilt: "); Serial.println (Tiltsensor);
 if (CSgood) Serial.println ("Checksumm is good!!!"); else  Serial.println ("Checksumm is Error!!!");
Serial.println(); 
}
#endif


void sl_data_read ()
{
   // получаем данные (слово 32 bit) от сигнализации 
 if (sl_data.available()) 
   {
    uint32_t data = (uint32_t)sl_data.reaD(); // слово, полученное от сигнализации
    if (data!=0){
    byte answer[3] = {0} ; 
answer[0] = data;
answer[1] = (data & 0x0000FF00)>>8;
answer[2] = (data & 0x00FF0000)>>16;
answer[3] = (data & 0xFF000000)>>24;
byte CS = 0 ; 
CS= ~(answer[0]+answer[1]+answer[2]+2);
bool CSgood = (CS==answer[3]);
if (CSgood) {GoodResponse_received_from_starline(data);}   ;
 #ifdef DEBUG  
      printdata(data, CSgood);
 #endif     
                }
   }

}

void sendword (uint8_t &inbyte)
{
  
  digitalWrite (SLDATA_TX , 1);    // send start bit 
  delayMicroseconds (BIT_DURATION); 
  for (byte g=0; g<2; g++){
  for (int i =0; i<8; i++)
  {
  if (!g) digitalWrite (SLDATA_TX ,  bitRead(inbyte, i));  else digitalWrite (SLDATA_TX ,  !bitRead(inbyte, i)); //send data bits
  delayMicroseconds (BIT_DURATION); 
  }
  
  }
#ifdef STOP_BIT
digitalWrite (SLDATA_TX , 0); // send stop bit
delayMicroseconds (BIT_DURATION);
#endif
}

void sendcommand (uint8_t comm)
{
   
  for (int g = 0; g<5; g++)
  {
      digitalWrite (SLDATA_TX , 0);
      delay (10);
      sendword(comm);
//   sl_data.wriTe(comm);  
  } 
  digitalWrite (SLDATA_TX , 1);
  }

#ifdef DEBUG
void debug_readcommand()
{
if (Serial.available())
  {
     byte inbyte = Serial.read();
     if (inbyte == 'z') {sendcommand (ZAPROS),    Serial.print(F(" Send Zapros Sostoyanya: ")); Serial.print (ZAPROS,HEX);}
else if (inbyte == 's') {sendcommand (START_ENG), Serial.print(F(" Send Start Engine: ")); Serial.print (START_ENG,HEX);}
else if (inbyte == 'p') {sendcommand (STOP_ENG),  Serial.print(F(" Send Stop Engine: ")); Serial.print (STOP_ENG,HEX);}
else if (inbyte == '1') {sendcommand (OHRANA_ON), Serial.print(F(" Send Ohrana ON: "));  Serial.print (OHRANA_ON,HEX);} 
else if (inbyte == '0') {sendcommand (OHRANA_OFF),Serial.print(F(" Send Ohrana OFF: ")); Serial.print (OHRANA_OFF,HEX);}
Serial.println();
  }
}

#endif

// в этой функции обновляем переменные, если получен ответ от старлайн с правильной контрольной суммой

void GoodResponse_received_from_starline(const uint32_t &data)
{
Doors    = !(bool(((uint32_t)1 << 0)  &  data));
Trunk    = !(bool(((uint32_t)1 << 2)  &  data));
Hood     = !(bool(((uint32_t)1 << 3)  &  data));
Ignition = !(bool(((uint32_t)1 << 19) &  data));
Brake    = !(bool(((uint32_t)1 << 18) &  data));
Engine   = !(bool(((uint32_t)1 << 7) &  data));     
Locks    = !(bool(((uint32_t)1 << 5)  &  data));
Ohrana   = !(bool(((uint32_t)1 << 6)  &  data));
Trevoga  = !(bool(((uint32_t)1 << 12)  &  data));
Shocksensor  =  !(bool(((uint32_t)1 << 14)  &  data));    // 
//Tiltsensor =  !(bool(((uint32_t)1 << ????)  &  data));  // тут нужно выяснить какой бит отвечает за сработку датчика наклона
// 23й бит это тип КПП, 1 - АКПП, 0 - МКПП. 
 
  }

void setup() 
{
  #ifdef DEBUG
  Serial.begin(115200);
  #endif
  sl_data.begin(SL_BAUDRATE);
  pinMode (SLDATA_TX , OUTPUT);
  digitalWrite (SLDATA_TX , 1);
}


void loop() 
{ 
  #ifdef DEBUG
 debug_readcommand();
  #endif
  
 sl_data_read (); // в скетче проекта в лупе оставляем только эту функцию
}
