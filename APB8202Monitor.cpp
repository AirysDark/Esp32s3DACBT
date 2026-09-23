#include "APB8202Monitor.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
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

void handleLine(char *line)
{
  if (!line || !*line) return;

  if (!strcmp(line,":help")) {
    printHelp();
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
