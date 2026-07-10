#include "look/dns.h"
#include <stdexcept>
#include <cstring>

#if defined(__linux__) || defined(__APPLE__)
#  include <sys/types.h>
#  include <netinet/in.h>
#  include <arpa/nameser.h>
#  include <resolv.h>

namespace look {

std::vector<std::string> dns_txt_lookup(const std::string& fqdn) {
    std::vector<std::string> results;

    unsigned char answer[4096];
    int len = res_query(fqdn.c_str(), C_IN, T_TXT,
                        answer, (int)sizeof(answer));
    if (len < 0) {
        // NXDOMAIN or no answer — not an error, just empty
        return results;
    }

    ns_msg handle;
    if (ns_initparse(answer, len, &handle) < 0) return results;

    int count = ns_msg_count(handle, ns_s_an);
    for (int i = 0; i < count; ++i) {
        ns_rr rr;
        if (ns_parserr(&handle, ns_s_an, i, &rr) < 0) continue;
        if (ns_rr_type(rr) != T_TXT) continue;

        // TXT RDATA: series of length-prefixed strings
        const unsigned char* rd  = ns_rr_rdata(rr);
        size_t               rdl = ns_rr_rdlen(rr);
        std::string txt;
        size_t pos = 0;
        while (pos < rdl) {
            uint8_t seg_len = rd[pos++];
            if (pos + seg_len > rdl) break;
            txt.append((const char*)rd + pos, seg_len);
            pos += seg_len;
        }
        if (!txt.empty()) results.push_back(std::move(txt));
    }
    return results;
}

} // namespace look

#elif defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <windns.h>
#  pragma comment(lib, "Dnsapi.lib")

namespace look {

std::vector<std::string> dns_txt_lookup(const std::string& fqdn) {
    std::vector<std::string> results;

    PDNS_RECORD pRecords = nullptr;
    DNS_STATUS status = DnsQuery_A(
        fqdn.c_str(), DNS_TYPE_TEXT, DNS_QUERY_STANDARD,
        nullptr, &pRecords, nullptr);

    if (status != ERROR_SUCCESS) return results; // NXDOMAIN or error

    for (PDNS_RECORD r = pRecords; r != nullptr; r = r->pNext) {
        if (r->wType != DNS_TYPE_TEXT) continue;
        std::string txt;
        for (DWORD s = 0; s < r->Data.TXT.dwStringCount; ++s)
            if (r->Data.TXT.pStringArray[s])
                txt += r->Data.TXT.pStringArray[s];
        if (!txt.empty()) results.push_back(std::move(txt));
    }

    DnsRecordListFree(pRecords, DnsFreeRecordList);
    return results;
}

} // namespace look

#else
namespace look {
std::vector<std::string> dns_txt_lookup(const std::string&) {
    return {}; // unsupported platform
}
} // namespace look
#endif
