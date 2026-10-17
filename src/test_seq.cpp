#include "test_seq.h"
#include "config.h"
#include "dshot.h"
#include "esc_telem.h"

#include <Arduino.h>
#include <atomic>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "seq";

/* All of this is owned by control_task. Other tasks talk to it only through the
 * request flags and the snapshot. */
static test_state_t  s_state         = ST_IDLE;
static uint32_t      s_state_ms      = 0;
static uint32_t      s_run_start_ms  = 0;
static uint32_t      s_run_id        = 0;
static uint32_t      s_signal_on_ms  = 0;
static uint32_t      s_idle_since_ms = 0;
static uint8_t       s_point         = 0;
static uint16_t      s_ramp_from     = 0;
static uint16_t      s_ramp_to       = 0;
static uint8_t       s_itrip_run     = 0;
static uint32_t      s_cmd_ms        = 0;   /* millis() of the last special command */
static uint32_t      s_req_mark      = 0;   /* telemetry requests at the window start */
static reason_code_t s_code          = RC_PASS;
static run_data_t    s_data;
static dwell_acc_t   s_acc;
static esc_sample_t  s_last;

static std::atomic<bool> s_req_start{false};
static std::atomic<bool> s_req_abort{false};
static std::atomic<bool> s_req_ack{false};
static std::atomic<bool> s_req_touch{false};

static SemaphoreHandle_t s_snap_lock = nullptr;
static live_t            s_snap;

static const uint32_t PER_POINT_MS = CFG_RAMP_MS + CFG_SETTLE_MS + CFG_DWELL_MS;
static const uint32_t TOTAL_MS     = CFG_PRECHECK_MS + CFG_ARM_MS
                                   + CFG_N_SETPOINTS * PER_POINT_MS + CFG_SPINDOWN_MS;

/* After the DShot stream restarts (idle disarm, then a touch), give the ESC this
 * long to lock on before Precheck starts counting its telemetry frames. */
static const uint32_t SIGNAL_SETTLE_MS = 1000;

/* How long spindown may wait at DShot 0 for eRPM to fall before giving up. */
static const uint32_t SPINDOWN_LIMIT_MS = CFG_RAMP_MS + 2000;

const char *test_state_name(test_state_t s)
{
    switch (s) {
    case ST_IDLE:     return "IDLE";
    case ST_PRECHECK: return "PRECHECK";
    case ST_ARM:      return "ARM";
    case ST_RAMP:     return "RAMP";
    case ST_SETTLE:   return "SETTLE";
    case ST_DWELL:    return "DWELL";
    case ST_SPINDOWN: return "SPINDOWN";
    case ST_VERDICT:  return "VERDICT";
    case ST_FAULT:    return "FAULT";
    }
    return "?";
}

static bool is_running(test_state_t s)
{
    return s >= ST_PRECHECK && s <= ST_SPINDOWN;
}

static void enter(test_state_t s, uint32_t now)
{
    s_state    = s;
    s_state_ms = now;
    if (s == ST_IDLE) s_idle_since_ms = now;
}

/* A window's sample count is judged against the requests actually made during
 * it, not against a nominal rate: only the ESC's answer rate should show up in
 * valid_pct, never a frame timer that slipped. */
static void acc_restart(void)
{
    dwell_acc_reset(&s_acc);
    s_req_mark = dshot_telem_requests();
}

static uint16_t requests_since_mark(void)
{
    const uint32_t n = dshot_telem_requests() - s_req_mark;
    return (uint16_t)(n > 0xFFFFu ? 0xFFFFu : n);
}

/* True while a DShot special command may still be stopping the ESC answering.
 * The beep is played by pulsing the motor; telemetry pauses while it runs, and
 * an interruption the tester asked for is not a fault on the ESC. */
static bool in_cmd_grace(uint32_t now)
{
    return s_cmd_ms != 0 && now - s_cmd_ms < CFG_CMD_TELEM_GRACE_MS;
}

#if CFG_DIAG_LOG
/* Reading an E10 starts here. tlm/s near the request rate means the link is
 * healthy and the fault is elsewhere. Zero with crc climbing is noise on the
 * wire. Zero with every reject counter flat is an ESC that never answered.
 * held climbing on its own means frames arrive too far apart to lock. */
static uint32_t s_diag_rate = 0;    /* last measured frames/s; a fault line is not on a tick boundary */

static void diag_line(const char *what, uint32_t now)
{
    esc_telem_stats_t t;
    esc_telem_get_stats(&t);
    const uint32_t rate = s_diag_rate;
    ESP_LOGI(TAG, "%-8s %-9s tlm %3lu/s age %5ld ms | rej crc %lu span %lu range %lu held %lu | %s | dshot %s req %lu miss %lu",
             test_state_name(s_state), what,
             (unsigned long)rate,
             t.last_ms ? (long)(now - t.last_ms) : -1L,
             (unsigned long)t.rej_crc, (unsigned long)t.rej_span,
             (unsigned long)t.rej_range, (unsigned long)t.held,
             t.sliding ? "sliding" : "gap",
             dshot_is_running() ? "on" : "off",
             (unsigned long)dshot_telem_requests(),
             (unsigned long)dshot_missed_frames());
}

static void diag_tick(uint32_t now)
{
    static uint32_t last_ms = 0, last_good = 0;
    if (now - last_ms < 1000) return;

    esc_telem_stats_t t;
    esc_telem_get_stats(&t);
    if (last_ms) {
        s_diag_rate = (t.good - last_good) * 1000u / (now - last_ms);
        diag_line("", now);
    }
    last_ms   = now;
    last_good = t.good;
}

/* Precheck's own counts, logged before its gates are applied: a Precheck E10
 * means this n fell under CFG_PRECHECK_MIN_FRAMES, and the number says whether
 * the ESC was slow to answer or silent. */
static void diag_precheck(const dwell_stats_t *p)
{
    ESP_LOGI(TAG, "precheck  frames %u/%u = %u%%  v %.2f  i %.2f  erpm %lu",
             (unsigned)p->n, (unsigned)p->expected, (unsigned)p->valid_pct,
             (double)p->v_med, (double)p->i_med, (unsigned long)p->erpm_med);
}

/* What each set point actually collected, which is where a verdict E10 comes
 * from: n against expected is the ESC's answer rate, nothing else. */
static void diag_run_summary(void)
{
    for (uint8_t k = 0; k < s_data.n_points && k < CFG_MAX_SETPOINTS; k++) {
        const dwell_stats_t &d = s_data.d[k];
        ESP_LOGI(TAG, "SP%u  frames %u/%u = %u%%  v %.2f  i %.2f  erpm %lu  tmax %u",
                 (unsigned)(k + 1), (unsigned)d.n, (unsigned)d.expected,
                 (unsigned)d.valid_pct, (double)d.v_med, (double)d.i_med,
                 (unsigned long)d.erpm_med, (unsigned)d.t_max);
    }
}
#else
static void diag_line(const char *, uint32_t) {}
static void diag_tick(uint32_t) {}
static void diag_precheck(const dwell_stats_t *) {}
static void diag_run_summary(void) {}
#endif

/* Every abort does the same thing: DShot 0 on the very next frame, and keep
 * transmitting 0. The stream itself is never stopped to stop the motor. */
static void enter_fault(reason_code_t rc, uint32_t now)
{
    dshot_set_throttle(0);
    s_code = rc;
    diag_line(reason_code_str(rc), now);
    enter(ST_FAULT, now);
}

static void ensure_signal(uint32_t now)
{
    if (!dshot_is_running()) {
        dshot_start();
        s_signal_on_ms = now;
    }
}

static uint16_t lerp_u16(uint16_t a, uint16_t b, uint32_t t, uint32_t span)
{
    if (t >= span) return b;
    return (uint16_t)((int32_t)a + ((int32_t)b - (int32_t)a) * (int32_t)t / (int32_t)span);
}

static void start_point(uint8_t k, uint32_t now)
{
    s_point = k;
    const uint16_t cur = dshot_get_throttle();
    s_ramp_from = (cur < 48) ? 48 : cur;
    s_ramp_to   = dshot_from_pct(CFG_SETPOINT_PCT[k]);
    enter(ST_RAMP, now);
}

static void begin_run(uint32_t now)
{
    s_run_id++;
    memset(&s_data, 0, sizeof(s_data));
    s_data.n_points = CFG_N_SETPOINTS;
    s_point     = 0;
    s_itrip_run = 0;
    s_cmd_ms    = 0;
    s_code      = RC_PASS;
    acc_restart();
    esc_telem_reset_stats();

    dshot_set_throttle(0);
    ensure_signal(now);
    s_run_start_ms = now;
    enter(ST_PRECHECK, now);
}

/* Instantaneous trips, checked on every sample while a run is live. These do
 * not wait for a dwell median: they exist to stop the motor, not to judge it. */
static void check_trips(const esc_sample_t *s, uint32_t now)
{
    if (!is_running(s_state)) return;

    if (sample_amps(s) > CFG_I_TRIP) {
        if (++s_itrip_run >= CFG_I_TRIP_SAMPLES) { enter_fault(RC_E07, now); return; }
    } else {
        s_itrip_run = 0;
    }
    if (s->temp_c > CFG_TEMP_TRIP) enter_fault(RC_E11, now);
}

static uint8_t progress_pct(uint32_t t)
{
    const uint32_t base = CFG_PRECHECK_MS + CFG_ARM_MS + s_point * PER_POINT_MS;
    uint32_t pos;
    switch (s_state) {
    case ST_PRECHECK: pos = t;                                        break;
    case ST_ARM:      pos = CFG_PRECHECK_MS + t;                      break;
    case ST_RAMP:     pos = base + t;                                 break;
    case ST_SETTLE:   pos = base + CFG_RAMP_MS + t;                   break;
    case ST_DWELL:    pos = base + CFG_RAMP_MS + CFG_SETTLE_MS + t;   break;
    case ST_SPINDOWN: pos = TOTAL_MS - CFG_SPINDOWN_MS + t;           break;
    case ST_VERDICT:  pos = TOTAL_MS;                                 break;
    default:          pos = 0;                                        break;
    }
    const uint32_t pct = pos * 100u / TOTAL_MS;
    return (uint8_t)(pct > 100 ? 100 : pct);
}

void test_seq_init(void)
{
    s_snap_lock = xSemaphoreCreateMutex();
    const uint32_t now = millis();
    dshot_set_throttle(0);
    ensure_signal(now);          /* ESC sees DShot 0 from boot and stays quiet */
    enter(ST_IDLE, now);
}

void test_seq_start(void) { s_req_start.store(true); }
void test_seq_abort(void) { s_req_abort.store(true); }
void test_seq_ack(void)   { s_req_ack.store(true); }
void test_seq_touch(void) { s_req_touch.store(true); }

void test_seq_tick(void)
{
    const uint32_t now = millis();

    /* 1. Drain telemetry. */
    esc_sample_t s;
    while (xQueueReceive(esc_telem_queue(), &s, 0) == pdTRUE) {
        s_last = s;
        if (s_state == ST_PRECHECK || s_state == ST_DWELL) dwell_acc_add(&s_acc, &s);
        check_trips(&s, now);
    }

    /* 2. Operator requests. */
    if (s_req_touch.exchange(false)) {
        s_idle_since_ms = now;
        ensure_signal(now);
    }
    if (s_req_abort.exchange(false) && is_running(s_state)) enter_fault(RC_E13, now);
    if (s_req_start.exchange(false) && s_state == ST_IDLE)  begin_run(now);
    if (s_req_ack.exchange(false) && (s_state == ST_VERDICT || s_state == ST_FAULT)) {
        dshot_set_throttle(0);
        enter(ST_IDLE, now);
    }

    /* 3. Watchdogs, only while a run is live. */
    if (is_running(s_state)) {
        const uint32_t last = esc_telem_last_ms();
        if (now - s_run_start_ms > CFG_RUN_WATCHDOG_MS) {
            enter_fault(RC_E12, now);
        } else if (dshot_missed_frames() > CFG_DSHOT_MISS_LIMIT) {
            enter_fault(RC_E12, now);
        } else if (s_state != ST_PRECHECK && !in_cmd_grace(now)
                   && (last == 0 || now - last > CFG_TELEM_GAP_MS)) {
            enter_fault(RC_E10, now);
        }
    }

    /* 4. State machine. */
    const uint32_t t = now - s_state_ms;
    switch (s_state) {
    case ST_IDLE:
        /* The ESC answers telemetry requests, it does not stream on its own, so
         * Idle keeps the DShot 0 stream running with the telemetry bit set on
         * every CFG_TELEM_REQUEST_DIV-th frame. That is what puts live voltage
         * and a meaningful link indicator on the Home screen, and it proves the
         * signal path before anyone presses START. It also keeps the ESC armed,
         * which is why the stream restarts here rather than only on a touch:
         * nothing else re-establishes it if it ever stopped.
         *
         * With CFG_IDLE_DISARM_MS set, the stream instead stops after that long
         * without operator action and the ESC disarms and beeps; telemetry
         * stops with it until the next touch. */
        if (CFG_IDLE_DISARM_MS != 0 && now - s_idle_since_ms > CFG_IDLE_DISARM_MS) {
            if (dshot_is_running()) dshot_stop();
        } else {
            ensure_signal(now);
        }
        break;

    case ST_PRECHECK:
        if (now - s_signal_on_ms < SIGNAL_SETTLE_MS) {
            s_state_ms = now;            /* hold the Precheck window closed */
            acc_restart();
            break;
        }
        if (t >= CFG_PRECHECK_MS) {
            dwell_stats_t pre;
            dwell_acc_finish(&s_acc, requests_since_mark(), &pre);
            diag_precheck(&pre);
            if (pre.n == 0)                          { enter_fault(RC_E01, now); break; }
            if (pre.n < CFG_PRECHECK_MIN_FRAMES)     { enter_fault(RC_E10, now); break; }
            if (pre.v_med > CFG_V_MAX)               { enter_fault(RC_E04, now); break; }
            if (pre.v_med < CFG_V_MIN)               { enter_fault(RC_E03, now); break; }
            if (pre.i_med > CFG_PRECHECK_I_MAX ||
                pre.erpm_med > CFG_SPINDOWN_ERPM)    { enter_fault(RC_E02, now); break; }

            s_data.v_precheck = pre.v_med;
            s_data.t_start    = s_last.temp_c;
#if CFG_BEEP_ON_START
            if (dshot_send_command(1)) s_cmd_ms = now;
#endif
            enter(ST_ARM, now);
        }
        break;

    case ST_ARM:
        if (t >= CFG_ARM_MS) start_point(0, now);
        break;

    case ST_RAMP:
        dshot_set_throttle(lerp_u16(s_ramp_from, s_ramp_to, t, CFG_RAMP_MS));
        if (t >= CFG_RAMP_MS) enter(ST_SETTLE, now);
        break;

    case ST_SETTLE:
        if (t >= CFG_SETTLE_MS) {
            acc_restart();
            enter(ST_DWELL, now);
        }
        break;

    case ST_DWELL:
        if (t >= CFG_DWELL_MS) {
            dwell_acc_finish(&s_acc, requests_since_mark(), &s_data.d[s_point]);
            if (s_point + 1 < CFG_N_SETPOINTS) {
                start_point(s_point + 1, now);
            } else {
                s_ramp_from = dshot_get_throttle();
                enter(ST_SPINDOWN, now);
            }
        }
        break;

    case ST_SPINDOWN:
        if (t < CFG_RAMP_MS) {
            dshot_set_throttle(lerp_u16(s_ramp_from, 48, t, CFG_RAMP_MS));
        } else {
            dshot_set_throttle(0);
        }
        if (t >= CFG_SPINDOWN_MS && s_last.erpm < CFG_SPINDOWN_ERPM) {
            s_code = verdict_evaluate(&s_data);
            diag_run_summary();
            diag_line(reason_code_str(s_code), now);
            enter(ST_VERDICT, now);
        } else if (t >= SPINDOWN_LIMIT_MS) {
            enter_fault(RC_E12, now);
        }
        break;

    case ST_VERDICT:
    case ST_FAULT:
        break;
    }

    /* 5. Publish a snapshot for the UI. */
    const uint32_t last = esc_telem_last_ms();
    const bool     link = last != 0 && now - last <= CFG_TELEM_GAP_MS;

    live_t L = {};
    L.state    = s_state;
    L.run_id   = s_run_id;
    L.n_points = CFG_N_SETPOINTS;
    L.point    = (s_state >= ST_RAMP && s_state <= ST_DWELL) ? s_point + 1 : 0;
    L.progress = progress_pct(now - s_state_ms);
    L.link     = link;
    L.throttle = dshot_get_throttle();
    if (link) {
        L.v    = sample_volts(&s_last);
        L.i    = sample_amps(&s_last);
        L.temp = s_last.temp_c;
        L.erpm = s_last.erpm;
        L.rpm  = erpm_to_rpm(s_last.erpm);
    }
    L.code = s_code;
    L.data = s_data;

    if (xSemaphoreTake(s_snap_lock, pdMS_TO_TICKS(2)) == pdTRUE) {
        s_snap = L;
        xSemaphoreGive(s_snap_lock);
    }

    diag_tick(now);
}

void test_seq_snapshot(live_t *out)
{
    if (xSemaphoreTake(s_snap_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
        *out = s_snap;
        xSemaphoreGive(s_snap_lock);
    }
}
