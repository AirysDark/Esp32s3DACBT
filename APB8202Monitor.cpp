#include "APB8202Monitor.h"
#include <Arduino.h>

namespace {

static const int PIN5_GPIO = 18;
static const int PIN6_GPIO = 17;
static const size_t MAX_EDGES = 8192;

struct Capture {
  volatile uint32_t t[MAX_EDGES];
  volatile size_t count;
  int initial;
};

static Capture p5 = {{0}, 0, LOW};
static Capture p6 = {{0}, 0, LOW};
static bool started = false;
static bool finished = false;
static uint32_t firstActivityMs = 0;
static uint32_t lastActivityMs = 0;
static size_t last5 = 0;
static size_t last6 = 0;

void IRAM_ATTR isr5() {
  size_t i = p5.count;
  if (i < MAX_EDGES) {
    p5.t[i] = (uint32_t)micros();
    p5.count = i + 1;
  }
}

void IRAM_ATTR isr6() {
  size_t i = p6.count;
  if (i < MAX_EDGES) {
    p6.t[i] = (uint32_t)micros();
    p6.count = i + 1;
  }
}

int levelAt(const Capture &c, uint32_t when, size_t n) {
  int level = c.initial;
  for (size_t i = 0; i < n && c.t[i] <= when; ++i) level = !level;
  return level;
}

struct Decode {
  uint32_t baud;
  bool inverted;
  size_t good;
  size_t bad;
  size_t count;
  uint8_t data[128];
};

Decode decode8N1(const Capture &c, uint32_t baud, bool inverted, size_t n) {
  Decode r = {baud, inverted, 0, 0, 0, {0}};
  const float bitUs = 1000000.0f / (float)baud;
  size_t i = 0;

  while (i < n && r.count < sizeof(r.data)) {
    int before = (i == 0) ? c.initial : ((c.initial + (int)i) & 1);
    int after = !before;
    if (inverted) { before = !before; after = !after; }

    if (before == HIGH && after == LOW) {
      const uint32_t start = c.t[i];
      uint8_t value = 0;

      for (int b = 0; b < 8; ++b) {
        int v = levelAt(c, start + (uint32_t)((1.5f + b) * bitUs), n);
        if (inverted) v = !v;
        if (v) value |= (1U << b);
      }

      int stop = levelAt(c, start + (uint32_t)(9.5f * bitUs), n);
      if (inverted) stop = !stop;

      if (stop == HIGH) {
        ++r.good;
        r.data[r.count++] = value;
        const uint32_t end = start + (uint32_t)(10.0f * bitUs);
        while (i < n && c.t[i] < end) ++i;
        continue;
      }
      ++r.bad;
    }
    ++i;
  }
  return r;
}

bool clockLike(const Decode &r) {
  if (r.count < 8) return false;
  size_t alternating = 0;
  for (size_t i = 0; i < r.count; ++i) {
    const uint8_t b = r.data[i];
    if (b == 0x55 || b == 0xAA || b == 0xF5 || b == 0xFD || b == 0x95)
      ++alternating;
  }
  return alternating * 100U / r.count >= 60U;
}

void reportPin(const char *name, int gpio, const Capture &c) {
  const size_t n = c.count;
  Serial0.println();
  Serial0.printf("----- %s / GPIO%d -----\n", name, gpio);
  Serial0.printf("Edges: %u  initial=%s\n", (unsigned)n, c.initial ? "HIGH" : "LOW");

  if (n < 3) {
    Serial0.println("Classification: STATIC / insufficient activity");
    return;
  }

  uint32_t hist[201] = {0};
  uint32_t minDt = 0xFFFFFFFFUL, maxDt = 0;
  for (size_t i = 1; i < n; ++i) {
    const uint32_t d = c.t[i] - c.t[i - 1];
    if (d < minDt) minDt = d;
    if (d > maxDt) maxDt = d;
    if (d >= 1 && d <= 200) ++hist[d];
  }

  uint32_t mode = 0, modeHits = 0;
  for (uint32_t u = 1; u <= 200; ++u) {
    if (hist[u] > modeHits) { modeHits = hist[u]; mode = u; }
  }

  Serial0.printf("Dominant interval: %lu us (%lu hits)\n",
                 (unsigned long)mode, (unsigned long)modeHits);
  Serial0.printf("Interval range: %lu .. %lu us\n",
                 (unsigned long)minDt, (unsigned long)maxDt);

  const uint32_t rates[] = {
    9600, 19200, 38400, 57600, 76800, 92160, 93750,
    96000, 100000, 115200, 128000, 230400, 460800, 921600
  };

  Decode best = {0, false, 0, 0, 0, {0}};
  unsigned bestScore = 0;

  for (size_t q = 0; q < sizeof(rates) / sizeof(rates[0]); ++q) {
    for (int inv = 0; inv < 2; ++inv) {
      Decode r = decode8N1(c, rates[q], inv != 0, n);
      const size_t total = r.good + r.bad;
      const unsigned score = total ? (unsigned)(100UL * r.good / total) : 0;
      if (r.count && (score > bestScore ||
          (score == bestScore && r.good > best.good))) {
        best = r;
        bestScore = score;
      }
    }
  }

  if (!best.count) {
    Serial0.println("UART: no usable 8N1 candidate");
    Serial0.println("Classification: NON-UART / UNKNOWN");
    return;
  }

  Serial0.printf("Best UART candidate: %lu 8N1 %s, framing=%u%%, bytes=%u\n",
                 (unsigned long)best.baud,
                 best.inverted ? "INVERTED" : "NORMAL",
                 bestScore, (unsigned)best.count);

  const bool looksClock = clockLike(best);
  Serial0.printf("Alternating-pattern test: %s\n",
                 looksClock ? "CLOCK-LIKE / FALSE-UART LIKELY" : "not dominant");

  Serial0.print("Bytes:");
  const size_t shown = best.count < 64 ? best.count : 64;
  for (size_t i = 0; i < shown; ++i) {
    if ((i % 16) == 0) Serial0.println();
    if (best.data[i] < 16) Serial0.print('0');
    Serial0.print(best.data[i], HEX);
    Serial0.print(' ');
  }
  Serial0.println();

  bool hci = false;
  for (size_t i = 0; i < best.count; ++i)
    if (best.data[i] == 0x04) { hci = true; break; }

  Serial0.printf("H4/HCI 0x04 marker: %s\n", hci ? "present (candidate only)" : "not seen");

  if (bestScore >= 90 && !looksClock)
    Serial0.println("Classification: STRONG UART CANDIDATE");
  else if (looksClock)
    Serial0.println("Classification: CLOCK/PERIODIC SIGNAL; do not trust UART bytes");
  else
    Serial0.println("Classification: POSSIBLE UART; needs confirmation");
}

void analyse() {
  detachInterrupt(digitalPinToInterrupt(PIN5_GPIO));
  detachInterrupt(digitalPinToInterrupt(PIN6_GPIO));

  Serial0.println();
  Serial0.println("========== APB TWO-PIN PASSIVE SIGNAL SCAN ==========");
  reportPin("APB physical pin 5", PIN5_GPIO, p5);
  reportPin("APB physical pin 6", PIN6_GPIO, p6);
  Serial0.println();
  Serial0.println("Both ESP32 pins remained INPUT-only.");
  Serial0.println("Serial1 was never started and nothing was transmitted.");
  Serial0.println("======================================================");
  finished = true;
}

} // namespace

namespace APB8202Monitor {

bool begin() {
  pinMode(PIN5_GPIO, INPUT);
  pinMode(PIN6_GPIO, INPUT);

  p5.count = 0;
  p6.count = 0;
  p5.initial = digitalRead(PIN5_GPIO);
  p6.initial = digitalRead(PIN6_GPIO);

  started = false;
  finished = false;
  last5 = last6 = 0;

  attachInterrupt(digitalPinToInterrupt(PIN5_GPIO), isr5, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN6_GPIO), isr6, CHANGE);

  Serial0.println("[SCAN] Two-pin passive analyser ARMED.");
  Serial0.printf("[SCAN] APB pin 5 -> GPIO18 initial=%s\n", p5.initial ? "HIGH" : "LOW");
  Serial0.printf("[SCAN] APB pin 6 -> GPIO17 initial=%s\n", p6.initial ? "HIGH" : "LOW");
  Serial0.println("[SCAN] GPIO17/18 INPUT-only. Serial1 NOT started.");
  Serial0.println("[SCAN] Turn APB power ON now.");
  Serial0.println("[SCAN] One power-up will analyse both candidate signal pins.");
  return true;
}

void update() {
  if (finished) return;

  const size_t n5 = p5.count;
  const size_t n6 = p6.count;

  if (n5 != last5 || n6 != last6) {
    last5 = n5;
    last6 = n6;
    lastActivityMs = millis();
    if (!started) {
      started = true;
      firstActivityMs = lastActivityMs;
      Serial0.println("[SCAN] Activity detected; capturing both pins...");
    }
  }

  if (n5 >= MAX_EDGES || n6 >= MAX_EDGES) {
    analyse();
    return;
  }

  if (started) {
    const uint32_t now = millis();
    if ((now - firstActivityMs) >= 6500 ||
        ((now - firstActivityMs) >= 5500 && (now - lastActivityMs) >= 1000))
      analyse();
  }
}

} // namespace APB8202Monitor
