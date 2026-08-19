#include "look/stdlib.h"
#include "look/html_escape.h"
#include "look/charset.h"
#include "look/parallel_runtime.h"
#include "look/logger.h"
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <cstdlib>
#include <ctime>
#include <regex>
#include <iomanip>
#include <optional>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#if defined(_WIN32)
#  include <windows.h>
#  include <bcrypt.h>
#else
#  include <fstream>
#endif

namespace look {

// CSPRNG bayt üretimi — string::random gibi token/anahtar üretmeye davetkâr
// yollar için. std::rand() KRİPTOGRAFİK DEĞİL (saniye-seed + LCG = tahmin
// edilebilir → API key/reset token üretilirse auth bypass). Fail-closed:
// güvenli kaynak yoksa zayıf çıktı yaymak yerine hata fırlat. (web.cpp
// random_hex ve session ID ile aynı disiplin.)
static void secure_random_fill(uint8_t* buf, size_t n) {
    if (n == 0) return;
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("string::random: CSPRNG unavailable (BCryptGenRandom)");
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    urandom.read(reinterpret_cast<char*>(buf), (std::streamsize)n);
    if (!urandom || (size_t)urandom.gcount() != n)
        throw std::runtime_error("string::random: CSPRNG unavailable (/dev/urandom)");
#endif
}

static void check_args(const std::string& fn, size_t got, size_t expected) {
    if (got != expected)
        throw std::runtime_error(fn + "() expects " + std::to_string(expected) +
                                 " argument(s), got " + std::to_string(got));
}
static void check_args_min(const std::string& fn, size_t got, size_t min) {
    if (got < min)
        throw std::runtime_error(fn + "() expects at least " + std::to_string(min) +
                                 " argument(s), got " + std::to_string(got));
}

// â"€â"€ math module â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€
static Module make_math() {
    Module m;
    m.name = "math";

    m.functions["sqrt"] = [](auto args) {
        check_args("math::sqrt", args.size(), 1);
        double x = args[0].to_float();
        // Negatif → sessiz NaN yerine net hata. NaN sonradan JSON'a girerse
        // geçersiz JSON (NaN yasak) üretirdi.
        if (x < 0) throw std::runtime_error("math::sqrt: square root of a negative number is undefined");
        return Value(std::sqrt(x));
    };
    m.functions["pow"] = [](auto args) {
        check_args("math::pow", args.size(), 2);
        return Value(std::pow(args[0].to_float(), args[1].to_float()));
    };
    m.functions["abs"] = [](auto args) {
        check_args("math::abs", args.size(), 1);
        if (args[0].type() == Value::FLOAT) return Value(std::abs(args[0].as_float()));
        return Value(std::abs(args[0].to_int()));
    };
    m.functions["floor"] = [](auto args) {
        check_args("math::floor", args.size(), 1);
        return Value((int64_t)std::floor(args[0].to_float()));
    };
    m.functions["ceil"] = [](auto args) {
        check_args("math::ceil", args.size(), 1);
        return Value((int64_t)std::ceil(args[0].to_float()));
    };
    m.functions["round"] = [](auto args) {
        check_args("math::round", args.size(), 1);
        return Value((int64_t)std::round(args[0].to_float()));
    };
    m.functions["max"] = [](auto args) -> Value {
        if (args.size() == 1 && args[0].type() == Value::ARRAY) {
            auto& arr = *args[0].as_array();
            if (arr.empty()) return Value();
            Value best = arr[0];
            for (size_t i = 1; i < arr.size(); ++i)
                if (arr[i] >= best) best = arr[i];
            return best;
        }
        if (args.size() < 2) throw std::runtime_error("math::max() expects 2+ arguments or 1 array");
        Value best = args[0];
        for (size_t i = 1; i < args.size(); ++i)
            if (args[i] >= best) best = args[i];
        return best;
    };
    m.functions["min"] = [](auto args) -> Value {
        if (args.size() == 1 && args[0].type() == Value::ARRAY) {
            auto& arr = *args[0].as_array();
            if (arr.empty()) return Value();
            Value best = arr[0];
            for (size_t i = 1; i < arr.size(); ++i)
                if (arr[i] <= best) best = arr[i];
            return best;
        }
        if (args.size() < 2) throw std::runtime_error("math::min() expects 2+ arguments or 1 array");
        Value best = args[0];
        for (size_t i = 1; i < args.size(); ++i)
            if (args[i] <= best) best = args[i];
        return best;
    };
    m.functions["pi"] = [](auto args) {
        return Value(3.14159265358979323846);
    };
    m.functions["random"] = [](auto args) -> Value {
        if (args.size() == 2) {
            int lo = args[0].to_int(), hi = args[1].to_int();
            // hi < lo → aralık boyutu ≤0; `rand() % 0` tamsayı sıfıra bölme
            // (SIGFPE crash), negatif aralık ise tanımsız modulus. Net hata ver.
            if (hi < lo) throw std::runtime_error("math::random: hi < lo (invalid range)");
            return Value(lo + std::rand() % (hi - lo + 1));
        }
        return Value((double)std::rand() / RAND_MAX);
    };
    m.functions["log"] = [](auto args) {
        check_args("math::log", args.size(), 1);
        double x = args[0].to_float();
        // x ≤ 0 → log(0)=-Inf, log(negatif)=NaN. sqrt gibi net hata ver — aksi
        // halde NaN/Inf sonradan JSON'a girip geçersiz JSON üretir.
        if (x <= 0) throw std::runtime_error("math::log: logarithm of a non-positive number is undefined");
        return Value(std::log(x));
    };
    m.functions["sin"] = [](auto args) {
        check_args("math::sin", args.size(), 1);
        return Value(std::sin(args[0].to_float()));
    };
    m.functions["cos"] = [](auto args) {
        check_args("math::cos", args.size(), 1);
        return Value(std::cos(args[0].to_float()));
    };
    m.functions["tan"] = [](auto args) {
        check_args("math::tan", args.size(), 1);
        return Value(std::tan(args[0].to_float()));
    };

    return m;
}

// â"€â"€ string module â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€
// ── UTF-8 yardımcıları ──────────────────────────────────────────────────────
// LOOK string'leri UTF-8. Byte-tabanlı len/upper/substr/reverse çok-byte
// karakterleri (ç,ğ,İ,ı,ö,ş,ü…) parçalayıp geçersiz UTF-8 üretiyordu.
// Bu yardımcılar codepoint-farkında çalışır; Türkiye pazarı için kritik.
static std::vector<uint32_t> utf8_decode(const std::string& s) {
    std::vector<uint32_t> cps;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp; int len;
        if (c < 0x80)             { cp = c;        len = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E){ cp = c & 0x07; len = 4; }
        else                      { cps.push_back(0xFFFD); i++; continue; }
        if (i + (size_t)len > n)  { cps.push_back(0xFFFD); break; }
        bool ok = true;
        for (int k = 1; k < len; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc >> 6) != 0x2) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { cps.push_back(0xFFFD); i++; continue; }
        cps.push_back(cp);
        i += (size_t)len;
    }
    return cps;
}
static void utf8_encode_cp(uint32_t cp, std::string& out) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}
static std::string utf8_encode(const std::vector<uint32_t>& cps) {
    std::string out;
    for (uint32_t cp : cps) utf8_encode_cp(cp, out);
    return out;
}
// Codepoint büyük/küçük harf — locale-BAĞIMSIZ (klasik diller gibi): i↔I.
// Türkçe'ye özgü i↔İ / ı↔I locale kuralı YOK (dil globaldir). Türkçe harfler
// yine standart Unicode karşılığına gider: ç↔Ç, ğ↔Ğ, ö↔Ö, ş↔Ş, ü↔Ü, ı→I, İ→i.
// KAPSAM: eskiden yalnizca ASCII + Latin-1 + 3 Turkce kod noktasi (ı ğ ş) vardi.
// Yani KURAL globaldi ama TABLO Turkiye'ye ozeldi: Kiril, Yunan ve Latin Ext-A'nin
// kalan ~125 karakteri sessizce donusmeden geciyordu —
//   upper("привет") = "привет"  (hic degismiyor)
//   upper("ελλάδα") = "ελλάδα"
//   upper("łódź")   = "łÓDź"    (YARIM donusum: o,d dondu; ł,ź donmedi)
// Hata yok, uyari yok: sessiz yanlis/karisik metin. Asagidaki araliklar eklendi.
//
// DIKKAT: Latin Ext-A'nin genel cift/tek kurali ı(0x131) -> İ(0x130) verir, yani
// TURKCE locale davranisini geri getirir. Turkce istisnalari bu yuzden kuraldan
// ONCE ele aliniyor (dil global kalmali: ı → I, İ → i).
static uint32_t cp_upper(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;   // ASCII: i → I
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32; // Latin-1 (à→À, ç→Ç, ö→Ö, ü→Ü…)
    switch (c) {
        case 0x131: return 'I';   // ı → I (standart Unicode — Turkce'de İ olurdu)
        case 0x11F: return 0x11E; // ğ → Ğ
        case 0x15F: return 0x15E; // ş → Ş
        case 0x17F: return 'S';   // ſ (uzun s) → S
        case 0x3C2: return 0x3A3; // ς (son sigma) → Σ
        case 0x3AC: return 0x386; // ά → Ά
        case 0x3CC: return 0x38C; // ό → Ό
    }
    // Latin Extended-A (0x100–0x17F): cogunlukla ikili eslesme.
    // 0x100–0x137 ve 0x14A–0x177: CIFT = buyuk, TEK = kucuk
    // 0x139–0x148 ve 0x179–0x17E: TEK = buyuk, CIFT = kucuk
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return (c % 2) ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return (c % 2 == 0) ? c - 1 : c;
    // Yunan: α–ρ, σ–ϋ  (0x3B1–0x3C1, 0x3C3–0x3CB)
    if ((c >= 0x3B1 && c <= 0x3C1) || (c >= 0x3C3 && c <= 0x3CB)) return c - 0x20;
    if (c >= 0x3AD && c <= 0x3AF) return c - 0x25; // έ ή ί → Έ Ή Ί
    if (c >= 0x3CD && c <= 0x3CE) return c - 0x3F; // ύ ώ → Ύ Ώ
    // Kiril: а–я (0x430–0x44F), ѐ–џ (0x450–0x45F)
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;
    return c;
}
static uint32_t cp_lower(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;   // ASCII: I → i
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32; // Latin-1 (À→à, Ç→ç, Ö→ö, Ü→ü…)
    switch (c) {
        case 0x130: return 'i';   // İ → i (standart Unicode — Turkce'de ı olurdu)
        case 0x11E: return 0x11F; // Ğ → ğ
        case 0x15E: return 0x15F; // Ş → ş
        case 0x178: return 0xFF;  // Ÿ → ÿ
        case 0x386: return 0x3AC; // Ά → ά
        case 0x38C: return 0x3CC; // Ό → ό
    }
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return (c % 2 == 0) ? c + 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return (c % 2) ? c + 1 : c;
    if ((c >= 0x391 && c <= 0x3A1) || (c >= 0x3A3 && c <= 0x3AB)) return c + 0x20;
    if (c >= 0x388 && c <= 0x38A) return c + 0x25; // Έ Ή Ί → έ ή ί
    if (c >= 0x38E && c <= 0x38F) return c + 0x3F; // Ύ Ώ → ύ ώ
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;
    return c;
}

// Dolgu — Go semantigi (fmt "%Ns"): hedef genislik ASGARI'dir ve KOD NOKTASI
// sayilir. Metin zaten uzunsa OLDUGU GIBI doner; dolgu fonksiyonu asla kirpmaz.
//
// ESKI HATA 1 (sessiz veri kaybi): uzun metin kirpiliyordu —
//   pad_left("abcdef", 3, "0") = "def"   (PHP str_pad / Python ljust kirpmaz)
// ESKI HATA 2 (bozuk cikti): kirpma BAYT tabanliydi, cok baytli karakteri
//   ortadan boluyordu — pad_left("şğü", 3, "x") = 0x9F 0xC3 0xBC, oksuz devam
//   baytiyla baslayan GECERSIZ UTF-8. Bu bozuk metin JSON'a, veritabanina ve
//   HTTP yanitina aynen gidiyordu.
// ESKI HATA 3 (tutarsizlik): genislik BAYT sayiyordu, oysa len/substr/upper/
//   reverse hepsi kod noktasi farkindalikli — pad_left("ş", 3, "x") = "xş",
//   gorunen uzunluk 2 (beklenen 3).
static std::string pad_to(const std::string& s, int64_t len,
                          const std::string& pad, bool left) {
    int64_t cur = (int64_t)utf8_decode(s).size();
    if (cur >= len) return s;                 // ASGARI genislik: kirpma YOK
    auto padcps = utf8_decode(pad);
    if (padcps.empty()) return s;             // bos dolgu -> sonsuz donguye girme
    std::string fill;
    int64_t need = len - cur;
    while (need > 0)
        for (uint32_t cp : padcps) {
            if (need <= 0) break;
            utf8_encode_cp(cp, fill);
            --need;
        }
    return left ? fill + s : s + fill;
}

static Module make_string() {
    Module m;
    m.name = "string";

    // Karakter (codepoint) sayısı — byte değil. len("İstanbul") = 8.
    m.functions["len"] = [](auto args) {
        check_args("string::len", args.size(), 1);
        return Value((int64_t)utf8_decode(args[0].to_string()).size());
    };
    // Bir single-byte charset'i (iso-8859-9, windows-1254, latin1/1252) UTF-8'e çevir.
    // Bilinmeyen charset → girdi olduğu gibi döner. HTTP istemcisi de aynı fonksiyonu
    // Content-Type charset'i için otomatik kullanır.
    m.functions["decode"] = [](auto args) -> Value {
        check_args("string::decode", args.size(), 2);
        std::string in = args[0].to_string();
        std::string cs = args[1].to_string();
        std::string out;
        if (look::charset_to_utf8(in, cs, out)) return Value(out);
        return Value(in);
    };
    // Büyük harf — locale-bağımsız (i→I). Çok-byte karakter bozulmaz.
    m.functions["upper"] = [](auto args) {
        check_args("string::upper", args.size(), 1);
        auto cps = utf8_decode(args[0].to_string());
        for (auto& c : cps) c = cp_upper(c);
        return Value(utf8_encode(cps));
    };
    // Küçük harf — locale-bağımsız (I→i).
    m.functions["lower"] = [](auto args) {
        check_args("string::lower", args.size(), 1);
        auto cps = utf8_decode(args[0].to_string());
        for (auto& c : cps) c = cp_lower(c);
        return Value(utf8_encode(cps));
    };
    m.functions["trim"] = [](auto args) {
        check_args("string::trim", args.size(), 1);
        std::string s = args[0].to_string();
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end   = s.find_last_not_of(" \t\r\n");
        return Value(start == std::string::npos ? "" : s.substr(start, end - start + 1));
    };
    m.functions["ltrim"] = [](auto args) {
        check_args("string::ltrim", args.size(), 1);
        std::string s = args[0].to_string();
        size_t start = s.find_first_not_of(" \t\r\n");
        return Value(start == std::string::npos ? "" : s.substr(start));
    };
    m.functions["rtrim"] = [](auto args) {
        check_args("string::rtrim", args.size(), 1);
        std::string s = args[0].to_string();
        size_t end = s.find_last_not_of(" \t\r\n");
        return Value(end == std::string::npos ? "" : s.substr(0, end + 1));
    };
    m.functions["replace"] = [](auto args) {
        check_args("string::replace", args.size(), 3);
        std::string s     = args[0].to_string();
        std::string from  = args[1].to_string();
        std::string to    = args[2].to_string();
        // BOS arama dizesi — Go semantigi (strings.Replace(s, "", new, -1)):
        // metnin BASINDA ve her UTF-8 dizisinden SONRA eslesir; k kod noktasi icin
        // k+1 ekleme. "abc" + "X" -> "XaXbXcX". Sinirli ve tanimli.
        // ESKI HATA: asagidaki dongu bos 'from' ile SONSUZA kadar donuyordu —
        // s.find("", pos) her zaman pos'u dondurur, her turda araya 'to' eklenir,
        // string surekli buyur. Sonsuz dongu + SINIRSIZ BELLEK. Web'de
        // string::replace($metin, $kullaniciGirdisi, $x) cagrisinda kullanici bos
        // string gonderirse worker kilitleniyordu: tek istekle DoS.
        if (from.empty()) {
            auto cps = utf8_decode(s);
            std::string out;
            out.reserve(s.size() + to.size() * (cps.size() + 1));
            out += to;
            for (uint32_t cp : cps) { utf8_encode_cp(cp, out); out += to; }
            return Value(out);
        }
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
        return Value(s);
    };
    m.functions["contains"] = [](auto args) {
        check_args("string::contains", args.size(), 2);
        return Value(args[0].to_string().find(args[1].to_string()) != std::string::npos);
    };
    m.functions["starts_with"] = [](auto args) {
        check_args("string::starts_with", args.size(), 2);
        std::string s = args[0].to_string(), prefix = args[1].to_string();
        return Value(s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix);
    };
    m.functions["ends_with"] = [](auto args) {
        check_args("string::ends_with", args.size(), 2);
        std::string s = args[0].to_string(), suffix = args[1].to_string();
        return Value(s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix);
    };
    // Codepoint-tabanlı: substr("çğü",0,2) = "çğ" (yarım karakter değil).
    m.functions["substr"] = [](auto args) {
        check_args_min("string::substr", args.size(), 2);
        auto cps = utf8_decode(args[0].to_string());
        int64_t n = (int64_t)cps.size();
        int64_t start = args[1].to_int();
        if (start < 0) start = std::max((int64_t)0, n + start);
        if (start >= n) return Value(std::string(""));
        int64_t count = (args.size() == 3) ? args[2].to_int() : (n - start);
        if (count < 0) count = 0;
        // TAŞMA önle: count kalan uzunluğa (n-start) kırpılır ÖNCE — aksi halde
        // start+count int64 taşıp negatif end üretir (vector(first>last) → length_error,
        // memory-safety değil ama UB iterator aritmetiği). Kırpınca doğru: kalanı döndür.
        if (count > n - start) count = n - start;
        int64_t end = start + count;   // artık taşmaz (count ≤ n-start)
        std::vector<uint32_t> sub(cps.begin() + start, cps.begin() + end);
        return Value(utf8_encode(sub));
    };
    m.functions["repeat"] = [](auto args) {
        check_args("string::repeat", args.size(), 2);
        std::string s = args[0].to_string();
        int n = args[1].to_int();
        if (n < 0) throw std::runtime_error("string::repeat: repeat count cannot be negative");
        static constexpr int64_t MAX_REPEAT = 10 * 1024 * 1024; // 10 MB
        if ((int64_t)n * (int64_t)s.size() > MAX_REPEAT)
            throw std::runtime_error("string::repeat: result exceeds the 10 MB limit");
        std::string result;
        result.reserve((size_t)n * s.size());
        for (int i = 0; i < n; i++) result += s;
        return Value(result);
    };
    // Codepoint-tabanlı: reverse("abç") = "çba" (byte değil karakter ters).
    m.functions["reverse"] = [](auto args) {
        check_args("string::reverse", args.size(), 1);
        auto cps = utf8_decode(args[0].to_string());
        std::reverse(cps.begin(), cps.end());
        return Value(utf8_encode(cps));
    };
    m.functions["index_of"] = [](auto args) {
        check_args("string::index_of", args.size(), 2);
        std::string s = args[0].to_string(), needle = args[1].to_string();
        size_t pos = s.find(needle);
        return Value(pos == std::string::npos ? -1 : (int)pos);
    };
    // string::split("a,b,c", ",") → ["a","b","c"]
    m.functions["split"] = [](auto args) -> Value {
        check_args_min("string::split", args.size(), 2);
        std::string s   = args[0].to_string();
        std::string sep = args[1].to_string();
        int limit       = args.size() >= 3 ? args[2].to_int() : -1;
        auto arr = std::make_shared<std::vector<Value>>();
        if (sep.empty()) {
            for (char c : s) arr->push_back(Value(std::string(1, c)));
            return Value(arr);
        }
        size_t pos = 0, found;
        int count = 0;
        while ((found = s.find(sep, pos)) != std::string::npos) {
            if (limit > 0 && count >= limit - 1) break;
            arr->push_back(Value(s.substr(pos, found - pos)));
            pos = found + sep.size();
            count++;
        }
        arr->push_back(Value(s.substr(pos)));
        return Value(arr);
    };
    // string::join(["a","b","c"], ",") → "a,b,c"  (alias for built-in join)
    m.functions["join"] = [](auto args) -> Value {
        check_args_min("string::join", args.size(), 1);
        if (args[0].type() != Value::ARRAY) return Value(args[0].to_string());
        std::string sep = args.size() >= 2 ? args[1].to_string() : "";
        std::string result;
        auto& arr = *args[0].as_array();
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i) result += sep;
            result += arr[i].to_string();
        }
        return Value(result);
    };

    // Hedef uzunluk üst sınırı — `pad_left("x", 2000000000, " ")` sınırsız/O(n²)
    // allocation (her tur `pad + s` yeniden kopyalar) → hang/OOM DoS. string::repeat
    // (10MB) ve string::random (1MB) ile aynı disiplin.
    static constexpr int PAD_MAX = 10 * 1024 * 1024;
    m.functions["pad_left"] = [](auto args) -> Value {
        check_args("string::pad_left", args.size(), 3);
        std::string s   = args[0].to_string();
        int64_t     len = args[1].as_int();
        if (len < 0) len = 0;
        if (len > PAD_MAX) throw std::runtime_error("string::pad_left: target length exceeds the 10 MB limit");
        std::string pad = args[2].to_string();
        if (pad.empty()) pad = " ";
        return Value(pad_to(s, len, pad, true));
    };

    m.functions["pad_right"] = [](auto args) -> Value {
        check_args("string::pad_right", args.size(), 3);
        std::string s   = args[0].to_string();
        int64_t     len = args[1].as_int();
        if (len < 0) len = 0;
        if (len > PAD_MAX) throw std::runtime_error("string::pad_right: target length exceeds the 10 MB limit");
        std::string pad = args[2].to_string();
        if (pad.empty()) pad = " ";
        return Value(pad_to(s, len, pad, false));
    };

    m.functions["slugify"] = [](auto args) -> Value {
        check_args("string::slugify", args.size(), 1);
        std::string s = args[0].to_string();
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        auto rep = [&](const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        rep("\xc4\xb1", "i"); rep("\xc4\x9f", "g"); rep("\xc3\xbc", "u");
        rep("\xc5\x9f", "s"); rep("\xc3\xb6", "o"); rep("\xc3\xa7", "c");
        rep("\xc4\x9e", "g"); rep("\xc3\x9c", "u"); rep("\xc5\x9e", "s");
        rep("\xc3\x96", "o"); rep("\xc3\x87", "c"); rep("\xc4\xb0", "i");
        std::string result;
        bool last_dash = false;
        for (unsigned char c : s) {
            if (std::isalnum(c)) { result += (char)c; last_dash = false; }
            else if (!last_dash && !result.empty()) { result += '-'; last_dash = true; }
        }
        while (!result.empty() && result.back() == '-') result.pop_back();
        return Value(result);
    };

    m.functions["random"] = [](auto args) -> Value {
        int length = args.size() >= 1 ? (int)args[0].to_int() : 8;
        if (length < 0) length = 0;
        static constexpr int MAX_RANDOM = 1 << 20;   // 1 MB üst sınır (OOM DoS)
        if (length > MAX_RANDOM) length = MAX_RANDOM;
        static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::string result((size_t)length, '\0');
        if (length > 0) {
            std::vector<uint8_t> buf((size_t)length);
            secure_random_fill(buf.data(), buf.size());   // CSPRNG (fail-closed)
            for (int i = 0; i < length; ++i)
                result[(size_t)i] = chars[buf[(size_t)i] % (sizeof(chars) - 1)];
        }
        return Value(result);
    };

    // string::format("Merhaba %s, yaşın %d, ortalama %.2f", $ad, $yas, $ort)
    m.functions["format"] = [](auto args) -> Value {
        if (args.empty()) throw std::runtime_error("string::format() requires format string");
        std::string fmt = args[0].to_string();
        std::string result;
        size_t arg_idx = 1;
        for (size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%' || i + 1 >= fmt.size()) { result += fmt[i]; continue; }
            ++i;
            if (fmt[i] == '%') { result += '%'; continue; }
            // Width/precision üst sınırı — `%2000000000d` gibi devasa genişlik
            // setw ile yüz-MB'lık dolgu ayırıp bellek-amplifikasyon DoS'u (ve int
            // taşması UB'si) yapardı. 1M fazlasıyla yeterli; string::repeat 10MB
            // cap'iyle aynı disiplin.
            static constexpr int FMT_MAX = 1000000;
            bool zero_pad = (fmt[i] == '0');
            int width = 0;
            while (i < fmt.size() && std::isdigit((unsigned char)fmt[i])) {
                width = width * 10 + (fmt[i++] - '0');
                if (width > FMT_MAX) width = FMT_MAX;   // clamp — taşmadan önce
            }
            int precision = -1;
            if (i < fmt.size() && fmt[i] == '.') {
                ++i; precision = 0;
                while (i < fmt.size() && std::isdigit((unsigned char)fmt[i])) {
                    precision = precision * 10 + (fmt[i++] - '0');
                    if (precision > FMT_MAX) precision = FMT_MAX;
                }
            }
            if (i >= fmt.size()) break;
            char spec = fmt[i];
            if (arg_idx >= args.size()) { result += '%'; result += spec; continue; }
            Value& v = args[arg_idx++];
            std::ostringstream oss;
            if (zero_pad && width > 0) { oss << std::setfill('0') << std::setw(width); }
            else if (width > 0) { oss << std::setw(width); }
            if (spec == 's') {
                oss << v.to_string();
            } else if (spec == 'd') {
                oss << v.to_int();
            } else if (spec == 'f') {
                if (precision >= 0) oss << std::fixed << std::setprecision(precision);
                oss << v.to_float();
            } else if (spec == 'x') {
                oss << std::hex << v.to_int();
            } else if (spec == 'X') {
                oss << std::uppercase << std::hex << v.to_int();
            } else if (spec == 'o') {
                oss << std::oct << v.to_int();
            } else {
                oss << '%' << spec; --arg_idx;
            }
            result += oss.str();
        }
        return Value(result);
    };

    // ReDoS koruma yardımcısı: thread::detach + condition_variable (250ms timeout)
    // std::async kullanılmaz — destructor bloklayarak timeout'u etkisiz kılar.
    static std::atomic<int> g_regex_threads{0};
    static constexpr int REGEX_MAX_THREADS = 8;
    static auto regex_with_timeout = [](auto fn) -> decltype(fn()) {
        if (g_regex_threads.load() >= REGEX_MAX_THREADS)
            throw std::runtime_error("string::regex: concurrent regex limit exceeded (max 8)");
        using T = decltype(fn());
        struct State {
            std::mutex              mtx;
            std::condition_variable cv;
            bool                    done       = false;
            bool                    timed_out  = false; // kim sayacı azaltacak
            std::optional<T>        result;
            std::exception_ptr      ex;
        };
        auto state = std::make_shared<State>();

        g_regex_threads++;
        std::thread([state, fn = std::move(fn)]() mutable {
            try {
                T r = fn();
                std::lock_guard<std::mutex> lk(state->mtx);
                state->result = std::move(r);
            } catch (...) {
                std::lock_guard<std::mutex> lk(state->mtx);
                state->ex = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lk(state->mtx);
                state->done = true;
            }
            // F-01: slot'u HER ZAMAN worker (bu thread) gerçekten bitince serbest bırak —
            // main-thread timeout'ta DEĞİL. Aksi halde slot boşalır ama kaçak thread fn()'i
            // (catastrophic regex) yakmaya devam eder → 8 limiti CANLI thread'leri sınırlamaz
            // (N istek → N yanan thread → uzaktan DoS). Bu satır tek decrement noktası:
            // main azaltmadığı için çift-azaltma yok, ve worker azalttığı için sızıntı da yok.
            g_regex_threads--;
            state->cv.notify_one();
        }).detach();

        std::unique_lock<std::mutex> lk(state->mtx);
        if (!state->cv.wait_for(lk, std::chrono::milliseconds(250), [&]{ return state->done; })) {
            // Timeout: isteği hızlıca sonlandır AMA sayacı AZALTMA — kaçak thread hâlâ
            // çalışıyor; slot yalnız o thread bitince (yukarıda) serbest kalır → aynı anda
            // must be at most 8 CANLI regex thread'i olur, 9.'su 596'da reddedilir (sistem yavaşlar
            // ama ölmez). timed_out yalnız sonuç/notify sırasında anlamlı.
            state->timed_out = true;
            throw std::runtime_error("string::regex: execution timeout (ReDoS protection — pattern too complex)");
        }
        if (state->ex) std::rethrow_exception(state->ex);
        return std::move(*state->result);
    };

    // string::regex_match($str, $pattern) → bool
    m.functions["regex_match"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("string::regex_match() requires string and pattern");
        std::string pat = args[1].to_string();
        std::string str = args[0].to_string();
        if (pat.size() > 2048) throw std::runtime_error("string::regex_match(): pattern too long (max 2048)");
        if (str.size() > 65536) throw std::runtime_error("string::regex_match(): input too long (max 65536)");
        try {
            std::regex re(pat);
            return Value(regex_with_timeout([str, re]{ return std::regex_search(str, re); }));
        } catch (const std::runtime_error&) { throw; }
          catch (...) { return Value(false); }
    };

    // string::regex_replace($str, $pattern, $replacement) → string
    m.functions["regex_replace"] = [](auto args) -> Value {
        if (args.size() < 3) throw std::runtime_error("string::regex_replace() requires string, pattern, replacement");
        std::string pat = args[1].to_string();
        std::string str = args[0].to_string();
        std::string rep = args[2].to_string();
        if (pat.size() > 2048) throw std::runtime_error("string::regex_replace(): pattern too long (max 2048)");
        if (str.size() > 65536) throw std::runtime_error("string::regex_replace(): input too long (max 65536)");
        try {
            std::regex re(pat);
            return Value(regex_with_timeout([str, re, rep]{ return std::regex_replace(str, re, rep); }));
        } catch (const std::runtime_error&) { throw; }
          catch (...) { return Value(args[0].to_string()); }
    };

    // string::regex_match_all($str, $pattern) → [[tam_eşleşme, grup1, ...], ...]
    m.functions["regex_match_all"] = [](auto args) -> Value {
        if (args.size() < 2) throw std::runtime_error("string::regex_match_all() requires string and pattern");
        auto result = std::make_shared<std::vector<Value>>();
        try {
            std::string s = args[0].to_string();
            std::string pat = args[1].to_string();
            if (pat.size() > 2048) throw std::runtime_error("string::regex_match_all(): pattern too long (max 2048)");
            if (s.size() > 65536) throw std::runtime_error("string::regex_match_all(): input too long (max 65536)");
            std::regex re(pat);
            auto matches = regex_with_timeout([s, re]{
                std::vector<std::vector<std::string>> out;
                auto it = std::sregex_iterator(s.begin(), s.end(), re);
                for (auto end_it = std::sregex_iterator(); it != end_it; ++it) {
                    std::vector<std::string> m;
                    for (size_t i = 0; i < it->size(); ++i) m.push_back((*it)[i].str());
                    out.push_back(std::move(m));
                }
                return out;
            });
            for (auto& m : matches) {
                auto match = std::make_shared<std::vector<Value>>();
                for (auto& s2 : m) match->push_back(Value(s2));
                result->push_back(Value(match));
            }
        } catch (const std::runtime_error&) { throw; }
          catch (...) {}
        return Value(result);
    };

    return m;
}

// ── type module ───────────────────────────────────────────────────────────────
static Module make_type() {
    Module m;
    m.name = "type";

    m.functions["of"] = [](auto args) -> Value {
        check_args("type::of", args.size(), 1);
        switch (args[0].type()) {
            case Value::INT:      return Value(std::string("int"));
            case Value::FLOAT:    return Value(std::string("float"));
            case Value::STRING:   return Value(std::string("string"));
            case Value::BOOL:     return Value(std::string("bool"));
            case Value::FUNCTION: return Value(std::string("function"));
            case Value::ARRAY:    return Value(std::string("array"));
            case Value::NONE:     return Value(std::string("null"));
        }
        return Value(std::string("unknown"));
    };
    m.functions["is_null"]     = [](auto a) { check_args("type::is_null",     a.size(),1); return Value(a[0].type()==Value::NONE); };
    m.functions["is_string"]   = [](auto a) { check_args("type::is_string",   a.size(),1); return Value(a[0].type()==Value::STRING); };
    m.functions["is_int"]      = [](auto a) { check_args("type::is_int",      a.size(),1); return Value(a[0].type()==Value::INT); };
    m.functions["is_float"]    = [](auto a) { check_args("type::is_float",    a.size(),1); return Value(a[0].type()==Value::FLOAT); };
    m.functions["is_bool"]     = [](auto a) { check_args("type::is_bool",     a.size(),1); return Value(a[0].type()==Value::BOOL); };
    m.functions["is_array"]    = [](auto a) { check_args("type::is_array",    a.size(),1); return Value(a[0].type()==Value::ARRAY); };
    // BYTECODE_FN de fonksiyondur: VM'de derlenen closure bu tiple gelir. Yalnız
    // FUNCTION'a bakmak iki motoru ayırıyordu (interpreter true / VM false) —
    // TYPE_OF opcode'u da ikisine "function" der; is_function onunla hizalı olmalı.
    m.functions["is_function"] = [](auto a) { check_args("type::is_function", a.size(),1);
        return Value(a[0].type()==Value::FUNCTION || a[0].type()==Value::BYTECODE_FN); };
    m.functions["to_int"]    = [](auto a) { check_args("type::to_int",    a.size(),1); return Value(a[0].to_int()); };
    m.functions["to_float"]  = [](auto a) { check_args("type::to_float",  a.size(),1); return Value(a[0].to_float()); };
    m.functions["to_string"] = [](auto a) { check_args("type::to_string", a.size(),1); return Value(a[0].to_string()); };
    m.functions["to_bool"]   = [](auto a) { check_args("type::to_bool",   a.size(),1); return Value(a[0].is_truthy()); };

    return m;
}

// â"€â"€ log module â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€

static Module make_html() {
    Module m;
    m.name = "html";

    m.functions["escape"] = [](auto args) -> Value {
        if (args.empty()) return Value(std::string(""));
        return Value(look::html_escape(args[0].to_string()));   // tek tanım: look/html_escape.h
    };

    m.functions["attr"] = [](auto args) -> Value {
        if (args.empty()) return Value(std::string(""));
        std::string s = args[0].to_string();
        std::string out;
        out.reserve(s.size() * 4);
        // OWASP attribute-encode: alfanümerik DIŞINDAKİ tüm ASCII karakterleri
        // &#xHH; ile kodla. html::escape sadece `<>&"'` kaçar — bu TIRNAKSIZ
        // attribute'ta yetersiz (boşluk/=/backtick/tab enjekte edilerek yeni
        // event-handler açılabilir → XSS). Burada boşluk/=/backtick dahil her şey
        // kodlanır → hem tırnaklı hem tırnaksız attribute bağlamında güvenli.
        // Tarayıcı entity'yi çözdüğü için meşru değer bozulmaz (URL/veri çalışır).
        for (unsigned char c : s) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c >= 0x80) {
                out += (char)c;                  // alfanümerik + çok-baytlı UTF-8: güvenli
            } else {
                char buf[8];
                snprintf(buf, sizeof(buf), "&#x%02X;", c);
                out += buf;
            }
        }
        return Value(out);
    };

    m.functions["strip"] = [](auto args) -> Value {
        if (args.empty()) return Value(std::string(""));
        std::string s = args[0].to_string();
        std::string out;
        bool in_tag = false;
        for (char c : s) {
            if (c == '<') in_tag = true;
            else if (c == '>') in_tag = false;
            else if (!in_tag) out += c;
        }
        return Value(out);
    };

    return m;
}

static Module make_log() {
    Module m;
    m.name = "log";

    auto do_log = [](LogLevel level, const std::string& cat, auto args) {
        if (args.empty()) return Value();
        std::string msg;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) msg += " ";
            msg += args[i].to_string();
        }
        Logger::instance().log(level, cat, msg);
        return Value();
    };

    m.functions["info"] = [do_log](auto args) -> Value {
        return do_log(LogLevel::LOG_INFO, "APP", args);
    };
    m.functions["error"] = [do_log](auto args) -> Value {
        return do_log(LogLevel::LOG_ERROR, "APP", args);
    };
    m.functions["warn"] = [do_log](auto args) -> Value {
        return do_log(LogLevel::LOG_WARN, "APP", args);
    };
    m.functions["debug"] = [do_log](auto args) -> Value {
        return do_log(LogLevel::LOG_DEBUG, "APP", args);
    };
    m.functions["query"] = [](auto args) -> Value {
        if (args.size() < 2) return Value();
        Logger::instance().log_query(args[0].to_string(), args[1].to_int());
        return Value();
    };
    m.functions["memory"] = [do_log](auto args) -> Value {
        // Windows'ta working set size
        // Basit: sadece bir mesaj log'la
        return do_log(LogLevel::LOG_INFO, "MEM", args);
    };
    m.functions["configure"] = [](auto args) -> Value {
        // log::configure("logs", true, "debug")
        std::string dir     = args.size() > 0 ? args[0].to_string() : "logs";
        bool verbose        = args.size() > 1 ? args[1].is_truthy() : false;
        std::string level_s = args.size() > 2 ? args[2].to_string() : "info";
        LogLevel level = LogLevel::LOG_INFO;
        if (level_s == "debug") level = LogLevel::LOG_DEBUG;
        if (level_s == "warn")  level = LogLevel::LOG_WARN;
        if (level_s == "error") level = LogLevel::LOG_ERROR;
        Logger::instance().configure(dir, verbose, level);
        return Value();
    };

    return m;
}

// â"€â"€ Registry â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€â"€
// ── parallel:: module ─────────────────────────────────────────────────────────
// Observability + control for tasks spawned with parallel().

static Module make_parallel_module() {
    Module m;
    m.name = "parallel";

    // parallel::active() → int — current task count
    m.functions["active"] = [](auto /*args*/) -> Value {
        return Value(task_active());
    };

    // parallel::wait([timeout_ms=5000]) → bool — true if all finished in time
    m.functions["wait"] = [](auto args) -> Value {
        int ms = 5000;
        if (!args.empty() && args[0].type() == Value::INT)
            ms = (int)args[0].as_int();
        return Value(task_wait(ms));
    };

    // parallel::limit() → int — max allowed tasks (LOOK_PARALLEL_LIMIT)
    m.functions["limit"] = [](auto /*args*/) -> Value {
        return Value(task_limit());
    };

    // parallel::at_capacity() → bool — non-throwing capacity check
    // Use to check before parallel() when you want to shed load gracefully:
    //   if (!parallel::at_capacity()) { parallel(fn) }
    m.functions["at_capacity"] = [](auto /*args*/) -> Value {
        return Value(task_active() >= task_limit());
    };

    return m;
}

// Forward declarations — defined in their respective .cpp files
Module make_http_module(Interpreter* interp);
Module make_cache_module();
Module make_queue_module();
Module make_jobs_module();
Module make_mail_module();

// ── error:: module ─────────────────────────────────────────────────────────────
// Typed errors for .lk catch blocks.
//
// Usage:
//   throw error::new("not_found", "User does not exist", 404);
//
// catch e {
//   if e.type == "not_found" { response::status(404); }
//   io::println(e.message);
//   io::println(e.code);  // → 404
// }

static Module make_error_module() {
    Module m;
    m.name = "error";

    // error::new(type, message [, code=0]) → throws LookRuntimeError with Value payload
    m.functions["new"] = [](auto args) -> Value {
        std::string type    = args.size() >= 1 ? args[0].to_string() : "error";
        std::string message = args.size() >= 2 ? args[1].to_string() : "";
        int         code    = args.size() >= 3 ? (int)args[2].to_int()  : 0;

        // Build assoc array: {type, message, code}
        auto arr = std::make_shared<std::vector<Value>>();
        arr->push_back(Value(std::string("type")));    arr->push_back(Value(type));
        arr->push_back(Value(std::string("message"))); arr->push_back(Value(message));
        arr->push_back(Value(std::string("code")));    arr->push_back(Value(code));
        Value payload(arr);

        throw LookRuntimeError(payload);
        return Value(); // unreachable
    };

    // error::is(e, type) → bool — safe type check in catch block
    m.functions["is"] = [](auto args) -> Value {
        if (args.size() < 2) return Value(false);
        // e can be assoc array (from error::new) or plain string
        Value& e = args[0];
        if (e.type() == Value::ARRAY) {
            auto& arr = *e.as_array();
            for (size_t i = 0; i + 1 < arr.size(); i += 2) {
                if (arr[i].to_string() == "type")
                    return Value(arr[i+1].to_string() == args[1].to_string());
            }
        }
        return Value(false);
    };

    // error::message(e) → string — safe message extraction
    m.functions["message"] = [](auto args) -> Value {
        if (args.empty()) return Value(std::string(""));
        Value& e = args[0];
        if (e.type() == Value::ARRAY) {
            auto& arr = *e.as_array();
            for (size_t i = 0; i + 1 < arr.size(); i += 2)
                if (arr[i].to_string() == "message") return Value(arr[i+1].to_string());
        }
        return e.type() == Value::STRING ? e : Value(e.to_string());
    };

    // error::code(e) → int
    m.functions["code"] = [](auto args) -> Value {
        if (args.empty()) return Value(0);
        Value& e = args[0];
        if (e.type() == Value::ARRAY) {
            auto& arr = *e.as_array();
            for (size_t i = 0; i + 1 < arr.size(); i += 2)
                if (arr[i].to_string() == "code") return Value((int)arr[i+1].to_int());
        }
        return Value(0);
    };

    return m;
}

std::map<std::string, Module> make_stdlib(Interpreter* interp) {
    std::srand((unsigned)std::time(nullptr));
    std::map<std::string, Module> stdlib;
    auto add = [&](Module mod) { stdlib[mod.name] = std::move(mod); };
    add(make_math());
    add(make_string());
    add(make_type());
    add(make_html());
    add(make_log());
    add(make_file_module());
    add(make_date_module());
    add(make_http_module(interp));
    add(make_cache_module());
    add(make_queue_module());
    add(make_jobs_module());
    add(make_mail_module());
    add(make_parallel_module());
    add(make_error_module());
    return stdlib;
}

} // namespace look


