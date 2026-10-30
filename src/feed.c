/*
 * feed.c - live top-of-book feed from Binance's public REST API.
 *
 * One batched GET /api/v3/ticker/bookTicker per poll returns best bid/ask
 * and size for every configured symbol. A single curl handle is reused so
 * the TLS connection stays open between polls.
 */
#include "hft.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    char  *data;
    size_t len, cap;
} Buf;

static size_t on_write(char *ptr, size_t sz, size_t nm, void *ud)
{
    Buf *b = ud;
    size_t n = sz * nm;
    if (b->len + n + 1 > b->cap) {
        size_t cap = (b->cap ? b->cap * 2 : 8192);
        while (cap < b->len + n + 1) cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d) return 0;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

/* Find "key":"value" inside [obj, end) and copy value. Binance sends all
 * numbers in this endpoint as JSON strings, so this is all we need. */
static int json_str(const char *obj, const char *end, const char *key,
                    char *out, size_t n)
{
    char pat[48];
    int pl = snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = memmem(obj, (size_t)(end - obj), pat, (size_t)pl);
    if (!p) return -1;
    p += pl;
    const char *q = memchr(p, '"', (size_t)(end - p));
    if (!q || (size_t)(q - p) >= n) return -1;
    memcpy(out, p, (size_t)(q - p));
    out[q - p] = '\0';
    return 0;
}

/* Parse the JSON array of bookTicker objects into `out`. Returns symbols updated. */
static int parse_book(const char *body, Quote *out, int nsym)
{
    int updated = 0;
    const char *p = body;
    while ((p = strchr(p, '{')) != NULL) {
        const char *e = strchr(p, '}');
        if (!e) break;
        char sym[16], bp[32], bq[32], ap[32], aq[32];
        if (json_str(p, e, "symbol", sym, sizeof sym) == 0 &&
            json_str(p, e, "bidPrice", bp, sizeof bp) == 0 &&
            json_str(p, e, "bidQty", bq, sizeof bq) == 0 &&
            json_str(p, e, "askPrice", ap, sizeof ap) == 0 &&
            json_str(p, e, "askQty", aq, sizeof aq) == 0) {
            for (int i = 0; i < nsym; i++) {
                if (strcmp(out[i].sym, sym) != 0) continue;
                double bid = atof(bp), ask = atof(ap);
                if (bid <= 0 || ask <= 0 || ask < bid) break;
                out[i].bid = bid;
                out[i].ask = ask;
                out[i].bidq = atof(bq);
                out[i].askq = atof(aq);
                if (!out[i].valid) out[i].first_mid = (bid + ask) / 2;
                out[i].valid = 1;
                updated++;
                break;
            }
        }
        p = e + 1;
    }
    return updated;
}

static void build_url(const Feed *f, char *url, size_t n)
{
    /* symbols=["A","B"] URL-encoded */
    int k = snprintf(url, n, "%s/api/v3/ticker/bookTicker?symbols=%%5B", f->api_base);
    for (int i = 0; i < f->nsym && (size_t)k < n; i++)
        k += snprintf(url + k, n - (size_t)k, "%s%%22%s%%22", i ? "%2C" : "", f->q[i].sym);
    if ((size_t)k < n) snprintf(url + k, n - (size_t)k, "%%5D");
}

static void set_err(Feed *f, const char *msg)
{
    pthread_mutex_lock(&f->mu);
    snprintf(f->err, sizeof f->err, "%s", msg);
    f->nerr++;
    pthread_mutex_unlock(&f->mu);
}

static void *feed_main(void *arg)
{
    Feed *f = arg;
    char url[1024];
    build_url(f, url, sizeof url);

    CURL *c = curl_easy_init();
    if (!c) {
        set_err(f, "curl_easy_init failed");
        return NULL;
    }
    Buf buf = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "hft-sim/1.0");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");

    Quote local[MAX_SYMBOLS];
    pthread_mutex_lock(&f->mu);
    int nsym = f->nsym;
    memcpy(local, f->q, sizeof local);
    pthread_mutex_unlock(&f->mu);

    while (!f->stop) {
        double t0 = mono_ms();
        buf.len = 0;
        CURLcode rc = curl_easy_perform(c);
        double rtt = mono_ms() - t0;
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);

        if (rc != CURLE_OK) {
            char msg[200];
            snprintf(msg, sizeof msg, "network: %s", curl_easy_strerror(rc));
            set_err(f, msg);
        } else if (status != 200) {
            char msg[200];
            snprintf(msg, sizeof msg, "HTTP %ld: %.150s", status, buf.data ? buf.data : "");
            set_err(f, msg);
        } else if (parse_book(buf.data, local, nsym) == 0) {
            set_err(f, "no symbols in response (check symbol names)");
        } else {
            pthread_mutex_lock(&f->mu);
            memcpy(f->q, local, sizeof(Quote) * (size_t)nsym);
            f->seq++;
            f->last_ok_ms = mono_ms();
            f->rtt_ms = rtt;
            f->err[0] = '\0';
            pthread_mutex_unlock(&f->mu);
        }

        /* Back off to 2s after errors so we don't hammer a failing endpoint. */
        double wait = (rc == CURLE_OK && status == 200 ? f->poll_ms : 2000) - (mono_ms() - t0);
        while (wait > 0 && !f->stop) {
            double step = wait > 50 ? 50 : wait;
            struct timespec ts = {0, (long)(step * 1e6)};
            nanosleep(&ts, NULL);
            wait -= step;
        }
    }
    free(buf.data);
    curl_easy_cleanup(c);
    return NULL;
}

int feed_start(Feed *f, pthread_t *th)
{
    f->stop = 0;
    return pthread_create(th, NULL, feed_main, f);
}

void feed_stop(Feed *f, pthread_t th)
{
    f->stop = 1;
    pthread_join(th, NULL);
}
