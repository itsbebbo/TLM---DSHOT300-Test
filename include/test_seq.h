#pragma once
#include <stdint.h>
#include "verdict.h"

typedef enum {
    ST_IDLE = 0, ST_PRECHECK, ST_ARM, ST_RAMP, ST_SETTLE,
    ST_DWELL, ST_SPINDOWN, ST_VERDICT, ST_FAULT
} test_state_t;

const char *test_state_name(test_state_t s);

/* Snapshot for the UI. Contains no LVGL types; the UI pulls this and does all
 * widget work itself, on its own core. */
typedef struct {
    test_state_t  state;
    uint32_t      run_id;       /* increments on every START */
    uint8_t       point;        /* 1..N during a set point, 0 otherwise */
    uint8_t       n_points;
    uint8_t       progress;     /* 0..100 over the whole run */
    bool          link;         /* valid telemetry within CFG_TELEM_GAP_MS */
    float         v, i;
    uint8_t       temp;
    uint32_t      erpm, rpm;
    uint16_t      throttle;
    reason_code_t code;         /* valid in ST_VERDICT and ST_FAULT */
    run_data_t    data;         /* valid in ST_VERDICT */
} live_t;

void test_seq_init(void);
void test_seq_tick(void);       /* call at 100 Hz from control_task */

/* Safe to call from any task. They only raise a request flag; the state
 * machine acts on it on its next tick, on its own task. */
void test_seq_start(void);
void test_seq_abort(void);      /* operator STOP */
void test_seq_ack(void);        /* clears ST_VERDICT / ST_FAULT */
void test_seq_touch(void);      /* any touch; wakes the ESC after idle disarm */

void test_seq_snapshot(live_t *out);
