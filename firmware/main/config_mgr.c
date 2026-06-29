/*
 * Configuration manager with NVS persistence.
 *
 * Stores WiFi, service port, and audio I2S settings in flash via NVS.
 * A mutex protects the in-memory config struct; setters persist to NVS
 * immediately so settings survive reset.
 */

#include "config_mgr.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_log.h"

#include "board_config.h"

static const char *TAG = "config_mgr";
static const char *NVS_NAMESPACE = "streamer";

static SemaphoreHandle_t s_mutex = NULL;
static device_config_t s_config;
static bool s_initialized = false;

/* ---- Defaults from board_config.h ---- */

static void set_defaults(device_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->wifi_ssid, WIFI_SSID_DEFAULT, sizeof(cfg->wifi_ssid) - 1);
    strncpy(cfg->wifi_password, WIFI_PASSWORD_DEFAULT, sizeof(cfg->wifi_password) - 1);
    strncpy(cfg->hostname, WIFI_HOSTNAME_DEFAULT, sizeof(cfg->hostname) - 1);
    cfg->tx_power = WIFI_TX_POWER_DEFAULT;
    cfg->svc_port = SVC_PORT_DEFAULT;
    cfg->sample_rate = AUDIO_SAMPLE_RATE_DEFAULT;
    cfg->bits_per_sample = I2S_BITS_PER_SAMPLE;
    cfg->comm_format = I2S_COMM_FORMAT_CFG;
    cfg->channel_format = I2S_CHANNEL_FORMAT;
    cfg->gain = AUDIO_GAIN_DEFAULT;
    cfg->agc_mode = AUDIO_AGC_DEFAULT;
    cfg->codec_mode = AUDIO_CODEC_DEFAULT;
    cfg->rawtx_mode = RAWTX_MODE_DEFAULT;
    cfg->wifi_channel = RAWTX_CHANNEL_DEFAULT;
}

/* ---- NVS load/save ---- */

static esp_err_t load_from_nvs(device_config_t *cfg)
{
    nvs_handle h;
    /* Обнуляем структуру перед загрузкой, чтобы отсутствующие в NVS ключи
     * не оставили мусор от предыдущего состояния. */
    memset(cfg, 0, sizeof(*cfg));

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        return err;
    }

    size_t len;

    len = sizeof(cfg->wifi_ssid);
    nvs_get_str(h, "ssid", cfg->wifi_ssid, &len);
    len = sizeof(cfg->wifi_password);
    nvs_get_str(h, "pass", cfg->wifi_password, &len);
    len = sizeof(cfg->hostname);
    nvs_get_str(h, "host", cfg->hostname, &len);

    nvs_get_u8(h, "txpwr", &cfg->tx_power);
    nvs_get_u16(h, "svcport", &cfg->svc_port);
    nvs_get_u32(h, "rate", &cfg->sample_rate);
    nvs_get_u8(h, "bits", &cfg->bits_per_sample);
    nvs_get_u8(h, "fmt", &cfg->comm_format);
    nvs_get_u8(h, "ch", &cfg->channel_format);
    nvs_get_u8(h, "gain", &cfg->gain);
    nvs_get_u8(h, "agc", &cfg->agc_mode);
    nvs_get_u8(h, "codec", &cfg->codec_mode);
    nvs_get_u8(h, "rawtx", &cfg->rawtx_mode);
    nvs_get_u8(h, "wch", &cfg->wifi_channel);

    nvs_close(h);
    return ESP_OK;
}

static esp_err_t save_to_nvs(const device_config_t *cfg)
{
    nvs_handle h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }

    nvs_set_str(h, "ssid", cfg->wifi_ssid);
    nvs_set_str(h, "pass", cfg->wifi_password);
    nvs_set_str(h, "host", cfg->hostname);
    nvs_set_u8(h, "txpwr", cfg->tx_power);
    nvs_set_u16(h, "svcport", cfg->svc_port);
    nvs_set_u32(h, "rate", cfg->sample_rate);
    nvs_set_u8(h, "bits", cfg->bits_per_sample);
    nvs_set_u8(h, "fmt", cfg->comm_format);
    nvs_set_u8(h, "ch", cfg->channel_format);
    nvs_set_u8(h, "gain", cfg->gain);
    nvs_set_u8(h, "agc", cfg->agc_mode);
    nvs_set_u8(h, "codec", cfg->codec_mode);
    nvs_set_u8(h, "rawtx", cfg->rawtx_mode);
    nvs_set_u8(h, "wch", cfg->wifi_channel);

    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---- Public API ---- */

esp_err_t config_mgr_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    set_defaults(&s_config);

    /* Try to load from NVS; if it fails, keep defaults and save them. */
    esp_err_t err = load_from_nvs(&s_config);
    if (err != ESP_OK)
    {
        ESP_LOGI(TAG, "No NVS config - saving defaults");
        set_defaults(&s_config); /* Восстанавливаем - load_from_nvs мог обнулить */
        save_to_nvs(&s_config);
    }
    else
    {
        ESP_LOGI(TAG, "Config loaded from NVS");
    }

    /* Validate loaded values, fall back to defaults if invalid. */
    if (!sample_rate_is_valid(s_config.sample_rate))
    {
        s_config.sample_rate = AUDIO_SAMPLE_RATE_DEFAULT;
    }
    if (s_config.bits_per_sample != 16 && s_config.bits_per_sample != 24)
    {
        s_config.bits_per_sample = I2S_BITS_PER_SAMPLE;
    }
    if (s_config.comm_format != I2S_CFMT_PHILIPS && s_config.comm_format != I2S_CFMT_LSB)
    {
        s_config.comm_format = I2S_COMM_FORMAT_CFG;
    }
    if (s_config.channel_format != I2S_CHFMT_LEFT &&
        s_config.channel_format != I2S_CHFMT_RIGHT &&
        s_config.channel_format != I2S_CHFMT_STEREO)
    {
        s_config.channel_format = I2S_CHANNEL_FORMAT;
    }
    if (s_config.tx_power > WIFI_TX_POWER_MAX)
    {
        s_config.tx_power = WIFI_TX_POWER_DEFAULT;
    }
    if (s_config.gain > 64)
    {
        s_config.gain = AUDIO_GAIN_DEFAULT;
    }
    if (s_config.agc_mode > AGC_MODE_HIGH)
    {
        s_config.agc_mode = AUDIO_AGC_DEFAULT;
    }
    if (s_config.codec_mode > CODEC_MODE_PCM)
    {
        s_config.codec_mode = AUDIO_CODEC_DEFAULT;
    }
    if (s_config.rawtx_mode > 1)
    {
        s_config.rawtx_mode = 0;
    }
    if (s_config.wifi_channel < 1 || s_config.wifi_channel > 13)
    {
        s_config.wifi_channel = 1;
    }
    ESP_LOGI(TAG, "Runtime audio: %u Hz, %d ms, %d-bit, fmt=%d, ch=%d, gain=%u, agc=%u, codec=%u",
             (unsigned)s_config.sample_rate, 20,
             s_config.bits_per_sample, s_config.comm_format,
             s_config.channel_format, (unsigned)s_config.gain,
             (unsigned)s_config.agc_mode, (unsigned)s_config.codec_mode);

    s_initialized = true;
    return ESP_OK;
}

void config_get_copy(device_config_t *cfg)
{
    if (!s_initialized || !cfg)
    {
        if (cfg)
            set_defaults(cfg);
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *cfg = s_config;
    xSemaphoreGive(s_mutex);
}

static esp_err_t save_locked(void)
{
    return save_to_nvs(&s_config);
}

esp_err_t config_set_wifi(const char *ssid, const char *password)
{
    if (!ssid || !password || !ssid[0] || !password[0])
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(ssid) >= sizeof(s_config.wifi_ssid) ||
        strlen(password) >= sizeof(s_config.wifi_password))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_config.wifi_ssid, ssid, sizeof(s_config.wifi_ssid) - 1);
    s_config.wifi_ssid[sizeof(s_config.wifi_ssid) - 1] = '\0';
    strncpy(s_config.wifi_password, password, sizeof(s_config.wifi_password) - 1);
    s_config.wifi_password[sizeof(s_config.wifi_password) - 1] = '\0';
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS");
    return err;
}

esp_err_t config_set_hostname(const char *hostname)
{
    if (!hostname || !hostname[0])
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(hostname) >= sizeof(s_config.hostname))
    {
        return ESP_ERR_INVALID_SIZE;
    }
    /* RFC 952: alphanumeric + hyphens, must start with letter.
     * We're lenient here - just reject clearly invalid chars. */
    for (const char *p = hostname; *p; p++)
    {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-'))
        {
            ESP_LOGE(TAG, "Invalid hostname char '%c' (0x%02X)", c, (unsigned char)c);
            return ESP_ERR_INVALID_ARG;
        }
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_config.hostname, hostname, sizeof(s_config.hostname) - 1);
    s_config.hostname[sizeof(s_config.hostname) - 1] = '\0';
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS");
    return err;
}

esp_err_t config_set_tx_power(uint8_t tx_power)
{
    if (tx_power > WIFI_TX_POWER_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.tx_power = tx_power;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS");
    return err;
}

esp_err_t config_set_svc_port(uint16_t port)
{
    if (port == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.svc_port = port;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS");
    return err;
}

esp_err_t config_set_sample_rate(uint32_t rate)
{
    if (!sample_rate_is_valid(rate))
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.sample_rate = rate;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS");
    return err;
}

esp_err_t config_set_bits_per_sample(uint8_t bits)
{
    if (bits != 16 && bits != 24)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.bits_per_sample = bits;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS");
    return err;
}

esp_err_t config_set_comm_format(uint8_t fmt)
{
    if (fmt != I2S_CFMT_PHILIPS && fmt != I2S_CFMT_LSB)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.comm_format = fmt;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS");
    return err;
}

esp_err_t config_set_channel_format(uint8_t fmt)
{
    if (fmt != I2S_CHFMT_LEFT && fmt != I2S_CHFMT_RIGHT && fmt != I2S_CHFMT_STEREO)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.channel_format = fmt;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS");
    return err;
}

esp_err_t config_set_gain(uint8_t gain)
{
    if (gain > 64)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.gain = gain;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS (gain=%u)", (unsigned)gain);
    return err;
}

esp_err_t config_set_agc_mode(uint8_t mode)
{
    if (mode > AGC_MODE_HIGH)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.agc_mode = mode;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS (agc_mode=%u)", (unsigned)mode);
    return err;
}

esp_err_t config_set_codec_mode(uint8_t mode)
{
    if (mode > CODEC_MODE_PCM)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.codec_mode = mode;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS (codec_mode=%u)", (unsigned)mode);
    return err;
}

esp_err_t config_set_rawtx_mode(uint8_t mode)
{
    if (mode > 1)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.rawtx_mode = mode;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS (rawtx_mode=%u)", (unsigned)mode);
    return err;
}

esp_err_t config_set_wifi_channel(uint8_t ch)
{
    if (ch < 1 || ch > 13)
    {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_config.wifi_channel = ch;
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "Config saved to NVS (wifi_channel=%u)", (unsigned)ch);
    return err;
}

esp_err_t config_factory_reset(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    set_defaults(&s_config);
    esp_err_t err = save_locked();
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Factory reset - defaults restored");
    return err;
}
