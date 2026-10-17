#include "ui.h"
#include "config.h"
#include "esc_telem.h"
#include "test_seq.h"

#include <M5Unified.h>
#include <lvgl.h>
#include <stdarg.h>
#include <stdio.h>

/* ---- Look ----------------------------------------------------------------- */

#define SCR_W   320
#define SCR_H   240
#define BTN_H   70          /* START and STOP: full width, never smaller */

#define C_BG     lv_color_hex(0x0E1116)
#define C_PANEL  lv_color_hex(0x1A1F27)
#define C_TEXT   lv_color_hex(0xE8EAED)
#define C_DIM    lv_color_hex(0x8A93A0)
#define C_GO     lv_color_hex(0x2FBF71)
#define C_STOP   lv_color_hex(0xE5484D)
#define C_ACCENT lv_color_hex(0x3B82F6)
#define C_WARN   lv_color_hex(0xF5A524)

/* ---- LVGL <-> M5GFX glue -------------------------------------------------- */

/* Two partial buffers in internal RAM. Static arrays land in internal SRAM, not
 * PSRAM, which keeps the SPI flush fast.
 *
 * 24 lines, not the 40 this carried on the CoreS3: the classic ESP32's DRAM
 * segment is smaller, and 40-line buffers plus LVGL's pool overflow it at link
 * time. 24 is a tenth of the screen, which is what LVGL recommends anyway, and
 * a full-screen redraw is SPI-bound rather than flush-count-bound. */
#define BUF_LINES   24
static uint8_t s_buf1[SCR_W * BUF_LINES * 2] __attribute__((aligned(4)));
static uint8_t s_buf2[SCR_W * BUF_LINES * 2] __attribute__((aligned(4)));

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    const int32_t w = lv_area_get_width(a);
    const int32_t h = lv_area_get_height(a);
    M5.Display.startWrite();
    M5.Display.setAddrWindow(a->x1, a->y1, w, h);
    /* LVGL renders little-endian RGB565; the panel wants it byte-swapped. If
     * colours come out wrong on first boot, flip this to false. */
    M5.Display.writePixels((uint16_t *)px, w * h, true);
    M5.Display.endWrite();
    lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *, lv_indev_data_t *d)
{
    const auto &t = M5.Touch.getDetail();
    if (!t.isPressed()) {
        d->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    test_seq_touch();          /* any touch wakes the ESC stream after idle disarm */

    /* The Core2's digitiser is 320x280 against a 320x240 display: the bottom
     * strip is the silkscreened BtnA/B/C row below the glass, not the screen.
     * The bottom of the screen is always START or STOP, so a press down there
     * must not be handed to LVGL as if it had landed on the button. */
    if (t.y >= SCR_H) {
        d->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    d->point.x = t.x;
    d->point.y = t.y;
    d->state   = LV_INDEV_STATE_PRESSED;
}

/* ---- Tones ---------------------------------------------------------------- */

typedef struct { uint16_t hz, ms; } tone_t;
static tone_t   s_tones[3];
static uint8_t  s_tone_n = 0, s_tone_i = 0;
static uint32_t s_tone_next_ms = 0;

static void play(const tone_t *seq, uint8_t n)
{
    if (n > 3) n = 3;
    memcpy(s_tones, seq, n * sizeof(tone_t));
    s_tone_n = n;
    s_tone_i = 0;
    s_tone_next_ms = 0;
}

static void tone_tick(void)
{
    if (s_tone_i >= s_tone_n || millis() < s_tone_next_ms) return;
    const tone_t &t = s_tones[s_tone_i++];
    M5.Speaker.tone(t.hz, t.ms);
    s_tone_next_ms = millis() + t.ms + 40;
}

static const tone_t TONE_START[] = { {2000, 60} };
static const tone_t TONE_PASS[]  = { {1200, 120}, {1800, 200} };
static const tone_t TONE_FAIL[]  = { {300, 700} };

/* ---- Widget helpers ------------------------------------------------------- */

/* Formatted text goes through newlib's vsnprintf, never lv_label_set_text_fmt or
 * lv_table_set_cell_value_fmt. LVGL's built-in printf only formats floats when
 * LV_USE_FLOAT is on, which would also switch its internal geometry to float;
 * without it "%.2f" renders as a bare "f". */
static void label_printf(lv_obj_t *label, const char *fmt, ...)
{
    char buf[128];      /* longest user: "E07  " + its reason text, ~75 chars */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(label, buf);
}

static void table_printf(lv_obj_t *table, uint32_t row, uint32_t col, const char *fmt, ...)
{
    char buf[24];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_table_set_cell_value(table, row, col, buf);
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *s = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s, C_BG, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s, C_TEXT, 0);
    lv_obj_set_scrollable(s, false);
    return s;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *txt, lv_color_t col,
                             int32_t w, int32_t h, const lv_font_t *font, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, col, 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = make_label(b, font, lv_color_white(), txt);
    lv_obj_center(l);
    return b;
}

/* A caption over a large value, for the 2x2 grid on the Running screen. */
static lv_obj_t *make_value_cell(lv_obj_t *parent, int32_t x, int32_t y, const char *caption,
                                 lv_obj_t **caption_out)
{
    lv_obj_t *c = make_label(parent, &lv_font_montserrat_14, C_DIM, caption);
    lv_obj_set_pos(c, x, y);
    if (caption_out) *caption_out = c;
    lv_obj_t *v = make_label(parent, &lv_font_montserrat_32, C_TEXT, "--");
    lv_obj_set_pos(v, x, y + 16);
    return v;
}

/* ---- Screens -------------------------------------------------------------- */

static lv_obj_t *scr_home, *scr_run, *scr_result, *scr_fault;
static lv_obj_t *stop_btn;

/* Home */
static lv_obj_t *home_volts, *home_link, *home_run, *home_temp;
#if ESC_SIM_MODE
static lv_obj_t *home_sim_lbl;
#endif

/* Running */
static lv_obj_t *run_phase, *run_bar;
static lv_obj_t *run_v, *run_i, *run_erpm, *run_erpm_cap, *run_t;

/* Result */
static lv_obj_t *res_verdict, *res_code, *res_table;

/* Fault */
static lv_obj_t *fault_code, *fault_text;

static void on_start(lv_event_t *) { test_seq_start(); }
static void on_stop(lv_event_t *)  { test_seq_abort(); }
static void on_ack(lv_event_t *)   { test_seq_ack(); }

#if ESC_SIM_MODE
static void on_sim(lv_event_t *)
{
    const sim_fault_t next = (sim_fault_t)((esc_sim_get_fault() + 1) % SIM_FAULT__COUNT);
    esc_sim_set_fault(next);
    label_printf(home_sim_lbl, "SIM: %s", esc_sim_fault_name(next));
}
#endif

/* The bench instruction the operator has to obey before every run. It is the
 * one thing on this screen that is not a number, and it is deliberately the
 * last thing above START. */
#define HOME_WARNING  "Connect Motor & ESC WITHOUT Propeller and Touch Start"

static void build_home(void)
{
    scr_home = make_screen();

    /* Header: what this is, and whether the ESC is talking. */
    lv_obj_t *title = make_label(scr_home, &lv_font_montserrat_20, C_DIM, "ESC TESTER");
    lv_obj_set_pos(title, 10, 6);

    home_link = make_label(scr_home, &lv_font_montserrat_14, C_STOP, "NO TLM");
    lv_obj_align(home_link, LV_ALIGN_TOP_RIGHT, -10, 10);

    lv_obj_t *rule = lv_obj_create(scr_home);
    lv_obj_set_size(rule, SCR_W, 1);
    lv_obj_set_pos(rule, 0, 32);
    lv_obj_set_style_bg_color(rule, C_PANEL, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_set_scrollable(rule, false);

    /* The live block: bus voltage large, with what it is and the two secondary
     * numbers on one strip beneath it. Positions are filled in at the end. */
    home_volts = make_label(scr_home, &lv_font_montserrat_48, C_TEXT, "--.- V");

    lv_obj_t *cap = make_label(scr_home, &lv_font_montserrat_14, C_DIM, "bus voltage");

    /* A hot ESC on the bench is worth seeing before a run rather than after one,
     * so the temperature sits on the strip opposite the run counter. */
    home_temp = make_label(scr_home, &lv_font_montserrat_14, C_DIM, "ESC -- C");
    home_run  = make_label(scr_home, &lv_font_montserrat_14, C_DIM, "runs: 0");

    /* The banner, directly above START. */
#if ESC_SIM_MODE
    /* There is no motor in simulation, so the bench warning would be a lie.
     * The fault selector takes the slot instead. */
    lv_obj_t *banner = lv_button_create(scr_home);
    lv_obj_add_event_cb(banner, on_sim, LV_EVENT_CLICKED, nullptr);
#else
    lv_obj_t *banner = lv_obj_create(scr_home);
    lv_obj_set_scrollable(banner, false);
#endif
    lv_obj_set_width(banner, SCR_W - 16);
    lv_obj_set_height(banner, LV_SIZE_CONTENT);   /* as tall as its text wraps to */
    lv_obj_set_style_bg_color(banner, C_PANEL, 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(banner, 4, 0);
    lv_obj_set_style_shadow_width(banner, 0, 0);
    lv_obj_set_style_pad_all(banner, 6, 0);
    lv_obj_set_style_border_width(banner, 1, 0);
    lv_obj_align(banner, LV_ALIGN_BOTTOM_MID, 0, -BTN_H - 6);

#if ESC_SIM_MODE
    lv_obj_set_style_border_color(banner, C_ACCENT, 0);
    home_sim_lbl = make_label(banner, &lv_font_montserrat_16, C_ACCENT, "SIM: healthy");
    lv_obj_set_width(home_sim_lbl, lv_pct(100));
    lv_obj_set_style_text_align(home_sim_lbl, LV_TEXT_ALIGN_CENTER, 0);
#else
    lv_obj_set_style_border_color(banner, C_WARN, 0);
    lv_obj_t *warn = make_label(banner, &lv_font_montserrat_16, C_WARN, HOME_WARNING);
    lv_obj_set_width(warn, lv_pct(100));
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, 0);
#endif

    lv_obj_t *start = make_button(scr_home, "START", C_GO, SCR_W, BTN_H,
                                  &lv_font_montserrat_32, on_start);
    lv_obj_align(start, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* The banner is as tall as its text wraps to, so the live block is placed
     * from that measured height rather than from fixed pixel rows: change the
     * wording and nothing above it slides under the banner or under START.
     * lv_obj_align() - not align_to() - because these labels resize as their
     * values do, and only align() re-applies itself when they do. */
    lv_obj_update_layout(scr_home);
    const int32_t strip_y = SCR_H - BTN_H - 6 - lv_obj_get_height(banner)
                                     - 6 - lv_obj_get_height(cap);
    lv_obj_align(home_temp,  LV_ALIGN_TOP_LEFT,   10, strip_y);
    lv_obj_align(cap,        LV_ALIGN_TOP_MID,     0, strip_y);
    lv_obj_align(home_run,   LV_ALIGN_TOP_RIGHT, -10, strip_y);
    lv_obj_align(home_volts, LV_ALIGN_TOP_MID,     0,
                 strip_y - 2 - lv_obj_get_height(home_volts));
}

static void build_running(void)
{
    scr_run = make_screen();

    run_phase = make_label(scr_run, &lv_font_montserrat_20, C_TEXT, "PRECHECK");
    lv_obj_set_pos(run_phase, 10, 6);

    run_bar = lv_bar_create(scr_run);
    lv_obj_set_size(run_bar, SCR_W - 20, 8);
    lv_obj_set_pos(run_bar, 10, 34);
    lv_bar_set_range(run_bar, 0, 100);
    lv_obj_set_style_bg_color(run_bar, C_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(run_bar, C_ACCENT, LV_PART_INDICATOR);

    run_v    = make_value_cell(scr_run, 10,  50,  "volts", nullptr);
    run_i    = make_value_cell(scr_run, 165, 50,  "amps", nullptr);
    run_erpm = make_value_cell(scr_run, 10,  106, "eRPM", &run_erpm_cap);
    run_t    = make_value_cell(scr_run, 165, 106, "ESC temp", nullptr);
}

static void build_result(void)
{
    scr_result = make_screen();

    res_verdict = make_label(scr_result, &lv_font_montserrat_48, C_GO, "PASS");
    lv_obj_align(res_verdict, LV_ALIGN_TOP_MID, 0, 0);

    res_code = make_label(scr_result, &lv_font_montserrat_14, C_TEXT, "");
    lv_obj_set_width(res_code, SCR_W - 20);
    lv_label_set_long_mode(res_code, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(res_code, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(res_code, LV_ALIGN_TOP_MID, 0, 54);

    res_table = lv_table_create(scr_result);
    lv_table_set_column_count(res_table, 4);
    lv_table_set_row_count(res_table, CFG_N_SETPOINTS + 1);
    static const int32_t widths[4] = { 50, 80, 80, 90 };
    for (uint32_t c = 0; c < 4; c++) lv_table_set_column_width(res_table, c, widths[c]);
    lv_table_set_cell_value(res_table, 0, 0, "SP");
    lv_table_set_cell_value(res_table, 0, 1, "V");
    lv_table_set_cell_value(res_table, 0, 2, "A");
    lv_table_set_cell_value(res_table, 0, 3, "eRPM");
    lv_obj_set_style_text_font(res_table, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(res_table, 3, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(res_table, C_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_text_color(res_table, C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_border_color(res_table, C_BG, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(res_table, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(res_table, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(res_table, 0, LV_PART_MAIN);
    lv_obj_align(res_table, LV_ALIGN_TOP_MID, 0, 88);

    lv_obj_t *done = make_button(scr_result, "DONE", C_ACCENT, SCR_W, 44,
                                 &lv_font_montserrat_20, on_ack);
    lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void build_fault(void)
{
    scr_fault = make_screen();

    lv_obj_t *hdr = make_label(scr_fault, &lv_font_montserrat_48, C_STOP, "FAULT");
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 4);

    fault_code = make_label(scr_fault, &lv_font_montserrat_32, C_TEXT, "E00");
    lv_obj_align(fault_code, LV_ALIGN_TOP_MID, 0, 58);

    fault_text = make_label(scr_fault, &lv_font_montserrat_14, C_TEXT, "");
    lv_obj_set_width(fault_text, SCR_W - 20);
    lv_label_set_long_mode(fault_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(fault_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(fault_text, LV_ALIGN_TOP_MID, 0, 100);

    lv_obj_t *ack = make_button(scr_fault, "ACK", C_ACCENT, SCR_W, BTN_H,
                                &lv_font_montserrat_32, on_ack);
    lv_obj_align(ack, LV_ALIGN_BOTTOM_MID, 0, 0);
}

/* STOP lives on the top layer, above every screen, so no screen change, dialog
 * or toast can ever cover it. Shown only while a run is live. */
static void build_stop(void)
{
    stop_btn = make_button(lv_layer_top(), "STOP", C_STOP, SCR_W, BTN_H,
                           &lv_font_montserrat_32, on_stop);
    lv_obj_align(stop_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_hidden(stop_btn, true);
}

/* ---- State-driven updates -------------------------------------------------- */

static bool is_live(test_state_t s) { return s >= ST_PRECHECK && s <= ST_SPINDOWN; }

static void fill_result(const live_t *L)
{
    const bool pass = L->code == RC_PASS;
    lv_label_set_text(res_verdict, pass ? "PASS" : "FAIL");
    lv_obj_set_style_text_color(res_verdict, pass ? C_GO : C_STOP, 0);

    if (pass) lv_label_set_text(res_code, reason_code_text(RC_PASS));
    else      label_printf(res_code, "%s  %s", reason_code_str(L->code), reason_code_text(L->code));

    for (uint8_t k = 0; k < L->data.n_points && k < CFG_N_SETPOINTS; k++) {
        const dwell_stats_t &d = L->data.d[k];
        table_printf(res_table, k + 1, 0, "%u", (unsigned)(k + 1));
        table_printf(res_table, k + 1, 1, "%.2f", (double)d.v_med);
        table_printf(res_table, k + 1, 2, "%.2f", (double)d.i_med);
        table_printf(res_table, k + 1, 3, "%lu", (unsigned long)d.erpm_med);
    }
}

static void fill_fault(const live_t *L)
{
    lv_label_set_text(fault_code, reason_code_str(L->code));
    lv_label_set_text(fault_text, reason_code_text(L->code));
}

static void on_state_change(test_state_t from, test_state_t to, const live_t *L)
{
    lv_obj_set_hidden(stop_btn, !is_live(to));

    switch (to) {
    case ST_IDLE:
        lv_screen_load(scr_home);
        break;
    case ST_PRECHECK:
        lv_screen_load(scr_run);
        play(TONE_START, 1);
        break;
    case ST_VERDICT:
        fill_result(L);
        lv_screen_load(scr_result);
        if (L->code == RC_PASS) play(TONE_PASS, 2);
        else                    play(TONE_FAIL, 1);
        break;
    case ST_FAULT:
        fill_fault(L);
        lv_screen_load(scr_fault);
        play(TONE_FAIL, 1);
        break;
    default:
        break;
    }
    (void)from;
}

/* Live numerics at 10 Hz: readable, and cheap to redraw. */
static void refresh_values(const live_t *L)
{
    if (L->state == ST_IDLE) {
        if (L->link) label_printf(home_volts, "%.2f V", (double)L->v);
        else         lv_label_set_text(home_volts, "--.- V");
        if (L->link) label_printf(home_temp, "ESC %u C", (unsigned)L->temp);
        else         lv_label_set_text(home_temp, "ESC -- C");
        lv_obj_set_style_text_color(home_temp, L->link ? C_TEXT : C_DIM, 0);
        lv_label_set_text(home_link, L->link ? "TLM OK" : "NO TLM");
        lv_obj_set_style_text_color(home_link, L->link ? C_GO : C_STOP, 0);
        label_printf(home_run, "runs: %lu", (unsigned long)L->run_id);
        return;
    }
    if (!is_live(L->state)) return;

    if (L->point) label_printf(run_phase, "%s   %u / %u", test_state_name(L->state),
                                        (unsigned)L->point, (unsigned)L->n_points);
    else          lv_label_set_text(run_phase, test_state_name(L->state));
    lv_bar_set_value(run_bar, L->progress, LV_ANIM_OFF);

    if (L->link) {
        label_printf(run_v, "%.2f", (double)L->v);
        label_printf(run_i, "%.2f", (double)L->i);
        label_printf(run_erpm, "%lu", (unsigned long)L->erpm);
        label_printf(run_erpm_cap, "eRPM  (%lu rpm)", (unsigned long)L->rpm);
        label_printf(run_t, "%u C", (unsigned)L->temp);
    } else {
        lv_label_set_text(run_v, "--");
        lv_label_set_text(run_i, "--");
        lv_label_set_text(run_erpm, "--");
        lv_label_set_text(run_t, "--");
    }
}

/* ---- Public ----------------------------------------------------------------- */

static test_state_t s_prev_state = ST_IDLE;
static uint8_t      s_div        = 0;

void ui_init(void)
{
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });

    lv_display_t *disp = lv_display_create(SCR_W, SCR_H);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, s_buf1, s_buf2, sizeof(s_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_cb);

    build_home();
    build_running();
    build_result();
    build_fault();
    build_stop();

    lv_screen_load(scr_home);
}

void ui_tick(void)
{
    live_t L;
    test_seq_snapshot(&L);

    if (L.state != s_prev_state) {
        on_state_change(s_prev_state, L.state, &L);
        s_prev_state = L.state;
    }
    if (++s_div >= 3) {
        s_div = 0;
        refresh_values(&L);
    }

    tone_tick();
    lv_timer_handler();
}
