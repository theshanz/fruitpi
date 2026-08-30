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

// ─── Cross-task safety ────────────────────────────────────────────────
// LCD state + the PCF8574 I2C device are touched from TWO tasks that run at
// the same time:
//   • the Arduino loop task — lcd_display_update() repaints/animates, and
//   • the NimBLE host task  — lcd_set_active_model()/lcd_idle() etc. when the
//     mobile app activates, deletes or uploads a model (bt_manager onWrite).
// Without a lock an app-driven model change races the loop's repaint, tearing
// the screen structs and corrupting the I2C transaction -> blank/gibberish
// display and a frozen menu. Every public lcd_*() now serializes on one mutex.
static SemaphoreHandle_t lcd_lock = nullptr;
static void lcd_lock_take() { if (lcd_lock) xSemaphoreTake(lcd_lock, portMAX_DELAY); }
static void lcd_lock_give() { if (lcd_lock) xSemaphoreGive(lcd_lock); }

// ─── Screen bookkeeping ──────────────────────────────────────────
struct Screen {
  char l1[17];
  char l2[17];
};

enum class Anim : uint8_t { NONE, SPINNER, BAR, MENU_CELL };

static Screen base_scr   = {"Fruitipi", "Ready - SCAN"};
static bool   base_is_idle = true;
static Screen trans_scr;
static bool   trans_active = false;
static uint32_t trans_until = 0;

static Anim      cur_anim = Anim::NONE;
static float     anim_value = 0.0f;        // 0..1 for BAR
static uint32_t  last_anim_ms = 0;
static uint8_t   spinner_frame = 0;
static bool s_quiet = false;               // piezo listening: freeze the bus

static const char SPINNER[4] = {'-', '/', char(0x5C), '|'};

static uint8_t  scan_pos = 0;            // idle "scanning" sweep position (0..SCAN_WIN_W-1)
static uint8_t menu_cell_col = 0;        // LCD column of the blinking bar cell
static bool    menu_cell_on = false;
static char    s_model_name[17] = {0};  // active model badge for idle screens

// Idle "scanning" animation: a small block sweeps left->right across the
// spare columns of line 2 (after "Press START"), implying ready + scanning.
#define SCAN_WIN_COL 11          // first column of the sweep window (11..15)
#define SCAN_WIN_W    5          // sweep width in columns

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
  base_is_idle = false;
  trans_active = false;                    // base replaces anything on screen
  cur_anim = Anim::NONE;
  commit(Where::BASE);
}

static void set_base_idle() {
  if (s_model_name[0]) {
    // Show just the active fruit — simple, no separator/truncation.
    strncpy(base_scr.l1, s_model_name, 16);  base_scr.l1[16] = '\0';
  } else {
    strncpy(base_scr.l1, "Fruitipi", 16);  base_scr.l1[16] = '\0';
  }
  strncpy(base_scr.l2, "Press START", 16); base_scr.l2[16] = '\0';
  base_is_idle = true;
  trans_active = false;
  cur_anim = Anim::NONE;
  commit(Where::BASE);
}

// Every end-of-cycle event lands here: base returns to idle FIRST, so no
// stale guide/listening screen can ever resurface under a transient.
static void finish_cycle(const char* l1, const char* l2, uint32_t hold_ms,
                         Anim anim = Anim::NONE, float value = 0.0f) {
  set_base_idle();
  set_screen(l1, l2, hold_ms, anim, value);
}

// Transient screens: shown now, revert to base automatically.
static void flash(const char* l1, const char* l2, uint32_t hold_ms) {
  set_screen(l1, l2, hold_ms);
}

// ─── Public API ──────────────────────────────────────────────────
// Every public entry point takes the mutex for the whole call. The internal
// helpers (set_screen/set_base/paint/commit/flash) are NEVER called directly
// from a task — only through a public lcd_*() — so there's no nesting.
void lcd_set_quiet(bool quiet) {
  lcd_lock_take();
  if (quiet == s_quiet) { lcd_lock_give(); return; }
  s_quiet = quiet;
  // Bus just went silent (piezo armed) or came back (capture done).
  // Repaint whatever belongs on screen now — the quiet window may have
  // swallowed requests or left a half-finished animation.
  if (!quiet) commit(trans_active ? Where::TRANSIENT : Where::BASE);
  lcd_lock_give();
}

void lcd_display_init() {
  if (!lcd_lock) lcd_lock = xSemaphoreCreateMutex();
  lcd_lock_take();
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
    lcd_lock_give();
    return;
  }
  Serial.printf("[LCD] Display found at 0x%02X\n", addr);

  lcd = new LiquidCrystal_I2C(addr, 16, 2);
  lcd->init();
  lcd->backlight();

  // Custom character: solid block used by the confidence bar + idle scan sweep.
  uint8_t block[8] = {0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F};
  lcd->createChar(0xFF & 0x07, block);     // slot 7; we print char(0xFF)
  // NOTE: LiquidCrystal_I2C maps write(0xFF) to CGRAM slot 7 on HD44780.

  lcd_ready = true;
  set_base_idle();
  lcd_lock_give();
}

void lcd_display_update() {
  lcd_lock_take();
  if (!lcd_ready || s_quiet) { lcd_lock_give(); return; }
  uint32_t now = millis();

  if (trans_active && trans_until != 0 && now >= trans_until) {
    trans_active = false;                  // event expired — back to base
    commit(Where::BASE);
    lcd_lock_give();
    return;
  }

  // Idle "scanning" animation: a block sweeps left->right across the spare
  // columns of line 2, implying the device is ready and scanning.
  if (!trans_active && base_is_idle) {
    uint32_t interval = 240U;                 // ~4 sweeps across 5 cols (plus wrap)
    if (now - last_anim_ms >= interval) {
      last_anim_ms = now;
      // Clear the previous block, then draw the next one.
      lcd->setCursor(SCAN_WIN_COL + scan_pos, 1);
      lcd->print(' ');
      scan_pos = (uint8_t)((scan_pos + 1) % SCAN_WIN_W);
      lcd->setCursor(SCAN_WIN_COL + scan_pos, 1);
      lcd->print(char(0xFF));
    }
    lcd_lock_give();
    return;
  }

  // Model-selector bar: the browsed cell pulses in place.
  if (!trans_active && cur_anim == Anim::MENU_CELL &&
      now - last_anim_ms >= LCD_ANIM_MS * 2) {
    last_anim_ms = now;
    menu_cell_on = !menu_cell_on;
    lcd->setCursor(menu_cell_col, 0);
    lcd->print(menu_cell_on ? char(0xFF) : '.');
  }

  // Spinner advances one character position per frame — no repaint.
  if (trans_active && cur_anim == Anim::SPINNER &&
      now - last_anim_ms >= LCD_ANIM_MS) {
    last_anim_ms = now;
    lcd->setCursor(15, 0);
    lcd->print(SPINNER[spinner_frame]);
    spinner_frame = (spinner_frame + 1) & 3;
  }
  lcd_lock_give();
}

// Held states — the device's answer to "what are you waiting for?"
void lcd_disarmed()         { lcd_lock_take(); set_base("Ready", "SCAN to arm"); lcd_lock_give(); }
void lcd_armed()            { lcd_lock_take(); set_base("Listening...", "TAP NOW!"); lcd_lock_give(); }
void lcd_countdown(uint8_t sec) {
  char l2[17];
  snprintf(l2, sizeof(l2), "TAP in %us", (unsigned)sec);
  lcd_lock_take(); set_base("Place fruit", l2); lcd_lock_give();
}
void lcd_place_fruit()      { lcd_lock_take(); set_base("Hold fruit up", "to camera..."); lcd_lock_give(); }

void lcd_menu(const char* name, uint8_t idx, uint8_t count, uint8_t active_idx) {
  lcd_lock_take();
  char l1[17];
  snprintf(l1, sizeof(l1), ">%s", name ? name : "?");
  l1[16] = '\0';

  cur_anim = Anim::NONE;
  if (count > 0 && count <= 6) {
    // Right-aligned information bar: block = active model, dot = idle,
    // the browsed cell pulses (handled by the MENU_CELL animation).
    const uint8_t first = 16 - count;
    for (uint8_t i = 0; i < count; i++)
      l1[first + i] = (i == active_idx) ? char(0xFF) : '.';
    if (idx < count) l1[first + idx] = '.';      // browsed cell starts hollow
    menu_cell_col = first + idx;
    cur_anim = Anim::MENU_CELL;
    menu_cell_on = true;
  } else if (count > 6) {
    snprintf(l1 + strlen(l1), 17 - strlen(l1), "%u/%u", idx + 1, count);
  }
  strncpy(base_scr.l1, l1, 16); base_scr.l1[16] = '\0';
  strncpy(base_scr.l2, (idx == active_idx) ? "Active, hold=use" : "Hold START: use", 16);
  base_scr.l2[16] = '\0';
  base_is_idle = false;
  trans_active = false;
  commit(Where::BASE);
  lcd_lock_give();
}

void lcd_ready_model(const char* name) {
  lcd_lock_take();
  strncpy(s_model_name, (name && name[0]) ? name : "", 16);
  s_model_name[16] = '\0';
  set_base_idle();
  lcd_lock_give();
}
void lcd_set_active_model(const char* name) {
  lcd_lock_take();
  strncpy(s_model_name, (name && name[0]) ? name : "", 16);
  s_model_name[16] = '\0';
  lcd_lock_give();
}
void lcd_idle() { lcd_lock_take(); set_base_idle(); lcd_lock_give(); }
void lcd_flash(const char* l1, const char* l2) {
  lcd_lock_take(); set_screen(l1, l2, CUE_HOLD_MS); lcd_lock_give();
}
void lcd_place_on_piezo()   { lcd_lock_take(); set_base("Put fruit on", "piezo + SCAN"); lcd_lock_give(); }

// Transient events — always auto-revert to the held state above.
void lcd_connected()        { lcd_lock_take(); flash("BLE", "Connected", CUE_HOLD_MS); lcd_lock_give(); }
void lcd_disconnected()     { lcd_lock_take(); flash("BLE lost", nullptr, CUE_HOLD_MS); lcd_lock_give(); }
void lcd_tap_ok()           { lcd_lock_take(); finish_cycle("Tap captured!", "Analyzing...", RESULT_HOLD_MS, Anim::SPINNER); lcd_lock_give(); }
void lcd_result(const char* decision, bool is_anomaly, float confidence_0_100) {
  lcd_lock_take();
  char l1[17];
  strncpy(l1, decision ? decision : "RESULT", 16); l1[16] = '\0';
  set_base_idle();                         // scan cycle over — base goes idle
  if (is_anomaly) set_screen(l1, "ANOMALY!", RESULT_HOLD_MS, Anim::BAR, 0.0f);
  else            set_screen(l1, "", RESULT_HOLD_MS, Anim::BAR,
                             confidence_0_100 / 100.0f);
  lcd_lock_give();
}
void lcd_timeout()          { lcd_lock_take(); finish_cycle("No tap", "SCAN again", RESULT_HOLD_MS); lcd_lock_give(); }
void lcd_placement_timeout(){ lcd_lock_take(); finish_cycle("Waiting...", "Timed out", RESULT_HOLD_MS); lcd_lock_give(); }
void lcd_camera_error()     { lcd_lock_take(); finish_cycle("Camera error!", "SCAN to retry", RESULT_HOLD_MS); lcd_lock_give(); }
#endif
