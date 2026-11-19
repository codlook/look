#!/bin/bash
# LOOK Language — re-run the installation by hand (ROOT).
# The real work is in sbin/look-setup; post-install.php gives it the registered domains. Old docs pointed to this
# file — it runs post-install.php so the same path is used.
# (It does not work with `plesk php`: the Plesk SDK — pm_Context/pm_ApiCli — is loaded only with --exec.)
exec plesk bin extension --exec look-lang post-install.php
