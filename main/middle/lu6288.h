#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t lu6288_init(void);
/* Send a full <G>... frame using raw bytes. The caller must supply the text in the
 * module's expected charset (GB2312/GBK). UTF-8 Chinese is NOT understood by the module. */
esp_err_t lu6288_speak_raw(const uint8_t *data, size_t len);
/* Speak the time for the given 24-hour value (0..23), e.g. "现在是上午十点整". */
esp_err_t lu6288_report_hour(uint8_t hour);
