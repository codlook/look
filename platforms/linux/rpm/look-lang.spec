Name:           look-lang
Version:        1.0.0
Release:        1%{?dist}
Summary:        LOOK — sıfır bağımlılık web scripting dili (gömülü SMTP/IMAP, VM, DB)

License:        Apache-2.0
URL:            https://codlook.com
BuildArch:      x86_64

# Sistem OpenSSL'e DİNAMİK bağlı (kurumsal doğru yol): CVE'de 'dnf update
# openssl-libs' bizi de korur; crypto-policies/FIPS'e uyar. LOOK'un kendini
# güncellemesi: 'dnf update look-lang'.
Requires:       openssl-libs >= 1.1.1
Requires:       glibc

%description
LOOK, sunucu taraflı web geliştirme için tasarlanmış tek-binary bir scripting
dilidir. Routing, DB (MySQL/SQLite/PostgreSQL), eşzamanlılık, WebSocket/SSE,
şablon motoru ve gömülü SMTP/IMAP sunucusu — hepsi dile gömülü, sıfır 3rd-party
bağımlılık (yalnız sistem OpenSSL'e dinamik bağlı).

Bileşenler:
  lk       — CLI / interpreter / REPL / test runner
  lk-fcgi  — uygulama sunucusu (--mode http veya FastCGI)
  lk-cgi   — klasik CGI köprüsü

%prep
# Prebuilt binary paketleme — kaynak derleme burada yapılmaz.

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}%{_bindir}
install -m 0755 %{_sourcedir}/lk       %{buildroot}%{_bindir}/lk
install -m 0755 %{_sourcedir}/lk-fcgi  %{buildroot}%{_bindir}/lk-fcgi
install -m 0755 %{_sourcedir}/lk-cgi   %{buildroot}%{_bindir}/lk-cgi

%files
%{_bindir}/lk
%{_bindir}/lk-fcgi
%{_bindir}/lk-cgi

%post
echo "LOOK %{version} kuruldu — 'lk --version' ile doğrula. Güncelleme: dnf update look-lang"

%changelog
* Tue Jul 08 2026 Codlook <info@codlook.com> - 1.0.0-1
- İlk v1 sürümü: gömülü SMTP/IMAP, fn/app::/response::error sözdizimi, VM
- Sistem OpenSSL'e dinamik bağlı (dnf ile güvenlik güncellemesi)
