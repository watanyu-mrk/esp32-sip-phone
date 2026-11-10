#include "rtp_client.h"

#include <errno.h>
#include <string.h>

#include "audio_driver.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "rtp_jitter.h"

static const char *TAG = "RTP";

#define RTP_HEADER_LEN 12
#define RTP_MAX_PAYLOAD 200

static int s_sock = -1;
static struct sockaddr_in s_remote;
static uint16_t s_local_port;
static uint8_t s_pt = RTP_PAYLOAD_TYPE_PCMU;
static volatile bool s_running;
static TaskHandle_t s_tx_task;
static TaskHandle_t s_rx_task;
static rtp_jitter_buffer_t *s_jb;

static uint16_t s_seq;
static uint32_t s_ts;
static uint32_t s_ssrc;

static volatile uint32_t s_sent;
static volatile uint32_t s_recv;
static volatile uint32_t s_lost;
static volatile uint32_t s_ooo;
static volatile uint32_t s_jitter_ms;

static uint16_t s_last_seq;
static bool s_have_seq;

/* --------------------------------------------------------------- G.711 */

static uint8_t linear_to_ulaw(int16_t pcm)
{
    int sign = (pcm >> 8) & 0x80;
    if (sign) {
        pcm = (int16_t)-pcm;
    }
    if (pcm > 32635) {
        pcm = 32635;
    }
    pcm = (int16_t)(pcm + 0x84);

    int exponent = 7;
    for (int mask = 0x4000; exponent > 0 && (pcm & mask) == 0; exponent--, mask >>= 1) {
    }
    int mantissa = (pcm >> (exponent + 3)) & 0x0F;
    return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

static int16_t ulaw_to_linear(uint8_t u)
{
    u = (uint8_t)~u;
    int sign = u & 0x80;
    int exponent = (u >> 4) & 0x07;
    int mantissa = u & 0x0F;
    int sample = (((mantissa << 3) + 0x84) << exponent) - 0x84;
    return (int16_t)(sign ? -sample : sample);
}

static uint8_t linear_to_alaw(int16_t pcm)
{
    int sign = (pcm >= 0) ? 0x80 : 0x00;
    int v = (pcm >= 0) ? pcm : -pcm - 1;
    if (v > 32767) {
        v = 32767;
    }

    int exponent = 7;
    for (int mask = 0x4000; exponent > 0 && (v & mask) == 0; exponent--, mask >>= 1) {
    }
    int mantissa = (exponent == 0) ? ((v >> 4) & 0x0F) : ((v >> (exponent + 3)) & 0x0F);
    return (uint8_t)((sign | (exponent << 4) | mantissa) ^ 0x55);
}

static int16_t alaw_to_linear(uint8_t a)
{
    a ^= 0x55;
    int sign = a & 0x80;
    int exponent = (a >> 4) & 0x07;
    int mantissa = a & 0x0F;
    int v = (exponent == 0) ? ((mantissa << 4) + 8)
                            : (((mantissa << 4) + 0x108) << (exponent - 1));
    return (int16_t)(sign ? v : -v);
}

/* -------------------------------------------------------------- socket */

esp_err_t rtp_client_open(uint16_t *local_port)
{
    if (s_sock >= 0) {
        if (local_port != NULL) {
            *local_port = s_local_port;
        }
        return ESP_OK;
    }

    for (uint16_t port = RTP_LOCAL_PORT_MIN; port <= RTP_LOCAL_PORT_MAX; port++) {
        int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) {
            ESP_LOGE(TAG, "[RTP] socket() failed errno=%d", errno);
            return ESP_FAIL;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            s_sock = fd;
            s_local_port = port;
            if (local_port != NULL) {
                *local_port = port;
            }
            ESP_LOGI(TAG, "[RTP] Local UDP port %u", (unsigned)port);
            return ESP_OK;
        }
        close(fd);
    }

    ESP_LOGE(TAG, "[RTP] No free port in %d..%d", RTP_LOCAL_PORT_MIN, RTP_LOCAL_PORT_MAX);
    return ESP_FAIL;
}

/* ---------------------------------------------------------------- tasks */

static void rtp_tx_task(void *arg)
{
    (void)arg;
    int16_t pcm[AUDIO_FRAME_SAMPLES];
    int16_t out[AUDIO_FRAME_SAMPLES];
    uint8_t pkt[RTP_HEADER_LEN + AUDIO_FRAME_BYTES];
    uint8_t payload[RTP_MAX_PAYLOAD];

    ESP_LOGI(TAG, "[RTP] TX task started (pt=%u)", (unsigned)s_pt);

    while (s_running) {
        size_t got = 0;
        esp_err_t err = audio_read_pcm(pcm, AUDIO_FRAME_SAMPLES, &got);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        for (size_t i = got; i < AUDIO_FRAME_SAMPLES; i++) {
            pcm[i] = 0;
        }

        pkt[0] = 0x80;
        pkt[1] = s_pt;
        pkt[2] = (uint8_t)(s_seq >> 8);
        pkt[3] = (uint8_t)(s_seq & 0xFF);
        pkt[4] = (uint8_t)(s_ts >> 24);
        pkt[5] = (uint8_t)(s_ts >> 16);
        pkt[6] = (uint8_t)(s_ts >> 8);
        pkt[7] = (uint8_t)(s_ts & 0xFF);
        pkt[8] = (uint8_t)(s_ssrc >> 24);
        pkt[9] = (uint8_t)(s_ssrc >> 16);
        pkt[10] = (uint8_t)(s_ssrc >> 8);
        pkt[11] = (uint8_t)(s_ssrc & 0xFF);

        for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            pkt[RTP_HEADER_LEN + i] = (s_pt == RTP_PAYLOAD_TYPE_PCMA)
                                          ? linear_to_alaw(pcm[i])
                                          : linear_to_ulaw(pcm[i]);
        }

        if (s_remote.sin_port != 0) {
            int sent = sendto(s_sock, pkt, RTP_HEADER_LEN + AUDIO_FRAME_BYTES, 0,
                              (struct sockaddr *)&s_remote, sizeof(s_remote));
            if (sent > 0) {
                s_sent++;
            }
        }
        s_seq++;
        s_ts += AUDIO_FRAME_SAMPLES;

        /* One 20 ms frame of playout per tick keeps the speaker fed even when
         * the network is silent: gaps become silence instead of underruns. */
        size_t n = rtp_jitter_pop(s_jb, payload, sizeof(payload));
        memset(out, 0, sizeof(out));
        for (size_t i = 0; i < n && i < AUDIO_FRAME_SAMPLES; i++) {
            out[i] = (s_pt == RTP_PAYLOAD_TYPE_PCMA) ? alaw_to_linear(payload[i])
                                                     : ulaw_to_linear(payload[i]);
        }
        audio_write_pcm(out, AUDIO_FRAME_SAMPLES);

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "[RTP] TX task stopped");
    s_tx_task = NULL;
    vTaskDelete(NULL);
}

static void rtp_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];

    ESP_LOGI(TAG, "[RTP] RX task started from %s:%u",
             inet_ntoa(s_remote.sin_addr), (unsigned)ntohs(s_remote.sin_port));

    while (s_running) {
        struct pollfd pfd = {
            .fd = s_sock,
            .events = POLLIN,
        };
        if (poll(&pfd, 1, 50) <= 0 || !(pfd.revents & POLLIN)) {
            continue;
        }

        int n = recv(s_sock, buf, sizeof(buf), 0);
        if (n < RTP_HEADER_LEN) {
            continue;
        }
        if ((buf[0] >> 6) != 2) {
            continue;
        }

        size_t header = RTP_HEADER_LEN + 4u * (buf[0] & 0x0F);
        if (buf[0] & 0x10) {
            if ((size_t)n < header + 4) {
                continue;
            }
            size_t ext_words = ((size_t)buf[header + 2] << 8) | buf[header + 3];
            header += 4 + ext_words * 4;
        }
        if ((size_t)n <= header) {
            continue;
        }

        uint8_t pt = buf[1] & 0x7F;
        if (pt != RTP_PAYLOAD_TYPE_PCMU && pt != RTP_PAYLOAD_TYPE_PCMA) {
            continue; /* DTMF and other payloads are not mixed into audio */
        }

        uint16_t seq = (uint16_t)((buf[2] << 8) | buf[3]);
        uint32_t ts = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                      ((uint32_t)buf[6] << 8) | buf[7];

        s_recv++;

        if (!s_have_seq) {
            s_last_seq = seq;
            s_have_seq = true;
        } else {
            int16_t diff = (int16_t)(seq - s_last_seq);
            if (diff == 0) {
                /* duplicate */
            } else if (diff < 0) {
                s_ooo++;
            } else {
                if (diff > 1) {
                    s_lost += (uint32_t)(diff - 1);
                }
                s_last_seq = seq;
            }
        }

        /* RFC 3550 interarrival jitter estimate, in milliseconds. */
        int64_t arrival_ms = esp_timer_get_time() / 1000;
        int64_t transit = arrival_ms - (int64_t)(ts / 8);
        static int64_t last_transit;
        static bool have_transit;
        if (have_transit) {
            int64_t d = transit - last_transit;
            if (d < 0) {
                d = -d;
            }
            s_jitter_ms += (uint32_t)((d - (int64_t)s_jitter_ms) / 16);
        }
        last_transit = transit;
        have_transit = true;

        rtp_jitter_push(s_jb, seq, ts, buf + header, (size_t)n - header);
    }

    ESP_LOGI(TAG, "[RTP] RX task stopped");
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ API */

esp_err_t rtp_client_init(void)
{
    return rtp_client_open(NULL);
}

esp_err_t rtp_client_start(const char *remote_ip, uint16_t remote_port, uint8_t payload_type)
{
    if (remote_ip == NULL || remote_port == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        rtp_client_stop();
    }

    uint16_t local = 0;
    esp_err_t err = rtp_client_open(&local);
    if (err != ESP_OK) {
        return err;
    }

    memset(&s_remote, 0, sizeof(s_remote));
    s_remote.sin_family = AF_INET;
    s_remote.sin_port = htons(remote_port);
    if (inet_aton(remote_ip, &s_remote.sin_addr) == 0) {
        ESP_LOGE(TAG, "[RTP] Invalid remote IP '%s'", remote_ip);
        return ESP_ERR_INVALID_ARG;
    }

    s_pt = (payload_type == RTP_PAYLOAD_TYPE_PCMA) ? RTP_PAYLOAD_TYPE_PCMA
                                                    : RTP_PAYLOAD_TYPE_PCMU;
    s_jb = rtp_jitter_create(RTP_JITTER_FRAMES);
    if (s_jb == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_seq = (uint16_t)esp_random();
    s_ts = esp_random();
    s_ssrc = esp_random();
    s_have_seq = false;
    s_sent = s_recv = s_lost = s_ooo = s_jitter_ms = 0;

    err = audio_output_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[RTP] Speaker start failed: %s", esp_err_to_name(err));
    }

    s_running = true;
    if (xTaskCreatePinnedToCore(rtp_rx_task, "rtp_rx", 4096, NULL, 6, &s_rx_task, 1) != pdPASS) {
        s_running = false;
        rtp_jitter_destroy(s_jb);
        s_jb = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(rtp_tx_task, "rtp_tx", 4096, NULL, 5, &s_tx_task, 1) != pdPASS) {
        s_running = false;
        rtp_jitter_destroy(s_jb);
        s_jb = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[RTP] Session with %s:%u pt=%u (%s), local port %u", remote_ip,
             (unsigned)remote_port, (unsigned)s_pt,
             (s_pt == RTP_PAYLOAD_TYPE_PCMA) ? "PCMA" : "PCMU", (unsigned)local);
    return ESP_OK;
}

esp_err_t rtp_client_stop(void)
{
    if (!s_running && s_sock >= 0 && s_jb == NULL) {
        return ESP_OK;
    }

    s_running = false;
    for (int i = 0; i < 100 && (s_tx_task != NULL || s_rx_task != NULL); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_jb != NULL) {
        rtp_jitter_destroy(s_jb);
        s_jb = NULL;
    }
    audio_output_stop();
    ESP_LOGI(TAG, "[RTP] Session stopped (sent %u, recv %u, lost %u)",
             (unsigned)s_sent, (unsigned)s_recv, (unsigned)s_lost);
    return ESP_OK;
}

esp_err_t rtp_client_send_pcm(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t pkt[RTP_HEADER_LEN + AUDIO_FRAME_BYTES];
    size_t n = sample_count;
    if (n > AUDIO_FRAME_SAMPLES) {
        n = AUDIO_FRAME_SAMPLES;
    }

    pkt[0] = 0x80;
    pkt[1] = s_pt;
    pkt[2] = (uint8_t)(s_seq >> 8);
    pkt[3] = (uint8_t)(s_seq & 0xFF);
    pkt[4] = (uint8_t)(s_ts >> 24);
    pkt[5] = (uint8_t)(s_ts >> 16);
    pkt[6] = (uint8_t)(s_ts >> 8);
    pkt[7] = (uint8_t)(s_ts & 0xFF);
    pkt[8] = (uint8_t)(s_ssrc >> 24);
    pkt[9] = (uint8_t)(s_ssrc >> 16);
    pkt[10] = (uint8_t)(s_ssrc >> 8);
    pkt[11] = (uint8_t)(s_ssrc & 0xFF);

    for (size_t i = 0; i < n; i++) {
        pkt[RTP_HEADER_LEN + i] = (s_pt == RTP_PAYLOAD_TYPE_PCMA)
                                      ? linear_to_alaw(samples[i])
                                      : linear_to_ulaw(samples[i]);
    }

    int sent = sendto(s_sock, pkt, RTP_HEADER_LEN + n, 0,
                      (struct sockaddr *)&s_remote, sizeof(s_remote));
    s_seq++;
    s_ts += (uint32_t)n;
    return (sent > 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t rtp_client_get_stats(rtp_stats_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->packets_sent = s_sent;
    out->packets_received = s_recv;
    out->packets_lost = s_lost;
    out->packets_out_of_order = s_ooo;
    out->jitter_ms = s_jitter_ms;
    return ESP_OK;
}
