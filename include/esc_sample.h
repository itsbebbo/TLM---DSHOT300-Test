#pragma once
#include <stdint.h>

/* One decoded telemetry frame. Deliberately free of any hardware dependency so
 * the verdict logic can be compiled and tested on a host machine. */
typedef struct {
    uint32_t t_ms;              /* Arrival time, millis() */
    uint8_t  temp_c;            /* degrees C */
    uint16_t voltage_cv;        /* 0.01 V */
    uint16_t current_ca;        /* 0.01 A */
    uint16_t consumption_mah;   /* mAh */
    uint32_t erpm;              /* already multiplied by 100 */
} esc_sample_t;

static inline float sample_volts(const esc_sample_t *s) { return s->voltage_cv * 0.01f; }
static inline float sample_amps (const esc_sample_t *s) { return s->current_ca * 0.01f; }
