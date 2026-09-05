#include "prayer.h"
#include <math.h>

#define DEG2RAD (M_PI / 180.0)
#define RAD2DEG (180.0 / M_PI)

// ISNA convention (widely used in North America).
#define ANGLE_FAJR    15.0
#define ANGLE_ISHA    15.0
#define ANGLE_HORIZON 0.833     // sunrise/sunset: refraction + solar radius
#define ASR_FACTOR    1.0       // Shafi'i (shadow = object + noon shadow)

// Solar position for a given day: declination (rad) and equation of time
// (hours), from the low-precision NOAA/almanac series — plenty for minutes.
static void sun(double days_j2000, double *decl, double *eqt_hours)
{
    double g = fmod(357.529 + 0.98560028 * days_j2000, 360.0) * DEG2RAD;  // mean anomaly
    double q = fmod(280.459 + 0.98564736 * days_j2000, 360.0);            // mean longitude
    double L = (q + 1.915 * sin(g) + 0.020 * sin(2 * g)) * DEG2RAD;       // ecliptic longitude
    double e = (23.439 - 0.00000036 * days_j2000) * DEG2RAD;              // obliquity

    *decl = asin(sin(e) * sin(L));
    double ra = atan2(cos(e) * sin(L), cos(L)) * RAD2DEG / 15.0;          // right ascension, hours
    double eqt = q / 15.0 - fmod(ra + 24.0, 24.0);
    // Normalize to a small signed value (the series can wrap by 24h).
    while (eqt > 12.0) eqt -= 24.0;
    while (eqt < -12.0) eqt += 24.0;
    *eqt_hours = eqt;
}

// Half-day arc (hours) to reach altitude `alt_deg` (negative = below horizon).
// Returns NAN when the sun never gets there (polar day/night).
static double hour_arc(double alt_deg, double lat_rad, double decl)
{
    double cosH = (sin(alt_deg * DEG2RAD) - sin(lat_rad) * sin(decl)) /
                  (cos(lat_rad) * cos(decl));
    if (cosH < -1.0 || cosH > 1.0) return NAN;
    return acos(cosH) * RAD2DEG / 15.0;
}

static bool put(PrayerTimes *out, PrayerId id, double hours_local)
{
    if (isnan(hours_local)) return false;
    hours_local = fmod(hours_local + 24.0, 24.0);
    out->minutes[id] = (int)(hours_local * 60.0 + 0.5) % 1440;
    return true;
}

bool prayer_compute(int64_t epoch_utc, int tz_min, float lat, float lng,
                    PrayerTimes *out)
{
    // Local civil date -> days since J2000 at that date's local noon (the
    // solar terms drift ~1'/day, so evaluating once per day is plenty).
    int64_t local = epoch_utc + (int64_t)tz_min * 60;
    double days = floor((double)local / 86400.0);
    double noon_utc = days * 86400.0 - (double)tz_min * 60.0 + 43200.0;
    double j2000 = noon_utc / 86400.0 + (2440587.5 - 2451545.0);

    double decl, eqt;
    sun(j2000, &decl, &eqt);

    double lat_r = (double)lat * DEG2RAD;
    double tz_h = tz_min / 60.0;
    double dhuhr = 12.0 + tz_h - (double)lng / 15.0 - eqt;

    double h_horizon = hour_arc(-ANGLE_HORIZON, lat_r, decl);
    double h_fajr    = hour_arc(-ANGLE_FAJR, lat_r, decl);
    double h_isha    = hour_arc(-ANGLE_ISHA, lat_r, decl);
    // Asr: sun altitude when shadow = ASR_FACTOR + shadow at noon.
    double asr_alt = RAD2DEG * atan(1.0 / (ASR_FACTOR + tan(fabs(lat_r - decl))));
    double h_asr = hour_arc(asr_alt, lat_r, decl);

    bool ok = true;
    ok &= put(out, PRAYER_FAJR,    dhuhr - h_fajr);
    ok &= put(out, PRAYER_SUNRISE, dhuhr - h_horizon);
    ok &= put(out, PRAYER_DHUHR,   dhuhr);
    ok &= put(out, PRAYER_ASR,     dhuhr + h_asr);
    ok &= put(out, PRAYER_MAGHRIB, dhuhr + h_horizon);
    ok &= put(out, PRAYER_ISHA,    dhuhr + h_isha);
    return ok;
}

int prayer_next(const PrayerTimes *pt, int now_min, PrayerId *which)
{
    static const PrayerId ORDER[] = {
        PRAYER_FAJR, PRAYER_DHUHR, PRAYER_ASR, PRAYER_MAGHRIB, PRAYER_ISHA,
    };
    for (unsigned i = 0; i < sizeof(ORDER) / sizeof(ORDER[0]); i++) {
        if (pt->minutes[ORDER[i]] >= now_min) {
            if (which) *which = ORDER[i];
            return pt->minutes[ORDER[i]] - now_min;
        }
    }
    // Past Isha: tomorrow's Fajr (today's is within a minute of tomorrow's).
    if (which) *which = PRAYER_FAJR;
    return pt->minutes[PRAYER_FAJR] + 1440 - now_min;
}

const char *prayer_name(PrayerId id)
{
    static const char *N[PRAYER_COUNT] = {
        "Fajr", "Sunrise", "Dhuhr", "Asr", "Maghrib", "Isha",
    };
    return (id >= 0 && id < PRAYER_COUNT) ? N[id] : "";
}
