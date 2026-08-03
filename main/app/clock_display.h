#pragma once

#include "esp_err.h"

#include "clock_logic.h"

esp_err_t clock_display_init(void);
void clock_display_post_event(clock_event_t event);
void clock_display_request_refresh(void);
