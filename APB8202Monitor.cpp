#include "APB8202Monitor.h"
#include <Arduino.h>

namespace {
static const int RX_PIN=18, SAFE_PIN=17;
static const size_t MAX_EDGES=8192;
static volatile uint32_t edgeUs[MAX_EDGES];
static volatile size_t edgeCount=0;
static int initialLevel=LOW;
static bool started=false, finished=false;
static uint32_t firstEdgeMs=0, lastSeenMs=0;
static size_t lastCount=0;

void IRAM_ATTR edgeISR(){
  size_t i=edgeCount;
  if(i<MAX_EDGES){ edgeUs[i]=(uint32_t)micros(); edgeCount=i+1; }
}

int levelAt(uint32_t t,size_t n){
  int level=initialLevel;
  for(size_t i=0;i<n && edgeUs[i]<=t;i++) level=!level;
  return level;
}

struct Result { uint32_t baud; bool inverted; size_t good,bad,count; uint8_t data[256]; };

Result decode(uint32_t baud,bool inv,size_t n){
  Result r={baud,inv,0,0,0,{0}};
  const float bit=1000000.0f/(float)baud;
  size_t i=0;
  while(i<n && r.count<256){
    int before=(i==0)?initialLevel:((initialLevel+(int)i)&1);
    int after=!before;
    if(inv){before=!before;after=!after;}
    if(before==HIGH && after==LOW){
      uint32_t start=edgeUs[i];
      uint8_t b=0;
      for(int k=0;k<8;k++){
        int v=levelAt(start+(uint32_t)((1.5f+k)*bit),n);
        if(inv)v=!v;
        if(v)b|=(1U<<k);
      }
      int stop=levelAt(start+(uint32_t)(9.5f*bit),n);
      if(inv)stop=!stop;
      if(stop==HIGH){
        r.good++;
        r.data[r.count++]=b;
        uint32_t end=start+(uint32_t)(10.0f*bit);
        while(i<n && edgeUs[i]<end)i++;
        continue;
      } else r.bad++;
    }
    i++;
  }
  return r;
}

void analyze(size_t n){
  detachInterrupt(digitalPinToInterrupt(RX_PIN));
  Serial0.println();
  Serial0.println("========== APB AUTOMATIC PASSIVE ANALYSIS ==========");
  Serial0.printf("Edges captured: %u\n",(unsigned)n);
  if(n<3){Serial0.println("Not enough activity to analyse.");finished=true;return;}

  uint32_t hist[201]={0},minDt=0xFFFFFFFFUL,maxDt=0;
  for(size_t i=1;i<n;i++){
    uint32_t d=edgeUs[i]-edgeUs[i-1];
    if(d<minDt)minDt=d; if(d>maxDt)maxDt=d;
    if(d>=1 && d<=200)hist[d]++;
  }
  uint32_t mode=0,hits=0;
  for(uint32_t u=1;u<=200;u++)if(hist[u]>hits){hits=hist[u];mode=u;}
  Serial0.printf("Dominant edge interval: %lu us (%lu hits)\n",(unsigned long)mode,(unsigned long)hits);
  if(mode)Serial0.printf("Raw timing estimate: ~%lu Hz if this is one bit/cycle\n",(unsigned long)(1000000UL/mode));
  Serial0.printf("Interval range: %lu .. %lu us\n",(unsigned long)minDt,(unsigned long)maxDt);

  const uint32_t rates[]={57600,76800,92160,93750,96000,100000,115200,128000,230400,460800,921600};
  Result best={0,false,0,0,0,{0}};
  Serial0.println();
  Serial0.println("UART software-decode candidates (no UART peripheral used):");
  for(size_t q=0;q<sizeof(rates)/sizeof(rates[0]);q++){
    for(int inv=0;inv<2;inv++){
      Result r=decode(rates[q],inv!=0,n);
      size_t total=r.good+r.bad;
      unsigned score=total?(unsigned)(100UL*r.good/total):0;
      Serial0.printf("  %6lu 8N1 %s : valid=%u invalid=%u score=%u%% bytes=%u\n",
        (unsigned long)r.baud,r.inverted?"INVERTED":"NORMAL",
        (unsigned)r.good,(unsigned)r.bad,score,(unsigned)r.count);
      size_t bt=best.good+best.bad;
      unsigned bs=bt?(unsigned)(100UL*best.good/bt):0;
      if(r.count && (score>bs || (score==bs && r.good>best.good)))best=r;
    }
  }

  Serial0.println();
  if(!best.count){
    Serial0.println("RESULT: No convincing 8N1 UART decode found.");
  }else{
    size_t total=best.good+best.bad;
    unsigned score=total?(unsigned)(100UL*best.good/total):0;
    Serial0.printf("BEST: %lu baud, 8N1, %s, framing score %u%%\n",
      (unsigned long)best.baud,best.inverted?"INVERTED":"NORMAL",score);
    Serial0.printf("Decoded bytes (%u max shown):\n",(unsigned)best.count);
    for(size_t i=0;i<best.count;i++){
      if((i%16)==0)Serial0.println();
      if(best.data[i]<16)Serial0.print('0');
      Serial0.print(best.data[i],HEX); Serial0.print(' ');
    }
    Serial0.println();
    bool hci=false;
    for(size_t i=0;i<best.count;i++)if(best.data[i]==0x04){hci=true;break;}
    Serial0.printf("H4/HCI event marker 0x04 present: %s\n",hci?"YES (candidate only)":"NO");
    if(score<80)Serial0.println("WARNING: framing score is weak; do not treat bytes as proven UART.");
  }
  Serial0.println();
  Serial0.println("GPIO18 stayed input-only. GPIO17 stayed input-only. Serial1 was never started.");
  Serial0.println("=====================================================");
  finished=true;
}
}

namespace APB8202Monitor {
bool begin(){
  pinMode(RX_PIN,INPUT);
  pinMode(SAFE_PIN,INPUT);
  initialLevel=digitalRead(RX_PIN);
  edgeCount=0;lastCount=0;started=false;finished=false;
  attachInterrupt(digitalPinToInterrupt(RX_PIN),edgeISR,CHANGE);
  Serial0.println("[BOOT] Passive automatic analyser ARMED.");
  Serial0.printf("[BOOT] GPIO18 initial level: %s\n",initialLevel?"HIGH":"LOW");
  Serial0.println("[BOOT] GPIO17/18 input-only; Serial1 NOT started.");
  Serial0.println("[BOOT] Turn APB power ON now.");
  Serial0.println("[BOOT] Capturing first activity plus later boot bursts automatically.");
  return true;
}
void update(){
  if(finished)return;
  size_t n=edgeCount;
  if(n!=lastCount){
    lastCount=n;lastSeenMs=millis();
    if(!started){started=true;firstEdgeMs=lastSeenMs;Serial0.println("[BOOT] Activity detected; capturing...");}
  }
  if(n>=MAX_EDGES){analyze(n);return;}
  if(started && ((millis()-firstEdgeMs)>=6500 || ((millis()-lastSeenMs)>=1500 && (millis()-firstEdgeMs)>=5500))) analyze(n);
}
}
