#pragma once

/* LVGL is not thread-safe: both calls below, and everything they reach, run on
 * ui_task and nowhere else. */

void ui_init(void);     /* once, from ui_task */
void ui_tick(void);     /* ~30 Hz, from ui_task */
