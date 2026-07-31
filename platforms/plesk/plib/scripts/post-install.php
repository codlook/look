<?php
// Plesk extension install hook — Plesk bunu KURULUMDA ROOT olarak calistirir.
// (Kök-dizindeki shell `post-install` Plesk tarafindan CALISTIRILMIYOR; Plesk yalniz
//  plib/scripts/post-install.php kosar — git/firewall gibi.) Bu, binary'yi /opt/look'a
// kurar + /etc/sudoers.d/look-lang olusturur. Sudoers OLMAZSA controller enable.sh'i
// `sudo` ile cagirinca "a password is required" hatasi (web'de terminal yok).
$setup = '/usr/local/psa/admin/htdocs/modules/look-lang/scripts/setup.sh';
if (is_file($setup)) {
    $out = [];
    $rc = 0;
    exec('/bin/bash ' . escapeshellarg($setup) . ' 2>&1', $out, $rc);
    foreach ($out as $line) {
        fwrite(STDERR, "look-lang setup: $line\n");
    }
    if ($rc !== 0) {
        fwrite(STDERR, "look-lang: setup.sh exit=$rc\n");
    }
} else {
    fwrite(STDERR, "look-lang: setup.sh bulunamadi ($setup)\n");
}
