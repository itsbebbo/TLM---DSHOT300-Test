#pragma once
#include <stdint.h>
#include "config.h"
#include "esc_sample.h"

/* Pure decision logic. No hardware, no FreeRTOS - compile it on a host and feed
 * it recorded sample sets to test every reason code. */

typedef enum {
    RC_PASS = 0,
    RC_E01, RC_E02, RC_E03, RC_E04, RC_E05, RC_E06, RC_E07,
    RC_E08, RC_E09, RC_E10, RC_E11, RC_E12, RC_E13
} reason_code_t;

const char *reason_code_str(reason_code_t rc);   /* "E06" */
const char *reason_code_text(reason_code_t rc);  /* plain-language likely cause */

/* Samples collected during one dwell, before reduction. */
typedef struct {
    uint16_t n;
    uint16_t v[CFG_MAX_DWELL_SAMPLES];
    uint16_t i[CFG_MAX_DWELL_SAMPLES];
    uint32_t e[CFG_MAX_DWELL_SAMPLES];
    uint8_t  t_max;
} dwell_acc_t;

/* The reduced form the verdict is taken from. */
typedef struct {
    uint16_t n;          /* valid frames received */
    uint16_t expected;   /* telemetry requests made */
    uint8_t  valid_pct;
    float    v_med;
    float    i_med;
    uint32_t erpm_med;
    uint8_t  t_max;
} dwell_stats_t;

typedef struct {
    float         v_precheck;
    uint8_t       t_start;
    uint8_t       n_points;
    dwell_stats_t d[CFG_MAX_SETPOINTS];
} run_data_t;

void dwell_acc_reset(dwell_acc_t *a);
void dwell_acc_add(dwell_acc_t *a, const esc_sample_t *s);
void dwell_acc_finish(const dwell_acc_t *a, uint16_t expected, dwell_stats_t *out);

/* Checks run in reason-code order across all dwells; the first failure wins.
 * E01, E02, E12 and E13 are never returned here - they belong to the state
 * machine, which sees them before there is any dwell data to judge. */
reason_code_t verdict_evaluate(const run_data_t *r);
