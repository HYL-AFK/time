#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t cx1000am_init(void);
esp_err_t cx1000am_play_track(uint16_t track);
bool cx1000am_report_hour(uint8_t hour);
