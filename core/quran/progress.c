#include "progress.h"
#include "hal.h"
#include "plat.h"
#include <string.h>

static const char *TAG = "PROGRESS";
#define STATE_MAGIC 0x51524731u   // "QRG1"

// One persisted blob for all reading state.
typedef struct {
    uint32_t magic;
    int32_t  has_resume;
    ResumePoint resume;
    int32_t  n_bm;
    Bookmark bm[QN_MAX_BOOKMARKS];
} ProgressBlob;

static ProgressBlob s_state;

static void persist(void)
{
    hal_state_save("progress", &s_state, sizeof(s_state));
}

void progress_init(void)
{
    size_t got = 0;
    if (hal_state_load("progress", &s_state, sizeof(s_state), &got) &&
        got == sizeof(s_state) && s_state.magic == STATE_MAGIC) {
        QN_LOGI(TAG, "loaded: resume %d:%d, %d bookmarks",
                s_state.resume.surah, s_state.resume.ayah, s_state.n_bm);
        return;
    }
    memset(&s_state, 0, sizeof(s_state));
    s_state.magic = STATE_MAGIC;
}

bool progress_has_resume(void) { return s_state.has_resume != 0; }
ResumePoint progress_resume(void) { return s_state.resume; }

void progress_set_resume(int surah, int ayah, float rate)
{
    s_state.has_resume = 1;
    s_state.resume.surah = surah;
    s_state.resume.ayah = ayah;
    s_state.resume.rate = rate;
    persist();
}

int progress_bookmark_count(void) { return s_state.n_bm; }

Bookmark progress_bookmark(int i)
{
    if (i < 0 || i >= s_state.n_bm) { Bookmark z = {0, 0}; return z; }
    return s_state.bm[i];
}

bool progress_is_bookmarked(int surah, int ayah)
{
    for (int i = 0; i < s_state.n_bm; i++)
        if (s_state.bm[i].surah == surah && s_state.bm[i].ayah == ayah) return true;
    return false;
}

void progress_add_bookmark(int surah, int ayah)
{
    // Remove any existing entry for this ref (so it moves to the front).
    int w = 0;
    for (int r = 0; r < s_state.n_bm; r++) {
        if (s_state.bm[r].surah == surah && s_state.bm[r].ayah == ayah) continue;
        s_state.bm[w++] = s_state.bm[r];
    }
    s_state.n_bm = w;
    // Shift down and insert at front.
    int keep = s_state.n_bm < QN_MAX_BOOKMARKS - 1 ? s_state.n_bm : QN_MAX_BOOKMARKS - 1;
    for (int i = keep; i > 0; i--) s_state.bm[i] = s_state.bm[i - 1];
    s_state.bm[0].surah = surah;
    s_state.bm[0].ayah = ayah;
    s_state.n_bm = keep + 1;
    persist();
}
