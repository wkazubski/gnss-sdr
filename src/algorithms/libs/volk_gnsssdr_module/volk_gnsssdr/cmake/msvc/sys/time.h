// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Carles Fernandez-Prades <carles.fernandez(at)cttc.es>
#ifndef _MSC_VER  // [
#error "Use this header only with Microsoft Visual C++ compilers!"
#else

#ifndef _MSC_SYS_TIME_H
#define _MSC_SYS_TIME_H

#ifndef NOMINMAX
#define NOMINMAX
#endif

// https://social.msdn.microsoft.com/Forums/vstudio/en-US/430449b3-f6dd-4e18-84de-eebd26a8d668/gettimeofday?forum=vcgeneral
#include < time.h >
#include <windows.h>  // I've omitted this line.
#if defined(_MSC_VER) || defined(_MSC_EXTENSIONS)
#define DELTA_EPOCH_IN_MICROSECS 11644473600000000Ui64
#else
#define DELTA_EPOCH_IN_MICROSECS 11644473600000000ULL
#endif

#if _MSC_VER < 1900
struct timespec
{
    time_t tv_sec; /* Seconds since 00:00:00 GMT, */

    /* 1 January 1970 */

    long tv_nsec; /* Additional nanoseconds since */

    /* tv_sec */
};
#endif

struct timezone
{
    int tz_minuteswest; /* minutes W of Greenwich */
    int tz_dsttime;     /* type of dst correction */
};

static inline int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    FILETIME ft;
    unsigned __int64 tmpres = 0;
    static int tzflag;

    if (NULL != tv)
        {
            GetSystemTimeAsFileTime(&ft);

            tmpres |= ft.dwHighDateTime;
            tmpres <<= 32;
            tmpres |= ft.dwLowDateTime;

            /* converting file time to unix epoch*/
            tmpres -= DELTA_EPOCH_IN_MICROSECS;
            tv->tv_sec = (long)(tmpres / 1000000UL);
            tv->tv_usec = (long)(tmpres % 1000000UL);
        }

    if (NULL != tz)
        {
            if (!tzflag)
                {
                    _tzset();
                    tzflag++;
                }
            tz->tz_minuteswest = _timezone / 60;
            tz->tz_dsttime = _daylight;
        }

    return 0;
}

#endif  // _MSC_SYS_TIME_H
#endif  // _MSC_VER