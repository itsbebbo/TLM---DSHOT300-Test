#include <Arduino.h>
#include <M5Unified.h>

#include "config.h"
#include "dshot.h"
#include "esc_telem.h"
#include "test_seq.h"
#include "ui.h"

/* Task layout
 *   esp_timer task   high prio  1 kHz   DShot frames (dshot.cpp)
 *   telem / sim      18, core 1          UART framing + CRC, or synthetic samples
 *   control          10, core 1  100 Hz  state machine, trips, verdict
 *   ui                5, core 0   30 Hz  M5.update, LVGL, touch, tones
 * The Core2's ESP32 is dual core like the S3 it was ported from, so the display
 * still stays on core 0 and a redraw can never delay the control loop. */

static void control_task(void *)
{
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        test_seq_tick();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(10));
    }
}

static void ui_task(void *)
{
    ui_init();
    for (;;) {
        M5.update();
        ui_tick();
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

static void halt(const char *why)
{
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_RED);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 10);
    M5.Display.println(why);
    Serial.println(why);
    for (;;) delay(1000);
}

void setup()
{
    /* Port A carries an I2C pull-up and DShot idles low: claim the pin before
     * anything else runs, M5.begin() included. dshot_init() takes it over
     * properly shortly after. */
    pinMode(CFG_DSHOT_PIN, OUTPUT);
    digitalWrite(CFG_DSHOT_PIN, LOW);

    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    /* No 5 V out of the Grove ports: the E80 has no BEC, and there is nothing on
     * Port A to power. On the Core2 this is the AXP192 boost converter, which
     * M5.begin() would otherwise bring up. */
    cfg.output_power    = false;
    M5.begin(cfg);
    M5.Speaker.setVolume(128);

    if (!dshot_init())     halt("DShot init failed");
    if (!esc_telem_init()) halt("Telemetry init failed");
    test_seq_init();               /* starts the DShot 0 stream */

#if ESC_SIM_MODE
    Serial.println("ESC tester: SIMULATION MODE - no ESC required");
#else
    Serial.println("ESC tester: hardware mode");
#endif

    xTaskCreatePinnedToCore(control_task, "control", 6144, nullptr, 10, nullptr, 1);
    xTaskCreatePinnedToCore(ui_task,      "ui",      8192, nullptr, 5,  nullptr, 0);
}

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
