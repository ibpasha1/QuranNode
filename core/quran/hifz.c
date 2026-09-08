#include "hifz.h"
#include "qday.h"
#include "quran_db.h"
#include "hal.h"
#include "plat.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "HIFZ";

#define HIFZ_MAGIC   0x315A4851u   // "QHZ1"
#define HIFZ_VERSION 1

// Hifz state changes on discrete user grades, not continuously like khatm's
// dwell accrual — so this is a burst debounce, not a throttle. Every meaningful
// transition also force-flushes.
#define SAVE_DEBOUNCE_MS 5000

// Sabqi walks these; a new portion is therefore seen on 6 of its first 8 days
// and crosses the classical 7-day ceiling before it can ever graduate.
static const uint8_t SABQI_IV[]  = { 1, 1, 2, 3, 4, 5 };
static const uint8_t MANZIL_IV[] = { 7, 7, 10, 14 };   // advisory; the cycle rules
#define SABQI_MAXBOX   ((int)(sizeof SABQI_IV / sizeof SABQI_IV[0]) - 1)
#define MANZIL_MAXBOX  ((int)(sizeof MANZIL_IV / sizeof MANZIL_IV[0]) - 1)

#define GRADUATE_STREAK    3
#define GRADUATE_AGE_DAYS 14
#define SABAQ_SETTLE       2    // same-day sittings before sabaq becomes sabqi

typedef struct { uint16_t day, portions, ayat_new, mins; } HifzDay;

// -------------------------------------------------------------------------
// Persisted blob. Same three rules as khatm.c, and they are load-bearing:
//   1. fields are only ever APPENDED into reserved[]
//   2. all-bits-zero must be a valid default (hence HifzPlanCfg.set)
//   3. the loader memsets first and accepts a SHORT read, so a blob written by
//      an older build upgrades with a zeroed tail instead of being rejected
// Written to alternating slots because hal_state_save opens "wb" and truncates.
//
// Sized deliberately under 16384: hal_state_save chunks SD writes at 16 KB, so
// this stays a single burst and never hits the long-burst failure mode.
// -------------------------------------------------------------------------
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    uint32_t seq;
    uint32_t crc;

    HifzScope   scope;
    HifzPlanCfg cfg;
    uint16_t    frontier_g;      // next global ayah to learn; 0 = derive from scope
    uint16_t    pad0;

    // bits 0-2 strength 0..7, bit 3 in-scope, bits 4-6 consecutive-good, bit 7 lapsed
    uint8_t     strength[HIFZ_TOTAL_AYAT];
    HifzPortion portions[HIFZ_MAX_PORTIONS];
    uint16_t    n_portions;
    uint16_t    pad1;

    HifzDay  days[HIFZ_DAYS];
    uint32_t session_ord;        // monotonic pseudo-day for the no-clock path
    uint16_t first_seen_day;
    uint16_t best_streak;
    uint32_t total_drill_secs;
    uint32_t flags;

    uint32_t reserved[8];
} HifzBlob;

// 15 KB is far too much for an ESP32 task stack, so the single instance and the
// load scratch are both static. Do not make these locals.
static HifzBlob s_b;

static bool     s_dirty;
static uint32_t s_last_save;
static int      s_cur_day;
static uint32_t s_seq = 1;

uint32_t hifz_state_seq(void) { return s_seq; }

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------
static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void mark_dirty(void) { s_dirty = true; s_seq++; }

// --- config accessors: resolve "not set" to the default ------------------
int hifz_cfg_new_words(void)
{ return (s_b.cfg.set & HZ_CFG_NEW_WORDS) ? s_b.cfg.new_words : HZ_DEF_NEW_WORDS; }
int hifz_cfg_listen_reps(void)
{ return (s_b.cfg.set & HZ_CFG_LISTEN) ? s_b.cfg.listen_reps : HZ_DEF_LISTEN_REPS; }
int hifz_cfg_echo_reps(void)
{ return (s_b.cfg.set & HZ_CFG_ECHO) ? s_b.cfg.echo_reps : HZ_DEF_ECHO_REPS; }
int hifz_cfg_pace_pct(void)
{ return (s_b.cfg.set & HZ_CFG_PACE) ? s_b.cfg.pace_pct : HZ_DEF_PACE_PCT; }
uint32_t hifz_cfg_manzil_mpages(void)
{ return (s_b.cfg.set & HZ_CFG_MANZIL) ? s_b.cfg.manzil_mpages : HZ_DEF_MANZIL_MP; }
static int cfg_sabqi_block(void)
{ return (s_b.cfg.set & HZ_CFG_BLOCK) ? s_b.cfg.sabqi_block : HZ_DEF_SABQI_BLOCK; }

void hifz_cfg_set_new_words(int words)
{
    s_b.cfg.new_words = (uint16_t)clampi(words, 3, 600);
    s_b.cfg.set |= HZ_CFG_NEW_WORDS;
    mark_dirty();
    hifz_flush();
}

void hifz_cfg_set_reps(int listen, int echo)
{
    s_b.cfg.listen_reps = (uint8_t)clampi(listen, 1, 20);
    s_b.cfg.echo_reps   = (uint8_t)clampi(echo, 1, 20);
    s_b.cfg.set |= HZ_CFG_LISTEN | HZ_CFG_ECHO;
    mark_dirty();
    hifz_flush();
}

// -------------------------------------------------------------------------
// Persistence
// -------------------------------------------------------------------------
#define BODY_OFF offsetof(HifzBlob, scope)

// Just enough of the head to compare write sequence. Peeking this instead of
// loading both slots in full is what keeps init to ONE resident blob — four
// copies of a 15 KB struct cost 60 KB of an ESP32's 320 KB.
typedef struct { uint32_t magic; uint16_t version, bytes; uint32_t seq; } SlotHdr;

static bool slot_seq(const char *name, uint32_t *out)
{
    SlotHdr h;
    size_t got = 0;
    if (!hal_state_load(name, &h, sizeof h, &got)) return false;
    if (got < sizeof h || h.magic != HIFZ_MAGIC) return false;
    *out = h.seq;
    return true;
}

// Loads straight into `dst` — no scratch copy. On failure dst is left zeroed
// past whatever was read, and the caller falls back to the other slot.
static bool load_slot(const char *name, HifzBlob *dst)
{
    memset(dst, 0, sizeof *dst);
    size_t got = 0;
    if (!hal_state_load(name, dst, sizeof *dst, &got)) return false;
    if (got < BODY_OFF + sizeof dst->strength) return false;
    if (dst->magic != HIFZ_MAGIC) return false;
    if (dst->bytes < BODY_OFF || dst->bytes > got) return false;   // truncated
    if (crc32((const uint8_t *)dst + BODY_OFF, dst->bytes - BODY_OFF) != dst->crc)
        return false;
    return true;   // tail beyond dst->bytes stayed zeroed => older blobs upgrade
}

static void persist(void)
{
    s_b.magic   = HIFZ_MAGIC;
    s_b.version = HIFZ_VERSION;
    s_b.bytes   = (uint16_t)sizeof s_b;
    s_b.seq++;
    s_b.crc = crc32((const uint8_t *)&s_b + BODY_OFF, s_b.bytes - BODY_OFF);
    hal_state_save((s_b.seq & 1) ? "hifz.a" : "hifz.b", &s_b, sizeof s_b);
    s_dirty = false;
    s_last_save = plat_millis();
}

void hifz_flush(void) { if (s_dirty) persist(); }

void hifz_init(void)
{
    // Pick the newer slot by header, load only that one, and fall back to the
    // other if it fails validation (that's the torn-write case).
    uint32_t sa = 0, sb = 0;
    bool ha = slot_seq("hifz.a", &sa), hb = slot_seq("hifz.b", &sb);
    const char *first = NULL, *second = NULL;
    if (ha && hb) {
        first  = (sa >= sb) ? "hifz.a" : "hifz.b";
        second = (sa >= sb) ? "hifz.b" : "hifz.a";
    } else if (ha) first = "hifz.a";
    else if (hb)   first = "hifz.b";

    bool ok = first && load_slot(first, &s_b);
    if (!ok && second) ok = load_slot(second, &s_b);
    if (!ok) {
        memset(&s_b, 0, sizeof s_b);
        s_b.magic = HIFZ_MAGIC;
        s_b.version = HIFZ_VERSION;
    }

    s_cur_day = qday_today();
    if (s_b.first_seen_day == 0 && s_cur_day > 0)
        s_b.first_seen_day = (uint16_t)s_cur_day;
    s_dirty = false;
    s_last_save = plat_millis();
    s_seq++;

    QN_LOGI(TAG, "loaded: %u portions, scope %u-%u",
            (unsigned)s_b.n_portions, (unsigned)s_b.scope.first_g,
            (unsigned)s_b.scope.last_g);
}

// -------------------------------------------------------------------------
// Per-ayah strength
// -------------------------------------------------------------------------
static int str_get(int g) { return (g < 1 || g > HIFZ_TOTAL_AYAT) ? 0 : (s_b.strength[g - 1] & 7); }

static void str_set(int g, int v)
{
    if (g < 1 || g > HIFZ_TOTAL_AYAT) return;
    uint8_t *p = &s_b.strength[g - 1];
    *p = (uint8_t)((*p & ~7) | (unsigned)clampi(v, 0, 7));
}

int hifz_strength(int surah, int ayah) { return str_get(qdb_global_index(surah, ayah)); }

void hifz_grade_ayah(int surah, int ayah, HifzGrade g)
{
    int gi = qdb_global_index(surah, ayah);
    if (!gi) return;
    int v = str_get(gi);
    if (g == HZ_GOT)        v = clampi(v + 1, 0, 7);
    else if (g == HZ_SHAKY) v = clampi(v, 1, 7);        // seen, but no advance
    else                    v = clampi(v - 1, 0, 7);
    str_set(gi, v);
    if (g == HZ_NO) s_b.strength[gi - 1] |= 0x80;       // lapsed marker
    mark_dirty();
}

static float frac_range(int g0, int g1)
{
    if (g1 < g0) return 0.f;
    int n = 0;
    for (int g = g0; g <= g1; g++) if (str_get(g) > 0) n++;
    return (float)n / (float)(g1 - g0 + 1);
}

float hifz_surah_frac(int surah)
{
    int n = qdb_ayah_count(surah);
    if (n <= 0) return 0.f;
    int g0 = qdb_global_index(surah, 1);
    return frac_range(g0, g0 + n - 1);
}

float hifz_juz_frac(int juz)
{
    if (juz < 1 || juz > QDB_JUZ_COUNT) return 0.f;
    QRef s = qdb_juz_start(juz);
    int g0 = qdb_global_index(s.surah, s.ayah), g1;
    if (juz == QDB_JUZ_COUNT) g1 = HIFZ_TOTAL_AYAT;
    else { QRef e = qdb_juz_start(juz + 1); g1 = qdb_global_index(e.surah, e.ayah) - 1; }
    return frac_range(g0, g1);
}

// -------------------------------------------------------------------------
// Scope
// -------------------------------------------------------------------------
void hifz_set_scope(HifzScopeKind kind, int arg, bool reverse)
{
    HifzScope sc;
    memset(&sc, 0, sizeof sc);
    sc.kind = (uint8_t)kind;
    sc.reverse = reverse ? 1 : 0;
    sc.label_arg = (uint16_t)arg;

    if (kind == HZ_SCOPE_SURAH) {
        int n = qdb_ayah_count(arg);
        if (n <= 0) return;
        sc.first_g = (uint16_t)qdb_global_index(arg, 1);
        sc.last_g  = (uint16_t)(sc.first_g + n - 1);
    } else if (kind == HZ_SCOPE_JUZ) {
        if (arg < 1 || arg > QDB_JUZ_COUNT) return;
        QRef s = qdb_juz_start(arg);
        sc.first_g = (uint16_t)qdb_global_index(s.surah, s.ayah);
        if (arg == QDB_JUZ_COUNT) sc.last_g = HIFZ_TOTAL_AYAT;
        else { QRef e = qdb_juz_start(arg + 1);
               sc.last_g = (uint16_t)(qdb_global_index(e.surah, e.ayah) - 1); }
    } else if (kind == HZ_SCOPE_QURAN) {
        sc.first_g = 1;
        sc.last_g  = HIFZ_TOTAL_AYAT;
    } else {
        memset(&s_b.scope, 0, sizeof s_b.scope);
        s_b.frontier_g = 0;
        mark_dirty(); hifz_flush();
        return;
    }

    s_b.scope = sc;
    s_b.frontier_g = 0;   // re-derive on the next carve
    mark_dirty();
    hifz_flush();
}

HifzScope hifz_scope(void) { return s_b.scope; }
void hifz_clear_scope(void) { hifz_set_scope(HZ_SCOPE_NONE, 0, false); }

void hifz_scope_label(char *buf, int n)
{
    switch (s_b.scope.kind) {
    case HZ_SCOPE_SURAH: snprintf(buf, n, "%s", qdb_surah_name(s_b.scope.label_arg)); break;
    case HZ_SCOPE_JUZ:   snprintf(buf, n, "Juz %d", s_b.scope.label_arg); break;
    case HZ_SCOPE_QURAN: snprintf(buf, n, "Whole Quran"); break;
    default:             snprintf(buf, n, "No target"); break;
    }
}

// -------------------------------------------------------------------------
// Portions
// -------------------------------------------------------------------------
int hifz_portion_count(void) { return s_b.n_portions; }

const HifzPortion *hifz_portion(int i)
{
    if (i < 0 || i >= s_b.n_portions) return NULL;
    return &s_b.portions[i];
}

int hifz_portion_at(int g)
{
    for (int i = 0; i < s_b.n_portions; i++)
        if (s_b.portions[i].first_g && g >= s_b.portions[i].first_g &&
            g <= s_b.portions[i].last_g) return i;
    return -1;
}

void hifz_portion_label(int portion, char *buf, int n)
{
    const HifzPortion *p = hifz_portion(portion);
    if (!p) { snprintf(buf, n, "--"); return; }
    QRef a = qdb_from_global(p->first_g), b = qdb_from_global(p->last_g);
    if (a.surah == b.surah) snprintf(buf, n, "%d:%d-%d", a.surah, a.ayah, b.ayah);
    else snprintf(buf, n, "%d:%d-%d:%d", a.surah, a.ayah, b.surah, b.ayah);
}

// -------------------------------------------------------------------------
// Grading — the heart of the scheduler.
// -------------------------------------------------------------------------
static void set_due(HifzPortion *p, int days)
{
    if (s_cur_day <= 0) { p->due_day = 0; return; }   // no clock: "due whenever"
    int d = s_cur_day + (days < 0 ? 0 : days);
    // Lapse-aware: portions dropped repeatedly come back sooner, permanently.
    // This is the only "ease" concept in the system, and it only shortens.
    if (days > 1 && p->lapses > 3) d -= 1;
    if (d < s_cur_day) d = s_cur_day;
    p->due_day = (uint16_t)d;
}

void hifz_grade(int portion, HifzGrade g, int peeks)
{
    if (portion < 0 || portion >= s_b.n_portions) return;
    HifzPortion *p = &s_b.portions[portion];
    if (!p->first_g) return;

    // Peeking caps the grade. Same philosophy as khatm's dwell rule: a score
    // you can trivially inflate isn't worth recording.
    if (peeks > 0 && g == HZ_GOT) g = HZ_SHAKY;

    switch (p->tier) {
    case HZ_SABAQ:
        if (g == HZ_GOT) {
            p->settle++;
            if (p->settle >= SABAQ_SETTLE) {
                p->tier = HZ_SABQI; p->box = 0; p->streak = 1;
                set_due(p, SABQI_IV[0]);
            } else {
                set_due(p, 0);          // come back to it again today
            }
        } else {
            p->settle = 0;
            if (g == HZ_NO) p->lapses++;
            set_due(p, 0);
        }
        break;

    case HZ_SABQI:
        if (g == HZ_GOT) {
            p->box = (uint8_t)clampi(p->box + 1, 0, SABQI_MAXBOX);
            p->streak++;
            // Graduation needs all three: the top box, a run of clean recalls,
            // AND real elapsed time since THIS portion was created. Box-walking
            // alone can be satisfied in 11 days by generous grading, and an
            // 11-day-old portion is not manzil material. Age must be per
            // portion — gating on the device's own age would graduate a
            // brand-new portion instantly on a device used for months.
            bool old_enough = (s_cur_day > 0 && p->created_day > 0) &&
                              (s_cur_day - (int)p->created_day) >= GRADUATE_AGE_DAYS;
            if (p->box >= SABQI_MAXBOX && p->streak >= GRADUATE_STREAK && old_enough) {
                p->tier = HZ_MANZIL; p->box = 0;
                set_due(p, MANZIL_IV[0]);
            } else {
                set_due(p, SABQI_IV[p->box]);
            }
        } else if (g == HZ_SHAKY) {
            p->streak = 0;
            set_due(p, 1);                       // box held, seen again tomorrow
        } else {
            p->box = 0; p->streak = 0; p->lapses++;
            set_due(p, 0);                       // repair work, today
        }
        break;

    case HZ_MANZIL:
        if (g == HZ_GOT) {
            p->box = (uint8_t)clampi(p->box + 1, 0, MANZIL_MAXBOX);
            p->streak++;
            set_due(p, MANZIL_IV[p->box]);
        } else if (g == HZ_SHAKY) {
            p->box = (uint8_t)clampi((int)p->box - 1, 0, MANZIL_MAXBOX);
            p->streak = 0;
            set_due(p, 3);
        } else {
            p->tier = HZ_SABQI; p->box = 0; p->streak = 0; p->lapses++;
            set_due(p, 1);                       // demoted back into daily review
        }
        break;

    default:
        break;
    }

    if (s_cur_day > 0) p->last_day = (uint16_t)s_cur_day;
    p->last_ord = (uint16_t)s_b.session_ord;

    // Per-ayah strength follows the portion's grade.
    for (int gi = p->first_g; gi <= p->last_g; gi++) {
        QRef r = qdb_from_global(gi);
        if (r.surah) hifz_grade_ayah(r.surah, r.ayah, g);
    }

    // Day tally.
    if (s_cur_day > 0) {
        HifzDay *slot = NULL, *free_slot = NULL;
        for (int i = 0; i < HIFZ_DAYS; i++) {
            if (s_b.days[i].day == (uint16_t)s_cur_day) { slot = &s_b.days[i]; break; }
            if (!free_slot && s_b.days[i].day == 0) free_slot = &s_b.days[i];
        }
        if (!slot) {
            if (!free_slot) {   // evict the oldest
                int oldest = 0;
                for (int i = 1; i < HIFZ_DAYS; i++)
                    if (s_b.days[i].day < s_b.days[oldest].day) oldest = i;
                free_slot = &s_b.days[oldest];
            }
            memset(free_slot, 0, sizeof *free_slot);
            free_slot->day = (uint16_t)s_cur_day;
            slot = free_slot;
        }
        slot->portions++;
    }

    mark_dirty();
    persist();   // a graded portion is not worth risking to a power cut
}

// -------------------------------------------------------------------------
// Carving new sabaq from the scope frontier
// -------------------------------------------------------------------------
static int frontier(void)
{
    if (s_b.scope.kind == HZ_SCOPE_NONE || !s_b.scope.first_g) return 0;
    if (s_b.frontier_g) return s_b.frontier_g;
    return s_b.scope.reverse ? s_b.scope.last_g : s_b.scope.first_g;
}

int hifz_start_new_portion(void)
{
    int f = frontier();
    if (!f) return -1;
    if (s_b.n_portions >= HIFZ_MAX_PORTIONS) {
        QN_LOGE(TAG, "portion table full (%d)", HIFZ_MAX_PORTIONS);
        return -1;
    }
    if (f < s_b.scope.first_g || f > s_b.scope.last_g) return -1;   // scope done

    int target = hifz_cfg_new_words();
    int words = 0, first = f, last = f;
    if (s_b.scope.reverse) {
        // Walk backwards, then normalise so first <= last.
        int g = f;
        while (g >= s_b.scope.first_g) {
            QRef r = qdb_from_global(g);
            int w = qdb_word_count(r.surah, r.ayah);
            if (words && words + w > target) break;
            words += w; first = g; g--;
            if (words >= target) break;
        }
        last = f;
        s_b.frontier_g = (uint16_t)(first - 1);
        if (first - 1 < s_b.scope.first_g) s_b.frontier_g = 0xFFFF;   // exhausted
    } else {
        int g = f;
        while (g <= s_b.scope.last_g) {
            QRef r = qdb_from_global(g);
            int w = qdb_word_count(r.surah, r.ayah);
            if (words && words + w > target) break;
            words += w; last = g; g++;
            if (words >= target) break;
        }
        first = f;
        s_b.frontier_g = (uint16_t)(last + 1);
        if (last + 1 > s_b.scope.last_g) s_b.frontier_g = 0xFFFF;
    }

    HifzPortion *p = &s_b.portions[s_b.n_portions];
    memset(p, 0, sizeof *p);
    p->first_g = (uint16_t)first;
    p->last_g  = (uint16_t)last;
    p->tier    = HZ_SABAQ;
    p->due_day = (uint16_t)(s_cur_day > 0 ? s_cur_day : 0);
    p->created_day = (uint16_t)(s_cur_day > 0 ? s_cur_day : 0);
    p->last_ord = (uint16_t)s_b.session_ord;
    int idx = s_b.n_portions++;

    if (s_cur_day > 0 && s_b.first_seen_day == 0)
        s_b.first_seen_day = (uint16_t)s_cur_day;

    mark_dirty();
    persist();
    return idx;
}

// -------------------------------------------------------------------------
// Today's plan
// -------------------------------------------------------------------------
static HifzPlan s_plan;
static uint32_t s_plan_seq;
static int      s_plan_day = -1;

static bool due_now(const HifzPortion *p)
{
    if (s_cur_day <= 0) return true;        // no clock: everything is "due whenever"
    return p->due_day == 0 || (int)p->due_day <= s_cur_day;
}

static int overdue_days(const HifzPortion *p)
{
    if (s_cur_day <= 0 || p->due_day == 0) return 0;
    int d = s_cur_day - (int)p->due_day;
    return d > 0 ? d : 0;
}

// Higher = more urgent. Age dominates; box and lapses break ties.
static int manzil_prio(const HifzPortion *p)
{
    int age = (s_cur_day > 0 && p->last_day)
            ? s_cur_day - (int)p->last_day
            : (int)(s_b.session_ord - p->last_ord);
    if (age < 0) age = 0;
    return age * 8 + (MANZIL_MAXBOX - p->box) * 3 + (p->lapses > 3 ? 6 : p->lapses);
}

static uint32_t portion_mpages(const HifzPortion *p)
{
    // Approximate: ayat spanned / ayat on those pages. Good enough for a budget.
    int pa = qdb_page_of_global(p->first_g), pb = qdb_page_of_global(p->last_g);
    if (!pa || !pb) return 1000;
    return (uint32_t)(pb - pa + 1) * 1000;
}

const HifzPlan *hifz_plan(void)
{
    if (s_plan_seq == s_seq && s_plan_day == s_cur_day) return &s_plan;
    s_plan_seq = s_seq;
    s_plan_day = s_cur_day;

    HifzPlan *pl = &s_plan;
    memset(pl, 0, sizeof *pl);
    pl->have_day = s_cur_day > 0;
    pl->today = s_cur_day;

    // --- sabqi (uncapped by definition) + overdue count -------------------
    for (int i = 0; i < s_b.n_portions; i++) {
        const HifzPortion *p = &s_b.portions[i];
        if (!p->first_g || p->tier != HZ_SABQI || !due_now(p)) continue;
        int od = overdue_days(p);
        if (od > 0) pl->n_sabqi_overdue++;
        if (pl->n_sabqi < HIFZ_PLAN_MAX) {
            pl->sabqi[pl->n_sabqi].portion = (int16_t)i;
            pl->sabqi[pl->n_sabqi].tier = HZ_SABQI;
            pl->sabqi[pl->n_sabqi].overdue = (uint8_t)clampi(od, 0, 255);
            pl->n_sabqi++;
        }
    }
    // due_day asc, then box asc — the most fragile first.
    for (int i = 1; i < pl->n_sabqi; i++) {
        HifzTask t = pl->sabqi[i];
        int j = i - 1;
        while (j >= 0) {
            const HifzPortion *a = &s_b.portions[pl->sabqi[j].portion];
            const HifzPortion *b = &s_b.portions[t.portion];
            bool worse = (a->due_day > b->due_day) ||
                         (a->due_day == b->due_day && a->box > b->box);
            if (!worse) break;
            pl->sabqi[j + 1] = pl->sabqi[j]; j--;
        }
        pl->sabqi[j + 1] = t;
    }

    // --- sabaq: at most one, and gated on the review backlog --------------
    // Piling new material on top of collapsing review is the main way people
    // lose their hifz. The app refuses rather than enabling it.
    pl->sabaq_blocked = pl->n_sabqi_overdue > cfg_sabqi_block();
    for (int i = 0; i < s_b.n_portions; i++) {
        const HifzPortion *p = &s_b.portions[i];
        if (p->first_g && p->tier == HZ_SABAQ && due_now(p)) {
            pl->sabaq.portion = (int16_t)i;
            pl->sabaq.tier = HZ_SABAQ;
            pl->sabaq.overdue = (uint8_t)clampi(overdue_days(p), 0, 255);
            pl->n_sabaq = 1;
            break;
        }
    }
    int f = frontier();
    pl->new_available = s_b.scope.kind != HZ_SCOPE_NONE && f &&
                        f != 0xFFFF && f >= s_b.scope.first_g && f <= s_b.scope.last_g;
    if (pl->n_sabaq == 0 && pl->sabaq_blocked) pl->new_available = false;

    // --- manzil: cycle-driven under a page budget -------------------------
    int cand[HIFZ_MAX_PORTIONS], nc = 0;
    for (int i = 0; i < s_b.n_portions; i++) {
        const HifzPortion *p = &s_b.portions[i];
        if (p->first_g && p->tier == HZ_MANZIL && due_now(p)) cand[nc++] = i;
    }
    // Selection sort by priority — nc is small and this runs on state change.
    uint32_t budget = hifz_cfg_manzil_mpages(), used = 0;
    for (int k = 0; k < nc; k++) {
        int best = -1, bestp = -1;
        for (int i = k; i < nc; i++) {
            int pr = manzil_prio(&s_b.portions[cand[i]]);
            if (pr > bestp) { bestp = pr; best = i; }
        }
        int tmp = cand[k]; cand[k] = cand[best]; cand[best] = tmp;

        uint32_t mp = portion_mpages(&s_b.portions[cand[k]]);
        if (used + mp > budget && pl->n_manzil > 0) { pl->manzil_backlog = nc - k; break; }
        if (pl->n_manzil >= HIFZ_PLAN_MAX) { pl->manzil_backlog = nc - k; break; }
        used += mp;
        pl->manzil[pl->n_manzil].portion = (int16_t)cand[k];
        pl->manzil[pl->n_manzil].tier = HZ_MANZIL;
        pl->manzil[pl->n_manzil].overdue = (uint8_t)clampi(overdue_days(&s_b.portions[cand[k]]), 0, 255);
        pl->n_manzil++;
    }
    pl->manzil_mpages = used;

    // --- today's completion ----------------------------------------------
    pl->total_today = pl->n_sabaq + pl->n_sabqi + pl->n_manzil;
    if (s_cur_day > 0) {
        for (int i = 0; i < s_b.n_portions; i++)
            if (s_b.portions[i].first_g && s_b.portions[i].last_day == (uint16_t)s_cur_day)
                pl->done_today++;
    }
    return pl;
}

// -------------------------------------------------------------------------
// Stats
// -------------------------------------------------------------------------
static HifzStats s_stats;
static uint32_t s_stats_seq;
static int      s_stats_day = -1;

static int compute_streak(void)
{
    if (s_cur_day <= 0) return 0;
    int streak = 0;
    for (int d = s_cur_day, n = 0; n < HIFZ_DAYS && d > 0; d--, n++) {
        int found = 0;
        for (int i = 0; i < HIFZ_DAYS; i++)
            if (s_b.days[i].day == (uint16_t)d && s_b.days[i].portions > 0) { found = 1; break; }
        if (found) streak++;
        else if (d == s_cur_day) continue;   // today isn't over yet
        else break;
    }
    return streak;
}

const HifzStats *hifz_stats(void)
{
    if (s_stats_seq == s_seq && s_stats_day == s_cur_day) return &s_stats;
    s_stats_seq = s_seq;
    s_stats_day = s_cur_day;

    HifzStats *k = &s_stats;
    memset(k, 0, sizeof *k);
    for (int g = 1; g <= HIFZ_TOTAL_AYAT; g++) if (str_get(g) > 0) k->memorized_ayat++;
    for (int i = 0; i < s_b.n_portions; i++) {
        if (!s_b.portions[i].first_g) continue;
        k->portions++;
        if (s_b.portions[i].tier == HZ_SABQI)  k->n_sabqi++;
        if (s_b.portions[i].tier == HZ_MANZIL) k->n_manzil++;
    }
    if (s_b.scope.first_g && s_b.scope.last_g >= s_b.scope.first_g) {
        k->scope_total = s_b.scope.last_g - s_b.scope.first_g + 1;
        for (int g = s_b.scope.first_g; g <= s_b.scope.last_g; g++)
            if (str_get(g) > 0) k->scope_done++;
        k->scope_frac = k->scope_total ? (float)k->scope_done / (float)k->scope_total : 0.f;
    }
    k->have_day = s_cur_day > 0;
    k->day = s_cur_day;
    k->streak = compute_streak();
    if (k->streak > (int)s_b.best_streak) s_b.best_streak = (uint16_t)k->streak;
    k->best_streak = s_b.best_streak;
    return k;
}

// -------------------------------------------------------------------------
// Chunker — pure; drives the drill's recite units.
// -------------------------------------------------------------------------
int hifz_seg_words(int surah, const HifzSeg *s)
{
    if (!s) return 0;
    if (s->a0 == s->a1) return s->w1 - s->w0 + 1;
    int n = qdb_word_count(surah, s->a0) - s->w0;      // tail of the first ayah
    for (int a = s->a0 + 1; a < s->a1; a++) n += qdb_word_count(surah, a);
    return n + s->w1 + 1;                              // head of the last
}

int hifz_chunk(int surah, int a0, int a1, int target_words, HifzSeg *out, int max)
{
    if (!out || max <= 0 || a0 < 1 || a1 < a0) return 0;
    int last = qdb_ayah_count(surah);
    if (last <= 0) return 0;
    if (a1 > last) a1 = last;
    if (target_words < 3) target_words = 3;

    int n = 0;
    int a = a0;
    int pend_a0 = -1, pend_w0 = 0, pend_words = 0;   // open whole-ayah run

    while (a <= a1 && n < max) {
        int w = qdb_word_count(surah, a);
        if (w <= 0) { a++; continue; }

        // An ayah far over target is split internally — otherwise the recall
        // wouldn't fit on screen (2:282 is 1753px at the smallest pack).
        if (w > target_words * 3 / 2) {
            if (pend_a0 >= 0) {                       // close the open run first
                out[n].a0 = (int16_t)pend_a0; out[n].w0 = (int16_t)pend_w0;
                out[n].a1 = (int16_t)(a - 1);
                out[n].w1 = (int16_t)(qdb_word_count(surah, a - 1) - 1);
                n++; pend_a0 = -1; pend_words = 0;
                if (n >= max) break;
            }
            int parts = (w + target_words - 1) / target_words;
            if (parts < 1) parts = 1;
            int done = 0;
            for (int i = 0; i < parts && n < max; i++) {
                int take = (w - done) / (parts - i);
                if (take < 1) take = 1;
                out[n].a0 = out[n].a1 = (int16_t)a;
                out[n].w0 = (int16_t)done;
                out[n].w1 = (int16_t)(done + take - 1);
                done += take;
                n++;
            }
            a++;
            continue;
        }

        // Would adding this ayah overshoot? Close the run and start fresh.
        if (pend_a0 >= 0 && pend_words + w > target_words) {
            out[n].a0 = (int16_t)pend_a0; out[n].w0 = (int16_t)pend_w0;
            out[n].a1 = (int16_t)(a - 1);
            out[n].w1 = (int16_t)(qdb_word_count(surah, a - 1) - 1);
            n++; pend_a0 = -1; pend_words = 0;
            if (n >= max) break;
        }
        if (pend_a0 < 0) { pend_a0 = a; pend_w0 = 0; pend_words = 0; }
        pend_words += w;
        a++;
    }

    if (pend_a0 >= 0 && n < max) {
        out[n].a0 = (int16_t)pend_a0; out[n].w0 = (int16_t)pend_w0;
        out[n].a1 = (int16_t)a1;
        out[n].w1 = (int16_t)(qdb_word_count(surah, a1) - 1);
        n++;
    }
    return n;
}

// -------------------------------------------------------------------------
// Per-frame service
// -------------------------------------------------------------------------
void hifz_service(void)
{
    int day = qday_today();

    if (day != s_cur_day) {
        int prev = s_cur_day;
        s_cur_day = day;
        if (day > 0) {
            if (s_b.first_seen_day == 0) s_b.first_seen_day = (uint16_t)day;
            // The clock just became known: portions parked as "due whenever"
            // become due now. Conservative and correct — boxes, streaks and
            // tiers are deliberately left alone.
            if (prev <= 0) {
                for (int i = 0; i < s_b.n_portions; i++) {
                    HifzPortion *p = &s_b.portions[i];
                    if (!p->first_g) continue;
                    if (p->due_day == 0) p->due_day = (uint16_t)day;
                    // Without this a portion started on a clockless device
                    // could never satisfy the graduation age gate.
                    if (p->created_day == 0) p->created_day = (uint16_t)day;
                }
            }
            s_b.session_ord++;
            persist();
        }
        s_seq++;
        s_plan_day = -1;
        s_stats_day = -1;
    }

    if (s_dirty && plat_millis() - s_last_save >= SAVE_DEBOUNCE_MS) persist();
}
