#!/bin/bash
# LOOK Language — kurulumu elle yeniden calistir (ROOT).
# Asil is sbin/look-setup'ta; post-install.php kayitli domain'leri ona verir. Eski belgeler bu
# dosyayi gosteriyordu — ayni yolu kullanmak icin post-install.php'yi calistirir.
# (`plesk php` ile calismaz: Plesk SDK — pm_Context/pm_ApiCli — yalniz --exec ile yuklenir.)
exec plesk bin extension --exec look-lang post-install.php
