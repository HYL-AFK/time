#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "app_config.h"
#include "clock_logic.h"

typedef void (*time_service_sync_finished_cb_t)(void);

esp_err_t time_service_init(const app_config_t *config, time_service_sync_finished_cb_t on_finished);
void time_service_start_sync(void);
void time_service_cancel_sync(void);
void time_service_start_demo(void);
bool time_service_get_local(clock_time_t *out);
bool time_service_has_valid_time(void);
