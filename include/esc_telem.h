#pragma once
#include <stdint.h>
#include "config.h"
#include "esc_sample.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* BLHeli_32 / KISS telemetry reader: 10 bytes, 115200 8N1, non-inverted.
 * Frames are normally delimited by the silence between bursts; if that fails
 * three times running (someone enabled Auto Telemetry on the ESC) the parser
 * switches to a sliding-window CRC search.
 *
 * The ESC only talks when a DShot frame asks it to, so for most of the tester's
 * life this pin is an input with nothing driving it. It is held at UART idle by
 * a pull-up, and a frame counts as telemetry only if it arrived as one burst,
 * passes CRC8, decodes inside the plausibility bounds in config.h, and is one
 * of CFG_TELEM_LOCK_FRAMES such frames in a row. A floating input and a
 * one-in-256 CRC are otherwise enough between them to invent a reading.
 *
 * With ESC_SIM_MODE the UART is never touched and a model of a healthy ESC and
 * motor feeds the same queue instead. */

bool          esc_telem_init(void);
QueueHandle_t esc_telem_queue(void);

/* Why frames were lost, not just how many. The split is what separates an ESC
 * that never answered (everything zero, good stalled) from noise on the wire
 * (rej_crc climbing) from a tester rejecting real frames (rej_span or
 * rej_range climbing). In the simulator only good and last_ms move. */
typedef struct {
    uint32_t good;        /* frames published to the consumer */
    uint32_t bad;         /* total rejected, the sum of the three below */
    uint32_t rej_span;    /* ten bytes that did not arrive as one burst */
    uint32_t rej_crc;     /* burst-shaped, but the CRC disagreed */
    uint32_t rej_range;   /* CRC passed, values an ESC could not produce */
    uint32_t held;        /* valid, but dropped while the link was locking on */
    uint32_t last_ms;     /* millis() of the last published frame */
    bool     sliding;     /* parser fell back to the sliding-window search */
} esc_telem_stats_t;

void esc_telem_get_stats(esc_telem_stats_t *out);

uint32_t esc_telem_good(void);
uint32_t esc_telem_bad(void);
uint32_t esc_telem_last_ms(void);   /* millis() of the last believed frame, 0 = never */
void     esc_telem_reset_stats(void);

#if ESC_SIM_MODE
typedef enum {
    SIM_FAULT_NONE = 0,
    SIM_FAULT_NO_TELEM,     /* expect E01 */
    SIM_FAULT_LOW_VOLTS,    /* expect E03 */
    SIM_FAULT_NO_CURRENT,   /* expect E06 */
    SIM_FAULT_OVERCURRENT,  /* expect E07 */
    SIM_FAULT_NO_SPIN,      /* expect E08 */
    SIM_FAULT_FLAT_RPM,     /* expect E09 */
    SIM_FAULT_DROPOUTS,     /* expect E10 */
    SIM_FAULT_HOT,          /* expect E11 */
    SIM_FAULT__COUNT
} sim_fault_t;

void        esc_sim_set_fault(sim_fault_t f);
sim_fault_t esc_sim_get_fault(void);
const char *esc_sim_fault_name(sim_fault_t f);
#endif
