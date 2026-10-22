#pragma once
// Yerleşik modüllerin çalışma zamanından istediği her şey — ve yalnız bu kadarı.
//
// Modül fabrikaları (make_stdlib, make_extra_stdlib, make_web_modules, …) eskiden
// yorumlayıcı nesnesini (bugünkü RuntimeHost) alıyordu; oysa modüller ondan yalnız şu dört
// şeyi kullanıyor. LOOK 2 tek motora (VM) geçerken modül tablosunu yorumlayıcı nesnesi
// olmadan kurabilmek için bağımlılık bu arayüze indirildi. Bugün RuntimeHost uygular; VM
// kendi ana makinesini getirdiğinde modüllere dokunmak gerekmez.
#include <vector>

namespace look {

class Value;   // value.h (ileri bildirim — bu başlık her yerden ucuzca dahil edilebilsin)

class ModuleHost {
public:
    virtual ~ModuleHost() = default;

    // Bir LOOK fonksiyon değerini çağırır (array::map/filter/…, db::transaction, http::stream
    // geri çağırmaları). Değer VM closure'ıysa etkin VM'de çalışır.
    virtual Value invoke(const Value& fn, std::vector<Value> args) = 0;

    // Üst düzey kurulum (setup) sürüyor mu — db::query kurulumdaki yan etkiyi buna göre ele alır.
    virtual bool is_setup_mode() const = 0;

    // runtime::stats sayaçları.
    virtual int  get_route_count()   const = 0;
    virtual long get_uptime_sec()    const = 0;
};

} // namespace look
