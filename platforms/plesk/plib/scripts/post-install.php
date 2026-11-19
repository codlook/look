<?php
// Plesk extension install/upgrade hook. Plesk runs this on install AND on upgrade
// (on upgrade argv = ["upgrade", "<old release>"]) — but NOT as ROOT, as psaadm (measured).
// The work that needs root (binary, sudoers, service units) is in sbin/look-setup; it is reached through Plesk's
// root path pm_ApiCli::callSbin. The registered domains are passed as arguments so that
// every service is rewritten according to the new rule (the domain user).
// Errors are NOT silenced: the output is printed, a non-zero exit shows up in Plesk as an install error.
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
        (string)($d['mode'] ?? 'fcgi'), (string)(int)($d['port'] ?? 9100), (string)($d['env'] ?? ''),
        (string)($d['memory'] ?? 'auto'));   // memory cap: absent in an old record → automatic
}

$r = pm_ApiCli::callSbin('look-setup', $args, pm_ApiCli::RESULT_FULL);
if (($r['stdout'] ?? '') !== '') fwrite(STDOUT, $r['stdout']);
if (($r['stderr'] ?? '') !== '') fwrite(STDERR, $r['stderr']);
if ((int)($r['code'] ?? 1) !== 0) {
    fwrite(STDERR, "LOOK Language setup failed (exit " . (int)($r['code'] ?? 1) . "). Fix the error above and install the extension again.\n");
    exit(1);
}
