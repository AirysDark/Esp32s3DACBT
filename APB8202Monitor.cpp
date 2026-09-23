#include "APB8202Monitor.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "ProjectConfig.h"
#include "BLEDebugConsole.h"

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
static volatile uint32_t edgeIntervals[kEdgeSampleCount];
static volatile size_t edgeCount = 0;
static volatile uint32_t edgeLastUs = 0;
static volatile bool edgeCaptureActive = false;

void IRAM_ATTR edgeISR()
{
  if (!edgeCaptureActive) return;
  uint32_t now = (uint32_t)esp_timer_get_time();
  uint32_t previous = edgeLastUs;
  edgeLastUs = now;
  if (previous && edgeCount < kEdgeSampleCount)
    edgeIntervals[edgeCount++] = now - previous;
}

void printBurst();

void consolePrintln(const char *s) { DebugConsole.println(s); }

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
  DebugConsole.printf("[APB] UART GPIO18 RX / GPIO17 TX @ %lu 8N1\n",
                 (unsigned long)baud);
  DebugConsole.println("[APB] No bytes are transmitted automatically.");
  return true;
}

void printHelp()
{
  DebugConsole.println();
  DebugConsole.println("[APB] Two-way RAW UART discovery console");
  DebugConsole.println("  Pin 5 TXD -> GPIO18 RX");
  DebugConsole.println("  Pin 6 RXD <- GPIO17 TX");
  DebugConsole.println("  Pin 7 CTS -> disconnected");
  DebugConsole.println();
  DebugConsole.println("Commands:");
  DebugConsole.println("  :baud <rate>       change UART speed");
  DebugConsole.println("  :nextbaud          cycle common speeds");
  DebugConsole.println("  :stats             counters");
  DebugConsole.println("  :clear             clear counters");
  DebugConsole.println("  :hex 01 03 0C 00   transmit exact bytes");
  DebugConsole.println("  :text abc          transmit exact text, no CR/LF");
  DebugConsole.println("  :hcireset          send standard H4 HCI Reset: 01 03 0C 00");
  DebugConsole.println("  :autoscan          automatically test every baud and print result");
  DebugConsole.println("  :listen            RX-only: continuously listen to the chip");
  DebugConsole.println("  :listen <rate>     set baud then continuously listen RX-only");
  DebugConsole.println("  :autolisten        RX-only rolling scan of ALL baud rates");
  DebugConsole.println("  :analyze           measure raw GPIO18 edge timing for 10 seconds");
  DebugConsole.println("  :stop              stop listen/autolisten mode");
  DebugConsole.println("  :help");
  DebugConsole.println();
  DebugConsole.println("Expected H4 event packet type is 04 if this UART exposes HCI.");
}

void printBurst()
{
  if (!rawBurstLength) return;
  ++totalBursts;

  DebugConsole.printf("[APB RX] baud=%lu burst=%lu len=%u HEX:",
                 (unsigned long)currentBaud,
                 (unsigned long)totalBursts,
                 (unsigned)rawBurstLength);
  for (size_t i=0;i<rawBurstLength;i++) DebugConsole.printf(" %02X", rawBurst[i]);

  DebugConsole.print("  ASCII: ");
  for (size_t i=0;i<rawBurstLength;i++) {
    uint8_t b=rawBurst[i];
    DebugConsole.write((b>=32 && b<=126) ? b : '.');
  }
  DebugConsole.println();
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

  DebugConsole.printf("[APB TX] %u bytes HEX:", (unsigned)n);
  for (size_t i=0;i<n;i++) DebugConsole.printf(" %02X",data[i]);
  DebugConsole.println();
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
      DebugConsole.println("[APB] Invalid :hex input; packet NOT sent.");
      return;
    }
    data[n++]=(uint8_t)v;
    p=end;
  }

  if (!n) {
    DebugConsole.println("[APB] No hex bytes supplied.");
    return;
  }
  sendBytes(data,n);
}

void startAutoScan()
{
  listenMode = false;
  autoListenMode = false;
  if (autoState != AUTO_IDLE && autoState != AUTO_DONE) {
    DebugConsole.println("[AUTO] scan already running");
    return;
  }

  autoBaudIndex = 0;
  autoAnyReply = false;
  autoReplyBaud = 0;
  autoRxStart = totalBytes;
  autoTxStart = totalTxBytes;
  DebugConsole.println();
  DebugConsole.println("============================================");
  DebugConsole.println(" APB AUTOMATIC HCI UART BAUD SCAN");
  DebugConsole.println(" Tests UART rates from 9600 through 3,000,000 baud");
  DebugConsole.println(" 3 Mbaud is the documented CW6638M HCI UART ceiling.");
  DebugConsole.println(" Sends H4 HCI Reset 01 03 0C 00 at each rate");
  DebugConsole.println("============================================");
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
    DebugConsole.printf("[AUTO] Testing %lu baud...\n",
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
      DebugConsole.printf("[AUTO] >>> RX DETECTED at %lu baud: %lu byte(s) <<<\n",
                     (unsigned long)autoReplyBaud,
                     (unsigned long)received);
    } else {
      DebugConsole.printf("[AUTO] no RX at %lu baud\n",
                     (unsigned long)kProbeBauds[autoBaudIndex]);
    }

    ++autoBaudIndex;
    if (autoBaudIndex >= kProbeBaudCount) {
      DebugConsole.println();
      DebugConsole.println("=============== AUTO RESULT ===============");
      if (autoAnyReply) {
        DebugConsole.printf("RESULT: RX DATA DETECTED. Last responding baud: %lu\n",
                       (unsigned long)autoReplyBaud);
        DebugConsole.println("Inspect the [APB RX] HEX output above.");
      } else {
        DebugConsole.println("RESULT: NO RX DATA AT ANY TESTED BAUD.");
        DebugConsole.println("TX worked, but no UART reply was detected.");
      }
      DebugConsole.printf("TOTAL SCAN TX=%lu bytes  CURRENT TOTAL RX=%lu bytes\n",
                     (unsigned long)(totalTxBytes-autoTxStart),
                     (unsigned long)totalBytes);
      DebugConsole.println("===========================================");
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
  DebugConsole.println();
  DebugConsole.println("=============== APB LISTEN MODE ===============");
  DebugConsole.printf("RX ONLY on GPIO18 at %lu baud. NOTHING will be transmitted.\n",
                 (unsigned long)currentBaud);
  DebugConsole.println("Listening continuously; incoming bytes appear as [APB RX].");
  DebugConsole.println("Use :baud <rate> to change speed or :stop to leave listen mode.");
  DebugConsole.println("================================================");
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

  DebugConsole.println();
  DebugConsole.println("============ APB ROLLING AUTO-LISTEN ============");
  DebugConsole.println("RX ONLY. NOTHING is transmitted to the BT module.");
  DebugConsole.println("Automatically cycling every configured baud rate.");
  DebugConsole.println("2 seconds per baud; repeats forever until :stop.");
  DebugConsole.println("Any received bytes are printed immediately as [APB RX].");
  DebugConsole.println("==================================================");
}

void updateAutoListen()
{
  if (!autoListenMode) return;
  if ((int32_t)(millis() - autoListenDeadlineMs) < 0) return;

  captureUart();
  uint32_t received = totalBytes - autoListenRxStart;
  if (received) {
    DebugConsole.printf("[AUTO-LISTEN] *** %lu RX byte(s) detected at %lu baud ***\n",
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
  edgeLastUs = 0;
  edgeCaptureActive = true;
  attachInterrupt(digitalPinToInterrupt(ProjectConfig::APB_UART_RX_GPIO),
                  edgeISR, CHANGE);

  DebugConsole.println();
  DebugConsole.println("============= RAW GPIO18 SIGNAL ANALYSER =============");
  DebugConsole.println("UART decoder OFF. Measuring Pin 5 -> GPIO18 directly.");
  DebugConsole.println("Capture time: 10 seconds. Generate BT activity now.");
  DebugConsole.println("Nothing is transmitted to the APB module.");

  uint32_t start = millis();
  while (millis() - start < 10000 && edgeCount < kEdgeSampleCount) {
    delay(10);
  }

  edgeCaptureActive = false;
  detachInterrupt(digitalPinToInterrupt(ProjectConfig::APB_UART_RX_GPIO));

  size_t n = edgeCount;
  DebugConsole.println();
  DebugConsole.println("================ RAW SIGNAL RESULT ================");
  DebugConsole.printf("Edges/intervals captured: %u\n", (unsigned)n);

  if (n < 4) {
    DebugConsole.println("RESULT: NOT ENOUGH EDGE ACTIVITY TO ANALYSE.");
    DebugConsole.println("Pin 5 was mostly static during this capture.");
  } else {
    // Histogram intervals from 1..200 us. The smallest strongly recurring
    // interval is the best first estimate of one serial bit time.
    uint16_t histogram[201];
    memset(histogram, 0, sizeof(histogram));
    uint32_t minUs = 0xFFFFFFFFUL;
    uint32_t maxUs = 0;

    noInterrupts();
    for (size_t i=0; i<n; ++i) {
      uint32_t v = edgeIntervals[i];
      if (v < minUs) minUs = v;
      if (v > maxUs) maxUs = v;
      if (v >= 1 && v <= 200 && histogram[v] != 0xFFFF)
        ++histogram[v];
    }
    interrupts();

    uint32_t bestUs = 0;
    uint16_t bestHits = 0;
    for (uint32_t us=1; us<=200; ++us) {
      if (histogram[us] > bestHits) {
        bestHits = histogram[us];
        bestUs = us;
      }
    }

    DebugConsole.printf("Shortest interval: %lu us\n", (unsigned long)minUs);
    DebugConsole.printf("Longest interval:  %lu us\n", (unsigned long)maxUs);
    DebugConsole.printf("Most common 1-200us interval: %lu us (%u hits)\n",
                   (unsigned long)bestUs, (unsigned)bestHits);

    if (bestUs) {
      uint32_t estimated = 1000000UL / bestUs;
      DebugConsole.printf("Raw timing estimate: ~%lu baud if that interval is one bit\n",
                     (unsigned long)estimated);

      uint32_t nearest = kProbeBauds[0];
      uint32_t nearestError = (nearest > estimated) ? nearest-estimated : estimated-nearest;
      for (size_t i=1; i<kProbeBaudCount; ++i) {
        uint32_t b=kProbeBauds[i];
        uint32_t e=(b > estimated) ? b-estimated : estimated-b;
        if (e < nearestError) { nearest=b; nearestError=e; }
      }
      DebugConsole.printf("Nearest configured UART rate: %lu baud\n",
                     (unsigned long)nearest);
      DebugConsole.println("NOTE: This is a timing estimate, not proof the signal is UART.");
    }
  }
  DebugConsole.println("===================================================");

  configureUart(currentBaud);
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
    if (!baud) DebugConsole.println("[APB] invalid listen baud");
    else startListen(baud);
  } else if (!strcmp(line,":autolisten")) {
    startAutoListen();
  } else if (!strcmp(line,":analyze")) {
    analyzeRawSignal();
  } else if (!strcmp(line,":stop")) {
    listenMode=false;
    autoListenMode=false;
    DebugConsole.println("[APB] listen/autolisten stopped");
  } else if (!strcmp(line,":stats")) {
    DebugConsole.printf("[APB] baud=%lu RX=%lu bursts=%lu TX=%lu\n",
      (unsigned long)currentBaud,(unsigned long)totalBytes,
      (unsigned long)totalBursts,(unsigned long)totalTxBytes);
  } else if (!strcmp(line,":clear")) {
    APB8202Monitor::clearCounters();
    totalTxBytes=0;
    DebugConsole.println("[APB] counters cleared");
  } else if (!strcmp(line,":nextbaud")) {
    APB8202Monitor::nextBaud();
  } else if (!strncmp(line,":baud ",6)) {
    uint32_t baud=strtoul(line+6,nullptr,10);
    if (!APB8202Monitor::setBaud(baud)) DebugConsole.println("[APB] invalid baud");
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
    DebugConsole.println("[APB] Unknown command. Use :help");
  }
}

void readConsole()
{
  while (DebugConsole.available()) {
    char c=(char)DebugConsole.read();
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
      DebugConsole.println("[APB] command too long; discarded");
    }
  }
}

} // namespace

namespace APB8202Monitor {

bool begin()
{
  DebugConsole.println("=== APB8202 / CW6638M RAW UART DISCOVERY ===");
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
    DebugConsole.printf("[APB] already at %lu baud\n",(unsigned long)baud);
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
