// tafsirgame.c — card generation for the meaning game. See tafsirgame.h.
//
// Pure and stateless: no SD state, no clock, no globals. The only I/O is
// reading glosses through the caller's open WordMeaning, and because that has
// a single cache slot, EVERY string a card keeps is copied the instant it is
// read — first the target ayah (snap_ayah), then the distractor scan, which
// clobbers the cache freely since the snapshot already owns what it needs.
#include "tafsirgame.h"

#include <string.h>
#include <ctype.h>

#define GLOSS_CAP 64        // per-word gloss copy (clamped; glosses are short)
#define CAND_MAX  64        // distractor candidates gathered before sampling

// --- tiny reproducible PRNG (xorshift32) ----------------------------------
void tg_seed(uint32_t *s, uint32_t seed) { *s = seed ? seed : 0xA5A5A5A5u; }

uint32_t tg_rng(uint32_t *s)
{
    uint32_t x = *s ? *s : 0xA5A5A5A5u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

static int rnd(uint32_t *s, int n) { return n <= 0 ? 0 : (int)(tg_rng(s) % (uint32_t)n); }

// --- string helpers --------------------------------------------------------
static void copy_clamp(char *dst, const char *src, int cap)
{
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// Case-insensitive, whitespace-trimmed compare — so "the day" and "The Day "
// don't sneak in as a distractor for each other.
static void norm(const char *s, char *out, int cap)
{
    while (*s == ' ' || *s == '\t') s++;
    int n = 0;
    for (; s[n] && n < cap - 1; n++) out[n] = (char)tolower((unsigned char)s[n]);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) out[--n] = '\0';
}

static bool norm_eq(const char *a, const char *b)
{
    char na[GLOSS_CAP], nb[GLOSS_CAP];
    norm(a, na, sizeof na);
    norm(b, nb, sizeof nb);
    return strcmp(na, nb) == 0;
}

// --- arena (offset-keyed, deduped) ----------------------------------------
// Offset 0 is always "" so an unset slot reads empty. intern() returns -1 when
// the card's arena is full — the builder then bails and the ayah is skipped.
static int intern(TafsirCard *c, const char *s)
{
    if (!s || !s[0]) return 0;
    for (int off = 1; off < c->arena_len; ) {
        if (strcmp(c->arena + off, s) == 0) return off;
        off += (int)strlen(c->arena + off) + 1;
    }
    int len = (int)strlen(s) + 1;
    if (c->arena_len + len > TG_ARENA) return -1;
    int at = c->arena_len;
    memcpy(c->arena + at, s, (size_t)len);
    c->arena_len = (uint16_t)(c->arena_len + len);
    return at;
}

static void card_init(TafsirCard *c, TafsirCardKind kind, int surah, int ayah)
{
    memset(c, 0, sizeof *c);
    c->kind  = kind;
    c->surah = (uint16_t)surah;
    c->ayah  = (uint16_t)ayah;
    c->arena[0] = '\0';
    c->arena_len = 1;
}

const char *tg_choice(const TafsirCard *c, int i)
{
    if (i < 0 || i >= c->n_choices) return "";
    return c->arena + c->choice_off[i];
}

const char *tg_item(const TafsirCard *c, int i)
{
    if (i < 0 || i >= c->n_items) return "";
    return c->arena + c->item_off[i];
}

// --- target-ayah snapshot --------------------------------------------------
typedef struct {
    int  n_words;                    // drawn words in the ayah
    int  cap;                        // positions captured (min(n_words, MAX))
    bool truncated;                  // n_words > TG_MAX_ITEMS
    int  glossed;                    // non-empty among captured
    char g[TG_MAX_ITEMS][GLOSS_CAP]; // gloss per position ("" if unglossed)
} AyahSnap;

static void snap_ayah(TafsirSource *src, int ayah, AyahSnap *s)
{
    memset(s, 0, sizeof *s);
    s->n_words   = wordmeaning_word_count(src->wm, ayah);
    s->truncated = s->n_words > TG_MAX_ITEMS;
    s->cap       = s->truncated ? TG_MAX_ITEMS : s->n_words;
    for (int i = 0; i < s->cap; i++) {
        copy_clamp(s->g[i], wordmeaning_word(src->wm, ayah, i), GLOSS_CAP);
        if (s->g[i][0]) s->glossed++;
    }
}

// nth (0-based) glossed position within the snapshot, or -1.
static int nth_glossed(const AyahSnap *s, int n)
{
    for (int i = 0; i < s->cap; i++)
        if (s->g[i][0] && n-- == 0) return i;
    return -1;
}

// --- distractors -----------------------------------------------------------
// Gather up to CAND_MAX distinct glosses from across the surah (excluding any
// that normalise-equal `answer`), starting at a random ayah so we don't always
// mine the opening verses, then Fisher–Yates a `want`-sized sample out of them.
static int collect_candidates(TafsirSource *src, int ayah_count,
                              const char *answer, uint32_t *rng,
                              char cand[][GLOSS_CAP])
{
    int n = 0;
    int start = rnd(rng, ayah_count);
    for (int k = 0; k < ayah_count && n < CAND_MAX; k++) {
        int a = (start + k) % ayah_count + 1;
        int wc = wordmeaning_word_count(src->wm, a);
        for (int w = 0; w < wc && n < CAND_MAX; w++) {
            const char *g = wordmeaning_word(src->wm, a, w);
            if (!g[0] || norm_eq(g, answer)) continue;
            bool dup = false;
            for (int j = 0; j < n && !dup; j++) dup = norm_eq(cand[j], g);
            if (!dup) copy_clamp(cand[n++], g, GLOSS_CAP);
        }
    }
    return n;
}

static int pick_distractors(char cand[][GLOSS_CAP], int n, uint32_t *rng,
                            char out[][GLOSS_CAP], int want)
{
    int take = want < n ? want : n;
    for (int i = 0; i < take; i++) {
        int j = i + rnd(rng, n - i);
        char tmp[GLOSS_CAP];
        copy_clamp(tmp, cand[i], GLOSS_CAP);
        copy_clamp(cand[i], cand[j], GLOSS_CAP);
        copy_clamp(cand[j], tmp, GLOSS_CAP);
        copy_clamp(out[i], cand[i], GLOSS_CAP);
    }
    return take;
}

// Intern answer + distractors into `out`, shuffle their slots, record correct.
static bool lay_choices(TafsirCard *out, const char *answer,
                        char dis[][GLOSS_CAP], int nd, uint32_t *rng)
{
    const char *pool[TG_MAX_CHOICES];
    int m = nd + 1;
    pool[0] = answer;
    for (int i = 0; i < nd; i++) pool[i + 1] = dis[i];
    for (int i = m - 1; i > 0; i--) {
        int j = rnd(rng, i + 1);
        const char *t = pool[i]; pool[i] = pool[j]; pool[j] = t;
    }
    out->n_choices = (uint8_t)m;
    out->correct   = 0;
    for (int i = 0; i < m; i++) {
        int off = intern(out, pool[i]);
        if (off < 0) return false;
        out->choice_off[i] = (uint16_t)off;
        if (pool[i] == answer) out->correct = (uint8_t)i;   // address identity
    }
    return true;
}

// --- builders --------------------------------------------------------------
static bool make_quiz(TafsirSource *src, int ayah, int ayah_count,
                      const AyahSnap *snap, uint32_t *rng, TafsirCard *out)
{
    if (snap->glossed < 1) return false;
    int wpos = nth_glossed(snap, rnd(rng, snap->glossed));
    const char *answer = snap->g[wpos];

    char cand[CAND_MAX][GLOSS_CAP], dis[TG_MAX_CHOICES][GLOSS_CAP];
    int nc = collect_candidates(src, ayah_count, answer, rng, cand);
    if (nc < 1) return false;
    int nd = pick_distractors(cand, nc, rng, dis, TG_MAX_CHOICES - 1);

    card_init(out, TG_QUIZ, src->surah, ayah);
    out->n_words = (uint8_t)snap->n_words;
    out->word    = (uint8_t)wpos;
    return lay_choices(out, answer, dis, nd, rng);
}

static bool make_cloze(TafsirSource *src, int ayah, int ayah_count,
                       const AyahSnap *snap, uint32_t *rng, TafsirCard *out)
{
    // Need the whole sentence on screen and enough of it glossed to make sense.
    if (snap->truncated || snap->n_words < 2) return false;
    if (snap->glossed * 5 < snap->n_words * 3) return false;    // < 60% glossed

    int wpos = nth_glossed(snap, rnd(rng, snap->glossed));
    const char *answer = snap->g[wpos];

    char cand[CAND_MAX][GLOSS_CAP], dis[TG_MAX_CHOICES][GLOSS_CAP];
    int nc = collect_candidates(src, ayah_count, answer, rng, cand);
    if (nc < 1) return false;
    int nd = pick_distractors(cand, nc, rng, dis, TG_MAX_CHOICES - 1);

    card_init(out, TG_CLOZE, src->surah, ayah);
    out->n_words = (uint8_t)snap->n_words;
    out->word    = (uint8_t)wpos;
    if (!lay_choices(out, answer, dis, nd, rng)) return false;

    out->n_items = (uint8_t)snap->cap;
    for (int i = 0; i < snap->cap; i++) {
        const char *t = i == wpos ? "____" : (snap->g[i][0] ? snap->g[i] : "…");
        int off = intern(out, t);
        if (off < 0) return false;
        out->item_off[i] = (uint16_t)off;
        out->show[i] = (uint8_t)i;
    }
    return true;
}

static bool make_assemble(TafsirSource *src, int ayah, int ayah_count,
                          const AyahSnap *snap, uint32_t *rng, TafsirCard *out)
{
    (void)ayah_count;
    int pos[TG_MAX_ITEMS], k = 0;
    for (int i = 0; i < snap->cap; i++)
        if (snap->g[i][0]) pos[k++] = i;
    if (k < 3) return false;                 // 2 tiles is not a puzzle

    card_init(out, TG_ASSEMBLE, src->surah, ayah);
    out->n_words = (uint8_t)snap->n_words;
    out->n_items = (uint8_t)k;
    for (int j = 0; j < k; j++) {
        int off = intern(out, snap->g[pos[j]]);
        if (off < 0) return false;
        out->item_off[j] = (uint16_t)off;
        out->show[j] = (uint8_t)j;
    }
    // Shuffle the presentation order; force off the identity so slot != answer.
    for (int i = k - 1; i > 0; i--) {
        int j = rnd(rng, i + 1);
        uint8_t t = out->show[i]; out->show[i] = out->show[j]; out->show[j] = t;
    }
    bool ident = true;
    for (int j = 0; j < k && ident; j++) ident = out->show[j] == j;
    if (ident) {                              // rotate by one
        uint8_t first = out->show[0];
        for (int j = 0; j < k - 1; j++) out->show[j] = out->show[j + 1];
        out->show[k - 1] = first;
    }
    return true;
}

// --- public API ------------------------------------------------------------
bool tg_make_card(TafsirSource *src, int ayah, TafsirCardKind kind,
                  uint32_t *rng, TafsirCard *out)
{
    if (!src || !src->wm || !out) return false;
    int ayah_count = src->wm->n_ayat;
    if (ayah < 1 || ayah > ayah_count) return false;

    AyahSnap snap;
    snap_ayah(src, ayah, &snap);
    if (snap.glossed < 1) return false;       // nothing to ask about

    switch (kind) {
    case TG_QUIZ:     return make_quiz(src, ayah, ayah_count, &snap, rng, out);
    case TG_CLOZE:    return make_cloze(src, ayah, ayah_count, &snap, rng, out);
    case TG_ASSEMBLE: return make_assemble(src, ayah, ayah_count, &snap, rng, out);
    default:          return false;
    }
}

bool tg_make_any(TafsirSource *src, int ayah, uint32_t *rng, int spin,
                 TafsirCard *out)
{
    int start = ((spin % TG_KIND_COUNT) + TG_KIND_COUNT) % TG_KIND_COUNT;
    for (int i = 0; i < TG_KIND_COUNT; i++) {
        TafsirCardKind k = (TafsirCardKind)((start + i) % TG_KIND_COUNT);
        if (tg_make_card(src, ayah, k, rng, out)) return true;
    }
    return false;
}
