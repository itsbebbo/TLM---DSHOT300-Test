#include "verdict.h"

#include <algorithm>
#include <string.h>

/* No Arduino, no FreeRTOS, no ESP-IDF in this file. Keep it that way: it is the
 * part of the firmware that can be unit-tested on a PC. */

const char *reason_code_str(reason_code_t rc)
{
    static const char *const names[] = {
        "PASS", "E01", "E02", "E03", "E04", "E05", "E06", "E07",
        "E08", "E09", "E10", "E11", "E12", "E13"
    };
    return (rc <= RC_E13) ? names[rc] : "E??";
}

const char *reason_code_text(reason_code_t rc)
{
    switch (rc) {
    case RC_PASS: return "All set points in range";
    case RC_E01:  return "No telemetry. Check telemetry wire and ground.";
    case RC_E02:  return "Load or motion before arming. Motor turning, or a shorted phase.";
    case RC_E03:  return "Bus voltage below floor. Supply too low or wrong pack.";
    case RC_E04:  return "Bus voltage above ceiling. Wrong supply.";
    case RC_E05:  return "Bus sag too large. Supply limit, thin leads, or missing cap.";
    case RC_E06:  return "No current at a set point. ESC not driving, or open phase.";
    case RC_E07:  return "Over-current. Shorted winding, dragging bearing, or a prop left on.";
    case RC_E08:  return "No rotation. Desync, damaged phase, or seized motor.";
    case RC_E09:  return "eRPM did not rise with command. Partial desync or gate driver.";
    case RC_E10:  return "Telemetry dropouts. Wiring noise, marginal ground, or ESC MCU.";
    case RC_E11:  return "Over-temperature.";
    case RC_E12:  return "Timeout. Tester or ESC stopped responding.";
    case RC_E13:  return "Aborted by operator. Void run, not an ESC verdict.";
    }
    return "";
}

/* ---- Accumulation --------------------------------------------------------- */

void dwell_acc_reset(dwell_acc_t *a)
{
    a->n     = 0;
    a->t_max = 0;
}

void dwell_acc_add(dwell_acc_t *a, const esc_sample_t *s)
{
    if (s->temp_c > a->t_max) a->t_max = s->temp_c;
    if (a->n >= CFG_MAX_DWELL_SAMPLES) return;
    a->v[a->n] = s->voltage_cv;
    a->i[a->n] = s->current_ca;
    a->e[a->n] = s->erpm;
    a->n++;
}

/* Upper median. The median rather than the mean because the ESC current sensor
 * throws single-frame spikes, and a median ignores them without a filter. */
template <typename T>
static T median_of(const T *src, uint16_t n)
{
    if (n == 0) return 0;
    T tmp[CFG_MAX_DWELL_SAMPLES];
    memcpy(tmp, src, n * sizeof(T));
    std::nth_element(tmp, tmp + n / 2, tmp + n);
    return tmp[n / 2];
}

void dwell_acc_finish(const dwell_acc_t *a, uint16_t expected, dwell_stats_t *out)
{
    out->n        = a->n;
    out->expected = expected;
    const uint32_t pct = expected ? (uint32_t)a->n * 100u / expected : 0;
    out->valid_pct = (uint8_t)(pct > 100 ? 100 : pct);
    out->v_med    = median_of(a->v, a->n) * 0.01f;
    out->i_med    = median_of(a->i, a->n) * 0.01f;
    out->erpm_med = median_of(a->e, a->n);
    out->t_max    = a->t_max;
}

/* ---- Decision ------------------------------------------------------------- */

reason_code_t verdict_evaluate(const run_data_t *r)
{
    const uint8_t np = r->n_points;

    /* A dwell with no samples has medians of zero, which would otherwise read as
     * "bus voltage below floor". Report the real cause first. */
    for (uint8_t k = 0; k < np; k++)
        if (r->d[k].n == 0) return RC_E10;

    for (uint8_t k = 0; k < np; k++)
        if (r->d[k].v_med < CFG_V_MIN) return RC_E03;

    for (uint8_t k = 0; k < np; k++)
        if (r->d[k].v_med > CFG_V_MAX) return RC_E04;

    for (uint8_t k = 0; k < np; k++)
        if (r->v_precheck - r->d[k].v_med > CFG_V_SAG_MAX) return RC_E05;

    for (uint8_t k = 0; k < np && k < CFG_N_SETPOINTS; k++)
        if (r->d[k].i_med < CFG_I_MIN[k]) return RC_E06;

    for (uint8_t k = 0; k < np; k++)
        if (r->d[k].i_med > CFG_I_MAX) return RC_E07;

    /* The eRPM floor is what proves the motor turned: current alone can flow into
     * a stalled or mis-commutating motor. */
    for (uint8_t k = 0; k < np; k++)
        if (r->d[k].erpm_med < CFG_ERPM_MIN) return RC_E08;

    for (uint8_t k = 1; k < np; k++) {
        const float need = r->d[k - 1].erpm_med * (1.0f + CFG_ERPM_RISE_MIN_PCT / 100.0f);
        if ((float)r->d[k].erpm_med < need) return RC_E09;
    }

    for (uint8_t k = 0; k < np; k++)
        if (r->d[k].valid_pct < CFG_TELEM_VALID_PCT) return RC_E10;

    int t_max = 0;
    for (uint8_t k = 0; k < np; k++)
        if (r->d[k].t_max > t_max) t_max = r->d[k].t_max;
    if (t_max > CFG_TEMP_MAX || t_max - (int)r->t_start > CFG_TEMP_RISE_MAX) return RC_E11;

    return RC_PASS;
}
