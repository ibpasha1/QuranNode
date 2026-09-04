// progress.h — durable reading state: resume point + bookmarks.
//
// Backed by the HAL state store (hal_state_*). The resume point is what makes
// "pick it up and you're back exactly where you were" work: last surah:ayah AND
// speed. Bookmarks are a small most-recent-first list.
#pragma once

#include <stdbool.h>

typedef struct { int surah, ayah; float rate; } ResumePoint;
typedef struct { int surah, ayah; } Bookmark;

#define QN_MAX_BOOKMARKS 20

void progress_init(void);   // load persisted state (call once at boot)

bool progress_has_resume(void);
ResumePoint progress_resume(void);
void progress_set_resume(int surah, int ayah, float rate);   // updates + persists

int  progress_bookmark_count(void);
Bookmark progress_bookmark(int i);              // 0 = most recent
void progress_add_bookmark(int surah, int ayah); // de-dups to front, persists
bool progress_is_bookmarked(int surah, int ayah);
