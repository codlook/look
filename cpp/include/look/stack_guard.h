#pragma once
// Kalan yerel (C++) yığın. Geri çağırma zinciri (map/filter/sort… içinden LOOK fonksiyonu → VM
// yeniden girişi) her seviyede C++ yığını harcar; sabit sayaç (VM::MAX_NATIVE_DEPTH) normal
// derlemede yeter ama yığın küçükse (ulimit -s, küçük fiber yığını, ASan'ın büyük çerçeveleri)
// sayaçtan önce taşabilir. Bu denetim gerçek kalan yığına bakar: süreç asla çökmez, istek temiz
// "Stack overflow" alır. Go yığını büyütür ve 1 GB'ta süreci öldürür; burada süreç ayakta kalır.
//
// Yığın aralığı: fiber içindeyse fiber'ın kendi yığını (Fiber::stack_range), değilse iş parçacığının
// (pthread_getattr_np; Windows'ta TIB + VirtualQuery — Windows fiber'ı TIB'i kendisi günceller).
// Aralık önbelleklenir; o anki yığın işaretçisi aralığın dışındaysa (fiber değişti) yeniden okunur.
#include <cstddef>
#include <cstdint>
#if defined(_MSC_VER)
#  include <intrin.h>   // _AddressOfReturnAddress (windows.h burada YOK: min/max/DELETE makroları)
#endif

namespace look {

// Bir geri çağırma seviyesi daha açmadan önce kalması gereken yığın: yığının 1/8'i, 64 KB ile
// 256 KB arasına sıkıştırılmış. Sabit 1 MB pay ölçüldü ve YANLIŞTI: 1 MB'lık yığında (Windows iş
// parçacığı varsayılanı) ilk geri çağırma bile hata veriyordu. 8 MB'ta 256 KB: 4.000 seviye (~6 MB)
// sayacı asıl sınır kalır, pay son seviyedeki değer dolaşımına (1000 iç içe to_string ~190 KB,
// ölçüldü) yeter. Alt sınır 64 KB: 128 KB'lık yığında 1/8 (16 KB) ASan'ın büyük çerçeveli tek bir
// seviyesine yetmiyor, süreç aralıklı çöküyordu (ölçüldü, 3'te 2).
// AddressSanitizer her çerçeveye koruma bölgesi ekler (çerçeveler ~4 kat): pay aynı oranda büyür.
// Ölçüldü: ASan'da 64 KB pay, 128–256 KB yığında iki denetim arası kodu (iç içe atama derlemesi,
// ayrıştırıcının öncelik zinciri) karşılamıyordu → ASan yığın taşması; normal derleme etkilenmez.
#if defined(__SANITIZE_ADDRESS__)
constexpr size_t LOOK_STACK_FRAME_SCALE = 4;
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
constexpr size_t LOOK_STACK_FRAME_SCALE = 4;
#  else
constexpr size_t LOOK_STACK_FRAME_SCALE = 1;
#  endif
#else
constexpr size_t LOOK_STACK_FRAME_SCALE = 1;
#endif
constexpr size_t LOOK_STACK_RESERVE_MIN = 64 * 1024 * LOOK_STACK_FRAME_SCALE;
constexpr size_t LOOK_STACK_RESERVE_MAX = 256 * 1024 * LOOK_STACK_FRAME_SCALE;

inline const char* look_stack_pointer() noexcept {
#if defined(_MSC_VER)
    return static_cast<const char*>(_AddressOfReturnAddress());
#else
    return static_cast<const char*>(__builtin_frame_address(0));
#endif
}

// fiber-ok: yalnız o anki yığının aralığını önbellekler; her çağrıda yığın işaretçisi aralıkla
// karşılaştırılır, fiber değişince (işaretçi dışarıda) yeniden okunur — askı ile bozulmaz.
struct LookStackRange { const char* lo = nullptr; const char* hi = nullptr; size_t reserve = 0; };
inline LookStackRange& look_stack_range_cache() noexcept { thread_local LookStackRange r; return r; }

// O anki yığının aralığı (fiber içindeyse fiber'ın, değilse iş parçacığının). Platforma özgü:
// fiber_posix.cpp / fiber_win.cpp.
bool look_read_stack_range(const char*& lo, const char*& hi) noexcept;

// Kalan yığın payın altındaysa true. Aralık bilinmiyorsa false (denetim yok, sayaç kalır).
inline bool look_native_stack_low() noexcept {
    const char* sp = look_stack_pointer();
    LookStackRange& r = look_stack_range_cache();
    if (sp < r.lo || sp >= r.hi) {
        const char* lo = nullptr; const char* hi = nullptr;
        if (!look_read_stack_range(lo, hi) || sp < lo || sp >= hi) {
            // Okunamadı: aralığı tüm adres alanı say, pay 0 → her çağrıda yeniden okuma (pahalı)
            // yok; bu iş parçacığında denetim kapalı, VM::MAX_NATIVE_DEPTH sayacı geçerli kalır.
            r.lo = nullptr; r.hi = reinterpret_cast<const char*>(UINTPTR_MAX); r.reserve = 0;
            return false;
        }
        const size_t size = static_cast<size_t>(hi - lo);
        r.lo = lo; r.hi = hi;
        const size_t eighth = size / 8;
        r.reserve = eighth < LOOK_STACK_RESERVE_MIN ? LOOK_STACK_RESERVE_MIN
                  : eighth > LOOK_STACK_RESERVE_MAX ? LOOK_STACK_RESERVE_MAX : eighth;
    }
    return static_cast<size_t>(sp - r.lo) < r.reserve;   // yığın aşağı doğru büyür
}

// Sözdizimi ağacını özyinelemeyle gezen her yer (ayrıştırıcı, yakalama / parametre / argüman
// denetimleri, tanımsız çağrı arama) girişte bunu çağırır: yığın azaldıysa ayrıştırma hatası
// (satır/sütunlu), çökme değil. Ayrıştırıcının sayaçları (iç içe 150, zincir 1000) gerçek yığına
// bakmaz: iç içe her parantez ~15 C++ çerçevesi harcar → 128 KB'ta 140 parantez, 256 KB'ta 140
// iç içe blok (yakalama denetimi) çöküyordu (ölçüldü). Gövde parser.cpp'de (LookParseError).
[[noreturn]] void look_throw_nested_too_deep(int line, int col);
inline void look_ast_guard(int line = 0, int col = 0) {
    if (look_native_stack_low()) look_throw_nested_too_deep(line, col);
}

} // namespace look
