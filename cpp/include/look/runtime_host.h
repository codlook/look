#pragma once
#include "look/value.h"
#include "look/module_host.h"   // ModuleHost — yerleşik modüllerin çalışma zamanı arayüzü
#include <sstream>

namespace look {

// ── RuntimeHost — yerleşik modüllerin ev sahibi ───────────────────────────────
// LOOK 2'de programı yalnız VM çalıştırır. Bu sınıf artık bir yorumlayıcı DEĞİL: modül
// tablosunu (stdlib + web çekirdeği + `use` edilenler), çıktı akışını, istek bağlamını ve
// app:: servis kaydını tutar; modül fonksiyonları geri çağırmayı invoke() ile VM'e verir.

class RuntimeHost : public ModuleHost {
public:
    RuntimeHost();
    explicit RuntimeHost(std::ostream& out);
    void  set_web_context(WebContext* ctx);
    void  set_output(std::ostream& out) { output_stream_ = &out; }
    void  set_output(std::ostream* out) { output_stream_ = out; }
    // Geri çağırmanın (iş işleyicisi, zamanlayıcı, parallel görevi) çıktısı nereye gider: komut
    // satırında (çıktı std::cout) stdout'a — ESKİ HATA: ayrı bir tampona yazılıp atılıyordu, bir iş
    // betiğindeki `print` sessizce kayboluyordu. Web'de geri çağırmanın yazacağı bir yanıt yok →
    // tampon (eskisi gibi).
    std::ostream& callback_output(const std::shared_ptr<std::ostringstream>& sink) const;
    std::ostream& output() const { return *output_stream_; }

    // Kurulum (üst düzey) sürüyor mu — db::query kurulumdaki yan etkiyi buna göre ele alır.
    void set_setup_mode(bool m) { setup_mode_ = m; }
    bool is_setup_mode()  const override { return setup_mode_; }

    Value invoke(const Value& fn, std::vector<Value> args) override;
    Value jobs_run(int interval_ms);   // jobs::run — web ve komut satırı için tek tanım

    // runtime:: stats. Rota sayısını web motoru kurulumdan sonra yazar (VM'in rota tablosu).
    void set_route_count(int n) { route_count_ = n; }
    int  get_route_count()   const override { return route_count_; }
    long get_uptime_sec()    const override {
        auto now = std::chrono::steady_clock::now();
        return (long)std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
    }

    void set_file(const std::string& file) { current_file_ = file; if (main_script_.empty()) main_script_ = file; }

    // İş parçacığı kopyası: aynı modül durumu ve servis kaydı, kendi çıktısı ve bağlamı.
    // Kopyada set_output() ve set_web_context() çağrılır.
    std::unique_ptr<RuntimeHost> make_dispatch_copy() const;

    // VM builtin wiring: set_web_context() sonrası modules_'dan fonksiyon al.
    // module = "response", fn = "header" → modules_["response"].functions["header"]
    // Bulunamazsa null std::function döner.
    NativeFn get_module_fn(const std::string& module_name, const std::string& fn_name) const;

    // Bir stdlib modülünü modules_'a yükler (`use` semantiği). Stdlib'de yoksa (paket modülü)
    // false — paket modülünü derleyici derler.
    bool load_stdlib_module(const std::string& name);
    // `use` isteyen bir stdlib modülü olup `use` edilmemiş mi? Öyleyse VM o modülün fonksiyonu
    // için "Module 'x' not loaded." verir — bağlayıcı yuvayı boş bırakır. (`use` istemeyen
    // modüller — web çekirdeği, ws/sse … — burada false döner.)
    bool module_not_used(const std::string& name) const {
        return stdlib_.count(name) != 0 && modules_.count(name) == 0;
    }
    // Yüklenebilir tüm modüller (stdlib + web çekirdeği): VM kapsam raporu için (lk --vm-report).
    const std::map<std::string, Module>& stdlib_modules() const { return stdlib_; }

private:
    std::ostream* output_stream_;
    std::map<std::string, Module> modules_;
    std::map<std::string, Module> stdlib_;

    // Servis kaydı — app::set/get (setup'ta yazılır, tüm dispatch'lerce paylaşılır)
    std::shared_ptr<ServiceRegistry> services_ = std::make_shared<ServiceRegistry>();
    void register_app_module();   // constructor'da çağrılır — app:: core modülü
    WebContext* web_ctx_ = nullptr;

    std::string main_script_;    // proje kök scripti — değişmez
    std::string current_file_;
    bool        setup_mode_  = false;
    int         route_count_ = 0;

    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();
};

// .env-aware env() — VM setup builtins için (g_env_vars + system env)
std::string look_get_env(const std::string& key, const std::string& default_val = "");

} // namespace look
