#include <string>
#include <cctype>
#include <cstdio>
static std::string lc(std::string s){for(char&c:s)c=(char)std::tolower((unsigned char)c);return s;}
// ESKI (bug'li)
bool old_aligned(std::string fd,std::string dd){
    fd=lc(fd);dd=lc(dd);
    if (fd!=dd && fd.size()>dd.size()+1 && fd.substr(fd.size()-dd.size()-1)!="."+dd) return false;
    if (fd.size()<dd.size()) return false;
    return true;
}
// YENI (fix)
bool new_aligned(std::string fd,std::string dd){
    fd=lc(fd);dd=lc(dd);
    if(fd.empty()||dd.empty())return false;
    return (fd==dd) || (fd.size()>dd.size() && fd.compare(fd.size()-dd.size()-1,dd.size()+1,"."+dd)==0);
}
struct C{const char*from;const char*d;bool legit;};
int main(){
    C cases[]={
        {"paypal.com","paypal.com",true},      // exact — geçmeli
        {"mail.example.com","example.com",true},// parent — geçmeli
        {"paypal.com","evildom.co",false},     // BYPASS: ayni uzunluk uyumsuz — REDDEDILMELI
        {"paypal.com","aypal.com",false},      // dd 9-char, fd 10 — REDDEDILMELI (suffix "aypal.com" degil ".aypal.com")
        {"bank.com","evilbank.com",false},     // fd kisa
        {"a.paypal.com","notpaypal.com",false},// suffix "paypal.com" ama nokta-siniri yok
    };
    int fail=0;
    for(auto&c:cases){
        bool o=old_aligned(c.from,c.d), n=new_aligned(c.from,c.d);
        bool ok = (n==c.legit);
        printf("From=%-16s d=%-14s legit=%d  ESKI=%d YENI=%d  %s%s\n",
            c.from,c.d,c.legit,o,n, ok?"OK":"FAIL",
            (o!=n)?"  <- davranis degisti":"");
        if(!ok)fail++;
    }
    printf(fail?"\n%d FAIL\n":"\nTUM VAKALAR GECTI\n",fail);
    return fail;
}
