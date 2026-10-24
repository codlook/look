<?php
// Plesk extension install/upgrade hook. Plesk bunu kurulumda VE yukseltmede calistirir
// (yukseltmede argv = ["upgrade", "<eski surum>"]) — ama ROOT DEGIL, psaadm olarak (olculdu).
// Root gerektiren is (ikili, sudoers, servis unit'leri) sbin/look-setup'ta; ona Plesk'in
// root yolu pm_ApiCli::callSbin ile gidilir. Kayitli domain'ler arguman olarak verilir ki
// her servis yeni kurala (domain kullanicisi) gore yeniden yazilsin.
// Hata SUSTURULMAZ: cikti basilir, sifir olmayan cikis Plesk'te kurulum hatasi olarak gorunur.
pm_Context::init('look-lang');

$domains = [];
foreach (['/usr/local/psa/var/modules/look-lang/domains.json', '/opt/look/conf/domains.json'] as $f) {
    if (!is_file($f)) continue;
    $d = json_decode((string)file_get_contents($f), true);
    if (is_array($d) && $d) { $domains = $d; break; }
}
$args = [];
foreach ($domains as $d) {
    if (empty($d['domain']) || empty($d['script'])) continue;
    array_push($args, (string)$d['domain'], (string)$d['script'], (string)(int)($d['workers'] ?? 4),
        (string)($d['mode'] ?? 'fcgi'), (string)(int)($d['port'] ?? 9100), (string)($d['env'] ?? ''));
}

$r = pm_ApiCli::callSbin('look-setup', $args, pm_ApiCli::RESULT_FULL);
if (($r['stdout'] ?? '') !== '') fwrite(STDOUT, $r['stdout']);
if (($r['stderr'] ?? '') !== '') fwrite(STDERR, $r['stderr']);
if ((int)($r['code'] ?? 1) !== 0) {
    fwrite(STDERR, "LOOK Language setup failed (exit " . (int)($r['code'] ?? 1) . "). Fix the error above and install the extension again.\n");
    exit(1);
}
