#include "timing.h"
#include "plat.h"
#include "hal.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "TIMING";

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

bool timing_open(TimingTable *t, int surah)
{
    memset(t, 0, sizeof(*t));
    char path[64];
    snprintf(path, sizeof(path), "quran/timings/%d.qtm", surah);
    if (!hal_fs_slurp(path, &t->data, &t->len)) return false;
    if (t->len < 12 || memcmp(t->data, "QNTM", 4) != 0) {
        QN_LOGE(TAG, "bad timing header: %s", path);
        free(t->data); t->data = NULL; return false;
    }
    t->surah = rd16(t->data + 6);
    t->n_ayat = rd16(t->data + 8);
    t->idx = calloc(t->n_ayat, sizeof(*t->idx));

    size_t off = 12;
    for (int i = 0; i < t->n_ayat; i++) {
        if (off + 4 > t->len) { QN_LOGE(TAG, "truncated"); break; }
        uint16_t ayah = rd16(t->data + off);
        uint16_t nw = rd16(t->data + off + 2);
        off += 4;
        t->idx[i].ayah = ayah;
        t->idx[i].n_words = nw;
        t->idx[i].words_off = (uint32_t)off;
        off += (size_t)nw * 8;   // n_words x (u32,u32)
    }
    QN_LOGI(TAG, "timings surah %d: %d ayat", t->surah, t->n_ayat);
    return true;
}

void timing_close(TimingTable *t)
{
    if (t->idx) free(t->idx);
    if (t->data) free(t->data);
    memset(t, 0, sizeof(*t));
}

static int find_ayah(const TimingTable *t, int ayah)
{
    for (int i = 0; i < t->n_ayat; i++)
        if (t->idx[i].ayah == ayah) return i;
    return -1;
}

int timing_word_count(const TimingTable *t, int ayah)
{
    if (!t->data) return 0;
    int i = find_ayah(t, ayah);
    return i < 0 ? 0 : t->idx[i].n_words;
}

bool timing_word(const TimingTable *t, int ayah, int word, WordTiming *out)
{
    if (!t->data) return false;
    int i = find_ayah(t, ayah);
    if (i < 0 || word < 0 || word >= t->idx[i].n_words) return false;
    const uint8_t *p = t->data + t->idx[i].words_off + (size_t)word * 8;
    out->start_ms = rd32(p);
    out->end_ms = rd32(p + 4);
    return true;
}

int timing_active_word(const TimingTable *t, int ayah, uint32_t pos_ms)
{
    if (!t->data) return -1;
    int i = find_ayah(t, ayah);
    if (i < 0 || t->idx[i].n_words == 0) return -1;
    int n = t->idx[i].n_words;
    const uint8_t *base = t->data + t->idx[i].words_off;

    // Before the first word starts.
    if (pos_ms < rd32(base)) return -1;
    for (int w = 0; w < n; w++) {
        uint32_t s = rd32(base + (size_t)w * 8);
        uint32_t e = rd32(base + (size_t)w * 8 + 4);
        if (pos_ms >= s && pos_ms < e) return w;
    }
    // Past the last word's end: keep it highlighted until the clip finishes.
    return n - 1;
}
