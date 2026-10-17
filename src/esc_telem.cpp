#include "esc_telem.h"
#include "config.h"

#include <Arduino.h>
#include <atomic>
#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

#if ESC_SIM_MODE
#include "dshot.h"
#include "esp_random.h"
#else
#include "driver/gpio.h"
#include "driver/uart.h"
#endif

static const char *TAG = "telem";

static QueueHandle_t          s_queue = nullptr;
static std::atomic<uint32_t>  s_good{0};
static std::atomic<uint32_t>  s_bad{0};
static std::atomic<uint32_t>  s_last_ms{0};
static std::atomic<bool>      s_reset_req{false};

/* Why frames were thrown away, split by gate. A total on its own cannot tell
 * "the ESC never answered" from "it answered and we rejected it", which is the
 * first question to ask of any E10. Unused by the simulator. */
static std::atomic<uint32_t>  s_rej_span{0};    /* ten bytes, but not one burst */
static std::atomic<uint32_t>  s_rej_crc{0};     /* burst, but the CRC disagreed */
static std::atomic<uint32_t>  s_rej_range{0};   /* CRC passed, values impossible */
static std::atomic<uint32_t>  s_held{0};        /* good, but the link had not locked yet */
static std::atomic<bool>      s_sliding_now{false};

/* Hand a sample to the consumer. If the consumer has fallen behind, the oldest
 * sample is dropped: fresh data matters more than complete data. */
static void publish(esc_sample_t *s)
{
    s->t_ms = millis();
    if (xQueueSend(s_queue, s, 0) != pdTRUE) {
        esc_sample_t stale;
        xQueueReceive(s_queue, &stale, 0);
        xQueueSend(s_queue, s, 0);
    }
    s_good.fetch_add(1);
    s_last_ms.store(s->t_ms ? s->t_ms : 1);
}

#if !ESC_SIM_MODE
/* ======================================================================== */
/*  Real ESC: UART1 RX on GPIO33, Port A SCL                                */
/* ======================================================================== */

static uint8_t crc8_update(uint8_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

static bool frame_crc_ok(const uint8_t *f)
{
    uint8_t crc = 0;
    for (int i = 0; i < 9; i++) crc = crc8_update(crc, f[i]);
    return crc == f[9];
}

static void decode(const uint8_t *f, esc_sample_t *s)
{
    s->temp_c          = f[0];
    s->voltage_cv      = (uint16_t)((f[1] << 8) | f[2]);
    s->current_ca      = (uint16_t)((f[3] << 8) | f[4]);
    s->consumption_mah = (uint16_t)((f[5] << 8) | f[6]);
    s->erpm            = (uint32_t)((f[7] << 8) | f[8]) * 100u;
}

/* Could an ESC have sent this? The bounds are wide on purpose: this throws out
 * noise that happened to pass the CRC, it does not judge the ESC. Anything a
 * real but faulty ESC could report has to get through to the verdict logic. */
static bool plausible(const esc_sample_t *s)
{
    return s->temp_c     <= CFG_PLAUS_TEMP_MAX
        && s->voltage_cv >= CFG_PLAUS_V_MIN
        && s->voltage_cv <= CFG_PLAUS_V_MAX
        && s->current_ca <= CFG_PLAUS_I_MAX
        && s->erpm       <= CFG_PLAUS_ERPM_MAX;
}

/* Parser state; touched only by telem_task. */
static uint8_t s_buf[10];
static uint8_t s_idx      = 0;
static uint8_t s_fill     = 0;
static uint8_t s_bad_run  = 0;
static uint8_t s_lock_run = 0;      /* valid frames in a row, saturates at the lock count */
static bool    s_sliding  = false;
static int64_t s_last_us  = 0;      /* previous byte */
static int64_t s_start_us = 0;      /* first byte of the frame being assembled */
static int64_t s_lock_us  = 0;      /* previous plausible frame */

/* A frame that gets here has the right burst shape and a good CRC. It still has
 * to look like telemetry, and the link is only believed once
 * CFG_TELEM_LOCK_FRAMES plausible frames have arrived in a row, close together.
 * One frame conjured out of noise then cannot light the link indicator, satisfy
 * Precheck, or reach the verdict. Returns whether the frame was plausible - the
 * sliding window uses that to decide if it has really found the alignment. */
static bool accept(const uint8_t *f, int64_t now_us)
{
    esc_sample_t s;
    decode(f, &s);
    if (!plausible(&s)) {
        s_bad.fetch_add(1);
        s_rej_range.fetch_add(1);
        return false;
    }

    if (now_us - s_lock_us > (int64_t)CFG_TELEM_GAP_MS * 1000) s_lock_run = 0;
    s_lock_us = now_us;

    if (s_lock_run < CFG_TELEM_LOCK_FRAMES) {
        if (++s_lock_run < CFG_TELEM_LOCK_FRAMES) {
            s_held.fetch_add(1);        /* still proving itself */
            return true;
        }
        ESP_LOGI(TAG, "telemetry link up");
    }
    publish(&s);
    return true;
}

static void feed(uint8_t b, int64_t now_us)
{
    if (!s_sliding) {
        /* Normal mode: a silence longer than CFG_FRAME_GAP_MS starts a new frame. */
        if (now_us - s_last_us > (int64_t)CFG_FRAME_GAP_MS * 1000) s_idx = 0;
        s_last_us = now_us;
        if (s_idx == 0) s_start_us = now_us;

        s_buf[s_idx++] = b;
        if (s_idx < 10) return;
        s_idx = 0;

        /* Ten bytes, but did they arrive as one burst? A frame is 868 us on the
         * wire; noise trickling in a byte at a time is not a frame however the
         * CRC lands. It is not evidence of a framing problem either, so it must
         * not push the parser into the sliding window below. */
        if (now_us - s_start_us > CFG_FRAME_SPAN_MAX_US) {
            s_bad.fetch_add(1);
            s_rej_span.fetch_add(1);
            return;
        }

        if (frame_crc_ok(s_buf)) {
            accept(s_buf, now_us);
            s_bad_run = 0;
        } else {
            s_bad.fetch_add(1);
            s_rej_crc.fetch_add(1);
            if (++s_bad_run >= 3) {
                /* Well-shaped bursts that keep failing the CRC: the ESC is
                 * probably streaming with Auto Telemetry enabled, so the gaps
                 * are not frame boundaries. Hunt for CRC-valid windows instead. */
                ESP_LOGW(TAG, "gap framing failed 3x; switching to sliding window");
                s_sliding = true;
                s_fill    = 0;
                s_sliding_now.store(true);
            }
        }
    } else {
        /* Sliding window: any ten consecutive bytes that pass the CRC and look
         * like telemetry. A silence still discards what was collected - a frame
         * never straddles one. */
        if (now_us - s_last_us > (int64_t)CFG_FRAME_GAP_MS * 1000) s_fill = 0;
        s_last_us = now_us;

        memmove(s_buf, s_buf + 1, 9);
        s_buf[9] = b;
        if (s_fill < 10) s_fill++;
        if (s_fill == 10 && frame_crc_ok(s_buf) && accept(s_buf, now_us)) {
            s_fill = 0;     /* alignment found; the next frame starts ten bytes on */
        }
    }
}

static void telem_task(void *)
{
    for (;;) {
        if (s_reset_req.exchange(false)) {
            s_idx = s_fill = s_bad_run = s_lock_run = 0;
            s_sliding = false;
            s_sliding_now.store(false);
            uart_flush_input(CFG_TELEM_UART);
        }
        /* One byte per read so every byte gets its own timestamp. The driver
         * buffers, so a burst is drained in a tight loop; at 100 frames/s this
         * is ~1000 calls/s, negligible on the ESP32. */
        uint8_t b;
        if (uart_read_bytes(CFG_TELEM_UART, &b, 1, pdMS_TO_TICKS(20)) == 1) {
            feed(b, esp_timer_get_time());
        }
    }
}

static bool start_source(void)
{
    uart_config_t cfg = {};
    cfg.baud_rate  = CFG_TELEM_BAUD;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    if (uart_driver_install(CFG_TELEM_UART, 256, 0, 0, nullptr, 0) != ESP_OK) return false;
    if (uart_param_config(CFG_TELEM_UART, &cfg) != ESP_OK) return false;
    /* RX only. */
    if (uart_set_pin(CFG_TELEM_UART, UART_PIN_NO_CHANGE, CFG_TELEM_PIN,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return false;

#if CFG_TELEM_PULLUP
    /* Hold the line at UART idle. Until the ESC is powered and wired nothing
     * drives this pin, and an unheld input decodes noise as bytes - which is
     * what the validation in feed() and accept() then has to throw away. Do not
     * rely on Port A's own pull-up: assert it here. */
    gpio_set_pull_mode((gpio_num_t)CFG_TELEM_PIN, GPIO_PULLUP_ONLY);
#endif
    /* Whatever the line did while it was being configured is not telemetry. */
    uart_flush_input(CFG_TELEM_UART);

    return xTaskCreatePinnedToCore(telem_task, "telem", 3072, nullptr, 18, nullptr, 1) == pdPASS;
}

#else
/* ======================================================================== */
/*  Simulation: a model of a healthy ESC and motor, plus injectable faults  */
/* ======================================================================== */

static std::atomic<uint8_t> s_fault{SIM_FAULT_NONE};

static float jitter(float span)
{
    return ((float)(esp_random() & 0xFFFF) / 65535.0f * 2.0f - 1.0f) * span;
}

/* Answers only while DShot is running, as a real ESC only answers requests. */
static void sim_task(void *)
{
    TickType_t last   = xTaskGetTickCount();
    float      temp   = 27.0f;
    float      mah    = 0.0f;
    uint32_t   n      = 0;

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000 / CFG_TELEM_HZ));
        n++;
        if (!dshot_is_running()) continue;

        const sim_fault_t f = (sim_fault_t)s_fault.load();
        if (f == SIM_FAULT_NO_TELEM) continue;
        if (f == SIM_FAULT_DROPOUTS && (n % 3) != 0) { s_bad.fetch_add(1); continue; }

        const uint16_t thr = dshot_get_throttle();
        const float    pct = (thr >= 48) ? (thr - 48) * 100.0f / 1999.0f : 0.0f;

        /* Healthy model: roughly quadratic current, linear eRPM. At 6/10/15 %
         * this gives about 0.37/0.95/2.1 A and 7k/12k/18k eRPM. */
        float amps = (pct > 0.0f) ? 0.05f + 0.009f * pct * pct : 0.0f;
        float erpm = pct * 1200.0f;

        switch (f) {
        case SIM_FAULT_NO_CURRENT:  amps = 0.0f;                                   break;
        case SIM_FAULT_OVERCURRENT: if (pct > 0.0f) amps = 14.0f + 0.2f * pct;      break;
        case SIM_FAULT_NO_SPIN:     erpm = 0.0f; amps *= 3.0f;                      break;
        case SIM_FAULT_FLAT_RPM:    if (pct > 0.0f) erpm = 7000.0f;                 break;
        default:                                                                    break;
        }
        amps = fmaxf(0.0f, amps * (1.0f + jitter(0.05f)));
        erpm = fmaxf(0.0f, erpm * (1.0f + jitter(0.02f)));

        const float volts = (f == SIM_FAULT_LOW_VOLTS ? 14.2f : 25.0f) - 0.04f * amps + jitter(0.03f);

        const float target_t = (f == SIM_FAULT_HOT) ? 90.0f : 27.0f + 1.5f * amps;
        temp += (target_t - temp) * ((f == SIM_FAULT_HOT) ? 0.01f : 0.002f);
        mah  += amps * (1000.0f / CFG_TELEM_HZ) / 3600.0f;

        esc_sample_t s;
        s.temp_c          = (uint8_t)fminf(255.0f, fmaxf(0.0f, temp));
        s.voltage_cv      = (uint16_t)(fmaxf(0.0f, volts) * 100.0f + 0.5f);
        s.current_ca      = (uint16_t)(amps * 100.0f + 0.5f);
        s.consumption_mah = (uint16_t)mah;
        s.erpm            = (uint32_t)(erpm / 100.0f + 0.5f) * 100u;   /* ESC resolution is 100 eRPM */
        publish(&s);
    }
}

static bool start_source(void)
{
    ESP_LOGW(TAG, "SIMULATION MODE: telemetry is synthetic, the UART is not used");
    return xTaskCreatePinnedToCore(sim_task, "telem_sim", 3072, nullptr, 18, nullptr, 1) == pdPASS;
}

void esc_sim_set_fault(sim_fault_t f)
{
    if (f < SIM_FAULT__COUNT) s_fault.store((uint8_t)f);
}

sim_fault_t esc_sim_get_fault(void)
{
    return (sim_fault_t)s_fault.load();
}

const char *esc_sim_fault_name(sim_fault_t f)
{
    switch (f) {
    case SIM_FAULT_NONE:        return "healthy";
    case SIM_FAULT_NO_TELEM:    return "no telem";
    case SIM_FAULT_LOW_VOLTS:   return "low volts";
    case SIM_FAULT_NO_CURRENT:  return "no current";
    case SIM_FAULT_OVERCURRENT: return "overcurrent";
    case SIM_FAULT_NO_SPIN:     return "no spin";
    case SIM_FAULT_FLAT_RPM:    return "flat rpm";
    case SIM_FAULT_DROPOUTS:    return "dropouts";
    case SIM_FAULT_HOT:         return "hot";
    default:                    return "?";
    }
}
#endif

/* ======================================================================== */

bool esc_telem_init(void)
{
    s_queue = xQueueCreate(32, sizeof(esc_sample_t));
    if (!s_queue) return false;
    return start_source();
}

QueueHandle_t esc_telem_queue(void) { return s_queue; }
uint32_t      esc_telem_good(void)  { return s_good.load(); }
uint32_t      esc_telem_bad(void)   { return s_bad.load(); }
uint32_t      esc_telem_last_ms(void) { return s_last_ms.load(); }

void esc_telem_reset_stats(void)
{
    s_good.store(0);
    s_bad.store(0);
    s_rej_span.store(0);
    s_rej_crc.store(0);
    s_rej_range.store(0);
    s_held.store(0);
    s_reset_req.store(true);
}

void esc_telem_get_stats(esc_telem_stats_t *out)
{
    out->good      = s_good.load();
    out->bad       = s_bad.load();
    out->rej_span  = s_rej_span.load();
    out->rej_crc   = s_rej_crc.load();
    out->rej_range = s_rej_range.load();
    out->held      = s_held.load();
    out->last_ms   = s_last_ms.load();
    out->sliding   = s_sliding_now.load();
}
