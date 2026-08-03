#include "blufi_security.h"

#include <stdlib.h>
#include <string.h>

#include "esp_blufi_api.h"
#include "esp_crc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/aes.h"
#include "mbedtls/dhm.h"
#include "mbedtls/md5.h"

#define DH_SELF_PUB_KEY_LEN 128U
#define SHARE_KEY_LEN 128U
#define PSK_LEN 16U
#define DH_PARAM_MAX_LEN 512U

#define SEC_TYPE_DH_PARAM_LEN 0x00U
#define SEC_TYPE_DH_PARAM_DATA 0x01U

static const char *TAG = "blufi_sec";

typedef struct {
    uint8_t self_public_key[DH_SELF_PUB_KEY_LEN];
    uint8_t share_key[SHARE_KEY_LEN];
    size_t share_len;
    uint8_t psk[PSK_LEN];
    uint8_t *dh_param;
    int dh_param_len;
    uint8_t iv[16];
    mbedtls_dhm_context dhm;
    mbedtls_aes_context aes;
} blufi_security_ctx_t;

static blufi_security_ctx_t *s_security;

extern void btc_blufi_report_error(esp_blufi_error_state_t state);

static int blufi_random(void *rng_state, unsigned char *output, size_t len)
{
    (void)rng_state;
    esp_fill_random(output, len);
    return 0;
}

void blufi_dh_negotiate_data_handler(uint8_t *data,
                                     int len,
                                     uint8_t **output_data,
                                     int *output_len,
                                     bool *need_free)
{
    if (data == NULL || len < 3 || output_data == NULL || output_len == NULL || need_free == NULL) {
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }
    if (s_security == NULL) {
        ESP_LOGE(TAG, "Security context is not initialized");
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }

    switch (data[0]) {
    case SEC_TYPE_DH_PARAM_LEN:
        s_security->dh_param_len = (data[1] << 8) | data[2];
        if (s_security->dh_param_len <= 0 || s_security->dh_param_len > DH_PARAM_MAX_LEN) {
            ESP_LOGE(TAG, "Invalid DH parameter length: %d", s_security->dh_param_len);
            s_security->dh_param_len = 0;
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }
        free(s_security->dh_param);
        s_security->dh_param = malloc((size_t)s_security->dh_param_len);
        if (s_security->dh_param == NULL) {
            s_security->dh_param_len = 0;
            btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
        }
        break;

    case SEC_TYPE_DH_PARAM_DATA: {
        if (s_security->dh_param == NULL || len < s_security->dh_param_len + 1) {
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }

        memcpy(s_security->dh_param, &data[1], (size_t)s_security->dh_param_len);
        uint8_t *param = s_security->dh_param;
        int ret = mbedtls_dhm_read_params(&s_security->dhm,
                                          &param,
                                          &param[s_security->dh_param_len]);
        free(s_security->dh_param);
        s_security->dh_param = NULL;
        if (ret != 0) {
            ESP_LOGE(TAG, "Read DH parameters failed: %d", ret);
            btc_blufi_report_error(ESP_BLUFI_READ_PARAM_ERROR);
            return;
        }

        const int dhm_len = mbedtls_dhm_get_len(&s_security->dhm);
        if (dhm_len <= 0 || dhm_len > DH_SELF_PUB_KEY_LEN) {
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }
        ret = mbedtls_dhm_make_public(&s_security->dhm,
                                      dhm_len,
                                      s_security->self_public_key,
                                      sizeof(s_security->self_public_key),
                                      blufi_random,
                                      NULL);
        if (ret != 0) {
            ESP_LOGE(TAG, "Make DH public key failed: %d", ret);
            btc_blufi_report_error(ESP_BLUFI_MAKE_PUBLIC_ERROR);
            return;
        }
        ret = mbedtls_dhm_calc_secret(&s_security->dhm,
                                      s_security->share_key,
                                      sizeof(s_security->share_key),
                                      &s_security->share_len,
                                      blufi_random,
                                      NULL);
        if (ret != 0) {
            ESP_LOGE(TAG, "Calculate DH secret failed: %d", ret);
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }
        if (mbedtls_md5(s_security->share_key, s_security->share_len, s_security->psk) != 0 ||
            mbedtls_aes_setkey_enc(&s_security->aes, s_security->psk, PSK_LEN * 8U) != 0) {
            btc_blufi_report_error(ESP_BLUFI_CALC_MD5_ERROR);
            return;
        }
        *output_data = s_security->self_public_key;
        *output_len = dhm_len;
        *need_free = false;
        break;
    }

    default:
        break;
    }
}

static int crypt_payload(uint8_t iv8, uint8_t *data, int len, int mode)
{
    if (s_security == NULL || data == NULL || len < 0) return -1;
    size_t offset = 0;
    uint8_t iv[sizeof(s_security->iv)];
    memcpy(iv, s_security->iv, sizeof(iv));
    iv[0] = iv8;
    return mbedtls_aes_crypt_cfb128(&s_security->aes, mode, (size_t)len, &offset, iv, data, data) == 0
               ? len
               : -1;
}

int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len)
{
    return crypt_payload(iv8, crypt_data, crypt_len, MBEDTLS_AES_ENCRYPT);
}

int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len)
{
    return crypt_payload(iv8, crypt_data, crypt_len, MBEDTLS_AES_DECRYPT);
}

uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len)
{
    (void)iv8;
    return data != NULL && len >= 0 ? esp_crc16_be(0, data, (size_t)len) : 0;
}

esp_err_t blufi_security_init(void)
{
    if (s_security != NULL) return ESP_OK;
    s_security = calloc(1, sizeof(*s_security));
    if (s_security == NULL) return ESP_ERR_NO_MEM;
    mbedtls_dhm_init(&s_security->dhm);
    mbedtls_aes_init(&s_security->aes);
    return ESP_OK;
}

void blufi_security_deinit(void)
{
    if (s_security == NULL) return;
    free(s_security->dh_param);
    mbedtls_dhm_free(&s_security->dhm);
    mbedtls_aes_free(&s_security->aes);
    free(s_security);
    s_security = NULL;
}
