#include "APB8202Monitor.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "ProjectConfig.h"

namespace {

static uint32_t currentBaud = ProjectConfig::APB_MONITOR_DEFAULT_BAUD;
static uint32_t totalBytes = 0;
static uint32_t totalBursts = 0;
static uint32_t totalTxBytes = 0;

static uint8_t rawBurst[96];
static size_t rawBurstLength = 0;
static uint32_t lastByteMs = 0;

static char terminalLine[192];
static size_t terminalLength = 0;

static const uint32_t kProbeBauds[] = {
  9600,
  38400,
  57600,
  115200,
  230400,
  460800,
  576000,
  750000,
  921600,
  1000000,
  1152000,
  1250000,
  1300000,
  1350000,
  1400000,
  1500000,
  1600000,
  1750000,
  1843200,
  2000000,
  2250000,
  2500000,
  2750000,
  3000000
};
static const size_t kProbeBaudCount =
    sizeof(kProbeBauds) / sizeof(kProbeBauds[0]);

enum AutoScanState { AUTO_IDLE, AUTO_SETTLE, AUTO_WAIT_REPLY, AUTO_DONE };
static AutoScanState autoState = AUTO_IDLE;
static size_t autoBaudIndex = 0;
static uint32_t autoDeadlineMs = 0;
static uint32_t autoRxStart = 0;
static uint32_t autoTxStart = 0;
static bool autoAnyReply = false;
static uint32_t autoReplyBaud = 0;
static bool listenMode = false;
static bool autoListenMode = false;
static size_t autoListenBaudIndex = 0;
static uint32_t autoListenDeadlineMs = 0;
static uint32_t autoListenRxStart = 0;
static const uint32_t kAutoListenDwellMs = 2000;

// Raw GPIO timing analyser. ISR stores edge-to-edge intervals in microseconds.
// This bypasses UART decoding completely.
static const size_t kEdgeSampleCount = 4096;
static volatile uint32_t edgeIntervals[kEdgeSampleCount]; // CPU cycles between edges
static volatile size_t edgeCount = 0;
static volatile uint32_t edgeLastCycles = 0;
static volatile bool edgeCaptureActive = false;

void IRAM_ATTR edgeISR()
{
  if (!edgeCaptureActive) return;
  uint32_t now = ESP.getCycleCount();
  uint32_t previous = edgeLastCycles;
  edgeLastCycles = now;
  if (previous && edgeCount < kEdgeSampleCount)
    edgeIntervals[edgeCount++] = now - previous;
}

void printBurst();

void consolePrintln(const char *s) { Serial0.println(s); }

bool configureUart(uint32_t baud)
{
  if (!baud) return false;
  if (rawBurstLength) printBurst();

  Serial1.end();
  Serial1.setRxBufferSize(2048);
  Serial1.begin(baud, SERIAL_8N1,
                ProjectConfig::APB_UART_RX_GPIO,
                ProjectConfig::APB_UART_TX_GPIO);

  currentBaud = baud;
  Serial0.printf("[APB] UART GPIO18 RX / GPIO17 TX @ %lu 8N1\n",
                 (unsigned long)baud);
  Serial0.println("[APB] No bytes are transmitted automatically.");
  return true;
}

void printHelp()
{
  Serial0.println();
  Serial0.println("[APB] Two-way RAW UART discovery console");
  Serial0.println("  Pin 5 TXD -> GPIO18 RX");
  Serial0.println("  Pin 6 RXD <- GPIO17 TX");
  Serial0.println("  Pin 7 CTS -> disconnected");
  Serial0.println();
  Serial0.println("Commands:");
  Serial0.println("  :baud <rate>       change UART speed");
  Serial0.println("  :nextbaud          cycle common speeds");
  Serial0.println("  :stats             counters");
  Serial0.println("  :clear             clear counters");
  Serial0.println("  :hex 01 03 0C 00   transmit exact bytes");
  Serial0.println("  :text abc          transmit exact text, no CR/LF");
  Serial0.println("  :hcireset          send standard H4 HCI Reset: 01 03 0C 00");
  Serial0.println("  :autoscan          automatically test every baud and print result");
  Serial0.println("  :listen            RX-only: continuously listen to the chip");
  Serial0.println("  :listen <rate>     set baud then continuously listen RX-only");
  Serial0.println("  :autolisten        RX-only rolling scan of ALL baud rates");
  Serial0.println("  :analyze           measure raw GPIO18 edge timing for 10 seconds");
  Serial0.println("  :stop              stop listen/autolisten mode");
  Serial0.println("  :help");
  Serial0.println();
  Serial0.println("Expected H4 event packet type is 04 if this UART exposes HCI.");
}

void printBurst()
{
  if (!rawBurstLength) return;
  ++totalBursts;

  Serial0.printf("[APB RX] baud=%lu burst=%lu len=%u HEX:",
                 (unsigned long)currentBaud,
                 (unsigned long)totalBursts,
                 (unsigned)rawBurstLength);
  for (size_t i=0;i<rawBurstLength;i++) Serial0.printf(" %02X", rawBurst[i]);

  Serial0.print("  ASCII: ");
  for (size_t i=0;i<rawBurstLength;i++) {
    uint8_t b=rawBurst[i];
    Serial0.write((b>=32 && b<=126) ? b : '.');
  }
  Serial0.println();
  rawBurstLength=0;
}

void captureUart()
{
  while (Serial1.available()) {
    int v=Serial1.read();
    if (v<0) break;
    if (rawBurstLength>=sizeof(rawBurst)) printBurst();
    rawBurst[rawBurstLength++]=(uint8_t)v;
    ++totalBytes;
    lastByteMs=millis();
  }

  if (rawBurstLength &&
      millis()-lastByteMs >= ProjectConfig::APB_MONITOR_BURST_GAP_MS)
    printBurst();
}

void sendBytes(const uint8_t *data, size_t len)
{
  if (!data || !len) return;
  size_t n=Serial1.write(data,len);
  Serial1.flush();
  totalTxBytes += n;

  Serial0.printf("[APB TX] %u bytes HEX:", (unsigned)n);
  for (size_t i=0;i<n;i++) Serial0.printf(" %02X",data[i]);
  Serial0.println();
}

void sendHex(const char *p)
{
  uint8_t data[128];
  size_t n=0;

  while (*p) {
    while (*p==' ' || *p=='\t') ++p;
    if (!*p) break;

    char *end=nullptr;
    unsigned long v=strtoul(p,&end,16);
    if (end==p || v>0xFF || n>=sizeof(data)) {
      Serial0.println("[APB] Invalid :hex input; packet NOT sent.");
      return;
    }
    data[n++]=(uint8_t)v;
    p=end;
  }

  if (!n) {
    Serial0.println("[APB] No hex bytes supplied.");
    return;
  }
  sendBytes(data,n);
}

void startAutoScan()
{
  listenMode = false;
  autoListenMode = false;
  if (autoState != AUTO_IDLE && autoState != AUTO_DONE) {
    Serial0.println("[AUTO] scan already running");
    return;
  }

  autoBaudIndex = 0;
  autoAnyReply = false;
  autoReplyBaud = 0;
  autoRxStart = totalBytes;
  autoTxStart = totalTxBytes;
  Serial0.println();
  Serial0.println("============================================");
  Serial0.println(" APB AUTOMATIC HCI UART BAUD SCAN");
  Serial0.println(" Tests UART rates from 9600 through 3,000,000 baud");
  Serial0.println(" 3 Mbaud is the documented CW6638M HCI UART ceiling.");
  Serial0.println(" Sends H4 HCI Reset 01 03 0C 00 at each rate");
  Serial0.println("============================================");
  configureUart(kProbeBauds[autoBaudIndex]);
  autoDeadlineMs = millis() + 250;
  autoState = AUTO_SETTLE;
}

void updateAutoScan()
{
  if (autoState == AUTO_IDLE || autoState == AUTO_DONE) return;

  captureUart();

  if ((int32_t)(millis() - autoDeadlineMs) < 0) return;

  if (autoState == AUTO_SETTLE) {
    const uint8_t reset[] = {0x01,0x03,0x0C,0x00};
    const uint32_t before = totalBytes;
    Serial0.printf("[AUTO] Testing %lu baud...\n",
                   (unsigned long)kProbeBauds[autoBaudIndex]);
    sendBytes(reset, sizeof(reset));
    autoRxStart = before;
    autoDeadlineMs = millis() + 1500;
    autoState = AUTO_WAIT_REPLY;
    return;
  }

  if (autoState == AUTO_WAIT_REPLY) {
    captureUart();
    const uint32_t received = totalBytes - autoRxStart;
    if (received > 0) {
      autoAnyReply = true;
      autoReplyBaud = kProbeBauds[autoBaudIndex];
      Serial0.printf("[AUTO] >>> RX DETECTED at %lu baud: %lu byte(s) <<<\n",
                     (unsigned long)autoReplyBaud,
                     (unsigned long)received);
    } else {
      Serial0.printf("[AUTO] no RX at %lu baud\n",
                     (unsigned long)kProbeBauds[autoBaudIndex]);
    }

    ++autoBaudIndex;
    if (autoBaudIndex >= kProbeBaudCount) {
      Serial0.println();
      Serial0.println("=============== AUTO RESULT ===============");
      if (autoAnyReply) {
        Serial0.printf("RESULT: RX DATA DETECTED. Last responding baud: %lu\n",
                       (unsigned long)autoReplyBaud);
        Serial0.println("Inspect the [APB RX] HEX output above.");
      } else {
        Serial0.println("RESULT: NO RX DATA AT ANY TESTED BAUD.");
        Serial0.println("TX worked, but no UART reply was detected.");
      }
      Serial0.printf("TOTAL SCAN TX=%lu bytes  CURRENT TOTAL RX=%lu bytes\n",
                     (unsigned long)(totalTxBytes-autoTxStart),
                     (unsigned long)totalBytes);
      Serial0.println("===========================================");
      autoState = AUTO_DONE;
      return;
    }

    configureUart(kProbeBauds[autoBaudIndex]);
    autoDeadlineMs = millis() + 250;
    autoState = AUTO_SETTLE;
  }
}

void startListen(uint32_t baud)
{
  if (baud && baud != currentBaud) configureUart(baud);
  listenMode = true;
  Serial0.println();
  Serial0.println("=============== APB LISTEN MODE ===============");
  Serial0.printf("RX ONLY on GPIO18 at %lu baud. NOTHING will be transmitted.\n",
                 (unsigned long)currentBaud);
  Serial0.println("Listening continuously; incoming bytes appear as [APB RX].");
  Serial0.println("Use :baud <rate> to change speed or :stop to leave listen mode.");
  Serial0.println("================================================");
}

void startAutoListen()
{
  autoState = AUTO_IDLE;
  listenMode = false;
  autoListenMode = true;
  autoListenBaudIndex = 0;
  autoListenRxStart = totalBytes;
  configureUart(kProbeBauds[0]);
  autoListenDeadlineMs = millis() + kAutoListenDwellMs;

  Serial0.println();
  Serial0.println("============ APB ROLLING AUTO-LISTEN ============");
  Serial0.println("RX ONLY. NOTHING is transmitted to the BT module.");
  Serial0.println("Automatically cycling every configured baud rate.");
  Serial0.println("2 seconds per baud; repeats forever until :stop.");
  Serial0.println("Any received bytes are printed immediately as [APB RX].");
  Serial0.println("==================================================");
}

void updateAutoListen()
{
  if (!autoListenMode) return;
  if ((int32_t)(millis() - autoListenDeadlineMs) < 0) return;

  captureUart();
  uint32_t received = totalBytes - autoListenRxStart;
  if (received) {
    Serial0.printf("[AUTO-LISTEN] *** %lu RX byte(s) detected at %lu baud ***\n",
                   (unsigned long)received,
                   (unsigned long)kProbeBauds[autoListenBaudIndex]);
  }

  autoListenBaudIndex = (autoListenBaudIndex + 1) % kProbeBaudCount;
  configureUart(kProbeBauds[autoListenBaudIndex]);
  autoListenRxStart = totalBytes;
  autoListenDeadlineMs = millis() + kAutoListenDwellMs;
}


void analyzeRawSignal()
{
  autoState = AUTO_IDLE;
  autoListenMode = false;
  listenMode = false;
  if (rawBurstLength) printBurst();

  Serial1.end();
  delay(20);
  pinMode(ProjectConfig::APB_UART_RX_GPIO, INPUT);

  edgeCount = 0;
  edgeLastCycles = 0;
  edgeCaptureActive = true;
  attachInterrupt(digitalPinToInterrupt(ProjectConfig::APB_UART_RX_GPIO),
                  edgeISR, CHANGE);

  Serial0.println();
  Serial0.println("============= RAW GPIO18 SIGNAL ANALYSER =============");
  Serial0.println("UART decoder OFF. Measuring Pin 5 -> GPIO18 directly.");
  Serial0.println("Capture time: 10 seconds. Generate BT activity now.");
  Serial0.println("Nothing is transmitted to the APB module.");

  uint32_t start = millis();
  while (millis() - start < 10000 && edgeCount < kEdgeSampleCount) {
    delay(10);
  }

  edgeCaptureActive = false;
  detachInterrupt(digitalPinToInterrupt(ProjectConfig::APB_UART_RX_GPIO));

  size_t n = edgeCount;
  Serial0.println();
  Serial0.println("================ RAW SIGNAL RESULT ================");
  Serial0.printf("Edges/intervals captured: %u\n", (unsigned)n);

  if (n < 8) {
    Serial0.println("RESULT: NOT ENOUGH EDGE ACTIVITY TO ANALYSE.");
    Serial0.println("Pin 5 was mostly static during this capture.");
  } else {
    const double cpuMHz = (double)ESP.getCpuFreqMHz();
    uint32_t samples[kEdgeSampleCount];
    noInterrupts();
    for (size_t i=0; i<n; ++i) samples[i]=edgeIntervals[i];
    interrupts();

    uint32_t minCycles=0xFFFFFFFFUL, maxCycles=0;
    for (size_t i=0;i<n;++i) {
      if(samples[i]<minCycles) minCycles=samples[i];
      if(samples[i]>maxCycles) maxCycles=samples[i];
    }
    Serial0.printf("CPU timing clock: %.0f MHz\n", cpuMHz);
    Serial0.printf("Shortest pulse: %.3f us\n", minCycles/cpuMHz);
    Serial0.printf("Longest pulse:  %.3f us\n", maxCycles/cpuMHz);

    // Score each candidate baud by asking whether each observed pulse width
    // is close to an integer multiple (1..12 bits) of that candidate bit time.
    struct Score { uint32_t baud; uint32_t matched; double error; };
    Score scores[kProbeBaudCount];
    for(size_t bi=0;bi<kProbeBaudCount;++bi){
      const uint32_t baud=kProbeBauds[bi];
      const double bitCycles=(cpuMHz*1000000.0)/(double)baud;
      uint32_t matched=0; double error=0.0;
      for(size_t i=0;i<n;++i){
        double mult=(double)samples[i]/bitCycles;
        int nearest=(int)(mult+0.5);
        if(nearest<1 || nearest>12) continue;
        double e=mult-nearest; if(e<0)e=-e;
        // +/-18% of one bit allows ISR latency/jitter but rejects poor fits.
        if(e<=0.18){ ++matched; error+=e; }
      }
      scores[bi]={baud,matched,error};
    }
    // Sort best match count first, then lowest accumulated fractional error.
    for(size_t i=0;i<kProbeBaudCount;i++) for(size_t j=i+1;j<kProbeBaudCount;j++){
      bool better=scores[j].matched>scores[i].matched ||
        (scores[j].matched==scores[i].matched && scores[j].error<scores[i].error);
      if(better){Score t=scores[i];scores[i]=scores[j];scores[j]=t;}
    }

    Serial0.println();
    Serial0.println("Top UART timing candidates:");
    size_t show=kProbeBaudCount<5?kProbeBaudCount:5;
    for(size_t i=0;i<show;++i){
      double pct=100.0*(double)scores[i].matched/(double)n;
      double bitUs=1000000.0/(double)scores[i].baud;
      Serial0.printf("  #%u  %lu baud  bit=%.3f us  match=%.1f%% (%lu/%u)\n",
        (unsigned)(i+1),(unsigned long)scores[i].baud,bitUs,pct,
        (unsigned long)scores[i].matched,(unsigned)n);
    }
    double bestPct=100.0*(double)scores[0].matched/(double)n;
    Serial0.println();
    if(bestPct>=70.0) Serial0.println("SIGNAL TIMING: STRONGLY UART-LIKE for the best listed candidate.");
    else if(bestPct>=45.0) Serial0.println("SIGNAL TIMING: POSSIBLY UART-LIKE; candidate is not conclusive.");
    else Serial0.println("SIGNAL TIMING: NO STRONG UART BIT-TIMING MATCH.");
    Serial0.printf("BEST TIMING CANDIDATE: %lu baud (%.1f%% pulse fit)\n",
                   (unsigned long)scores[0].baud,bestPct);
    Serial0.println("This is timing evidence only; valid UART framing/data is still unproven.");
  }
  Serial0.println("===================================================");

  Serial0.println("[APB] UART remains OFF after analysis to prevent garbage flooding.");
  Serial0.println("[APB] Use :baud <rate>, :listen <rate>, or :autolisten to re-enable it.");
}

void handleLine(char *line)
{
  if (!line || !*line) return;

  if (!strcmp(line,":help")) {
    printHelp();
  } else if (!strcmp(line,":listen")) {
    startListen(0);
  } else if (!strncmp(line,":listen ",8)) {
    uint32_t baud=strtoul(line+8,nullptr,10);
    if (!baud) Serial0.println("[APB] invalid listen baud");
    else startListen(baud);
  } else if (!strcmp(line,":autolisten")) {
    startAutoListen();
  } else if (!strcmp(line,":analyze")) {
    analyzeRawSignal();
  } else if (!strcmp(line,":stop")) {
    listenMode=false;
    autoListenMode=false;
    Serial0.println("[APB] listen/autolisten stopped");
  } else if (!strcmp(line,":stats")) {
    Serial0.printf("[APB] baud=%lu RX=%lu bursts=%lu TX=%lu\n",
      (unsigned long)currentBaud,(unsigned long)totalBytes,
      (unsigned long)totalBursts,(unsigned long)totalTxBytes);
  } else if (!strcmp(line,":clear")) {
    APB8202Monitor::clearCounters();
    totalTxBytes=0;
    Serial0.println("[APB] counters cleared");
  } else if (!strcmp(line,":nextbaud")) {
    APB8202Monitor::nextBaud();
  } else if (!strncmp(line,":baud ",6)) {
    uint32_t baud=strtoul(line+6,nullptr,10);
    if (!APB8202Monitor::setBaud(baud)) Serial0.println("[APB] invalid baud");
  } else if (!strncmp(line,":hex ",5)) {
    sendHex(line+5);
  } else if (!strncmp(line,":text ",6)) {
    const uint8_t *p=(const uint8_t *)(line+6);
    sendBytes(p,strlen(line+6));
  } else if (!strcmp(line,":hcireset")) {
    const uint8_t reset[]={0x01,0x03,0x0C,0x00};
    sendBytes(reset,sizeof(reset));
  } else if (!strcmp(line,":autoscan")) {
    startAutoScan();
  } else {
    Serial0.println("[APB] Unknown command. Use :help");
  }
}

void readConsole()
{
  while (Serial0.available()) {
    char c=(char)Serial0.read();
    if (c=='\r' || c=='\n') {
      if (terminalLength) {
        terminalLine[terminalLength]='\0';
        handleLine(terminalLine);
        terminalLength=0;
      }
    } else if (terminalLength<sizeof(terminalLine)-1) {
      terminalLine[terminalLength++]=c;
    } else {
      terminalLength=0;
      Serial0.println("[APB] command too long; discarded");
    }
  }
}

} // namespace

namespace APB8202Monitor {

bool begin()
{
  Serial0.println("=== APB8202 / CW6638M RAW UART DISCOVERY ===");
  totalBytes=0; totalBursts=0; totalTxBytes=0;
  rawBurstLength=0; terminalLength=0; lastByteMs=millis();
  if (!configureUart(ProjectConfig::APB_MONITOR_DEFAULT_BAUD)) return false;
  printHelp();
  return true;
}

void update()
{
  captureUart();
  updateAutoScan();
  updateAutoListen();
  readConsole();
}

bool setBaud(uint32_t baud)
{
  if (!baud) return false;
  if (baud==currentBaud) {
    Serial0.printf("[APB] already at %lu baud\n",(unsigned long)baud);
    return true;
  }
  return configureUart(baud);
}

bool nextBaud()
{
  for (size_t i=0;i<kProbeBaudCount;i++)
    if (kProbeBauds[i]==currentBaud)
      return setBaud(kProbeBauds[(i+1)%kProbeBaudCount]);
  return setBaud(kProbeBauds[0]);
}

uint32_t baudRate(){return currentBaud;}
uint32_t bytesSeen(){return totalBytes;}
uint32_t burstsSeen(){return totalBursts;}

void clearCounters()
{
  totalBytes=0; totalBursts=0; rawBurstLength=0;
}

} // namespace APB8202Monitor
