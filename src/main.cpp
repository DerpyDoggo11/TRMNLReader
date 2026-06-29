// trmnlreader — Seeed XIAO ESP32-S3 + 7.5" e-paper kit
//
// Talks to the Next.js server in TRMNLReaderServer/. The server renders PDF
// pages into 1-bit 800x480 bitmaps (one bit per pixel, MSB-first, 1 = white)
// which we push straight to the panel via drawBitmap.
//
// Buttons (active-low, INPUT_PULLUP):
//   BTN_SELECT  short = open hovered book / no-op in reader
//               long  = close book and return to dashboard
//   BTN_PREV    dashboard = move highlight left
//               reader    = previous page
//   BTN_NEXT    dashboard = move highlight right
//               reader    = next page (prefetches up to N more pages)
//
// Pin numbers below are best-guess for the Seeed XIAO 7.5" e-paper driver
// board; the e-paper itself takes D0–D3 and D8–D10, leaving D4–D7 free.
// Adjust if your board routes the buttons elsewhere.

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
#define READER_KEEPALIVE_MS 600000UL  // 10 min — Render free spins down at 15
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
static constexpr size_t PAGE_BYTES = (SCREEN_W * SCREEN_H) / 8;

static constexpr int COVER_W   = 160;
static constexpr int COVER_H   = 224;
static constexpr size_t COVER_BYTES = (COVER_W * COVER_H) / 8;

static constexpr int MAX_BOOKS = 32;
static constexpr int GRID_COLS = 4;
static constexpr int GRID_ROWS = 2;
static constexpr int GRID_PAGE = GRID_COLS * GRID_ROWS;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
EPaper epd;

struct Book {
  String   id;
  String   title;
  String   author;
  int      pageCount   = 0;
  int      currentPage = 0;
  uint8_t* cover       = nullptr;  // PSRAM, COVER_BYTES (or null on alloc fail)
};
Book books[MAX_BOOKS];
int bookCount   = 0;
int selectedIdx = 0;

enum class Screen { Boot, Dashboard, Reader, Error };
Screen screen = Screen::Boot;

struct PageSlot {
  int      page = -1;
  uint8_t* data = nullptr;
};
static constexpr int CACHE_SLOTS = READER_PREFETCH_PAGES + 2;
PageSlot pageCache[CACHE_SLOTS];

int           readerBookIdx = -1;
int           readerPage    = 1;
unsigned long lastPingMs    = 0;

// ---------------------------------------------------------------------------
// Buttons — debounced click vs. long-press
// ---------------------------------------------------------------------------
enum class BtnEvent { None, Select, SelectLong, Prev, Next };

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
      if (i == 0 && millis() - b.downAt >= READER_LONG_PRESS_MS) {
        b.longFired = true;
        return BtnEvent::SelectLong;
      }
    } else if (!pressed && b.down) {
      b.down = false;
      if (!b.longFired) {
        if (i == 0) return BtnEvent::Select;
        if (i == 1) return BtnEvent::Prev;
        if (i == 2) return BtnEvent::Next;
      }
    }
  }
  return BtnEvent::None;
}

// ---------------------------------------------------------------------------
// HTTP helpers
// ---------------------------------------------------------------------------
static WiFiClientSecure tlsClient;
static WiFiClient       plainClient;

static WiFiClient& clientFor(const String& url) {
  if (url.startsWith("https://")) {
    tlsClient.setInsecure();  // no CA bundle on device; we trust DNS + token
    return tlsClient;
  }
  return plainClient;
}

// Fetch the raw body straight into `out` (caller-allocated, exact size).
static bool httpFetchBytes(const String& path, uint8_t* out, size_t expected) {
  String url = String(SERVER_URL) + path;
  HTTPClient http;
  // Render's free tier spins down after 15 min and can take ~30s to wake; the
  // first fetch after a long idle is the slow one.
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
    Serial.printf("[http] short read %u/%u\n", (unsigned)got, (unsigned)expected);
    return false;
  }
  return true;
}

static String httpGetString(const String& path) {
  String url = String(SERVER_URL) + path;
  HTTPClient http;
  // Render's free tier spins down after 15 min and can take ~30s to wake; the
  // first fetch after a long idle is the slow one.
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
    s.page = -1;
  }
}

static int cacheFind(int page) {
  for (int i = 0; i < CACHE_SLOTS; i++) {
    if (pageCache[i].page == page) return i;
  }
  return -1;
}

// Evict the slot whose page is furthest from `keepNear`. Empty slots win.
static int cacheEvictSlot(int keepNear) {
  int best = 0, bestDist = -1;
  for (int i = 0; i < CACHE_SLOTS; i++) {
    if (pageCache[i].page == -1) return i;
    int d = abs(pageCache[i].page - keepNear);
    if (d > bestDist) { bestDist = d; best = i; }
  }
  return best;
}

static bool fetchPageInto(uint8_t* buf, int page) {
  String path = String("/api/device/books/") + books[readerBookIdx].id +
                "/page/" + page;
  return httpFetchBytes(path, buf, PAGE_BYTES);
}

static int cacheLoad(int page) {
  int found = cacheFind(page);
  if (found >= 0) return found;

  int slot = cacheEvictSlot(page);
  if (!pageCache[slot].data) {
    pageCache[slot].data = psAlloc(PAGE_BYTES);
    if (!pageCache[slot].data) return -1;
  }
  pageCache[slot].page = -1;
  if (!fetchPageInto(pageCache[slot].data, page)) return -1;
  pageCache[slot].page = page;
  return slot;
}

// Best-effort fill of [from..from+count-1].
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
// API: book list + cover
// ---------------------------------------------------------------------------
static bool loadBooks() {
  String body = httpGetString("/api/device/books");
  if (body.length() == 0) return false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[json] %s\n", err.c_str());
    return false;
  }
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
    // Leave allocated as a white block so drawDashboard renders *something*.
    memset(b.cover, 0xFF, COVER_BYTES);
  }
}

// ---------------------------------------------------------------------------
// Rendering — dashboard
// ---------------------------------------------------------------------------
static void drawCard(int idx, int x, int y, int w, int h, bool selected) {
  Book& b = books[idx];

  if (selected) {
    epd.fillRect(x - 4, y - 4, w + 8, h + 8, TFT_BLACK);
    epd.fillRect(x - 2, y - 2, w + 4, h + 4, TFT_WHITE);
  } else {
    epd.drawRect(x, y, w, h, TFT_BLACK);
  }

  int coverX = x + (w - COVER_W) / 2;
  int coverY = y + 12;
  if (b.cover) {
    epd.drawBitmap(coverX, coverY, b.cover, COVER_W, COVER_H,
                   TFT_WHITE, TFT_BLACK);
  } else {
    epd.drawRect(coverX, coverY, COVER_W, COVER_H, TFT_BLACK);
  }

  int textY = coverY + COVER_H + 8;
  epd.setTextColor(TFT_BLACK);

  // Font 2 ≈ 7px/char at default scale; trim to fit.
  int maxChars = w / 7;
  String t = b.title;
  if ((int)t.length() > maxChars) t = t.substring(0, maxChars - 1) + "…";
  epd.drawString(t, x + 10, textY, 2);

  if (b.author.length()) {
    String a = b.author;
    if ((int)a.length() > maxChars) a = a.substring(0, maxChars - 1) + "…";
    epd.drawString(a, x + 10, textY + 18, 2);
  }
}

static void drawDashboard() {
  epd.fillScreen(TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  epd.drawString("trmnl·reader", 24, 18, 4);
  epd.drawLine(24, 50, SCREEN_W - 24, 50, TFT_BLACK);

  if (bookCount == 0) {
    epd.drawString("No books yet — upload one from the web dashboard.",
                   24, 200, 4);
    epd.update();
    return;
  }

  int cellW = (SCREEN_W - 24 * 2 - (GRID_COLS - 1) * 16) / GRID_COLS;
  int cellH = (SCREEN_H - 70 - 24 - (GRID_ROWS - 1) * 16) / GRID_ROWS;
  int originX = 24;
  int originY = 64;

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

  String foot = String(selectedIdx + 1) + " / " + bookCount;
  epd.drawString(foot, SCREEN_W - 24 - 80, SCREEN_H - 28, 2);
  epd.update();
}

// ---------------------------------------------------------------------------
// Rendering — reader
// ---------------------------------------------------------------------------
static void drawReaderPage() {
  int slot = cacheFind(readerPage);
  if (slot < 0) {
    epd.fillScreen(TFT_WHITE);
    epd.setTextColor(TFT_BLACK);
    epd.drawString("Loading page " + String(readerPage) + "…", 40, 220, 4);
    epd.update();
    slot = cacheLoad(readerPage);
    if (slot < 0) {
      epd.fillScreen(TFT_WHITE);
      epd.drawString("Couldn't load page " + String(readerPage), 40, 220, 4);
      epd.update();
      return;
    }
  }
  epd.drawBitmap(0, 0, pageCache[slot].data, SCREEN_W, SCREEN_H,
                 TFT_WHITE, TFT_BLACK);
  epd.update();
}

static void postProgress() {
  String body = String("{\"page\":") + readerPage + "}";
  String path = "/api/device/books/" + books[readerBookIdx].id + "/progress";
  httpPostJson(path, body);
}

static void enterReader(int bookIdx) {
  readerBookIdx = bookIdx;
  cacheReset();
  Book& b = books[bookIdx];
  readerPage = b.currentPage > 0 ? b.currentPage : 1;
  screen = Screen::Reader;
  drawReaderPage();
  postProgress();
  prefetchAhead(readerPage + 1, READER_PREFETCH_PAGES);
}

static void exitReader() {
  cacheReset();
  readerBookIdx = -1;
  screen = Screen::Dashboard;
  drawDashboard();
}

static void readerNext() {
  Book& b = books[readerBookIdx];
  if (b.pageCount > 0 && readerPage >= b.pageCount) return;
  readerPage++;
  b.currentPage = readerPage;
  drawReaderPage();
  postProgress();
  prefetchAhead(readerPage + 1, READER_PREFETCH_PAGES);
}

static void readerPrev() {
  if (readerPage <= 1) return;
  readerPage--;
  books[readerBookIdx].currentPage = readerPage;
  drawReaderPage();
  postProgress();
}

// ---------------------------------------------------------------------------
// Boot screens
// ---------------------------------------------------------------------------
static void drawBootMessage(const String& msg) {
  epd.fillScreen(TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  epd.drawString("trmnl·reader", 24, 18, 4);
  epd.drawLine(24, 50, SCREEN_W - 24, 50, TFT_BLACK);
  epd.drawString(msg, 40, 220, 4);
  epd.update();
}

static void drawError(const String& msg) {
  screen = Screen::Error;
  epd.fillScreen(TFT_WHITE);
  epd.setTextColor(TFT_BLACK);
  epd.drawString("trmnl·reader", 24, 18, 4);
  epd.drawString("error:", 40, 180, 4);
  epd.drawString(msg, 40, 220, 4);
  epd.drawString("(press SELECT to retry)", 40, 260, 2);
  epd.update();
}

static bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 30000) return false;
    delay(200);
  }
  return true;
}

static bool bootSequence() {
  drawBootMessage("Connecting to WiFi…");
  if (!connectWiFi()) { drawError("WiFi failed"); return false; }

  drawBootMessage("Fetching books…");
  if (!loadBooks())   { drawError("Couldn't load books"); return false; }

  drawBootMessage(String("Loading ") + bookCount + " covers…");
  for (int i = 0; i < bookCount; i++) loadCover(books[i]);

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
  if (ev == BtnEvent::Prev) {
    selectedIdx = (selectedIdx - 1 + bookCount) % bookCount;
    drawDashboard();
  } else if (ev == BtnEvent::Next) {
    selectedIdx = (selectedIdx + 1) % bookCount;
    drawDashboard();
  } else if (ev == BtnEvent::Select) {
    enterReader(selectedIdx);
  }
}

static void handleReader(BtnEvent ev) {
  if (ev == BtnEvent::Prev)            readerPrev();
  else if (ev == BtnEvent::Next)       readerNext();
  else if (ev == BtnEvent::SelectLong) exitReader();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  buttonsInit();
  epd.begin();
  epd.setRotation(0);
  epd.setTextColor(TFT_BLACK);

  bootSequence();
}

void loop() {
  BtnEvent ev = pollButtons();

  if (screen == Screen::Dashboard)        handleDashboard(ev);
  else if (screen == Screen::Reader)      handleReader(ev);
  else if (screen == Screen::Error &&
           ev == BtnEvent::Select)        bootSequence();

  if (millis() - lastPingMs > READER_KEEPALIVE_MS) {
    lastPingMs = millis();
    httpGetString("/api/device/ping");
  }

  delay(10);
}
