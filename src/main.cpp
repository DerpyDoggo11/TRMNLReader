// trmnlreader — Seeed XIAO ESP32-S3 + 7.5" e-paper kit
//
// Server (TRMNLReaderServer/) renders PDF pages at fill-width — width is
// always 800 px, height is whatever the page works out to (padded to >= 480).
// Body is a 1-bit packed bitmap, MSB-first, 1 = white. Device shows a 480-row
// viewport at a configurable scrollY, advancing through the page on
// short-press and through pages on long-press.
//
// Buttons (active-low, INPUT_PULLUP):
//   BTN_SELECT  short = open hovered book in dashboard (no-op in reader)
//               long  = close book and return to dashboard
//   BTN_PREV    short = dashboard: cursor left ;  reader: scroll up
//               long  = dashboard: noop        ;  reader: previous page
//   BTN_NEXT    short = dashboard: cursor right;  reader: scroll down
//               long  = dashboard: noop        ;  reader: next page
//
// Pin numbers below are best-guess; the firmware probes all four free GPIOs
// (D4–D7) on boot and prints which one fires when a button is pressed, so
// you can re-map BTN_SELECT/BTN_PREV/BTN_NEXT if needed.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "esp_heap_caps.h"

// ---------------------------------------------------------------------------
// Compile-time config (see platformio.ini)
// ---------------------------------------------------------------------------
#ifndef WIFI_SSID
#define WIFI_SSID "set-in-platformio-ini"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef SERVER_URL
#define SERVER_URL "https://trmnlreaderserver.onrender.com"
#endif
#ifndef DEVICE_TOKEN
#define DEVICE_TOKEN ""
#endif
#ifndef READER_PREFETCH_PAGES
#define READER_PREFETCH_PAGES 5
#endif
#ifndef READER_KEEPALIVE_MS
#define READER_KEEPALIVE_MS 600000UL
#endif
#ifndef READER_LONG_PRESS_MS
#define READER_LONG_PRESS_MS 600
#endif

#define BTN_SELECT D4
#define BTN_PREV   D5
#define BTN_NEXT   D6

// ---------------------------------------------------------------------------
// Display constants (must match server-side pdfRender.ts)
// ---------------------------------------------------------------------------
static constexpr int SCREEN_W   = 800;
static constexpr int SCREEN_H   = 480;
static constexpr int ROW_BYTES  = SCREEN_W / 8;        // 100

static constexpr int COVER_W   = 160;
static constexpr int COVER_H   = 224;
static constexpr size_t COVER_BYTES = (COVER_W * COVER_H) / 8;

static constexpr int MAX_BOOKS = 32;
static constexpr int GRID_COLS = 4;
static constexpr int GRID_ROWS = 2;
static constexpr int GRID_PAGE = GRID_COLS * GRID_ROWS;

// scroll step inside a page (a bit less than a full screenful so context
// carries over between presses).
static constexpr int SCROLL_STEP = 380;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
EPaper epd;

struct Book {
  String   id;
  String   title;
  String   author;
  int      pageCount    = 0;
  int      currentPage  = 0;
  uint32_t secondsRead  = 0;
  bool     available    = false;     // we have a file OR can fetch one
  uint8_t* cover        = nullptr;   // COVER_BYTES, PSRAM
};
Book books[MAX_BOOKS];
int bookCount   = 0;
int selectedIdx = 0;

enum class Screen { Boot, Dashboard, Reader, Error };
Screen screen = Screen::Boot;

// Variable-height page bitmap cache. Each slot has its own size.
struct PageSlot {
  int      page     = -1;
  int      height   = 0;            // rows (each row = ROW_BYTES)
  size_t   capacity = 0;            // allocated bytes
  uint8_t* data     = nullptr;
};
static constexpr int CACHE_SLOTS = READER_PREFETCH_PAGES + 2;
PageSlot pageCache[CACHE_SLOTS];

int  readerBookIdx = -1;
int  readerPage    = 1;
int  readerScrollY = 0;
unsigned long lastPingMs    = 0;
unsigned long pageEnteredAt = 0;
int  pagesReadAccum         = 0;

// ---------------------------------------------------------------------------
// Buttons — debounced click vs. long-press
// ---------------------------------------------------------------------------
enum class BtnEvent {
  None,
  SelectShort, SelectLong,
  PrevShort,   PrevLong,
  NextShort,   NextLong,
};

struct BtnState {
  uint8_t        pin;
  bool           down;
  unsigned long  downAt;
  bool           longFired;
};
BtnState btn[3] = {
  {BTN_SELECT, false, 0, false},
  {BTN_PREV,   false, 0, false},
  {BTN_NEXT,   false, 0, false},
};

// Extra probe — these are the *other* candidate pins (D4–D7) that aren't
// already in btn[]. On press they print to serial so you can identify the
// real button-to-pin mapping for your kit and update BTN_* above.
static const uint8_t PROBE_PINS[] = {D4, D5, D6, D7};
static bool probeLastState[4]      = {true, true, true, true};

static void buttonsInit() {
  for (auto& b : btn) pinMode(b.pin, INPUT_PULLUP);
  for (auto p : PROBE_PINS) pinMode(p, INPUT_PULLUP);
}

static void pollProbe() {
  for (int i = 0; i < (int)(sizeof(PROBE_PINS) / sizeof(PROBE_PINS[0])); i++) {
    bool now = digitalRead(PROBE_PINS[i]);
    if (now != probeLastState[i]) {
      probeLastState[i] = now;
      if (!now) Serial.printf("[probe] D%d (GPIO%d) pressed\n",
                              i + 4, PROBE_PINS[i]);
    }
  }
}

static BtnEvent pollButtons() {
  pollProbe();
  for (int i = 0; i < 3; i++) {
    auto& b = btn[i];
    bool pressed = digitalRead(b.pin) == LOW;
    if (pressed && !b.down) {
      b.down = true; b.downAt = millis(); b.longFired = false;
    } else if (pressed && b.down && !b.longFired) {
      if (millis() - b.downAt >= READER_LONG_PRESS_MS) {
        b.longFired = true;
        if (i == 0) return BtnEvent::SelectLong;
        if (i == 1) return BtnEvent::PrevLong;
        return BtnEvent::NextLong;
      }
    } else if (!pressed && b.down) {
      b.down = false;
      if (!b.longFired) {
        if (i == 0) return BtnEvent::SelectShort;
        if (i == 1) return BtnEvent::PrevShort;
        return BtnEvent::NextShort;
      }
    }
  }
  return BtnEvent::None;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
static WiFiClientSecure tlsClient;
static WiFiClient       plainClient;

static WiFiClient& clientFor(const String& url) {
  if (url.startsWith("https://")) {
    tlsClient.setInsecure();
    return tlsClient;
  }
  return plainClient;
}

// Stream the body straight into `out` (caller-allocated, exact size).
static bool httpFetchBytes(const String& path, uint8_t* out, size_t expected) {
  String url = String(SERVER_URL) + path;
  HTTPClient http;
  http.setTimeout(60000);
  http.setConnectTimeout(20000);
  if (!http.begin(clientFor(url), url)) return false;
  http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[http] GET %s -> %d\n", path.c_str(), code);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  unsigned long lastByteAt = millis();
  while (got < expected) {
    size_t avail = stream->available();
    if (avail) {
      int n = stream->readBytes(out + got, min(avail, expected - got));
      if (n > 0) { got += n; lastByteAt = millis(); }
    } else if (!http.connected() && stream->available() == 0) {
      break;
    } else if (millis() - lastByteAt > 15000) {
      Serial.println("[http] stalled");
      break;
    } else {
      delay(2);
    }
  }
  http.end();
  if (got != expected) {
    Serial.printf("[http] short read %u/%u\n",
                  (unsigned)got, (unsigned)expected);
    return false;
  }
  return true;
}

// Variant that allocates based on Content-Length and reads the variable-size
// page bitmap. Sets *outHeight from the X-Image-Height header (or computes
// it from Content-Length / ROW_BYTES if the header is missing).
static const char* PAGE_HEADERS[] = {"X-Image-Height", "X-Page-Count"};
static bool httpFetchPage(const String& path, PageSlot* slot, int* outPageCount) {
  String url = String(SERVER_URL) + path;
  HTTPClient http;
  http.setTimeout(60000);
  http.setConnectTimeout(20000);
  if (!http.begin(clientFor(url), url)) return false;
  http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
  http.collectHeaders(PAGE_HEADERS, 2);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[http] GET %s -> %d\n", path.c_str(), code);
    http.end();
    return false;
  }

  int contentLen = http.getSize();
  int height = http.header("X-Image-Height").toInt();
  if (height <= 0 && contentLen > 0) height = contentLen / ROW_BYTES;
  if (height <= 0 || height > 5000) {
    Serial.printf("[http] bad height %d (clen=%d)\n", height, contentLen);
    http.end();
    return false;
  }
  if (outPageCount) {
    int pc = http.header("X-Page-Count").toInt();
    if (pc > 0) *outPageCount = pc;
  }

  size_t need = (size_t)height * ROW_BYTES;
  if (slot->capacity < need) {
    if (slot->data) free(slot->data);
    slot->data = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!slot->data) slot->data = (uint8_t*)malloc(need);
    if (!slot->data) { slot->capacity = 0; http.end(); return false; }
    slot->capacity = need;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  unsigned long lastByteAt = millis();
  while (got < need) {
    size_t avail = stream->available();
    if (avail) {
      int n = stream->readBytes(slot->data + got, min(avail, need - got));
      if (n > 0) { got += n; lastByteAt = millis(); }
    } else if (!http.connected() && stream->available() == 0) {
      break;
    } else if (millis() - lastByteAt > 15000) {
      break;
    } else {
      delay(2);
    }
  }
  http.end();
  if (got != need) {
    Serial.printf("[http] short page read %u/%u\n",
                  (unsigned)got, (unsigned)need);
    return false;
  }
  slot->height = height;
  return true;
}

static String httpGetString(const String& path) {
  String url = String(SERVER_URL) + path;
  HTTPClient http;
  http.setTimeout(60000);
  http.setConnectTimeout(20000);
  if (!http.begin(clientFor(url), url)) return String();
  http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
  int code = http.GET();
  String body;
  if (code == HTTP_CODE_OK) body = http.getString();
  else Serial.printf("[http] GET %s -> %d\n", path.c_str(), code);
  http.end();
  return body;
}

static bool httpPostJson(const String& path, const String& body) {
  String url = String(SERVER_URL) + path;
  HTTPClient http;
  http.setTimeout(30000);
  http.setConnectTimeout(20000);
  if (!http.begin(clientFor(url), url)) return false;
  http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();
  return code >= 200 && code < 300;
}

// ---------------------------------------------------------------------------
// PSRAM helpers
// ---------------------------------------------------------------------------
static uint8_t* psAlloc(size_t n) {
  uint8_t* p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!p) p = (uint8_t*)malloc(n);
  return p;
}

// ---------------------------------------------------------------------------
// Page cache
// ---------------------------------------------------------------------------
static void cacheReset() {
  for (auto& s : pageCache) {
    if (s.data) { free(s.data); s.data = nullptr; }
    s.page = -1; s.height = 0; s.capacity = 0;
  }
}

static int cacheFind(int page) {
  for (int i = 0; i < CACHE_SLOTS; i++) {
    if (pageCache[i].page == page) return i;
  }
  return -1;
}

// Evict slot whose page is furthest from `keepNear`. Empty slots win.
static int cacheEvictSlot(int keepNear) {
  int best = 0, bestDist = -1;
  for (int i = 0; i < CACHE_SLOTS; i++) {
    if (pageCache[i].page == -1) return i;
    int d = abs(pageCache[i].page - keepNear);
    if (d > bestDist) { bestDist = d; best = i; }
  }
  return best;
}

static int cacheLoad(int page) {
  int found = cacheFind(page);
  if (found >= 0) return found;
  int slot = cacheEvictSlot(page);
  pageCache[slot].page = -1;
  String pathStr = String("/api/device/books/") + books[readerBookIdx].id +
                   "/page/" + page;
  int pageCount = 0;
  if (!httpFetchPage(pathStr, &pageCache[slot], &pageCount)) return -1;
  pageCache[slot].page = page;
  if (pageCount > 0 && books[readerBookIdx].pageCount == 0) {
    books[readerBookIdx].pageCount = pageCount;
  }
  return slot;
}

static void prefetchAhead(int from, int count) {
  int maxPage = books[readerBookIdx].pageCount > 0
                    ? books[readerBookIdx].pageCount
                    : (from + count);
  for (int p = from; p < from + count && p <= maxPage; p++) {
    if (cacheFind(p) >= 0) continue;
    if (cacheLoad(p) < 0) return;
  }
}

// ---------------------------------------------------------------------------
// API: books + cover
// ---------------------------------------------------------------------------
static bool loadBooks() {
  String body = httpGetString("/api/device/books");
  if (body.length() == 0) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) { Serial.printf("[json] %s\n", err.c_str()); return false; }

  JsonArray arr = doc["books"].as<JsonArray>();
  bookCount = 0;
  for (JsonObject obj : arr) {
    if (bookCount >= MAX_BOOKS) break;
    Book& b = books[bookCount++];
    b.id          = String((const char*)obj["id"]);
    b.title       = String((const char*)(obj["title"]  | ""));
    b.author      = String((const char*)(obj["author"] | ""));
    b.pageCount   = obj["pageCount"]   | 0;
    b.currentPage = obj["currentPage"] | 0;
    b.secondsRead = obj["secondsRead"] | 0;
    b.available   = obj["available"]   | false;
  }
  if (selectedIdx >= bookCount) selectedIdx = 0;
  return true;
}

static void loadCover(Book& b) {
  if (b.cover) return;
  b.cover = psAlloc(COVER_BYTES);
  if (!b.cover) return;
  String path = "/api/device/books/" + b.id + "/cover";
  if (!httpFetchBytes(path, b.cover, COVER_BYTES)) {
    memset(b.cover, 0xFF, COVER_BYTES);  // blank placeholder
  }
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------
static String truncToWidth(const String& s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  return s.substring(0, maxChars - 1) + "...";
}

static String formatDuration(uint32_t secs) {
  if (secs == 0) return String("0m");
  uint32_t h = secs / 3600;
  uint32_t m = (secs % 3600) / 60;
  String out;
  if (h > 0) { out += String(h) + "h "; }
  out += String(m) + "m";
  return out;
}

// ---------------------------------------------------------------------------
// Rendering — dashboard
// ---------------------------------------------------------------------------
static void drawCard(int idx, int x, int y, int w, int h, bool selected) {
  Book& b = books[idx];

  // Outline: a clean 2 px border. For the selected card, draw two nested
  // rects so the hover indicator is unmistakable on e-paper.
  if (selected) {
    epd.drawRect(x - 6, y - 6, w + 12, h + 12, TFT_BLACK);
    epd.drawRect(x - 5, y - 5, w + 10, h + 10, TFT_BLACK);
    epd.drawRect(x - 4, y - 4, w + 8,  h + 8,  TFT_BLACK);
    epd.drawRect(x - 3, y - 3, w + 6,  h + 6,  TFT_BLACK);
  }
  epd.drawRect(x, y, w, h, TFT_BLACK);

  int coverX = x + (w - COVER_W) / 2;
  int coverY = y + 10;
  if (b.cover) {
    epd.drawBitmap(coverX, coverY, b.cover, COVER_W, COVER_H,
                   TFT_WHITE, TFT_BLACK);
  } else {
    epd.drawRect(coverX, coverY, COVER_W, COVER_H, TFT_BLACK);
  }
  if (!b.available) {
    // diagonal stripes hint that the book can't be opened
    for (int i = 0; i < COVER_H; i += 8) {
      epd.drawLine(coverX, coverY + i, coverX + COVER_W, coverY + i, TFT_BLACK);
    }
  }

  int maxChars = w / 7;
  int textY = coverY + COVER_H + 8;
  epd.setTextColor(TFT_BLACK);
  epd.drawString(truncToWidth(b.title, maxChars), x + 8, textY, 2);
  if (b.author.length()) {
    epd.drawString(truncToWidth(b.author, maxChars), x + 8, textY + 18, 2);
  }
}

static void drawHoverPanel(int x, int y, int w, int h) {
  if (bookCount == 0) return;
  Book& b = books[selectedIdx];
  epd.drawRect(x, y, w, h, TFT_BLACK);
  epd.setTextColor(TFT_BLACK);

  // Title row (truncated to fit)
  int maxChars = w / 8;
  epd.drawString(truncToWidth(b.title, maxChars), x + 12, y + 10, 4);

  // Stats row
  String stats;
  if (b.pageCount > 0) {
    int pct = (int)((b.currentPage * 100L) / b.pageCount);
    stats = "page " + String(b.currentPage) + " / " + String(b.pageCount) +
            "  (" + String(pct) + "%)";
  } else if (b.currentPage > 0) {
    stats = "page " + String(b.currentPage);
  } else {
    stats = b.available ? "not started" : "unavailable on device";
  }
  epd.drawString(stats, x + 12, y + 44, 2);

  String time = "read for " + formatDuration(b.secondsRead);
  epd.drawString(time, x + 12, y + 64, 2);
}

static void drawDashboard() {
  epd.fillScreen(TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  epd.drawString("trmnl - reader", 24, 16, 4);
  epd.drawLine(24, 48, SCREEN_W - 24, 48, TFT_BLACK);

  if (bookCount == 0) {
    epd.drawString("No books yet — upload from the web dashboard.",
                   24, 200, 4);
    epd.update();
    return;
  }

  // Grid takes the top portion; bottom 92 px is the hover info panel.
  int gridTop    = 60;
  int gridBottom = SCREEN_H - 92;
  int cellW      = (SCREEN_W - 24 * 2 - (GRID_COLS - 1) * 16) / GRID_COLS;
  int cellH      = (gridBottom - gridTop - (GRID_ROWS - 1) * 16) / GRID_ROWS;
  int originX    = 24;
  int originY    = gridTop;

  int pageStart = (selectedIdx / GRID_PAGE) * GRID_PAGE;
  int end = min(bookCount, pageStart + GRID_PAGE);
  for (int i = pageStart; i < end; i++) {
    int local = i - pageStart;
    int col = local % GRID_COLS;
    int row = local / GRID_COLS;
    int x = originX + col * (cellW + 16);
    int y = originY + row * (cellH + 16);
    drawCard(i, x, y, cellW, cellH, i == selectedIdx);
  }

  drawHoverPanel(24, SCREEN_H - 84, SCREEN_W - 48, 76);

  String pos = String(selectedIdx + 1) + " / " + bookCount;
  epd.drawString(pos, SCREEN_W - 90, 20, 2);

  epd.update();
}

// ---------------------------------------------------------------------------
// Rendering — reader
// ---------------------------------------------------------------------------
static void drawReaderViewport(int slot) {
  PageSlot& s = pageCache[slot];
  int maxScroll = max(0, s.height - SCREEN_H);
  if (readerScrollY < 0) readerScrollY = 0;
  if (readerScrollY > maxScroll) readerScrollY = maxScroll;

  // Bitmap is row-major, 1 bit per pixel, MSB-first; each row is exactly
  // ROW_BYTES (= SCREEN_W/8 = 100) bytes. Skip `readerScrollY` rows by
  // offsetting the pointer.
  uint8_t* offset = s.data + (size_t)readerScrollY * ROW_BYTES;
  epd.fillScreen(TFT_WHITE);
  epd.drawBitmap(0, 0, offset, SCREEN_W, SCREEN_H, TFT_WHITE, TFT_BLACK);

  // Scroll-position indicator (a thin bar on the right) only if the page is
  // taller than the viewport.
  if (s.height > SCREEN_H) {
    int trackH = SCREEN_H - 20;
    int barH = max(20, (SCREEN_H * trackH) / s.height);
    int barY = 10 + (maxScroll == 0 ? 0 : (readerScrollY * (trackH - barH)) / maxScroll);
    epd.fillRect(SCREEN_W - 4, barY, 3, barH, TFT_BLACK);
  }

  // Tiny page-N-of-M label, bottom-right.
  int pc = books[readerBookIdx].pageCount;
  String label = String(readerPage);
  if (pc > 0) label += " / " + String(pc);
  epd.fillRect(SCREEN_W - 90, SCREEN_H - 22, 80, 18, TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  epd.drawString(label, SCREEN_W - 88, SCREEN_H - 20, 2);

  epd.update();
}

static void drawReaderPage() {
  int slot = cacheFind(readerPage);
  if (slot < 0) {
    epd.fillScreen(TFT_WHITE);
    epd.setTextColor(TFT_BLACK);
    epd.drawString("Loading page " + String(readerPage) + "...",
                   40, 220, 4);
    epd.update();
    slot = cacheLoad(readerPage);
    if (slot < 0) {
      epd.fillScreen(TFT_WHITE);
      epd.drawString("Couldn't load page " + String(readerPage),
                     40, 220, 4);
      epd.update();
      return;
    }
  }
  drawReaderViewport(slot);
}

static void postProgress() {
  uint32_t secsOnPage = pageEnteredAt == 0
                            ? 0
                            : (millis() - pageEnteredAt) / 1000;
  if (secsOnPage > 3600) secsOnPage = 3600;
  String body = String("{\"page\":") + readerPage +
                ",\"seconds\":" + secsOnPage +
                ",\"pagesRead\":" + pagesReadAccum + "}";
  pagesReadAccum = 0;
  pageEnteredAt = millis();
  String path = "/api/device/books/" + books[readerBookIdx].id + "/progress";
  httpPostJson(path, body);
}

static void enterReader(int bookIdx) {
  if (!books[bookIdx].available) return;
  readerBookIdx = bookIdx;
  cacheReset();
  Book& b = books[bookIdx];
  readerPage = b.currentPage > 0 ? b.currentPage : 1;
  readerScrollY = 0;
  pageEnteredAt = millis();
  pagesReadAccum = 0;
  screen = Screen::Reader;
  drawReaderPage();
  postProgress();
  prefetchAhead(readerPage + 1, READER_PREFETCH_PAGES);
}

static void exitReader() {
  if (pageEnteredAt != 0) postProgress();
  cacheReset();
  readerBookIdx = -1;
  screen = Screen::Dashboard;
  drawDashboard();
}

static void readerNextPage() {
  Book& b = books[readerBookIdx];
  if (b.pageCount > 0 && readerPage >= b.pageCount) return;
  postProgress();
  pagesReadAccum++;
  readerPage++;
  readerScrollY = 0;
  b.currentPage = readerPage;
  drawReaderPage();
  prefetchAhead(readerPage + 1, READER_PREFETCH_PAGES);
}

static void readerPrevPage() {
  if (readerPage <= 1) return;
  postProgress();
  readerPage--;
  readerScrollY = 0;
  books[readerBookIdx].currentPage = readerPage;
  drawReaderPage();
}

static void readerScroll(int delta) {
  int slot = cacheFind(readerPage);
  if (slot < 0) return;
  int newY = readerScrollY + delta;
  int maxScroll = max(0, pageCache[slot].height - SCREEN_H);
  if (newY < 0) newY = 0;
  if (newY > maxScroll) newY = maxScroll;
  if (newY == readerScrollY) return;
  readerScrollY = newY;
  drawReaderViewport(slot);
}

// ---------------------------------------------------------------------------
// Boot screens
// ---------------------------------------------------------------------------
static void drawBootMessage(const String& msg) {
  epd.fillScreen(TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  epd.drawString("trmnl - reader", 24, 16, 4);
  epd.drawLine(24, 48, SCREEN_W - 24, 48, TFT_BLACK);
  epd.drawString(msg, 40, 220, 4);
  epd.update();
}

static void drawError(const String& msg) {
  screen = Screen::Error;
  epd.fillScreen(TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  epd.drawString("trmnl - reader", 24, 16, 4);
  epd.drawString("error:", 40, 180, 4);
  epd.drawString(msg, 40, 220, 4);
  epd.drawString("(press SELECT to retry)", 40, 260, 2);
  epd.update();
}

static bool connectWiFi() {
  Serial.printf("[wifi] connecting to SSID '%s'\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 30000) {
      Serial.printf("[wifi] failed, status=%d\n", WiFi.status());
      return false;
    }
    delay(200);
  }
  Serial.printf("[wifi] ok, ip=%s rssi=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

static bool bootSequence() {
  drawBootMessage("Connecting to WiFi...");
  if (!connectWiFi()) { drawError("WiFi failed"); return false; }

  drawBootMessage("Fetching books...");
  if (!loadBooks())   { drawError("Couldn't load books"); return false; }

  drawBootMessage(String("Loading ") + bookCount + " covers...");
  for (int i = 0; i < bookCount; i++) {
    if (books[i].available) loadCover(books[i]);
  }

  screen = Screen::Dashboard;
  drawDashboard();
  lastPingMs = millis();
  return true;
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------
static void handleDashboard(BtnEvent ev) {
  if (bookCount == 0) return;
  if (ev == BtnEvent::PrevShort) {
    selectedIdx = (selectedIdx - 1 + bookCount) % bookCount;
    drawDashboard();
  } else if (ev == BtnEvent::NextShort) {
    selectedIdx = (selectedIdx + 1) % bookCount;
    drawDashboard();
  } else if (ev == BtnEvent::SelectShort) {
    enterReader(selectedIdx);
  }
  // long-press in dashboard: ignore
}

static void handleReader(BtnEvent ev) {
  switch (ev) {
    case BtnEvent::NextShort:   readerScroll( SCROLL_STEP); break;
    case BtnEvent::PrevShort:   readerScroll(-SCROLL_STEP); break;
    case BtnEvent::NextLong:    readerNextPage();           break;
    case BtnEvent::PrevLong:    readerPrevPage();           break;
    case BtnEvent::SelectLong:  exitReader();               break;
    default:                                                break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  buttonsInit();
  epd.begin();
  epd.setRotation(0);
  epd.setTextColor(TFT_BLACK);

  Serial.println("[probe] press each physical button — D4-D7 presses logged");
  bootSequence();
}

void loop() {
  BtnEvent ev = pollButtons();

  if (screen == Screen::Dashboard)        handleDashboard(ev);
  else if (screen == Screen::Reader)      handleReader(ev);
  else if (screen == Screen::Error &&
           ev == BtnEvent::SelectShort)   bootSequence();

  if (millis() - lastPingMs > READER_KEEPALIVE_MS) {
    lastPingMs = millis();
    httpGetString("/api/device/ping");
  }

  delay(10);
}
