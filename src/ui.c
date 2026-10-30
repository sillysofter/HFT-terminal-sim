/*
 * ui.c - ncurses dashboard.
 *
 *   header / feed status
 *   live market table
 *   profile leaderboard
 *   selected profile detail | selected profile trade log
 *   key help
 */
#include "hft.h"

#include <curses.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

enum { C_GREEN = 1, C_RED, C_YELLOW, C_CYAN, C_HEAD, C_SEL, C_DIM };

void ui_init(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(20);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(C_GREEN, COLOR_GREEN, -1);
        init_pair(C_RED, COLOR_RED, -1);
        init_pair(C_YELLOW, COLOR_YELLOW, -1);
        init_pair(C_CYAN, COLOR_CYAN, -1);
        init_pair(C_HEAD, COLOR_BLACK, COLOR_CYAN);
        init_pair(C_SEL, COLOR_BLACK, COLOR_WHITE);
        init_pair(C_DIM, COLOR_BLUE, -1);
    }
}

void ui_end(void)
{
    endwin();
}

/* printf at (y,x), clipped to `w` columns (or the screen edge if w<=0). */
static void put(int y, int x, int w, attr_t a, const char *fmt, ...)
{
    if (y < 0 || y >= LINES || x >= COLS) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int max = COLS - x;
    if (w > 0 && w < max) max = w;
    attron(a);
    mvaddnstr(y, x, buf, max);
    attroff(a);
}

static attr_t pnl_attr(double v)
{
    return v > 1e-9 ? COLOR_PAIR(C_GREEN) : v < -1e-9 ? COLOR_PAIR(C_RED) : A_NORMAL;
}

static int px_prec(double px)
{
    return px >= 1000 ? 2 : px >= 10 ? 3 : px >= 1 ? 4 : 6;
}

static void clock_str(double wall, char *out, size_t n, int ms)
{
    time_t t = (time_t)(wall / 1000);
    struct tm tm;
    localtime_r(&t, &tm);
    size_t k = strftime(out, n, "%H:%M:%S", &tm);
    if (ms && k < n) snprintf(out + k, n - k, ".%03d", (int)fmod(wall, 1000));
}

static void draw_header(App *app, int y)
{
    Feed *f = &app->feed;
    pthread_mutex_lock(&f->mu);
    double age = f->seq ? mono_ms() - f->last_ok_ms : -1;
    double rtt = f->rtt_ms;
    unsigned long seq = f->seq, nerr = f->nerr;
    char err[200];
    snprintf(err, sizeof err, "%s", f->err);
    pthread_mutex_unlock(&f->mu);

    const char *state = !seq ? "CONNECTING" : age > 3000 ? "STALE" : "LIVE";
    char now[16];
    clock_str(wall_ms(), now, sizeof now, 0);

    attron(COLOR_PAIR(C_HEAD));
    mvhline(y, 0, ' ', COLS);
    attroff(COLOR_PAIR(C_HEAD));
    put(y, 1, 0, COLOR_PAIR(C_HEAD) | A_BOLD, "HFT-SIM");
    put(y, 9, 0, COLOR_PAIR(C_HEAD),
        "paper trading on live %s  | feed %-10s rtt %4.0fms  age %5.0fms  ticks %lu  errors %lu",
        f->api_base + (strncmp(f->api_base, "https://", 8) ? 0 : 8), state, rtt,
        age < 0 ? 0 : age, seq, nerr);
    put(y, COLS - 9, 0, COLOR_PAIR(C_HEAD), "%s", now);
    if (err[0]) put(y + 1, 1, 0, COLOR_PAIR(C_RED) | A_BOLD, "feed error: %s", err);
}

static void draw_market(const Quote *q, int nsym, int y)
{
    put(y, 1, 0, COLOR_PAIR(C_CYAN) | A_BOLD,
        "%-10s %14s %14s %8s %12s %12s %6s %9s", "SYMBOL", "BID", "ASK", "SPRD bp",
        "BID QTY", "ASK QTY", "IMB", "SESSION");
    for (int i = 0; i < nsym; i++) {
        const Quote *o = &q[i];
        int r = y + 1 + i;
        if (!o->valid) {
            put(r, 1, 0, COLOR_PAIR(C_DIM), "%-10s waiting for data...", o->sym);
            continue;
        }
        double mid = (o->bid + o->ask) / 2;
        double sp = (o->ask - o->bid) / mid * 1e4;
        double imb = o->bidq / (o->bidq + o->askq);
        double chg = (mid / o->first_mid - 1) * 100;
        int pr = px_prec(mid);
        put(r, 1, 0, A_BOLD, "%-10s", o->sym);
        put(r, 12, 0, COLOR_PAIR(C_GREEN), "%14.*f", pr, o->bid);
        put(r, 27, 0, COLOR_PAIR(C_RED), "%14.*f", pr, o->ask);
        put(r, 42, 0, A_NORMAL, "%8.2f %12.4f %12.4f", sp, o->bidq, o->askq);
        put(r, 77, 0, imb > 0.6 ? COLOR_PAIR(C_GREEN) : imb < 0.4 ? COLOR_PAIR(C_RED) : A_NORMAL,
            "%6.2f", imb);
        put(r, 84, 0, pnl_attr(chg), "%+8.3f%%", chg);
    }
}

static void draw_profiles(App *app, const Quote *q, int nsym, int sel, int y)
{
    put(y, 1, 0, COLOR_PAIR(C_CYAN) | A_BOLD,
        "%-2s %-14s %-10s %13s %11s %11s %6s %4s %4s %6s %4s  %-8s", "#", "PROFILE",
        "STRATEGY", "EQUITY", "Δ EQUITY", "STRAT P&L", "TRADES", "W", "L", "WIN%", "REJ", "STATE");
    for (int k = 0; k < app->nprof; k++) {
        Profile *p = &app->prof[k];
        int r = y + 1 + k;
        attr_t base = k == sel ? (COLOR_PAIR(C_SEL) | A_BOLD) : A_NORMAL;
        if (k == sel) {
            attron(base);
            mvhline(r, 0, ' ', COLS);
            attroff(base);
        }
        put(r, 0, 0, base, "%s%-2d %-14.14s %-10s", k == sel ? ">" : " ", k + 1, p->name,
            p->strat_name);
        if (!p->started) {
            put(r, 30, 0, base | (k == sel ? 0 : COLOR_PAIR(C_DIM)), "waiting for first prices");
            continue;
        }
        double eq = profile_equity(p, q, nsym);
        double strat = p->realized + profile_unrealized(p, q, nsym);
        int n = p->wins + p->losses;
        attr_t ga = k == sel ? base : pnl_attr(eq - p->start_equity);
        attr_t sa = k == sel ? base : pnl_attr(strat);
        put(r, 30, 0, base, "%13.2f", eq);
        put(r, 44, 0, ga, "%+11.2f", eq - p->start_equity);
        put(r, 56, 0, sa, "%+11.4f", strat);
        put(r, 67, 0, base, " %6d %4d %4d %5.1f%% %4d  ", n, p->wins, p->losses,
            n ? 100.0 * p->wins / n : 0.0, p->rejects);
        put(r, 98, 0, base | (k == sel || !p->paused ? 0 : COLOR_PAIR(C_YELLOW)), "%-8s",
            p->paused ? "PAUSED" : "RUNNING");
    }
}

static void sparkline(int y, int x, int w, const Profile *p)
{
    static const char *blk[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    int n = p->eq_n < w ? p->eq_n : w;
    if (n < 2) {
        put(y, x, w, COLOR_PAIR(C_DIM), "(collecting...)");
        return;
    }
    double lo = 1e300, hi = -1e300;
    for (int i = 0; i < n; i++) {
        double v = p->eq_hist[(p->eq_head - n + i + EQ_HIST) % EQ_HIST];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    for (int i = 0; i < n; i++) {
        double v = p->eq_hist[(p->eq_head - n + i + EQ_HIST) % EQ_HIST];
        int lvl = hi - lo < 1e-9 ? 3 : (int)((v - lo) / (hi - lo) * 7.999);
        attr_t a = v >= p->start_equity ? COLOR_PAIR(C_GREEN) : COLOR_PAIR(C_RED);
        attron(a);
        mvaddstr(y, x + i, blk[lvl]);
        attroff(a);
    }
}

static void draw_detail(App *app, const Quote *q, int nsym, Profile *p, int y, int x, int w,
                        int bottom)
{
    const StratParams *sp = &p->sp;
    put(y, x, w, A_BOLD | COLOR_PAIR(C_CYAN), "%s", p->name);
    put(y, x + (int)strlen(p->name) + 1, w, p->paused ? COLOR_PAIR(C_YELLOW) : COLOR_PAIR(C_GREEN),
        "[%s]", p->paused ? "PAUSED - exits only" : "RUNNING");
    char params[160];
    switch (sp->kind) {
    case STRAT_MOMENTUM:
        snprintf(params, sizeof params, "EMA %g/%g  thr %.2fbp", sp->fast, sp->slow, sp->threshold_bps);
        break;
    case STRAT_MEANREV:
        snprintf(params, sizeof params, "window %d  z %.2f", sp->window, sp->z_entry);
        break;
    case STRAT_IMBALANCE:
        snprintf(params, sizeof params, "imbalance > %.2f", sp->imb_entry);
        break;
    }
    put(y + 1, x, w, COLOR_PAIR(C_DIM), "%s: %s   $%.0f/order", p->strat_name, params,
        sp->order_usd);
    put(y + 2, x, w, COLOR_PAIR(C_DIM), "tp %.1fbp  sl %.1fbp  fee %.1fbp/side  max hold %ds",
        sp->tp_bps, sp->sl_bps, sp->fee_bps, sp->max_hold_ms / 1000);
    if (!p->started) {
        put(y + 3, x, w, COLOR_PAIR(C_DIM), "Waiting for prices on all symbols...");
        return;
    }

    double eq = profile_equity(p, q, nsym);
    double un = profile_unrealized(p, q, nsym);
    double strat = p->realized + un;
    int n = p->wins + p->losses;
    int r = y + 4;

    put(r, x, w, A_NORMAL, "Strategy P&L ");
    put(r, x + 13, w - 13, pnl_attr(strat) | A_BOLD, "%+.4f", strat);
    put(r++, x + 27, w - 27, COLOR_PAIR(C_DIM), "realized %+.4f  open %+.4f", p->realized, un);
    put(r, x, w, A_NORMAL, "Equity       %.2f", eq);
    put(r++, x + 27, w - 27, pnl_attr(eq - p->start_equity), "Δ %+.2f (%+.3f%%)",
        eq - p->start_equity, (eq / p->start_equity - 1) * 100);
    put(r, x, w, A_NORMAL, "Round trips  %-5d", n);
    put(r, x + 19, w, COLOR_PAIR(C_GREEN) | A_BOLD, "W %d", p->wins);
    put(r, x + 27, w, COLOR_PAIR(C_RED) | A_BOLD, "L %d", p->losses);
    put(r++, x + 35, w - 35, A_NORMAL, "win rate %.1f%%", n ? 100.0 * p->wins / n : 0.0);
    char pf[16] = "n/a";
    if (p->gross_loss > 0) snprintf(pf, sizeof pf, "%.2f", p->gross_win / p->gross_loss);
    put(r++, x, w, A_NORMAL, "Avg win      %+.4f   avg loss %+.4f   profit factor %s",
        p->wins ? p->gross_win / p->wins : 0.0, p->losses ? -p->gross_loss / p->losses : 0.0, pf);
    put(r++, x, w, A_NORMAL, "Fees paid    %.4f   max drawdown %.4f", p->fees_paid, p->max_dd);
    put(r, x, w, A_NORMAL, "Rejected     ");
    put(r++, x + 13, w - 13, p->rejects ? COLOR_PAIR(C_YELLOW) : A_NORMAL, "%d orders", p->rejects);
    put(r++, x, w, A_NORMAL, "Decision     avg %.1fµs  max %.1fµs  (%lu ticks)",
        p->lat_n ? p->lat_us_sum / p->lat_n : 0.0, p->lat_us_max, p->lat_n);

    r++;
    if (r < bottom)
        put(r++, x, w, COLOR_PAIR(C_CYAN) | A_BOLD, "%-5s %10s %10s %9s  %s", "ASSET", "START",
            "NOW", "VALUE", "OPEN POSITION");
    if (r < bottom)
        put(r++, x, w, A_NORMAL, "%-5s %10.2f %10.2f %9.0f", app->feed.quote_ccy, p->start_cash,
            p->cash, p->cash);
    for (int s = 0; s < nsym && r < bottom - 2; s++) {
        const Lot *l = &p->lot[s];
        if (!p->use_sym[s] && p->start_hold[s] == 0 && p->hold[s] == 0) continue;
        double mid = (q[s].bid + q[s].ask) / 2;
        put(r, x, w, A_NORMAL, "%-5s %10.6g %10.6g %9.0f", q[s].base, p->start_hold[s],
            p->hold[s], p->hold[s] * mid);
        if (l->dir) {
            double px = l->dir > 0 ? q[s].bid : q[s].ask;
            double bp = l->dir * (px - l->entry) / l->entry * 1e4;
            put(r, x + 39, w - 39, pnl_attr(bp), "%s %.4g @%.*f %+.1fbp",
                l->dir > 0 ? "LONG" : "SHORT", l->qty, px_prec(l->entry), l->entry, bp);
        } else if (!p->use_sym[s]) {
            put(r, x + 39, w - 39, COLOR_PAIR(C_DIM), "not traded");
        }
        r++;
    }

    if (bottom - 2 > r) {
        put(bottom - 2, x, w, COLOR_PAIR(C_CYAN) | A_BOLD,
            "Equity, last %d s (green = above start)", p->eq_n < w ? p->eq_n : w);
        sparkline(bottom - 1, x, w, p);
    }
}

static void draw_log(Profile *p, int y, int x, int w, int bottom)
{
    put(y, x, w, COLOR_PAIR(C_CYAN) | A_BOLD, "TRADES - %s (newest first)", p->name);
    put(y + 1, x, w, COLOR_PAIR(C_CYAN), "%-12s %-8s %-5s %10s %11s %11s %9s %s", "TIME",
        "SYMBOL", "SIDE", "QTY", "ENTRY", "EXIT", "P&L", "RESULT");
    int r = y + 2;
    for (int i = 0; i < p->log_n && r < bottom; i++, r++) {
        const TradeRec *t = &p->log[(p->log_head - 1 - i + MAX_TRADES_LOG) % MAX_TRADES_LOG];
        char ts[24];
        clock_str(t->wall_ms, ts, sizeof ts, 1);
        const char *side = t->dir > 0 ? "LONG" : "SHORT";
        if (t->result == 'R') {
            put(r, x, w, COLOR_PAIR(C_YELLOW), "%-12s %-8s %-5s  REJECTED: %s", ts, t->sym, side,
                t->reason);
            continue;
        }
        int pr = px_prec(t->entry);
        attr_t a = t->result == 'W' ? COLOR_PAIR(C_GREEN) : COLOR_PAIR(C_RED);
        put(r, x, w, a, "%-12s %-8s %-5s %10.4g %11.*f %11.*f %+9.4f %s %s", ts, t->sym, side,
            t->qty, pr, t->entry, pr, t->exit, t->pnl, t->result == 'W' ? "WIN " : "LOSS",
            t->reason);
    }
    if (p->log_n == 0)
        put(r, x, w, COLOR_PAIR(C_DIM), "No trades yet - waiting for a signal.");
}

void ui_draw(App *app, const Quote *q, int nsym, int sel, const char *msg)
{
    erase();
    if (COLS < 110 || LINES < 28) {
        put(0, 0, 0, A_BOLD, "Terminal too small (%dx%d). Need at least 110x28.", COLS, LINES);
        refresh();
        return;
    }
    draw_header(app, 0);
    int y = 2;
    draw_market(q, nsym, y);
    y += nsym + 2;
    draw_profiles(app, q, nsym, sel, y);
    y += app->nprof + 2;

    int bottom = LINES - 2;
    int lw = COLS * 40 / 100;
    if (lw < 70) lw = 70;
    if (y < bottom) {
        attron(COLOR_PAIR(C_DIM));
        for (int c = 0; c < COLS; c++) mvaddstr(y - 1, c, c == lw ? "┬" : "─");
        for (int r = y; r <= bottom; r++) mvaddstr(r, lw, "│");
        attroff(COLOR_PAIR(C_DIM));
        Profile *p = &app->prof[sel];
        draw_detail(app, q, nsym, p, y, 1, lw - 2, bottom + 1);
        draw_log(p, y, lw + 2, COLS - lw - 3, bottom + 1);
    }

    attron(A_REVERSE);
    mvhline(LINES - 1, 0, ' ', COLS);
    attroff(A_REVERSE);
    if (msg && *msg)
        put(LINES - 1, 1, 0, A_REVERSE | A_BOLD, "%s", msg);
    else
        put(LINES - 1, 1, 0, A_REVERSE,
            "↑/↓ select   space pause/resume   a pause/resume all   f flatten   n new profile   q quit");
    refresh();
}

/* Blocking single-line input on the bottom row. Returns 0 if non-empty. */
int ui_prompt(const char *label, char *buf, int n)
{
    attron(A_REVERSE);
    mvhline(LINES - 1, 0, ' ', COLS);
    mvaddstr(LINES - 1, 1, label);
    attroff(A_REVERSE);
    move(LINES - 1, 2 + (int)strlen(label));
    clrtoeol();
    echo();
    curs_set(1);
    timeout(-1);
    buf[0] = '\0';
    getnstr(buf, n - 1);
    noecho();
    curs_set(0);
    timeout(20);
    return buf[0] ? 0 : -1;
}
