Name:           look-lang
Version:        1.1.2
Release:        1%{?dist}
Summary:        LOOK — a language built for the web (embedded SMTP/IMAP, bytecode VM, DB clients)

License:        Apache-2.0
URL:            https://codlook.com
BuildArch:      x86_64

# Sistem OpenSSL'e DİNAMİK bağlı (kurumsal doğru yol): CVE'de 'dnf update
# openssl-libs' bizi de korur; crypto-policies/FIPS'e uyar. LOOK'un kendini
# güncellemesi: 'dnf update look-lang'.
Requires:       openssl-libs >= 1.1.1
Requires:       glibc

%description
LOOK is a single-binary language built for server-side web development.
Routing, databases (MySQL/MariaDB, PostgreSQL, SQLite, Redis), concurrency,
WebSocket/SSE, a template engine and an embedded SMTP/IMAP server are part of
the language, with minimal external runtime dependencies (this package links
the system OpenSSL dynamically).

Components:
  lk       — CLI / REPL / test runner
  lk-fcgi  — application server (--mode http or FastCGI)
  lk-cgi   — classic CGI bridge

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
echo "LOOK %{version} installed — check with 'lk --version'. Update: dnf update look-lang"

%changelog
* Tue Jul 08 2026 Codlook <info@codlook.com> - 1.0.0-1
- First release: embedded SMTP/IMAP, fn/app::/response::error syntax, VM
- Links the system OpenSSL dynamically (security updates through dnf)
