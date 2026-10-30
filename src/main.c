/*
 * main.c - HFT paper-trading simulator on live Binance market data.
 *
 *   hft-sim [config]                 interactive dashboard
 *   hft-sim [config] --headless N    run N seconds, print a summary
 */
#include "hft.h"

#include <ctype.h>
#include <curl/curl.h>
#include <curses.h>
#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

double wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Copy the latest quotes if the feed has moved on. Returns 1 on new data. */
static int snapshot(Feed *f, Quote *q, unsigned long *seq)
{
    int fresh = 0;
    pthread_mutex_lock(&f->mu);
    if (f->seq != *seq) {
        memcpy(q, f->q, sizeof(Quote) * (size_t)f->nsym);
        *seq = f->seq;
        fresh = 1;
    }
    pthread_mutex_unlock(&f->mu);
    return fresh;
}

static void print_summary(App *app, const Quote *q, int nsym, FILE *out)
{
    fprintf(out, "\n%-14s %-10s %12s %12s %12s %6s %5s %5s %6s %5s\n", "PROFILE", "STRATEGY",
            "START EQ", "END EQ", "STRAT P&L", "TRADES", "W", "L", "WIN%", "REJ");
    for (int k = 0; k < app->nprof; k++) {
        Profile *p = &app->prof[k];
        if (!p->started) {
            fprintf(out, "%-14s %-10s (never received prices)\n", p->name, p->strat_name);
            continue;
        }
        int n = p->wins + p->losses;
        fprintf(out, "%-14s %-10s %12.2f %12.2f %+12.4f %6d %5d %5d %5.1f%% %5d\n", p->name,
                p->strat_name, p->start_equity, profile_equity(p, q, nsym),
                p->realized + profile_unrealized(p, q, nsym), n, p->wins, p->losses,
                n ? 100.0 * p->wins / n : 0.0, p->rejects);
    }
    fprintf(out, "Trade logs: logs/<profile>.csv\n");
}

/* Interactive "new profile" wizard. Writes the profile back to the config. */
static void new_profile(App *app, char *msg, size_t msgn)
{
    if (app->nprof >= MAX_PROFILES) {
        snprintf(msg, msgn, "Maximum of %d profiles reached", MAX_PROFILES);
        return;
    }
    Profile tmp;
    profile_defaults(&tmp, app->feed.nsym);
    char in[256], err[256] = "";

    if (ui_prompt("New profile name (empty cancels):", in, 32)) return;
    for (char *c = in; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-') {
            snprintf(msg, msgn, "Name may only use letters, digits, '_' and '-'");
            return;
        }
    for (int k = 0; k < app->nprof; k++)
        if (!strcasecmp(app->prof[k].name, in)) {
            snprintf(msg, msgn, "Profile '%.31s' already exists", in);
            return;
        }
    snprintf(tmp.name, sizeof tmp.name, "%.31s", in);

    if (!ui_prompt("Strategy [momentum|meanrev|imbalance] (default imbalance):", in, sizeof in) &&
        profile_set(app, &tmp, "strategy", in, err, sizeof err))
        goto fail;
    if (!ui_prompt("Starting cash (default 10000):", in, sizeof in) &&
        profile_set(app, &tmp, "cash", in, err, sizeof err))
        goto fail;
    char hint[200];
    int k = snprintf(hint, sizeof hint, "Starting holdings e.g. ");
    for (int i = 0; i < app->feed.nsym && i < 2; i++)
        k += snprintf(hint + k, sizeof hint - (size_t)k, "%s%s=%s", i ? "," : "",
                      app->feed.q[i].base, i ? "2" : "0.05");
    snprintf(hint + k, sizeof hint - (size_t)k, " (empty for none):");
    if (!ui_prompt(hint, in, sizeof in)) {
        for (char *tok = strtok(in, ", "); tok; tok = strtok(NULL, ", ")) {
            char *eq = strchr(tok, '=');
            if (!eq) {
                snprintf(err, sizeof err, "expected ASSET=QTY, got '%s'", tok);
                goto fail;
            }
            *eq = '\0';
            char key[48];
            snprintf(key, sizeof key, "hold.%s", tok);
            if (profile_set(app, &tmp, key, eq + 1, err, sizeof err)) goto fail;
        }
    }
    if (profile_validate(&tmp, err, sizeof err)) goto fail;

    Profile *p = &app->prof[app->nprof];
    *p = tmp;
    profile_open_csv(p);
    app->nprof++;
    if (config_append_profile(app->conf_path, p, app))
        snprintf(msg, msgn, "Added '%s' (could not save to %.180s)", p->name, app->conf_path);
    else
        snprintf(msg, msgn, "Added '%s' and saved it to %.180s", p->name, app->conf_path);
    return;
fail:
    snprintf(msg, msgn, "Not added: %.200s", err);
}

int main(int argc, char **argv)
{
    const char *conf = "profiles.conf";
    int headless = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless") && i + 1 < argc) {
            headless = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: %s [config] [--headless SECONDS]\n", argv[0]);
            return 0;
        } else {
            conf = argv[i];
        }
    }

    setlocale(LC_ALL, "");
    static App app;
    char err[256];
    pthread_mutex_init(&app.feed.mu, NULL);
    if (config_load(&app, conf, err, sizeof err)) {
        fprintf(stderr, "config error: %s\n", err);
        return 1;
    }
    for (int k = 0; k < app.nprof; k++)
        if (profile_open_csv(&app.prof[k]))
            fprintf(stderr, "warning: cannot open trade log for %s\n", app.prof[k].name);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    pthread_t th;
    if (feed_start(&app.feed, &th)) {
        fprintf(stderr, "cannot start feed thread\n");
        return 1;
    }

    int nsym = app.feed.nsym;
    Quote q[MAX_SYMBOLS] = {0};
    unsigned long seq = 0;
    double start = mono_ms();

    if (headless) {
        printf("Running %d profile(s) on %d symbol(s) for %ds...\n", app.nprof, nsym, headless);
        double last_report = start;
        while (!g_quit && mono_ms() - start < headless * 1000.0) {
            if (snapshot(&app.feed, q, &seq)) engine_tick(&app, q, nsym, mono_ms(), wall_ms());
            if (mono_ms() - last_report > 5000) {
                pthread_mutex_lock(&app.feed.mu);
                printf("  t=%3.0fs ticks=%lu rtt=%.0fms %s\n", (mono_ms() - start) / 1000,
                       app.feed.seq, app.feed.rtt_ms, app.feed.err);
                pthread_mutex_unlock(&app.feed.mu);
                fflush(stdout);
                last_report = mono_ms();
            }
            struct timespec ts = {0, 5 * 1000000};
            nanosleep(&ts, NULL);
        }
    } else {
        ui_init();
        int sel = 0;
        char msg[256] = "";
        double msg_until = 0, last_draw = 0;
        while (!g_quit) {
            int ch = getch();
            double now = mono_ms();
            int dirty = ch != ERR;
            switch (ch) {
            case 'q': case 'Q': g_quit = 1; break;
            case KEY_UP: case 'k': if (sel > 0) sel--; break;
            case KEY_DOWN: case 'j': if (sel < app.nprof - 1) sel++; break;
            case ' ':
                app.prof[sel].paused ^= 1;
                break;
            case 'a': {
                int any_running = 0;
                for (int k = 0; k < app.nprof; k++) any_running |= !app.prof[k].paused;
                for (int k = 0; k < app.nprof; k++) app.prof[k].paused = any_running;
                break;
            }
            case 'f':
                engine_flatten(&app, &app.prof[sel], q, nsym, now, wall_ms());
                snprintf(msg, sizeof msg, "Flattened open positions for %s", app.prof[sel].name);
                msg_until = now + 3000;
                break;
            case 'n':
                new_profile(&app, msg, sizeof msg);
                msg_until = mono_ms() + 5000;
                if (app.nprof) sel = app.nprof - 1;
                break;
            }
            if (snapshot(&app.feed, q, &seq)) {
                engine_tick(&app, q, nsym, mono_ms(), wall_ms());
                dirty = 1;
            }
            now = mono_ms();
            if (now > msg_until) msg[0] = '\0';
            if (dirty || now - last_draw > 250) {
                ui_draw(&app, q, nsym, sel, msg);
                last_draw = now;
            }
        }
        ui_end();
    }

    feed_stop(&app.feed, th);
    print_summary(&app, q, nsym, stdout);
    for (int k = 0; k < app.nprof; k++)
        if (app.prof[k].csv) fclose(app.prof[k].csv);
    curl_global_cleanup();
    return 0;
}
