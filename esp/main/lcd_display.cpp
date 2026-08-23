// ─────────────────────────────────────────────────────────────────
//  lcd_display.cpp — 16x2 LCD1602 on a PCF8574 I2C backpack.
//
//  Screen model:
//    BASE     — what the device is waiting for right now (idle, guide
//               steps, armed). Stays until an event replaces it.
//    TRANSIENT— event feedback (BLE connect, result, timeout, error).
//               Always reverts to the BASE screen automatically, so no
//               message ever gets stuck on screen.
//
//  Animations (spinner, confidence bar) redraw at LCD_ANIM_MS. While the
//  piezo sampler listens, ALL bus traffic stops (it couples into the ADC);
//  requests made then are replayed when sampling ends. The armed screen
//  itself is written once before arming for exactly that reason.
// ─────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include "lcd_display.h"
#include "config.h"

#ifdef CONFIG_IDF_TARGET_ESP32S3
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <cstdio>
#include <cstring>

static LiquidCrystal_I2C* lcd = nullptr;
static bool lcd_ready = false;

// ─── Screen bookkeeping ──────────────────────────────────────────
struct Screen {
  char l1[17];
  char l2[17];
};

enum class Anim : uint8_t { NONE, SPINNER, BAR };

static Screen base_scr   = {"Fruitipi", "Ready - SCAN"};
static Screen trans_scr;
static bool   trans_active = false;
static uint32_t trans_until = 0;

static Anim      cur_anim = Anim::NONE;
static float     anim_value = 0.0f;        // 0..1 for BAR
static uint32_t  last_anim_ms = 0;
static uint8_t   spinner_frame = 0;
static bool s_quiet = false;               // piezo listening: freeze the bus

static const char SPINNER[4] = {'-', '/', char(0x5C), '|'};

// ─── Low-level write ─────────────────────────────────────────────
static void paint(const Screen& s) {
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print(s.l1);
  if (s.l2[0]) {
    lcd->setCursor(0, 1);
    lcd->print(s.l2);
  }
}

// Confidence bar occupies all of line 2: ten block/dot cells + pct.
static void paint_bar(float v01) {
  char cells[11];
  float filled = v01 * 10.0f;
  for (int i = 0; i < 10; i++) {
    cells[i] = ((float)i + 1.0f <= filled) ? char(0xFF) : '.';
  }
  cells[10] = '\0';
  char line[17];
  snprintf(line, sizeof(line), "%s %3d%%", cells, (int)(v01 * 100.0f + 0.5f));
  lcd->setCursor(0, 1);
  lcd->print(line);
}

enum class Where : uint8_t { BASE, TRANSIENT };

static void commit(Where w) {
  if (!lcd_ready || s_quiet) return;
  if (w == Where::BASE) {
    paint(base_scr);
    return;
  }
  paint(trans_scr);
  if (cur_anim == Anim::BAR) paint_bar(anim_value);
}

static void set_screen(const char* l1, const char* l2, uint32_t hold_ms,
                       Anim anim = Anim::NONE, float value = 0.0f) {
  strncpy(trans_scr.l1, l1 ? l1 : "", 16); trans_scr.l1[16] = '\0';
  strncpy(trans_scr.l2, l2 ? l2 : "", 16); trans_scr.l2[16] = '\0';
  trans_active = true;
  cur_anim = anim;
  anim_value = value;
  trans_until = millis() + hold_ms;
  last_anim_ms = millis();
  commit(Where::TRANSIENT);
}

static void set_base(const char* l1, const char* l2) {
  strncpy(base_scr.l1, l1 ? l1 : "", 16); base_scr.l1[16] = '\0';
  strncpy(base_scr.l2, l2 ? l2 : "", 16); base_scr.l2[16] = '\0';
  trans_active = false;                    // base replaces anything on screen
  cur_anim = Anim::NONE;
  commit(Where::BASE);
}

// Transient screens: shown now, revert to base automatically.
static void flash(const char* l1, const char* l2, uint32_t hold_ms) {
  set_screen(l1, l2, hold_ms);
}

// ─── Public API ──────────────────────────────────────────────────
void lcd_set_quiet(bool quiet) {
  if (quiet == s_quiet) return;
  s_quiet = quiet;
  // Bus just went silent (piezo armed) or came back (capture done).
  // Repaint whatever belongs on screen now — the quiet window may have
  // swallowed requests or left a half-finished animation.
  if (!quiet) commit(trans_active ? Where::TRANSIENT : Where::BASE);
}

void lcd_display_init() {
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  Wire.setClock(LCD_I2C_HZ);

  // Backpack address varies with the PCF8574 jumper — scan for it.
  uint8_t addr = 0;
  for (uint8_t a = 0x20; a <= 0x3F && addr == 0; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) addr = a;
  }
  if (addr == 0) {
    Serial.println("[LCD] No display found");
    return;
  }
  Serial.printf("[LCD] Display found at 0x%02X\n", addr);

  lcd = new LiquidCrystal_I2C(addr, 16, 2);
  lcd->init();
  lcd->backlight();

  // Custom character: solid block used by the confidence bar.
  uint8_t block[8] = {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F};
  lcd->createChar(0xFF & 0x07, block);     // slot 7; we print char(0xFF)
  // NOTE: LiquidCrystal_I2C maps write(0xFF) to CGRAM slot 7 on HD44780.

  lcd_ready = true;
  paint(base_scr);
}

void lcd_display_update() {
  if (!lcd_ready || s_quiet) return;
  uint32_t now = millis();

  if (trans_active && trans_until != 0 && now >= trans_until) {
    trans_active = false;                  // event expired — back to base
    commit(Where::BASE);
    return;
  }

  // Spinner advances one character position per frame — no repaint.
  if (trans_active && cur_anim == Anim::SPINNER &&
      now - last_anim_ms >= LCD_ANIM_MS) {
    last_anim_ms = now;
    lcd->setCursor(15, 0);
    lcd->print(SPINNER[spinner_frame]);
    spinner_frame = (spinner_frame + 1) & 3;
  }
}

// Held states — the device's answer to "what are you waiting for?"
void lcd_disarmed()         { set_base("Ready", "SCAN to arm"); }
void lcd_armed()            { set_base("Listening...", "TAP NOW!"); }
void lcd_countdown(uint8_t sec) {
  char l2[17];
  snprintf(l2, sizeof(l2), "TAP in %us", (unsigned)sec);
  set_base("Place fruit", l2);
}
void lcd_place_fruit()      { set_base("Hold fruit up", "to camera..."); }
void lcd_place_on_piezo()   { set_base("Put fruit on", "piezo + SCAN"); }

// Transient events — always auto-revert to the held state above.
void lcd_connected()        { flash("BLE", "Connected", CUE_HOLD_MS); }
void lcd_disconnected()     { flash("BLE lost", nullptr, CUE_HOLD_MS); }
void lcd_tap_ok()           { set_screen("Tap captured!", "Analyzing...", RESULT_HOLD_MS, Anim::SPINNER); }
void lcd_result(const char* decision, bool is_anomaly, float confidence_0_100) {
  char l1[17];
  strncpy(l1, decision ? decision : "RESULT", 16); l1[16] = '\0';
  if (is_anomaly) set_screen(l1, "ANOMALY!", RESULT_HOLD_MS, Anim::BAR, 0.0f);
  else            set_screen(l1, "", RESULT_HOLD_MS, Anim::BAR,
                             confidence_0_100 / 100.0f);
}
void lcd_timeout()          { flash("No tap", "SCAN again", RESULT_HOLD_MS); }
void lcd_placement_timeout(){ flash("Waiting...", "Timed out", RESULT_HOLD_MS); }
void lcd_camera_error()     { flash("Camera error!", "SCAN to retry", RESULT_HOLD_MS); }
#endif
