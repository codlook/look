#pragma once
// LOOK 2 — kurulumda (üst düzeyde) kaydedilen geri çağırmaların VM'e devri.
//
// Eskiden web uygulamasının üst düzeyi önce yorumlayıcıda çalışıyordu (ilk geçiş), sonra VM aynı
// kodu geri oynatarak kendi rotalarını kuruyordu; `timer::after/every` ve `jobs::worker` geri
// çağırmaları ilk geçişte kaydedildiği için VM modunda bile yorumlayıcıda koşuyordu.
//
// Devir (o dönemin): ilk geçiş her zamanlayıcıyı bir YUVA üzerinden kurup iş işleyicilerini
// sırasıyla kaydediyordu; VM kurulumu BAŞARIYLA bitince yuvalar ve işleyiciler VM closure'larıyla
// değiştiriliyordu. LOOK 2'de ilk geçiş yok: bugün yalnız vm_closure_runner kullanılır,
// CallbackSlot / SetupHandoff'u çağıran yok.
#include "look/runtime_host.h"
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
// değerleri); web sunucusu ve komut satırı (cli_runtime) kurar. ok=false ise hata
// loglanmıştır. Kurulmamışsa boştur.
using VmClosureRunner = std::function<Value(const Value& fn, std::vector<Value> args, const char* what, bool& ok)>;
inline VmClosureRunner& vm_closure_runner() { static VmClosureRunner r; return r; }

} // namespace look
