#pragma once
#include <stdint.h>

/* -------------------------------------------------------------------------
 * ESC Tester configuration.
 *
 * Every value the tester behaves by lives here. Nothing in this file should be
 * duplicated as a literal anywhere else in the firmware.
 * ---------------------------------------------------------------------- */

/* ---- Build options ---------------------------------------------------- */

/* 1 = synthetic telemetry generated on-device, no ESC or motor required.
 * The Home screen then shows a SIM button that cycles injected faults so every
 * reason code can be exercised on a bare Core2. Set to 0 for the real bench. */
#define ESC_SIM_MODE            0

/* Chirp the ESC (DShot command 1) at the start of arming. Confirms the signal
 * wire end to end before anything spins. Harmless: the motor is stopped. */
#define CFG_BEEP_ON_START       1

/* ---- Pins and protocol ------------------------------------------------- */

/* Core2 Port A: Grove pin 2 = GPIO32 (SDA), Grove pin 1 = GPIO33 (SCL). Both are
 * ordinary output-capable GPIOs on the classic ESP32 - GPIO34..39 are not, which
 * is why the telemetry line cannot simply be moved up there. */
#define CFG_DSHOT_PIN           32          /* Port A SDA line, Grove pin 2 */
#define CFG_TELEM_PIN           33          /* Port A SCL line, Grove pin 1, UART1 RX */
#define CFG_DSHOT_RESOLUTION_HZ 80000000u   /* RMT tick source: 12.5 ns tick */

/* One RMT memory block. The driver requires a multiple of the per-channel word
 * count, which is 64 on the classic ESP32 (it is 48 on the S3 - this is not a
 * value to carry across from a CoreS3 build). A frame needs 16. */
#define CFG_DSHOT_RMT_SYMBOLS   64
#define CFG_DSHOT_FRAME_HZ      1000        /* Continuous frame rate */
#define CFG_TELEM_REQUEST_DIV   10          /* Telemetry bit every Nth frame -> 100 Hz */
#define CFG_TELEM_BAUD          115200
#define CFG_TELEM_UART          UART_NUM_1

/* Nothing drives the telemetry line until the ESC is powered and wired, and an
 * unheld input picks up enough noise for the UART to decode bytes out of it.
 * Hold the pin at the UART idle level instead. Pull-UP, not down: a line held
 * low is a start bit that never ends, which is worse than floating. This does
 * not rely on Port A's own pull-up being populated. */
#define CFG_TELEM_PULLUP        1

/* ---- Telemetry frame validation ---------------------------------------- */

/* A genuine 10-byte frame occupies 868 us on the wire at 115200 baud. Ten bytes
 * that took much longer than that to arrive are not one burst, whatever their
 * CRC says; noise dribbling in a byte at a time is exactly that shape. */
#define CFG_FRAME_SPAN_MAX_US   2000

/* Random bytes pass CRC8 once in 256 tries, so on a noisy line the CRC alone is
 * not enough to believe a frame. These bounds throw out the survivors that
 * decode to nonsense. They are deliberately far wider than the pass thresholds:
 * they say "an ESC could have sent this", not "this is a good ESC". A genuine
 * fault must still reach the verdict logic as data, not be filtered away here. */
#define CFG_PLAUS_V_MIN         100u        /* 1.00 V, in 0.01 V. Not zero: a line
                                             * stuck low delivers 0x00 bytes, and
                                             * ten of those are a CRC-valid frame */
#define CFG_PLAUS_V_MAX         6000u       /* 60.00 V */
#define CFG_PLAUS_I_MAX         20000u      /* 200.00 A, in 0.01 A */
#define CFG_PLAUS_TEMP_MAX      150         /* degrees C */
#define CFG_PLAUS_ERPM_MAX      500000u     /* eRPM */

/* Valid frames in a row, arriving no more than CFG_TELEM_GAP_MS apart, before
 * the link counts as up. One frame decoded out of noise then cannot light the
 * link indicator or feed the verdict. Costs 20 ms of data on a re-lock. */
#define CFG_TELEM_LOCK_FRAMES   3

/* One diagnostic line a second on the serial monitor, plus a line on every
 * fault and a per-set-point summary at the end of a run. This is what tells an
 * E10 caused by the ESC not answering apart from one caused by the tester
 * rejecting the answers. Costs nothing but console noise; leave it on. */
#define CFG_DIAG_LOG            1

/* DShot300 symbols at 80 MHz. 800/3 is not an integer number of ticks, so the
 * bit period lands at 3.3375 us => 299.6 kbit/s, 0.12 % slow. Far inside what
 * BLHeli_32 bitrate auto-detection accepts. */
#define CFG_DSHOT_T0H_TICKS     100         /* 1.250 us */
#define CFG_DSHOT_T0L_TICKS     167         /* 2.0875 us */
#define CFG_DSHOT_T1H_TICKS     200         /* 2.500 us */
#define CFG_DSHOT_T1L_TICKS     67          /* 0.8375 us */

#define CFG_DSHOT_CMD_REPEATS   10          /* Frames a special command is held for */

/* ---- Sequence timing (ms) ---------------------------------------------- */

#define CFG_PRECHECK_MS         500
#define CFG_ARM_MS              1500
#define CFG_RAMP_MS             300
#define CFG_SETTLE_MS           500
#define CFG_DWELL_MS            2000
#define CFG_SPINDOWN_MS         800

/* ---- Set points and motor ---------------------------------------------- */

#define CFG_MAX_SETPOINTS       4
#define CFG_N_SETPOINTS         3
static const float CFG_SETPOINT_PCT[CFG_N_SETPOINTS] = { 6.0f, 10.0f, 15.0f };

#define CFG_MOTOR_POLES         12          /* Fixture motor; display RPM only */

/* ---- Pass thresholds --------------------------------------------------- */

#define CFG_V_MIN               15.0f       /* Bench is fixed at 6S */
#define CFG_V_MAX               34.0f
#define CFG_V_SAG_MAX           2.0f
static const float CFG_I_MIN[CFG_N_SETPOINTS] = { 0.08f, 0.17f, 0.28f };
#define CFG_I_MAX               12.0f
#define CFG_ERPM_MIN            1000u
#define CFG_ERPM_RISE_MIN_PCT   10.0f
#define CFG_TELEM_VALID_PCT     80
#define CFG_TEMP_MAX            60
#define CFG_TEMP_RISE_MAX       15

/* Precheck gates */
#define CFG_PRECHECK_MIN_FRAMES 30
#define CFG_PRECHECK_I_MAX      0.5f        /* Nothing should already be loaded */

/* ---- Trips and watchdogs ----------------------------------------------- */

#define CFG_I_TRIP              20.0f
#define CFG_I_TRIP_SAMPLES      3
#define CFG_TEMP_TRIP           80
#define CFG_TELEM_GAP_MS        250

/* A DShot special command is a deliberate interruption: BLHeli_32 plays the
 * beep by pulsing the motor, and it does not answer telemetry requests while it
 * is doing so. Suspend the telemetry-gap watchdog for this long after the last
 * command frame, so a disruption the tester asked for is not read as a fault on
 * the ESC. Long enough for a beep, short against the 1500 ms arm window that
 * follows, which still has to show telemetry before anything spins. */
#define CFG_CMD_TELEM_GRACE_MS  400
#define CFG_FRAME_GAP_MS        3
#define CFG_RUN_WATCHDOG_MS     30000

/* Idle time after which the DShot stream stops and the ESC disarms and beeps.
 * 0 = never stop. The ESC answers telemetry requests, it does not stream on its
 * own, so a stopped stream is also a dead Home screen: no voltage, no link. The
 * default is therefore to keep transmitting DShot 0 and keep polling. Set this
 * to e.g. 60000 if you would rather have the disarm beep as a cue that the
 * bench is no longer driving the ESC, and accept losing telemetry with it. */
#define CFG_IDLE_DISARM_MS      0
#define CFG_SPINDOWN_ERPM       500u
#define CFG_DSHOT_MISS_LIMIT    5

/* ---- Derived ------------------------------------------------------------ */

#define CFG_MAX_DWELL_SAMPLES   256
#define CFG_TELEM_HZ            (CFG_DSHOT_FRAME_HZ / CFG_TELEM_REQUEST_DIV)

static_assert(CFG_N_SETPOINTS >= 2 && CFG_N_SETPOINTS <= CFG_MAX_SETPOINTS,
              "need 2..CFG_MAX_SETPOINTS set points");
static_assert(CFG_DWELL_MS / (1000 / CFG_TELEM_HZ) <= CFG_MAX_DWELL_SAMPLES,
              "dwell would overflow the sample buffer");

static inline uint16_t dshot_from_pct(float pct)
{
    if (pct <= 0.0f) return 48;
    if (pct >= 100.0f) return 2047;
    return (uint16_t)(48.0f + (pct * 1999.0f / 100.0f) + 0.5f);
}

static inline uint32_t erpm_to_rpm(uint32_t erpm)
{
    return (erpm * 2u) / CFG_MOTOR_POLES;
}
