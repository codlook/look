#pragma once
// Bosta bekleyen bir veritabani soketinin kullanilabilirligi — HICBIR SEY GONDERMEDEN.
//
// Bir sorgunun cevabi tamamen okunduktan sonra soketin okunabilir olmamasi gerekir. Okunabilir
// ya da hatali ise sunucu baglantiyi kapatmistir (FIN/RST: wait_timeout, yeniden baslatma, KILL,
// pg_terminate_backend) ya da kapatmadan once bir hata paketi yollamistir (MySQL 8 wait_timeout).
// Sorgudan ONCE anlasilir → yeni baglanti guvenli, cunku hicbir sey gonderilmedi (tekrar yok).
#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <poll.h>
#  include <cerrno>
#endif

namespace look {

template <class Sock>
inline bool idle_socket_unusable(Sock fd) {
#ifdef _WIN32
    WSAPOLLFD p{};
    p.fd = fd;
    p.events = POLLRDNORM;
    return WSAPoll(&p, 1, 0) != 0;   // okunabilir, hata ya da kapanmis
#else
    pollfd p{};
    p.fd = fd;
    p.events = POLLIN;
    int r;
    do { r = ::poll(&p, 1, 0); } while (r < 0 && errno == EINTR);
    return r != 0;                   // okunabilir (veri/EOF), POLLERR/POLLHUP ya da poll hatasi
#endif
}

} // namespace look
