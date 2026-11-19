#include "look/stdlib.h"
#include <cstdint>
#include <stdexcept>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <chrono>

namespace look {

// Thread-safe localtime — std::localtime returns a shared static buffer and its
// internal tzset races on the global tz state (TSan data race, date_stdlib:128).
// localtime_r/localtime_s write to the caller's buffer; each thread gets its own tm.
static inline std::tm look_localtime(std::time_t t) {
    std::tm out{};
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

// "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DD" → tm
static std::tm parse_iso(const std::string& s) {
    std::tm t{};
    if (s.size() >= 19) {
        // YYYY-MM-DD HH:MM:SS
        sscanf(s.c_str(), "%4d-%2d-%2d %2d:%2d:%2d",
               &t.tm_year, &t.tm_mon, &t.tm_mday,
               &t.tm_hour, &t.tm_min, &t.tm_sec);
    } else if (s.size() >= 10) {
        // YYYY-MM-DD
        sscanf(s.c_str(), "%4d-%2d-%2d", &t.tm_year, &t.tm_mon, &t.tm_mday);
    } else {
        throw std::runtime_error("date: invalid date: " + s);
    }
    t.tm_year -= 1900;
    t.tm_mon  -= 1;
    t.tm_isdst = -1;
    return t;
}

// tm → "YYYY-MM-DD HH:MM:SS"
static std::string tm_to_iso(const std::tm& t, bool with_time = false) {
    char buf[32];
    if (with_time)
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year+1900, t.tm_mon+1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    else
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                 t.tm_year+1900, t.tm_mon+1, t.tm_mday);
    return buf;
}

// Apply custom format tokens — PHP-like but simple
// d=01-31  m=01-12  Y=2026  y=26  H=00-23  i=00-59  s=00-59
// D=Mon-Sun  l=Monday-Sunday  M=Jan-Dec  F=January-December
// N=1-7(Mon=1)  j=1-31(no leading zero)  n=1-12(no leading zero)  G=0-23
// t=number of days in the month  W=ISO week no  U=timestamp
static std::string format_date(const std::string& fmt, const std::tm& t) {
    static const char* short_days[]  = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* long_days[]   = {"Sunday","Monday","Tuesday","Wednesday",
                                        "Thursday","Friday","Saturday"};
    static const char* short_months[]= {"Jan","Feb","Mar","Apr","May","Jun",
                                        "Jul","Aug","Sep","Oct","Nov","Dec"};
    static const char* long_months[] = {"January","February","March","April","May","June",
                                        "July","August","September","October","November","December"};

    // number of days in the month
    auto days_in_month = [](int m, int y) -> int {
        static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        if (m == 1 && ((y%4==0 && y%100!=0) || y%400==0)) return 29;
        return dim[m];
    };

    std::string out;
    out.reserve(fmt.size() * 2);
    char buf[16];
    bool escape = false;

    for (size_t i = 0; i < fmt.size(); ++i) {
        char c = fmt[i];
        if (escape) { out += c; escape = false; continue; }
        if (c == '\\') { escape = true; continue; }

        switch (c) {
            case 'd': snprintf(buf,sizeof(buf),"%02d",t.tm_mday);       out+=buf; break;
            case 'j': snprintf(buf,sizeof(buf),"%d", t.tm_mday);        out+=buf; break;
            case 'm': snprintf(buf,sizeof(buf),"%02d",t.tm_mon+1);      out+=buf; break;
            case 'n': snprintf(buf,sizeof(buf),"%d", t.tm_mon+1);       out+=buf; break;
            case 'Y': snprintf(buf,sizeof(buf),"%04d",t.tm_year+1900);  out+=buf; break;
            case 'y': snprintf(buf,sizeof(buf),"%02d",(t.tm_year+1900)%100); out+=buf; break;
            case 'H': snprintf(buf,sizeof(buf),"%02d",t.tm_hour);       out+=buf; break;
            case 'G': snprintf(buf,sizeof(buf),"%d", t.tm_hour);        out+=buf; break;
            case 'i': snprintf(buf,sizeof(buf),"%02d",t.tm_min);        out+=buf; break;
            case 's': snprintf(buf,sizeof(buf),"%02d",t.tm_sec);        out+=buf; break;
            case 'D': out += short_days[t.tm_wday];  break;
            case 'l': out += long_days[t.tm_wday];   break;
            case 'M': out += short_months[t.tm_mon]; break;
            case 'F': out += long_months[t.tm_mon];  break;
            case 'N': snprintf(buf,sizeof(buf),"%d", t.tm_wday==0?7:t.tm_wday); out+=buf; break;
            case 't': snprintf(buf,sizeof(buf),"%d", days_in_month(t.tm_mon, t.tm_year+1900)); out+=buf; break;
            case 'U': {
                std::tm tmp = t;
                out += std::to_string((long long)mktime(&tmp));
                break;
            }
            default: out += c; break;
        }
    }
    return out;
}

// unit string → seconds multiplier (-1 = special handling required: month/year)
static long unit_to_seconds(const std::string& unit) {
    if (unit == "second" || unit == "seconds") return 1;
    if (unit == "minute" || unit == "minutes") return 60;
    if (unit == "hour"   || unit == "hours")   return 3600;
    if (unit == "day"    || unit == "days")     return 86400;
    if (unit == "week"   || unit == "weeks")    return 604800;
    if (unit == "month"  || unit == "months")   return -30;   // sentinel: month
    if (unit == "year"   || unit == "years")    return -365;  // sentinel: year
    throw std::runtime_error("date::add/sub: invalid unit '" + unit +
                             "' (second/minute/hour/day/week/month/year)");
}

// CALENDAR day count since 1970-01-01 (proleptic Gregorian, Howard Hinnant
// days_from_civil). COMPLETELY independent of TZ/DST — date::diff's day/week difference
// is computed with it; the old (mktime-mktime)/86400 path returned a difference spanning
// spring-forward one short (3 days → 2). y=full year, m=1..12, d=1..31.
static long days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

// ── date:: Module ─────────────────────────────────────────────────────────────

Module make_date_module() {
    Module m;
    m.name = "date";

    // date::now() → "YYYY-MM-DD HH:MM:SS"
    m.functions["now"] = [](auto) -> Value {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm _tm = look_localtime(t); std::tm* tm = &_tm;
        return Value(tm_to_iso(*tm, true));
    };

    // date::today() → "YYYY-MM-DD"
    m.functions["today"] = [](auto) -> Value {
        std::time_t t = std::time(nullptr);
        std::tm _tm = look_localtime(t); std::tm* tm = &_tm;
        return Value(tm_to_iso(*tm, false));
    };

    // date::timestamp() → int (Unix epoch seconds)
    m.functions["timestamp"] = [](auto) -> Value {
        return Value((int64_t)std::time(nullptr));
    };

    // date::mono_us() → monotonic steady_clock in microseconds (int64).
    // High-resolution timer for benchmarking; not wall-clock, only for measuring
    // elapsed durations (deltas). Independent of system clock adjustments.
    m.functions["mono_us"] = [](auto) -> Value {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        return Value((int64_t)std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    };

    // date::format(date, format_str) → string
    // date: Unix timestamp (int) OR "YYYY-MM-DD [HH:MM:SS]" ISO string.
    //   The most common format coming from a DB/API is a Unix timestamp — if INT/FLOAT it is
    //   interpreted directly as a timestamp (before, it silently produced a wrong date such as "1699...").
    //   If it is a string, it is parsed as ISO.
    // format: "d.m.Y", "Y/m/d H:i:s", etc. (PHP-style).
    m.functions["format"] = [](auto args) -> Value {
        if (args.size() < 2)
            throw std::runtime_error("date::format() requires (date, format)");
        std::tm t{};
        if (args[0].type() == Value::INT || args[0].type() == Value::FLOAT) {
            t = look_localtime((std::time_t)args[0].to_int());  // Unix timestamp
        } else {
            t = parse_iso(args[0].to_string());                 // ISO string
            mktime(&t);  // normalize tm_wday, tm_yday
        }
        return Value(format_date(args[1].to_string(), t));
    };

    // date::parse(date_str, format_str) → "YYYY-MM-DD" or "YYYY-MM-DD HH:MM:SS"
    // Supported format tokens: d m Y y H i s (others are literal)
    m.functions["parse"] = [](auto args) -> Value {
        if (args.size() < 2)
            throw std::runtime_error("date::parse() requires (date_string, format)");
        std::string input = args[0].to_string();
        std::string fmt   = args[1].to_string();

        std::tm t{};
        t.tm_hour = 0; t.tm_min = 0; t.tm_sec = 0;
        size_t si = 0;  // input index

        for (size_t fi = 0; fi < fmt.size() && si < input.size(); ++fi) {
            char fc = fmt[fi];
            if (fc == 'd' || fc == 'm' || fc == 'H' || fc == 'i' || fc == 's') {
                int val = 0;
                if (si < input.size() && std::isdigit(input[si])) val = (input[si++] - '0') * 10;
                if (si < input.size() && std::isdigit(input[si])) val += (input[si++] - '0');
                if (fc=='d') t.tm_mday = val;
                if (fc=='m') t.tm_mon  = val - 1;
                if (fc=='H') t.tm_hour = val;
                if (fc=='i') t.tm_min  = val;
                if (fc=='s') t.tm_sec  = val;
            } else if (fc == 'Y') {
                int val = 0;
                for (int k = 0; k < 4 && si < input.size() && std::isdigit(input[si]); ++k)
                    val = val * 10 + (input[si++] - '0');
                t.tm_year = val - 1900;
            } else if (fc == 'y') {
                int val = 0;
                for (int k = 0; k < 2 && si < input.size() && std::isdigit(input[si]); ++k)
                    val = val * 10 + (input[si++] - '0');
                t.tm_year = (val >= 70 ? 1900 + val : 2000 + val) - 1900;
            } else {
                // literal character — match and skip
                si++;
            }
        }

        // Field validation — there was no check before: parse("2023-60-99") silently returned
        // "2023-60-99" (month 60, day 99), parse("hello") → "0000-00-00"
        // (tm_to_iso does not normalize). A broken date silently flowed into arithmetic and
        // broke expiry/subscription logic. Now it throws a clear error (consistent with is_valid).
        if (t.tm_mon  < 0  || t.tm_mon  > 11 ||
            t.tm_mday < 1  || t.tm_mday > 31 ||
            t.tm_hour < 0  || t.tm_hour > 23 ||
            t.tm_min  < 0  || t.tm_min  > 59 ||
            t.tm_sec  < 0  || t.tm_sec  > 60)  // 60: leap second
            throw std::runtime_error("date::parse(): invalid date — '" + input + "'");

        // CALENDAR validation — the range check above is COARSE: since it only requires "day ≤ 31",
        // 2024-04-31 (April has 30) and 2023-02-29 (not a leap year) passed, then
        // mktime silently SHIFTED them to 1 May / 1 March. So parse returned successfully
        // with a WRONG date — and is_valid says false for the same inputs
        // (this block's own comment aimed at being "consistent with is_valid" but
        // it did not reach the goal because month-specific day limits were not checked).
        // Apply is_valid's technique: normalize with mktime, if the fields CHANGED
        // the input was calendrically impossible → error. (Month/day/year are compared; the hour
        // is not included in the comparison because it may shift due to DST.)
        {
            std::tm chk = t;
            chk.tm_isdst = -1;
            std::time_t ts = mktime(&chk);
            if (ts == (std::time_t)-1 ||
                chk.tm_year != t.tm_year || chk.tm_mon != t.tm_mon || chk.tm_mday != t.tm_mday)
                throw std::runtime_error("date::parse(): invalid date — '" + input + "'");
        }

        t.tm_isdst = -1;
        bool has_time = fmt.find('H') != std::string::npos;
        return Value(tm_to_iso(t, has_time));
    };

    // date::add(date, amount, unit) → "YYYY-MM-DD" or "YYYY-MM-DD HH:MM:SS"
    m.functions["add"] = [](auto args) -> Value {
        if (args.size() < 3)
            throw std::runtime_error("date::add() requires (date, amount, unit)");
        std::string src = args[0].to_string();
        std::tm t = parse_iso(src);
        t.tm_isdst = -1;
        bool has_time = src.size() >= 19;
        int  amount   = args[1].to_int();
        std::string unit = args[2].to_string();

        if (unit == "month" || unit == "months") {
            t.tm_mon += amount;
            // mktime normalizes month overflow (e.g. month=13 → year+1, month=1)
        } else if (unit == "year" || unit == "years") {
            t.tm_year += amount;
        } else if (unit == "day" || unit == "days") {
            // CALENDAR day — DST-safe. The old 86400-second path on a DST fall-back
            // (25-hour day) moved +1 day to the SAME day (a silent 1-day error:
            // subscription/reservation/reminder). "1 day" = 1 calendar day; mktime
            // (tm_isdst=-1) normalization gives the correct result independent of DST.
            t.tm_mday += amount;
        } else if (unit == "week" || unit == "weeks") {
            t.tm_mday += amount * 7;
        } else {
            // hour/minute/second: REAL duration → adding seconds is DST-correct.
            long secs = (long)amount * unit_to_seconds(unit);
            std::time_t ts = mktime(&t) + secs;
            std::tm _r = look_localtime(ts); std::tm* r = &_r;
            return Value(tm_to_iso(*r, has_time));
        }
        mktime(&t);
        return Value(tm_to_iso(t, has_time));
    };

    // date::sub(date, amount, unit) → "YYYY-MM-DD" or "YYYY-MM-DD HH:MM:SS"
    m.functions["sub"] = [](auto args) -> Value {
        if (args.size() < 3)
            throw std::runtime_error("date::sub() requires (date, amount, unit)");
        std::string src = args[0].to_string();
        std::tm t = parse_iso(src);
        t.tm_isdst = -1;
        bool has_time = src.size() >= 19;
        int  amount   = args[1].to_int();
        std::string unit = args[2].to_string();

        if (unit == "month" || unit == "months") {
            t.tm_mon -= amount;
        } else if (unit == "year" || unit == "years") {
            t.tm_year -= amount;
        } else if (unit == "day" || unit == "days") {
            t.tm_mday -= amount;                 // CALENDAR day — DST-safe (see add)
        } else if (unit == "week" || unit == "weeks") {
            t.tm_mday -= amount * 7;
        } else {
            // hour/minute/second: real duration → subtracting seconds is DST-correct.
            long secs = (long)amount * unit_to_seconds(unit);
            std::time_t ts = mktime(&t) - secs;
            std::tm _r = look_localtime(ts); std::tm* r = &_r;
            return Value(tm_to_iso(*r, has_time));
        }
        mktime(&t);
        return Value(tm_to_iso(t, has_time));
    };

    // date::diff(date1, date2, unit) → int
    // date1 - date2 (positive or negative)
    m.functions["diff"] = [](auto args) -> Value {
        if (args.size() < 3)
            throw std::runtime_error("date::diff() requires (date1, date2, unit)");
        std::string unit = args[2].to_string();
        std::tm t1 = parse_iso(args[0].to_string());
        std::tm t2 = parse_iso(args[1].to_string());
        // day/week: CALENDAR difference — DST-independent (civil-day). The seconds/86400 path
        // returned a difference spanning spring-forward (23-hour day) one short.
        if (unit == "day" || unit == "days" || unit == "week" || unit == "weeks") {
            long days = days_from_civil(t1.tm_year + 1900, t1.tm_mon + 1, t1.tm_mday)
                      - days_from_civil(t2.tm_year + 1900, t2.tm_mon + 1, t2.tm_mday);
            return Value((int64_t)(unit[0] == 'w' ? days / 7 : days));
        }
        // hour/minute/second: real duration → seconds difference is DST-correct.
        t1.tm_isdst = -1; t2.tm_isdst = -1;
        int64_t secs = (int64_t)(mktime(&t1) - mktime(&t2));
        int64_t div  = (int64_t)unit_to_seconds(unit);
        return Value((int64_t)(div != 0 ? secs / div : 0));
    };

    // date::from_timestamp(unix_ts) → "YYYY-MM-DD HH:MM:SS"
    m.functions["from_timestamp"] = [](auto args) -> Value {
        if (args.empty())
            throw std::runtime_error("date::from_timestamp() requires unix timestamp");
        std::time_t ts = (std::time_t)args[0].to_int();
        std::tm _t = look_localtime(ts); std::tm* t = &_t;
        return Value(tm_to_iso(*t, true));
    };

    // date::is_valid(date) → bool — is the "YYYY-MM-DD" format valid?
    m.functions["is_valid"] = [](auto args) -> Value {
        if (args.empty()) return Value(false);
        std::string s = args[0].to_string();
        if (s.size() < 10) return Value(false);
        try {
            std::tm t = parse_iso(s);
            t.tm_isdst = -1;
            std::time_t ts = mktime(&t);
            if (ts == -1) return Value(false);
            // Normalize check — mktime corrects it if a month/day limit was exceeded
            std::tm _check = look_localtime(ts); std::tm* check = &_check;
            int y = std::stoi(s.substr(0,4));
            int mo = std::stoi(s.substr(5,2));
            int d  = std::stoi(s.substr(8,2));
            return Value(check->tm_year+1900 == y && check->tm_mon+1 == mo && check->tm_mday == d);
        } catch (...) { return Value(false); }
    };

    // date::weekday(date) → int (1=Monday ... 7=Sunday, ISO 8601)
    m.functions["weekday"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("date::weekday() requires date");
        std::tm t = parse_iso(args[0].to_string());
        t.tm_isdst = -1;
        mktime(&t);
        int wday = t.tm_wday;  // 0=Sunday
        return Value(wday == 0 ? 7 : wday);
    };

    // date::week(date) → int (ISO week number 1-53)
    m.functions["week"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("date::week() requires date");
        std::tm t = parse_iso(args[0].to_string());
        t.tm_isdst = -1;
        mktime(&t);
        // ISO 8601 week number
        char buf[4];
        strftime(buf, sizeof(buf), "%V", &t);
        return Value(std::stoi(buf));
    };

    return m;
}

} // namespace look
