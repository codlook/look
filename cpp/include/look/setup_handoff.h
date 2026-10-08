#pragma once
// LOOK 2 — kurulumda (üst düzeyde) kaydedilen geri çağırmaların VM'e devri.
//
// Web uygulamasının üst düzeyi önce yorumlayıcıda çalışır (ilk geçiş), sonra VM aynı kodu
// geri oynatarak kendi rotalarını kurar. `timer::after/every` ve `jobs::worker` ilk geçişte
// GERÇEKTEN kaydedilir — geri çağırmaları yorumlayıcı fonksiyonlarıdır ve VM modunda bile
// yorumlayıcıda koşuyorlardı (uygulamanın geri kalanı VM'deyken).
//
// Devir: ilk geçiş her zamanlayıcıyı bir YUVA üzerinden kurar ve iş işleyicilerini sırasıyla
// kaydeder. VM geçişi aynı çağrıları aynı sırada görür ve kendi closure'larını (BYTECODE_FN)
// bekletir. VM kurulumu BAŞARIYLA bitince yuvalar ve işleyiciler VM closure'larıyla değiştirilir;
// başarısız olursa hiçbir şey değişmez, uygulama yorumlayıcıda kaldığı gibi geri çağırmalar da
// orada kalır. Zamanlayıcı kimlikleri değişmez (aynı kayıt, yalnız çağırdığı şey değişir).
#include "look/interpreter.h"
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace look {

// Zamanlayıcının çağırdığı şey: kurulumdan sonra değiştirilebilir (zamanlayıcı iş parçacığı okur).
struct CallbackSlot {
    void set(std::function<void()> f) { std::lock_guard<std::mutex> lk(m_); fn_ = std::move(f); }
    void call() {
        std::function<void()> f;
        { std::lock_guard<std::mutex> lk(m_); f = fn_; }
        if (f) f();
    }
private:
    std::mutex m_;
    std::function<void()> fn_;
};

struct SetupHandoff {
    std::vector<std::shared_ptr<CallbackSlot>> timer_slots;   // ilk geçişte üst düzeyde kurulan zamanlayıcılar, sırayla
    size_t worker_base = 0;                                    // ilk geçişten ÖNCE kayıtlı iş işleyicisi sayısı
    void reset(size_t workers_before) { timer_slots.clear(); worker_base = workers_before; }
};
inline SetupHandoff& setup_handoff() { static SetupHandoff h; return h; }

// Bir VM closure'ını YENİ BİR İSTEK gibi çalıştırır (kendi VM'i, global'lerin bildirilen
// değerleri); web sunucusu kurar. ok=false ise hata loglanmıştır. Kurulmamışsa boştur
// (komut satırı, yorumlayıcı modu).
using VmClosureRunner = std::function<Value(const Value& fn, std::vector<Value> args, const char* what, bool& ok)>;
inline VmClosureRunner& vm_closure_runner() { static VmClosureRunner r; return r; }

} // namespace look
