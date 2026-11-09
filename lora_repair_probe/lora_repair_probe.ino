// Temporary bench probe: RadioLib SX126x_Receive_Interrupt lifecycle,
// Heltec official board-config.h V3 pins. Same image on both V3s.
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
SX1262 radio = new Module(8,14,12,13);
volatile bool received=false;
void IRAM_ATTR radioIrq(){ received=true; }
uint32_t uid, sent=0, got=0; unsigned long nextTx;
void check(const char* op,int st){Serial.printf("%s=%d\n",op,st);if(st){while(true)delay(1000);}}
void setup(){
 Serial.begin(115200);delay(200);
 uid=(uint32_t)(ESP.getEfuseMac() >> 16);
 Serial.printf("DB_PROBE uid=%08lx MAC=%012llx\n",uid,ESP.getEfuseMac());
 pinMode(36,OUTPUT);digitalWrite(36,LOW);
 SPI.begin(9,11,10,8);
 check("begin",radio.begin(915.0,250.0,7,5,0x12,0,8,1.8,false));
 check("DIO2",radio.setDio2AsRfSwitch(true));
 check("CRC",radio.setCRC(2));
 radio.setPacketReceivedAction(radioIrq);
 check("startRX",radio.startReceive());
 nextTx=millis()+1500+(uid%700);
}
void loop(){
 if(received){
  received=false;
  size_t n=radio.getPacketLength();
  uint8_t buf[256]={0};int st=radio.readData(buf,n);
  float rssi=radio.getRSSI(),snr=radio.getSNR();
  Serial.printf("RX n=%u st=%d RSSI=%.1f SNR=%.1f data=",(unsigned)n,st,rssi,snr);
  Serial.write(buf,n);Serial.println();
  if(st==0)got++;
  check("rearm",radio.startReceive());
 }
 if((int32_t)(millis()-nextTx)>=0){
  radio.standby();received=false;
  char msg[80];snprintf(msg,sizeof(msg),"DB_PROBE uid=%08lx seq=%lu",uid,++sent);
  int st=radio.transmit(msg);
  Serial.printf("TX seq=%lu st=%d RXtotal=%lu\n",sent,st,got);
  received=false;check("postTX_RX",radio.startReceive());
  nextTx=millis()+1400+esp_random()%800;
 }
 delay(1);
}
