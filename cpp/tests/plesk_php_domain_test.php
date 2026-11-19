<?php
/**
 * LOOK — Plesk index.php input-validation injection-regression guard (NO NETWORK).
 *
 * index.php passes domain/script to enable.sh on the add/edit action (from there the unquoted
 * heredoc → systemd unit → sudo restart → root). FIRST defense layer:
 *   - the domain FQDN regex (line 229)
 *   - the script path regex + '..' check (line 233)
 *   - the valid_svc() look-* regex (line 118) — closes path-injection
 *
 * This guard EXTRACTS the regexes FROM THE REAL index.php (not a copy): if someone
 * weakens a regex the test goes red. NO Plesk/network needed — pure preg_match.
 *
 * Run:  php cpp/tests/_plesk_php_domain_test.php
 */

$src = __DIR__ . '/../../platforms/plesk/htdocs/index.php';
if (!is_file($src)) { fwrite(STDERR, "index.php yok: $src\n"); exit(2); }
$code = file_get_contents($src);

$fail = 0;
function pass($m){ echo "  \033[32mPASS\033[0m $m\n"; }
function fl($m){ global $fail; echo "  \033[31mFAIL\033[0m $m\n"; $fail=1; }

/* --- Extract the regexes from the source --- */
// domain:  if (!preg_match('/^(?=.{1,253}$)[a-z0-9]...$/i', $domain))
if (!preg_match('/preg_match\(\s*(\'(?:\\\\.|[^\'\\\\])*\')\s*,\s*\$domain\s*\)/', $code, $m)) {
    fl("domain preg_match satırı çıkarılamadı (kaldırılmış?)"); $DOMAIN_RE = null;
} else { $DOMAIN_RE = eval("return {$m[1]};"); pass("domain regex çıkarıldı: {$m[1]}"); }

// script: preg_match('#^/var/www/vhosts/[...]\.lk$#', $script)
if (!preg_match('/preg_match\(\s*(\'(?:\\\\.|[^\'\\\\])*\')\s*,\s*\$script\s*\)/', $code, $m)) {
    fl("script preg_match satırı çıkarılamadı"); $SCRIPT_RE = null;
} else { $SCRIPT_RE = eval("return {$m[1]};"); pass("script regex çıkarıldı: {$m[1]}"); }

// valid_svc: return (bool)preg_match('/^look-[a-z0-9-]+$/', $svc);
if (!preg_match('/preg_match\(\s*(\'(?:\\\\.|[^\'\\\\])*\')\s*,\s*\$svc\s*\)/', $code, $m)) {
    fl("valid_svc preg_match satırı çıkarılamadı"); $SVC_RE = null;
} else { $SVC_RE = eval("return {$m[1]};"); pass("valid_svc regex çıkarıldı: {$m[1]}"); }

/* --- Test vectors --- */
$bad_domains = [
    "look.com\nExecStartPre=/bin/touch /tmp/pwn",   // line-end unit injection (the actual privesc)
    "look.com\n[Service]\nExecStart=/bin/sh",
    "look.com; touch /tmp/pwn",
    "look.com\$(id)",
    "look.com`id`",
    "look.com && reboot",
    "look.com | nc x 9",
    "../../etc/passwd",
    "look .com",                                     // space
    "look.com/",                                     // slash
    "",                                              // empty
    "look.com\r\nExecStart=/evil",                   // CRLF
];
$good_domains = ["look.codlook.com", "qrmenu.codlook.com", "a.bc", "sub.domain.example.org"];

$bad_scripts = [
    "/var/www/vhosts/x/app.lk\nExecStart=/evil",
    "/var/www/vhosts/../../etc/x.lk",                // traversal
    "/etc/passwd",                                   // outside the prefix
    "/var/www/vhosts/x/app.lk; rm -rf /",
    "/var/www/vhosts/x/app.php",                     // not .lk
    "/var/www/vhosts/x/\$(id).lk",
];
$good_scripts = ["/var/www/vhosts/codlook.com/look.codlook.com/index.lk"];

$bad_svcs  = ["look-../../tmp/evil", "look-x/../../y", "sshd", "look-x;rm", "look-a\nb", "mariadb"];
$good_svcs = ["look-look-codlook-com", "look-qrmenu-codlook-com"];

function check_reject($re, $inputs, $label){
    foreach ($inputs as $in) {
        if ($re === null) return;
        $shown = str_replace(["\n","\r"], ["\\n","\\r"], $in);
        // preg_match without /m: '$' doesn't allow trailing newline unless /D absent...
        if (preg_match($re, $in)) fl("$label KABUL etti (enjeksiyon geçti): [$shown]");
        else pass("$label reddetti: [$shown]");
    }
}
function check_accept($re, $inputs, $label){
    foreach ($inputs as $in) {
        if ($re === null) return;
        if (preg_match($re, $in)) pass("$label kabul etti (meşru): [$in]");
        else fl("$label meşru girdiyi reddetti (regresyon): [$in]");
    }
}

echo "== 1) domain regex: kötü-niyetli girdileri REDDET ==\n";
check_reject($DOMAIN_RE, $bad_domains, "domain");
echo "== 2) domain regex: meşru domain'leri KABUL et ==\n";
check_accept($DOMAIN_RE, $good_domains, "domain");

echo "== 3) script regex: kötü yolları REDDET ('..' de ayrıca elenir) ==\n";
// in index.php the script is also filtered by str_contains(..'..'); the regex is tested alone.
foreach ($bad_scripts as $s) {
    if ($SCRIPT_RE === null) break;
    $shown = str_replace(["\n","\r"], ["\\n","\\r"], $s);
    $ok_regex = preg_match($SCRIPT_RE, $s);
    $has_dotdot = strpos($s, '..') !== false;   // extra check in index.php
    $accepted = $ok_regex && !$has_dotdot;
    $accepted ? fl("script KABUL etti: [$shown]") : pass("script reddetti: [$shown]");
}
echo "== 4) script regex: meşru yolu KABUL et ==\n";
check_accept($SCRIPT_RE, $good_scripts, "script");

echo "== 5) valid_svc: path-injection svc'yi REDDET, meşru svc'yi KABUL ==\n";
check_reject($SVC_RE, $bad_svcs, "svc");
check_accept($SVC_RE, $good_svcs, "svc");

/* --- POSITIVE CONTROL: if the source were weakened (permissive regex) the injection would pass --- */
echo "== 6) POZİTİF KONTROL: naif regex privesc payload'ını KABUL eder (test ayırt edici) ==\n";
$naive = '/.+/';
$evil  = "look.com\nExecStartPre=/bin/touch /tmp/pwn";
if (preg_match($naive, $evil) && $DOMAIN_RE !== null && !preg_match($DOMAIN_RE, $evil)) {
    pass("naif regex enjeksiyonu geçirir; gerçek regex engeller → guard ayırt edici");
} else {
    fl("pozitif kontrol beklendiği gibi ayrışmadı");
}

echo "\n";
if ($fail === 0) { echo "== TÜM PHP GİRİŞ-DOĞRULAMA GUARD'LARI GEÇTİ ==\n"; exit(0); }
echo "== PHP GUARD BAŞARISIZ ==\n"; exit(1);
