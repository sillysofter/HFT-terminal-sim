/*
 * hft.h - shared types for the HFT paper-trading simulator.
 *
 * The feed thread owns `Feed` (guarded by its mutex). Everything in
 * `Profile` is touched only by the main thread.
 */
#ifndef HFT_H
#define HFT_H

#include <pthread.h>
#include <stdio.h>

#define MAX_SYMBOLS    16
#define MAX_PROFILES   16
#define MAX_TRADES_LOG 256
#define EQ_HIST        512
#define MR_WIN_MAX     512

/* ---------- market data ---------- */

typedef struct {
    char   sym[16];      /* e.g. BTCUSDT */
    char   base[12];     /* e.g. BTC     */
    double bid, ask;     /* best bid / ask */
    double bidq, askq;   /* size at best bid / ask */
    double first_mid;    /* mid at session start */
    int    valid;
} Quote;

typedef struct {
    pthread_mutex_t mu;
    int    nsym;
    Quote  q[MAX_SYMBOLS];
    char   quote_ccy[8];       /* e.g. USDT */
    char   api_base[128];      /* e.g. https://api.binance.com */
    int    poll_ms;

    unsigned long seq;         /* bumped on every successful update */
    unsigned long nerr;
    double last_ok_ms;         /* monotonic ms of last good update */
    double rtt_ms;             /* round-trip of last request */
    char   err[200];           /* last error, "" when healthy */
    volatile int stop;
} Feed;

/* ---------- strategy ---------- */

typedef enum { STRAT_MOMENTUM, STRAT_MEANREV, STRAT_IMBALANCE } StratKind;

typedef struct {
    StratKind kind;
    double order_usd;      /* notional per entry */
    double tp_bps;         /* take profit */
    double sl_bps;         /* stop loss */
    double fee_bps;        /* taker fee per side */
    int    max_hold_ms;    /* force exit after this long */
    int    cooldown_ms;    /* min gap between entries on a symbol */
    /* momentum: EMA crossover of mid price (periods in ticks) */
    double fast, slow, threshold_bps;
    /* mean reversion: z-score of mid vs rolling window */
    int    window;
    double z_entry;
    /* order-book imbalance: bidq / (bidq + askq) */
    double imb_entry;
} StratParams;

typedef struct {
    double ema_f, ema_s;
    long   n;
    double buf[MR_WIN_MAX];
    int    head, count;
    double sum, sumsq;
} SymState;

/* One open round-trip per symbol. dir=+1 long (bought with cash),
 * dir=-1 short against inventory (sold starting holdings, buy back later). */
typedef struct {
    int    dir;
    double qty, entry, fees, realized, open_ms;
    double qty0;             /* quantity at entry */
} Lot;

typedef struct {
    double wall_ms;
    char   sym[16];
    int    dir;
    double qty, entry, exit, pnl;
    char   result;           /* 'W' win, 'L' loss, 'R' rejected order */
    char   reason[32];
} TradeRec;

typedef struct {
    char   name[32];
    char   strat_name[16];
    StratParams sp;
    int    use_sym[MAX_SYMBOLS];

    double start_cash, start_hold[MAX_SYMBOLS];
    double cash, hold[MAX_SYMBOLS];
    double start_equity;
    int    started;

    SymState ss[MAX_SYMBOLS];
    Lot      lot[MAX_SYMBOLS];
    double   next_ok_ms[MAX_SYMBOLS];

    int    wins, losses, rejects;
    double realized, fees_paid, gross_win, gross_loss;
    double peak_eq, max_dd;

    double eq_hist[EQ_HIST];
    int    eq_head, eq_n;
    double last_eq_sample_ms;

    TradeRec log[MAX_TRADES_LOG];
    int    log_head, log_n;

    int    paused;
    double lat_us_sum, lat_us_max;
    unsigned long lat_n;
    FILE  *csv;
} Profile;

typedef struct {
    Feed    feed;
    int     nprof;
    Profile prof[MAX_PROFILES];
    char    conf_path[256];
} App;

/* config.c */
int  config_load(App *app, const char *path, char *err, size_t errn);
int  profile_set(App *app, Profile *p, const char *key, const char *val,
                 char *err, size_t errn);
int  profile_validate(const Profile *p, char *err, size_t errn);
void profile_defaults(Profile *p, int nsym);
int  config_append_profile(const char *path, const Profile *p,
                           const App *app);

/* feed.c */
int  feed_start(Feed *f, pthread_t *th);
void feed_stop(Feed *f, pthread_t th);

/* engine.c */
void   engine_tick(App *app, const Quote *q, int nsym, double mono, double wall);
void   engine_flatten(App *app, Profile *p, const Quote *q, int nsym,
                      double mono, double wall);
double profile_equity(const Profile *p, const Quote *q, int nsym);
double profile_unrealized(const Profile *p, const Quote *q, int nsym);
int    profile_open_csv(Profile *p);

/* ui.c */
void ui_init(void);
void ui_end(void);
void ui_draw(App *app, const Quote *q, int nsym, int sel, const char *msg);
int  ui_prompt(const char *label, char *buf, int n);

/* util */
double mono_ms(void);
double wall_ms(void);

#endif
