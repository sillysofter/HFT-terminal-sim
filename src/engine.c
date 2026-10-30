/*
 * engine.c - strategies, simulated execution and P&L accounting.
 *
 * Fills are taken against the live top of book: buys pay the ask, sells
 * hit the bid, size is capped at the quantity shown at that level, and a
 * per-side fee is charged in the quote currency. Each entry/exit pair is
 * one round trip; it counts as a win if its net P&L (after fees) is > 0.
 */
#include "hft.h"

#include <math.h>
#include <string.h>
#include <sys/stat.h>

#define DUST 1e-12

double profile_equity(const Profile *p, const Quote *q, int nsym)
{
    double eq = p->cash;
    for (int i = 0; i < nsym; i++)
        if (q[i].valid) eq += p->hold[i] * (q[i].bid + q[i].ask) / 2;
    return eq;
}

/* Mark open lots to the price we'd actually get on exit, less the exit fee. */
double profile_unrealized(const Profile *p, const Quote *q, int nsym)
{
    double u = 0;
    for (int i = 0; i < nsym; i++) {
        const Lot *l = &p->lot[i];
        if (!l->dir || !q[i].valid) continue;
        double px = l->dir > 0 ? q[i].bid : q[i].ask;
        u += l->dir * l->qty * (px - l->entry) - l->fees
             - l->qty * px * p->sp.fee_bps / 1e4;
    }
    return u;
}

int profile_open_csv(Profile *p)
{
    char path[96];
    mkdir("logs", 0755);
    snprintf(path, sizeof path, "logs/%s.csv", p->name);
    struct stat st;
    int fresh = stat(path, &st) != 0 || st.st_size == 0;
    p->csv = fopen(path, "a");
    if (!p->csv) return -1;
    if (fresh)
        fprintf(p->csv, "time_ms,symbol,side,qty,entry,exit,pnl,result,reason\n");
    return 0;
}

static void record(Profile *p, double wall, const char *sym, int dir, double qty,
                   double entry, double exit, double pnl, char result, const char *reason)
{
    TradeRec *t = &p->log[p->log_head];
    p->log_head = (p->log_head + 1) % MAX_TRADES_LOG;
    if (p->log_n < MAX_TRADES_LOG) p->log_n++;
    memset(t, 0, sizeof *t);
    t->wall_ms = wall;
    snprintf(t->sym, sizeof t->sym, "%s", sym);
    t->dir = dir;
    t->qty = qty;
    t->entry = entry;
    t->exit = exit;
    t->pnl = pnl;
    t->result = result;
    snprintf(t->reason, sizeof t->reason, "%s", reason);
    if (p->csv) {
        fprintf(p->csv, "%.0f,%s,%s,%.8g,%.8g,%.8g,%.6f,%c,%s\n", wall, sym,
                dir > 0 ? "LONG" : "SHORT", qty, entry, exit, pnl, result, reason);
        fflush(p->csv);
    }
}

static void reject(Profile *p, int s, const Quote *q, int dir, double mono, double wall,
                   const char *why)
{
    p->rejects++;
    record(p, wall, q->sym, dir, 0, 0, 0, 0, 'R', why);
    /* back off so a persistent condition doesn't flood the log */
    double back = p->sp.cooldown_ms > 15000 ? p->sp.cooldown_ms : 15000;
    p->next_ok_ms[s] = mono + back;
}

/* ---------- signals ---------- */

static int signal_for(Profile *p, int s, const Quote *q)
{
    StratParams *sp = &p->sp;
    SymState *st = &p->ss[s];
    double mid = (q->bid + q->ask) / 2;
    st->n++;

    switch (sp->kind) {
    case STRAT_MOMENTUM: {
        double af = 2.0 / (sp->fast + 1), as = 2.0 / (sp->slow + 1);
        if (st->n == 1) st->ema_f = st->ema_s = mid;
        st->ema_f += af * (mid - st->ema_f);
        st->ema_s += as * (mid - st->ema_s);
        if (st->n < (long)sp->slow) return 0;          /* warm-up */
        double d = (st->ema_f - st->ema_s) / st->ema_s * 1e4;
        return d > sp->threshold_bps ? 1 : d < -sp->threshold_bps ? -1 : 0;
    }
    case STRAT_MEANREV: {
        int w = sp->window;
        if (st->count == w) {
            double old = st->buf[st->head];
            st->sum -= old;
            st->sumsq -= old * old;
        } else {
            st->count++;
        }
        st->buf[st->head] = mid;
        st->head = (st->head + 1) % w;
        st->sum += mid;
        st->sumsq += mid * mid;
        if (st->count < w) return 0;                    /* warm-up */
        double mean = st->sum / w;
        double var = st->sumsq / w - mean * mean;
        if (var <= 0) return 0;
        double z = (mid - mean) / sqrt(var);
        return z < -sp->z_entry ? 1 : z > sp->z_entry ? -1 : 0;
    }
    case STRAT_IMBALANCE: {
        double tot = q->bidq + q->askq;
        if (tot <= 0) return 0;
        double imb = q->bidq / tot;
        return imb > sp->imb_entry ? 1 : imb < 1 - sp->imb_entry ? -1 : 0;
    }
    }
    return 0;
}

/* ---------- execution ---------- */

static void open_lot(Profile *p, int s, const Quote *q, int dir, double mono, double wall)
{
    const StratParams *sp = &p->sp;
    double fee_r = sp->fee_bps / 1e4;
    double px = dir > 0 ? q->ask : q->bid;
    double qty = sp->order_usd / px;
    double book = dir > 0 ? q->askq : q->bidq;
    if (qty > book) qty = book;

    if (dir > 0) {
        double afford = p->cash / (px * (1 + fee_r));
        if (qty > afford) qty = afford;
        if (qty * px < sp->order_usd * 0.1) {
            reject(p, s, q, dir, mono, wall, "insufficient cash");
            return;
        }
        double fee = qty * px * fee_r;
        p->cash -= qty * px + fee;
        p->hold[s] += qty;
        p->lot[s] = (Lot){dir, qty, px, fee, 0, mono, qty};
        p->fees_paid += fee;
    } else {
        if (qty > p->hold[s]) qty = p->hold[s];
        if (qty * px < sp->order_usd * 0.1) {
            reject(p, s, q, dir, mono, wall, "no inventory to sell");
            return;
        }
        double fee = qty * px * fee_r;
        p->cash += qty * px - fee;
        p->hold[s] -= qty;
        p->lot[s] = (Lot){dir, qty, px, fee, 0, mono, qty};
        p->fees_paid += fee;
    }
    p->next_ok_ms[s] = mono + sp->cooldown_ms;
}

/* Close (part of) a lot. The round trip is scored once fully closed. */
static void close_lot(Profile *p, int s, const Quote *q, double wall, const char *why)
{
    Lot *l = &p->lot[s];
    double fee_r = p->sp.fee_bps / 1e4;
    double px = l->dir > 0 ? q->bid : q->ask;
    double qty = l->qty;
    double book = l->dir > 0 ? q->bidq : q->askq;
    if (qty > book) qty = book;

    if (l->dir < 0) {
        /* buying back inventory needs cash */
        double afford = p->cash / (px * (1 + fee_r));
        if (qty > afford) qty = afford;
    }
    if (qty <= DUST) return;

    double fee = qty * px * fee_r;
    double frac = qty / l->qty;
    double entry_fee = l->fees * frac;
    if (l->dir > 0) {
        p->cash += qty * px - fee;
        p->hold[s] -= qty;
    } else {
        p->cash -= qty * px + fee;
        p->hold[s] += qty;
    }
    if (p->hold[s] < DUST) p->hold[s] = 0;
    p->fees_paid += fee;
    l->realized += l->dir * qty * (px - l->entry) - fee - entry_fee;
    l->fees -= entry_fee;
    l->qty -= qty;

    if (l->qty > l->qty0 * 1e-9) return;                /* partial fill, keep going */

    double pnl = l->realized;
    p->realized += pnl;
    if (pnl > 0) { p->wins++;   p->gross_win += pnl; }
    else         { p->losses++; p->gross_loss -= pnl; }
    record(p, wall, q->sym, l->dir, l->qty0, l->entry, px, pnl, pnl > 0 ? 'W' : 'L', why);
    memset(l, 0, sizeof *l);
}

static void start_profile(Profile *p, const Quote *q, int nsym, double mono)
{
    p->cash = p->start_cash;
    memcpy(p->hold, p->start_hold, sizeof p->hold);
    p->start_equity = profile_equity(p, q, nsym);
    p->peak_eq = p->start_equity;
    p->last_eq_sample_ms = mono - 1e9;
    p->started = 1;
}

void engine_tick(App *app, const Quote *q, int nsym, double mono, double wall)
{
    int all_valid = 1;
    for (int i = 0; i < nsym; i++) all_valid &= q[i].valid;

    for (int k = 0; k < app->nprof; k++) {
        Profile *p = &app->prof[k];
        if (!p->started) {
            if (!all_valid) continue;    /* need marks for every held asset */
            start_profile(p, q, nsym, mono);
        }
        double t0 = mono_ms();

        for (int s = 0; s < nsym; s++) {
            if (!p->use_sym[s] || !q[s].valid) continue;
            int sig = signal_for(p, s, &q[s]);
            Lot *l = &p->lot[s];

            if (l->dir) {
                double px = l->dir > 0 ? q[s].bid : q[s].ask;
                double move = l->dir * (px - l->entry) / l->entry * 1e4;
                const char *why = NULL;
                if (move >= p->sp.tp_bps)                        why = "take-profit";
                else if (move <= -p->sp.sl_bps)                  why = "stop-loss";
                else if (sig == -l->dir)                         why = "reversal";
                else if (mono - l->open_ms > p->sp.max_hold_ms)  why = "timeout";
                if (why) close_lot(p, s, &q[s], wall, why);
            } else if (sig && !p->paused && mono >= p->next_ok_ms[s]) {
                open_lot(p, s, &q[s], sig, mono, wall);
            }
        }

        double us = (mono_ms() - t0) * 1000;
        p->lat_us_sum += us;
        p->lat_n++;
        if (us > p->lat_us_max) p->lat_us_max = us;

        double eq = profile_equity(p, q, nsym);
        if (eq > p->peak_eq) p->peak_eq = eq;
        double dd = p->peak_eq - eq;
        if (dd > p->max_dd) p->max_dd = dd;
        if (mono - p->last_eq_sample_ms >= 1000) {
            p->eq_hist[p->eq_head] = eq;
            p->eq_head = (p->eq_head + 1) % EQ_HIST;
            if (p->eq_n < EQ_HIST) p->eq_n++;
            p->last_eq_sample_ms = mono;
        }
    }
}

void engine_flatten(App *app, Profile *p, const Quote *q, int nsym, double mono, double wall)
{
    (void)app;
    (void)mono;
    for (int s = 0; s < nsym; s++)
        if (p->lot[s].dir && q[s].valid) close_lot(p, s, &q[s], wall, "manual");
}
