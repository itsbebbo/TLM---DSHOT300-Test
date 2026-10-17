#include "dshot.h"
#include "config.h"

#include <atomic>
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "dshot";

static rmt_channel_handle_t s_chan    = nullptr;
static rmt_encoder_handle_t s_encoder = nullptr;
static esp_timer_handle_t   s_timer   = nullptr;

/* Two banks so the buffer the RMT driver may still be reading is never the one
 * being encoded. At 53 us per frame against a 1000 us period one bank would do
 * in practice; the second costs 64 bytes and removes the question. */
static rmt_symbol_word_t s_sym[2][16];
static uint8_t           s_bank = 0;

static std::atomic<uint16_t> s_throttle{0};
static std::atomic<uint8_t>  s_cmd{0};
static std::atomic<uint8_t>  s_cmd_left{0};
static std::atomic<uint32_t> s_missed{0};
static std::atomic<uint32_t> s_telem_req{0};
static std::atomic<bool>     s_running{false};
static uint32_t              s_frame_ctr = 0;

static uint16_t build_frame(uint16_t value11, bool telem)
{
    const uint16_t v   = (uint16_t)((value11 << 1) | (telem ? 1u : 0u));
    const uint16_t crc = (uint16_t)((v ^ (v >> 4) ^ (v >> 8)) & 0x0F);   /* non-inverted: not bidirectional */
    return (uint16_t)((v << 4) | crc);
}

static void encode_frame(uint16_t frame, rmt_symbol_word_t *out)
{
    for (int i = 0; i < 16; i++) {
        const bool one = (frame & 0x8000) != 0;
        frame <<= 1;
        out[i].level0    = 1;
        out[i].duration0 = one ? CFG_DSHOT_T1H_TICKS : CFG_DSHOT_T0H_TICKS;
        out[i].level1    = 0;
        out[i].duration1 = one ? CFG_DSHOT_T1L_TICKS : CFG_DSHOT_T0L_TICKS;
    }
}

/* Runs on the esp_timer task (task dispatch, high priority), not in an ISR, so
 * calling the RMT driver here is allowed. It must never block: a stalled frame
 * is counted as a missed deadline and skipped. */
static void frame_tick(void *)
{
    if (!s_running.load()) return;

    if (rmt_tx_wait_all_done(s_chan, 0) != ESP_OK) {
        s_missed.fetch_add(1);
        return;
    }
    s_missed.store(0);

    uint16_t value;
    bool     telem;
    const uint8_t left = s_cmd_left.load();
    if (left > 0) {
        /* Special commands are sent with the telemetry bit set, as Betaflight does. */
        value = s_cmd.load();
        telem = true;
        s_cmd_left.store(left - 1);
    } else {
        value = s_throttle.load();
        telem = (s_frame_ctr % CFG_TELEM_REQUEST_DIV) == 0;
    }
    s_frame_ctr++;
    if (telem) s_telem_req.fetch_add(1);

    s_bank ^= 1;
    encode_frame(build_frame(value, telem), s_sym[s_bank]);

    rmt_transmit_config_t tx = {};
    tx.loop_count      = 0;
    tx.flags.eot_level = 0;     /* DShot idles low between frames */
    rmt_transmit(s_chan, s_encoder, s_sym[s_bank], sizeof(s_sym[s_bank]), &tx);
}

bool dshot_init(void)
{
    /* Port A carries an I2C pull-up, so until RMT owns the pin it sits high.
     * DShot idles low: drive it low before anything else touches it. */
    gpio_reset_pin((gpio_num_t)CFG_DSHOT_PIN);
    gpio_set_direction((gpio_num_t)CFG_DSHOT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)CFG_DSHOT_PIN, 0);

    rmt_tx_channel_config_t ch = {};
    ch.gpio_num          = (gpio_num_t)CFG_DSHOT_PIN;
    ch.clk_src           = RMT_CLK_SRC_DEFAULT;       /* APB, 80 MHz on the ESP32 */
    ch.resolution_hz     = CFG_DSHOT_RESOLUTION_HZ;
    ch.mem_block_symbols = CFG_DSHOT_RMT_SYMBOLS;     /* one RMT block; a frame needs 16 */
    ch.trans_queue_depth = 4;
    if (rmt_new_tx_channel(&ch, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed");
        return false;
    }

    rmt_copy_encoder_config_t enc = {};
    if (rmt_new_copy_encoder(&enc, &s_encoder) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed");
        return false;
    }
    if (rmt_enable(s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed");
        return false;
    }

    esp_timer_create_args_t targs = {};
    targs.callback        = frame_tick;
    targs.dispatch_method = ESP_TIMER_TASK;
    targs.name            = "dshot";
    if (esp_timer_create(&targs, &s_timer) != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed");
        return false;
    }
    return true;
}

void dshot_start(void)
{
    if (s_running.load()) return;
    s_missed.store(0);
    /* The timer is stopped here, so frame_tick cannot be touching the counter.
     * Starting from 0 puts the telemetry bit on the first frame out, so the ESC
     * is asked for telemetry immediately rather than up to 10 ms later. */
    s_frame_ctr = 0;
    s_running.store(true);      /* before the timer: frame_tick ignores ticks without it */

    const esp_err_t err = esp_timer_start_periodic(s_timer, 1000000u / CFG_DSHOT_FRAME_HZ);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* Nothing will be transmitted. Say so, rather than let callers believe in
         * a stream that does not exist; ensure_signal() then keeps trying. */
        s_running.store(false);
        static bool logged = false;
        if (!logged) { ESP_LOGE(TAG, "frame timer would not start (%d)", (int)err); logged = true; }
    }
}

void dshot_stop(void)
{
    if (!s_running.load()) return;
    esp_timer_stop(s_timer);
    s_running.store(false);
    rmt_tx_wait_all_done(s_chan, 5);   /* let the last frame finish; line then idles low */
}

bool dshot_is_running(void)
{
    return s_running.load();
}

void dshot_set_throttle(uint16_t value)
{
    if (value != 0 && value < 48) {
        ESP_LOGW(TAG, "throttle %u is a command value; refused", value);
        return;
    }
    if (value > 2047) value = 2047;
    s_throttle.store(value);
}

uint16_t dshot_get_throttle(void)
{
    return s_throttle.load();
}

bool dshot_send_command(uint8_t cmd)
{
    if (cmd != 0 && cmd != 1 && cmd != 6) {
        ESP_LOGE(TAG, "command %u is not allowed on a tester", cmd);
        return false;
    }
    if (s_throttle.load() != 0) {
        ESP_LOGW(TAG, "command %u refused: motor not stopped", cmd);
        return false;
    }
    s_cmd.store(cmd);
    s_cmd_left.store(CFG_DSHOT_CMD_REPEATS);
    return true;
}

uint32_t dshot_missed_frames(void)
{
    return s_missed.load();
}

uint32_t dshot_telem_requests(void)
{
    return s_telem_req.load();
}
