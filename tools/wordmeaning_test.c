// wordmeaning_test.c — the .qwm loader + font_wrap_lines, no device, no network.
//
// We can't run the quran.com fetch here, so the test SYNTHESIZES a .qwm (byte
// for byte, the format build_wordmeanings.py writes) to a temp file, then reads
// it back through the real loader: word counts, per-word glosses, an empty
// gloss, out-of-range indices, and the single-slot cache surviving an ayah
// switch. Then it checks font_wrap_lines keeps every line within the pixel
// budget and preserves word order.
#include "wordmeaning.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// --- HAL fs stub: map rel path under /tmp/wbwtest ---
struct HalFile { FILE *fp; };
static const char *BASE = "/tmp/wbwtest";

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

// --- synthesize a .qwm exactly as tools/build_wordmeanings.py would ---
static void wr_u16(FILE *f, unsigned v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void wr_u32(FILE *f, unsigned v)
{ for (int i = 0; i < 4; i++) fputc((v >> (8 * i)) & 0xff, f); }

static void write_qwm(int surah, int n_ayat, const char ***glosses, const int *nw)
{
    char dir[256]; snprintf(dir, sizeof dir, "%s/quran/wbw", BASE);
    // mkdir -p BASE/quran/wbw
    char step[256];
    snprintf(step, sizeof step, "%s", BASE);              mkdir(step, 0755);
    snprintf(step, sizeof step, "%s/quran", BASE);        mkdir(step, 0755);
    snprintf(step, sizeof step, "%s/quran/wbw", BASE);    mkdir(step, 0755);

    char path[256]; snprintf(path, sizeof path, "%s/%d.qwm", dir, surah);
    FILE *f = fopen(path, "wb");
    if (!f) { perror("write_qwm"); exit(2); }

    fwrite("QNWM", 1, 4, f);
    wr_u16(f, 1); wr_u16(f, surah); wr_u16(f, n_ayat); wr_u16(f, 0);

    // Pre-compute each ayah's blob so we know offsets/lengths.
    int base = 12 + n_ayat * 12;
    int off_acc = 0;
    int *blen = calloc(n_ayat, sizeof(int));
    for (int a = 0; a < n_ayat; a++) {
        int b = nw[a] * 2;
        for (int w = 0; w < nw[a]; w++) b += (int)strlen(glosses[a][w]) + 1;
        blen[a] = b;
    }
    // index
    for (int a = 0; a < n_ayat; a++) {
        wr_u16(f, a + 1); wr_u16(f, nw[a]);
        wr_u32(f, base + off_acc); wr_u32(f, blen[a]);
        off_acc += blen[a];
    }
    // blobs
    for (int a = 0; a < n_ayat; a++) {
        int pos = nw[a] * 2;
        for (int w = 0; w < nw[a]; w++) { wr_u16(f, pos); pos += (int)strlen(glosses[a][w]) + 1; }
        for (int w = 0; w < nw[a]; w++) fwrite(glosses[a][w], 1, strlen(glosses[a][w]) + 1, f);
    }
    free(blen);
    fclose(f);
}

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
#define STREQ(a, b) (strcmp((a), (b)) == 0)

int main(void)
{
    printf("-- .qwm loader round-trips counts, glosses, empties --\n");
    {
        const char *a1[] = { "In (the) name", "(of) Allah", "the Most Merciful" };
        const char *a2[] = { "Praise", "be", "to Allah", "" };   // last unglossed
        const char **g[] = { a1, a2 };
        int nw[] = { 3, 4 };
        write_qwm(207, 2, g, nw);

        WordMeaning wm;
        CHECK(wordmeaning_open(&wm, 207), "open failed");
        CHECK(wordmeaning_word_count(&wm, 1) == 3, "ayah1 count %d",
              wordmeaning_word_count(&wm, 1));
        CHECK(wordmeaning_word_count(&wm, 2) == 4, "ayah2 count %d",
              wordmeaning_word_count(&wm, 2));
        CHECK(STREQ(wordmeaning_word(&wm, 1, 0), "In (the) name"), "1:0 '%s'",
              wordmeaning_word(&wm, 1, 0));
        CHECK(STREQ(wordmeaning_word(&wm, 1, 2), "the Most Merciful"), "1:2 '%s'",
              wordmeaning_word(&wm, 1, 2));
        // switch ayah (single cache slot), then a word, then back
        CHECK(STREQ(wordmeaning_word(&wm, 2, 2), "to Allah"), "2:2 '%s'",
              wordmeaning_word(&wm, 2, 2));
        CHECK(STREQ(wordmeaning_word(&wm, 2, 3), ""), "2:3 empty '%s'",
              wordmeaning_word(&wm, 2, 3));
        CHECK(STREQ(wordmeaning_word(&wm, 1, 1), "(of) Allah"), "1:1 after switch '%s'",
              wordmeaning_word(&wm, 1, 1));
        // out of range / missing ayah -> ""
        CHECK(STREQ(wordmeaning_word(&wm, 1, 9), ""), "oob word not empty");
        CHECK(STREQ(wordmeaning_word(&wm, 5, 0), ""), "missing ayah not empty");
        CHECK(wordmeaning_word_count(&wm, 9) == 0, "missing ayah count != 0");
        wordmeaning_close(&wm);
    }

    printf("-- missing surah opens false, degrades gracefully --\n");
    {
        WordMeaning wm;
        CHECK(!wordmeaning_open(&wm, 199), "missing surah opened true");
        // functions on a closed loader must be safe and empty
        CHECK(wordmeaning_word_count(&wm, 1) == 0, "closed count != 0");
        CHECK(STREQ(wordmeaning_word(&wm, 1, 0), ""), "closed word != empty");
    }

    printf("-- font_wrap_lines keeps lines within budget, order intact --\n");
    {
        const char *txt = "In the name of Allah the Most Gracious the Most Merciful";
        int max_w = 90;   // px
        char lines[FONT_WRAP_LINE_CAP][FONT_WRAP_LINE_CAP];
        int n = font_wrap_lines(&font_tiny, txt, max_w,
                                (char (*)[FONT_WRAP_LINE_CAP])lines, 6);
        CHECK(n >= 2, "expected wrapping into >=2 lines, got %d", n);
        char joined[256] = "";
        for (int i = 0; i < n; i++) {
            CHECK(font_string_width(&font_tiny, lines[i]) <= max_w,
                  "line %d '%s' width %d > %d", i, lines[i],
                  font_string_width(&font_tiny, lines[i]), max_w);
            if (i) strcat(joined, " ");
            strcat(joined, lines[i]);
        }
        CHECK(STREQ(joined, txt), "reassembled '%s' != original", joined);
    }

    printf("-- font_wrap_lines hard-clamps an over-long word (no overflow) --\n");
    {
        const char *txt = "supercalifragilisticexpialidocious";
        int max_w = 40;
        char lines[FONT_WRAP_LINE_CAP][FONT_WRAP_LINE_CAP];
        int n = font_wrap_lines(&font_tiny, txt, max_w,
                                (char (*)[FONT_WRAP_LINE_CAP])lines, 6);
        CHECK(n >= 1, "no lines");
        for (int i = 0; i < n; i++)
            CHECK(font_string_width(&font_tiny, lines[i]) <= max_w,
                  "clamped line %d width %d > %d", i,
                  font_string_width(&font_tiny, lines[i]), max_w);
    }

    if (fails) { printf("\nwbw-test: %d checks FAILED\n", fails); return 1; }
    printf("\nwbw-test: all checks passed\n");
    return 0;
}
