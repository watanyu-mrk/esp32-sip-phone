/*
 * SIP user agent for the XIAO ESP32-S3 phone, implemented directly on lwIP.
 *
 * Why not esp_sip_service/esp_rtc: on this target the pre-compiled stack
 * reads its 262-byte response buffer in a tight loop after the first
 * REGISTER, never transmits the authenticated retry, and starves CPU0 until
 * the task watchdog fires (web UI and audio stop). That is a hard blocker,
 * verified against two PBXs and a standards-compliant local registrar, so the
 * SIP subset the phone needs is implemented here instead:
 *
 *   - REGISTER with MD5 digest auth (qop=auth and legacy RFC 2069)
 *   - registration refresh before expiry and retry with backoff
 *   - INVITE/ACK/CANCEL/BYE plus 100/180/183/200 handling
 *   - SDP offer/answer for G.711 PCMU/PCMA
 *   - tolerant parsing of CRLF and the CRCRLF some PBXs emit
 *
 * All signalling runs on one task that never blocks on the network, so the
 * idle task and the web server stay responsive.
 */
#include "sip_client.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_manager.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/md5.h"
#include "rtp_client.h"
#include "wifi_manager.h"

static const char *TAG = "SIP";

#define SIP_BUF_LEN       1600
#define SIP_LINE_LEN      512
#define SIP_RTX_MAX       6
#define SIP_T1_MS         500
#define SIP_REG_RETRY_MS  30000

typedef struct {
    char host[CFG_SERVER_LEN];
    uint16_t port;
    char user[CFG_ID_LEN];
    char auth_id[CFG_ID_LEN];
    char pass[CFG_SECRET_LEN];
    char ext[CFG_EXT_LEN];
    char display[CFG_NAME_LEN];
    uint16_t expiry;
} sip_account_t;

typedef struct {
    int status;                 /* 0 for requests */
    char method[16];
    char start_line[128];
    char via[192];
    char from[SIP_LINE_LEN];
    char from_tag[32];
    char to[SIP_LINE_LEN];
    char to_tag[32];
    char call_id[96];
    char cseq_raw[48];
    uint32_t cseq_num;
    char cseq_method[16];
    char contact[192];
    char www_auth[512];
    char proxy_auth[512];
    char content_type[64];
    const char *body;
    size_t body_len;
} sip_msg_t;

static sip_account_t s_acc;
static int s_sock = -1;
static struct sockaddr_in s_dst;
static struct sockaddr_in s_reply_to;    /* source of the request being handled */
static struct sockaddr_in s_invite_reply; /* where to send the 200 OK later */
static struct sockaddr_in s_peer_addr;   /* peer of a direct (non-proxy) call */
static bool s_peer_direct;
static const struct sockaddr_in *s_resp_target;
static char s_local_ip[16];
static uint16_t s_local_port;
static uint16_t s_rtp_port = 20000;
static volatile bool s_running;
static TaskHandle_t s_task;

static sip_reg_state_t s_reg = SIP_REG_STATE_UNREGISTERED;
static sip_call_state_t s_call = SIP_CALL_IDLE;
static char s_peer[48];

/* Registration transaction. */
static char s_reg_call_id[96];
static char s_reg_from_tag[24];
static uint32_t s_reg_cseq;
static char s_reg_msg[1024];
static size_t s_reg_msg_len;
static uint16_t s_reg_expiry;
static int s_reg_rtx_n;
static int s_reg_rtx_ms;
static int64_t s_reg_rtx_at;
static int64_t s_reg_refresh_at;
static int64_t s_reg_retry_at;
static int64_t s_ip_check_at;
static bool s_link_up;
static int s_fail_count;

/* Digest credentials from the last 401/407. */
static char s_realm[64];
static char s_nonce[160];
static bool s_qop_auth;
static uint32_t s_nc;

/* Dialog (one call at a time). */
static char s_dlg_call_id[96];
static char s_local_tag[24];
static char s_remote_tag[32];
static char s_remote_uri[192];
static char s_remote_to[192];
static char s_remote_ip[16];
static uint16_t s_remote_port;
static uint8_t s_pt;
static uint32_t s_dlg_cseq;
static char s_dlg_msg[2048];
static size_t s_dlg_msg_len;
static char s_inv_branch[48];

/* INVITE digest state is kept apart from the registration challenge so a
 * REGISTER refresh cannot invalidate an in-flight call setup. */
static char s_inv_realm[64];
static char s_inv_nonce[160];
static bool s_inv_qop_auth;
static uint32_t s_inv_nc;
static bool s_inv_proxy_auth;
static char s_inv_uri[96];
static char s_inv_auth[600];
static bool s_inv_have_auth;
static int s_inv_auth_tries;
static char s_dial_number[CFG_EXT_LEN];
static sip_msg_t s_pending_invite;
static char s_answer_msg[1400];
static size_t s_answer_len;
static int s_dlg_rtx_n;
static int s_dlg_rtx_ms;
static int64_t s_dlg_rtx_at;

/* ------------------------------------------------------------------ utils */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void append(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    if (*len + 1 >= cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *len += (size_t)n;
        if (*len >= cap) {
            *len = cap - 1;
        }
    }
}

static void md5_hex(const char *in, char out[33])
{
    unsigned char digest[16];
    mbedtls_md5((const unsigned char *)in, strlen(in), digest);
    for (int i = 0; i < 16; i++) {
        snprintf(out + i * 2, 3, "%02x", digest[i]);
    }
    out[32] = '\0';
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, cap, "%s", src);
}

/* Extracts name=value from a header such as WWW-Authenticate. */
static bool sip_param(const char *hdr, const char *key, char *out, size_t out_sz)
{
    if (out_sz > 0) {
        out[0] = '\0';
    }
    if (hdr == NULL || key == NULL) {
        return false;
    }

    size_t kl = strlen(key);
    const char *p = hdr;
    while ((p = strstr(p, key)) != NULL) {
        bool left_ok = (p == hdr) ||
                       !(isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '-');
        if (left_ok && p[kl] == '=') {
            const char *v = p + kl + 1;
            const char *e;
            if (*v == '"') {
                v++;
                e = strchr(v, '"');
                if (e == NULL) {
                    return false;
                }
            } else {
                e = v;
                while (*e && *e != ',' && *e != ';' && *e != ' ' && *e != '\r' && *e != '\n') {
                    e++;
                }
            }
            size_t l = (size_t)(e - v);
            if (l >= out_sz) {
                l = out_sz - 1;
            }
            memcpy(out, v, l);
            out[l] = '\0';
            return out[0] != '\0';
        }
        p += kl;
    }
    return false;
}

static void peer_from_uri(const char *uri, char *out, size_t out_sz)
{
    out[0] = '\0';
    if (uri == NULL) {
        return;
    }
    const char *s = strstr(uri, "sip:");
    if (s == NULL) {
        return;
    }
    s += 4;
    if (strncmp(s, "sips:", 5) == 0) {
        s += 5;
    }
    size_t i = 0;
    while (s[i] && s[i] != '@' && s[i] != '>' && s[i] != ';' && i + 1 < out_sz) {
        out[i] = s[i];
        i++;
    }
    out[i] = '\0';
}

/* --------------------------------------------------------------- transport */

static void udp_send_to(const struct sockaddr_in *dst, const char *msg, size_t len)
{
    if (s_sock < 0 || len == 0 || dst == NULL) {
        return;
    }
    int r = sendto(s_sock, msg, len, 0, (const struct sockaddr *)dst, sizeof(*dst));
    if (r < 0) {
        ESP_LOGW(TAG, "[SIP] sendto failed errno=%d", errno);
    }
}

static void udp_send(const char *msg, size_t len)
{
    udp_send_to(&s_dst, msg, len);
}

static void resolve_destination(void)
{
    memset(&s_dst, 0, sizeof(s_dst));
    s_dst.sin_family = AF_INET;
    s_dst.sin_port = htons(s_acc.port);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_DGRAM,
    };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)s_acc.port);

    if (getaddrinfo(s_acc.host, port_str, &hints, &res) == 0 && res != NULL) {
        memcpy(&s_dst, res->ai_addr, sizeof(s_dst));
        freeaddrinfo(res);
    } else {
        ESP_LOGE(TAG, "[SIP] Cannot resolve %s", s_acc.host);
    }
}

/* --------------------------------------------------------------- parsing */

static void parse_header_line(sip_msg_t *m, char *line)
{
    char *colon = strchr(line, ':');
    if (colon == NULL) {
        return;
    }
    *colon = '\0';
    char *name = line;
    char *val = colon + 1;
    while (*val == ' ' || *val == '\t') {
        val++;
    }
    size_t vlen = strlen(val);
    while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) {
        val[--vlen] = '\0';
    }

    if (strcasecmp(name, "Via") == 0) {
        copy_str(m->via, sizeof(m->via), val);
    } else if (strcasecmp(name, "From") == 0) {
        copy_str(m->from, sizeof(m->from), val);
        sip_param(val, "tag", m->from_tag, sizeof(m->from_tag));
    } else if (strcasecmp(name, "To") == 0) {
        copy_str(m->to, sizeof(m->to), val);
        sip_param(val, "tag", m->to_tag, sizeof(m->to_tag));
    } else if (strcasecmp(name, "Call-ID") == 0 || strcasecmp(name, "i") == 0) {
        copy_str(m->call_id, sizeof(m->call_id), val);
    } else if (strcasecmp(name, "CSeq") == 0) {
        copy_str(m->cseq_raw, sizeof(m->cseq_raw), val);
        unsigned num = 0;
        char method[16] = {0};
        if (sscanf(val, "%u %15s", &num, method) == 2) {
            m->cseq_num = num;
            copy_str(m->cseq_method, sizeof(m->cseq_method), method);
        }
    } else if (strcasecmp(name, "Contact") == 0 || strcasecmp(name, "m") == 0) {
        copy_str(m->contact, sizeof(m->contact), val);
    } else if (strcasecmp(name, "WWW-Authenticate") == 0) {
        copy_str(m->www_auth, sizeof(m->www_auth), val);
    } else if (strcasecmp(name, "Proxy-Authenticate") == 0) {
        copy_str(m->proxy_auth, sizeof(m->proxy_auth), val);
    } else if (strcasecmp(name, "Content-Type") == 0 || strcasecmp(name, "c") == 0) {
        copy_str(m->content_type, sizeof(m->content_type), val);
    }
}

static void parse_msg(const char *buf, sip_msg_t *m)
{
    memset(m, 0, sizeof(*m));
    const char *p = buf;
    bool first = true;

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == ' ' || p[n - 1] == '\t')) {
            n--;
        }
        if (n == 0) {
            const char *body = (eol != NULL) ? eol + 1 : p + n;
            m->body = body;
            m->body_len = strlen(body);
            return;
        }

        char line[SIP_LINE_LEN];
        size_t copy = (n < sizeof(line) - 1) ? n : sizeof(line) - 1;
        memcpy(line, p, copy);
        line[copy] = '\0';

        if (first) {
            copy_str(m->start_line, sizeof(m->start_line), line);
            if (strncmp(line, "SIP/2.0 ", 8) == 0) {
                m->status = atoi(line + 8);
            } else {
                size_t i = 0;
                while (line[i] && line[i] != ' ' && i + 1 < sizeof(m->method)) {
                    m->method[i] = line[i];
                    i++;
                }
                m->method[i] = '\0';
            }
            first = false;
        } else {
            parse_header_line(m, line);
        }

        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
}

/* ------------------------------------------------------------------- auth */

static bool build_auth(char *out, size_t out_sz, const char *method, const char *uri)
{
    if (s_nonce[0] == '\0' || s_realm[0] == '\0') {
        return false;
    }

    char user[CFG_ID_LEN];
    copy_str(user, sizeof(user), (s_acc.auth_id[0] != '\0') ? s_acc.auth_id : s_acc.user);

    char ha1[33], ha2[33], resp[33];
    char tmp[400];
    snprintf(tmp, sizeof(tmp), "%s:%s:%s", user, s_realm, s_acc.pass);
    md5_hex(tmp, ha1);

    snprintf(tmp, sizeof(tmp), "%s:%s", method, uri);
    md5_hex(tmp, ha2);

    if (s_qop_auth) {
        char nc[9];
        char cnonce[17];
        s_nc++;
        snprintf(nc, sizeof(nc), "%08x", (unsigned)s_nc);
        snprintf(cnonce, sizeof(cnonce), "%08x", (unsigned)esp_random());
        snprintf(tmp, sizeof(tmp), "%s:%s:%s:%s:auth:%s", ha1, s_nonce, nc, cnonce, ha2);
        md5_hex(tmp, resp);
        snprintf(out, out_sz,
                 "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                 "response=\"%s\", algorithm=MD5, qop=auth, nc=%s, cnonce=\"%s\"",
                 user, s_realm, s_nonce, uri, resp, nc, cnonce);
    } else {
        snprintf(tmp, sizeof(tmp), "%s:%s:%s", ha1, s_nonce, ha2);
        md5_hex(tmp, resp);
        snprintf(out, out_sz,
                 "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                 "response=\"%s\", algorithm=MD5",
                 user, s_realm, s_nonce, uri, resp);
    }
    return true;
}

static bool build_invite_auth(char *out, size_t out_sz)
{
    if (s_inv_nonce[0] == '\0' || s_inv_realm[0] == '\0') {
        return false;
    }

    char user[CFG_ID_LEN];
    copy_str(user, sizeof(user), (s_acc.auth_id[0] != '\0') ? s_acc.auth_id : s_acc.user);

    char ha1[33], ha2[33], resp[33];
    char tmp[400];
    snprintf(tmp, sizeof(tmp), "%s:%s:%s", user, s_inv_realm, s_acc.pass);
    md5_hex(tmp, ha1);

    snprintf(tmp, sizeof(tmp), "INVITE:%s", s_inv_uri);
    md5_hex(tmp, ha2);

    if (s_inv_qop_auth) {
        char nc[9];
        char cnonce[17];
        s_inv_nc++;
        snprintf(nc, sizeof(nc), "%08x", (unsigned)s_inv_nc);
        snprintf(cnonce, sizeof(cnonce), "%08x", (unsigned)esp_random());
        snprintf(tmp, sizeof(tmp), "%s:%s:%s:%s:auth:%s", ha1, s_inv_nonce, nc, cnonce, ha2);
        md5_hex(tmp, resp);
        snprintf(out, out_sz,
                 "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                 "response=\"%s\", algorithm=MD5, qop=auth, nc=%s, cnonce=\"%s\"",
                 user, s_inv_realm, s_inv_nonce, s_inv_uri, resp, nc, cnonce);
    } else {
        snprintf(tmp, sizeof(tmp), "%s:%s:%s", ha1, s_inv_nonce, ha2);
        md5_hex(tmp, resp);
        snprintf(out, out_sz,
                 "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                 "response=\"%s\", algorithm=MD5",
                 user, s_inv_realm, s_inv_nonce, s_inv_uri, resp);
    }
    return true;
}

/* ------------------------------------------------------------- registration */

static void fail_registration(const char *why, int retry_s)
{
    /* Recover quickly from a lost packet during a Wi-Fi roam, then back off so
     * a PBX auto-defense block is not refreshed by a retry storm. */
    (void)retry_s;
    static const int retry_table[] = {15, 30, 60, 300};
    int idx = (s_fail_count < 3) ? s_fail_count : 3;
    int retry = retry_table[idx];
    s_fail_count++;

    ESP_LOGE(TAG, "[SIP] Registration failed: %s (retry in %ds)", why, retry);
    if (s_fail_count >= 3) {
        ESP_LOGW(TAG, "[SIP] Repeated failures: check that the PBX has not blocked %s",
                 s_local_ip);
    }
    s_reg = SIP_REG_STATE_FAILED;
    s_reg_rtx_at = 0;
    s_reg_rtx_n = 0;
    s_reg_rtx_ms = SIP_T1_MS;
    s_reg_retry_at = now_ms() + (int64_t)retry * 1000;
}

static void send_register_ex(uint16_t expires)
{
    if (s_sock < 0) {
        return;
    }

    char uri[96];
    snprintf(uri, sizeof(uri), "sip:%s:%u", s_acc.host, (unsigned)s_acc.port);

    char branch[48];
    snprintf(branch, sizeof(branch), "z9hG4bK%08x%08x",
             (unsigned)esp_random(), (unsigned)(esp_timer_get_time() & 0xffffffffu));

    if (expires > 0) {
        s_reg_cseq++;
    }

    s_reg_expiry = expires;

    static char buf[1024];
    size_t len = 0;
    append(buf, sizeof(buf), &len,
           "REGISTER %s SIP/2.0\r\n"
           "Via: SIP/2.0/UDP %s:%u;rport;branch=%s\r\n"
           "Max-Forwards: 70\r\n"
           "From: \"%s\" <sip:%s@%s>;tag=%s\r\n"
           "To: <sip:%s@%s>\r\n"
           "Call-ID: %s\r\n"
           "CSeq: %u REGISTER\r\n"
           "Contact: <sip:%s@%s:%u>\r\n"
           "Expires: %u\r\n"
           "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, INFO\r\n"
           "User-Agent: XIAO-SIP/1.0\r\n",
           uri,
           s_local_ip, (unsigned)s_local_port, branch,
           s_acc.display, s_acc.user, s_acc.host, s_reg_from_tag,
           s_acc.user, s_acc.host,
           s_reg_call_id,
           (unsigned)s_reg_cseq,
           s_acc.user, s_local_ip, (unsigned)s_local_port,
           (unsigned)expires);

    static char auth[600];
    if (build_auth(auth, sizeof(auth), "REGISTER", uri)) {
        append(buf, sizeof(buf), &len, "Authorization: %s\r\n", auth);
    }
    append(buf, sizeof(buf), &len, "Content-Length: 0\r\n\r\n");

    memcpy(s_reg_msg, buf, len);
    s_reg_msg_len = len;

    udp_send(buf, len);
    ESP_LOGI(TAG, "[SIP] REGISTER -> %s:%u (cseq %u, expires %u%s)", s_acc.host,
             (unsigned)s_acc.port, (unsigned)s_reg_cseq, (unsigned)expires,
             (s_nonce[0] != '\0') ? ", auth" : "");

    s_reg_rtx_n = 0;
    s_reg_rtx_ms = SIP_T1_MS;
    s_reg_rtx_at = now_ms() + SIP_T1_MS;
}

static void send_register(void)
{
    send_register_ex((s_acc.expiry != 0) ? s_acc.expiry : 300);
}

static void handle_register_response(const sip_msg_t *m)
{
    if (m->cseq_num != s_reg_cseq) {
        ESP_LOGD(TAG, "[SIP] Ignoring stale REGISTER response (cseq %u vs %u)",
                 (unsigned)m->cseq_num, (unsigned)s_reg_cseq);
        return;
    }

    if (m->status == 401 || m->status == 407) {
        const char *hdr = (m->status == 401) ? m->www_auth : m->proxy_auth;
        char realm[64] = {0};
        char nonce[160] = {0};
        char qop[24] = {0};

        if (!sip_param(hdr, "realm", realm, sizeof(realm)) ||
            !sip_param(hdr, "nonce", nonce, sizeof(nonce))) {
            fail_registration("challenge without realm/nonce", 30);
            return;
        }

        bool qop_auth = sip_param(hdr, "qop", qop, sizeof(qop)) && strstr(qop, "auth") != NULL;
        bool new_nonce = strcmp(nonce, s_nonce) != 0;

        if (new_nonce) {
            s_nc = 0;
        }
        copy_str(s_realm, sizeof(s_realm), realm);
        copy_str(s_nonce, sizeof(s_nonce), nonce);
        s_qop_auth = qop_auth;

        ESP_LOGI(TAG, "[SIP] %d challenge realm='%s' qop=%s", m->status, s_realm,
                 qop_auth ? "auth" : "none");
        /* The digest we build assumes plain MD5; print the header once so a
         * PBX demanding MD5-sess or a different algorithm is visible. */
        static bool logged_challenge;
        if (!logged_challenge) {
            logged_challenge = true;
            ESP_LOGI(TAG, "[SIP] Challenge header: %s", hdr);
        }

        if (new_nonce) {
            send_register_ex(s_reg_expiry > 0 ? s_reg_expiry : 300);
        } else {
            fail_registration("rejected after digest", 30);
        }
        return;
    }

    if (m->status == 200) {
        s_reg_rtx_at = 0;
        s_reg_rtx_n = 0;
        s_fail_count = 0;
        s_reg = SIP_REG_STATE_REGISTERED;
        ESP_LOGI(TAG, "[SIP] Registration Successful");
        ESP_LOGI(TAG, "[SIP] State: REGISTERED (expires %us)", (unsigned)s_reg_expiry);

        uint32_t refresh = (uint32_t)((s_reg_expiry * 9u) / 10u);
        if (refresh < 30u) {
            refresh = 30u;
        }
        s_reg_refresh_at = now_ms() + (int64_t)refresh * 1000;
        s_reg_retry_at = 0;
        return;
    }

    if (m->status >= 300) {
        char why[32];
        snprintf(why, sizeof(why), "SIP %d", m->status);
        fail_registration(why, 30);
    }
}

/* -------------------------------------------------------------------- SDP */

static uint16_t local_rtp_port(void)
{
    return s_rtp_port;
}

static int build_sdp(char *out, size_t cap, bool answer, uint8_t pt, uint16_t port)
{
    uint32_t sid = (uint32_t)(esp_timer_get_time() / 1000);
    if (answer) {
        return snprintf(out, cap,
                        "v=0\r\n"
                        "o=- %u %u IN IP4 %s\r\n"
                        "s=XIAO SIP Phone\r\n"
                        "c=IN IP4 %s\r\n"
                        "t=0 0\r\n"
                        "m=audio %u RTP/AVP %u\r\n"
                        "a=rtpmap:%u %s/8000\r\n"
                        "a=ptime:20\r\n"
                        "a=sendrecv\r\n",
                        (unsigned)sid, (unsigned)sid, s_local_ip, s_local_ip,
                        (unsigned)port, (unsigned)pt, (unsigned)pt,
                        (pt == RTP_PAYLOAD_TYPE_PCMA) ? "PCMA" : "PCMU");
    }
    return snprintf(out, cap,
                    "v=0\r\n"
                    "o=- %u %u IN IP4 %s\r\n"
                    "s=XIAO SIP Phone\r\n"
                    "c=IN IP4 %s\r\n"
                    "t=0 0\r\n"
                    "m=audio %u RTP/AVP 0 8\r\n"
                    "a=rtpmap:0 PCMU/8000\r\n"
                    "a=rtpmap:8 PCMA/8000\r\n"
                    "a=ptime:20\r\n"
                    "a=sendrecv\r\n",
                    (unsigned)sid, (unsigned)sid, s_local_ip, s_local_ip,
                    (unsigned)port);
}

static bool sdp_parse(const char *body, char *ip, size_t ip_sz, uint16_t *port, uint8_t *pt)
{
    char line[160];
    bool have_ip = false;
    bool have_media = false;
    *port = 0;
    *pt = RTP_PAYLOAD_TYPE_PCMU;

    const char *p = body;
    while (p != NULL && *p) {
        const char *eol = strchr(p, '\n');
        size_t n = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        while (n > 0 && p[n - 1] == '\r') {
            n--;
        }
        if (n >= sizeof(line)) {
            n = sizeof(line) - 1;
        }
        memcpy(line, p, n);
        line[n] = '\0';

        if (strncmp(line, "c=IN IP4 ", 9) == 0 && !have_ip) {
            copy_str(ip, ip_sz, line + 9);
            have_ip = true;
        } else if (strncmp(line, "m=audio ", 8) == 0 && !have_media) {
            unsigned pnum = 0;
            char rest[96] = {0};
            if (sscanf(line + 8, "%u %95[^\r\n]", &pnum, rest) >= 1 && pnum > 0) {
                *port = (uint16_t)pnum;
                have_media = true;
                if (strstr(rest, " 0") != NULL) {
                    *pt = RTP_PAYLOAD_TYPE_PCMU;
                } else if (strstr(rest, "8") != NULL) {
                    *pt = RTP_PAYLOAD_TYPE_PCMA;
                } else {
                    have_media = false;
                }
            }
        }

        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }

    return have_ip && have_media;
}

/* ---------------------------------------------------------------- helpers */

static void media_start(void)
{
    char ip[16];
    copy_str(ip, sizeof(ip), s_remote_ip);
    if (ip[0] == '\0' || s_remote_port == 0) {
        ESP_LOGW(TAG, "[SIP] No remote media address in SDP");
        return;
    }
    esp_err_t err = rtp_client_start(ip, s_remote_port, s_pt);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[RTP] Audio session started: %s:%u pt=%u", ip,
                 (unsigned)s_remote_port, (unsigned)s_pt);
    } else {
        ESP_LOGW(TAG, "[RTP] Media not started yet (%s): %s:%u pt=%u",
                 esp_err_to_name(err), ip, (unsigned)s_remote_port, (unsigned)s_pt);
    }
}

static void media_stop(void)
{
    rtp_client_stop();
}

static void set_call_state(sip_call_state_t st)
{
    s_call = st;
}

static void end_call(void)
{
    media_stop();
    s_peer_direct = false;
    s_dlg_call_id[0] = '\0';
    s_remote_tag[0] = '\0';
    s_remote_uri[0] = '\0';
    s_remote_to[0] = '\0';
    s_pending_invite.call_id[0] = '\0';
    s_dlg_rtx_at = 0;
    s_answer_len = 0;
    s_peer[0] = '\0';
    set_call_state(SIP_CALL_IDLE);
    ESP_LOGI(TAG, "[PHONE] State: IDLE");
}

static void send_simple_request(const char *method, uint32_t cseq, const char *to_tag,
                                const char *branch)
{
    char uri[192];
    copy_str(uri, sizeof(uri), s_remote_uri[0] ? s_remote_uri : "");
    if (uri[0] == '\0') {
        snprintf(uri, sizeof(uri), "sip:%s@%s:%u", s_acc.user, s_acc.host,
                 (unsigned)s_acc.port);
    }

    char br[48];
    if (branch != NULL) {
        copy_str(br, sizeof(br), branch);
    } else {
        snprintf(br, sizeof(br), "z9hG4bK%08x%08x", (unsigned)esp_random(),
                 (unsigned)(esp_timer_get_time() & 0xffffffffu));
    }

    /* Copy the peer's To header verbatim when we have it: rebuilding it from
     * user@host loses the port and the exact tag, and the PBX then ignores the
     * ACK and retransmits 200 OK. */
    char to[192];
    if (s_remote_to[0] != '\0') {
        copy_str(to, sizeof(to), s_remote_to);
    } else {
        snprintf(to, sizeof(to), "<sip:%s@%s>%s%s", s_acc.user, s_acc.host,
                 (to_tag != NULL && to_tag[0] != '\0') ? ";tag=" : "",
                 (to_tag != NULL) ? to_tag : "");
    }

    char buf[900];
    size_t len = 0;
    append(buf, sizeof(buf), &len,
           "%s %s SIP/2.0\r\n"
           "Via: SIP/2.0/UDP %s:%u;rport;branch=%s\r\n"
           "Max-Forwards: 70\r\n"
           "From: <sip:%s@%s>;tag=%s\r\n"
           "To: %s\r\n"
           "Call-ID: %s\r\n"
           "CSeq: %u %s\r\n"
           "User-Agent: XIAO-SIP/1.0\r\n"
           "Content-Length: 0\r\n\r\n",
           method, uri,
           s_local_ip, (unsigned)s_local_port, br,
           s_acc.user, s_acc.host, s_local_tag,
           to,
           s_dlg_call_id,
           (unsigned)cseq, method);

    udp_send_to(s_peer_direct ? &s_peer_addr : &s_dst, buf, len);
    ESP_LOGI(TAG, "[SIP] %s sent", method);
}

static void send_ack(void)
{
    /* ACK for a 2xx keeps the INVITE's CSeq number; incrementing it makes the
     * PBX treat the ACK as unmatched and retransmit 200 OK every T2. */
    send_simple_request("ACK", s_dlg_cseq, s_remote_tag, NULL);
}

/* ACK for a non-2xx final response must carry the same CSeq as the INVITE it
 * acknowledges, otherwise the PBX keeps retransmitting the challenge. */
static void send_ack_for(uint32_t cseq)
{
    char uri[192];
    if (s_inv_uri[0] != '\0') {
        copy_str(uri, sizeof(uri), s_inv_uri);
    } else if (s_remote_uri[0] != '\0') {
        copy_str(uri, sizeof(uri), s_remote_uri);
    } else {
        snprintf(uri, sizeof(uri), "sip:%s@%s:%u", s_acc.user, s_acc.host,
                 (unsigned)s_acc.port);
    }

    char br[48];
    snprintf(br, sizeof(br), "z9hG4bK%08x%08x", (unsigned)esp_random(),
             (unsigned)(esp_timer_get_time() & 0xffffffffu));

    char buf[900];
    size_t len = 0;
    append(buf, sizeof(buf), &len,
           "ACK %s SIP/2.0\r\n"
           "Via: SIP/2.0/UDP %s:%u;rport;branch=%s\r\n"
           "Max-Forwards: 70\r\n"
           "From: <sip:%s@%s>;tag=%s\r\n"
           "To: <sip:%s@%s>%s%s\r\n"
           "Call-ID: %s\r\n"
           "CSeq: %u ACK\r\n"
           "User-Agent: XIAO-SIP/1.0\r\n"
           "Content-Length: 0\r\n\r\n",
           uri, s_local_ip, (unsigned)s_local_port, br,
           s_acc.user, s_acc.host, s_local_tag,
           s_acc.user, s_acc.host,
           (s_remote_tag[0] != '\0') ? ";tag=" : "", s_remote_tag,
           s_dlg_call_id, (unsigned)cseq);

    udp_send_to(s_peer_direct ? &s_peer_addr : &s_dst, buf, len);
    ESP_LOGI(TAG, "[SIP] ACK sent (cseq %u)", (unsigned)cseq);
}

static void send_bye(void)
{
    s_dlg_cseq++;
    send_simple_request("BYE", s_dlg_cseq, s_remote_tag, NULL);
    set_call_state(SIP_CALL_TERMINATING);
}

static void send_response_to(const struct sockaddr_in *dst, const sip_msg_t *req, int code,
                             const char *reason, const char *extra, const char *ctype,
                             const char *body)
{
    char to[SIP_LINE_LEN + 32];
    if (req->to_tag[0] != '\0') {
        copy_str(to, sizeof(to), req->to);
    } else {
        snprintf(to, sizeof(to), "%s;tag=%s", req->to, s_local_tag);
    }

    static char buf[1400];
    size_t len = 0;
    append(buf, sizeof(buf), &len,
           "SIP/2.0 %d %s\r\n"
           "Via: %s\r\n"
           "From: %s\r\n"
           "To: %s\r\n"
           "Call-ID: %s\r\n"
           "CSeq: %s\r\n"
           "User-Agent: XIAO-SIP/1.0\r\n",
           code, reason, req->via, req->from, to, req->call_id, req->cseq_raw);

    if (extra != NULL) {
        append(buf, sizeof(buf), &len, "%s", extra);
    }
    if (body != NULL && body[0] != '\0') {
        append(buf, sizeof(buf), &len, "Content-Type: %s\r\nContent-Length: %zu\r\n\r\n%s",
               (ctype != NULL) ? ctype : "application/sdp", strlen(body), body);
    } else {
        append(buf, sizeof(buf), &len, "Content-Length: 0\r\n\r\n");
    }

    udp_send_to((dst != NULL) ? dst : &s_reply_to, buf, len);
    if (code == 200 && req->status == 0) {
        memcpy(s_answer_msg, buf, len);
        s_answer_len = len;
    }
}

static void send_response(const sip_msg_t *req, int code, const char *reason,
                          const char *extra, const char *ctype, const char *body)
{
    send_response_to(s_resp_target, req, code, reason, extra, ctype, body);
}

/* ----------------------------------------------------------- calls: dialing */

static void send_invite_ex(const char *number, bool retry)
{
    char uri[96];
    snprintf(uri, sizeof(uri), "sip:%s@%s:%u", number, s_acc.host, (unsigned)s_acc.port);
    copy_str(s_inv_uri, sizeof(s_inv_uri), uri);

    snprintf(s_inv_branch, sizeof(s_inv_branch), "z9hG4bK%08x%08x",
             (unsigned)esp_random(), (unsigned)(esp_timer_get_time() & 0xffffffffu));

    if (retry) {
        /* Keep Call-ID/From tag, use a new CSeq and branch for the authenticated
         * retry; the 401 to-tag must not leak into the new INVITE. */
        s_dlg_cseq++;
        s_inv_have_auth = build_invite_auth(s_inv_auth, sizeof(s_inv_auth));
    } else {
        snprintf(s_dlg_call_id, sizeof(s_dlg_call_id), "%08x%08x@%s",
                 (unsigned)esp_random(), (unsigned)(esp_timer_get_time() & 0xffffffffu),
                 s_local_ip);
        snprintf(s_local_tag, sizeof(s_local_tag), "%08x", (unsigned)esp_random());
        s_dlg_cseq = 1;
        s_inv_have_auth = false;
        s_inv_auth_tries = 0;
        copy_str(s_dial_number, sizeof(s_dial_number), number);
    }
    s_remote_tag[0] = '\0';
    copy_str(s_remote_uri, sizeof(s_remote_uri), uri);

    char sdp[640];
    build_sdp(sdp, sizeof(sdp), false, RTP_PAYLOAD_TYPE_PCMU, local_rtp_port());

    size_t len = 0;
    append(s_dlg_msg, sizeof(s_dlg_msg), &len,
           "INVITE %s SIP/2.0\r\n"
           "Via: SIP/2.0/UDP %s:%u;rport;branch=%s\r\n"
           "Max-Forwards: 70\r\n"
           "From: \"%s\" <sip:%s@%s>;tag=%s\r\n"
           "To: <sip:%s@%s>\r\n"
           "Call-ID: %s\r\n"
           "CSeq: %u INVITE\r\n"
           "Contact: <sip:%s@%s:%u>\r\n"
           "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, INFO\r\n"
           "User-Agent: XIAO-SIP/1.0\r\n",
           uri,
           s_local_ip, (unsigned)s_local_port, s_inv_branch,
           s_acc.display, s_acc.user, s_acc.host, s_local_tag,
           number, s_acc.host,
           s_dlg_call_id,
           (unsigned)s_dlg_cseq,
           s_acc.user, s_local_ip, (unsigned)s_local_port);

    if (s_inv_have_auth) {
        append(s_dlg_msg, sizeof(s_dlg_msg), &len, "%s: %s\r\n",
               s_inv_proxy_auth ? "Proxy-Authorization" : "Authorization", s_inv_auth);
    }
    append(s_dlg_msg, sizeof(s_dlg_msg), &len,
           "Content-Type: application/sdp\r\n"
           "Content-Length: %zu\r\n"
           "\r\n%s", strlen(sdp), sdp);
    s_dlg_msg_len = len;

    udp_send(s_dlg_msg, s_dlg_msg_len);
    ESP_LOGI(TAG, "[SIP] INVITE -> %s (call-id %s, cseq %u%s)", uri, s_dlg_call_id,
             (unsigned)s_dlg_cseq, s_inv_have_auth ? ", auth" : "");

    s_dlg_rtx_n = 0;
    s_dlg_rtx_ms = SIP_T1_MS;
    s_dlg_rtx_at = now_ms() + SIP_T1_MS;
}

static void handle_invite_response(const sip_msg_t *m)
{
    if (strcmp(m->call_id, s_dlg_call_id) != 0) {
        return;
    }

    if (m->to_tag[0] != '\0' && s_remote_tag[0] == '\0') {
        copy_str(s_remote_tag, sizeof(s_remote_tag), m->to_tag);
    }
    if (m->to[0] != '\0') {
        copy_str(s_remote_to, sizeof(s_remote_to), m->to);
    }
    if (m->contact[0] != '\0') {
        copy_str(s_remote_uri, sizeof(s_remote_uri), m->contact);
        char *lt = strchr(s_remote_uri, '<');
        if (lt != NULL) {
            char *gt = strchr(lt + 1, '>');
            if (gt != NULL) {
                *gt = '\0';
                memmove(s_remote_uri, lt + 1, strlen(lt + 1) + 1);
            }
        }
    }

    if (m->status < 200) {
        s_dlg_rtx_at = 0;
        if (m->status == 180 && s_call == SIP_CALL_DIALING) {
            set_call_state(SIP_CALL_RINGING_OUTGOING);
            ESP_LOGI(TAG, "[SIP] 180 Ringing");
        } else if (m->status == 183) {
            set_call_state(SIP_CALL_RINGING_OUTGOING);
            ESP_LOGI(TAG, "[SIP] 183 Session Progress");
        }
        return;
    }

    s_dlg_rtx_at = 0;

    if (m->status == 200) {
        if (s_call == SIP_CALL_ESTABLISHED) {
            /* Retransmitted 200 OK: repeat the ACK, do not restart media. */
            send_ack();
            return;
        }
        if (m->body != NULL && m->body_len > 0) {
            char ip[16];
            uint16_t port;
            uint8_t pt;
            if (sdp_parse(m->body, ip, sizeof(ip), &port, &pt)) {
                copy_str(s_remote_ip, sizeof(s_remote_ip), ip);
                s_remote_port = port;
                s_pt = pt;
                ESP_LOGI(TAG, "[SIP] SDP: %s:%u pt=%u (%s)", ip, (unsigned)port,
                         (unsigned)pt,
                         (pt == RTP_PAYLOAD_TYPE_PCMA) ? "PCMA" : "PCMU");
            }
        }
        send_ack();
        set_call_state(SIP_CALL_ESTABLISHED);
        ESP_LOGI(TAG, "[PHONE] State: CONNECTED");
        media_start();
        return;
    }

    if (m->status == 401 || m->status == 407) {
        if (m->cseq_num != s_dlg_cseq) {
            return; /* stale challenge for an INVITE we already retried */
        }

        const char *hdr = (m->status == 401) ? m->www_auth : m->proxy_auth;
        char realm[64] = {0};
        char nonce[160] = {0};
        char qop[24] = {0};

        send_ack_for(m->cseq_num);
        if (!sip_param(hdr, "realm", realm, sizeof(realm)) ||
            !sip_param(hdr, "nonce", nonce, sizeof(nonce))) {
            ESP_LOGE(TAG, "[SIP] INVITE challenge without realm/nonce");
            end_call();
            return;
        }

        bool new_nonce = strcmp(nonce, s_inv_nonce) != 0;
        if (!new_nonce && s_inv_auth_tries >= 1) {
            ESP_LOGE(TAG, "[SIP] INVITE rejected after digest (check SIP password)");
            end_call();
            return;
        }
        s_inv_auth_tries++;
        copy_str(s_inv_realm, sizeof(s_inv_realm), realm);
        copy_str(s_inv_nonce, sizeof(s_inv_nonce), nonce);
        s_inv_qop_auth = sip_param(hdr, "qop", qop, sizeof(qop)) && strstr(qop, "auth") != NULL;
        s_inv_proxy_auth = (m->status == 407);
        if (new_nonce) {
            s_inv_nc = 0;
        }

        ESP_LOGI(TAG, "[SIP] INVITE %d challenge realm='%s' qop=%s, retrying with digest",
                 m->status, s_inv_realm, s_inv_qop_auth ? "auth" : "none");
        s_dlg_rtx_at = 0;
        send_invite_ex(s_dial_number, true);
        set_call_state(SIP_CALL_DIALING);
        return;
    }

    /* Other 3xx-6xx: acknowledge, then drop the call. */
    ESP_LOGW(TAG, "[SIP] Call failed: %d %s", m->status, m->start_line);
    send_ack_for(m->cseq_num);
    end_call();
}

/* ------------------------------------------------------------- call handling */

static void handle_incoming_invite(const sip_msg_t *req)
{
    if (s_call != SIP_CALL_IDLE) {
        send_response(req, 486, "Busy Here", NULL, NULL, NULL);
        ESP_LOGW(TAG, "[SIP] INVITE rejected: busy");
        return;
    }

    s_pending_invite = *req;
    s_invite_reply = s_reply_to;
    s_peer_addr = s_reply_to;
    s_peer_direct = true;
    copy_str(s_remote_to, sizeof(s_remote_to), req->from);
    copy_str(s_dlg_call_id, sizeof(s_dlg_call_id), req->call_id);
    copy_str(s_remote_tag, sizeof(s_remote_tag), req->from_tag);
    snprintf(s_local_tag, sizeof(s_local_tag), "%08x", (unsigned)esp_random());
    s_dlg_cseq = 0;

    if (req->contact[0] != '\0') {
        copy_str(s_remote_uri, sizeof(s_remote_uri), req->contact);
    } else {
        snprintf(s_remote_uri, sizeof(s_remote_uri), "sip:%s@%s:%u", s_acc.user,
                 s_acc.host, (unsigned)s_acc.port);
    }

    if (req->body != NULL && req->body_len > 0) {
        char ip[16];
        uint16_t port;
        uint8_t pt;
        if (sdp_parse(req->body, ip, sizeof(ip), &port, &pt)) {
            copy_str(s_remote_ip, sizeof(s_remote_ip), ip);
            s_remote_port = port;
            s_pt = pt;
            ESP_LOGI(TAG, "[SIP] Remote SDP: %s:%u pt=%u (%s)", ip, (unsigned)port,
                     (unsigned)pt, (pt == RTP_PAYLOAD_TYPE_PCMA) ? "PCMA" : "PCMU");
        }
    } else {
        s_pt = RTP_PAYLOAD_TYPE_PCMU;
    }

    peer_from_uri(req->from, s_peer, sizeof(s_peer));

    send_response(req, 100, "Trying", NULL, NULL, NULL);
    send_response(req, 180, "Ringing", NULL, NULL, NULL);
    set_call_state(SIP_CALL_RINGING_INCOMING);
    ESP_LOGI(TAG, "[SIP] Incoming INVITE from '%s'", s_peer);
    ESP_LOGI(TAG, "[PHONE] State: RINGING");
}

static void handle_request(const sip_msg_t *req)
{
    if (strcasecmp(req->method, "INVITE") == 0) {
        if (strcmp(req->call_id, s_dlg_call_id) == 0 &&
            s_call == SIP_CALL_RINGING_INCOMING) {
            /* Retransmitted INVITE: repeat our provisional answer. */
            send_response(req, 180, "Ringing", NULL, NULL, NULL);
            return;
        }
        if (strcmp(req->call_id, s_dlg_call_id) == 0 && s_answer_len > 0 &&
            s_call != SIP_CALL_IDLE) {
            udp_send_to(&s_reply_to, s_answer_msg, s_answer_len);
            return;
        }
        handle_incoming_invite(req);
        return;
    }

    if (strcasecmp(req->method, "ACK") == 0) {
        if (strcmp(req->call_id, s_dlg_call_id) == 0) {
            ESP_LOGI(TAG, "[SIP] ACK received");
            if (s_call == SIP_CALL_ESTABLISHED || s_call == SIP_CALL_TERMINATING) {
                return;
            }
            set_call_state(SIP_CALL_ESTABLISHED);
            ESP_LOGI(TAG, "[PHONE] State: CONNECTED");
            media_start();
        }
        return;
    }

    if (strcasecmp(req->method, "BYE") == 0) {
        send_response(req, 200, "OK", NULL, NULL, NULL);
        ESP_LOGI(TAG, "[SIP] BYE received");
        ESP_LOGI(TAG, "[PHONE] Call Terminated");
        end_call();
        return;
    }

    if (strcasecmp(req->method, "CANCEL") == 0) {
        send_response(req, 200, "OK", NULL, NULL, NULL);
        send_response(req, 487, "Request Terminated", NULL, NULL, NULL);
        ESP_LOGI(TAG, "[SIP] CANCEL received");
        end_call();
        return;
    }

    if (strcasecmp(req->method, "OPTIONS") == 0) {
        char allow[128];
        snprintf(allow, sizeof(allow),
                 "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, INFO\r\n"
                 "Accept: application/sdp\r\n");
        send_response(req, 200, "OK", allow, NULL, NULL);
        return;
    }

    if (strcasecmp(req->method, "INFO") == 0) {
        send_response(req, 200, "OK", NULL, NULL, NULL);
        return;
    }

    send_response(req, 501, "Not Implemented", NULL, NULL, NULL);
}

static void handle_response(const sip_msg_t *m)
{
    if (strcasecmp(m->cseq_method, "REGISTER") == 0) {
        handle_register_response(m);
    } else if (strcasecmp(m->cseq_method, "INVITE") == 0) {
        handle_invite_response(m);
    } else if (strcasecmp(m->cseq_method, "BYE") == 0) {
        if (m->status >= 200) {
            ESP_LOGI(TAG, "[SIP] Call Terminated");
            end_call();
        }
    } else if (strcasecmp(m->cseq_method, "CANCEL") == 0) {
        /* The matching 487 for the INVITE ends the call. */
        ESP_LOGD(TAG, "[SIP] CANCEL response %d", m->status);
    }
}

/* ------------------------------------------------------------------- task */

static void handle_packet(char *buf, int len, const struct sockaddr_in *from)
{
    static sip_msg_t m;
    s_reply_to = *from;
    parse_msg(buf, &m);

    ESP_LOGD(TAG, "[SIP] RX %d bytes: %s", len, m.start_line);

    if (m.status > 0) {
        handle_response(&m);
    } else {
        handle_request(&m);
    }
}

static void sip_task(void *arg)
{
    (void)arg;
    static char buf[SIP_BUF_LEN];

    ESP_LOGI(TAG, "[SIP] Task started on %s:%u -> %s:%u", s_local_ip,
             (unsigned)s_local_port, s_acc.host, (unsigned)s_acc.port);

    while (s_running) {
        struct pollfd pfd = {
            .fd = s_sock,
            .events = POLLIN,
        };
        int pr = poll(&pfd, 1, 50);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            int n = recvfrom(s_sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &fl);
            if (n > 0) {
                buf[n] = '\0';
                handle_packet(buf, n, &from);
            }
        }

        int64_t now = now_ms();

        /* The APs on this SSID hand out different subnets; after a roam the
         * Via/Contact and the existing binding are stale, so re-register with
         * the new address (and drop a call the old address was carrying). */
        if (now >= s_ip_check_at) {
            s_ip_check_at = now + 2000;

            bool link_up = wifi_manager_is_connected();
            if (link_up != s_link_up) {
                s_link_up = link_up;
                if (link_up) {
                    char new_ip[16] = {0};
                    if (wifi_manager_get_ip(new_ip, sizeof(new_ip)) == ESP_OK) {
                        copy_str(s_local_ip, sizeof(s_local_ip), new_ip);
                    }
                    ESP_LOGI(TAG, "[SIP] Link is back (%s), refreshing registration",
                             s_local_ip);
                    s_reg_rtx_at = 0;
                    s_reg_refresh_at = 0;
                    s_reg_retry_at = 0;
                    s_fail_count = 0;
                    s_reg = SIP_REG_STATE_REGISTERING;
                    if (s_local_ip[0] != '\0') {
                        send_register_ex((s_acc.expiry != 0) ? s_acc.expiry : 300);
                    }
                } else {
                    ESP_LOGW(TAG, "[SIP] Link down, registration paused");
                    s_reg = SIP_REG_STATE_UNREGISTERED;
                    s_reg_rtx_at = 0;
                    s_reg_refresh_at = 0;
                }
            }

            char ip[16] = {0};
            if (wifi_manager_is_connected() &&
                wifi_manager_get_ip(ip, sizeof(ip)) == ESP_OK && ip[0] != '\0' &&
                strcmp(ip, s_local_ip) != 0) {
                ESP_LOGW(TAG, "[SIP] Local IP changed %s -> %s, re-registering",
                         s_local_ip, ip);
                copy_str(s_local_ip, sizeof(s_local_ip), ip);
                if (s_call != SIP_CALL_IDLE) {
                    ESP_LOGW(TAG, "[SIP] Dropping call after address change");
                    end_call();
                }
                s_reg_rtx_at = 0;
                s_reg_refresh_at = 0;
                s_reg_retry_at = 0;
                s_nonce[0] = '\0';
                s_realm[0] = '\0';
                s_fail_count = 0;
                s_reg = SIP_REG_STATE_REGISTERING;
                send_register_ex((s_acc.expiry != 0) ? s_acc.expiry : 300);
            }
        }

        if (s_reg_rtx_at != 0 && now >= s_reg_rtx_at) {
            if (s_reg_rtx_n < SIP_RTX_MAX) {
                s_reg_rtx_n++;
                s_reg_rtx_ms *= 2;
                s_reg_rtx_at = now + s_reg_rtx_ms;
                udp_send(s_reg_msg, s_reg_msg_len);
                ESP_LOGW(TAG, "[SIP] REGISTER retransmit %d", s_reg_rtx_n);
            } else {
                s_reg_rtx_at = 0;
                fail_registration("timeout, no response", 30);
            }
        }

        if (s_reg_refresh_at != 0 && now >= s_reg_refresh_at &&
            s_reg == SIP_REG_STATE_REGISTERED) {
            s_reg_refresh_at = 0;
            ESP_LOGI(TAG, "[SIP] Registration refresh");
            send_register_ex((s_acc.expiry != 0) ? s_acc.expiry : 300);
        }

        if (s_reg_retry_at != 0 && now >= s_reg_retry_at) {
            s_reg_retry_at = 0;
            ESP_LOGI(TAG, "[SIP] Retrying registration");
            s_reg = SIP_REG_STATE_REGISTERING;
            send_register_ex((s_acc.expiry != 0) ? s_acc.expiry : 300);
        }

        if (s_dlg_rtx_at != 0 && now >= s_dlg_rtx_at) {
            if (s_dlg_rtx_n < SIP_RTX_MAX) {
                s_dlg_rtx_n++;
                s_dlg_rtx_ms *= 2;
                s_dlg_rtx_at = now + s_dlg_rtx_ms;
                udp_send(s_dlg_msg, s_dlg_msg_len);
                ESP_LOGW(TAG, "[SIP] INVITE retransmit %d", s_dlg_rtx_n);
            } else {
                s_dlg_rtx_at = 0;
                ESP_LOGE(TAG, "[SIP] Call failed: no response");
                end_call();
            }
        }
    }

    ESP_LOGI(TAG, "[SIP] Task stopped");
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------- API */

esp_err_t sip_client_init(void)
{
    if (s_sock >= 0) {
        return ESP_OK;
    }

    const phone_config_t *cfg = config_manager_get();
    if (cfg->sip.server[0] == '\0' || cfg->sip.username[0] == '\0') {
        ESP_LOGW(TAG, "[SIP] Server/username not configured, not starting");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_acc, 0, sizeof(s_acc));
    copy_str(s_acc.host, sizeof(s_acc.host), cfg->sip.server);
    s_acc.port = (cfg->sip.port != 0) ? cfg->sip.port : 5060;
    copy_str(s_acc.user, sizeof(s_acc.user), cfg->sip.username);
    copy_str(s_acc.auth_id, sizeof(s_acc.auth_id), cfg->sip.auth_id);
    copy_str(s_acc.pass, sizeof(s_acc.pass), cfg->sip.password);
    copy_str(s_acc.ext, sizeof(s_acc.ext),
             (cfg->sip.extension[0] != '\0') ? cfg->sip.extension : cfg->sip.username);
    copy_str(s_acc.display, sizeof(s_acc.display),
             (cfg->sip.display_name[0] != '\0') ? cfg->sip.display_name : s_acc.ext);
    s_acc.expiry = (cfg->sip.register_expiry_s != 0) ? cfg->sip.register_expiry_s : 300;

    s_local_ip[0] = '\0';
    if (wifi_manager_get_ip(s_local_ip, sizeof(s_local_ip)) != ESP_OK) {
        /* DHCP can take longer than the boot wait; start the UA anyway and
         * register as soon as the link reports an address. */
        ESP_LOGW(TAG, "[SIP] No IP yet; registration deferred until the link is up");
        s_local_ip[0] = '\0';
    }

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "[SIP] socket() failed errno=%d", errno);
        return ESP_FAIL;
    }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(5060);
    if (bind(s_sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        ESP_LOGW(TAG, "[SIP] bind 5060 failed (errno=%d), using ephemeral port", errno);
        local.sin_port = 0;
        if (bind(s_sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
            ESP_LOGE(TAG, "[SIP] bind failed errno=%d", errno);
            close(s_sock);
            s_sock = -1;
            return ESP_FAIL;
        }
    }

    socklen_t sl = sizeof(local);
    if (getsockname(s_sock, (struct sockaddr *)&local, &sl) == 0) {
        s_local_port = ntohs(local.sin_port);
    }

    resolve_destination();

    uint16_t rtp_port = 0;
    if (rtp_client_open(&rtp_port) == ESP_OK) {
        s_rtp_port = rtp_port;
    }

    snprintf(s_reg_call_id, sizeof(s_reg_call_id), "%08x%08x@%s",
             (unsigned)esp_random(), (unsigned)(esp_timer_get_time() & 0xffffffffu),
             s_local_ip);
    snprintf(s_reg_from_tag, sizeof(s_reg_from_tag), "%08x", (unsigned)esp_random());

    ESP_LOGI(TAG, "[SIP] Initializing SIP UA");
    ESP_LOGI(TAG, "[SIP] Server: %s:%u", s_acc.host, (unsigned)s_acc.port);
    ESP_LOGI(TAG, "[SIP] Extension: %s", s_acc.ext);
    /* Which identity digest answers with, and whether a secret exists at all:
     * a stale auth id or an empty password looks identical to "server rejected
     * us" in the 401 loop. */
    ESP_LOGI(TAG, "[SIP] Digest user: %s (auth id '%s'), password %s (%u chars)",
             (s_acc.auth_id[0] != '\0') ? s_acc.auth_id : s_acc.user, s_acc.user,
             (s_acc.pass[0] != '\0') ? "set" : "EMPTY", (unsigned)strlen(s_acc.pass));

    s_link_up = wifi_manager_is_connected();
    s_running = true;
    if (xTaskCreatePinnedToCore(sip_task, "sip_task", 8192, NULL, 5, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "[SIP] Failed to create SIP task");
        s_running = false;
        close(s_sock);
        s_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    s_reg = SIP_REG_STATE_REGISTERING;
    if (s_local_ip[0] != '\0') {
        ESP_LOGI(TAG, "[SIP] Registering...");
        send_register();
    }
    return ESP_OK;
}

esp_err_t sip_client_register(void)
{
    if (s_sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_call != SIP_CALL_IDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_reg = SIP_REG_STATE_REGISTERING;
    send_register();
    return ESP_OK;
}

esp_err_t sip_client_unregister(void)
{
    if (s_sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "[SIP] Sending unregister (Expires: 0)");
    send_register_ex(0);
    s_reg = SIP_REG_STATE_UNREGISTERED;
    s_reg_refresh_at = 0;
    return ESP_OK;
}

esp_err_t sip_client_dial(const char *number)
{
    if (s_sock < 0 || number == NULL || number[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_call != SIP_CALL_IDLE) {
        ESP_LOGW(TAG, "[SIP] Cannot dial '%s': line busy", number);
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(s_peer, sizeof(s_peer), "%s", number);
    set_call_state(SIP_CALL_DIALING);
    send_invite_ex(number, false);
    ESP_LOGI(TAG, "[PHONE] State: DIALING");
    return ESP_OK;
}

esp_err_t sip_client_answer(void)
{
    if (s_sock < 0 || s_call != SIP_CALL_RINGING_INCOMING) {
        return ESP_ERR_INVALID_STATE;
    }

    char sdp[640];
    build_sdp(sdp, sizeof(sdp), true, s_pt, local_rtp_port());
    char extra[256];
    snprintf(extra, sizeof(extra),
             "Contact: <sip:%s@%s:%u>\r\n"
             "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, INFO\r\n",
             s_acc.user, s_local_ip, (unsigned)s_local_port);

    send_response_to(&s_invite_reply, &s_pending_invite, 200, "OK", extra, "application/sdp", sdp);
    ESP_LOGI(TAG, "[SIP] Sending 200 OK");
    set_call_state(SIP_CALL_ESTABLISHED);
    ESP_LOGI(TAG, "[PHONE] State: CONNECTED");
    media_start();
    return ESP_OK;
}

esp_err_t sip_client_hangup(void)
{
    if (s_sock < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (s_call) {
    case SIP_CALL_DIALING:
    case SIP_CALL_RINGING_OUTGOING:
        s_dlg_cseq++;
        send_simple_request("CANCEL", s_dlg_cseq, s_remote_tag, s_inv_branch);
        ESP_LOGI(TAG, "[SIP] Cancelling call");
        end_call();
        return ESP_OK;

    case SIP_CALL_ESTABLISHED:
        ESP_LOGI(TAG, "[SIP] Sending BYE");
        send_bye();
        return ESP_OK;

    case SIP_CALL_RINGING_INCOMING:
        send_response(&s_pending_invite, 486, "Busy Here", NULL, NULL, NULL);
        ESP_LOGI(TAG, "[SIP] Call rejected");
        end_call();
        return ESP_OK;

    default:
        return ESP_ERR_INVALID_STATE;
    }
}

sip_reg_state_t sip_client_get_reg_state(void)
{
    return s_reg;
}

sip_call_state_t sip_client_get_call_state(void)
{
    return s_call;
}

void sip_client_get_peer(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    snprintf(buf, len, "%s", s_peer);
}
