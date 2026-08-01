// HTML metin-içerik kaçışı — tek tanım (html::escape + TemplateEngine ikisi de kullanır).
// B-08: backtick (`` ` ``) da kaçırılır — JS template-literal `${...}` XSS'ini kapatır.
// NOT: html::attr AYRI bir kavramdır (izin-listesi: alfanümerik+UTF-8 dışında her şeyi kaçırır,
// öznitelik bağlamı için daha sağlam) — buraya birleştirilmez. "Tek yapı" AYNI kavramın iki
// kopyası içindir; escape ve attr iki farklı kavram.
#pragma once
#include <string>

namespace look {

inline std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            case '`':  out += "&#96;";  break;   // B-08: JS template-literal XSS
            default:   out += (char)c;
        }
    }
    return out;
}

} // namespace look
