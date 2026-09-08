#include "khatm.h"
#include "qday.h"
#include "quran_db.h"
#include "progress.h"
#include "hal.h"
#include "plat.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "KHATM";

#define KHATM_MAGIC   0x314B4B51u   // "QKK1"
#define KHATM_VERSION 1

#define FLAG_BACKFILL_OFFERED 0x1u

#define SAVE_INTERVAL_MS 30000u   // at most one blob write per 30s
#define DT_CLAMP_MS        250u   // ignore SD stalls / frame hitches
#define AVG_WINDOW_DAYS      7

// -------------------------------------------------------------------------
// Persisted state.
//
// Three rules keep this growable across firmware versions, which is what
// ProgressBlob got wrong:
//   1. Fields are only ever APPENDED (into reserved[]). Never reordered,
//      never repurposed.
//   2. All-bits-zero must be a valid default for every field. Hence day == 0
//      means "empty slot" and goal.start_day == 0 means "no goal" — day 0 is
//      1970-01-01, which can never legitimately occur.
//   3. The loader memsets first and accepts a SHORT read, so a blob written by
//      an older build leaves the newer tail zeroed instead of being rejected.
//
// Written to two alternating slots because hal_state_save opens "wb": it
// truncates before writing, so a power cut mid-write destroys whichever slot
// it was working on. The other one survives.
// -------------------------------------------------------------------------
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;                 // sizeof(KhatmBlob) as written
    uint32_t seq;                   // higher wins when both slots are valid
    uint32_t crc;                   // over [offsetof(read) .. bytes)

    uint8_t  read[(KHATM_TOTAL_AYAT + 7) / 8];   // bit (g-1) = global ayah g
    uint32_t read_count;
    uint32_t read_mpages;

    KhatmGoal goal;

    KhatmDay days[KHATM_DAYS];      // unordered, keyed by .day (find-or-create)
    uint8_t  day_head;              // next slot to evict once full
    uint8_t  pad0[3];

    uint16_t pend_ayat;             // credited while the clock was unknown
    uint16_t pad1;
    uint32_t pend_mpages;

    uint16_t khatms_done;
    uint16_t last_khatm_day;
    uint16_t best_streak;
    uint16_t first_seen_day;
    uint32_t total_read_secs;
    uint32_t flags;

    uint32_t reserved[7];           // append here; zero means default
} KhatmBlob;

static KhatmBlob s_b;

static bool     s_dirty;
static uint32_t s_last_save;
static int      s_cur_day;
static uint32_t s_cov_seq = 1;    // bumped on coverage change
static uint32_t s_goal_seq = 1;   // bumped on goal change
static uint32_t s_secs_accum_ms;

// Focus / dwell state for the ayah currently on screen.
static struct {
    int      surah, ayah;
    int      n_words;
    uint32_t dwell_ms, need_ms;
    bool     credited;
} s_focus;

// -------------------------------------------------------------------------
// Small helpers
// -------------------------------------------------------------------------

// Table-free CRC-32 (reflected, poly 0xEDB88320). ~1us over 2.4KB; there is no
// crc32 anywhere else in core/, and this is the only caller.
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

// Milli-pages for `c` of `n` ayat read. credit() DIFFERENCES this rather than
// adding 1000/n per ayah, so a full page is always exactly 1000 milli-pages
// (1000/42*42 = 966 — the naive version drifts and never reaches 604.0).
static uint32_t mp_of(int c, int n)
{
    if (n <= 0) return 0;
    return (uint32_t)((1000 * (long)c + n / 2) / n);
}

static bool bit_get(int g)   // g is 1-based global ayah index
{
    if (g < 1 || g > KHATM_TOTAL_AYAT) return false;
    return (s_b.read[(g - 1) >> 3] >> ((g - 1) & 7)) & 1;
}

static void bit_set(int g)
{
    if (g < 1 || g > KHATM_TOTAL_AYAT) return;
    s_b.read[(g - 1) >> 3] |= (uint8_t)(1u << ((g - 1) & 7));
}

// Ayat read on a page. Pages hold 1..42 ayat, so a plain loop is fine.
static int page_read_count(int page)
{
    int first = qdb_page_first_global(page), n = qdb_page_ayah_count(page);
    int cnt = 0;
    for (int i = first; i < first + n; i++)
        if (bit_get(i)) cnt++;
    return cnt;
}

static int count_range(int g0, int g1)
{
    int cnt = 0;
    for (int i = g0; i <= g1; i++)
        if (bit_get(i)) cnt++;
    return cnt;
}

// -------------------------------------------------------------------------
// Day index. The computation lives in qday.c because hifz needs it too; khatm
// keeps a *cached* day (refreshed in khatm_service) while qday_today() is the
// live read — that distinction matters, so the two are not interchangeable.
// -------------------------------------------------------------------------
static int today_index(void) { return qday_today(); }

int khatm_today(void) { return s_cur_day; }

void khatm_format_day(int day, char *buf, int n) { qday_format(day, buf, n); }

// -------------------------------------------------------------------------
// Day history ring — find-or-create, keyed by day. Deliberately unordered:
// that makes a backwards clock jump (bad NTP, timezone edit) add to the
// existing slot instead of corrupting or rewriting history.
// -------------------------------------------------------------------------
static KhatmDay *day_slot(int day, bool create)
{
    if (day <= 0) return NULL;
    KhatmDay *free_slot = NULL;
    for (int i = 0; i < KHATM_DAYS; i++) {
        if (s_b.days[i].day == (uint16_t)day) return &s_b.days[i];
        if (!free_slot && s_b.days[i].day == 0) free_slot = &s_b.days[i];
    }
    if (!create) return NULL;
    if (free_slot) {
        memset(free_slot, 0, sizeof *free_slot);
        free_slot->day = (uint16_t)day;
        return free_slot;
    }
    // Full: evict the oldest day rather than the round-robin head, so a stale
    // clock can't punch a hole in recent history.
    int oldest = 0;
    for (int i = 1; i < KHATM_DAYS; i++)
        if (s_b.days[i].day < s_b.days[oldest].day) oldest = i;
    memset(&s_b.days[oldest], 0, sizeof s_b.days[oldest]);
    s_b.days[oldest].day = (uint16_t)day;
    return &s_b.days[oldest];
}

static uint32_t day_mpages(int day)
{
    KhatmDay *d = day_slot(day, false);
    return d ? d->mpages : 0;
}

static void day_add(int day, int ayat, uint32_t mpages)
{
    if (day <= 0) {   // clock unknown: hold it until we learn the date
        s_b.pend_ayat = (uint16_t)(s_b.pend_ayat + ayat);
        s_b.pend_mpages += mpages;
        return;
    }
    KhatmDay *d = day_slot(day, true);
    if (!d) return;
    d->ayat = (uint16_t)(d->ayat + ayat);
    d->mpages += mpages;
}

// -------------------------------------------------------------------------
// Persistence
// -------------------------------------------------------------------------
#define BODY_OFF offsetof(KhatmBlob, read)

// Just enough of the head to compare write sequence, so init never holds two
// blobs at once (see khatm_init).
typedef struct { uint32_t magic; uint16_t version, bytes; uint32_t seq; } SlotHdr;

static bool slot_seq(const char *name, uint32_t *out)
{
    SlotHdr h;
    size_t got = 0;
    if (!hal_state_load(name, &h, sizeof h, &got)) return false;
    if (got < sizeof h || h.magic != KHATM_MAGIC) return false;
    *out = h.seq;
    return true;
}

// Loads straight into `dst` — no scratch copy.
static bool load_slot(const char *name, KhatmBlob *dst)
{
    memset(dst, 0, sizeof *dst);
    size_t got = 0;
    if (!hal_state_load(name, dst, sizeof *dst, &got)) return false;
    if (got < BODY_OFF + sizeof dst->read) return false;   // too short to be useful
    if (dst->magic != KHATM_MAGIC) return false;
    if (dst->bytes < BODY_OFF || dst->bytes > got) return false;   // truncated
    if (crc32((const uint8_t *)dst + BODY_OFF, dst->bytes - BODY_OFF) != dst->crc)
        return false;
    return true;   // tail beyond dst->bytes stayed zeroed => older blobs upgrade
}

static void persist(void)
{
    s_b.magic   = KHATM_MAGIC;
    s_b.version = KHATM_VERSION;
    s_b.bytes   = (uint16_t)sizeof s_b;
    s_b.seq++;
    s_b.crc = crc32((const uint8_t *)&s_b + BODY_OFF, s_b.bytes - BODY_OFF);
    hal_state_save((s_b.seq & 1) ? "khatm.a" : "khatm.b", &s_b, sizeof s_b);
    s_dirty = false;
    s_last_save = plat_millis();
}

static void mark_dirty(void) { s_dirty = true; }

void khatm_flush(void) { if (s_dirty) persist(); }

// Recompute read_count / read_mpages from the bitmap. Used after a bulk edit
// and as a repair if the cached totals ever disagree with the bits.
static void recount(void)
{
    uint32_t mp = 0;
    int total = 0;
    for (int p = 1; p <= KHATM_TOTAL_PAGES; p++) {
        int n = qdb_page_ayah_count(p), c = page_read_count(p);
        total += c;
        mp += mp_of(c, n);
    }
    s_b.read_count  = (uint32_t)total;
    s_b.read_mpages = mp;
}

void khatm_init(void)
{
    // Pick the newer slot by header, load only that one, fall back to the other
    // if it fails validation (the torn-write case). Loading both in full would
    // put two more blobs on the stack for no benefit.
    uint32_t sa = 0, sb = 0;
    bool ha = slot_seq("khatm.a", &sa), hb = slot_seq("khatm.b", &sb);
    const char *first = NULL, *second = NULL;
    if (ha && hb) {
        first  = (sa >= sb) ? "khatm.a" : "khatm.b";
        second = (sa >= sb) ? "khatm.b" : "khatm.a";
    } else if (ha) first = "khatm.a";
    else if (hb)   first = "khatm.b";

    bool ok = first && load_slot(first, &s_b);
    if (!ok && second) ok = load_slot(second, &s_b);
    if (!ok) {
        memset(&s_b, 0, sizeof s_b);
        s_b.magic = KHATM_MAGIC;
        s_b.version = KHATM_VERSION;
    }

    s_cur_day = today_index();
    if (s_b.first_seen_day == 0 && s_cur_day > 0)
        s_b.first_seen_day = (uint16_t)s_cur_day;

    recount();   // cheap, and self-heals a torn cache
    memset(&s_focus, 0, sizeof s_focus);
    s_last_save = plat_millis();

    QN_LOGI(TAG, "loaded: %u ayat read (%u.%03u pages), %u khatm done",
            (unsigned)s_b.read_count, (unsigned)(s_b.read_mpages / 1000),
            (unsigned)(s_b.read_mpages % 1000), (unsigned)s_b.khatms_done);
}

// -------------------------------------------------------------------------
// Crediting
// -------------------------------------------------------------------------
static void credit(int surah, int ayah)
{
    int g = qdb_global_index(surah, ayah);
    if (!g || bit_get(g)) return;   // unknown ref, or already read this khatm

    int p = qdb_page_of_global(g);
    int n = qdb_page_ayah_count(p);
    int before = page_read_count(p);

    bit_set(g);
    s_b.read_count++;
    uint32_t d = mp_of(before + 1, n) - mp_of(before, n);
    s_b.read_mpages += d;
    day_add(s_cur_day, 1, d);
    s_cov_seq++;

    if (s_b.read_count >= KHATM_TOTAL_AYAT && s_b.last_khatm_day != s_cur_day) {
        s_b.khatms_done++;
        s_b.last_khatm_day = (uint16_t)s_cur_day;
        persist();   // a completed khatm is not something to risk losing
        return;
    }
    mark_dirty();
}

void khatm_focus(int surah, int ayah, int n_words)
{
    if (surah != s_focus.surah || ayah != s_focus.ayah) {
        s_focus.surah = surah;
        s_focus.ayah = ayah;
        s_focus.dwell_ms = 0;
        s_focus.n_words = 0;
        s_focus.need_ms = KHATM_MIN_DWELL_MS;
        s_focus.credited = khatm_is_read(surah, ayah);
    }
    // The glyph pack supplies the word count, and app_tick runs before
    // app_render — so the first tick after a change has none. Accept it late.
    if (n_words > 0 && n_words != s_focus.n_words) {
        s_focus.n_words = n_words;
        uint32_t need = (uint32_t)n_words * KHATM_MS_PER_WORD;
        if (need < KHATM_MIN_DWELL_MS) need = KHATM_MIN_DWELL_MS;
        if (need > KHATM_MAX_DWELL_MS) need = KHATM_MAX_DWELL_MS;
        s_focus.need_ms = need;
    }
}

void khatm_tick(uint32_t dt_ms)
{
    if (!s_focus.surah) return;
    if (dt_ms > DT_CLAMP_MS) dt_ms = DT_CLAMP_MS;

    s_secs_accum_ms += dt_ms;
    if (s_secs_accum_ms >= 1000) {
        s_b.total_read_secs += s_secs_accum_ms / 1000;
        s_secs_accum_ms %= 1000;
    }

    if (s_focus.credited) return;
    s_focus.dwell_ms += dt_ms;
    if (s_focus.dwell_ms >= s_focus.need_ms) {
        credit(s_focus.surah, s_focus.ayah);
        s_focus.credited = true;
    }
}

void khatm_audio_complete(int surah, int ayah)
{
    credit(surah, ayah);
    if (surah == s_focus.surah && ayah == s_focus.ayah) s_focus.credited = true;
}

float khatm_focus_dwell_frac(void)
{
    if (!s_focus.surah || s_focus.need_ms == 0) return 0.f;
    if (s_focus.dwell_ms >= s_focus.need_ms) return 1.f;
    return (float)s_focus.dwell_ms / (float)s_focus.need_ms;
}

uint32_t khatm_focus_dwell_ms(void) { return s_focus.dwell_ms; }
bool     khatm_focus_credited(void) { return s_focus.credited; }

// -------------------------------------------------------------------------
// Coverage queries
// -------------------------------------------------------------------------
bool khatm_is_read(int surah, int ayah)
{
    return bit_get(qdb_global_index(surah, ayah));
}

float khatm_page_frac(int page)
{
    int n = qdb_page_ayah_count(page);
    if (n <= 0) return 0.f;
    return (float)page_read_count(page) / (float)n;
}

float khatm_surah_frac(int surah)
{
    int n = qdb_ayah_count(surah);
    if (n <= 0) return 0.f;
    int g0 = qdb_global_index(surah, 1);
    return (float)count_range(g0, g0 + n - 1) / (float)n;
}

float khatm_juz_frac(int juz)
{
    if (juz < 1 || juz > QDB_JUZ_COUNT) return 0.f;
    QRef s = qdb_juz_start(juz);
    int g0 = qdb_global_index(s.surah, s.ayah);
    int g1;
    if (juz == QDB_JUZ_COUNT) {
        g1 = KHATM_TOTAL_AYAT;
    } else {
        QRef e = qdb_juz_start(juz + 1);
        g1 = qdb_global_index(e.surah, e.ayah) - 1;
    }
    if (g1 < g0) return 0.f;
    return (float)count_range(g0, g1) / (float)(g1 - g0 + 1);
}

uint32_t khatm_coverage_seq(void) { return s_cov_seq; }

// -------------------------------------------------------------------------
// Stats
// -------------------------------------------------------------------------
static KhatmStats s_stats;
static uint32_t s_stats_cov, s_stats_goal;
static int      s_stats_day = -1;

static int compute_streak(void)
{
    if (s_cur_day <= 0) return 0;
    int streak = 0;
    for (int d = s_cur_day, n = 0; n < KHATM_DAYS && d > 0; d--, n++) {
        if (day_mpages(d) >= KHATM_STREAK_MPAGES) {
            streak++;
        } else if (d == s_cur_day) {
            continue;   // today is still in progress; it can't break a streak
        } else {
            break;
        }
    }
    return streak;
}

const KhatmStats *khatm_stats(void)
{
    if (s_stats_cov == s_cov_seq && s_stats_day == s_cur_day &&
        s_stats_goal == s_goal_seq)
        return &s_stats;
    s_stats_cov = s_cov_seq;
    s_stats_day = s_cur_day;
    s_stats_goal = s_goal_seq;

    KhatmStats *k = &s_stats;
    memset(k, 0, sizeof *k);

    k->read_mpages = s_b.read_mpages;
    k->ayat_read   = (int)s_b.read_count;
    k->pages       = (float)s_b.read_mpages / 1000.f;
    k->percent     = (float)s_b.read_mpages * 100.f / (float)KHATM_TOTAL_MPAGES;
    k->complete    = s_b.read_count >= KHATM_TOTAL_AYAT;
    k->khatms_done = s_b.khatms_done;
    k->best_streak = s_b.best_streak;

    for (int p = 1; p <= KHATM_TOTAL_PAGES; p++)
        if (page_read_count(p) == qdb_page_ayah_count(p)) k->pages_full++;
    for (int j = 1; j <= QDB_JUZ_COUNT; j++)
        if (khatm_juz_frac(j) >= 1.f) k->juz_full++;

    k->have_day = s_cur_day > 0;
    if (!k->have_day) return k;   // no clock: everything below is meaningless

    k->today_mpages = day_mpages(s_cur_day);
    k->streak = compute_streak();
    if (k->streak > k->best_streak) k->best_streak = k->streak;

    // Trailing average. Divide by the days actually elapsed (capped at the
    // window) so day one doesn't report a bogus one-seventh pace. Zero days
    // inside the window still count against you — that's honest.
    int span = AVG_WINDOW_DAYS;
    int since = s_b.first_seen_day ? s_cur_day - (int)s_b.first_seen_day + 1 : 1;
    if (since < 1) since = 1;
    if (span > since) span = since;
    uint32_t sum = 0;
    for (int i = 0; i < span; i++) sum += day_mpages(s_cur_day - i);
    k->avg = span > 0 ? (float)sum / (1000.f * (float)span) : 0.f;

    uint32_t remaining = KHATM_TOTAL_MPAGES > s_b.read_mpages
                       ? KHATM_TOTAL_MPAGES - s_b.read_mpages : 0;

    // Projected finish from the trailing pace.
    if (remaining == 0) {
        k->eta_day = s_cur_day;
    } else if (k->avg > 0.001f) {
        uint32_t rate = (uint32_t)(k->avg * 1000.f);
        if (rate < 1) rate = 1;
        k->eta_day = s_cur_day + (int)((remaining + rate - 1) / rate);
    }

    if (s_b.goal.start_day == 0) return k;   // no goal: pace stats only

    k->have_goal    = true;
    k->days_elapsed = s_cur_day - (int)s_b.goal.start_day + 1;
    if (k->days_elapsed < 1) k->days_elapsed = 1;
    int left = (int)s_b.goal.target_day - s_cur_day;
    k->days_left = left > 0 ? left : 0;
    k->overdue   = left < 0 && !k->complete;

    // The original straight-line rate the goal implied.
    int total_days = (int)s_b.goal.target_day - (int)s_b.goal.start_day + 1;
    if (total_days < 1) total_days = 1;
    uint32_t plan_span = KHATM_TOTAL_MPAGES > s_b.goal.start_mpages
                       ? KHATM_TOTAL_MPAGES - s_b.goal.start_mpages : 0;
    uint32_t per_day = plan_span / (uint32_t)total_days;

    // Re-deriving the quota from what's actually left IS the ahead/behind
    // mechanism: get ahead and remaining shrinks faster than the days do, so
    // tomorrow asks for less. Fall behind and it asks for more.
    if (k->overdue) {
        // Past the deadline there are no days left to divide by, and dividing
        // by the clamped 1 would demand the entire rest of the Quran today.
        // Keep asking for the original daily amount until the user extends or
        // re-plans — the goal card prompts them to.
        k->quota_mpages = per_day ? per_day : KHATM_STREAK_MPAGES;
    } else {
        uint32_t denom = (uint32_t)left + 1;
        k->quota_mpages = (remaining + denom - 1) / denom;
    }
    if (k->quota_mpages > remaining) k->quota_mpages = remaining;

    // Narrative delta: where the original straight-line plan said you'd be.
    // Counts COMPLETED days only — today is still in progress, so being told
    // you're a day behind before the day is over would just be wrong.
    uint32_t expected = s_b.goal.start_mpages +
                        per_day * (uint32_t)(k->days_elapsed - 1);
    if (expected > KHATM_TOTAL_MPAGES) expected = KHATM_TOTAL_MPAGES;
    k->delta_mpages = (int32_t)s_b.read_mpages - (int32_t)expected;

    return k;
}

// -------------------------------------------------------------------------
// "What to read today"
// -------------------------------------------------------------------------
static KhatmPlan s_plan;
static uint32_t s_plan_cov, s_plan_goal;
static int      s_plan_day = -1;

const KhatmPlan *khatm_today_plan(void)
{
    if (s_plan_cov == s_cov_seq && s_plan_day == s_cur_day &&
        s_plan_goal == s_goal_seq)
        return &s_plan;
    s_plan_cov = s_cov_seq;
    s_plan_day = s_cur_day;
    s_plan_goal = s_goal_seq;

    KhatmPlan *pl = &s_plan;
    memset(pl, 0, sizeof *pl);

    const KhatmStats *k = khatm_stats();
    if (k->complete) return pl;

    // Start from where the reader left off, then walk to the first page that
    // still has unread ayat, wrapping once at the end of the mushaf.
    int p0 = 1;
    if (progress_has_resume()) {
        ResumePoint r = progress_resume();
        int p = qdb_page_of(r.surah, r.ayah);
        if (p > 0) p0 = p;
    }
    int start = p0;
    for (int i = 0; i < KHATM_TOTAL_PAGES; i++) {
        int p = start + i;
        if (p > KHATM_TOTAL_PAGES) { p -= KHATM_TOTAL_PAGES; pl->wrapped = true; }
        if (page_read_count(p) < qdb_page_ayah_count(p)) { p0 = p; break; }
    }

    // How much is still owed today. Once the quota is met, still offer a page
    // rather than nothing — the screen shouldn't dead-end.
    uint32_t need = 1000;
    if (k->have_goal && k->quota_mpages > k->today_mpages)
        need = k->quota_mpages - k->today_mpages;
    if (need == 0) need = 1000;

    uint32_t got = 0;
    int p = p0, last = p0;
    for (int i = 0; i < KHATM_TOTAL_PAGES && got < need; i++) {
        int n = qdb_page_ayah_count(p);
        uint32_t unread = 1000 - mp_of(page_read_count(p), n);
        got += unread;
        last = p;
        if (++p > KHATM_TOTAL_PAGES) { p = 1; pl->wrapped = true; }
    }

    pl->from_page = p0;
    pl->to_page   = last;
    pl->mpages    = got;

    // Land on the first UNREAD ayah of the opening page, so pressing SELECT
    // puts you exactly where you should start rather than at a page boundary
    // you already covered.
    int first = qdb_page_first_global(p0), n0 = qdb_page_ayah_count(p0);
    int g = first;
    for (int i = first; i < first + n0; i++)
        if (!bit_get(i)) { g = i; break; }
    QRef a = qdb_from_global(g);
    QRef b = qdb_page_end(last);
    pl->from_surah = a.surah; pl->from_ayah = a.ayah;
    pl->to_surah   = b.surah; pl->to_ayah   = b.ayah;
    return pl;
}

// -------------------------------------------------------------------------
// Goal
// -------------------------------------------------------------------------
KhatmGoal khatm_goal(void) { return s_b.goal; }

void khatm_set_goal_days(int days)
{
    if (s_cur_day <= 0) return;   // a deadline needs a calendar
    if (days < 1) days = 1;
    if (days > 3650) days = 3650;
    s_b.goal.start_day    = (uint16_t)s_cur_day;
    s_b.goal.target_day   = (uint16_t)(s_cur_day + days - 1);
    s_b.goal.start_mpages = s_b.read_mpages;
    s_b.goal.last_days    = (uint16_t)days;
    s_goal_seq++;
    persist();
}

void khatm_extend_goal(int days)
{
    if (s_b.goal.start_day == 0 || days <= 0) return;
    // Extending from TODAY, not from the old target: if the deadline slipped a
    // month ago, "+7 days" should mean seven days from now.
    int base = (int)s_b.goal.target_day;
    if (base < s_cur_day) base = s_cur_day;
    s_b.goal.target_day = (uint16_t)(base + days);
    s_goal_seq++;
    persist();
}

void khatm_clear_goal(void)
{
    memset(&s_b.goal, 0, sizeof s_b.goal);
    s_goal_seq++;
    persist();
}

// -------------------------------------------------------------------------
// Bulk edits
// -------------------------------------------------------------------------
void khatm_mark_pages(int from_page, int to_page)
{
    if (from_page < 1) from_page = 1;
    if (to_page > KHATM_TOTAL_PAGES) to_page = KHATM_TOTAL_PAGES;
    if (from_page > to_page) return;
    int g0 = qdb_page_first_global(from_page);
    int g1 = qdb_page_first_global(to_page) + qdb_page_ayah_count(to_page) - 1;
    for (int g = g0; g <= g1; g++) bit_set(g);
    recount();   // NOT credited to today: see khatm.h
    s_cov_seq++;
    persist();
}

void khatm_reset_coverage(void)
{
    memset(s_b.read, 0, sizeof s_b.read);
    s_b.read_count = 0;
    s_b.read_mpages = 0;
    // Keep days[], streak, best_streak, khatms_done and total_read_secs — the
    // habit carries across into the next khatm even though coverage restarts.
    if (s_b.goal.start_day && s_cur_day > 0) {
        int days = s_b.goal.last_days ? s_b.goal.last_days : 30;
        s_b.goal.start_day    = (uint16_t)s_cur_day;
        s_b.goal.target_day   = (uint16_t)(s_cur_day + days - 1);
        s_b.goal.start_mpages = 0;
    }
    memset(&s_focus, 0, sizeof s_focus);
    s_cov_seq++;
    s_goal_seq++;
    persist();
}

bool khatm_backfill_offered(void) { return (s_b.flags & FLAG_BACKFILL_OFFERED) != 0; }

void khatm_set_backfill_offered(void)
{
    s_b.flags |= FLAG_BACKFILL_OFFERED;
    persist();
}

// -------------------------------------------------------------------------
// Per-frame service: day rollover + throttled saves.
// -------------------------------------------------------------------------
void khatm_service(void)
{
    int day = today_index();

    if (day != s_cur_day) {
        int prev = s_cur_day;
        s_cur_day = day;

        if (day > 0) {
            if (s_b.first_seen_day == 0) s_b.first_seen_day = (uint16_t)day;
            // The clock just became known — move anything credited in the dark
            // into today rather than discarding it.
            if (s_b.pend_ayat || s_b.pend_mpages) {
                day_add(day, s_b.pend_ayat, s_b.pend_mpages);
                s_b.pend_ayat = 0;
                s_b.pend_mpages = 0;
            }
            if (prev > 0) {   // a real rollover, not the first sync
                int st = compute_streak();
                if (st > (int)s_b.best_streak) s_b.best_streak = (uint16_t)st;
            }
            persist();
        }
        s_stats_day = -1;   // invalidate the caches
        s_plan_day = -1;
    }

    if (s_dirty && plat_millis() - s_last_save >= SAVE_INTERVAL_MS) persist();
}
