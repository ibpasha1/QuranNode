// prayer.h — daily prayer time calculation (pure math, no I/O).
//
// Standard solar-position method: from the date we derive the sun's
// declination and the equation of time, then each prayer is the moment the sun
// reaches its defining altitude (ISNA angles: Fajr/Isha at -15°; Asr at the
// Shafi'i shadow factor of 1). Accuracy is within a minute or two of published
// timetables — right for a glanceable "next prayer" line, not for adhan
// automation at extreme latitudes (>60° needs special-case rules; we clamp).
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PRAYER_FAJR = 0, PRAYER_SUNRISE, PRAYER_DHUHR,
    PRAYER_ASR, PRAYER_MAGHRIB, PRAYER_ISHA,
    PRAYER_COUNT,
} PrayerId;

typedef struct {
    int minutes[PRAYER_COUNT];   // minutes after local midnight (0..1439)
} PrayerTimes;

// Compute today's times for the local day containing `epoch_utc` (seconds) at
// lat/lng (degrees, +N/+E) with the local tz offset in minutes. Returns false
// if a time is undefined (polar night/day at high latitude).
bool prayer_compute(int64_t epoch_utc, int tz_min, float lat, float lng,
                    PrayerTimes *out);

// The next prayer at/after local `now_min` (minutes after midnight); Sunrise is
// skipped (it ends Fajr, it isn't prayed). After Isha, wraps to tomorrow's
// Fajr. Returns minutes until it, and the prayer via *which.
int prayer_next(const PrayerTimes *pt, int now_min, PrayerId *which);

const char *prayer_name(PrayerId id);   // "Fajr" ... "Isha"
