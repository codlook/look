#pragma once
// DKIM-Signature tag-list parser (RFC 6376 §3.2) — SAF fonksiyon, I/O yok.
// "Dikişi aç" deseni: dkim.cpp bunu include eder, dkim_tag_test.cpp da → tek tanım,
// drift yok, doğrudan fuzz/KAT'lanabilir.
//
// ESKİ HATA (①, iki bağımsız ölçümle doğrulandı): `dkim_hdr_raw.find(name + "=")`
// tag adını ALT DİZE olarak arıyordu → `tag("h")` "bh=" içindeki "h="e çarpıyordu.
// `bh=` başlıkta `h=`'den ÖNCE gelince (RFC sıra zorunlu KILMAZ, saldırgan sıralar),
// imzalanan-başlık-listesi (h=) yerine gövde-hash'i (bh=) okunuyordu → hiçbir başlık
// kanonik edilmez → From: imza kapsamı dışında kalabilirdi (doğrulama mantığı hatası).
//
// RFC-UYUMLU: tag-list = tag-spec *( ";" tag-spec ); tag-spec = tag-name "=" tag-value.
// `;` ile böl, her parçayı '='de ayır, tag ADINI TAM eşleştir (alt-dize değil).
#include <string>
#include <algorithm>

namespace look {

inline std::string dkim_tag(const std::string& dkim_header, const std::string& name) {
    // dkim_header = "DKIM-Signature: v=1; a=...; h=...; bh=...; b=..." (unfold edilmiş).
    // Değer '=' sonrası, bir sonraki ';'ye kadar; iç boşluk (WSP/CRLF, folding) atılır.
    size_t colon = dkim_header.find(':');
    const std::string body = (colon != std::string::npos) ? dkim_header.substr(colon + 1)
                                                           : dkim_header;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t semi = body.find(';', pos);
        std::string seg = (semi == std::string::npos) ? body.substr(pos)
                                                       : body.substr(pos, semi - pos);
        // segmenti trim et (folding'den gelen WSP/CRLF dahil)
        size_t a = seg.find_first_not_of(" \t\r\n");
        if (a != std::string::npos) {
            size_t b = seg.find_last_not_of(" \t\r\n");
            seg = seg.substr(a, b - a + 1);
            size_t eq = seg.find('=');
            if (eq != std::string::npos) {
                std::string tname = seg.substr(0, eq);
                // '=' etrafındaki WSP RFC'de serbest → tag adını da trim et
                size_t ta = tname.find_first_not_of(" \t");
                if (ta != std::string::npos) {
                    size_t tb = tname.find_last_not_of(" \t");
                    tname = tname.substr(ta, tb - ta + 1);
                }
                if (tname == name) {
                    std::string val = seg.substr(eq + 1);
                    val.erase(std::remove_if(val.begin(), val.end(),
                        [](char c){ return c==' '||c=='\t'||c=='\r'||c=='\n'; }), val.end());
                    return val;
                }
            }
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return "";
}

} // namespace look
