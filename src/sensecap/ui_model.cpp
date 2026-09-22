#include "ui_model.h"

#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
#include <time.h>

#include "districts_de.h"
#include "lage_db.h"
#include "meshtastic_proto.h"
#include "station_config.h"
#include "wall_clock.h"

// Hand-built mirror of ui/ui.yaml's main_menu -> {fire,police,ambulance,
// information}_menu -> emergency_confirmation flow, plus the chrome
// (status bar) and hardware-input model (context bar, hold_confirm) from
// ui/README.md Abschnitt 19. IDs/labels/parameters below must stay in
// sync with ui.yaml by hand for now.

namespace {

constexpr int32_t SCR = 480;
constexpr int32_t CHROME_TOP = 34;  // persistent status bar height
constexpr int32_t TITLE_H = 40;     // per-screen title band (when shown)
constexpr int32_t CTXBAR_H = 70;
constexpr int32_t TILE_W = 210;
constexpr int32_t TILE_H = 150;
constexpr int32_t TILE_GAP = 16;

constexpr uint32_t COLOR_PRIMARY_BLUE = 0x10537E;
constexpr uint32_t COLOR_GREEN = 0x62B22E;
constexpr uint32_t COLOR_TEAL = 0x04A098;
constexpr uint32_t COLOR_WARN_RED = 0xB22E2E;
constexpr uint32_t COLOR_CHROME_BG = 0x0A2E45;
constexpr uint32_t COLOR_DIM = 0x3A5A6E;

const unsigned long HOLD_CONFIRM_DURATION_MS = 3000;
const unsigned long INDICATOR_FLASH_MS = 400;

struct EmergencyChoice {
  const char *category;
  const char *type;
  const char *label;
};

// Plain aggregate on purpose (no default member initializers, no
// constructor): this core is built with an older C++ standard where a
// type with in-class initializers stops being an aggregate and
// `x = {...}` list-assignment no longer works. `CtxSlot ctx[3] = {};`
// still zero-initializes everything, and `ctx[i] = CtxSlot{...};` still
// works as aggregate-init-then-copy-assign.
struct CtxSlot {
  bool active;
  const char *symbol;
  const char *label;
  uint32_t color;
  lv_event_cb_t cb;
  void *user_data;
};

lv_obj_t *g_scr_main = nullptr;
lv_obj_t *g_scr_fire = nullptr;
lv_obj_t *g_scr_police = nullptr;
lv_obj_t *g_scr_ambulance = nullptr;
lv_obj_t *g_scr_info = nullptr;
lv_obj_t *g_scr_confirm = nullptr;
lv_obj_t *g_scr_situation = nullptr;
lv_obj_t *g_scr_crisis = nullptr;
lv_obj_t *g_scr_emergency_details = nullptr;
lv_obj_t *g_emergency_details_label = nullptr;
lv_obj_t *g_scr_transmission = nullptr;
lv_obj_t *g_transmission_label = nullptr;
lv_obj_t *g_transmission_bar = nullptr;
lv_obj_t *g_transmission_stage_label = nullptr;
// CLI-style per-step rows (Testprotokoll B2, 2026-09-18): "Verbindung",
// "Senden", "Bestaetigung" -- spinner icon while pending, green check once
// that step actually completed, red X if the whole thing ends in failure
// before a step got there.
lv_obj_t *g_transmission_row_icon[2] = {nullptr, nullptr};
lv_obj_t *g_scr_transmission_failed = nullptr;
lv_obj_t *g_transmission_failed_label = nullptr;
lv_obj_t *g_scr_history = nullptr;
lv_obj_t *g_scr_language = nullptr;
lv_obj_t *g_scr_pin_entry = nullptr;
lv_obj_t *g_pin_textarea = nullptr;
lv_obj_t *g_pin_error_label = nullptr;
lv_obj_t *g_scr_settings = nullptr;
lv_obj_t *g_settings_test_btn_label = nullptr;
lv_obj_t *g_settings_dispatch_ta = nullptr;
lv_obj_t *g_settings_location_ta = nullptr;
lv_obj_t *g_settings_clock_ta = nullptr;
lv_obj_t *g_settings_saved_label = nullptr;
lv_obj_t *g_settings_kb_preview = nullptr;

// --- Setup-Assistent (2026-09-23): erzwungen beim allerersten Start,
// danach ueber Einstellungen erneut aufrufbar (siehe station_config.h's
// setupCompleted-Kommentar). Ort-Auswahl zweistufig (Bundesland dann
// Landkreis) statt einer flachen 401-Eintraege-Liste -- Bayern allein hat
// 71 Landkreise/kreisfreie Staedte, das waere auf dem Touchscreen kaum
// noch scrollbar.
lv_obj_t *g_scr_setup_bundesland = nullptr;
lv_obj_t *g_scr_setup_landkreis = nullptr;
lv_obj_t *g_setup_landkreis_list = nullptr;
lv_obj_t *g_scr_setup_leitstelle = nullptr;
lv_obj_t *g_setup_leitstelle_ta = nullptr;
lv_obj_t *g_setup_leitstelle_kb_preview = nullptr;
lv_obj_t *g_setup_selected_ort_label = nullptr;

lv_obj_t *g_confirm_label = nullptr;
lv_obj_t *g_confirm_back_target = nullptr;
EmergencyChoice g_selected{};

// --- emergency_transmission state ---
// meshtastic_proto.cpp's meshtastic_send_emergency() call is real and
// synchronous (see start_transmission) and only tells us the radio locally
// accepted the send. Genuine delivery confirmation is a real ACK from the
// dispatch node, which can take a few seconds to come back over the air --
// see ui_model_tick()'s handling of g_transmission_pending, which now waits
// up to EMERGENCY_ACK_TIMEOUT_MS for meshtastic_proto_emergency_ack_received()
// instead of always finishing after a fixed delay. If the local send itself
// already failed (no point waiting for an ACK that was never requested),
// TRANSMISSION_LOCAL_FAIL_DELAY_MS is used instead -- just long enough for
// "senden..." to be visible before showing the failure, like before.
const unsigned long TRANSMISSION_LOCAL_FAIL_DELAY_MS = 2000;
const unsigned long EMERGENCY_ACK_TIMEOUT_MS = 8000;
bool g_transmission_pending = false;
unsigned long g_transmission_started_at = 0;
int g_transmission_db_id = -1;

// Stage narration for the sending screen: real steps (encode/encrypt already
// happened by the time this screen even shows, radio send happened
// synchronously in start_transmission(), then we wait on the real ACK) told
// at a pace a human can read instead of flashing past instantly. This is
// pacing for an operation that already happened/is happening, not a fake
// countdown pretending toward an outcome we don't know yet — the actual
// result still only ever comes from g_last_send_ok / the real ACK.
int g_transmission_stage = 0;
// Row 0 ("Notmeldung gesendet") is set directly in start_transmission() --
// g_last_send_ok is already a known, real fact by the time this screen
// loads, no reason to fake-pace revealing it. Row 1 ("Von der Leitstelle
// bestaetigt") is the only one this function still drives, and only ever
// from a real signal: stage>=4 = genuine application ACK received,
// stage==5 = the wait timed out with nothing.
//
// There used to be a 3rd, middle row ("Verbindung zur Leitstelle") that
// checked off purely because a fixed pacing delay had elapsed -- not
// because anything about the dispatch link was actually confirmed. Found
// live (2026-09-18) that this looked outright contradictory: "Verbindung"
// shown as done right before the whole send reported failure. Removed --
// Meshtastic/LoRa has no real "connection established" step to report in
// the first place, only "sent" and "acknowledged".
void set_transmission_stage(int stage, const char *text, int barPercent) {
  g_transmission_stage = stage;
  if (g_transmission_stage_label) lv_label_set_text(g_transmission_stage_label, text);
  if (g_transmission_bar) lv_bar_set_value(g_transmission_bar, barPercent, LV_ANIM_ON);

  auto setIcon = [](int row, const char *symbol, uint32_t color) {
    if (!g_transmission_row_icon[row]) return;
    lv_label_set_text(g_transmission_row_icon[row], symbol);
    lv_obj_set_style_text_color(g_transmission_row_icon[row], lv_color_hex(color), 0);
  };
  if (stage >= 4) setIcon(1, LV_SYMBOL_OK, COLOR_GREEN);
  if (stage == 5) setIcon(1, LV_SYMBOL_CLOSE, COLOR_WARN_RED);
}

// Whether we have a real Meshtastic link. Set by main.cpp's setup() once
// lora_radio_begin() + meshtastic_proto_begin() both succeed (real onboard
// radio, initialized and listening on the default channel) — drives the
// status bar's ONLINE/OFFLINE dot. A "testconnect on/off" serial command
// (main.cpp) can override it for manual QA of the success screen without
// the UI silently lying about connectivity by default.
bool g_meshtastic_connected = false;

// --- hold_confirm state (single long-press button, 3s — see
// confirm_btn_press_cb for why this isn't the two-slot-held-together
// gesture ui.yaml originally described) ---
// Circular ring instead of the old linear bar, plus a warning-color
// background pulse while holding (Testprotokoll B1, 2026-09-18: from
// some angles it wasn't obvious the hold had even started).
lv_obj_t *g_hold_arc = nullptr;
lv_obj_t *g_hold_pct_label = nullptr;
lv_obj_t *g_confirm_circle_btn = nullptr;
unsigned long g_hold_started_at = 0;
bool g_hold_in_progress = false;
// True for the brief pause between "held long enough" and actually
// triggering -- long enough for the eye to register a clean 100%/check
// instead of jumping straight to the next screen (Testprotokoll B1
// follow-up, 2026-09-18: felt like the ring "wasn't working" otherwise).
bool g_hold_completing = false;
constexpr uint32_t COLOR_CONFIRM_BTN = COLOR_GREEN; // g_confirm_circle_btn's resting color

// --- status bar ---
// Tried this as a single lv_layer_top() overlay shared by every screen —
// broke rendering badly (only the status bar's own pixels ever showed,
// rest of the screen went blank/stale). LVGL's partial-refresh dirty-
// rect tracking doesn't reliably composite a changing top-layer object
// over independently-changing screen content underneath it. Simpler and
// provenly-correct: build the same status bar as an ordinary child of
// *every* screen (see make_screen()) and update all copies in lockstep.
struct StatusBarWidgets {
  lv_obj_t *time_label;
  lv_obj_t *tx_dot;
  lv_obj_t *rx_dot;
  lv_obj_t *power_dot;
  lv_obj_t *power_lbl;
};
constexpr int MAX_SCREENS = 16; // 12 screens built today, headroom for more
lv_obj_t *g_status_bar_screens[MAX_SCREENS]; // parallel to g_status_bars[]
StatusBarWidgets g_status_bars[MAX_SCREENS];
int g_status_bar_count = 0;
// Last-applied tx/rx/connected state per status bar copy -- -1 means
// "never set yet" so the very first tick always applies once. Needed
// because of full_refresh=1 (see main.cpp's init_lvgl comment): every
// lv_obj_set_style_bg_color() call invalidates and triggers a full-screen
// flush regardless of whether the color is actually changing, and this
// ran unconditionally on EVERY tick (dozens/sec) even when nothing about
// tx/rx/connectivity had changed -- a likely major contributor to the
// flicker reported live (2026-09-18), continuously, not just during any
// one animation.
int8_t g_last_tx_on[MAX_SCREENS];
int8_t g_last_rx_on[MAX_SCREENS];
int8_t g_last_connected[MAX_SCREENS];
unsigned long g_last_tx_flash = 0;
unsigned long g_last_rx_flash = 0;
unsigned long g_last_clock_update = 0;

void build_status_bar_on(lv_obj_t *scr); // defined below; used by make_screen()

// Tried lv_scr_load_anim(FADE_ON) here to smooth out the transition
// glitch — made it much worse (many more partial redraws means many
// more chances for our un-synchronized direct-framebuffer flush to tear
// mid-scanout). Back to a plain, single instant load until the flush
// path has real double buffering.
void nav_to(lv_obj_t *target) {
  if (!target) return;
  lv_scr_load(target);
}

// Buttons kept their "pressed" highlight after navigating away because
// the screen swap happens before LVGL's own release/state-clear step
// reaches the tapped object (the object persists — screens are never
// deleted — so the stale state was still visible next time it was shown).
void clear_pressed(lv_obj_t *obj) {
  if (obj) lv_obj_clear_state(obj, LV_STATE_PRESSED | LV_STATE_FOCUSED);
}

// bg_color defaults to the neutral primary blue; callers pass a different
// one to visually mark which area of the app a screen belongs to
// (Testprotokoll A2, 2026-09-18: "unterschiedliche Hintergrundfarben pro
// Bereich" -- the existing slight layout shift between screens wasn't
// distinctive enough on its own).
lv_obj_t *make_screen(const char *title, uint32_t bg_color = COLOR_PRIMARY_BLUE) {
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(bg_color), 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  build_status_bar_on(scr);

  if (title) {
    lv_obj_t *title_lbl = lv_label_create(scr);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, CHROME_TOP + 8);
  }
  return scr;
}

int32_t content_top(bool has_title) {
  return has_title ? CHROME_TOP + TITLE_H : CHROME_TOP + 10;
}

void style_tile(lv_obj_t *btn) {
  lv_obj_set_style_radius(btn, 14, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(COLOR_TEAL), 0);
  lv_obj_set_style_border_width(btn, 2, 0);
}

// slot: 0=top_left(sel_1) 1=top_right(sel_2) 2=bottom_left(sel_3) 3=bottom_right(sel_4)
void position_tile(lv_obj_t *btn, int slot, int32_t grid_top) {
  int col = slot % 2;
  int row = slot / 2;
  int32_t x = 20 + col * (TILE_W + TILE_GAP);
  int32_t y = grid_top + row * (TILE_H + TILE_GAP);
  lv_obj_set_size(btn, TILE_W, TILE_H);
  lv_obj_set_pos(btn, x, y);
}

// Icons are LVGL's built-in symbol glyphs, not real pictograms — there's
// no built-in fire-truck/police-badge/ambulance-cross glyph set. Good
// enough to give kids a consistent shape+color to recognize per screen;
// real custom icon assets are a follow-up (see chat/README notes).
lv_obj_t *add_tile(lv_obj_t *scr, int slot, int32_t grid_top, const char *symbol, const char *label) {
  lv_obj_t *btn = lv_btn_create(scr);
  style_tile(btn);
  position_tile(btn, slot, grid_top);

  lv_obj_t *col = lv_obj_create(btn);
  lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 4, 0);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *icon = lv_label_create(col);
  lv_label_set_text(icon, symbol);
  lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_PRIMARY_BLUE), 0);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);

  lv_obj_t *lbl = lv_label_create(col);
  lv_label_set_text(lbl, label);
  lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_PRIMARY_BLUE), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

  return btn;
}

// Always lays out a full-width 3-slot band, so every screen visually
// reads as "the same 3 context buttons" even where only 1-2 are active
// (see ui/README.md Abschnitt 19: ctx_1/ctx_2/ctx_3 are fixed hardware
// roles). Inactive slots stay empty rather than getting a fake button.
void build_context_bar(lv_obj_t *scr, const CtxSlot slots[3]) {
  lv_obj_t *band = lv_obj_create(scr);
  lv_obj_set_size(band, SCR, CTXBAR_H);
  lv_obj_set_pos(band, 0, SCR - CTXBAR_H);
  lv_obj_set_style_bg_color(band, lv_color_hex(COLOR_CHROME_BG), 0);
  lv_obj_set_style_border_width(band, 0, 0);
  lv_obj_set_style_radius(band, 0, 0);
  lv_obj_clear_flag(band, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);

  int32_t w = (SCR - 40) / 3 - 10;
  for (int i = 0; i < 3; i++) {
    if (!slots[i].active) continue;
    int32_t x = 20 + i * (w + 10);

    lv_obj_t *btn = lv_btn_create(band);
    lv_obj_set_size(btn, w, CTXBAR_H - 20);
    lv_obj_set_pos(btn, x, 10);
    lv_obj_set_style_bg_color(btn, lv_color_hex(slots[i].color), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    if (slots[i].cb) lv_obj_add_event_cb(btn, slots[i].cb, LV_EVENT_CLICKED, slots[i].user_data);

    lv_obj_t *row = lv_obj_create(btn);
    lv_obj_set_size(row, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);

    if (slots[i].symbol) {
      lv_obj_t *icon = lv_label_create(row);
      lv_label_set_text(icon, slots[i].symbol);
    }
    if (slots[i].label) {
      lv_obj_t *lbl = lv_label_create(row);
      lv_label_set_text(lbl, slots[i].label);
    }
  }
}

// ---------------------------------------------------------------------
// Navigation / action callbacks
// ---------------------------------------------------------------------

void nav_cb(lv_event_t *e) {
  clear_pressed(lv_event_get_target(e));
  lv_obj_t *target = (lv_obj_t *)lv_event_get_user_data(e);
  nav_to(target);
}

// Back to "not holding" -- ring at 0, button back to its resting color.
// Shared by every place that needs to cancel/reset the hold gesture.
void reset_hold_visual() {
  if (g_hold_arc) lv_arc_set_value(g_hold_arc, 0);
  if (g_hold_pct_label) lv_label_set_text(g_hold_pct_label, LV_SYMBOL_OK);
  if (g_confirm_circle_btn) lv_obj_set_style_bg_color(g_confirm_circle_btn, lv_color_hex(COLOR_CONFIRM_BTN), 0);
}

void emergency_select_cb(lv_event_t *e) {
  clear_pressed(lv_event_get_target(e));
  auto *choice = (EmergencyChoice *)lv_event_get_user_data(e);
  g_selected = *choice;
  g_confirm_back_target = lv_scr_act();
  lv_label_set_text_fmt(
      g_confirm_label,
      // Simplified per Testprotokoll B1 (2026-09-18): fewer words, plain
      // language -- the legal warning stays (real safety requirement,
      // not just decoration), the "how to confirm" instruction moves
      // below the ring instead of being crammed in here too.
      "%s melden\n\n"
      "ACHTUNG: Geht sofort an die Leitstelle.\n"
      "Nur bei echtem Notfall!\n"
      "Falschalarm ist strafbar.",
      choice->label);
  g_hold_in_progress = false;
  reset_hold_visual();
  nav_to(g_scr_confirm);
}

void confirm_cancel_cb(lv_event_t *e) {
  clear_pressed(lv_event_get_target(e));
  g_hold_in_progress = false;
  reset_hold_visual();
  if (g_confirm_back_target) nav_to(g_confirm_back_target);
}

// One-line timestamp for display: real wall-clock if it's been set
// (see wall_clock.h), otherwise honestly labelled uptime.
String format_timestamp() {
  if (wallClockIsSet()) return wallClockNowHMS();
  unsigned long s = millis() / 1000;
  char buf[40];
  snprintf(buf, sizeof(buf), "Laufzeit %02lu:%02lu:%02lu (Uhrzeit nicht gestellt)",
           (s / 3600) % 24, (s / 60) % 60, s % 60);
  return String(buf);
}

// Set by the real meshtastic_send_emergency() call in start_transmission:
// whether the radio locally accepted the send, NOT whether the dispatch
// node actually received it. See ui_model_tick()'s handling of
// g_transmission_pending for how the real ACK wait works.
bool g_last_send_ok = false;

void start_transmission(const char *sending_verb) {
  lv_label_set_text_fmt(g_transmission_label, "%s\n\n" LV_SYMBOL_LOOP " %s ...",
                        g_selected.label, sending_verb);
  // Retry after a previous failure would otherwise still show that
  // attempt's checkmarks/X -- these screens are reused, never rebuilt.
  for (lv_obj_t *icon : g_transmission_row_icon) {
    if (!icon) continue;
    lv_label_set_text(icon, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_color(icon, lv_color_hex(COLOR_DIM), 0);
  }

  // Real, already-known fact by the time this line runs (the radio call
  // is synchronous) -- shown immediately, not paced, and never flips back
  // (see set_transmission_stage()'s comment on why the old middle
  // "Verbindung"-row was removed).
  g_last_send_ok = meshtastic_send_emergency(g_selected.category, g_selected.type, g_selected.label);
  if (g_transmission_row_icon[0]) {
    lv_label_set_text(g_transmission_row_icon[0], g_last_send_ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(g_transmission_row_icon[0], lv_color_hex(g_last_send_ok ? COLOR_GREEN : COLOR_WARN_RED),
                                0);
  }
  set_transmission_stage(1, g_last_send_ok ? "Warte auf Bestaetigung der Leitstelle..." : "Senden fehlgeschlagen.",
                          g_last_send_ok ? 50 : 100);
  g_transmission_pending = true;
  g_transmission_started_at = millis();
  nav_to(g_scr_transmission);
}

void do_trigger_emergency() {
  Serial.printf("[UI] trigger_emergency: category=%s type=%s label=%s\n",
                g_selected.category, g_selected.type, g_selected.label);
  g_hold_in_progress = false;
  reset_hold_visual();

  g_transmission_db_id = lageDbCreate(g_selected.category, "wird uebermittelt",
                                      g_selected.label, "!lokal-touch");
  start_transmission("Meldung wird gesendet");
}

void retry_transmission_cb(lv_event_t *e) {
  clear_pressed(lv_event_get_target(e));
  if (g_transmission_db_id >= 0) {
    lageDbUpdate(g_transmission_db_id, g_selected.category, "wird uebermittelt",
                 g_selected.label, "!lokal-touch");
  }
  start_transmission("Meldung wird erneut gesendet");
}

void transmission_failed_cancel_cb(lv_event_t *e) {
  clear_pressed(lv_event_get_target(e));
  nav_to(g_scr_main);
}

void finish_transmission(bool success) {
  // Issue #33: die Aktivierung/Meldung selbst gehoert in die Betriebshistorie,
  // unabhaengig davon ob die Leitstelle sie am Ende bestaetigt hat oder nicht.
  {
    char msg[96];
    snprintf(msg, sizeof(msg), "Notmeldung VG-%04d (%s) %s", g_transmission_db_id, g_selected.category,
             success ? "zugestellt" : "fehlgeschlagen");
    eventLog(success ? "aktivierung" : "fehler", msg);
  }
  if (success) {
    lageDbUpdate(g_transmission_db_id, g_selected.category, "uebermittelt",
                 g_selected.label, "!lokal-touch");
    // Ort: manuell in den Einstellungen gesetzt (kein GPS auf diesem Board,
    // siehe station_config.h) -- Testprotokoll B3, 2026-09-18.
    String ort = station_config().locationText;
    if (ort.length() == 0) ort = "unbekannt (kein GPS, nicht in Einstellungen gesetzt)";
    lv_label_set_text_fmt(
        g_emergency_details_label,
        "%s\n\n"
        "Vorgangsnummer: VG-%04d\n"
        "Zeitstempel: %s\n"
        "Notfallsaeule: %s\n"
        "Ort: %s\n\n"
        "Bitte notieren.",
        g_selected.label, g_transmission_db_id, format_timestamp().c_str(),
        station_config().stationId.c_str(), ort.c_str());
    nav_to(g_scr_emergency_details);
  } else {
    lageDbUpdate(g_transmission_db_id, g_selected.category, "fehlgeschlagen",
                 g_selected.label, "!lokal-touch");
    lv_label_set_text_fmt(
        g_transmission_failed_label,
        "%s\n\n"
        LV_SYMBOL_WARNING " Uebertragung fehlgeschlagen.\n\n"
        "Bitte erneut versuchen. Wenn das Problem\n"
        "bestehen bleibt, lokalen Kontakt kontaktieren:\n\n"
        "%s (%s)",
        g_selected.label, station_config().operatorName.c_str(),
        station_config().localContact.c_str());
    nav_to(g_scr_transmission_failed);
  }
}

// hold_confirm, single-touch version: this panel's touch controller only
// ever reports one point at a time (confirmed on real hardware — no
// multitouch), so the original ui.yaml idea of holding two context slots
// at once isn't achievable here. Falls back to a single long-press
// button instead, using LVGL's normal PRESSED/PRESSING/RELEASED events
// on the one indev — no raw touch polling needed for this anymore.
// One-shot: give the eye ~250ms to register a clean, complete ring before
// swapping to the next screen (see g_hold_completing's comment).
void finish_hold_timer_cb(lv_timer_t *t) {
  g_hold_completing = false;
  lv_timer_del(t);
  do_trigger_emergency();
}

void confirm_btn_press_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);

  if (code == LV_EVENT_PRESSED) {
    if (g_hold_completing) return; // already committed, ignore a stray re-press
    g_hold_in_progress = true;
    g_hold_started_at = millis();
    if (g_hold_arc) lv_arc_set_value(g_hold_arc, 0);
  } else if (code == LV_EVENT_PRESSING && g_hold_in_progress && !g_hold_completing) {
    unsigned long held = millis() - g_hold_started_at;
    int32_t pct = (int32_t)((held * 100) / HOLD_CONFIRM_DURATION_MS);
    if (pct > 100) pct = 100;

    // Found live (2026-09-18) that even a throttled sine pulse was still
    // visibly juddery on this panel, and felt too hectic besides. All
    // per-frame visual work (not just the color) is now bundled into one
    // slow, calm cadence -- ~6/s, plenty for a 3s gesture -- instead of
    // firing at full touch-poll rate. Colors swap slowly between blue and
    // red like an emergency vehicle's lights ("Blaulicht") rather than a
    // fast blend, and only the small circular button repaints, not the
    // screen.
    static unsigned long lastUpdateAt = 0;
    if (millis() - lastUpdateAt >= 150) {
      lastUpdateAt = millis();
      if (g_hold_arc) lv_arc_set_value(g_hold_arc, pct);
      if (g_hold_pct_label) lv_label_set_text_fmt(g_hold_pct_label, "%d%%", (int)pct);

      const unsigned long kPeriodMs = 1800;
      unsigned long phase = held % kPeriodMs;
      float t = phase < kPeriodMs / 2 ? (float)phase / (kPeriodMs / 2) : 2.0f - (float)phase / (kPeriodMs / 2);
      float mixFactor = ((float)pct / 100.0f) * t; // ramps in as the hold progresses, not from the first frame
      lv_color_t bg = lv_color_mix(lv_color_hex(COLOR_WARN_RED), lv_color_hex(COLOR_PRIMARY_BLUE),
                                   (uint8_t)((1.0f - mixFactor) * 255));
      if (g_confirm_circle_btn) lv_obj_set_style_bg_color(g_confirm_circle_btn, bg, 0);
    }

    if (held >= HOLD_CONFIRM_DURATION_MS) {
      g_hold_completing = true;
      if (g_hold_arc) lv_arc_set_value(g_hold_arc, 100);
      if (g_hold_pct_label) lv_label_set_text(g_hold_pct_label, LV_SYMBOL_OK);
      if (g_confirm_circle_btn) lv_obj_set_style_bg_color(g_confirm_circle_btn, lv_color_hex(COLOR_GREEN), 0);
      lv_timer_t *t = lv_timer_create(finish_hold_timer_cb, 250, nullptr);
      lv_timer_set_repeat_count(t, 1);
    }
  } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && !g_hold_completing) {
    g_hold_in_progress = false;
    reset_hold_visual();
  }
}

// ---------------------------------------------------------------------
// Screen builders
// ---------------------------------------------------------------------

lv_obj_t *build_department_menu(const char *title, const char *symbol,
                                 const EmergencyChoice items[4], lv_obj_t *back_target,
                                 uint32_t bg_color) {
  static EmergencyChoice storage[3][4]; // fire, police, ambulance — 3 calls total
  static int menu_index = 0;
  int row = menu_index++;

  lv_obj_t *scr = make_screen(title, bg_color);
  int32_t top = content_top(true);
  for (int i = 0; i < 4; i++) {
    storage[row][i] = items[i];
    // Sub-items share the department's icon (see add_tile comment above):
    // per-item pictograms (Brand vs. Chemie vs. ...) don't have a good
    // built-in glyph, and a wrong-looking icon would confuse more than
    // plain text would.
    lv_obj_t *tile = add_tile(scr, i, top, symbol, items[i].label);
    lv_obj_add_event_cb(tile, emergency_select_cb, LV_EVENT_CLICKED, &storage[row][i]);
  }

  CtxSlot ctx[3] = {};
  ctx[1] = CtxSlot{true, LV_SYMBOL_LEFT, "Zurueck", COLOR_GREEN, nav_cb, back_target};
  build_context_bar(scr, ctx);
  return scr;
}

// Honest "not connected yet" screen (Testprotokoll C2, 2026-09-18): the
// Krisenstab/Systeminfo page had been quietly reusing the Lagemeldungen
// feed, showing this station's own data mislabeled as crisis-staff system
// info. There is no real data source for that yet -- per this project's
// "never claim more than is real" rule, say so plainly instead of faking
// content just to make the page look populated.
lv_obj_t *build_not_connected_page(const char *title, const char *reason, lv_obj_t *back_target,
                                    uint32_t bg_color = COLOR_PRIMARY_BLUE) {
  lv_obj_t *scr = make_screen(title, bg_color);
  int32_t top = content_top(true);

  lv_obj_t *box = lv_obj_create(scr);
  lv_obj_set_size(box, SCR - 40, SCR - CTXBAR_H - top - 10);
  lv_obj_set_pos(box, 20, top);
  lv_obj_set_style_bg_color(box, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_radius(box, 10, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *label = lv_label_create(box);
  lv_label_set_text_fmt(label, LV_SYMBOL_WARNING " Noch nicht angebunden.\n\n%s", reason);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(label, LV_PCT(90));
  lv_obj_center(label);

  CtxSlot ctx[3] = {};
  ctx[1] = CtxSlot{true, LV_SYMBOL_LEFT, "Zurueck", COLOR_GREEN, nav_cb, back_target};
  build_context_bar(scr, ctx);
  return scr;
}

// Whether a lagemeldungen row is a genuine, externally received report vs.
// this station's own doing (Testprotokoll C1, 2026-09-18: the page was
// showing the station's own triggered reports -- and leftover demo/seed
// rows on already-flashed devices -- as if they were "empfangene
// offizielle Meldungen"). Real received reports get a "!<hex-node-id>"
// fromNode from handleTextMessage() in meshtastic_proto.cpp.
bool isGenuinelyReceived(const LageMeldungSummary &row) {
  return !row.fromNode.startsWith("!lokal-touch") && !row.fromNode.startsWith("!seed");
}

lv_obj_t *build_info_list_page(const char *title, bool onlyReceived, lv_obj_t *back_target) {
  lv_obj_t *scr = make_screen(title);
  int32_t top = content_top(true);

  lv_obj_t *list = lv_obj_create(scr);
  lv_obj_set_size(list, SCR - 40, SCR - CTXBAR_H - top - 10);
  lv_obj_set_pos(list, 20, top);
  lv_obj_set_style_bg_color(list, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_radius(list, 10, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(list, 10, 0);
  lv_obj_set_style_pad_row(list, 8, 0);
  // No momentum/scroll-animation on drag — Hoch/Runter buttons drive it
  // instantly instead (see the context bar below); one less animated
  // thing fighting the un-synchronized flush.
  lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

  // Filtering happens here in C++ after a plain fetch, NOT via a SQL
  // WHERE clause -- found live (2026-09-18) that adding one made
  // sqlite3_step() fail with a disk I/O error on this platform (see
  // lageDbGetRecentSummaries()'s comment in lage_db.cpp). To still avoid
  // the earlier windowing bug (fetching only 10 rows total let a busy
  // station's own local Notmeldungen crowd out genuinely received ones
  // before filtering ever got a chance), the raw fetch is deliberately
  // much larger than what's actually displayed.
  constexpr int kRawFetch = 50;
  constexpr int kMaxDisplayed = 10;
  static LageMeldungSummary rawRows[kRawFetch];
  int rawCount = lageDbGetRecentSummaries(rawRows, kRawFetch);

  LageMeldungSummary rows[kMaxDisplayed];
  int shown = 0;
  for (int i = 0; i < rawCount && shown < kMaxDisplayed; i++) {
    if (onlyReceived && !isGenuinelyReceived(rawRows[i])) continue;
    rows[shown++] = rawRows[i];
  }
  bool any = shown > 0;
  for (int i = 0; i < shown; i++) {
    lv_obj_t *card = lv_obj_create(list);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xF0F4F6), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Single recolored label instead of a separate header+body label pair
    // -- fixes the pixelated "#"/digit rendering reported live on this
    // page (2026-09-18); the same fix already proven on
    // build_history_page() below.
    time_t t = (time_t)rows[i].updatedAt;
    struct tm tmv;
    localtime_r(&t, &tmv);
    char timeBuf[20];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    lv_obj_t *entry = lv_label_create(card);
    lv_label_set_recolor(entry, true);
    lv_label_set_text_fmt(entry, "#10537e %s  %s / %s#  (%s)\n%s", timeBuf, rows[i].kategorie.c_str(),
                          rows[i].status.c_str(), rows[i].fromNode.c_str(), rows[i].text.c_str());
    lv_obj_set_style_text_font(entry, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(entry, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(entry, LV_PCT(100));
  }
  if (!any) {
    lv_obj_t *empty = lv_label_create(list);
    lv_label_set_text(empty, onlyReceived ? "Keine empfangenen Meldungen vorhanden."
                                           : "Keine Lagemeldungen vorhanden.");
  }

  CtxSlot ctx[3] = {};
  ctx[0] = CtxSlot{true, LV_SYMBOL_UP, "Hoch", COLOR_TEAL, [](lv_event_t *e) {
               lv_obj_t *l = (lv_obj_t *)lv_event_get_user_data(e);
               lv_obj_scroll_by(l, 0, 80, LV_ANIM_OFF);
             }, list};
  ctx[1] = CtxSlot{true, LV_SYMBOL_LEFT, "Zurueck", COLOR_GREEN, nav_cb, back_target};
  ctx[2] = CtxSlot{true, LV_SYMBOL_DOWN, "Runter", COLOR_TEAL, [](lv_event_t *e) {
               lv_obj_t *l = (lv_obj_t *)lv_event_get_user_data(e);
               lv_obj_scroll_by(l, 0, -80, LV_ANIM_OFF);
             }, list};
  build_context_bar(scr, ctx);
  return scr;
}

struct HistoryCategory {
  const char *key;
  const char *label;
  const char *icon;
};
const HistoryCategory kHistoryCategories[] = {
    {"fire_department", "Feuerwehr", LV_SYMBOL_WARNING},
    {"police", "Polizei", LV_SYMBOL_EYE_OPEN},
    {"ambulance", "Krankenwagen", LV_SYMBOL_PLUS},
};

// Notmeldungshistorie: every lage_db entry (both locally triggered ones
// and anything a future Meshtastic bridge writes in) grouped under a
// folder header per "Ereignis" (the department/category it belongs to).
lv_obj_t *build_history_page(lv_obj_t *back_target) {
  lv_obj_t *scr = make_screen("Notmeldungshistorie");
  int32_t top = content_top(true);

  lv_obj_t *list = lv_obj_create(scr);
  lv_obj_set_size(list, SCR - 40, SCR - CTXBAR_H - top - 10);
  lv_obj_set_pos(list, 20, top);
  lv_obj_set_style_bg_color(list, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_radius(list, 10, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(list, 10, 0);
  lv_obj_set_style_pad_row(list, 8, 0);
  lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

  LageMeldungSummary rows[20];
  int count = lageDbGetRecentSummaries(rows, 20);
  bool any = false;

  for (const HistoryCategory &cat : kHistoryCategories) {
    bool has_entry = false;
    for (int i = 0; i < count; i++) {
      if (rows[i].kategorie == cat.key) { has_entry = true; break; }
    }
    if (!has_entry) continue;
    any = true;

    lv_obj_t *folder = lv_label_create(list);
    lv_label_set_text_fmt(folder, "%s  %s", cat.icon, cat.label);
    lv_obj_set_style_text_color(folder, lv_color_hex(COLOR_PRIMARY_BLUE), 0);
    lv_obj_set_style_text_font(folder, &lv_font_montserrat_18, 0);

    for (int i = 0; i < count; i++) {
      if (rows[i].kategorie != cat.key) continue;
      lv_obj_t *card = lv_obj_create(list);
      lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
      lv_obj_set_style_bg_color(card, lv_color_hex(0xF0F4F6), 0);
      lv_obj_set_style_radius(card, 8, 0);
      lv_obj_set_style_pad_all(card, 8, 0);
      lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
      // C3's headline+subline (below) are two separate label children --
      // without an explicit layout they both default to position (0,0)
      // and render stacked directly on top of each other. Found live
      // (2026-09-18, "steht noch komplett uebereinander").
      lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

      // Single label per entry (was card+header+body, 3 LVGL objects) --
      // fewer widgets to lay out/redraw per entry, which is the leading
      // suspect for the pixelation reported on this page (2026-09-18 live
      // test) given simpler screens elsewhere never showed it.
      time_t t = (time_t)rows[i].updatedAt;
      struct tm tmv;
      localtime_r(&t, &tmv);
      char timeBuf[20];
      snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
               tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

      // Reordered per Testprotokoll C3 (2026-09-18): time + Art der Meldung
      // up front and prominent, clear checkmark/X for the outcome, bigger
      // text using the full line width. VG number + raw status demoted to
      // a smaller secondary line.
      bool failed = rows[i].status.indexOf("fehlgeschlagen") >= 0;
      bool succeeded = !failed && (rows[i].status.indexOf("uebermittelt") >= 0 || rows[i].status.indexOf("erledigt") >= 0);
      const char *mark = succeeded ? "  #62b22e " LV_SYMBOL_OK "#" : failed ? "  #b22e2e " LV_SYMBOL_CLOSE "#" : "";

      lv_obj_t *headline = lv_label_create(card);
      lv_label_set_recolor(headline, true);
      lv_label_set_text_fmt(headline, "#10537e %s  %s/%s#%s", timeBuf, cat.label, rows[i].text.c_str(), mark);
      lv_obj_set_style_text_font(headline, &lv_font_montserrat_18, 0);
      lv_label_set_long_mode(headline, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(headline, LV_PCT(100));

      lv_obj_t *subline = lv_label_create(card);
      lv_label_set_text_fmt(subline, "VG-%04d - %s", rows[i].id, rows[i].status.c_str());
      lv_obj_set_style_text_font(subline, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_color(subline, lv_color_hex(COLOR_DIM), 0);
      lv_obj_set_width(subline, LV_PCT(100));
    }
  }
  if (!any) {
    lv_obj_t *empty = lv_label_create(list);
    lv_label_set_text(empty, "Keine Notmeldungen vorhanden.");
  }

  CtxSlot ctx[3] = {};
  ctx[0] = CtxSlot{true, LV_SYMBOL_UP, "Hoch", COLOR_TEAL, [](lv_event_t *e) {
               lv_obj_t *l = (lv_obj_t *)lv_event_get_user_data(e);
               lv_obj_scroll_by(l, 0, 80, LV_ANIM_OFF);
             }, list};
  ctx[1] = CtxSlot{true, LV_SYMBOL_LEFT, "Zurueck", COLOR_GREEN, nav_cb, back_target};
  ctx[2] = CtxSlot{true, LV_SYMBOL_DOWN, "Runter", COLOR_TEAL, [](lv_event_t *e) {
               lv_obj_t *l = (lv_obj_t *)lv_event_get_user_data(e);
               lv_obj_scroll_by(l, 0, -80, LV_ANIM_OFF);
             }, list};
  build_context_bar(scr, ctx);
  return scr;
}

// ---------------------------------------------------------------------
// Status bar — built as a normal child of each screen (see make_screen).
// ---------------------------------------------------------------------

void build_status_bar_on(lv_obj_t *scr) {
  lv_obj_t *bar = lv_obj_create(scr);
  lv_obj_set_size(bar, SCR, CHROME_TOP);
  lv_obj_set_pos(bar, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_CHROME_BG), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_all(bar, 0, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *station = lv_label_create(bar);
  lv_label_set_text(station, station_config().stationId.c_str());
  lv_obj_set_style_text_color(station, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(station, &lv_font_montserrat_14, 0);
  lv_obj_align(station, LV_ALIGN_LEFT_MID, 10, 0);

  // Driven by g_meshtastic_connected (updated in ui_model_tick) — always
  // false/red/OFFLINE until a real Meshtastic bridge exists to report a
  // real link state.
  lv_obj_t *power_dot = lv_obj_create(bar);
  lv_obj_set_size(power_dot, 10, 10);
  lv_obj_set_style_radius(power_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(power_dot, lv_color_hex(COLOR_WARN_RED), 0);
  lv_obj_set_style_border_width(power_dot, 0, 0);
  lv_obj_align(power_dot, LV_ALIGN_LEFT_MID, 120, 0);
  lv_obj_t *power_lbl = lv_label_create(bar);
  lv_label_set_text(power_lbl, "OFFLINE");
  lv_obj_set_style_text_color(power_lbl, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(power_lbl, &lv_font_montserrat_14, 0);
  lv_obj_align(power_lbl, LV_ALIGN_LEFT_MID, 134, 0);

  lv_obj_t *time_label = lv_label_create(bar);
  // Real wall-clock (see wall_clock.h) — no RTC battery or NTP/WiFi on
  // this board yet, so it shows "--:--:--" until set once over Serial
  // (settime YYYY-MM-DD HH:MM:SS) and resets to unset on every reboot.
  // Used to show millis()-based uptime here instead, which looked like
  // a real clock but wasn't one and was "komplett falsch".
  lv_label_set_text(time_label, "--:--:--");
  lv_obj_set_style_text_color(time_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(time_label, &lv_font_montserrat_14, 0);
  lv_obj_align(time_label, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t *tx_lbl = lv_label_create(bar);
  lv_label_set_text(tx_lbl, "TX");
  lv_obj_set_style_text_color(tx_lbl, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(tx_lbl, &lv_font_montserrat_14, 0);
  lv_obj_align(tx_lbl, LV_ALIGN_RIGHT_MID, -76, 0);

  lv_obj_t *tx_dot = lv_obj_create(bar);
  lv_obj_set_size(tx_dot, 10, 10);
  lv_obj_set_style_radius(tx_dot, 2, 0);
  lv_obj_set_style_bg_color(tx_dot, lv_color_hex(COLOR_DIM), 0);
  lv_obj_set_style_border_width(tx_dot, 0, 0);
  lv_obj_align(tx_dot, LV_ALIGN_RIGHT_MID, -56, 0);

  lv_obj_t *rx_lbl = lv_label_create(bar);
  lv_label_set_text(rx_lbl, "RX");
  lv_obj_set_style_text_color(rx_lbl, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(rx_lbl, &lv_font_montserrat_14, 0);
  lv_obj_align(rx_lbl, LV_ALIGN_RIGHT_MID, -30, 0);

  lv_obj_t *rx_dot = lv_obj_create(bar);
  lv_obj_set_size(rx_dot, 10, 10);
  lv_obj_set_style_radius(rx_dot, 2, 0);
  lv_obj_set_style_bg_color(rx_dot, lv_color_hex(COLOR_DIM), 0);
  lv_obj_set_style_border_width(rx_dot, 0, 0);
  lv_obj_align(rx_dot, LV_ALIGN_RIGHT_MID, -10, 0);

  if (g_status_bar_count < MAX_SCREENS) {
    g_status_bar_screens[g_status_bar_count] = scr;
    g_status_bars[g_status_bar_count] = StatusBarWidgets{time_label, tx_dot, rx_dot, power_dot, power_lbl};
    g_status_bar_count++;
  }
}

// Honest placeholder (same reasoning as build_not_connected_page): the
// language-flag button the user asked for (Testprotokoll A1, 2026-09-18)
// has a real home in the main menu now, but actually translating every
// hardcoded German string in this file is a much larger, separate piece of
// work. Say so instead of a button that looks like it does something it
// doesn't.
lv_obj_t *build_language_page(lv_obj_t *back_target) {
  return build_not_connected_page("Sprache", "Andere Sprachen sind noch nicht umgesetzt.\n"
                                              "Die Oberflaeche ist bisher nur auf Deutsch verfuegbar.",
                                   back_target, COLOR_CHROME_BG);
}

void pin_keyboard_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    const char *entered = lv_textarea_get_text(g_pin_textarea);
    bool ok = station_config().settingsPin.equals(entered);
    lv_textarea_set_text(g_pin_textarea, "");
    if (ok) {
      lv_label_set_text(g_pin_error_label, "");
      // Refresh with the live values every time settings are entered --
      // these fields are edited in place further down, not rebuilt.
      lv_textarea_set_text(g_settings_dispatch_ta,
                            station_config().dispatchNodeNum == 0
                                ? ""
                                : String("!" + String(station_config().dispatchNodeNum, 16)).c_str());
      lv_textarea_set_text(g_settings_location_ta, station_config().locationText.c_str());
      lv_textarea_set_text(g_settings_clock_ta, wallClockNowYMDHMS().c_str());
      lv_label_set_text_fmt(g_settings_test_btn_label, "Testmodus: %s (erzwungen)\nZum Umschalten tippen.",
                            g_meshtastic_connected ? "ONLINE" : "OFFLINE");
      lv_label_set_text(g_settings_saved_label, "");
      nav_to(g_scr_settings);
    } else {
      lv_label_set_text(g_pin_error_label, "Falscher Code.");
    }
  } else if (code == LV_EVENT_CANCEL) {
    lv_textarea_set_text(g_pin_textarea, "");
    lv_label_set_text(g_pin_error_label, "");
    nav_to(g_scr_main);
  }
}

// Code-geschuetzter Zugang (Testprotokoll A1, 2026-09-18: "Einstellungen,
// welche Code geschuetzt sind"). PIN kommt aus station_config() -- siehe
// dessen Kommentar zum aktuellen Platzhalter-Wert.
lv_obj_t *build_pin_entry_page(lv_obj_t *back_target) {
  lv_obj_t *scr = make_screen("Einstellungen (PIN)", COLOR_CHROME_BG);
  int32_t top = content_top(true);

  g_pin_textarea = lv_textarea_create(scr);
  lv_textarea_set_password_mode(g_pin_textarea, true);
  lv_textarea_set_one_line(g_pin_textarea, true);
  lv_textarea_set_max_length(g_pin_textarea, 8);
  lv_textarea_set_placeholder_text(g_pin_textarea, "Code");
  lv_obj_set_width(g_pin_textarea, SCR - 80);
  lv_obj_set_pos(g_pin_textarea, 40, top);

  g_pin_error_label = lv_label_create(scr);
  lv_label_set_text(g_pin_error_label, "");
  lv_obj_set_style_text_color(g_pin_error_label, lv_color_hex(COLOR_WARN_RED), 0);
  lv_obj_align_to(g_pin_error_label, g_pin_textarea, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

  lv_obj_t *kb = lv_keyboard_create(scr);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
  lv_keyboard_set_textarea(kb, g_pin_textarea);
  lv_obj_set_size(kb, SCR - 40, SCR - CTXBAR_H - top - 60);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -CTXBAR_H - 10);
  lv_obj_add_event_cb(kb, pin_keyboard_event_cb, LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(kb, pin_keyboard_event_cb, LV_EVENT_CANCEL, nullptr);

  CtxSlot ctx[3] = {};
  ctx[1] = CtxSlot{true, LV_SYMBOL_LEFT, "Zurueck", COLOR_GREEN, nav_cb, back_target};
  build_context_bar(scr, ctx);
  return scr;
}

// The textareas themselves sit lower on the page than the popup keyboard
// covers, so the field being edited was invisible while typing
// (Testprotokoll B1-follow-up, 2026-09-18: "sieht man aktuell gar nicht
// was man tippt"). Rather than relocating the actual textarea widget
// (fragile -- would need to save/restore its original position), a
// dedicated preview label sits just above the keyboard and mirrors
// whichever field currently has focus.
void settings_ta_focus_cb(lv_event_t *e) {
  lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
  lv_obj_t *ta = lv_event_get_target(e);
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(kb); // popup: sit on top of the fields/button below it
  if (g_settings_kb_preview) {
    lv_obj_clear_flag(g_settings_kb_preview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_settings_kb_preview);
    lv_label_set_text_fmt(g_settings_kb_preview, "> %s", lv_textarea_get_text(ta));
  }
}

void settings_ta_changed_cb(lv_event_t *e) {
  if (!g_settings_kb_preview) return;
  lv_label_set_text_fmt(g_settings_kb_preview, "> %s", lv_textarea_get_text(lv_event_get_target(e)));
}

void settings_kb_done_cb(lv_event_t *e) {
  lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  if (g_settings_kb_preview) lv_obj_add_flag(g_settings_kb_preview, LV_OBJ_FLAG_HIDDEN);
}

void settings_test_toggle_cb(lv_event_t *e) {
  clear_pressed(lv_event_get_target(e));
  // Testprotokoll A5/D4, 2026-09-18: exposes the same override the
  // "testconnect on|off" serial command already had, from the UI instead --
  // no new semantics, ui_model_set_connected() is still the one-shot flag
  // documented in this file's header comment.
  ui_model_set_connected(!g_meshtastic_connected);
  lv_label_set_text_fmt(g_settings_test_btn_label, "Testmodus: %s (erzwungen)\nZum Umschalten tippen.",
                        g_meshtastic_connected ? "ONLINE" : "OFFLINE");
}

void settings_save_cb(lv_event_t *e) {
  clear_pressed(lv_event_get_target(e));
  const char *dispatchText = lv_textarea_get_text(g_settings_dispatch_ta);
  uint32_t nodeNum = strtoul(dispatchText[0] == '!' ? dispatchText + 1 : dispatchText, nullptr, 16);
  station_config_set_dispatch_node(nodeNum);
  station_config_set_location(String(lv_textarea_get_text(g_settings_location_ta)));

  // Same "YYYY-MM-DD HH:MM:SS" format the serial "settime" command accepts
  // (wallClockHandleSerialLine) -- this field is prefilled with the board's
  // current idea of the time, so leaving it untouched just re-confirms it.
  // A field that fails to parse is a typo, not "leave the clock alone", so
  // it gets its own error instead of the generic "Gespeichert." (this is
  // the only way to correct the clock without a laptop -- see wall_clock.h).
  int year, month, day, hour, minute, second;
  int n = sscanf(lv_textarea_get_text(g_settings_clock_ta), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour,
                 &minute, &second);
  if (n == 6) {
    wallClockSet(year, month, day, hour, minute, second);
    lv_obj_set_style_text_color(g_settings_saved_label, lv_color_hex(COLOR_GREEN), 0);
    lv_label_set_text(g_settings_saved_label, "Gespeichert.");
  } else {
    lv_obj_set_style_text_color(g_settings_saved_label, lv_color_hex(COLOR_WARN_RED), 0);
    lv_label_set_text(g_settings_saved_label, "Leitstelle/Ort gespeichert. Uhrzeit-Format falsch, nicht "
                                               "uebernommen: JJJJ-MM-TT HH:MM:SS");
  }
}

// Testprotokoll A5/D4/D5/B3, 2026-09-18: bundles everything that came out
// of the walkthrough as "put this behind the PIN-protected settings
// button" -- test-connect override, Leitstelle-Node, Ort. All edit fields
// share one on-screen keyboard, bound to whichever one has focus.
lv_obj_t *build_settings_page(lv_obj_t *back_target) {
  lv_obj_t *scr = make_screen("Einstellungen", COLOR_CHROME_BG);
  int32_t top = content_top(true);
  int32_t y = top;

  g_settings_test_btn_label = lv_label_create(scr);
  lv_obj_set_style_text_color(g_settings_test_btn_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_pos(g_settings_test_btn_label, 20, y);
  y += 40;

  lv_obj_t *toggle_btn = lv_btn_create(scr);
  lv_obj_set_size(toggle_btn, SCR - 40, 50);
  lv_obj_set_pos(toggle_btn, 20, y);
  lv_obj_add_event_cb(toggle_btn, settings_test_toggle_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *toggle_lbl = lv_label_create(toggle_btn);
  lv_label_set_text(toggle_lbl, "Testmodus umschalten");
  lv_obj_center(toggle_lbl);
  y += 62;

  lv_obj_t *dispatch_caption = lv_label_create(scr);
  lv_label_set_text(dispatch_caption, "Leitstelle (Node-ID, z.B. !ce0ffa28):");
  lv_obj_set_style_text_color(dispatch_caption, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_pos(dispatch_caption, 20, y);
  y += 24;

  g_settings_dispatch_ta = lv_textarea_create(scr);
  lv_textarea_set_one_line(g_settings_dispatch_ta, true);
  lv_obj_set_width(g_settings_dispatch_ta, SCR - 40);
  lv_obj_set_pos(g_settings_dispatch_ta, 20, y);
  y += 46;

  lv_obj_t *location_caption = lv_label_create(scr);
  lv_label_set_text(location_caption, "Ort (Freitext, kein GPS auf diesem Board):");
  lv_obj_set_style_text_color(location_caption, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_pos(location_caption, 20, y);
  y += 24;

  g_settings_location_ta = lv_textarea_create(scr);
  lv_textarea_set_one_line(g_settings_location_ta, true);
  lv_obj_set_width(g_settings_location_ta, SCR - 40);
  lv_obj_set_pos(g_settings_location_ta, 20, y);
  y += 46;

  // Kein RTC-Chip, kein NTP/WiFi auf diesem Board -- ohne dieses Feld gibt
  // es im Feld ohne Laptop keine Moeglichkeit, die Uhr zu stellen (bisher
  // nur ueber das serielle "settime"-Kommando, siehe wall_clock.h). Ohne
  // gestellte Uhr sind auch die Zeitstempel der Lagemeldungen falsch.
  lv_obj_t *clock_caption = lv_label_create(scr);
  lv_label_set_text(clock_caption, "Uhrzeit (JJJJ-MM-TT HH:MM:SS):");
  lv_obj_set_style_text_color(clock_caption, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_pos(clock_caption, 20, y);
  y += 24;

  g_settings_clock_ta = lv_textarea_create(scr);
  lv_textarea_set_one_line(g_settings_clock_ta, true);
  lv_obj_set_width(g_settings_clock_ta, SCR - 40);
  lv_obj_set_pos(g_settings_clock_ta, 20, y);
  y += 46;

  // Wiedereinstieg in den Setup-Assistenten (2026-09-23) -- rein navigierend,
  // aendert fuer sich genommen nichts an setupCompleted; das passiert nur,
  // wenn der Assistent auch wirklich bis "Fertig" durchlaufen wird.
  lv_obj_t *setup_btn = lv_btn_create(scr);
  lv_obj_set_size(setup_btn, SCR - 40, 46);
  lv_obj_set_pos(setup_btn, 20, y);
  lv_obj_add_event_cb(setup_btn, nav_cb, LV_EVENT_CLICKED, g_scr_setup_bundesland);
  lv_obj_t *setup_lbl = lv_label_create(setup_btn);
  lv_label_set_text(setup_lbl, "Ersteinrichtung erneut starten");
  lv_obj_center(setup_lbl);
  y += 58;

  lv_obj_t *save_btn = lv_btn_create(scr);
  lv_obj_set_size(save_btn, 160, 46);
  lv_obj_set_pos(save_btn, 20, y);
  lv_obj_add_event_cb(save_btn, settings_save_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *save_lbl = lv_label_create(save_btn);
  lv_label_set_text(save_lbl, "Speichern");
  lv_obj_center(save_lbl);

  g_settings_saved_label = lv_label_create(scr);
  lv_label_set_text(g_settings_saved_label, "");
  lv_label_set_long_mode(g_settings_saved_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(g_settings_saved_label, SCR - 40);
  lv_obj_set_style_text_color(g_settings_saved_label, lv_color_hex(COLOR_GREEN), 0);
  lv_obj_set_pos(g_settings_saved_label, 20, y + 56);

  // Popup keyboard (Testprotokoll B1-follow-up, 2026-09-18): fitting it
  // into whatever space happened to be left below the fields (originally
  // as little as ~24px) squashed its rows into an unreadable mess. Fixed
  // height instead, overlaid on top of the rest of the page and only
  // shown while a field actually has focus.
  lv_obj_t *kb = lv_keyboard_create(scr);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_set_size(kb, SCR, 200);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN); // shown once a field is focused

  // Live preview of what's being typed, just above the keyboard -- the
  // actual field further up the page is covered by the popup while it's
  // open (see settings_ta_focus_cb's comment).
  g_settings_kb_preview = lv_label_create(scr);
  lv_label_set_text(g_settings_kb_preview, "");
  lv_obj_set_style_text_color(g_settings_kb_preview, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(g_settings_kb_preview, &lv_font_montserrat_18, 0);
  lv_obj_set_style_bg_color(g_settings_kb_preview, lv_color_hex(COLOR_CHROME_BG), 0);
  lv_obj_set_style_bg_opa(g_settings_kb_preview, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(g_settings_kb_preview, 8, 0);
  lv_obj_set_width(g_settings_kb_preview, SCR);
  lv_obj_align_to(g_settings_kb_preview, kb, LV_ALIGN_OUT_TOP_MID, 0, 0);
  lv_obj_add_flag(g_settings_kb_preview, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_event_cb(g_settings_dispatch_ta, settings_ta_focus_cb, LV_EVENT_FOCUSED, kb);
  lv_obj_add_event_cb(g_settings_location_ta, settings_ta_focus_cb, LV_EVENT_FOCUSED, kb);
  lv_obj_add_event_cb(g_settings_clock_ta, settings_ta_focus_cb, LV_EVENT_FOCUSED, kb);
  lv_obj_add_event_cb(g_settings_dispatch_ta, settings_ta_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(g_settings_location_ta, settings_ta_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(g_settings_clock_ta, settings_ta_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(kb, settings_kb_done_cb, LV_EVENT_READY, kb);
  lv_obj_add_event_cb(kb, settings_kb_done_cb, LV_EVENT_CANCEL, kb);

  CtxSlot ctx[3] = {};
  ctx[1] = CtxSlot{true, LV_SYMBOL_LEFT, "Zurueck", COLOR_GREEN, nav_cb, back_target};
  build_context_bar(scr, ctx);
  return scr;
}

// ---------------------------------------------------------------------
// Setup-Assistent (2026-09-23)
// ---------------------------------------------------------------------
// Erzwungen beim allerersten Start (siehe ui_model_build()'s abschliessendes
// lv_scr_load), danach ueber die Einstellungen-Seite erneut aufrufbar, ohne
// dass ein Durchlauf ohne "Fertig" auf der letzten Seite irgendetwas
// zuruecksetzt -- station_config_set_setup_completed(true) wird nur dort
// aufgerufen, ein Abbruch mittendrin aendert am gespeicherten Zustand nichts.

void district_selected_cb(lv_event_t *e) {
  clear_pressed(lv_event_get_target(e));
  const DistrictEntry *d = (const DistrictEntry *)lv_event_get_user_data(e);
  // "Kennzeichen -- Name" statt nur des Namens: das Kennzeichen ist die
  // kompakte, jedem bekannte Kurzform, die im Notfall schneller
  // kommuniziert werden kann als der volle Landkreisname (siehe
  // data/presets/README.md fuer die Begruendung dieses Systems).
  String text = String(d->kennzeichen) + " -- " + d->name;
  station_config_set_location(text);
  if (g_setup_selected_ort_label) lv_label_set_text_fmt(g_setup_selected_ort_label, "Ort: %s", text.c_str());
  nav_to(g_scr_setup_leitstelle);
}

void populate_setup_landkreis_list(int bundeslandIdx) {
  lv_obj_clean(g_setup_landkreis_list);
  for (uint16_t i = 0; i < kDistrictCount; i++) {
    if (kDistricts[i].bundeslandIndex != bundeslandIdx) continue;
    lv_obj_t *btn = lv_btn_create(g_setup_landkreis_list);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, 44);
    lv_obj_add_event_cb(btn, district_selected_cb, LV_EVENT_CLICKED, (void *)&kDistricts[i]);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text_fmt(lbl, "%s (%s)", kDistricts[i].name, kDistricts[i].kennzeichen);
  }
}

void bundesland_selected_cb(lv_event_t *e) {
  clear_pressed(lv_event_get_target(e));
  const BundeslandEntry *be = (const BundeslandEntry *)lv_event_get_user_data(e);
  int idx = (int)(be - kBundeslaender); // Index statt Pointer -- kDistricts speichert bundeslandIndex, keinen Pointer
  populate_setup_landkreis_list(idx);
  nav_to(g_scr_setup_landkreis);
}

// Root-Seite des Assistenten: kein Zurueck/Abbruch-Knopf, absichtlich wie
// main_menu (siehe dessen context_bar-Kommentar) -- das ist der erzwungene
// Einstieg, es gibt nichts, wohin man "zurueck" koennte.
lv_obj_t *build_setup_bundesland_page() {
  lv_obj_t *scr = make_screen("Ersteinrichtung: Bundesland", COLOR_CHROME_BG);
  int32_t top = content_top(true);

  lv_obj_t *intro = lv_label_create(scr);
  lv_label_set_text(intro, "In welchem Bundesland steht dieses Geraet?");
  lv_obj_set_style_text_color(intro, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_pos(intro, 20, top);
  top += 30;

  lv_obj_t *list = lv_obj_create(scr);
  lv_obj_set_size(list, SCR - 40, SCR - CTXBAR_H - top - 10);
  lv_obj_set_pos(list, 20, top);
  lv_obj_set_style_bg_color(list, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_radius(list, 10, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(list, 10, 0);
  lv_obj_set_style_pad_row(list, 8, 0);
  lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

  for (uint16_t i = 0; i < kBundeslandCount; i++) {
    lv_obj_t *btn = lv_btn_create(list);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, 44);
    lv_obj_add_event_cb(btn, bundesland_selected_cb, LV_EVENT_CLICKED, (void *)&kBundeslaender[i]);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, kBundeslaender[i].name);
  }

  CtxSlot ctx[3] = {};
  ctx[0] = CtxSlot{true, LV_SYMBOL_UP, "Hoch", COLOR_TEAL,
                   [](lv_event_t *e) { lv_obj_scroll_by((lv_obj_t *)lv_event_get_user_data(e), 0, 80, LV_ANIM_OFF); },
                   list};
  ctx[2] = CtxSlot{
      true, LV_SYMBOL_DOWN, "Runter", COLOR_TEAL,
      [](lv_event_t *e) { lv_obj_scroll_by((lv_obj_t *)lv_event_get_user_data(e), 0, -80, LV_ANIM_OFF); }, list};
  build_context_bar(scr, ctx);
  return scr;
}

// Inhalt wird erst bei Betreten dynamisch befuellt (populate_setup_landkreis_list,
// aufgerufen von bundesland_selected_cb) -- anders als jede andere Seite in
// dieser Datei, die ihren Inhalt einmalig bei ui_model_build() aufbaut, weil
// hier die Auswahl vom vorherigen Schritt abhaengt.
lv_obj_t *build_setup_landkreis_page() {
  lv_obj_t *scr = make_screen("Ersteinrichtung: Landkreis", COLOR_CHROME_BG);
  int32_t top = content_top(true);

  g_setup_landkreis_list = lv_obj_create(scr);
  lv_obj_set_size(g_setup_landkreis_list, SCR - 40, SCR - CTXBAR_H - top - 10);
  lv_obj_set_pos(g_setup_landkreis_list, 20, top);
  lv_obj_set_style_bg_color(g_setup_landkreis_list, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_radius(g_setup_landkreis_list, 10, 0);
  lv_obj_set_flex_flow(g_setup_landkreis_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(g_setup_landkreis_list, 10, 0);
  lv_obj_set_style_pad_row(g_setup_landkreis_list, 8, 0);
  lv_obj_clear_flag(g_setup_landkreis_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

  CtxSlot ctx[3] = {};
  ctx[0] = CtxSlot{true, LV_SYMBOL_UP, "Hoch", COLOR_TEAL,
                   [](lv_event_t *e) { lv_obj_scroll_by((lv_obj_t *)lv_event_get_user_data(e), 0, 80, LV_ANIM_OFF); },
                   g_setup_landkreis_list};
  ctx[1] = CtxSlot{true, LV_SYMBOL_LEFT, "Zurueck", COLOR_GREEN, nav_cb, g_scr_setup_bundesland};
  ctx[2] = CtxSlot{true, LV_SYMBOL_DOWN, "Runter", COLOR_TEAL,
                   [](lv_event_t *e) { lv_obj_scroll_by((lv_obj_t *)lv_event_get_user_data(e), 0, -80, LV_ANIM_OFF); },
                   g_setup_landkreis_list};
  build_context_bar(scr, ctx);
  return scr;
}

// Eigene, auf diese Seite hartcodierte Fokus/Tastatur-Callbacks statt der
// settings_ta_*-Funktionen: die dort verwendeten Globals (g_settings_kb_preview
// etc.) gehoeren zur Einstellungen-Seite, ein Wiederverwenden wuerde die
// Vorschauzeile auf der falschen, gerade unsichtbaren Seite aktualisieren.
void setup_ta_focus_cb(lv_event_t *e) {
  lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
  lv_obj_t *ta = lv_event_get_target(e);
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(kb);
  if (g_setup_leitstelle_kb_preview) {
    lv_obj_clear_flag(g_setup_leitstelle_kb_preview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_setup_leitstelle_kb_preview);
    lv_label_set_text_fmt(g_setup_leitstelle_kb_preview, "> %s", lv_textarea_get_text(ta));
  }
}

void setup_ta_changed_cb(lv_event_t *e) {
  if (!g_setup_leitstelle_kb_preview) return;
  lv_label_set_text_fmt(g_setup_leitstelle_kb_preview, "> %s", lv_textarea_get_text(lv_event_get_target(e)));
}

void setup_kb_done_cb(lv_event_t *e) {
  lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
  if (g_setup_leitstelle_kb_preview) lv_obj_add_flag(g_setup_leitstelle_kb_preview, LV_OBJ_FLAG_HIDDEN);
}

void setup_finish_cb(lv_event_t *e) {
  clear_pressed(lv_event_get_target(e));
  const char *dispatchText = lv_textarea_get_text(g_setup_leitstelle_ta);
  if (dispatchText[0] != '\0') {
    uint32_t nodeNum = strtoul(dispatchText[0] == '!' ? dispatchText + 1 : dispatchText, nullptr, 16);
    station_config_set_dispatch_node(nodeNum);
  }
  // Leitstelle bleibt bewusst optional (leer lassen + "Fertig" schliesst den
  // Assistenten trotzdem ab) -- sie laesst sich jederzeit in den
  // Einstellungen nachtragen, ein Notmeldeterminal soll wegen einer noch
  // unbekannten Leitstellen-Adresse nicht in der Ersteinrichtung haengen
  // bleiben.
  station_config_set_setup_completed(true);
  nav_to(g_scr_main);
}

lv_obj_t *build_setup_leitstelle_page() {
  lv_obj_t *scr = make_screen("Ersteinrichtung: Leitstelle", COLOR_CHROME_BG);
  int32_t top = content_top(true);
  int32_t y = top;

  g_setup_selected_ort_label = lv_label_create(scr);
  lv_obj_set_style_text_color(g_setup_selected_ort_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_pos(g_setup_selected_ort_label, 20, y);
  y += 30;

  lv_obj_t *caption = lv_label_create(scr);
  lv_label_set_text(caption,
                     "Leitstelle (Node-ID, z.B. !ce0ffa28) -- optional, spaeter in den Einstellungen aenderbar:");
  lv_obj_set_style_text_color(caption, lv_color_hex(0xFFFFFF), 0);
  lv_label_set_long_mode(caption, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(caption, SCR - 40);
  lv_obj_set_pos(caption, 20, y);
  y += 48;

  g_setup_leitstelle_ta = lv_textarea_create(scr);
  lv_textarea_set_one_line(g_setup_leitstelle_ta, true);
  lv_obj_set_width(g_setup_leitstelle_ta, SCR - 40);
  lv_obj_set_pos(g_setup_leitstelle_ta, 20, y);
  y += 46;

  lv_obj_t *finish_btn = lv_btn_create(scr);
  lv_obj_set_size(finish_btn, 160, 46);
  lv_obj_set_pos(finish_btn, 20, y);
  lv_obj_add_event_cb(finish_btn, setup_finish_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *finish_lbl = lv_label_create(finish_btn);
  lv_label_set_text(finish_lbl, "Fertig");
  lv_obj_center(finish_lbl);

  lv_obj_t *kb = lv_keyboard_create(scr);
  lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_set_size(kb, SCR, 200);
  lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

  g_setup_leitstelle_kb_preview = lv_label_create(scr);
  lv_label_set_text(g_setup_leitstelle_kb_preview, "");
  lv_obj_set_style_text_color(g_setup_leitstelle_kb_preview, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(g_setup_leitstelle_kb_preview, &lv_font_montserrat_18, 0);
  lv_obj_set_style_bg_color(g_setup_leitstelle_kb_preview, lv_color_hex(COLOR_CHROME_BG), 0);
  lv_obj_set_style_bg_opa(g_setup_leitstelle_kb_preview, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(g_setup_leitstelle_kb_preview, 8, 0);
  lv_obj_set_width(g_setup_leitstelle_kb_preview, SCR);
  lv_obj_align_to(g_setup_leitstelle_kb_preview, kb, LV_ALIGN_OUT_TOP_MID, 0, 0);
  lv_obj_add_flag(g_setup_leitstelle_kb_preview, LV_OBJ_FLAG_HIDDEN);

  lv_obj_add_event_cb(g_setup_leitstelle_ta, setup_ta_focus_cb, LV_EVENT_FOCUSED, kb);
  lv_obj_add_event_cb(g_setup_leitstelle_ta, setup_ta_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(kb, setup_kb_done_cb, LV_EVENT_READY, kb);
  lv_obj_add_event_cb(kb, setup_kb_done_cb, LV_EVENT_CANCEL, kb);

  CtxSlot ctx[3] = {};
  ctx[1] = CtxSlot{true, LV_SYMBOL_LEFT, "Zurueck", COLOR_GREEN, nav_cb, g_scr_setup_landkreis};
  build_context_bar(scr, ctx);
  return scr;
}

} // namespace

void ui_model_build() {
  memset(g_last_tx_on, -1, sizeof(g_last_tx_on));
  memset(g_last_rx_on, -1, sizeof(g_last_rx_on));
  memset(g_last_connected, -1, sizeof(g_last_connected));

  // emergency_confirmation first: department menus need a valid target for it.
  g_scr_confirm = make_screen("Notfall bestaetigen");
  int32_t confirm_top = content_top(true);

  g_confirm_label = lv_label_create(g_scr_confirm);
  lv_obj_set_style_text_color(g_confirm_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(g_confirm_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_align(g_confirm_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(g_confirm_label, SCR - 40);
  lv_obj_set_pos(g_confirm_label, 20, confirm_top);

  // Circular hold-to-confirm target (Testprotokoll B1, 2026-09-18):
  // wrapper just for positioning, holds a progress ring (lv_arc, purely
  // visual) around a circular button (the actual tap target -- large, so
  // it stays easy to hold accurately for 3s, unlike a thin ring alone
  // would be). Hint text goes below the whole thing, not crammed inside.
  lv_obj_t *hold_wrap = lv_obj_create(g_scr_confirm);
  lv_obj_set_size(hold_wrap, 200, 200);
  lv_obj_set_style_bg_opa(hold_wrap, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(hold_wrap, 0, 0);
  lv_obj_set_style_pad_all(hold_wrap, 0, 0);
  lv_obj_clear_flag(hold_wrap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(hold_wrap, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(hold_wrap, LV_ALIGN_BOTTOM_MID, 0, -(CTXBAR_H + 46));

  g_hold_arc = lv_arc_create(hold_wrap);
  lv_obj_set_size(g_hold_arc, 200, 200);
  lv_obj_center(g_hold_arc);
  lv_arc_set_rotation(g_hold_arc, 270);
  lv_arc_set_bg_angles(g_hold_arc, 0, 360);
  lv_arc_set_range(g_hold_arc, 0, 100);
  lv_arc_set_value(g_hold_arc, 0);
  lv_obj_remove_style(g_hold_arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(g_hold_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(g_hold_arc, lv_color_hex(COLOR_DIM), LV_PART_MAIN);
  lv_obj_set_style_arc_color(g_hold_arc, lv_color_hex(COLOR_GREEN), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(g_hold_arc, 14, LV_PART_MAIN);
  lv_obj_set_style_arc_width(g_hold_arc, 14, LV_PART_INDICATOR);

  // The actual tap-and-hold target, inset inside the ring.
  lv_obj_t *confirm_btn = lv_btn_create(hold_wrap);
  g_confirm_circle_btn = confirm_btn;
  lv_obj_set_size(confirm_btn, 170, 170);
  lv_obj_center(confirm_btn);
  lv_obj_set_style_radius(confirm_btn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(confirm_btn, lv_color_hex(COLOR_GREEN), 0);
  lv_obj_add_event_cb(confirm_btn, confirm_btn_press_cb, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(confirm_btn, confirm_btn_press_cb, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(confirm_btn, confirm_btn_press_cb, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(confirm_btn, confirm_btn_press_cb, LV_EVENT_PRESS_LOST, nullptr);

  g_hold_pct_label = lv_label_create(confirm_btn);
  lv_label_set_text(g_hold_pct_label, LV_SYMBOL_OK);
  lv_obj_set_style_text_font(g_hold_pct_label, &lv_font_montserrat_32, 0);
  lv_obj_set_style_text_color(g_hold_pct_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(g_hold_pct_label);

  lv_obj_t *hold_hint = lv_label_create(g_scr_confirm);
  lv_label_set_text(hold_hint, "3 Sekunden halten zum Bestaetigen");
  lv_obj_set_style_text_color(hold_hint, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(hold_hint, &lv_font_montserrat_16, 0);
  lv_obj_align_to(hold_hint, hold_wrap, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

  CtxSlot ctx[3] = {};
  ctx[1] = CtxSlot{true, LV_SYMBOL_CLOSE, "Abbruch", COLOR_WARN_RED, confirm_cancel_cb, nullptr};
  build_context_bar(g_scr_confirm, ctx);

  const EmergencyChoice fire_items[4] = {
      {"fire_department", "fire", "Brand"},
      {"fire_department", "chemical", "Chemie"},
      {"fire_department", "traffic_accident", "Verkehrsunfall"},
      {"fire_department", "other", "Andere"},
  };
  const EmergencyChoice police_items[4] = {
      {"police", "burglary", "Einbruch"},
      {"police", "theft", "Diebstahl"},
      {"police", "security", "Sicherheit"},
      {"police", "other", "Andere"},
  };
  const EmergencyChoice ambulance_items[4] = {
      {"ambulance", "injury", "Verletzung"},
      {"ambulance", "emergency_doctor", "Notarzt"},
      {"ambulance", "transport", "Transport"},
      {"ambulance", "other", "Andere"},
  };

  // main_menu: no per-screen title (the status bar already shows the
  // station's canonical ID — a generic "Hauptmenue" heading is redundant).
  g_scr_main = make_screen(nullptr);

  // Distinct per-department background (Testprotokoll A2, 2026-09-18):
  // fire departs from the AOT-CD palette on purpose -- red is the one
  // near-universal safety color for fire, and a life-safety emergency
  // terminal should favor instant recognizability over strict palette
  // consistency here.
  g_scr_fire = build_department_menu("Feuerwehr", LV_SYMBOL_WARNING, fire_items, g_scr_main, COLOR_WARN_RED);
  g_scr_police = build_department_menu("Polizei", LV_SYMBOL_EYE_OPEN, police_items, g_scr_main, COLOR_GREEN);
  g_scr_ambulance = build_department_menu("Krankenwagen", LV_SYMBOL_PLUS, ambulance_items, g_scr_main, COLOR_TEAL);

  // g_scr_info's shell is built first (empty) so situation/crisis/history
  // — all reached FROM the info menu, not from main_menu — can point
  // their "Zurueck" at it. Same nullptr-capture bug as before otherwise:
  // these back-targets are baked in at CtxSlot construction time, so the
  // target has to already be a valid pointer, not just assigned later.
  // (This was the "Zurueck geht auf Home statt zurueck" report — these
  // three previously pointed at g_scr_main directly, skipping a menu
  // level.)
  g_scr_info = make_screen("Info");

  g_scr_situation = build_info_list_page("Lageinformationen", /*onlyReceived=*/true, g_scr_info);
  g_scr_crisis = build_not_connected_page(
      "Systeminfo Krisenstab", "Es gibt noch keine Anbindung an ein Krisenstab-System.\n"
      "Diese Seite zeigt daher keine Daten an, statt etwas Falsches vorzutaeuschen.",
      g_scr_info);
  g_scr_history = build_history_page(g_scr_info);

  // Two more main-menu buttons the user asked for (Testprotokoll A1,
  // 2026-09-18): language and code-protected settings. Both built here,
  // after g_scr_main exists, for the same reason g_scr_info's children are
  // (see comment above) -- their "Zurueck" targets must already be valid.
  g_scr_language = build_language_page(g_scr_main);
  // Vor build_settings_page(): dessen "Ersteinrichtung erneut starten"-Knopf
  // erfasst g_scr_setup_bundesland als Zeiger-WERT beim Registrieren des
  // Callbacks, nicht als spaeter aufgeloeste Referenz -- muesste sonst noch
  // nullptr sein.
  g_scr_setup_bundesland = build_setup_bundesland_page();
  g_scr_setup_landkreis = build_setup_landkreis_page();
  g_scr_setup_leitstelle = build_setup_leitstelle_page();

  g_scr_settings = build_settings_page(g_scr_main);
  g_scr_pin_entry = build_pin_entry_page(g_scr_main);

  {
    int32_t top = content_top(true);
    lv_obj_t *t1 = add_tile(g_scr_info, 0, top, LV_SYMBOL_LIST, "Lageinformationen\nanfordern");
    lv_obj_add_event_cb(t1, nav_cb, LV_EVENT_CLICKED, g_scr_situation);
    lv_obj_t *t2 = add_tile(g_scr_info, 1, top, LV_SYMBOL_LIST, "Systeminfo\nKrisenstab");
    lv_obj_add_event_cb(t2, nav_cb, LV_EVENT_CLICKED, g_scr_crisis);
    lv_obj_t *t3 = add_tile(g_scr_info, 2, top, LV_SYMBOL_BELL, "Notmeldungs-\nhistorie");
    lv_obj_add_event_cb(t3, nav_cb, LV_EVENT_CLICKED, g_scr_history);

    CtxSlot ctx_info[3] = {};
    ctx_info[1] = CtxSlot{true, LV_SYMBOL_LEFT, "Zurueck", COLOR_GREEN, nav_cb, g_scr_main};
    build_context_bar(g_scr_info, ctx_info);
  }

  {
    int32_t top = content_top(false);
    lv_obj_t *t1 = add_tile(g_scr_main, 0, top, LV_SYMBOL_WARNING, "Feuerwehr");
    lv_obj_add_event_cb(t1, nav_cb, LV_EVENT_CLICKED, g_scr_fire);
    lv_obj_t *t2 = add_tile(g_scr_main, 1, top, LV_SYMBOL_EYE_OPEN, "Polizei");
    lv_obj_add_event_cb(t2, nav_cb, LV_EVENT_CLICKED, g_scr_police);
    lv_obj_t *t3 = add_tile(g_scr_main, 2, top, LV_SYMBOL_PLUS, "Krankenwagen");
    lv_obj_add_event_cb(t3, nav_cb, LV_EVENT_CLICKED, g_scr_ambulance);
    lv_obj_t *t4 = add_tile(g_scr_main, 3, top, LV_SYMBOL_LIST, "Info");
    lv_obj_add_event_cb(t4, nav_cb, LV_EVENT_CLICKED, g_scr_info);

    // root page: no "Zurueck" (middle slot stays inactive) -- the two
    // outer slots were unused until now, so this is where the two extra
    // buttons the user asked for (Testprotokoll A1, 2026-09-18) belong.
    CtxSlot ctx_main[3] = {};
    ctx_main[0] = CtxSlot{true, "DE", "Sprache", COLOR_TEAL, nav_cb, g_scr_language};
    ctx_main[2] = CtxSlot{true, LV_SYMBOL_SETTINGS, "Einstellungen", COLOR_TEAL, nav_cb, g_scr_pin_entry};
    build_context_bar(g_scr_main, ctx_main);
  }

  // emergency_details / emergency_transmission / emergency_transmission_failed
  // (ui.yaml): built here, after g_scr_main exists, not earlier — their
  // "Zurueck"/"Abbruch" buttons target g_scr_main, and CtxSlot.user_data
  // captures that pointer's value at construction time. Building these
  // before g_scr_main was assigned baked in a nullptr, which is why
  // "Zurueck" silently did nothing on the first version of this screen.
  g_scr_emergency_details = make_screen("Notfall uebermittelt");
  {
    int32_t top = content_top(true);
    lv_obj_t *check = lv_label_create(g_scr_emergency_details);
    lv_label_set_text(check, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(check, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_text_font(check, &lv_font_montserrat_32, 0);
    lv_obj_align(check, LV_ALIGN_TOP_MID, 0, top);

    g_emergency_details_label = lv_label_create(g_scr_emergency_details);
    lv_obj_set_style_text_color(g_emergency_details_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(g_emergency_details_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(g_emergency_details_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_emergency_details_label, SCR - 40);
    lv_obj_set_pos(g_emergency_details_label, 20, top + 50);

    CtxSlot ctx_details[3] = {};
    ctx_details[1] = CtxSlot{true, LV_SYMBOL_LEFT, "Zurueck", COLOR_GREEN, nav_cb, g_scr_main};
    build_context_bar(g_scr_emergency_details, ctx_details);
  }

  // emergency_transmission: shown while sending. No context bar at all:
  // nothing to do here but wait, matching ui.yaml (no buttons listed).
  // The stage bar/label below narrate real steps (radio send, then
  // waiting for the dispatch node's ACK) at a pace a human can actually
  // read, not a fake randomized delay — see ui_model_tick()'s handling of
  // g_transmission_pending for what actually drives each stage.
  g_scr_transmission = make_screen("Notfall wird uebermittelt");
  {
    int32_t top = content_top(true);
    g_transmission_label = lv_label_create(g_scr_transmission);
    lv_obj_set_style_text_color(g_transmission_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(g_transmission_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(g_transmission_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g_transmission_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_transmission_label, SCR - 40);
    lv_obj_set_pos(g_transmission_label, 20, top + 10);

    // Positioned relative to the label above it, not a fixed y offset --
    // found live (2026-09-18) that a longer label (2-3 wrapped lines) grew
    // taller than the fixed gap allowed for, so the bar ended up
    // overlapping/hiding it. lv_obj_update_layout() forces the label's
    // real wrapped height to be known before anything aligns off it.
    lv_obj_update_layout(g_transmission_label);

    g_transmission_bar = lv_bar_create(g_scr_transmission);
    lv_obj_set_size(g_transmission_bar, SCR - 80, 22);
    lv_obj_align_to(g_transmission_bar, g_transmission_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_bar_set_range(g_transmission_bar, 0, 100);
    lv_obj_set_style_bg_color(g_transmission_bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_transmission_bar, LV_OPA_30, 0);
    lv_obj_set_style_bg_color(g_transmission_bar, lv_color_hex(COLOR_TEAL), LV_PART_INDICATOR);

    // CLI-style step rows (Testprotokoll B2, 2026-09-18) -- each starts
    // with a wait icon, set_transmission_stage() turns it into a green
    // check (or, on final failure, a red X for whichever step never
    // completed).
    // Only two rows -- see set_transmission_stage()'s comment on why the
    // old middle "Verbindung zur Leitstelle" row was removed (it didn't
    // correspond to any real, checkable step).
    static const char *const kRowLabels[2] = {"Notmeldung gesendet", "Von der Leitstelle bestaetigt"};
    lv_obj_t *rows = lv_obj_create(g_scr_transmission);
    lv_obj_set_size(rows, SCR - 80, 2 * 34);
    lv_obj_align_to(rows, g_transmission_bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
    lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rows, 0, 0);
    lv_obj_set_style_pad_all(rows, 0, 0);
    lv_obj_clear_flag(rows, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(rows, 6, 0);
    for (int i = 0; i < 2; i++) {
      lv_obj_t *row = lv_obj_create(rows);
      lv_obj_set_size(row, LV_PCT(100), 28);
      lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(row, 0, 0);
      lv_obj_set_style_pad_all(row, 0, 0);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(row, 10, 0);

      g_transmission_row_icon[i] = lv_label_create(row);
      lv_label_set_text(g_transmission_row_icon[i], LV_SYMBOL_LOOP);
      lv_obj_set_style_text_color(g_transmission_row_icon[i], lv_color_hex(COLOR_DIM), 0);
      lv_obj_set_style_text_font(g_transmission_row_icon[i], &lv_font_montserrat_16, 0);

      lv_obj_t *row_lbl = lv_label_create(row);
      lv_label_set_text(row_lbl, kRowLabels[i]);
      lv_obj_set_style_text_color(row_lbl, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_text_font(row_lbl, &lv_font_montserrat_16, 0);
    }

    g_transmission_stage_label = lv_label_create(g_scr_transmission);
    lv_obj_set_style_text_color(g_transmission_stage_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(g_transmission_stage_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(g_transmission_stage_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_transmission_stage_label, SCR - 40);
    lv_obj_align_to(g_transmission_stage_label, rows, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
  }

  g_scr_transmission_failed = make_screen("Uebertragung fehlgeschlagen");
  {
    int32_t top = content_top(true);
    g_transmission_failed_label = lv_label_create(g_scr_transmission_failed);
    lv_obj_set_style_text_color(g_transmission_failed_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(g_transmission_failed_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(g_transmission_failed_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_transmission_failed_label, SCR - 40);
    lv_obj_set_pos(g_transmission_failed_label, 20, top);

    CtxSlot ctx_failed[3] = {};
    ctx_failed[0] = CtxSlot{true, LV_SYMBOL_REFRESH, "Erneut senden", COLOR_TEAL, retry_transmission_cb, nullptr};
    ctx_failed[1] = CtxSlot{true, LV_SYMBOL_CLOSE, "Abbruch", COLOR_WARN_RED, transmission_failed_cancel_cb, nullptr};
    build_context_bar(g_scr_transmission_failed, ctx_failed);
  }

  // Erster Start (setupCompleted noch false, siehe station_config.h): der
  // Setup-Assistent statt des Hauptmenues, damit ein frisch geflashtes oder
  // -zurueckgesetztes Geraet nicht ohne Ort/Leitstelle in Betrieb geht.
  lv_scr_load(station_config().setupCompleted ? g_scr_main : g_scr_setup_bundesland); // first load: no fade needed
}

void ui_model_tick() {
  unsigned long now = millis();

  // Only touch the currently visible screen's own status bar copy — the
  // other 7 are off-screen and don't need updating every tick.
  lv_obj_t *active = lv_scr_act();
  int active_idx = -1;
  for (int i = 0; i < g_status_bar_count; i++) {
    if (g_status_bar_screens[i] == active) { active_idx = i; break; }
  }

  if (active_idx >= 0) {
    if (now - g_last_clock_update >= 1000) {
      g_last_clock_update = now;
      lv_label_set_text(g_status_bars[active_idx].time_label, wallClockNowHMS().c_str());
    }
    bool tx_on = (now - g_last_tx_flash) < INDICATOR_FLASH_MS;
    bool rx_on = (now - g_last_rx_flash) < INDICATOR_FLASH_MS;
    if (g_last_tx_on[active_idx] != (int8_t)tx_on) {
      g_last_tx_on[active_idx] = (int8_t)tx_on;
      lv_obj_set_style_bg_color(g_status_bars[active_idx].tx_dot, lv_color_hex(tx_on ? 0xFFA500 : COLOR_DIM), 0);
    }
    if (g_last_rx_on[active_idx] != (int8_t)rx_on) {
      g_last_rx_on[active_idx] = (int8_t)rx_on;
      lv_obj_set_style_bg_color(g_status_bars[active_idx].rx_dot, lv_color_hex(rx_on ? COLOR_GREEN : COLOR_DIM), 0);
    }
    if (g_last_connected[active_idx] != (int8_t)g_meshtastic_connected) {
      g_last_connected[active_idx] = (int8_t)g_meshtastic_connected;
      lv_obj_set_style_bg_color(g_status_bars[active_idx].power_dot,
                                lv_color_hex(g_meshtastic_connected ? COLOR_GREEN : COLOR_WARN_RED), 0);
      lv_label_set_text(g_status_bars[active_idx].power_lbl, g_meshtastic_connected ? "ONLINE" : "OFFLINE");
    }
  }
  // hold_confirm is now driven entirely by LVGL's PRESSED/PRESSING/
  // RELEASED events on the single confirm button (confirm_btn_press_cb)
  // — no per-tick polling needed here anymore.

  // The actual meshtastic_send_emergency() call already happened (in
  // start_transmission, via meshtastic_proto.cpp). What happens next
  // depends on whether the radio even accepted the send locally:
  if (g_transmission_pending) {
    unsigned long elapsed = now - g_transmission_started_at;

    // Row 0 (Gesendet) and the initial narration were already set
    // synchronously in start_transmission() -- g_last_send_ok is a real,
    // known fact from the moment this screen loads, nothing to pace here.
    // Row 1 (Bestaetigung) is what this loop actually watches for.

    // Waiting for the real ACK can take up to EMERGENCY_ACK_TIMEOUT_MS
    // (8s) with nothing else changing on screen -- found live (2026-09-18)
    // that a static wait icon made it "feel broken" during that stretch.
    // Simple ASCII spinner keeps row 1 visibly alive without claiming any
    // progress that isn't real (only the actual ACK/timeout still decides
    // success or failure).
    if (g_last_send_ok && g_transmission_row_icon[1]) {
      static const char *const kSpinnerFrames[] = {"|", "/", "-", "\\"};
      static unsigned long lastSpinAt = 0;
      static int spinFrame = 0;
      if (now - lastSpinAt >= 200) {
        lastSpinAt = now;
        spinFrame = (spinFrame + 1) % 4;
        lv_label_set_text(g_transmission_row_icon[1], kSpinnerFrames[spinFrame]);
      }
    }

    if (!g_last_send_ok) {
      // Local send already failed (no dispatch configured, radio error) --
      // nothing to wait for. Same short delay as before just so
      // "senden..." doesn't flash past instantly.
      if (elapsed >= TRANSMISSION_LOCAL_FAIL_DELAY_MS) {
        g_transmission_pending = false;
        finish_transmission(false);
      }
    } else if (meshtastic_proto_emergency_ack_received()) {
      // A real application-level ACK came back from the dispatch node --
      // genuine acceptance confirmation, finish as soon as it arrives.
      set_transmission_stage(4, "Von der Leitstelle bestaetigt!", 100);
      g_transmission_pending = false;
      finish_transmission(true);
    } else if (elapsed >= EMERGENCY_ACK_TIMEOUT_MS) {
      // Radio sent it, but nobody confirmed receipt in time. Report as
      // failure -- we genuinely don't know if it arrived, and this screen
      // must never claim success without real confirmation.
      set_transmission_stage(5, "Keine Bestaetigung erhalten.", 100);
      g_transmission_pending = false;
      finish_transmission(false);
    }
  }
}

void ui_model_set_connected(bool connected) { g_meshtastic_connected = connected; }

void ui_model_test_trigger_emergency(const char *category, const char *type, const char *label) {
  g_selected = EmergencyChoice{category, type, label};
  do_trigger_emergency();
}

void ui_model_notify_tx() { g_last_tx_flash = millis(); }
void ui_model_notify_rx() { g_last_rx_flash = millis(); }
