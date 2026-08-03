#pragma once

#include "esp_err.h"

#define CLOCK_BOOT_BUTTON_GPIO 9

typedef void (*button_long_press_cb_t)(void);

esp_err_t button_init(button_long_press_cb_t on_long_press);
