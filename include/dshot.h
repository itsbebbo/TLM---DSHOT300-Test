#pragma once
#include <stdint.h>

/* DShot300 generator. Knows nothing about the test sequence.
 *
 * Frames are emitted continuously at CFG_DSHOT_FRAME_HZ from an esp_timer
 * callback, from the moment dshot_start() is called until dshot_stop().
 * BLHeli_32 treats signal loss as a fault, so the stream is never used as a way
 * of stopping the motor - set the throttle to 0 instead. */

bool     dshot_init(void);
void     dshot_start(void);
void     dshot_stop(void);
bool     dshot_is_running(void);

/* 0 = motor stop, or 48..2047. Values 1..47 are rejected: they are commands. */
void     dshot_set_throttle(uint16_t value);
uint16_t dshot_get_throttle(void);

/* Only commands 0, 1 and 6 are accepted. Anything that changes spin direction
 * or writes the ESC flash is refused - this is a tester, not a configurator. */
bool     dshot_send_command(uint8_t cmd);

/* Consecutive 1 kHz deadlines missed because the previous frame had not
 * finished transmitting. Above CFG_DSHOT_MISS_LIMIT the generator is starved
 * and the run should abort. */
uint32_t dshot_missed_frames(void);

/* Frames sent with the telemetry bit set, since boot. One of these is one
 * answer asked of the ESC, so the difference across a window is what a
 * telemetry frame count for that window should be measured against - rather
 * than a nominal rate that assumes the frame timer never slipped. */
uint32_t dshot_telem_requests(void);
