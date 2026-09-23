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
  9600, 38400, 57600, 115200, 230400, 460800, 921600
};
static const size_t kProbeBaudCount =
    sizeof(kProbeBauds) / sizeof(kProbeBauds[0]);

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

void update() { captureUart(); readConsole(); }

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
