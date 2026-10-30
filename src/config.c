/*
 * config.c - INI-style profile/market configuration.
 *
 *   [market]
 *   symbols = BTCUSDT, ETHUSDT
 *   [profile Alice]
 *   strategy = imbalance
 *   cash = 10000
 *   hold.BTC = 0.05
 */
#include "hft.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int parse_num(const char *v, double *out)
{
    char *end;
    errno = 0;
    double d = strtod(v, &end);
    if (errno || end == v || *trim(end) != '\0') return -1;
    *out = d;
    return 0;
}

static int sym_index(const Feed *f, const char *sym)
{
    for (int i = 0; i < f->nsym; i++)
        if (strcasecmp(f->q[i].sym, sym) == 0) return i;
    return -1;
}

static int base_index(const Feed *f, const char *base)
{
    for (int i = 0; i < f->nsym; i++)
        if (strcasecmp(f->q[i].base, base) == 0) return i;
    return -1;
}

static int market_set(Feed *f, const char *key, const char *val,
                      char *err, size_t errn)
{
    double d;
    if (!strcmp(key, "symbols")) {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s", val);
        f->nsym = 0;
        for (char *tok = strtok(tmp, ", \t"); tok; tok = strtok(NULL, ", \t")) {
            if (f->nsym >= MAX_SYMBOLS) {
                snprintf(err, errn, "too many symbols (max %d)", MAX_SYMBOLS);
                return -1;
            }
            Quote *q = &f->q[f->nsym++];
            memset(q, 0, sizeof *q);
            snprintf(q->sym, sizeof q->sym, "%s", tok);
            for (char *c = q->sym; *c; c++) *c = (char)toupper((unsigned char)*c);
        }
    } else if (!strcmp(key, "quote")) {
        snprintf(f->quote_ccy, sizeof f->quote_ccy, "%s", val);
    } else if (!strcmp(key, "api_base")) {
        snprintf(f->api_base, sizeof f->api_base, "%s", val);
    } else if (!strcmp(key, "poll_ms")) {
        if (parse_num(val, &d) || d < 50) {
            snprintf(err, errn, "poll_ms must be >= 50");
            return -1;
        }
        f->poll_ms = (int)d;
    } else {
        snprintf(err, errn, "unknown market key '%s'", key);
        return -1;
    }
    return 0;
}

/* Derive base asset names (BTCUSDT -> BTC) once symbols and quote are known. */
static int market_finish(Feed *f, char *err, size_t errn)
{
    if (f->nsym == 0) {
        snprintf(err, errn, "[market] needs at least one symbol");
        return -1;
    }
    size_t ql = strlen(f->quote_ccy);
    for (int i = 0; i < f->nsym; i++) {
        Quote *q = &f->q[i];
        size_t sl = strlen(q->sym);
        if (sl <= ql || strcasecmp(q->sym + sl - ql, f->quote_ccy) != 0) {
            snprintf(err, errn, "symbol %s is not quoted in %s", q->sym, f->quote_ccy);
            return -1;
        }
        snprintf(q->base, sizeof q->base, "%.*s", (int)(sl - ql), q->sym);
    }
    return 0;
}

void profile_defaults(Profile *p, int nsym)
{
    memset(p, 0, sizeof *p);
    snprintf(p->strat_name, sizeof p->strat_name, "imbalance");
    p->sp.kind = STRAT_IMBALANCE;
    p->sp.order_usd = 500;
    p->sp.tp_bps = 8;
    p->sp.sl_bps = 12;
    p->sp.fee_bps = 10;
    p->sp.max_hold_ms = 60000;
    p->sp.cooldown_ms = 2000;
    p->sp.fast = 8;
    p->sp.slow = 40;
    p->sp.threshold_bps = 1.0;
    p->sp.window = 120;
    p->sp.z_entry = 2.0;
    p->sp.imb_entry = 0.8;
    p->start_cash = 10000;
    for (int i = 0; i < nsym; i++) p->use_sym[i] = 1;
}

int profile_set(App *app, Profile *p, const char *key, const char *val,
                char *err, size_t errn)
{
    Feed *f = &app->feed;
    StratParams *sp = &p->sp;
    double d;

    if (!strncasecmp(key, "hold.", 5)) {
        int i = base_index(f, key + 5);
        if (i < 0) {
            snprintf(err, errn, "%s: asset %s is not in [market] symbols", p->name, key + 5);
            return -1;
        }
        if (parse_num(val, &d) || d < 0) {
            snprintf(err, errn, "%s: bad holding '%s'", p->name, val);
            return -1;
        }
        p->start_hold[i] = d;
        return 0;
    }
    if (!strcmp(key, "strategy")) {
        if (!strcasecmp(val, "momentum")) sp->kind = STRAT_MOMENTUM;
        else if (!strcasecmp(val, "meanrev")) sp->kind = STRAT_MEANREV;
        else if (!strcasecmp(val, "imbalance")) sp->kind = STRAT_IMBALANCE;
        else {
            snprintf(err, errn, "%s: strategy must be momentum|meanrev|imbalance", p->name);
            return -1;
        }
        snprintf(p->strat_name, sizeof p->strat_name, "%s", val);
        for (char *c = p->strat_name; *c; c++) *c = (char)tolower((unsigned char)*c);
        return 0;
    }
    if (!strcmp(key, "symbols")) {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s", val);
        memset(p->use_sym, 0, sizeof p->use_sym);
        for (char *tok = strtok(tmp, ", \t"); tok; tok = strtok(NULL, ", \t")) {
            int i = sym_index(f, tok);
            if (i < 0) {
                snprintf(err, errn, "%s: symbol %s is not in [market]", p->name, tok);
                return -1;
            }
            p->use_sym[i] = 1;
        }
        return 0;
    }

    if (parse_num(val, &d)) {
        snprintf(err, errn, "%s: '%s' needs a number, got '%s'", p->name, key, val);
        return -1;
    }
    if      (!strcmp(key, "cash"))          p->start_cash = d;
    else if (!strcmp(key, "order_usd"))     sp->order_usd = d;
    else if (!strcmp(key, "tp_bps"))        sp->tp_bps = d;
    else if (!strcmp(key, "sl_bps"))        sp->sl_bps = d;
    else if (!strcmp(key, "fee_bps"))       sp->fee_bps = d;
    else if (!strcmp(key, "max_hold_ms"))   sp->max_hold_ms = (int)d;
    else if (!strcmp(key, "cooldown_ms"))   sp->cooldown_ms = (int)d;
    else if (!strcmp(key, "fast"))          sp->fast = d;
    else if (!strcmp(key, "slow"))          sp->slow = d;
    else if (!strcmp(key, "threshold_bps")) sp->threshold_bps = d;
    else if (!strcmp(key, "window"))        sp->window = (int)d;
    else if (!strcmp(key, "z_entry"))       sp->z_entry = d;
    else if (!strcmp(key, "imb_entry"))     sp->imb_entry = d;
    else {
        snprintf(err, errn, "%s: unknown key '%s'", p->name, key);
        return -1;
    }
    return 0;
}

int profile_validate(const Profile *p, char *err, size_t errn)
{
    const StratParams *sp = &p->sp;
    if (!p->name[0]) { snprintf(err, errn, "profile needs a name"); return -1; }
    if (p->start_cash < 0 || sp->order_usd <= 0 || sp->fast < 1 || sp->slow <= sp->fast ||
        sp->window < 10 || sp->window > MR_WIN_MAX || sp->imb_entry <= 0.5 ||
        sp->imb_entry >= 1.0 || sp->fee_bps < 0 || sp->tp_bps <= 0 || sp->sl_bps <= 0) {
        snprintf(err, errn,
                 "%s: invalid parameters (need cash>=0, order_usd>0, tp/sl_bps>0, "
                 "slow>fast>=1, 10<=window<=%d, 0.5<imb_entry<1)", p->name, MR_WIN_MAX);
        return -1;
    }
    return 0;
}

int config_load(App *app, const char *path, char *err, size_t errn)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        snprintf(err, errn, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    Feed *f = &app->feed;
    snprintf(f->quote_ccy, sizeof f->quote_ccy, "USDT");
    snprintf(f->api_base, sizeof f->api_base, "https://api.binance.com");
    f->poll_ms = 250;
    snprintf(app->conf_path, sizeof app->conf_path, "%s", path);

    enum { NONE, MARKET, PROFILE } sec = NONE;
    int market_done = 0, lineno = 0, rc = 0;
    Profile *cur = NULL;
    char line[1024];

    while (fgets(line, sizeof line, fp)) {
        lineno++;
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;

        if (*s == '[') {
            char *close = strchr(s, ']');
            if (!close) { snprintf(err, errn, "line %d: missing ']'", lineno); rc = -1; break; }
            *close = '\0';
            char *hdr = trim(s + 1);
            if (!strcasecmp(hdr, "market")) {
                sec = MARKET;
            } else if (!strncasecmp(hdr, "profile", 7) && isspace((unsigned char)hdr[7])) {
                if (!market_done) {
                    if (market_finish(f, err, errn)) { rc = -1; break; }
                    market_done = 1;
                }
                if (app->nprof >= MAX_PROFILES) {
                    snprintf(err, errn, "too many profiles (max %d)", MAX_PROFILES);
                    rc = -1; break;
                }
                if (cur && profile_validate(cur, err, errn)) { rc = -1; break; }
                cur = &app->prof[app->nprof++];
                profile_defaults(cur, f->nsym);
                snprintf(cur->name, sizeof cur->name, "%s", trim(hdr + 8));
                sec = PROFILE;
            } else {
                snprintf(err, errn, "line %d: unknown section [%s]", lineno, hdr);
                rc = -1; break;
            }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) { snprintf(err, errn, "line %d: expected key = value", lineno); rc = -1; break; }
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);
        char sub[200] = "";

        if (sec == MARKET) {
            if (market_done) {
                snprintf(err, errn, "line %d: [market] must come before profiles", lineno);
                rc = -1; break;
            }
            rc = market_set(f, key, val, sub, sizeof sub);
        } else if (sec == PROFILE) {
            rc = profile_set(app, cur, key, val, sub, sizeof sub);
        } else {
            snprintf(sub, sizeof sub, "key outside of a section");
            rc = -1;
        }
        if (rc) { snprintf(err, errn, "line %d: %s", lineno, sub); break; }
    }
    fclose(fp);
    if (rc) return rc;
    if (cur && profile_validate(cur, err, errn)) return -1;
    if (!market_done && market_finish(f, err, errn)) return -1;
    if (app->nprof == 0) {
        snprintf(err, errn, "%s defines no [profile ...] sections", path);
        return -1;
    }
    return 0;
}

int config_append_profile(const char *path, const Profile *p, const App *app)
{
    FILE *fp = fopen(path, "a");
    if (!fp) return -1;
    fprintf(fp, "\n[profile %s]\nstrategy = %s\ncash = %.2f\n",
            p->name, p->strat_name, p->start_cash);
    for (int i = 0; i < app->feed.nsym; i++)
        if (p->start_hold[i] > 0)
            fprintf(fp, "hold.%s = %.8g\n", app->feed.q[i].base, p->start_hold[i]);
    return fclose(fp);
}
