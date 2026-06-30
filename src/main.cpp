// trmnlreader — Seeed XIAO ESP32-S3 + 7.5" e-paper kit
//
// Server (TRMNLReaderServer/) renders PDF pages at fill-width. Bitmap width
// matches the device's active orientation (800 px landscape, 480 px portrait)
// and height is whatever the page works out to (padded to >= viewport).
// Body is a 1-bit packed bitmap, MSB-first, 1 = white.
//
// Buttons (active-low, INPUT_PULLUP):
//   BTN_SELECT  short = dashboard: open hovered book ;  reader: exit to dash
//               long  = toggle orientation (landscape <-> portrait)
//   BTN_PREV    short = dashboard: cursor left       ;  reader: scroll up
//               long  = dashboard: previous 8 books  ;  reader: previous page
//   BTN_NEXT    short = dashboard: cursor right      ;  reader: scroll down
//               long  = dashboard: next 8 books      ;  reader: next page

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

#define BTN_SELECT 2
#define BTN_PREV   3
#define BTN_NEXT   5

// ---------------------------------------------------------------------------
// Display constants
// ---------------------------------------------------------------------------
// Native panel resolution is 800 × 480. Long edge → landscape width / portrait
// height, short edge → landscape height / portrait width.
static constexpr int PANEL_LONG  = 800;
static constexpr int PANEL_SHORT = 480;

static constexpr int COVER_W   = 120;
static constexpr int COVER_H   = 160;
static constexpr size_t COVER_BYTES = (COVER_W * COVER_H) / 8;

static constexpr int MAX_BOOKS = 32;

// Scroll step a bit less than a full viewport so context carries over.
static constexpr int SCROLL_STEP_LANDSCAPE = 380;
static constexpr int SCROLL_STEP_PORTRAIT  = 680;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
EPaper epd;

enum class Orientation { Landscape, Portrait };
Orientation orientation = Orientation::Landscape;

static int currentScreenW() {
  return orientation == Orientation::Portrait ? PANEL_SHORT : PANEL_LONG;
}
static int currentScreenH() {
  return orientation == Orientation::Portrait ? PANEL_LONG : PANEL_SHORT;
}
static int currentRowBytes() { return currentScreenW() / 8; }
static int currentScrollStep() {
  return orientation == Orientation::Portrait
             ? SCROLL_STEP_PORTRAIT
             : SCROLL_STEP_LANDSCAPE;
}
static int gridCols() {
  return orientation == Orientation::Portrait ? 2 : 4;
}
static int gridRows() {
  return orientation == Orientation::Portrait ? 4 : 2;
}
static int gridPageSize() { return gridCols() * gridRows(); }

struct Book {
  String   id;
  String   title;
  String   author;
  int      pageCount    = 0;
  int      currentPage  = 0;
  uint32_t secondsRead  = 0;
  bool     available    = false;
  uint8_t* cover        = nullptr;
};
Book books[MAX_BOOKS];
int bookCount   = 0;
int selectedIdx = 0;

enum class Screen { Boot, Dashboard, Reader, Error };
Screen screen = Screen::Boot;

struct PageSlot {
  int      page     = -1;
  int      height   = 0;
  size_t   capacity = 0;
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

static void buttonsInit() {
  for (auto& b : btn) pinMode(b.pin, INPUT_PULLUP);
}

static BtnEvent pollButtons() {
  for (int i = 0; i < 3; i++) {
    auto& b = btn[i];
    bool pressed = digitalRead(b.pin) == LOW;
    if (pressed && !b.down) {
      b.down = true; b.downAt = millis(); b.longFired = false;
    } else if (pressed && b.down && !b.longFired) {
      if (millis() - b.downAt >= READER_LONG_PRESS_MS) {
        b.longFired = true;
        switch (i) {
          case 0: return BtnEvent::SelectLong;
          case 1: return BtnEvent::PrevLong;
          case 2: return BtnEvent::NextLong;
        }
      }
    } else if (!pressed && b.down) {
      b.down = false;
      if (!b.longFired) {
        switch (i) {
          case 0: return BtnEvent::SelectShort;
          case 1: return BtnEvent::PrevShort;
          case 2: return BtnEvent::NextShort;
        }
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

  int rb = currentRowBytes();
  int contentLen = http.getSize();
  int height = http.header("X-Image-Height").toInt();
  if (height <= 0 && contentLen > 0) height = contentLen / rb;
  if (height <= 0 || height > 5000) {
    Serial.printf("[http] bad height %d (clen=%d)\n", height, contentLen);
    http.end();
    return false;
  }
  if (outPageCount) {
    int pc = http.header("X-Page-Count").toInt();
    if (pc > 0) *outPageCount = pc;
  }

  size_t need = (size_t)height * rb;
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
                   "/page/" + page +
                   "?w=" + currentScreenW() +
                   "&h=" + currentScreenH();
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
    memset(b.cover, 0xFF, COVER_BYTES);
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

  if (selected) {
    epd.drawRect(x - 6, y - 6, w + 12, h + 12, TFT_BLACK);
    epd.drawRect(x - 5, y - 5, w + 10, h + 10, TFT_BLACK);
    epd.drawRect(x - 4, y - 4, w + 8,  h + 8,  TFT_BLACK);
    epd.drawRect(x - 3, y - 3, w + 6,  h + 6,  TFT_BLACK);
  }
  epd.drawRect(x, y, w, h, TFT_BLACK);

  int coverX = x + (w - COVER_W) / 2;
  int coverY = y + 8;
  if (b.cover) {
    epd.drawBitmap(coverX, coverY, b.cover, COVER_W, COVER_H,
                   TFT_WHITE, TFT_BLACK);
  } else {
    epd.drawRect(coverX, coverY, COVER_W, COVER_H, TFT_BLACK);
  }
  if (!b.available) {
    for (int i = 0; i < COVER_H; i += 8) {
      epd.drawLine(coverX, coverY + i, coverX + COVER_W, coverY + i, TFT_BLACK);
    }
  }

  int maxChars = max(8, w / 7);
  int textY = coverY + COVER_H + 4;
  epd.setTextColor(TFT_BLACK);
  epd.drawString(truncToWidth(b.title, maxChars), x + 6, textY, 2);
}

// Slim one-line hover bar at the bottom — keeps the grid the dominant element
// of the dashboard. Shows the selected book's title and (page progress, time
// read) compactly.
static void drawHoverBar(int x, int y, int w, int h) {
  if (bookCount == 0) return;
  Book& b = books[selectedIdx];
  epd.drawRect(x, y, w, h, TFT_BLACK);
  epd.setTextColor(TFT_BLACK);

  int titleChars = w / 12;
  epd.drawString(truncToWidth(b.title, titleChars), x + 10, y + 8, 4);

  String stats;
  if (b.pageCount > 0) {
    int pct = (int)((b.currentPage * 100L) / b.pageCount);
    stats = "p." + String(b.currentPage) + "/" + String(b.pageCount) +
            " (" + String(pct) + "%)  " + formatDuration(b.secondsRead);
  } else if (b.currentPage > 0) {
    stats = "p." + String(b.currentPage) + "  " +
            formatDuration(b.secondsRead);
  } else {
    stats = b.available ? "not started" : "unavailable";
  }
  // Right-justify-ish: anchor near the right edge.
  int statsChars = stats.length();
  int statsX = x + w - statsChars * 8 - 12;
  if (statsX < x + 10 + titleChars * 12 + 10) statsX = x + 10 + titleChars * 12 + 10;
  epd.drawString(stats, statsX, y + 14, 2);
}

static void drawDashboard() {
  epd.fillScreen(TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  int W = currentScreenW();
  int H = currentScreenH();

  // Slim top bar — title + page-of-pages indicator.
  epd.drawString("trmnl - reader", 16, 8, 4);
  int pageOfBooks = (selectedIdx / gridPageSize()) + 1;
  int totalPages  = bookCount > 0
                      ? ((bookCount - 1) / gridPageSize() + 1)
                      : 1;
  String posTop = String(selectedIdx + 1) + "/" + bookCount +
                  "  pg " + pageOfBooks + "/" + totalPages;
  epd.drawString(posTop, W - 200, 14, 2);
  epd.drawLine(16, 36, W - 16, 36, TFT_BLACK);

  if (bookCount == 0) {
    epd.drawString("No books yet - upload from the web dashboard.",
                   24, H / 2, 4);
    epd.update();
    return;
  }

  int cols = gridCols();
  int rows = gridRows();
  int gridPage = cols * rows;
  int gap     = 14;
  int padX    = 16;
  int hoverH  = 50;
  int gridTop = 44;
  int gridBottom = H - hoverH - 8;
  int cellW = (W - 2 * padX - (cols - 1) * gap) / cols;
  int cellH = (gridBottom - gridTop - (rows - 1) * gap) / rows;
  int originX = padX;
  int originY = gridTop;

  int pageStart = (selectedIdx / gridPage) * gridPage;
  int end = min(bookCount, pageStart + gridPage);
  for (int i = pageStart; i < end; i++) {
    int local = i - pageStart;
    int col = local % cols;
    int row = local / cols;
    int xC = originX + col * (cellW + gap);
    int yC = originY + row * (cellH + gap);
    drawCard(i, xC, yC, cellW, cellH, i == selectedIdx);
  }

  drawHoverBar(padX, H - hoverH - 4, W - 2 * padX, hoverH);
  epd.update();
}

// ---------------------------------------------------------------------------
// Rendering — reader
// ---------------------------------------------------------------------------
static void drawReaderViewport(int slot) {
  PageSlot& s = pageCache[slot];
  int W  = currentScreenW();
  int H  = currentScreenH();
  int rb = currentRowBytes();
  int maxScroll = max(0, s.height - H);
  if (readerScrollY < 0) readerScrollY = 0;
  if (readerScrollY > maxScroll) readerScrollY = maxScroll;

  uint8_t* offset = s.data + (size_t)readerScrollY * rb;
  epd.fillScreen(TFT_WHITE);
  epd.drawBitmap(0, 0, offset, W, H, TFT_WHITE, TFT_BLACK);

  if (s.height > H) {
    int trackH = H - 20;
    int barH = max(20, (H * trackH) / s.height);
    int barY = 10 + (maxScroll == 0
                         ? 0
                         : (readerScrollY * (trackH - barH)) / maxScroll);
    epd.fillRect(W - 4, barY, 3, barH, TFT_BLACK);
  }

  int pc = books[readerBookIdx].pageCount;
  String label = String(readerPage);
  if (pc > 0) label += " / " + String(pc);
  epd.fillRect(W - 90, H - 22, 80, 18, TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  epd.drawString(label, W - 88, H - 20, 2);

  epd.update();
}

static void drawReaderPage() {
  int slot = cacheFind(readerPage);
  if (slot < 0) {
    epd.fillScreen(TFT_WHITE);
    epd.setTextColor(TFT_BLACK);
    epd.drawString("Loading page " + String(readerPage) + "...",
                   40, currentScreenH() / 2, 4);
    epd.update();
    slot = cacheLoad(readerPage);
    if (slot < 0) {
      epd.fillScreen(TFT_WHITE);
      epd.drawString("Couldn't load page " + String(readerPage),
                     40, currentScreenH() / 2, 4);
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
  int maxScroll = max(0, pageCache[slot].height - currentScreenH());
  if (newY < 0) newY = 0;
  if (newY > maxScroll) newY = maxScroll;
  if (newY == readerScrollY) return;
  readerScrollY = newY;
  drawReaderViewport(slot);
}

// ---------------------------------------------------------------------------
// Orientation toggle
// ---------------------------------------------------------------------------
static void applyRotation() {
  // Rotation 1 = landscape (long edge horizontal, panel ribbon at top).
  // Rotation 2 = portrait flipped 180° from native — the device's natural
  // hand-held orientation with the buttons toward the user. The user found
  // rotation 0 to be the "wrong direction"; this flip puts text right-side
  // up when held that way.
  epd.setRotation(orientation == Orientation::Landscape ? 1 : 2);
}

static void toggleOrientation() {
  orientation = orientation == Orientation::Landscape
                    ? Orientation::Portrait
                    : Orientation::Landscape;
  applyRotation();
  // Cached pages are at the old width — drop them so the new orientation
  // refetches at the right size.
  cacheReset();
  readerScrollY = 0;
  if (screen == Screen::Reader) {
    drawReaderPage();
    prefetchAhead(readerPage + 1, READER_PREFETCH_PAGES);
  } else if (screen == Screen::Dashboard) {
    drawDashboard();
  }
}

// ---------------------------------------------------------------------------
// Boot screens
// ---------------------------------------------------------------------------
static void drawBootMessage(const String& msg) {
  epd.fillScreen(TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  int W = currentScreenW();
  int H = currentScreenH();
  epd.drawString("trmnl - reader", 16, 8, 4);
  epd.drawLine(16, 36, W - 16, 36, TFT_BLACK);
  epd.drawString(msg, 40, H / 2, 4);
  epd.update();
}

static void drawError(const String& msg) {
  screen = Screen::Error;
  epd.fillScreen(TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  int H = currentScreenH();
  epd.drawString("trmnl - reader", 16, 8, 4);
  epd.drawString("error:", 40, H / 2 - 30, 4);
  epd.drawString(msg, 40, H / 2 + 10, 4);
  epd.drawString("(press SELECT to retry)", 40, H / 2 + 50, 2);
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
// Jump cursor to the next/previous grid page (8 books per page in either
// orientation), keeping the cursor on the equivalent slot of the new page.
static void dashboardJumpPage(int dir) {
  if (bookCount == 0) return;
  int page = gridPageSize();
  int slotInPage = selectedIdx % page;
  int newIdx = selectedIdx + dir * page;
  if (newIdx < 0) newIdx = 0;
  if (newIdx >= bookCount) {
    int lastPageStart = (bookCount - 1) / page * page;
    newIdx = min(lastPageStart + slotInPage, bookCount - 1);
  }
  if (newIdx == selectedIdx) return;
  selectedIdx = newIdx;
  drawDashboard();
}

static void handleDashboard(BtnEvent ev) {
  if (ev == BtnEvent::SelectLong) { toggleOrientation(); return; }
  if (bookCount == 0) return;
  switch (ev) {
    case BtnEvent::PrevShort:
      selectedIdx = (selectedIdx - 1 + bookCount) % bookCount;
      drawDashboard();
      break;
    case BtnEvent::NextShort:
      selectedIdx = (selectedIdx + 1) % bookCount;
      drawDashboard();
      break;
    case BtnEvent::PrevLong: dashboardJumpPage(-1); break;
    case BtnEvent::NextLong: dashboardJumpPage(+1); break;
    case BtnEvent::SelectShort: enterReader(selectedIdx); break;
    default: break;
  }
}

static void handleReader(BtnEvent ev) {
  switch (ev) {
    case BtnEvent::NextShort:   readerScroll( currentScrollStep()); break;
    case BtnEvent::PrevShort:   readerScroll(-currentScrollStep()); break;
    case BtnEvent::NextLong:    readerNextPage();                   break;
    case BtnEvent::PrevLong:    readerPrevPage();                   break;
    case BtnEvent::SelectShort: exitReader();                       break;
    case BtnEvent::SelectLong:  toggleOrientation();                break;
    default:                                                        break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  buttonsInit();
  epd.begin();
  applyRotation();
  epd.setTextColor(TFT_BLACK);

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
