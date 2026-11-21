// The loading window's state (loadwin.h): the stages, their counted work and its rate, the log text for the Details
// view and the length of the last start. The window itself is loadwin_win.c; without it (other OSes) the panel never
// opens and every call returns at once.
#include "loadwin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

volatile int lw_taking_log;

// The panel has a window system on Windows only (loadwin_win.c). Elsewhere lw_start returns before it buffers or logs
// anything, so a start there is the same as without this file.
#ifdef _WIN32
#define LW_HAS_PANEL 1
#else
#define LW_HAS_PANEL 0
#endif

#define RATE_SAMPLES 48
#define LOG_TEXT_CAP (16u << 20)   // text waiting for the Details view; a trace-heavy start drops the rest

typedef struct {
    int state;                      // 0 not begun, 1 running, 2 ended
    double begun;                   // seconds since process start
    uint64_t total, done;           // counted work; done is the last value read
    uint64_t (*read)(void *ctx);    // NULL once the count ended
    void *ctx;
    double t[RATE_SAMPLES];         // (time, done) samples, oldest at head
    uint64_t d[RATE_SAMPLES];
    int head, n;
    double left, last;              // the smoothed time left (negative: unknown), the time it was last updated
} Stage;

static struct {
    PlatMutex lock;                 // all below
    volatile int open;              // the panel's thread runs
    int player;                     // a player's run (no --seconds): lw_fatal keeps the panel open
    int failed;
    char why[512];
    Stage st[LW_STAGES];
    char *text;                     // log text not yet taken by the panel
    size_t text_n, text_cap;
    int text_cut;
    double expect_s;                // the last start's length to the game window, 0 when unknown
    double shown_s;                 // this start's, set when the game window opened
    double overall;                 // the last share of the start shown (lw_view)
    char time_path[PLAT_PATH_MAX * 3];
} L = {.lock = PLAT_MUTEX_INIT};

static double now_s(void) { return (double)plat_process_uptime_ns() / 1e9; }

// The start's length lives in <profile>/loading-time.txt: "start_ms <n>". Missing or unreadable: no estimate.
static void expect_read(void) {
    const char *prof = rt_config()->profile;
    if (!prof || snprintf(L.time_path, sizeof L.time_path, "%s/loading-time.txt", prof) >= (int)sizeof L.time_path) {
        L.time_path[0] = 0;
        return;
    }
    uint64_t size = 0;
    uint8_t *p = plat_file_read(L.time_path, &size);
    if (!p) return;
    char buf[64];
    size_t n = size < sizeof buf - 1 ? (size_t)size : sizeof buf - 1;
    memcpy(buf, p, n);
    buf[n] = 0;
    free(p);
    unsigned long long ms = 0;
    if (sscanf(buf, "start_ms %llu", &ms) == 1 && ms > 0 && ms < 3600000ull) L.expect_s = (double)ms / 1000.0;
}

static void expect_write(double s) {
    if (!L.time_path[0] || s <= 0) return;
    char buf[48];
    int n = snprintf(buf, sizeof buf, "start_ms %llu\n", (unsigned long long)(s * 1000.0));
    if (!plat_file_write_atomic(L.time_path, buf, (size_t)n)) rt_log("loadwin: cannot write %s", L.time_path);
}

static void text_add(const char *p, size_t n) {
    if (L.text_n + n > L.text_cap) {
        size_t cap = L.text_cap ? L.text_cap : 65536;
        while (cap < L.text_n + n) cap *= 2;
        if (cap > LOG_TEXT_CAP) cap = LOG_TEXT_CAP;
        char *t = L.text_n + n <= cap ? realloc(L.text, cap) : NULL;
        if (!t) {
            L.text_cut = 1;
            return;
        }
        L.text = t, L.text_cap = cap;
    }
    memcpy(L.text + L.text_n, p, n);
    L.text_n += n;
}

void lw_start(const char *early, size_t early_n) {
    const char *gpu = rt_config()->gpu;
    if (!LW_HAS_PANEL || cfg_bool("TOTK_CONSOLE") || (gpu && !strcmp(gpu, "null"))) return;
    L.player = rt_config()->seconds <= 0;
    expect_read();
    plat_mutex_lock(&L.lock);
    text_add(early, early_n);
    plat_mutex_unlock(&L.lock);
    lw_taking_log = 1;
    const char *bg = cfg_get("TOTK_BACKGROUND_WINDOW");
    L.st[LW_START] = (Stage){.state = 1, .begun = now_s(), .left = -1};
    if (!lw_wsys_open(bg && atoi(bg))) {
        lw_taking_log = 0;
        plat_mutex_lock(&L.lock);
        free(L.text);
        L.text = NULL, L.text_n = L.text_cap = 0;
        plat_mutex_unlock(&L.lock);
        rt_log("loadwin: the loading window did not open; the console stays");
        return;
    }
    L.open = 1;
    if (lw_wsys_detach_console())
        rt_log("loadwin: the console window is closed; the log is in the loading window's Details and in the log file "
               "(console = 1 keeps the console)");
}

void lw_begin(LwStage s) {
    if (!L.open) return;
    plat_mutex_lock(&L.lock);
    if (L.st[s].state != 1) L.st[s] = (Stage){.state = 1, .begun = now_s(), .left = -1};
    plat_mutex_unlock(&L.lock);
}

void lw_end(LwStage s) {
    if (!L.open) return;
    plat_mutex_lock(&L.lock);
    L.st[s].state = 2;
    L.st[s].read = NULL;
    plat_mutex_unlock(&L.lock);
}

void lw_count(LwStage s, uint64_t total, uint64_t (*done)(void *ctx), void *ctx) {
    if (!L.open) return;
    plat_mutex_lock(&L.lock);
    Stage *t = &L.st[s];
    t->total = total, t->done = 0, t->read = done, t->ctx = ctx;
    t->head = t->n = 0, t->left = -1, t->last = 0;
    plat_mutex_unlock(&L.lock);
}

void lw_count_end(LwStage s) {
    if (!L.open) return;
    plat_mutex_lock(&L.lock);
    if (L.st[s].read) L.st[s].done = L.st[s].read(L.st[s].ctx);
    L.st[s].read = NULL;
    plat_mutex_unlock(&L.lock);
}

void lw_log(const char *p, size_t n) {
    // A thread that crashes inside text_add logs its crash report here again: that report goes to the file only.
    static _Thread_local int inside;
    if (!lw_taking_log || inside) return;
    inside = 1;
    plat_mutex_lock(&L.lock);
    if (lw_taking_log) text_add(p, n);
    plat_mutex_unlock(&L.lock);
    inside = 0;
}

char *lw_log_take(size_t *n) {
    plat_mutex_lock(&L.lock);
    char *t = L.text;
    *n = L.text_n;
    if (L.text_cut && t) {   // the panel shows where text went missing
        static const char cut[] = "\n[loading window: log lines dropped here; logs\\totk.log has them all]\n";
        char *u = realloc(t, *n + sizeof cut);
        if (u) memcpy(u + *n, cut, sizeof cut - 1), *n += sizeof cut - 1, t = u;
    }
    L.text = NULL, L.text_n = L.text_cap = 0, L.text_cut = 0;
    plat_mutex_unlock(&L.lock);
    return t;
}

// Time left of a counted stage: the rate over the last 12 s of samples, once 2 s of them and 3% of the work are in,
// eased toward each new value over about 3 s so that it does not jump with every burst of cached items.
static void stage_sample(Stage *t, double now) {
    if (t->n && now - t->t[(t->head + t->n - 1) % RATE_SAMPLES] < 0.25) return;
    if (t->n == RATE_SAMPLES) t->head = (t->head + 1) % RATE_SAMPLES, t->n--;
    int at = (t->head + t->n) % RATE_SAMPLES;
    t->t[at] = now, t->d[at] = t->done, t->n++;
    while (t->n > 2 && now - t->t[(t->head + 1) % RATE_SAMPLES] >= 12.0) t->head = (t->head + 1) % RATE_SAMPLES, t->n--;
    double raw = -1;
    double span = now - t->t[t->head];
    uint64_t moved = t->done - t->d[t->head];
    if (t->done >= t->total) raw = 0;
    else if (span >= 2.0 && moved && (double)t->done >= 0.03 * (double)t->total)
        raw = (double)(t->total - t->done) * span / (double)moved;
    double dt = t->last > 0 ? now - t->last : 0;
    t->last = now;
    if (raw < 0) t->left = -1;
    else if (t->left < 0 || raw == 0) t->left = raw;
    else {
        t->left = t->left > dt ? t->left - dt : 0;
        t->left += (raw - t->left) * (dt < 3.0 ? dt / 3.0 : 1.0);
    }
}

void lw_view(LwView *v) {
    double now = now_s();
    *v = (LwView){.stage = -1, .task_left_s = -1, .other = -1, .game_left_s = -1, .elapsed_s = now};
    plat_mutex_lock(&L.lock);
    v->failed = L.failed;
    memcpy(v->why, L.why, sizeof v->why);
    for (int s = 0; s < LW_STAGES; s++) {
        Stage *t = &L.st[s];
        if (t->state == 1 && t->read) {
            t->done = t->read(t->ctx);
            if (t->done > t->total) t->done = t->total;
            stage_sample(t, now);
        }
    }
    // The panel names the furthest stage running; another running stage with a count goes on the line below it.
    for (int s = LW_STAGES - 1; s >= 0; s--)
        if (L.st[s].state == 1) {
            if (v->stage < 0) v->stage = s;
            else if (v->other < 0 && L.st[s].total) v->other = s;
        }
    if (v->stage < 0) v->stage = LW_GAME;   // every stage ended: the game window is about to open
    const Stage *t = &L.st[v->stage];
    if (t->total) {
        v->done = t->done, v->total = t->total, v->task_left_s = t->left;
    }
    if (v->other >= 0) v->other_done = L.st[v->other].done, v->other_total = L.st[v->other].total;
    if (L.expect_s > 0) {
        v->game_left_s = L.expect_s - now;
        if (v->game_left_s < 0) v->game_left_s = 0;
        // The guest waits for the prewarm: a start slower than the last one shows the prewarm's own time left
        if ((v->stage == LW_SHADERS || v->stage == LW_PIPELINES) && v->task_left_s > v->game_left_s)
            v->game_left_s = v->task_left_s;
    }
    // The share of the whole start: with the last start's length, the time so far against the time so far plus the
    // time left. Without it (the first start), fixed shares of the stages: an ended stage its whole share, a counted
    // one by its count, another one nearing its share over a few seconds. Never back, and below 1 until the game window.
    static const double share[LW_STAGES] = {0.05, 0.05, 0.15, 0.35, 0.25, 0.15};
    double p = 0;
    if (v->game_left_s >= 0) {
        p = now / (now + v->game_left_s);
    } else {
        for (int s = 0; s < LW_STAGES; s++) {
            const Stage *u = &L.st[s];
            if (u->state == 2) p += share[s];
            else if (u->state == 1 && u->total) p += share[s] * (double)u->done / (double)u->total;
            else if (u->state == 1) p += share[s] * (1 - exp(-(now - u->begun) / 4.0));
        }
    }
    if (p > 0.99) p = 0.99;
    if (p < L.overall) p = L.overall;
    v->overall = L.overall = p;
    plat_mutex_unlock(&L.lock);
}

void lw_game_window(void) {
    if (!L.open) return;
    plat_mutex_lock(&L.lock);
    L.shown_s = now_s();
    for (int s = 0; s < LW_STAGES; s++) L.st[s].state = L.st[s].state ? 2 : 0, L.st[s].read = NULL;
    double shown = L.shown_s, expect = L.expect_s;
    plat_mutex_unlock(&L.lock);
    if (expect > 0) rt_log("loadwin: the game window opened %.0f ms after the process start (the last start: %.0f ms)",
                           shown * 1000, expect * 1000);
    else rt_log("loadwin: the game window opened %.0f ms after the process start", shown * 1000);
    lw_wsys_close();
}

void lw_closed(int by_player) {
    lw_taking_log = 0;
    plat_mutex_lock(&L.lock);
    L.open = 0;
    free(L.text);
    L.text = NULL, L.text_n = L.text_cap = 0;
    int failed = L.failed;
    double shown = L.shown_s;
    plat_mutex_unlock(&L.lock);
    if (failed) return;   // lw_fatal's caller ends the process
    if (by_player) {
        rt_log("loadwin: the player closed the loading window; the game exits");
        plat_exit(0);
    }
    expect_write(shown);
}

void lw_fatal(const char *why) {
    if (!L.open || !L.player || lw_wsys_on_panel_thread()) return;
    plat_mutex_lock(&L.lock);
    L.failed = 1;
    snprintf(L.why, sizeof L.why, "%s", why ? why : "");
    plat_mutex_unlock(&L.lock);
    lw_wsys_fatal();
    lw_wsys_wait_closed();
}

#ifndef _WIN32
int lw_wsys_open(int background) { (void)background; return 0; }
void lw_wsys_close(void) {}
void lw_wsys_fatal(void) {}
void lw_wsys_wait_closed(void) {}
int lw_wsys_detach_console(void) { return 0; }
int lw_wsys_on_panel_thread(void) { return 0; }
#endif
