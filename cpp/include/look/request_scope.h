#pragma once
// İstek tutamacı — şu an çalışan isteğin bağlamı ve çıktısı.
//
// Web modülleri (request::, response::, cookie::, session::, csrf::, route::) ve print bağlamı
// yakalamaz; çağrıldıkları anda buradan okur. Böylece modüller ve builtin tablosu iş parçacığı
// başına bir kez kurulur. Eskiden her istekte yeniden kuruluyordu (fiber yığınında bağlamın
// adresi istek başına değiştiği için) — ölçüm: istek maliyetinin büyük kısmı, VM'in kendisi %2.
//
// Saklama yeri: fiber içinde Fiber::current()->local (her fiber'ın kendi isteği — askıya alınan
// istek, aynı iş parçacığında sonra başlayan isteği görmez); fiber dışında (işçi havuzu, FastCGI,
// CGI, zamanlayıcı iş parçacığı) iş parçacığı yereli. RequestScope önceki değeri saklar ve geri
// yükler — iç içe dağıtım (zamanlayıcı/WS geri çağırması) dış isteği bozmaz.
#include <ostream>

namespace look {

struct WebContext;

// Şu anki isteğin bağlamı / çıktısı; istek yoksa nullptr.
WebContext*   current_request_web();
std::ostream* current_request_out();

class RequestScope {
public:
    RequestScope(WebContext* web, std::ostream* out);
    ~RequestScope();
    RequestScope(const RequestScope&) = delete;
    RequestScope& operator=(const RequestScope&) = delete;
private:
    WebContext*   prev_web_;
    std::ostream* prev_out_;
};

} // namespace look
