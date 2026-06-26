#include "look/stdlib.h"
#include <stdexcept>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <chrono>

namespace look {

// ── Helpers ───────────────────────────────────────────────────────────────────

// "YYYY-MM-DD HH:MM:SS" veya "YYYY-MM-DD" → tm
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
        throw std::runtime_error("date: geçersiz tarih: " + s);
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

// Özel format tokenlarını uygula — PHP benzeri ama sade
// d=01-31  m=01-12  Y=2026  y=26  H=00-23  i=00-59  s=00-59
// D=Mon-Sun  l=Monday-Sunday  M=Jan-Dec  F=January-December
// N=1-7(Pzt=1)  j=1-31(leading yok)  n=1-12(leading yok)  G=0-23
// t=ayın günü sayısı  W=ISO hafta no  U=timestamp
static std::string format_date(const std::string& fmt, const std::tm& t) {
    static const char* short_days[]  = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char* long_days[]   = {"Sunday","Monday","Tuesday","Wednesday",
                                        "Thursday","Friday","Saturday"};
    static const char* short_months[]= {"Jan","Feb","Mar","Apr","May","Jun",
                                        "Jul","Aug","Sep","Oct","Nov","Dec"};
    static const char* long_months[] = {"January","February","March","April","May","June",
                                        "July","August","September","October","November","December"};

    // ayın gün sayısı
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

// birim string → saniye çarpanı (-1 = özel işlem gerekir: month/year)
static long unit_to_seconds(const std::string& unit) {
    if (unit == "second" || unit == "seconds") return 1;
    if (unit == "minute" || unit == "minutes") return 60;
    if (unit == "hour"   || unit == "hours")   return 3600;
    if (unit == "day"    || unit == "days")     return 86400;
    if (unit == "week"   || unit == "weeks")    return 604800;
    if (unit == "month"  || unit == "months")   return -30;   // sentinel: month
    if (unit == "year"   || unit == "years")    return -365;  // sentinel: year
    throw std::runtime_error("date::add/sub: geçersiz birim '" + unit +
                             "' (second/minute/hour/day/week/month/year)");
}

// ── date:: Module ─────────────────────────────────────────────────────────────

Module make_date_module() {
    Module m;
    m.name = "date";

    // date::now() → "YYYY-MM-DD HH:MM:SS"
    m.functions["now"] = [](auto) -> Value {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm* tm = std::localtime(&t);
        return Value(tm_to_iso(*tm, true));
    };

    // date::today() → "YYYY-MM-DD"
    m.functions["today"] = [](auto) -> Value {
        std::time_t t = std::time(nullptr);
        std::tm* tm = std::localtime(&t);
        return Value(tm_to_iso(*tm, false));
    };

    // date::timestamp() → int (Unix epoch saniye)
    m.functions["timestamp"] = [](auto) -> Value {
        return Value((int)std::time(nullptr));
    };

    // date::format(tarih, format_str) → string
    // tarih: "YYYY-MM-DD" veya "YYYY-MM-DD HH:MM:SS"
    // format: "d.m.Y", "Y/m/d H:i:s", vb.
    m.functions["format"] = [](auto args) -> Value {
        if (args.size() < 2)
            throw std::runtime_error("date::format() requires (date, format)");
        std::tm t = parse_iso(args[0].to_string());
        mktime(&t);  // tm_wday, tm_yday normalize et
        return Value(format_date(args[1].to_string(), t));
    };

    // date::parse(tarih_str, format_str) → "YYYY-MM-DD" veya "YYYY-MM-DD HH:MM:SS"
    // Desteklenen format tokenları: d m Y y H i s (diğerleri literal)
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
                // literal karakter — eşleştir ve atla
                si++;
            }
        }

        t.tm_isdst = -1;
        bool has_time = fmt.find('H') != std::string::npos;
        return Value(tm_to_iso(t, has_time));
    };

    // date::add(tarih, miktar, birim) → "YYYY-MM-DD" veya "YYYY-MM-DD HH:MM:SS"
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
            // mktime ay taşmasını normalize eder (örn. ay=13 → yıl+1, ay=1)
        } else if (unit == "year" || unit == "years") {
            t.tm_year += amount;
        } else {
            long secs = (long)amount * unit_to_seconds(unit);
            std::time_t ts = mktime(&t) + secs;
            std::tm* r = std::localtime(&ts);
            return Value(tm_to_iso(*r, has_time));
        }
        mktime(&t);
        return Value(tm_to_iso(t, has_time));
    };

    // date::sub(tarih, miktar, birim) → "YYYY-MM-DD" veya "YYYY-MM-DD HH:MM:SS"
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
        } else {
            long secs = (long)amount * unit_to_seconds(unit);
            std::time_t ts = mktime(&t) - secs;
            std::tm* r = std::localtime(&ts);
            return Value(tm_to_iso(*r, has_time));
        }
        mktime(&t);
        return Value(tm_to_iso(t, has_time));
    };

    // date::diff(tarih1, tarih2, birim) → int
    // tarih1 - tarih2 (pozitif veya negatif)
    m.functions["diff"] = [](auto args) -> Value {
        if (args.size() < 3)
            throw std::runtime_error("date::diff() requires (date1, date2, unit)");
        std::tm t1 = parse_iso(args[0].to_string()); t1.tm_isdst = -1;
        std::tm t2 = parse_iso(args[1].to_string()); t2.tm_isdst = -1;
        long secs  = (long)(mktime(&t1) - mktime(&t2));
        long div   = unit_to_seconds(args[2].to_string());
        return Value((int)(secs / div));
    };

    // date::from_timestamp(unix_ts) → "YYYY-MM-DD HH:MM:SS"
    m.functions["from_timestamp"] = [](auto args) -> Value {
        if (args.empty())
            throw std::runtime_error("date::from_timestamp() requires unix timestamp");
        std::time_t ts = (std::time_t)args[0].to_int();
        std::tm* t = std::localtime(&ts);
        return Value(tm_to_iso(*t, true));
    };

    // date::is_valid(tarih) → bool — "YYYY-MM-DD" formatı geçerli mi?
    m.functions["is_valid"] = [](auto args) -> Value {
        if (args.empty()) return Value(false);
        std::string s = args[0].to_string();
        if (s.size() < 10) return Value(false);
        try {
            std::tm t = parse_iso(s);
            t.tm_isdst = -1;
            std::time_t ts = mktime(&t);
            if (ts == -1) return Value(false);
            // Normalize kontrol — ay/gün sınır aşıldıysa mktime düzeltir
            std::tm* check = std::localtime(&ts);
            int y = std::stoi(s.substr(0,4));
            int mo = std::stoi(s.substr(5,2));
            int d  = std::stoi(s.substr(8,2));
            return Value(check->tm_year+1900 == y && check->tm_mon+1 == mo && check->tm_mday == d);
        } catch (...) { return Value(false); }
    };

    // date::weekday(tarih) → int (1=Pazartesi ... 7=Pazar, ISO 8601)
    m.functions["weekday"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("date::weekday() requires date");
        std::tm t = parse_iso(args[0].to_string());
        t.tm_isdst = -1;
        mktime(&t);
        int wday = t.tm_wday;  // 0=Pazar
        return Value(wday == 0 ? 7 : wday);
    };

    // date::week(tarih) → int (ISO hafta numarası 1-53)
    m.functions["week"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("date::week() requires date");
        std::tm t = parse_iso(args[0].to_string());
        t.tm_isdst = -1;
        mktime(&t);
        // ISO 8601 hafta numarası
        char buf[4];
        strftime(buf, sizeof(buf), "%V", &t);
        return Value(std::stoi(buf));
    };

    return m;
}

} // namespace look
