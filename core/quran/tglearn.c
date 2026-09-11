// tglearn.c — Tafsir Game learner: scope, Leitner scheduling, persistence.
// See tglearn.h. Structure and the persistence idioms mirror khatm.c/hifz.c so
// the three read the same way.
#include "tglearn.h"
#include "qday.h"
#include "quran_db.h"
#include "hal.h"
#include "plat.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define TGL_MAGIC   0x314C4754u   // "TGL1"
#define TGL_VERSION 1
#define SAVE_DEBOUNCE_MS 5000

// Leitner ladder: box 0 is due the same day (relearn), each step ~2x the last.
const uint16_t TGL_BOX_DAYS[] = { 0, 1, 3, 7, 16, 35 };
const int      TGL_BOX_COUNT  = (int)(sizeof TGL_BOX_DAYS / sizeof TGL_BOX_DAYS[0]);
#define TOP_BOX (TGL_BOX_COUNT - 1)

// -------------------------------------------------------------------------
// Persisted blob. Header (magic..crc) then body; the CRC covers [scope..bytes)
// and the reserved tail is append-only — an all-zero tail is a valid default,
// so a blob from an older/smaller build upgrades cleanly on load.
// -------------------------------------------------------------------------
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;             // sizeof(TgBlob) as written
    uint32_t seq;               // higher wins when both slots are valid
    uint32_t crc;               // over [offsetof(scope) .. bytes)

    TgScope  scope;
    uint16_t frontier;          // ordinal of the next unstarted ayah in scope
    uint16_t n_items;
    uint16_t ord;               // monotonic session ordinal (no-clock ordering)
    uint8_t  new_per_day;       // 0 => TGL_DEF_NEW_PER_DAY
    uint8_t  pad0;
    uint16_t first_seen_day;
    uint16_t pad1;
    uint32_t answered, correct;

    TgItem   items[TGL_MAX_ITEMS];

    uint32_t reserved[8];       // append here; zero means default
} TgBlob;

static TgBlob   s_b;
static bool     s_dirty;
static uint32_t s_last_save;
static int      s_cur_day;
static uint32_t s_state_seq = 1;

#define BODY_OFF offsetof(TgBlob, scope)

// -------------------------------------------------------------------------
// Small helpers
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

static void mark_dirty(void) { s_dirty = true; s_state_seq++; }

static int find_item(int g)
{
    for (int i = 0; i < s_b.n_items; i++)
        if (s_b.items[i].g == (uint16_t)g) return i;
    return -1;
}

static int scope_total(void)
{
    return s_b.scope.first_g ? (s_b.scope.last_g - s_b.scope.first_g + 1) : 0;
}

static bool in_scope(int g)
{
    return s_b.scope.first_g && g >= s_b.scope.first_g && g <= s_b.scope.last_g;
}

// Set an item's next-review day from an interval, honouring the no-clock rule.
static void set_due(TgItem *it, int days)
{
    if (s_cur_day <= 0) { it->due_day = 0; return; }   // due whenever
    int d = s_cur_day + (days < 0 ? 0 : days);
    if (d < s_cur_day) d = s_cur_day;
    it->due_day = (uint16_t)(d > 65535 ? 65535 : d);
}

static bool is_due(const TgItem *it)
{
    if (s_cur_day <= 0) return true;                   // no clock: all eligible
    return it->due_day == 0 || (int)it->due_day <= s_cur_day;
}

static int overdue_days(const TgItem *it)
{
    if (s_cur_day <= 0 || it->due_day == 0) return 0;
    int od = s_cur_day - (int)it->due_day;
    return od > 0 ? od : 0;
}

// -------------------------------------------------------------------------
// Persistence (dual-slot + CRC, mirrors khatm.c)
// -------------------------------------------------------------------------
typedef struct { uint32_t magic; uint16_t version, bytes; uint32_t seq; } SlotHdr;

static bool slot_seq(const char *name, uint32_t *out)
{
    SlotHdr h;
    size_t got = 0;
    if (!hal_state_load(name, &h, sizeof h, &got)) return false;
    if (got < sizeof h || h.magic != TGL_MAGIC) return false;
    *out = h.seq;
    return true;
}

static bool load_slot(const char *name, TgBlob *dst)
{
    memset(dst, 0, sizeof *dst);
    size_t got = 0;
    if (!hal_state_load(name, dst, sizeof *dst, &got)) return false;
    if (got < BODY_OFF + sizeof dst->scope) return false;
    if (dst->magic != TGL_MAGIC) return false;
    if (dst->bytes < BODY_OFF || dst->bytes > got) return false;
    if (crc32((const uint8_t *)dst + BODY_OFF, dst->bytes - BODY_OFF) != dst->crc)
        return false;
    return true;   // tail beyond dst->bytes stayed zeroed => older blobs upgrade
}

static void persist(void)
{
    s_b.magic   = TGL_MAGIC;
    s_b.version = TGL_VERSION;
    s_b.bytes   = (uint16_t)sizeof s_b;
    s_b.seq++;
    s_b.crc = crc32((const uint8_t *)&s_b + BODY_OFF, s_b.bytes - BODY_OFF);
    hal_state_save((s_b.seq & 1) ? "tglearn.a" : "tglearn.b", &s_b, sizeof s_b);
    s_dirty = false;
    s_last_save = plat_millis();
}

void tglearn_flush(void) { if (s_dirty) persist(); }

void tglearn_init(void)
{
    uint32_t sa = 0, sb = 0;
    bool ha = slot_seq("tglearn.a", &sa), hb = slot_seq("tglearn.b", &sb);
    const char *first = NULL, *second = NULL;
    if (ha && hb) {
        first  = (sa >= sb) ? "tglearn.a" : "tglearn.b";
        second = (sa >= sb) ? "tglearn.b" : "tglearn.a";
    } else if (ha) first = "tglearn.a";
    else if (hb)   first = "tglearn.b";

    bool ok = first && load_slot(first, &s_b);
    if (!ok && second) ok = load_slot(second, &s_b);
    if (!ok) {
        memset(&s_b, 0, sizeof s_b);
        s_b.magic = TGL_MAGIC;
        s_b.version = TGL_VERSION;
    }

    s_cur_day = qday_today();
    if (s_b.first_seen_day == 0 && s_cur_day > 0)
        s_b.first_seen_day = (uint16_t)s_cur_day;

    s_state_seq++;
    s_last_save = plat_millis();
}

void tglearn_service(void)
{
    int day = qday_today();
    if (day != s_cur_day) { s_cur_day = day; s_state_seq++; }   // rollover
    if (s_dirty && plat_millis() - s_last_save >= SAVE_DEBOUNCE_MS) persist();
}

// -------------------------------------------------------------------------
// Target
// -------------------------------------------------------------------------
void tglearn_set_scope(TgScopeKind kind, int arg, bool reverse)
{
    TgScope sc;
    memset(&sc, 0, sizeof sc);
    sc.kind = (uint8_t)kind;
    sc.reverse = reverse ? 1 : 0;
    sc.label_arg = (uint16_t)arg;

    if (kind == TGL_SCOPE_SURAH) {
        int n = qdb_ayah_count(arg);
        if (n <= 0) return;
        sc.first_g = (uint16_t)qdb_global_index(arg, 1);
        sc.last_g  = (uint16_t)(sc.first_g + n - 1);
    } else if (kind == TGL_SCOPE_JUZ) {
        if (arg < 1 || arg > QDB_JUZ_COUNT) return;
        QRef s = qdb_juz_start(arg);
        sc.first_g = (uint16_t)qdb_global_index(s.surah, s.ayah);
        if (arg == QDB_JUZ_COUNT) sc.last_g = TGL_TOTAL_AYAT;
        else { QRef e = qdb_juz_start(arg + 1);
               sc.last_g = (uint16_t)(qdb_global_index(e.surah, e.ayah) - 1); }
    } else if (kind == TGL_SCOPE_QURAN) {
        sc.first_g = 1;
        sc.last_g  = TGL_TOTAL_AYAT;
    } else {
        memset(&s_b.scope, 0, sizeof s_b.scope);
        s_b.frontier = 0;
        mark_dirty(); tglearn_flush();
        return;
    }

    s_b.scope = sc;
    s_b.frontier = 0;   // re-derive on the next introduction (skips started ayat)
    mark_dirty();
    tglearn_flush();
}

TgScope tglearn_scope(void) { return s_b.scope; }
void tglearn_clear_scope(void) { tglearn_set_scope(TGL_SCOPE_NONE, 0, false); }

void tglearn_scope_label(char *buf, int n)
{
    switch (s_b.scope.kind) {
    case TGL_SCOPE_SURAH: snprintf(buf, n, "%s", qdb_surah_name(s_b.scope.label_arg)); break;
    case TGL_SCOPE_JUZ:   snprintf(buf, n, "Juz %d", s_b.scope.label_arg); break;
    case TGL_SCOPE_QURAN: snprintf(buf, n, "Whole Quran"); break;
    default:              snprintf(buf, n, "No target"); break;
    }
}

// -------------------------------------------------------------------------
// Study flow
// -------------------------------------------------------------------------
static TgItem *add_item(int g)
{
    if (s_b.n_items >= TGL_MAX_ITEMS) return NULL;
    TgItem *it = &s_b.items[s_b.n_items++];
    memset(it, 0, sizeof *it);
    it->g = (uint16_t)g;
    it->last_ord = ++s_b.ord;
    if (s_cur_day > 0 && s_b.first_seen_day == 0)
        s_b.first_seen_day = (uint16_t)s_cur_day;
    return it;
}

int tglearn_start_new(void)
{
    int total = scope_total();
    if (total <= 0) return 0;
    while (s_b.frontier < total) {
        int ord = s_b.frontier;
        int g = s_b.scope.reverse ? s_b.scope.last_g - ord : s_b.scope.first_g + ord;
        s_b.frontier++;
        if (find_item(g) >= 0) continue;          // already studying it
        TgItem *it = add_item(g);
        if (!it) { s_b.frontier--; return 0; }     // table full; retry this ord later
        it->box = 0;
        set_due(it, 0);                            // due now
        mark_dirty();
        return g;
    }
    return 0;
}

void tglearn_grade(int surah, int ayah, TgGrade g)
{
    int gi = qdb_global_index(surah, ayah);
    if (gi <= 0) return;
    int idx = find_item(gi);
    TgItem *it = idx < 0 ? add_item(gi) : &s_b.items[idx];
    if (!it) return;

    switch (g) {
    case TG_GOT:   if (it->box < TOP_BOX) it->box++;
                   if (it->streak < 255)  it->streak++;
                   break;
    case TG_HARD:  it->streak = 0;                     // hold the interval
                   break;
    case TG_WRONG: it->box = 0; it->streak = 0;
                   if (it->lapses < 255) it->lapses++;
                   break;
    }
    if (s_cur_day > 0) it->last_day = (uint16_t)s_cur_day;
    it->last_ord = ++s_b.ord;
    set_due(it, TGL_BOX_DAYS[it->box]);

    s_b.answered++;
    if (g == TG_GOT) s_b.correct++;
    mark_dirty();
}

// -------------------------------------------------------------------------
// Queues
// -------------------------------------------------------------------------
// Order two due items: most overdue first, then weakest box, then oldest seen.
static int cmp_due(const TgItem *a, const TgItem *b)
{
    int oa = overdue_days(a), ob = overdue_days(b);
    if (oa != ob) return ob - oa;
    if (a->box != b->box) return a->box - b->box;
    return (int)a->last_ord - (int)b->last_ord;
}

int tglearn_today(int *out_g, int max)
{
    int idx[TGL_MAX_ITEMS], nd = 0;
    for (int i = 0; i < s_b.n_items; i++)
        if (is_due(&s_b.items[i])) idx[nd++] = i;

    // insertion sort (nd <= 512, runs rarely)
    for (int i = 1; i < nd; i++) {
        int v = idx[i], j = i - 1;
        while (j >= 0 && cmp_due(&s_b.items[idx[j]], &s_b.items[v]) > 0) {
            idx[j + 1] = idx[j]; j--;
        }
        idx[j + 1] = v;
    }

    int n = nd < max ? nd : max;
    for (int i = 0; i < n; i++) out_g[i] = s_b.items[idx[i]].g;
    return n;
}

int tglearn_due_count(void)
{
    int n = 0;
    for (int i = 0; i < s_b.n_items; i++) if (is_due(&s_b.items[i])) n++;
    return n;
}

int tglearn_new_available(void)
{
    int total = scope_total();
    if (total <= 0) return 0;
    int started_in = 0;
    for (int i = 0; i < s_b.n_items; i++)
        if (in_scope(s_b.items[i].g)) started_in++;
    int left = total - started_in;
    return left > 0 ? left : 0;
}

// -------------------------------------------------------------------------
// Introspection
// -------------------------------------------------------------------------
int tglearn_item_count(void) { return s_b.n_items; }

const TgItem *tglearn_item(int i)
{
    if (i < 0 || i >= s_b.n_items) return NULL;
    return &s_b.items[i];
}

int tglearn_item_at(int surah, int ayah)
{
    int gi = qdb_global_index(surah, ayah);
    return gi <= 0 ? -1 : find_item(gi);
}

int tglearn_box(int surah, int ayah)
{
    int idx = tglearn_item_at(surah, ayah);
    return idx < 0 ? -1 : s_b.items[idx].box;
}

const TgStats *tglearn_stats(void)
{
    static TgStats st;
    memset(&st, 0, sizeof st);
    st.started     = s_b.n_items;
    st.scope_total = scope_total();
    st.have_day    = s_cur_day > 0;
    st.day         = s_cur_day;
    st.answered    = s_b.answered;
    st.correct     = s_b.correct;
    for (int i = 0; i < s_b.n_items; i++) {
        const TgItem *it = &s_b.items[i];
        if (it->box >= TOP_BOX) st.mastered++;
        if (is_due(it))         st.due++;
        if (in_scope(it->g))    st.scope_done++;
    }
    return &st;
}

uint32_t tglearn_state_seq(void) { return s_state_seq; }

int tglearn_new_per_day(void)
{
    return s_b.new_per_day ? s_b.new_per_day : TGL_DEF_NEW_PER_DAY;
}

void tglearn_set_new_per_day(int n)
{
    if (n < 1) n = 1;
    if (n > 255) n = 255;
    s_b.new_per_day = (uint8_t)n;
    mark_dirty();
}
