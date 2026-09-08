#include "qday.h"
#include "hal.h"
#include <stdio.h>
#include <time.h>

int qday_today(void)
{
    int64_t epoch = hal_wall_clock();
    if (epoch <= 0) return 0;
    int64_t local = epoch + (int64_t)hal_tz_offset_min() * 60;
    if (local < 0) return 0;
    return (int)(local / 86400);
}

void qday_format(int day, char *buf, int n)
{
    static const char *MON[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (day <= 0) { snprintf(buf, n, "--"); return; }
    // Midday of that local day, read back as UTC, gives its calendar date.
    time_t t = (time_t)day * 86400 + 43200;
    struct tm g;
    gmtime_r(&t, &g);
    snprintf(buf, n, "%s %d", MON[g.tm_mon % 12], g.tm_mday);
}

int qday_diff(int from, int to)
{
    if (from <= 0 || to <= 0) return 0;
    return to - from;
}
