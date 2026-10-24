#pragma once
// Bağımlı bir servis (oturum deposu Redis gibi) yanıt vermiyor ya da erişilemiyor: istek
// uygulamanın hatası değil, geçici bir hizmet kesintisi. LOOK kodu yakalamazsa dağıtıcı
// 500 yerine 503 Service Unavailable döner ve hatayı günlüğe yazar. LOOK try/catch onu
// sıradan bir hata mesajı olarak görür.
#include <stdexcept>
#include <string>

namespace look {

class ServiceUnavailable : public std::runtime_error {
public:
    explicit ServiceUnavailable(const std::string& msg) : std::runtime_error(msg) {}
};

} // namespace look
