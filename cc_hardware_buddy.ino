/*
 * Claude Desktop Buddy - Arduino IDE main program for Root Maker / ESP32-S3.
 *
 * Hardware access is routed through the Ryzobee BSP:
 * - Display: Rootmaker_Lcd, LovyanGFX, 240x240 ST7789
 * - Button: Rootmaker_btn on GPIO 0
 * - IMU: Rootmaker_lis2dwtr / LIS2DW12
 * - RGB LED: Rootmaker_led / NeoPixel on GPIO 45
 *
 * Board features not exposed by the current BSP are represented by local
 * placeholders so the application surface remains stable while the lower
 * board support catches up.
 */

#include <Ryzobee.h>          // Main BSP for Ryzobee
#include <LittleFS.h>
#include <esp_mac.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include "src/ble_bridge.h"
#include "src/data.h"
#include "src/buddy.h"
#include "src/character.h"
#include "src/persona_state.h"
#include "src/stats.h"
#include "src/speaker.h"

Ryzobee board(RYZOBEE_ROOTMAKER);

// Global display and graphics
static lgfx::LGFX_Sprite* g_sprite = nullptr;
static lgfx::LGFX_Device* g_lcd = nullptr;
static const uint8_t ROOTMAKER_BUTTON_PIN = 0;

// ============================================================================
// RYZOBEE BSP INITIALIZATION
// ============================================================================

static void initRyzobee() {
  board.begin();
  g_lcd = board.rootmaker.lcd;
  
  if (g_lcd) {
    g_lcd->setRotation(2);
    g_lcd->setBrightness(255);
    g_lcd->fillScreen(0x0000);
  }
  
  // Create off-screen sprite for rendering
  g_sprite = new lgfx::LGFX_Sprite(g_lcd);
  g_sprite->setColorDepth(16);      // RGB565
  g_sprite->createSprite(240, 240);

  pinMode(ROOTMAKER_BUTTON_PIN, INPUT_PULLUP);

  Serial.println("[Init] Ryzobee BSP initialized");
}

// ============================================================================
// BT & CONFIG
// ============================================================================

static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
  Serial.printf("[BT] Started as %s\n", btName);
}
 
// ============================================================================
// STATE & CONSTANTS
// ============================================================================

const char* stateNames[] = { 
  "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" 
};

// Display modes
enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };

// UI state
static TamaState    tama = {};
static PersonaState baseState   = P_SLEEP;
static PersonaState activeState = P_SLEEP;
static uint32_t     oneShotUntil = 0;
static uint32_t     lastShakeCheck = 0;
static float        accelBaseline = 1.0f;
static uint32_t     t = 0;

// Display
static DisplayMode  displayMode  = DISP_NORMAL;
static uint8_t      infoPage     = 0;
static uint8_t      petPage      = 0;
static uint32_t     lastInteractMs = 0;
static bool         screenOff    = false;
bool                buddyMode    = true;
bool                gifAvailable = false;
static uint32_t     wakeTransitionUntil = 0;

// Menu
static bool         menuOpen     = false;
static uint8_t      menuSel      = 0;
static bool         settingsOpen = false;
static uint8_t      settingsSel  = 0;
static bool         resetOpen    = false;
static uint8_t      resetSel     = 0;

// Nap / face-down sleep
static bool         napping      = false;
static uint32_t     napStartMs   = 0;
static int8_t       faceDownFrames = 0;

// Prompt approval
static char         lastPromptId[40] = "";
static uint32_t     promptArrivedMs = 0;
static bool         responseSent = false;

// Settings
static uint8_t      brightLevel  = 4;  // 0..4 brightness

const uint16_t SCREEN_OFF_MS_DIV_1000 = 300;  // 5 minutes
const uint16_t HOT   = 0xFA20;   // red-orange
const uint16_t PANEL = 0x2104;   // panel bg
const uint16_t GREEN = 0x07E0;
const uint16_t RED   = 0xF800;
const int W = 240, H = 240;
const uint8_t SPECIES_GIF = 0xFF;
const uint8_t PET_PAGES = 2;
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;
static uint8_t msgScroll = 0;
static uint16_t lastLineGen = 0;
static uint32_t resetConfirmUntil = 0;
static uint8_t resetConfirmIdx = 0xFF;
static bool forceFullRedraw = true;

const char* menuItems[] = { "settings", "screen off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;
const char* settingsItems[] = {
  "brightness", "sound", "bluetooth", "wifi", "led",
  "transcript", "clock rot", "ascii pet", "reset", "back"
};
const uint8_t SETTINGS_N = 10;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static void applyBrightness() {
  if (!g_lcd) return;
  static const uint8_t levels[] = { 24, 64, 112, 176, 255 };
  uint8_t idx = brightLevel;
  if (idx >= sizeof(levels)) idx = sizeof(levels) - 1;
  g_lcd->setBrightness(levels[idx]);
}

static void screenSleep(bool off) {
  if (!g_lcd) return;
  if (off) {
    g_lcd->setBrightness(0);
  } else {
    applyBrightness();
  }
}

static void statusLed(uint8_t r, uint8_t g, uint8_t b) {
  if (!settings().led) r = g = b = 0;
  board.rootmaker.led.strip.setPixelColor(0, board.rootmaker.led.strip.Color(r, g, b));
  board.rootmaker.led.strip.show();
}

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    screenSleep(false);
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
}

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) {
    speakerBeep(freq, dur);
  }
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
  forceFullRedraw = true;
}

static void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  if (g_sprite) g_sprite->fillSprite(characterPalette().bg);
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
  forceFullRedraw = true;
}

static void rootmakerPowerOffPlaceholder() {
  screenOff = true;
  screenSleep(true);
  Serial.println("[Power] Root Maker power-off placeholder: screen off only");
}

static PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;
}

static void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

static bool checkShake() {
  if (!board.rootmaker.lis2dwtr) return false;

  int32_t raw[3] = {0, 0, 0};
  board.rootmaker.lis2dwtr->read_acceleration(raw, 3);
  float ax = raw[0] / 1000.0f;
  float ay = raw[1] / 1000.0f;
  float az = raw[2] / 1000.0f;

  float mag = sqrtf(ax * ax + ay * ay + az * az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}

static bool isFaceDown() {
  if (!board.rootmaker.lis2dwtr) return false;

  int32_t raw[3] = {0, 0, 0};
  board.rootmaker.lis2dwtr->read_acceleration(raw, 3);
  float ax = raw[0] / 1000.0f;
  float ay = raw[1] / 1000.0f;
  float az = raw[2] / 1000.0f;

  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

// ============================================================================
// UI RENDERING
// ============================================================================

static void infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  g_sprite->setTextSize(1);
  g_sprite->setTextColor(p.text, p.bg);
  g_sprite->setCursor(8, y);
  g_sprite->print("Info");
  g_sprite->setTextColor(p.textDim, p.bg);
  g_sprite->setCursor(W - 42, y);
  g_sprite->printf("%u/%u", page + 1, INFO_PAGES);
  y += 14;
  g_sprite->setTextColor(p.body, p.bg);
  g_sprite->setCursor(8, y);
  g_sprite->print(section);
  y += 16;
}

static void lineAt(int& y, const char* fmt, ...) {
  char b[64];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  g_sprite->setCursor(10, y);
  g_sprite->print(b);
  y += 12;
}

static void drawPasskey() {
  const Palette& p = characterPalette();
  g_sprite->fillSprite(p.bg);
  g_sprite->setTextSize(1);
  g_sprite->setTextColor(p.textDim, p.bg);
  g_sprite->setCursor(34, 56);
  g_sprite->print("BLUETOOTH PAIRING");
  g_sprite->setCursor(46, 178);
  g_sprite->print("enter on desktop");
  g_sprite->setTextSize(4);
  g_sprite->setTextColor(p.text, p.bg);
  char b[8];
  snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  g_sprite->setCursor(48, 104);
  g_sprite->print(b);
}

static void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 76;
  g_sprite->fillRect(0, TOP, W, H - TOP, p.bg);
  g_sprite->setTextSize(1);
  int y = TOP + 4;

  if (infoPage == 0) {
    infoHeader(p, y, "ABOUT", infoPage);
    g_sprite->setTextColor(p.textDim, p.bg);
    lineAt(y, "I watch your Claude");
    lineAt(y, "desktop sessions.");
    y += 8;
    lineAt(y, "I sleep when idle,");
    lineAt(y, "wake when you work,");
    lineAt(y, "and react to approvals.");
    y += 8;
    g_sprite->setTextColor(p.text, p.bg);
    lineAt(y, "Short press approves");
    lineAt(y, "a pending prompt.");
  } else if (infoPage == 1) {
    infoHeader(p, y, "BUTTONS", infoPage);
    g_sprite->setTextColor(p.text, p.bg);    lineAt(y, "Short press");
    g_sprite->setTextColor(p.textDim, p.bg); lineAt(y, "  next screen/page");
    lineAt(y, "  approve prompt");
    y += 6;
    g_sprite->setTextColor(p.text, p.bg);    lineAt(y, "Long press");
    g_sprite->setTextColor(p.textDim, p.bg); lineAt(y, "  menu / select");
    lineAt(y, "  deny prompt");
  } else if (infoPage == 2) {
    infoHeader(p, y, "CLAUDE", infoPage);
    g_sprite->setTextColor(p.textDim, p.bg);
    lineAt(y, "sessions  %u", tama.sessionsTotal);
    lineAt(y, "running   %u", tama.sessionsRunning);
    lineAt(y, "waiting   %u", tama.sessionsWaiting);
    y += 8;
    g_sprite->setTextColor(p.text, p.bg);
    lineAt(y, "LINK");
    g_sprite->setTextColor(p.textDim, p.bg);
    lineAt(y, "via       %s", dataScenarioName());
    lineAt(y, "ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "open");
    uint32_t age = tama.lastUpdated ? (millis() - tama.lastUpdated) / 1000 : 0;
    lineAt(y, "last msg  %lus", (unsigned long)age);
    lineAt(y, "state     %s", stateNames[activeState]);
  } else if (infoPage == 3) {
    infoHeader(p, y, "DEVICE", infoPage);
    g_sprite->setTextColor(p.text, p.bg);
    g_sprite->setTextSize(2);
    g_sprite->setCursor(10, y);
    g_sprite->print("battery --");
    g_sprite->setTextSize(1);
    y += 26;
    g_sprite->setTextColor(p.textDim, p.bg);
    lineAt(y, "battery  placeholder");
    lineAt(y, "current  placeholder");
    lineAt(y, "usb      placeholder");
    y += 8;
    g_sprite->setTextColor(p.text, p.bg);
    lineAt(y, "SYSTEM");
    g_sprite->setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) lineAt(y, "owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    lineAt(y, "uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    lineAt(y, "heap     %uKB", ESP.getFreeHeap() / 1024);
    lineAt(y, "bright   %u/4", brightLevel);
  } else if (infoPage == 4) {
    infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();
    g_sprite->setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    g_sprite->setTextSize(2);
    g_sprite->setCursor(10, y);
    g_sprite->print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    g_sprite->setTextSize(1);
    y += 26;
    g_sprite->setTextColor(p.text, p.bg);
    lineAt(y, "%s", btName);
    g_sprite->setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    lineAt(y, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    y += 8;
    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      lineAt(y, "last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      g_sprite->setTextColor(p.text, p.bg);
      lineAt(y, "TO PAIR");
      g_sprite->setTextColor(p.textDim, p.bg);
      lineAt(y, "Open desktop bridge");
      lineAt(y, "and choose %s", btName);
    }
  } else {
    infoHeader(p, y, "CREDITS", infoPage);
    g_sprite->setTextColor(p.textDim, p.bg);
    lineAt(y, "made by");
    y += 4;
    g_sprite->setTextColor(p.text, p.bg);
    lineAt(y, "Felix Rieseberg");
    y += 10;
    g_sprite->setTextColor(p.textDim, p.bg);
    lineAt(y, "hardware");
    y += 4;
    lineAt(y, "Root Maker");
    lineAt(y, "ESP32-S3 + Ryzobee");
  }
}

static uint8_t wrapInto(const char* in, char out[][32], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' ';
      col = 1;
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take);
      col += take;
      w += take;
      wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' ';
      col = 1;
    }
    memcpy(&out[row][col], w, wlen);
    col += wlen;
  }
  if (col > 0 && row < maxRows) {
    out[row][col] = 0;
    row++;
  }
  return row;
}

static void drawSimpleHUD() {
  if (!g_lcd || screenOff) return;
  
  const Palette& p = characterPalette();
  
  // Clear bottom section
  g_sprite->fillRect(0, 120, 240, 120, p.bg);
  
  // Show basic state info
  g_sprite->setTextSize(1);
  g_sprite->setTextColor(p.text, p.bg);
  g_sprite->setCursor(10, 130);
  g_sprite->printf("State: %s", stateNames[activeState]);
  
  g_sprite->setCursor(10, 145);
  g_sprite->printf("Sessions: %u/%u", tama.sessionsWaiting, tama.sessionsTotal);
  
  g_sprite->setCursor(10, 160);
  if (tama.promptId[0]) {
    g_sprite->printf("Approval: %.20s", tama.promptTool);
  } else {
    g_sprite->print("Ready");
  }
}

static void drawApprovalPrompt() {
  if (!tama.promptId[0]) return;
  
  const Palette& p = characterPalette();
  const int AREA = 78;
  
  g_sprite->fillRect(0, 240 - AREA, 240, AREA, p.bg);
  g_sprite->drawFastHLine(0, 240 - AREA, 240, p.textDim);
  
  g_sprite->setTextSize(1);
  g_sprite->setTextColor(p.textDim, p.bg);
  g_sprite->setCursor(10, 240 - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) g_sprite->setTextColor(HOT, p.bg);
  g_sprite->printf("Approve? %lus", (unsigned long)waited);
  
  g_sprite->setTextColor(p.text, p.bg);
  g_sprite->setTextSize(2);
  g_sprite->setCursor(10, 240 - AREA + 20);
  g_sprite->print(tama.promptTool);
  
  if (!responseSent) {
    g_sprite->setTextSize(1);
    g_sprite->setTextColor(0x07E0, p.bg);  // green
    g_sprite->setCursor(10, 240 - 12);
    g_sprite->print("BTN: Approve");
    
    g_sprite->setTextColor(HOT, p.bg);
    g_sprite->setCursor(150, 240 - 12);
    g_sprite->print("HOLD: Deny");
  } else {
    g_sprite->setTextColor(p.textDim, p.bg);
    g_sprite->setCursor(10, 240 - 12);
    g_sprite->print("Sent...");
  }
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    g_sprite->fillCircle(x - 2, y, 2, col);
    g_sprite->fillCircle(x + 2, y, 2, col);
    g_sprite->fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    g_sprite->drawCircle(x - 2, y, 2, col);
    g_sprite->drawCircle(x + 2, y, 2, col);
    g_sprite->drawLine(x - 4, y + 1, x, y + 5, col);
    g_sprite->drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = 72;
  g_sprite->fillRect(0, TOP, W, H - TOP, p.bg);
  g_sprite->setTextSize(1);
  int y = TOP + 24;

  g_sprite->setTextColor(p.textDim, p.bg);
  g_sprite->setCursor(10, y - 2);
  g_sprite->print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(78 + i * 20, y + 2, i < mood, moodCol);

  y += 23;
  g_sprite->setCursor(10, y - 2);
  g_sprite->print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 64 + i * 13;
    if (i < fed) g_sprite->fillCircle(px, y + 1, 2, p.body);
    else g_sprite->drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 23;
  g_sprite->setCursor(10, y - 2);
  g_sprite->print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 82 + i * 18;
    if (i < en) g_sprite->fillRect(px, y - 2, 12, 7, enCol);
    else g_sprite->drawRect(px, y - 2, 12, 7, p.textDim);
  }

  y += 26;
  g_sprite->fillRoundRect(10, y - 2, 54, 16, 3, p.body);
  g_sprite->setTextColor(p.bg, p.body);
  g_sprite->setCursor(18, y + 2);
  g_sprite->printf("Lv %u", stats().level);
  g_sprite->setTextColor(p.textDim, p.bg);
  g_sprite->setCursor(78, y);
  g_sprite->printf("approved %u", stats().approvals);
  g_sprite->setCursor(78, y + 12);
  g_sprite->printf("denied   %u", stats().denials);

  uint32_t nap = stats().napSeconds;
  g_sprite->setCursor(10, y + 28);
  g_sprite->printf("napped %luh%02lum", nap / 3600, (nap / 60) % 60);
  g_sprite->setCursor(10, y + 40);
  g_sprite->printf("tokens %lu", (unsigned long)stats().tokens);
  g_sprite->setCursor(120, y + 40);
  g_sprite->printf("today %lu", (unsigned long)tama.tokensToday);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 72;
  g_sprite->fillRect(0, TOP, W, H - TOP, p.bg);
  g_sprite->setTextSize(1);
  int y = TOP + 22;
  auto ln = [&](uint16_t c, const char* s) {
    g_sprite->setTextColor(c, p.bg);
    g_sprite->setCursor(14, y);
    g_sprite->print(s);
    y += 11;
  };
  ln(p.body, "MOOD");
  ln(p.textDim, "approve fast = up");
  ln(p.textDim, "deny lots = down");
  y += 3;
  ln(p.body, "FED");
  ln(p.textDim, "50K tokens = level up");
  y += 3;
  ln(p.body, "ENERGY");
  ln(p.textDim, "face-down to nap");
  ln(p.textDim, "idle 30s = screen off");
  y += 3;
  ln(p.textDim, "Short: pages/screens");
  ln(p.textDim, "Hold: menu/select");
}

static void drawPet() {
  const Palette& p = characterPalette();
  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  g_sprite->setTextSize(1);
  g_sprite->setTextColor(p.text, p.bg);
  g_sprite->setCursor(10, 80);
  if (ownerName()[0]) g_sprite->printf("%s's %s", ownerName(), petName());
  else g_sprite->print(petName());
  g_sprite->setTextColor(p.textDim, p.bg);
  g_sprite->setCursor(W - 42, 80);
  g_sprite->printf("%u/%u", petPage + 1, PET_PAGES);
}

static void drawHUD() {
  if (tama.promptId[0]) {
    drawApprovalPrompt();
    return;
  }
  const Palette& p = characterPalette();
  const int SHOW = 4, LH = 12, WIDTH = 34;
  const int AREA = SHOW * LH + 6;
  g_sprite->fillRect(0, H - AREA, W, AREA, p.bg);
  g_sprite->setTextSize(1);

  if (tama.lineGen != lastLineGen) {
    msgScroll = 0;
    lastLineGen = tama.lineGen;
    wake();
  }

  if (tama.nLines == 0) {
    g_sprite->setTextColor(p.text, p.bg);
    g_sprite->setCursor(10, H - LH - 4);
    g_sprite->print(tama.msg);
    return;
  }

  static char disp[40][32];
  static uint8_t srcOf[40];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 40; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 40 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }
  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;
  int end = (int)nDisp - msgScroll;
  int start = end - SHOW;
  if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    g_sprite->setTextColor(fresh ? p.text : p.textDim, p.bg);
    g_sprite->setCursor(10, H - AREA + 4 + i * LH);
    g_sprite->print(disp[row]);
  }
  if (msgScroll > 0) {
    g_sprite->setTextColor(p.body, p.bg);
    g_sprite->setCursor(W - 30, H - LH - 4);
    g_sprite->printf("-%u", msgScroll);
  }
}

static void drawMenuOverlay() {
  const Palette& p = characterPalette();
  int mx = 28, my = 36, mw = 184, mh = 140;
  g_sprite->fillRoundRect(mx, my, mw, mh, 4, PANEL);
  g_sprite->drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  g_sprite->setTextSize(1);
  for (uint8_t i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    g_sprite->setTextColor(sel ? p.text : p.textDim, PANEL);
    g_sprite->setCursor(mx + 12, my + 12 + i * 16);
    g_sprite->print(sel ? "> " : "  ");
    g_sprite->print(menuItems[i]);
    if (i == 4) g_sprite->print(dataDemo() ? " on" : " off");
  }
  g_sprite->drawFastHLine(mx + 8, my + mh - 22, mw - 16, p.textDim);
  g_sprite->setTextColor(p.textDim, PANEL);
  g_sprite->setCursor(mx + 12, my + mh - 15);
  g_sprite->print("Short:next Hold:select");
}

static void drawResetOverlay() {
  const Palette& p = characterPalette();
  int mx = 28, my = 62, mw = 184, mh = 92;
  g_sprite->fillRoundRect(mx, my, mw, mh, 4, PANEL);
  g_sprite->drawRoundRect(mx, my, mw, mh, 4, HOT);
  g_sprite->setTextSize(1);
  for (uint8_t i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    bool armed = (i == resetConfirmIdx) && (int32_t)(millis() - resetConfirmUntil) < 0;
    g_sprite->setTextColor(armed ? HOT : (sel ? p.text : p.textDim), PANEL);
    g_sprite->setCursor(mx + 12, my + 12 + i * 16);
    g_sprite->print(sel ? "> " : "  ");
    g_sprite->print(armed ? "really?" : resetItems[i]);
  }
  g_sprite->drawFastHLine(mx + 8, my + mh - 22, mw - 16, p.textDim);
  g_sprite->setTextColor(p.textDim, PANEL);
  g_sprite->setCursor(mx + 12, my + mh - 15);
  g_sprite->print("Short:next Hold:select");
}


static void drawSettingsOverlay() {
  const Palette& p = characterPalette();
  int mx = 20, my = 22, mw = 200, mh = 196;
  g_sprite->fillRoundRect(mx, my, mw, mh, 4, PANEL);
  g_sprite->drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  g_sprite->setTextSize(1);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  for (uint8_t i = 0; i < SETTINGS_N; i++) {
    bool sel = (i == settingsSel);
    int y = my + 10 + i * 15;
    g_sprite->setTextColor(sel ? p.text : p.textDim, PANEL);
    g_sprite->setCursor(mx + 10, y);
    g_sprite->print(sel ? "> " : "  ");
    g_sprite->print(settingsItems[i]);
    g_sprite->setCursor(mx + mw - 54, y);
    g_sprite->setTextColor(p.textDim, PANEL);
    if (i == 0) {
      g_sprite->printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      g_sprite->setTextColor(vals[i - 1] ? GREEN : p.textDim, PANEL);
      g_sprite->print(vals[i - 1] ? "on" : "off");
    } else if (i == 6) {
      static const char* const rn[] = { "auto", "port", "land" };
      g_sprite->print(rn[s.clockRot]);
    } else if (i == 7) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos = buddyMode ? buddySpeciesIdx() + 1 : total;
      g_sprite->printf("%u/%u", pos, total);
    }
  }
  g_sprite->drawFastHLine(mx + 8, my + mh - 22, mw - 16, p.textDim);
  g_sprite->setTextColor(p.textDim, PANEL);
  g_sprite->setCursor(mx + 12, my + mh - 15);
  g_sprite->print("Short:next Hold:change");
}

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2: s.bt = !s.bt; break;
    case 3: s.wifi = !s.wifi; break;
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: s.clockRot = (s.clockRot + 1) % 3; break;
    case 7: nextPet(); return;
    case 8:
      resetOpen = true;
      settingsOpen = false;
      resetSel = 0;
      resetConfirmIdx = 0xFF;
      forceFullRedraw = true;
      return;
    case 9:
      settingsOpen = false;
      menuOpen = true;
      forceFullRedraw = true;
      return;
  }
  settingsSave();
}

static uint32_t wipeDir(const char* dir) {
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) {
    LittleFS.mkdir(dir);
    return 0;
  }
  uint32_t freed = 0;
  File f = d.openNextFile();
  while (f) {
    freed += f.size();
    char p[96];
    snprintf(p, sizeof(p), "%s/%s", dir, f.name());
    bool isDir = f.isDirectory();
    f.close();
    if (isDir) {
      freed += wipeDir(p);
      LittleFS.rmdir(p);
    } else {
      LittleFS.remove(p);
    }
    f = d.openNextFile();
  }
  d.close();
  return freed;
}

static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;
  if (idx == 2) {
    resetOpen = false;
    settingsOpen = true;
    resetConfirmIdx = 0xFF;
    forceFullRedraw = true;
    return;
  }
  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    return;
  }
  if (idx == 0) {
    wipeDir("/characters");
  } else {
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

static void menuConfirm() {
  switch (menuSel) {
    case 0:
      settingsOpen = true;
      menuOpen = false;
      settingsSel = 0;
      forceFullRedraw = true;
      break;
    case 1:
      menuOpen = false;
      rootmakerPowerOffPlaceholder();
      break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      break;
    case 4:
      dataSetDemo(!dataDemo());
      break;
    case 5:
      menuOpen = false;
      forceFullRedraw = true;
      break;
  }
}

// ============================================================================
// BUTTON HANDLING
// ============================================================================

static uint32_t buttonPressStart = 0;
static bool     buttonLongPressed = false;

static void invalidateUi() {
  forceFullRedraw = true;
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}

static void handleButtonInput() {
  bool btnPressed = (digitalRead(ROOTMAKER_BUTTON_PIN) == LOW);
  
  if (btnPressed && buttonPressStart == 0) {
    buttonPressStart = millis();
    buttonLongPressed = false;
    wake();
  } else if (btnPressed && !buttonLongPressed && (millis() - buttonPressStart) > 600) {
    buttonLongPressed = true;
    
    // Long press: open/select/close.
    if (tama.promptId[0] && !responseSent) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd),
        "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}",
        tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (resetOpen) {
      applyReset(resetSel);
    } else if (settingsOpen) {
      applySetting(settingsSel);
    } else if (menuOpen) {
      menuConfirm();
    } else {
      menuOpen = true;
      menuSel = 0;
    }
    invalidateUi();
    Serial.println(menuOpen ? "menu: open" : "menu: close");
  } else if (!btnPressed && buttonPressStart != 0) {
    // Button released
    uint32_t pressDur = millis() - buttonPressStart;
    buttonPressStart = 0;
    
    if (buttonLongPressed) {
      buttonLongPressed = false;
      return;  // long press already handled
    }
    
    // Short press: cycle through states/menus
    if (tama.promptId[0] && !responseSent) {
      // Approve
      char cmd[96];
      snprintf(cmd, sizeof(cmd), 
        "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", 
        tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      uint32_t tookS = (millis() - promptArrivedMs) / 1000;
      statsOnApproval(tookS);
      beep(2400, 60);
      if (tookS < 5) triggerOneShot(P_HEART, 2000);
    } else if (resetOpen) {
      resetSel = (resetSel + 1) % RESET_N;
      resetConfirmIdx = 0xFF;
      invalidateUi();
    } else if (menuOpen) {
      menuSel = (menuSel + 1) % MENU_N;
      invalidateUi();
    } else if (settingsOpen) {
      settingsSel = (settingsSel + 1) % SETTINGS_N;
      invalidateUi();
    } else if (displayMode == DISP_PET) {
      petPage++;
      if (petPage >= PET_PAGES) {
        petPage = 0;
        displayMode = DISP_INFO;
        infoPage = 0;
        applyDisplayMode();
      } else {
        invalidateUi();
      }
    } else if (displayMode == DISP_INFO) {
      infoPage++;
      if (infoPage >= INFO_PAGES) {
        infoPage = 0;
        displayMode = DISP_NORMAL;
        applyDisplayMode();
      } else {
        invalidateUi();
      }
    } else {
      displayMode = DISP_PET;
      petPage = 0;
      applyDisplayMode();
    }
  }
}

// ============================================================================
// PROMPT HANDLING
// ============================================================================

static void checkPromptArrival() {
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
      Serial.println("[Prompt] New approval request arrived");
    }
  }
}

// ============================================================================
// MAIN SETUP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Claude Desktop Buddy (Ryzobee) ===\n");
  
  // Initialize hardware
  initRyzobee();
  speakerInit();
  
  // Initialize subsystems
  startBt();
  statusLed(0, 0, 0);
  
  // Load persistent data
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  
  // Initialize buddy and character systems
  buddyInit(g_lcd, g_sprite);
  characterSetTarget(g_lcd, g_sprite);
  characterInit(nullptr);
  gifAvailable = characterLoaded();
  
  // Load saved buddy mode / species
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();
  
  // Splash screen
  {
    const Palette& p = characterPalette();
    g_sprite->fillSprite(p.bg);
    g_sprite->setTextDatum(MC_DATUM);
    g_sprite->setTextSize(2);
    g_sprite->setTextColor(p.text, p.bg);
    g_sprite->drawString("Claude's", 120, 100);
    g_sprite->drawString("Buddy", 120, 140);
    g_sprite->setTextSize(1);
    g_sprite->setTextColor(p.textDim, p.bg);
    g_sprite->drawString("Ryzobee Edition", 120, 180);
    g_sprite->setTextDatum(TL_DATUM);
    g_sprite->pushSprite(0, 0);
    delay(2000);
  }
  
  Serial.printf("buddy: %s mode\n", buddyMode ? "ASCII" : "GIF");
  Serial.println("[Setup] Ready!\n");
}

// ============================================================================
// MAIN LOOP
// ============================================================================

void loop() {
  t++;
  uint32_t now = millis();
  
  // 1. Poll data from USB/BLE
  dataPoll(&tama);
  if (statsPollLevelUp()) { triggerOneShot(P_CELEBRATE, 3000); beep(2400, 400); }
  
  // 2. Derive base state
  baseState = derive(tama);
  
  // Hold sleep for 12s after waking
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) {
    baseState = P_SLEEP;
  }
  
  // 3. Apply one-shot overrides
  if ((int32_t)(now - oneShotUntil) >= 0) {
    activeState = baseState;
  }
  
  // 4. LED pulse on attention
  if (activeState == P_ATTENTION && settings().led) {
    if ((now / 400) % 2) {
      statusLed(28, 0, 0);
    } else {
      statusLed(0, 0, 0);
    }
  } else {
    statusLed(0, 0, 0);
  }
  
  // 5. Check for shake
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      beep(800, 200);
      Serial.println("[IMU] Shake detected -> dizzy");
    }
  }
  
  // 6. Check prompt arrival
  checkPromptArrival();
  bool inPrompt = tama.promptId[0] && !responseSent;
  
  // 7. Handle button input
  handleButtonInput();
  
  // 8. Face-down nap detection
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down) {
      if (faceDownFrames < 20) faceDownFrames++;
    } else {
      if (faceDownFrames > -10) faceDownFrames--;
    }
  }
  
  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    Serial.println("[IMU] Face-down -> napping");
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
    Serial.println("[IMU] Wake from nap");
  }
  
  // 9. Auto screen-off after 30s inactivity
  if (!screenOff && !inPrompt && millis() - lastInteractMs > (SCREEN_OFF_MS_DIV_1000 * 1000)) {
    screenOff = true;
    screenSleep(true);
    Serial.println("[Display] Screen off (timeout)");
  }
  
  // 10. Render
  if (napping || screenOff) {
    // Skip rendering while napping or while the screen backlight is off.
  } else if (buddyMode) {
    if (forceFullRedraw) {
      g_sprite->fillSprite(characterPalette().bg);
      buddyInvalidate();
      forceFullRedraw = false;
    }
    buddyTick(activeState);
  } else if (characterLoaded()) {
    if (forceFullRedraw) {
      g_sprite->fillSprite(characterPalette().bg);
      characterInvalidate();
      forceFullRedraw = false;
    }
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    g_sprite->fillSprite(p.bg);
    forceFullRedraw = false;
    g_sprite->setTextColor(p.textDim, p.bg);
    g_sprite->setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress();
      uint32_t total = xferTotal();
      g_sprite->setCursor(42, 90);
      g_sprite->print("installing");
      g_sprite->setCursor(42, 106);
      g_sprite->printf("%luK / %luK", done / 1024, total / 1024);
      int barW = W - 60;
      g_sprite->drawRect(30, 126, barW, 10, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) g_sprite->fillRect(31, 127, fill - 1, 8, p.body);
      }
    } else {
      g_sprite->setCursor(56, 106);
      g_sprite->print("No character loaded");
    }
  }
  
  // 11. Draw UI overlays
  if (!napping && !screenOff) {
    if (blePasskey()) {
      drawPasskey();
    } else if (displayMode == DISP_INFO) {
      drawInfo();
    } else if (displayMode == DISP_PET) {
      drawPet();
    } else if (settings().hud) {
      drawHUD();
    }
    if (resetOpen) {
      drawResetOverlay();
    } else if (settingsOpen) {
      drawSettingsOverlay();
    } else if (menuOpen) {
      drawMenuOverlay();
    }
    g_sprite->pushSprite(0, 0);
  }
  
  // 12. Frame rate control
  uint32_t frameTime = millis() - now;
  if (screenOff) {
    delay(100);  // lower frame rate when off
  } else {
    int remaining = 16 - frameTime;  // target 60 FPS
    if (remaining > 0) delay(remaining);
  }
}
