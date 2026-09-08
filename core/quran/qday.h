// qday.h — the shared calendar-day index.
//
// Days since the Unix epoch, in LOCAL time. **0 means "the clock is unknown"**
// and is safe as a sentinel because day 0 is 1970-01-01, which can never
// legitimately occur: the board has no RTC, so it only learns the date if SNTP
// happens to land during the boot Wi-Fi window (see hal/esp32/ota_esp32.c).
//
// Every feature that buckets work by day (khatm coverage, hifz review
// scheduling) keys off this, and every one of them must degrade honestly when
// it returns 0 rather than pretending it is 1970.
#pragma once

int  qday_today(void);                        // live; 0 when the clock is unknown
void qday_format(int day, char *buf, int n);  // "Mar 14", or "--" when unknown
int  qday_diff(int from, int to);             // to - from; 0 if either is unknown
