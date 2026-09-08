#include "wordmeaning.h"
#include "hal.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WBW";

#define WBW_IDX_STRIDE 12   // u16 ayah, u16 n_words, u32 blob_off, u32 blob_len

static inline uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

bool wordmeaning_open(WordMeaning *wm, int surah)
{
    memset(wm, 0, sizeof(*wm));
    wm->cache_ayah = -1;

    char rel[48];
    snprintf(rel, sizeof(rel), "quran/wbw/%d.qwm", surah);
    wm->file = hal_fs_open(rel);
    if (!wm->file) return false;   // no glosses for this surah — caller degrades

    uint8_t hdr[12];
    if (hal_fs_pread(wm->file, hdr, 12, 0) != 12 || memcmp(hdr, "QNWM", 4) != 0) {
        QN_LOGE(TAG, "bad qwm header: %s", rel);
        hal_fs_close(wm->file); wm->file = NULL; return false;
    }
    wm->version = rd_u16(hdr + 4);
    wm->surah   = rd_u16(hdr + 6);
    wm->n_ayat  = rd_u16(hdr + 8);

    size_t idx_bytes = (size_t)wm->n_ayat * WBW_IDX_STRIDE;
    wm->index = malloc(idx_bytes ? idx_bytes : 1);
    if (!wm->index ||
        (size_t)hal_fs_pread(wm->file, wm->index, idx_bytes, 12) != idx_bytes) {
        QN_LOGE(TAG, "qwm index read failed: %s", rel);
        free(wm->index); wm->index = NULL;
        hal_fs_close(wm->file); wm->file = NULL; return false;
    }
    QN_LOGI(TAG, "wbw %s: %u ayat (streamed)", rel, (unsigned)wm->n_ayat);
    return true;
}

void wordmeaning_close(WordMeaning *wm)
{
    if (wm->file) hal_fs_close(wm->file);
    free(wm->index);
    free(wm->rec);
    memset(wm, 0, sizeof(*wm));
    wm->cache_ayah = -1;
}

static const uint8_t *find_entry(const WordMeaning *wm, int ayah)
{
    for (uint32_t i = 0; i < wm->n_ayat; i++) {
        const uint8_t *e = wm->index + (size_t)i * WBW_IDX_STRIDE;
        if (rd_u16(e) == ayah) return e;
    }
    return NULL;
}

int wordmeaning_word_count(WordMeaning *wm, int ayah)
{
    if (!wm->file || !wm->index) return 0;
    const uint8_t *e = find_entry(wm, ayah);
    return e ? rd_u16(e + 2) : 0;
}

// Stream `ayah`'s blob into the single cache slot. Returns false on any error.
static bool load_record(WordMeaning *wm, int ayah)
{
    if (wm->cache_ayah == ayah) return true;
    const uint8_t *e = find_entry(wm, ayah);
    if (!e) return false;
    int nw          = rd_u16(e + 2);
    uint32_t off    = rd_u32(e + 4);
    uint32_t len    = rd_u32(e + 8);
    if (len < (uint32_t)nw * 2) return false;   // must at least hold the off table

    if (wm->rec_cap < len) {
        uint8_t *nb = realloc(wm->rec, len);
        if (!nb) return false;
        wm->rec = nb; wm->rec_cap = len;
    }
    if ((size_t)hal_fs_pread(wm->file, wm->rec, len, off) != len) {
        QN_LOGE(TAG, "qwm blob read failed ayah %d", ayah);
        wm->cache_ayah = -1;
        return false;
    }
    wm->cache_ayah   = ayah;
    wm->cache_nwords = nw;
    wm->rec_len      = len;
    return true;
}

const char *wordmeaning_word(WordMeaning *wm, int ayah, int word)
{
    if (!wm->file || !wm->index || word < 0) return "";
    if (!load_record(wm, ayah)) return "";
    if (word >= wm->cache_nwords) return "";
    uint16_t o = rd_u16(wm->rec + (size_t)word * 2);
    if (o >= wm->rec_len) return "";               // corrupt offset
    // The string must be NUL-terminated within the record.
    const char *s = (const char *)(wm->rec + o);
    for (size_t i = o; i < wm->rec_len; i++)
        if (wm->rec[i] == 0) return s;
    return "";
}
