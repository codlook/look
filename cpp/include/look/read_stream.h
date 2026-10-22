#pragma once
// Bir akışın kalanını tek seferde dizgiye okur (filebuf'tan toplu kopya).
//
// istreambuf_iterator ile okumak dosyayı karakter karakter alıp dizgiyi defalarca büyütüyordu:
// ölçüm, qr-menu CGI isteğinin komutlarının %7'si (kaynak + dahil edilen dosyalar). Akışın
// kipi aynen korunur — metin kipinde açılmış dosyada Windows'un \r\n → \n çevirisi eskisi gibi.
#include <istream>
#include <sstream>
#include <string>

namespace look {

inline std::string read_stream(std::istream& in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace look
