// tafsir_test.c — the Tafsir Game card generator, headless and deterministic.
//
// There's no quran.com fetch here (same as wbw-test), so we SYNTHESIZE a .qwm
// with hand-written glosses, read it back through the real WordMeaning loader,
// and drive tg_make_card()/tg_make_any() over it. Card generation is the
// riskiest part of the feature: if the glosses can't yield fair puzzles the
// whole game idea is dead, so this pins every invariant a card must hold and
// then prints a few sample cards for the eyeball test.
#include "tafsirgame.h"
#include "wordmeaning.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

// --- HAL fs stub: map rel path under /tmp/tgtest (mirrors wordmeaning_test) ---
struct HalFile { FILE *fp; };
static const char *BASE = "/tmp/tgtest";

struct HalFile *hal_fs_open(const char *rel)
{
    char p[256]; snprintf(p, sizeof p, "%s/%s", BASE, rel);
    FILE *fp = fopen(p, "rb");
    if (!fp) return NULL;
    struct HalFile *f = calloc(1, sizeof *f); f->fp = fp; return f;
}
int hal_fs_pread(struct HalFile *f, void *buf, size_t len, size_t off)
{
    if (!f || fseek(f->fp, (long)off, SEEK_SET) != 0) return -1;
    return (int)fread(buf, 1, len, f->fp);
}
void hal_fs_close(struct HalFile *f) { if (f) { fclose(f->fp); free(f); } }

// --- synthesize a .qwm exactly as tools/build_wordmeanings.py would --------
static void wr_u16(FILE *f, unsigned v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void wr_u32(FILE *f, unsigned v)
{ for (int i = 0; i < 4; i++) fputc((v >> (8 * i)) & 0xff, f); }

static void write_qwm(int surah, int n_ayat, const char ***glosses, const int *nw)
{
    char dir[256]; snprintf(dir, sizeof dir, "%s/quran/wbw", BASE);
    char step[256];
    snprintf(step, sizeof step, "%s", BASE);           mkdir(step, 0755);
    snprintf(step, sizeof step, "%s/quran", BASE);      mkdir(step, 0755);
    snprintf(step, sizeof step, "%s/quran/wbw", BASE);  mkdir(step, 0755);

    char path[256]; snprintf(path, sizeof path, "%s/%d.qwm", dir, surah);
    FILE *f = fopen(path, "wb");
    if (!f) { perror("write_qwm"); exit(2); }

    fwrite("QNWM", 1, 4, f);
    wr_u16(f, 1); wr_u16(f, surah); wr_u16(f, n_ayat); wr_u16(f, 0);

    int base = 12 + n_ayat * 12, off_acc = 0;
    int *blen = calloc(n_ayat, sizeof(int));
    for (int a = 0; a < n_ayat; a++) {
        int b = nw[a] * 2;
        for (int w = 0; w < nw[a]; w++) b += (int)strlen(glosses[a][w]) + 1;
        blen[a] = b;
    }
    for (int a = 0; a < n_ayat; a++) {
        wr_u16(f, a + 1); wr_u16(f, nw[a]);
        wr_u32(f, base + off_acc); wr_u32(f, blen[a]);
        off_acc += blen[a];
    }
    for (int a = 0; a < n_ayat; a++) {
        int pos = nw[a] * 2;
        for (int w = 0; w < nw[a]; w++) { wr_u16(f, pos); pos += (int)strlen(glosses[a][w]) + 1; }
        for (int w = 0; w < nw[a]; w++) fwrite(glosses[a][w], 1, strlen(glosses[a][w]) + 1, f);
    }
    free(blen);
    fclose(f);
}

// --- the synthetic surah (kept here so the test can check answers) ---------
#define SUR 207
static const char *A1[] = { "In (the) name", "(of) Allah", "the Most Gracious", "the Most Merciful" };
static const char *A2[] = { "All praise", "(is) for Allah", "Lord", "(of) the worlds" };
static const char *A3[] = { "The Most Gracious", "the Most Merciful" };
static const char *A4[] = { "Master", "(of the) Day", "(of the) Judgment" };
static const char *A5[] = { "You Alone", "we worship", "and You Alone", "we ask for help" };
static const char *A6[] = { "Guide us", "(to) the path", "the straight" };
static const char *A7[] = { "", "", "" };                       // wholly unglossed
static const char *A8[] = { "path", "(of) those", "You have blessed" };
static const char **GL[] = { A1, A2, A3, A4, A5, A6, A7, A8 };
static const int    NW[] = {  4,  4,  2,  3,  4,  3,  3,  3 };
#define NAYAT 8

static const char *gloss_of(int ayah, int word) { return GL[ayah - 1][word]; }
static int glossed_count(int ayah)
{
    int n = 0;
    for (int w = 0; w < NW[ayah - 1]; w++) if (GL[ayah - 1][w][0]) n++;
    return n;
}

// --- checks ----------------------------------------------------------------
static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool ci_eq(const char *a, const char *b)
{
    while (*a && *b) { if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return false; }
    return *a == *b;
}

static void check_choices_sane(const TafsirCard *c)
{
    CHECK(c->n_choices >= 2 && c->n_choices <= TG_MAX_CHOICES,
          "n_choices %d out of range", c->n_choices);
    CHECK(c->correct < c->n_choices, "correct %d >= n_choices %d", c->correct, c->n_choices);
    for (int i = 0; i < c->n_choices; i++) {
        CHECK(tg_choice(c, i)[0], "choice %d empty", i);
        for (int j = i + 1; j < c->n_choices; j++)
            CHECK(!ci_eq(tg_choice(c, i), tg_choice(c, j)),
                  "choices %d,%d duplicate: '%s'", i, j, tg_choice(c, i));
    }
}

static void check_quiz(const TafsirCard *c)
{
    CHECK(c->kind == TG_QUIZ, "not a quiz");
    check_choices_sane(c);
    CHECK(ci_eq(tg_choice(c, c->correct), gloss_of(c->ayah, c->word)),
          "quiz answer '%s' != word gloss '%s'",
          tg_choice(c, c->correct), gloss_of(c->ayah, c->word));
}

static void check_cloze(const TafsirCard *c)
{
    CHECK(c->kind == TG_CLOZE, "not a cloze");
    check_choices_sane(c);
    CHECK(c->n_items == NW[c->ayah - 1], "cloze n_items %d != word count %d",
          c->n_items, NW[c->ayah - 1]);
    int blanks = 0, blank_at = -1;
    for (int i = 0; i < c->n_items; i++)
        if (strcmp(tg_item(c, i), "____") == 0) { blanks++; blank_at = i; }
    CHECK(blanks == 1, "cloze has %d blanks (want 1)", blanks);
    CHECK(blank_at == c->word, "blank at item %d but card->word %d", blank_at, c->word);
    CHECK(ci_eq(tg_choice(c, c->correct), gloss_of(c->ayah, c->word)),
          "cloze answer '%s' != blanked gloss '%s'",
          tg_choice(c, c->correct), gloss_of(c->ayah, c->word));
}

static void check_assemble(const TafsirCard *c)
{
    CHECK(c->kind == TG_ASSEMBLE, "not an assemble");
    CHECK(c->n_items == glossed_count(c->ayah), "assemble n_items %d != glossed %d",
          c->n_items, glossed_count(c->ayah));
    CHECK(c->n_items >= 3, "assemble too small: %d", c->n_items);
    // items are the glossed words in reading order
    int gw = 0;
    for (int w = 0; w < NW[c->ayah - 1]; w++) {
        if (!gloss_of(c->ayah, w)[0]) continue;
        CHECK(ci_eq(tg_item(c, gw), gloss_of(c->ayah, w)),
              "assemble item %d '%s' != '%s'", gw, tg_item(c, gw), gloss_of(c->ayah, w));
        gw++;
    }
    // show[] is a permutation of 0..n-1, and not the identity
    int seen[TG_MAX_ITEMS] = {0}; bool ident = true;
    for (int i = 0; i < c->n_items; i++) {
        CHECK(c->show[i] < c->n_items, "show[%d]=%d out of range", i, c->show[i]);
        seen[c->show[i]]++;
        if (c->show[i] != i) ident = false;
    }
    for (int i = 0; i < c->n_items; i++)
        CHECK(seen[i] == 1, "show[] not a permutation: value %d seen %d times", i, seen[i]);
    CHECK(!ident, "assemble presentation order is the identity (no puzzle)");
}

// --- pretty print for the eyeball test -------------------------------------
static void print_card(const TafsirCard *c)
{
    static const char *KN[] = { "QUIZ", "CLOZE", "ASSEMBLE" };
    printf("  [%s] %d:%d\n", KN[c->kind], c->surah, c->ayah);
    if (c->kind == TG_QUIZ) {
        printf("    word #%d — pick its meaning:\n", c->word + 1);
        for (int i = 0; i < c->n_choices; i++)
            printf("      %c %s\n", i == c->correct ? '*' : ' ', tg_choice(c, i));
    } else if (c->kind == TG_CLOZE) {
        printf("    ");
        for (int i = 0; i < c->n_items; i++) printf("%s ", tg_item(c, i));
        printf("\n    fill the blank:\n");
        for (int i = 0; i < c->n_choices; i++)
            printf("      %c %s\n", i == c->correct ? '*' : ' ', tg_choice(c, i));
    } else {
        printf("    shown shuffled — put in order:\n");
        for (int p = 0; p < c->n_items; p++)
            printf("      slot %d: %s\n", p + 1, tg_item(c, c->show[p]));
        printf("    (correct order: ");
        for (int j = 0; j < c->n_items; j++) printf("%s / ", tg_item(c, j));
        printf(")\n");
    }
}

int main(void)
{
    write_qwm(SUR, NAYAT, GL, NW);

    WordMeaning wm;
    if (!wordmeaning_open(&wm, SUR)) { printf("FAIL: could not open synth .qwm\n"); return 1; }
    TafsirSource src = { .wm = &wm, .surah = SUR };

    printf("-- each kind holds its invariants across many seeds --\n");
    for (uint32_t seed = 1; seed <= 200; seed++) {
        for (int ayah = 1; ayah <= NAYAT; ayah++) {
            uint32_t rng;
            TafsirCard c;
            tg_seed(&rng, seed * 131 + ayah);
            if (tg_make_card(&src, ayah, TG_QUIZ, &rng, &c)) check_quiz(&c);
            tg_seed(&rng, seed * 977 + ayah);
            if (tg_make_card(&src, ayah, TG_CLOZE, &rng, &c)) check_cloze(&c);
            tg_seed(&rng, seed * 613 + ayah);
            if (tg_make_card(&src, ayah, TG_ASSEMBLE, &rng, &c)) check_assemble(&c);
        }
    }

    printf("-- fair-card refusals (graceful skip, never a broken card) --\n");
    {
        uint32_t rng; TafsirCard c;
        // ayah 7 is wholly unglossed -> no kind can be built
        tg_seed(&rng, 7);
        CHECK(!tg_make_card(&src, 7, TG_QUIZ, &rng, &c),  "quiz built on unglossed ayah");
        CHECK(!tg_make_card(&src, 7, TG_CLOZE, &rng, &c), "cloze built on unglossed ayah");
        CHECK(!tg_make_card(&src, 7, TG_ASSEMBLE, &rng, &c), "assemble built on unglossed ayah");
        CHECK(!tg_make_any(&src, 7, &rng, 0, &c), "tg_make_any built on unglossed ayah");
        // ayah 3 has only 2 glossed words -> assemble refuses, quiz/cloze work
        tg_seed(&rng, 3);
        CHECK(!tg_make_card(&src, 3, TG_ASSEMBLE, &rng, &c), "assemble built on 2-word ayah");
        CHECK(tg_make_card(&src, 3, TG_QUIZ, &rng, &c), "quiz refused on 2-word ayah");
        // out-of-range ayah
        CHECK(!tg_make_card(&src, 99, TG_QUIZ, &rng, &c), "quiz built on missing ayah");
    }

    printf("-- tg_make_any rotates the kind by spin --\n");
    {
        // ayah 1 supports all three kinds, so spin picks the leading kind.
        TafsirCardKind want[] = { TG_QUIZ, TG_CLOZE, TG_ASSEMBLE };
        for (int spin = 0; spin < 3; spin++) {
            uint32_t rng; TafsirCard c;
            tg_seed(&rng, 42 + spin);
            CHECK(tg_make_any(&src, 1, &rng, spin, &c), "make_any failed on rich ayah");
            CHECK(c.kind == want[spin], "spin %d -> kind %d (want %d)", spin, c.kind, want[spin]);
        }
    }

    printf("\n-- sample cards (eyeball the quality) --\n");
    {
        uint32_t rng; TafsirCard c;
        tg_seed(&rng, 12345);
        if (tg_make_card(&src, 1, TG_QUIZ, &rng, &c))     print_card(&c);
        if (tg_make_card(&src, 2, TG_CLOZE, &rng, &c))    print_card(&c);
        if (tg_make_card(&src, 5, TG_ASSEMBLE, &rng, &c)) print_card(&c);
        if (tg_make_card(&src, 4, TG_QUIZ, &rng, &c))     print_card(&c);
    }

    wordmeaning_close(&wm);
    if (fails) { printf("\ntafsir-test: %d checks FAILED\n", fails); return 1; }
    printf("\ntafsir-test: all checks passed\n");
    return 0;
}
