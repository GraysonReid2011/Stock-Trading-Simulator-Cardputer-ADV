/*
 * Cardputer Stock Trading Simulator
 * Target: M5Stack Cardputer / Cardputer ADV (ESP32-S3, 240x135 LCD, keyboard)
 *
 * Libraries (Arduino Library Manager): M5Cardputer (pulls in M5Unified + M5GFX)
 * Board: "M5Stack-StampS3" (or M5Cardputer if your core lists it)
 *
 * Controls
 *   ; / .       select previous / next stock   (the up / down arrow keys)
 *   , / /       decrease / increase order size (the left / right arrow keys)
 *   1-6         jump straight to a stock
 *   b           buy selected stock
 *   s           sell selected stock
 *   x           sell entire position
 *   v or Tab    toggle Market / Portfolio view
 *   p           pause / resume
 *   f           cycle simulation speed
 *   m           mute / unmute
 *   o           flip screen orientation (landscape, saved)
 *   r (twice)   reset game to $10,000
 *
 * Progress (cash, holdings, prices) is saved to flash automatically.
 */

#include <M5Cardputer.h>
#include <Preferences.h>

#define N_STOCKS    6
#define HIST        130        // price history points (one per pixel of chart)
#define START_CASH  10000.0f
#define DAY_TICKS   60         // ticks per "day" (used for daily change %)

// ---------- colours ----------
#define C_BG     0x0000
#define C_HDR    0x2104
#define C_SEL    0x18E3
#define C_UP     0x07E0
#define C_DOWN   0xF800
#define C_TEXT   0xFFFF
#define C_DIM    0x8410
#define C_WARN   0xFFE0
#define C_CYAN   0x07FF

struct Stock {
  const char* sym;
  const char* name;
  float price;
  float base;     // long-run "fair value", price mean-reverts toward it
  float drift;    // per-tick drift
  float vol;      // per-tick volatility
  float hist[HIST];
  int   shares;
  float avg;      // average cost per share
  float prevClose;
};

Stock stocks[N_STOCKS] = {
  {"BYTE", "ByteWorks Inc",     120.0f, 120.0f, 0.00008f, 0.008f},
  {"MOON", "Moonshot Rockets",   45.0f,  45.0f, 0.00005f, 0.014f},
  {"OILX", "OilX Energy",        78.0f,  78.0f, 0.00002f, 0.009f},
  {"GOLD", "Golden Vault",      190.0f, 190.0f, 0.00001f, 0.004f},
  {"MEME", "MemeCoin Corp",       8.0f,   8.0f, 0.00000f, 0.030f},
  {"BANK", "First Pixel Bank",   62.0f,  62.0f, 0.00003f, 0.006f},
};

M5Canvas canvas(&M5Cardputer.Display);
Preferences prefs;

float    cash = START_CASH;
float    nwHist[HIST];
int      sel = 0;
int      view = 0;               // 0 = market, 1 = portfolio
bool     paused = false;
bool     muted = false;
uint32_t tickCount = 0;
uint32_t day = 1;

const int qtySteps[] = {1, 5, 10, 25, 100, 0};   // 0 = MAX
const int N_QTY = 6;
int qtyIdx = 2;

const uint32_t speedMs[] = {2000, 1000, 500, 250};
const char*    speedName[] = {"0.5x", "1x", "2x", "4x"};
int speedIdx = 1;

char     news[64] = "Welcome to the market!";
uint32_t newsColor = C_CYAN;
char     toast[40] = "";
uint32_t toastUntil = 0;
uint32_t toastColor = C_WARN;
uint32_t resetArmedUntil = 0;

uint32_t lastTick = 0, lastDraw = 0, lastSave = 0;

// ---------- screen rotation ----------
int rot = 1;   // 0-3; only landscape ones (width > height) are ever used

bool isLandscape() {
  return M5Cardputer.Display.width() > M5Cardputer.Display.height();
}

void saveRotation() {
  prefs.begin("stocksim", false);
  prefs.putInt("rot", rot);
  prefs.end();
}

// Apply the saved rotation; if it isn't landscape, find one that is.
void applyRotation() {
  for (int k = 0; k < 4; k++) {
    int r = (rot + k) % 4;
    M5Cardputer.Display.setRotation(r);
    if (isLandscape()) { rot = r; return; }
  }
}

// Flip to the next landscape orientation (i.e. turn the screen 180 degrees).
void cycleRotation() {
  for (int k = 1; k <= 4; k++) {
    int r = (rot + k) % 4;
    M5Cardputer.Display.setRotation(r);
    if (isLandscape() && r != rot) { rot = r; break; }
  }
  saveRotation();
}

// ---------- helpers ----------
float gauss() {                    // approx. standard normal
  float s = 0;
  for (int i = 0; i < 4; i++) s += random(0, 10001) / 10000.0f;
  return (s - 2.0f) * 1.732f;
}

float netWorth() {
  float t = cash;
  for (int i = 0; i < N_STOCKS; i++) t += stocks[i].shares * stocks[i].price;
  return t;
}

void showToast(const char* m, uint32_t col = C_WARN) {
  strncpy(toast, m, sizeof(toast) - 1);
  toast[sizeof(toast) - 1] = 0;
  toastColor = col;
  toastUntil = millis() + 2000;
}

void beep(int f, int ms) {
  if (!muted) M5Cardputer.Speaker.tone(f, ms);
}

int currentQty(bool buying) {
  Stock& s = stocks[sel];
  int q = qtySteps[qtyIdx];
  if (q == 0) q = buying ? (int)floorf(cash / s.price) : s.shares;
  return q;
}

// Build a plausible history ending at the current price (random walk backwards)
void seedHistory(int i) {
  Stock& s = stocks[i];
  float p = s.price;
  for (int k = HIST - 1; k >= 0; k--) {
    s.hist[k] = p;
    float r = s.drift + s.vol * gauss();
    p /= (1.0f + r);
    if (p < 0.5f) p = 0.5f;
  }
  s.prevClose = s.hist[max(0, HIST - 1 - DAY_TICKS)];
}

// ---------- persistence ----------
void save() {
  prefs.begin("stocksim", false);
  prefs.putFloat("cash", cash);
  prefs.putUInt("tick", tickCount);
  prefs.putUInt("day", day);
  char k[8];
  for (int i = 0; i < N_STOCKS; i++) {
    snprintf(k, sizeof(k), "sh%d", i); prefs.putInt(k, stocks[i].shares);
    snprintf(k, sizeof(k), "av%d", i); prefs.putFloat(k, stocks[i].avg);
    snprintf(k, sizeof(k), "pr%d", i); prefs.putFloat(k, stocks[i].price);
  }
  prefs.end();
}

bool load() {
  prefs.begin("stocksim", true);
  bool ok = prefs.isKey("cash");
  if (ok) {
    cash = prefs.getFloat("cash", START_CASH);
    tickCount = prefs.getUInt("tick", 0);
    day = prefs.getUInt("day", 1);
    char k[8];
    for (int i = 0; i < N_STOCKS; i++) {
      snprintf(k, sizeof(k), "sh%d", i); stocks[i].shares = prefs.getInt(k, 0);
      snprintf(k, sizeof(k), "av%d", i); stocks[i].avg = prefs.getFloat(k, 0);
      snprintf(k, sizeof(k), "pr%d", i); stocks[i].price = prefs.getFloat(k, stocks[i].price);
      stocks[i].base = stocks[i].price;
    }
  }
  prefs.end();
  return ok;
}

void resetGame() {
  cash = START_CASH;
  tickCount = 0;
  day = 1;
  const float startPrices[N_STOCKS] = {120, 45, 78, 190, 8, 62};
  for (int i = 0; i < N_STOCKS; i++) {
    stocks[i].price = stocks[i].base = startPrices[i];
    stocks[i].shares = 0;
    stocks[i].avg = 0;
    seedHistory(i);
  }
  float nw = netWorth();
  for (int i = 0; i < HIST; i++) nwHist[i] = nw;
  snprintf(news, sizeof(news), "New game. You have $%.0f", START_CASH);
  newsColor = C_CYAN;
  save();
}

// ---------- simulation ----------
void randomEvent() {
  if (random(0, 100) < 2) {                      // market-wide event
    bool up = random(0, 2);
    float m = (random(10, 30) / 1000.0f) * (up ? 1 : -1);
    for (int i = 0; i < N_STOCKS; i++) stocks[i].price *= (1.0f + m);
    snprintf(news, sizeof(news), up ? "Rate cut! Markets rally" : "Recession fears hit markets");
    newsColor = up ? C_UP : C_DOWN;
    return;
  }
  int i = random(0, N_STOCKS);
  bool up = random(0, 2);
  float shock = random(30, 100) / 1000.0f;
  if (stocks[i].vol > 0.02f) shock *= 1.8f;      // memes move harder
  stocks[i].price *= up ? (1.0f + shock) : (1.0f - shock);
  static const char* goodNews[] = {"%s beats earnings!", "%s unveils new product", "%s lands huge contract", "Analysts upgrade %s"};
  static const char* badNews[]  = {"%s CEO resigns", "%s product recall", "%s misses earnings", "Analysts downgrade %s"};
  snprintf(news, sizeof(news), up ? goodNews[random(0, 4)] : badNews[random(0, 4)], stocks[i].sym);
  newsColor = up ? C_UP : C_DOWN;
}

void tick() {
  float mkt = 0.002f * gauss();                  // shared market factor
  for (int i = 0; i < N_STOCKS; i++) {
    Stock& s = stocks[i];
    float r = s.drift + mkt + s.vol * gauss() + 0.002f * logf(s.base / s.price);
    s.price *= (1.0f + r);
    s.base *= (1.0f + s.drift);
  }
  if (random(0, 100) < 4) randomEvent();         // ~1 event / 25 ticks

  for (int i = 0; i < N_STOCKS; i++) {
    Stock& s = stocks[i];
    if (s.price < 0.5f) s.price = 0.5f;
    memmove(&s.hist[0], &s.hist[1], sizeof(float) * (HIST - 1));
    s.hist[HIST - 1] = s.price;
  }
  memmove(&nwHist[0], &nwHist[1], sizeof(float) * (HIST - 1));
  nwHist[HIST - 1] = netWorth();

  tickCount++;
  if (tickCount % DAY_TICKS == 0) {
    day++;
    for (int i = 0; i < N_STOCKS; i++) stocks[i].prevClose = stocks[i].price;
  }
}

// ---------- trading ----------
void doBuy() {
  Stock& s = stocks[sel];
  int q = currentQty(true);
  if (q <= 0) { showToast("Not enough cash", C_DOWN); beep(200, 120); return; }
  float cost = q * s.price;
  if (cost > cash + 0.001f) { showToast("Not enough cash", C_DOWN); beep(200, 120); return; }
  s.avg = (s.avg * s.shares + cost) / (s.shares + q);
  s.shares += q;
  cash -= cost;
  char m[40]; snprintf(m, sizeof(m), "Bought %d %s @ %.2f", q, s.sym, s.price);
  showToast(m, C_UP);
  beep(1800, 60);
  save();
}

void doSell(bool all) {
  Stock& s = stocks[sel];
  int q = all ? s.shares : currentQty(false);
  if (q > s.shares) q = s.shares;
  if (q <= 0) { showToast("Nothing to sell", C_DOWN); beep(200, 120); return; }
  float proceeds = q * s.price;
  float pl = q * (s.price - s.avg);
  cash += proceeds;
  s.shares -= q;
  if (s.shares == 0) s.avg = 0;
  char m[40]; snprintf(m, sizeof(m), "Sold %d %s %s$%.0f", q, s.sym, pl >= 0 ? "+" : "-", fabsf(pl));
  showToast(m, pl >= 0 ? C_UP : C_DOWN);
  beep(pl >= 0 ? 2200 : 900, 80);
  save();
}

// ---------- drawing ----------
void drawSpark(const float* d, int n, int x, int y, int w, int h) {
  float lo = d[0], hi = d[0];
  for (int i = 1; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
  float range = hi - lo;
  if (range < 0.0001f) range = 0.0001f;
  uint16_t col = (d[n - 1] >= d[0]) ? C_UP : C_DOWN;
  canvas.drawRect(x - 1, y - 1, w + 2, h + 2, C_HDR);
  int px = 0, py = 0;
  for (int i = 0; i < n; i++) {
    int cx = x + (i * (w - 1)) / (n - 1);
    int cy = y + h - 1 - (int)((d[i] - lo) / range * (h - 1));
    if (i > 0) canvas.drawLine(px, py, cx, cy, col);
    px = cx; py = cy;
  }
}

void drawHeader() {
  canvas.fillRect(0, 0, 240, 13, C_HDR);
  char b[32];
  canvas.setTextColor(C_TEXT, C_HDR);
  canvas.setTextDatum(top_left);
  snprintf(b, sizeof(b), "Cash $%.0f", cash);
  canvas.drawString(b, 3, 3);
  float nw = netWorth();
  canvas.setTextColor(nw >= START_CASH ? C_UP : C_DOWN, C_HDR);
  canvas.setTextDatum(top_right);
  snprintf(b, sizeof(b), "NW $%.0f", nw);
  canvas.drawString(b, 237, 3);
  canvas.setTextColor(C_DIM, C_HDR);
  canvas.setTextDatum(top_center);
  snprintf(b, sizeof(b), "D%lu", (unsigned long)day);
  canvas.drawString(b, 120, 3);
  canvas.setTextDatum(top_left);
}

void drawBottom() {
  canvas.fillRect(0, 114, 240, 21, C_BG);
  canvas.drawFastHLine(0, 114, 240, C_HDR);
  canvas.setTextDatum(top_left);
  if (millis() < toastUntil) {
    canvas.setTextColor(toastColor, C_BG);
    canvas.drawString(toast, 3, 117);
  } else {
    canvas.setTextColor(newsColor, C_BG);
    canvas.drawString(news, 3, 117);
  }
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString("b buy s sell x all ;. pick ,/ qty v", 3, 126);
}

void drawMarket() {
  char b[48];
  canvas.setTextDatum(top_left);
  // stock list
  for (int i = 0; i < N_STOCKS; i++) {
    Stock& s = stocks[i];
    int y = 16 + i * 14;
    if (i == sel) canvas.fillRect(0, y - 1, 104, 13, C_SEL);
    uint16_t bg = (i == sel) ? C_SEL : C_BG;
    bool up = s.price >= s.prevClose;
    canvas.setTextColor(C_TEXT, bg);
    canvas.drawString(s.sym, 3, y + 2);
    canvas.setTextColor(up ? C_UP : C_DOWN, bg);
    snprintf(b, sizeof(b), "%.2f", s.price);
    canvas.drawString(b, 40, y + 2);
    if (s.shares > 0) {
      canvas.setTextColor(C_WARN, bg);
      canvas.drawString("*", 96, y + 2);
    }
  }
  // order size + speed under list
  canvas.setTextColor(C_DIM, C_BG);
  int q = qtySteps[qtyIdx];
  if (q == 0) snprintf(b, sizeof(b), "Qty: MAX");
  else snprintf(b, sizeof(b), "Qty: %d", q);
  canvas.setTextColor(C_CYAN, C_BG);
  canvas.drawString(b, 3, 102);
  canvas.setTextColor(paused ? C_WARN : C_DIM, C_BG);
  snprintf(b, sizeof(b), "%s", paused ? "PAUSED" : speedName[speedIdx]);
  canvas.drawString(b, 66, 102);

  // right panel
  Stock& s = stocks[sel];
  canvas.setTextColor(C_TEXT, C_BG);
  canvas.drawString(s.name, 110, 16);
  drawSpark(s.hist, HIST, 108, 28, 128, 46);

  float chg = (s.price / s.prevClose - 1.0f) * 100.0f;
  canvas.setTextColor(chg >= 0 ? C_UP : C_DOWN, C_BG);
  snprintf(b, sizeof(b), "$%.2f  %+.2f%%", s.price, chg);
  canvas.drawString(b, 110, 80);

  canvas.setTextColor(C_TEXT, C_BG);
  if (s.shares > 0) {
    snprintf(b, sizeof(b), "Own %d @ %.2f", s.shares, s.avg);
    canvas.drawString(b, 110, 91);
    float pl = s.shares * (s.price - s.avg);
    canvas.setTextColor(pl >= 0 ? C_UP : C_DOWN, C_BG);
    snprintf(b, sizeof(b), "P/L %s$%.2f", pl >= 0 ? "+" : "-", fabsf(pl));
    canvas.drawString(b, 110, 102);
  } else {
    canvas.setTextColor(C_DIM, C_BG);
    canvas.drawString("No position", 110, 91);
  }
}

void drawPortfolio() {
  char b[48];
  canvas.setTextDatum(top_left);
  canvas.setTextColor(C_DIM, C_BG);
  canvas.drawString("SYM", 3, 15);
  canvas.drawString("QTY", 40, 15);
  canvas.drawString("AVG", 72, 15);
  canvas.drawString("NOW", 120, 15);
  canvas.setTextDatum(top_right);
  canvas.drawString("P/L", 237, 15);
  canvas.setTextDatum(top_left);

  float totalPL = 0;
  for (int i = 0; i < N_STOCKS; i++) {
    Stock& s = stocks[i];
    int y = 25 + i * 11;
    if (i == sel) canvas.fillRect(0, y - 1, 240, 11, C_SEL);
    uint16_t bg = (i == sel) ? C_SEL : C_BG;
    canvas.setTextColor(C_TEXT, bg);
    canvas.drawString(s.sym, 3, y);
    snprintf(b, sizeof(b), "%d", s.shares);
    canvas.drawString(b, 40, y);
    if (s.shares > 0) {
      snprintf(b, sizeof(b), "%.2f", s.avg);
      canvas.drawString(b, 72, y);
    } else {
      canvas.setTextColor(C_DIM, bg);
      canvas.drawString("-", 72, y);
    }
    canvas.setTextColor(s.price >= s.prevClose ? C_UP : C_DOWN, bg);
    snprintf(b, sizeof(b), "%.2f", s.price);
    canvas.drawString(b, 120, y);
    float pl = s.shares * (s.price - s.avg);
    totalPL += pl;
    canvas.setTextDatum(top_right);
    if (s.shares > 0) {
      canvas.setTextColor(pl >= 0 ? C_UP : C_DOWN, bg);
      snprintf(b, sizeof(b), "%+.2f", pl);
    } else {
      canvas.setTextColor(C_DIM, bg);
      snprintf(b, sizeof(b), "-");
    }
    canvas.drawString(b, 237, y);
    canvas.setTextDatum(top_left);
  }

  float nw = netWorth();
  float ret = (nw / START_CASH - 1.0f) * 100.0f;
  canvas.setTextColor(ret >= 0 ? C_UP : C_DOWN, C_BG);
  snprintf(b, sizeof(b), "Total return %+.2f%%  (open P/L %+.0f)", ret, totalPL);
  canvas.drawString(b, 3, 93);
  drawSpark(nwHist, HIST, 3, 103, 234, 9);
}

void draw() {
  canvas.fillSprite(C_BG);
  drawHeader();
  if (view == 0) drawMarket(); else drawPortfolio();
  drawBottom();
  canvas.pushSprite(0, 0);
}

// ---------- input ----------
void handleKeys() {
  if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) return;
  auto st = M5Cardputer.Keyboard.keysState();
  if (st.tab) view ^= 1;

  for (char c : st.word) {
    c = tolower(c);
    switch (c) {
      case ';': sel = (sel + N_STOCKS - 1) % N_STOCKS; break;
      case '.': sel = (sel + 1) % N_STOCKS; break;
      case ',': if (qtyIdx > 0) qtyIdx--; break;
      case '/': if (qtyIdx < N_QTY - 1) qtyIdx++; break;
      case 'b': doBuy(); break;
      case 's': doSell(false); break;
      case 'x': doSell(true); break;
      case 'v': view ^= 1; break;
      case 'p': paused = !paused; break;
      case 'f': speedIdx = (speedIdx + 1) % 4; break;
      case 'o': cycleRotation(); break;
      case 'm': muted = !muted; showToast(muted ? "Sound off" : "Sound on", C_CYAN); break;
      case 'r':
        if (millis() < resetArmedUntil) {
          resetGame();
          showToast("Game reset!", C_CYAN);
          resetArmedUntil = 0;
        } else {
          resetArmedUntil = millis() + 3000;
          showToast("Press r again to reset", C_WARN);
        }
        break;
      default:
        if (c >= '1' && c <= '0' + N_STOCKS) sel = c - '1';
        break;
    }
  }
}

// ---------- main ----------
void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  prefs.begin("stocksim", true);
  rot = prefs.getInt("rot", 1);
  prefs.end();
  applyRotation();
  M5Cardputer.Display.setBrightness(120);
  M5Cardputer.Speaker.setVolume(80);

  canvas.setColorDepth(16);
  canvas.createSprite(240, 135);
  canvas.setTextSize(1);
  canvas.setTextWrap(false);

  randomSeed(esp_random());

  if (load()) {
    for (int i = 0; i < N_STOCKS; i++) seedHistory(i);
    float nw = netWorth();
    for (int i = 0; i < HIST; i++) nwHist[i] = nw;
    snprintf(news, sizeof(news), "Welcome back, trader!");
  } else {
    resetGame();
  }
  lastTick = lastDraw = lastSave = millis();
}

void loop() {
  M5Cardputer.update();
  handleKeys();

  uint32_t now = millis();
  if (!paused && now - lastTick >= speedMs[speedIdx]) {
    lastTick = now;
    tick();
  }
  if (now - lastDraw >= 50) {
    lastDraw = now;
    draw();
  }
  if (now - lastSave >= 30000) {
    lastSave = now;
    save();
  }
}
