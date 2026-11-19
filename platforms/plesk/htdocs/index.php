<?php
/**
 * LOOK Language — Plesk Extension
 * AJAX: returns JSON based on the action parameter
 * Page: rendered inside the Plesk frame via pm_Application->run()
 */

define('LOOK_OPT',     '/opt/look');
define('LOOK_CONF',    LOOK_OPT . '/conf');
define('LOOK_BIN',     LOOK_OPT . '/lk-fcgi');
define('SCRIPTS_DIR',  __DIR__ . '/scripts');
define('BASE_PORT',    9100);

// pm_ApiCli::callSbin needs the extension's context. The panel reaches this file through IndexController (which already
// has the context) or directly via /modules/look-lang/index.php — the latter has no context.
pm_Context::init('look-lang');

// State (domains.json) — kept in Plesk's panel-writable module var directory.
// /opt/look/conf belongs to root (the dnf/RPM binary folder); the panel runs as psaadm
// and CANNOT WRITE there → a domain added would not be saved and would not show in the list. This directory
// is owned by psaadm. For CLI/fallback it falls back to /opt/look/conf.
define('LOOK_VAR', is_dir('/usr/local/psa/var/modules/look-lang')
    ? '/usr/local/psa/var/modules/look-lang'
    : LOOK_CONF);
define('DOMAINS_JSON', LOOK_VAR . '/domains.json');

function auto_setup(): void {
    // The binary, sudoers and service units are ROOT's job: the install/upgrade hook
    // (plib/scripts/post-install.php → pm_ApiCli::callSbin('look-setup')) does them. The panel runs as
    // psaadm; an attempt to copy into /opt/look here always failed with "Permission denied" and
    // wrote a warning to the Plesk log.

    // State directory + one-time migration from the old /opt/look/conf/domains.json
    if (!is_dir(LOOK_VAR)) @mkdir(LOOK_VAR, 0755, true);
    if (!file_exists(DOMAINS_JSON)) {
        $legacy = LOOK_CONF . '/domains.json';
        $seed = (file_exists($legacy) && trim(@file_get_contents($legacy)) !== '[]')
            ? @file_get_contents($legacy) : '[]';
        @file_put_contents(DOMAINS_JSON, $seed ?: '[]');
    }
}
auto_setup();

function load_domains(): array {
    if (!file_exists(DOMAINS_JSON)) return [];
    $d = json_decode(file_get_contents(DOMAINS_JSON), true);
    return is_array($d) ? $d : [];
}
function save_domains(array $domains): bool {
    return file_put_contents(DOMAINS_JSON, json_encode($domains, JSON_PRETTY_PRINT)) !== false;
}
// Find the domain record in domains.json (safe — no raw path is TAKEN from the client).
function find_domain(array $domains, string $name): ?array {
    foreach ($domains as $d) if (($d['domain'] ?? '') === $name) return $d;
    return null;
}
// Validate a domain's registered script path: under /var/www/vhosts + with a .lk extension.
function safe_script_path(?array $d): ?string {
    if (!$d || empty($d['script'])) return null;
    $p = $d['script'];
    if (substr($p, -3) !== '.lk') return null;
    if (strpos($p, '/var/www/vhosts/') !== 0) return null;
    if (strpos($p, '..') !== false) return null;
    return $p;
}
// enable.sh arguments: <domain> <script> <workers> <mode> <port> <env-file> <memory>
// (old domains.json records have no 'env'/'memory' → no environment file, automatic memory cap)
function enable_args(array $d): array {
    return ['enable', (string)$d['domain'], (string)$d['script'], (string)(int)$d['workers'],
            (string)$d['mode'], (string)(int)$d['port'], (string)($d['env'] ?? ''), valid_memory((string)($d['memory'] ?? 'auto'))];
}
// The site's memory cap (systemd MemoryMax): "auto" (25% of the machine, at most 1G), "off" or 512M / 2G.
// enable.sh validates it too (it is written into the unit); here an invalid value falls back to "auto".
function valid_memory(string $m): string {
    $m = strtoupper(trim($m));
    if ($m === '' || $m === 'AUTO') return 'auto';
    if ($m === 'OFF') return 'off';
    return preg_match('/^[0-9]{1,7}[MG]$/', $m) ? $m : 'auto';
}
// Root operations: NO sudo. Plesk's root path is pm_ApiCli::callSbin → sbin/look-ctl (the action, service
// name and argument count are checked there). The arguments go without a shell (argv).
// Sudoers used to give psaadm root with the rule 'systemctl enable look-*'; since '*' also matches spaces,
// 'systemctl enable look-a /tmp/evil.service' was allowed too.
function ctl(array $args): array {
    $r = pm_ApiCli::callSbin('look-ctl', $args, pm_ApiCli::RESULT_FULL);
    return ['output' => trim(($r['stdout'] ?? '') . ($r['stderr'] ?? '')), 'code' => (int)($r['code'] ?? 1)];
}
function svc_status(string $svc): array {
    $out = ctl(['status', $svc])['output'];
    if (str_starts_with($out, 'active:')) return ['state'=>'active','pid'=>substr($out,7)];
    return ['state' => $out ?: 'unknown', 'pid' => ''];
}
// SECURITY: svc is always 'look-' + alphanumeric/dash (enable.sh SVC_NAME generation:
// [^a-z0-9] in the domain -> '-'). svc goes to systemctl as root (look-ctl applies the same rule once
// more); systemctl loads an argument containing '/' as a PATH -> 'look-../../tmp/evil.service'
// would have started the attacker's unit as ROOT. Reject an svc containing slash/'..' here too.
function valid_svc(string $svc): bool {
    return (bool)preg_match('/^look-[a-z0-9-]+$/', $svc);
}
// LOOK version state (written by sbin/look-setup + scripts/enable.sh, as root; the panel only reads it): is the site on the newest
// LOOK, or did it stay on the release it runs because its code fails the new release's `lk --check`. A site that stays
// receives no fixes (security included) — the panel shows this and the lines to fix.
function release_status(string $svc): ?array {
    if (!valid_svc($svc)) return null;
    $f = LOOK_CONF . '/status/' . $svc . '.json';
    if (!is_file($f)) return null;
    $j = json_decode((string)file_get_contents($f), true);
    return is_array($j) ? $j : null;
}
// `lk --check` output: "CHECK <line> <column> <message>" lines; other lines are taken as the message as they are.
function check_lines(string $out): array {
    $rows = [];
    foreach (preg_split('/\R/', $out) as $l) {
        $l = trim($l);
        if ($l === '' || $l === 'OK') continue;
        if (preg_match('/^CHECK (\d+) (\d+) (.*)$/', $l, $m)) $rows[] = ['line' => (int)$m[1], 'col' => (int)$m[2], 'msg' => $m[3]];
        else $rows[] = ['line' => 0, 'col' => 0, 'msg' => $l];
    }
    return array_slice($rows, 0, 50);
}
function release_map(array $domains): array {
    $m = [];
    foreach ($domains as $d) { if (!empty($d['svc']) && ($st = release_status((string)$d['svc']))) $m[$d['svc']] = $st; }
    return $m;
}
function next_free_port(array $domains): int {
    $used = array_column($domains, 'port');
    $sys  = array_map(fn($l) => (int)(explode(':', trim($l))[1] ?? 0),
        array_filter(explode("\n", shell_exec("ss -tlnp | awk 'NR>1{print \$4}' 2>/dev/null") ?? '')));
    $all  = array_unique(array_merge($used, $sys));
    $p    = BASE_PORT;
    while (in_array($p, $all)) $p++;
    return $p;
}
function get_plesk_domains(): array {
    $out   = shell_exec("plesk bin site --list 2>/dev/null") ?? '';
    $lines = array_filter(array_map('trim', explode("\n", $out)));
    return array_values($lines) ?: ['example.com'];
}
function look_version(): string {
    // "LOOK 1.0.0 (linux/amd64)" -> "1.0.0" (phtml already adds the "LOOK " prefix)
    $raw = trim(shell_exec(LOOK_OPT . '/lk --version 2>/dev/null | head -1') ?? '');
    if (preg_match('/([0-9]+\.[0-9]+\.[0-9]+)/', $raw, $m)) return $m[1];
    return $raw ?: '—';
}

// System statistics — top bar (CPU/RAM/disk/load/uptime)
function system_stats(): array {
    $s = ['cpu'=>0,'ram_used'=>0,'ram_total'=>0,'ram_pct'=>0,'disk_pct'=>0,
          'load'=>'—','uptime'=>'—','os'=>PHP_OS];
    // RAM
    if (is_readable('/proc/meminfo')) {
        $mi = file_get_contents('/proc/meminfo');
        preg_match('/MemTotal:\s+(\d+)/', $mi, $t); preg_match('/MemAvailable:\s+(\d+)/', $mi, $a);
        if ($t && $a) { $tot=$t[1]/1024; $av=$a[1]/1024; $s['ram_total']=round($tot); $s['ram_used']=round($tot-$av);
            $s['ram_pct']=$tot? round(($tot-$av)/$tot*100):0; }
    }
    // CPU (approximated with loadavg/core instead of short sampling)
    $ncpu = (int)trim(shell_exec('nproc 2>/dev/null') ?? '1') ?: 1;
    if (is_readable('/proc/loadavg')) {
        $la = explode(' ', trim(file_get_contents('/proc/loadavg')));
        $s['load'] = ($la[0]??'—').' '.($la[1]??'').' '.($la[2]??'');
        $s['cpu']  = min(100, round(((float)($la[0]??0))/$ncpu*100));
    }
    // Disk (/)
    $df = @disk_free_space('/'); $dt = @disk_total_space('/');
    if ($df && $dt) $s['disk_pct'] = round(($dt-$df)/$dt*100);
    // Uptime
    if (is_readable('/proc/uptime')) {
        $u = (int)floatval(file_get_contents('/proc/uptime'));
        $s['uptime'] = intdiv($u,86400).'d '.intdiv($u%86400,3600).'h '.intdiv($u%3600,60).'m';
    }
    $s['cores'] = $ncpu;
    return $s;
}

// ─── AJAX handler ─────────────────────────────────────────────────────────────
$action = $_POST['action'] ?? $_GET['action'] ?? '';
if ($action !== '') {
    header('Content-Type: application/json; charset=utf-8');

    // SECURITY — CSRF: actions create/delete services, call root scripts, write the site's .lk
    // file (save_script -> enable.sh restart -> the written code runs). Without token
    // protection, it can be triggered cross-site by making a logged-in administrator open a bad page.
    // On top of that $action was also read from GET (<img src=?action=delete>).
    // FAIL-SAFE (allowlist, NOT denylist): apply the guard to EVERY action WITHOUT EXCEPTION. Instead of
    // listing action names (the '$MUTATING' list had missed save_script/delete — an allowlist
    // that fails open) invert it: lkApi() already sends ALL actions as POST+same-origin, so
    // guarding all does NOT break the panel and every action added in the future is protected automatically.
    // It covers read_script too (the site source cannot be read cross-site). (1) POST ONLY;
    // (2) same-origin (Origin/Referer host == Host); if neither is present, fail-closed 403.
    if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {
        http_response_code(405);
        echo json_encode(['ok'=>false,'error'=>'POST required']); exit;
    }
    // HTTP_HOST includes the port (host:8443) but parse_url(...HOST) returns it WITHOUT the port ->
    // reduce both to host-only, otherwise a legitimate panel request (8443) is blocked.
    $host   = preg_replace('/:\d+$/', '', $_SERVER['HTTP_HOST'] ?? '');
    $origin = $_SERVER['HTTP_ORIGIN'] ?? '';
    $ref    = $_SERVER['HTTP_REFERER'] ?? '';
    $src_host = $origin !== '' ? parse_url($origin, PHP_URL_HOST)
              : ($ref !== '' ? parse_url($ref, PHP_URL_HOST) : '');
    if ($host === '' || !$src_host || strcasecmp((string)$src_host, $host) !== 0) {
        http_response_code(403);
        echo json_encode(['ok'=>false,'error'=>'Cross-origin request blocked']); exit;
    }

    $domains = load_domains();

    if ($action === 'status') {
        $svc = trim($_POST['svc'] ?? $_GET['svc'] ?? '');
        if (!valid_svc($svc)) { echo json_encode(['ok'=>false,'error'=>'Invalid service']); exit; }
        echo json_encode(['ok'=>true] + svc_status($svc)); exit;
    }
    if ($action === 'list') {
        echo json_encode(['ok'=>true,'domains'=>$domains,'version'=>look_version(),'release_status'=>release_map($domains)]); exit;
    }
    if ($action === 'next_port') {
        echo json_encode(['port' => next_free_port($domains)]); exit;
    }
    if ($action === 'domains_list') {
        echo json_encode(get_plesk_domains()); exit;
    }
    if ($action === 'add' || $action === 'edit') {
        $domain  = trim($_POST['domain']  ?? '');
        $script  = trim($_POST['script']  ?? '');
        $env     = trim($_POST['env']     ?? '');
        $workers = max(1, (int)($_POST['workers'] ?? 4));
        $mode    = in_array($_POST['mode'] ?? 'fcgi', ['fcgi','http']) ? $_POST['mode'] : 'fcgi';
        $port    = (int)($_POST['port']   ?? next_free_port($domains));
        $memory_in = trim((string)($_POST['memory'] ?? 'auto'));
        $memory  = valid_memory($memory_in);
        if (!$domain) { echo json_encode(['ok'=>false,'error'=>'Domain is required']); exit; }
        if ($memory === 'auto' && $memory_in !== '' && strcasecmp($memory_in, 'auto') !== 0) {
            echo json_encode(['ok'=>false,'error'=>'Memory limit: write megabytes or gigabytes (512M, 2G), auto or off']); exit;
        }
        // SECURITY: domain/script go RAW to enable.sh, and from there into the systemd unit
        // through an unquoted heredoc (root systemctl restart). Without format validation
        // a domain containing an embedded line end writes its own [Service]/ExecStartPre
        // directive into the unit and runs a root command (panel user -> root). Validate strictly:
        if (!preg_match('/^(?=.{1,253}$)[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)+$/i', $domain)) {
            echo json_encode(['ok'=>false,'error'=>'Invalid domain']); exit;
        }
        if ($script !== '' &&
            (!preg_match('#^/var/www/vhosts/[A-Za-z0-9._/-]+\.lk$#', $script) || str_contains($script, '..'))) {
            echo json_encode(['ok'=>false,'error'=>'Invalid script path']); exit;
        }
        // Environment file (optional): enable.sh also checks that it is inside the domain's document root
        // and that the service's user can read it
        if ($env !== '' &&
            (!preg_match('#^/var/www/vhosts/[A-Za-z0-9._/-]+$#', $env) || str_contains($env, '..'))) {
            echo json_encode(['ok'=>false,'error'=>'Invalid env file path']); exit;
        }
        if (!$script) {
            $parent = implode('.', array_slice(explode('.', $domain), 1));
            $script = is_dir("/var/www/vhosts/$parent/$domain")
                ? "/var/www/vhosts/$parent/$domain/index.lk"
                : "/var/www/vhosts/$domain/httpdocs/index.lk";
        }
        $svc    = 'look-' . preg_replace('/[^a-z0-9]/', '-', strtolower($domain));
        $result = ctl(enable_args(compact('domain','script','workers','mode','port','env','memory')));
        if (str_contains($result['output'], 'OK:')) {
            $entry = compact('domain','script','env','workers','mode','port','memory','svc');
            $idx   = array_search($domain, array_column($domains, 'domain'));
            if ($idx !== false) $domains[$idx] = $entry; else $domains[] = $entry;
            if (!save_domains($domains)) {
                echo json_encode(['ok'=>false,'error'=>'Service installed but state could not be saved: '.DOMAINS_JSON.' (permissions?)']);
                exit;
            }
            echo json_encode(['ok'=>true,'svc'=>$svc,'port'=>$port,'out'=>$result['output']]);
        } else {
            echo json_encode(['ok'=>false,'error'=>$result['output']]);
        }
        exit;
    }
    if ($action === 'start') {
        $svc = trim($_POST['svc'] ?? '');
        if (!valid_svc($svc)) { echo json_encode(['ok'=>false,'error'=>'Invalid service']); exit; }
        ctl(['start', $svc]);
        $st = svc_status($svc);
        echo json_encode(['ok' => $st['state']==='active', 'state'=>$st['state']]); exit;
    }
    if ($action === 'stop') {
        $svc    = trim($_POST['svc']    ?? '');
        $domain = trim($_POST['domain'] ?? '');
        if (!valid_svc($svc)) { echo json_encode(['ok'=>false,'error'=>'Invalid service']); exit; }
        $result = ctl(['disable', $svc, $domain]);
        $st     = svc_status($svc);
        echo json_encode(['ok'=>true,'state'=>$st['state'],'out'=>$result['output']]); exit;
    }
    if ($action === 'restart') {
        $domain = trim($_POST['domain'] ?? '');
        $idx    = array_search($domain, array_column($domains, 'domain'));
        if ($idx === false) { echo json_encode(['ok'=>false,'error'=>'Domain not found']); exit; }
        $d      = $domains[$idx];
        $result = ctl(enable_args($d));
        $st = svc_status($d['svc']);
        echo json_encode(['ok' => str_contains($result['output'],'OK:'), 'state'=>$st['state'], 'out'=>$result['output']]); exit;
    }
    if ($action === 'delete') {
        $domain = trim($_POST['domain'] ?? '');
        $svc    = trim($_POST['svc']    ?? '');
        if (!valid_svc($svc)) { echo json_encode(['ok'=>false,'error'=>'Invalid service']); exit; }
        // 'remove': the unit file and .service.d are deleted too (they stay on Stop)
        ctl(['disable', $svc, $domain, 'remove']);
        $domains = array_values(array_filter($domains, fn($d) => $d['domain'] !== $domain));
        save_domains($domains);
        echo json_encode(['ok'=>true]); exit;
    }
    // Live system metrics (fetched periodically for the dashboard sparklines)
    if ($action === 'sysstats') {
        echo json_encode(system_stats()); exit;
    }
    // Read the contents of index.lk (only the safe path of a registered domain)
    if ($action === 'read_script') {
        $domain = trim($_POST['domain'] ?? $_GET['domain'] ?? '');
        $path   = safe_script_path(find_domain($domains, $domain));
        if (!$path) { echo json_encode(['ok'=>false,'error'=>'Invalid domain']); exit; }
        if (!file_exists($path)) { echo json_encode(['ok'=>true,'content'=>'','path'=>$path,'exists'=>false]); exit; }
        echo json_encode(['ok'=>true,'content'=>file_get_contents($path),'path'=>$path,'exists'=>true]); exit;
    }
    // Save index.lk + restart the service (code editor)
    if ($action === 'save_script') {
        $domain  = trim($_POST['domain'] ?? '');
        $content = (string)($_POST['content'] ?? '');
        $d       = find_domain($domains, $domain);
        $path    = safe_script_path($d);
        if (!$path) { echo json_encode(['ok'=>false,'error'=>'Invalid domain']); exit; }
        if (strlen($content) > 1048576) { echo json_encode(['ok'=>false,'error'=>'File too large (>1MB)']); exit; }
        if (@file_put_contents($path, $content) === false) {
            echo json_encode(['ok'=>false,'error'=>'Could not write: '.$path.' (permissions?)']); exit;
        }
        // redeploy — enable.sh restart logic (refreshes the bytecode cache)
        $result = ctl(enable_args($d));
        echo json_encode(['ok'=>str_contains($result['output'],'OK:'),'out'=>$result['output']]); exit;
    }
    // Service logs (journalctl — look-ctl logs)
    if ($action === 'logs') {
        $svc = trim($_POST['svc'] ?? $_GET['svc'] ?? '');
        if (!valid_svc($svc)) { echo json_encode(['ok'=>false,'error'=>'Invalid service']); exit; }
        $n   = min(1000, max(20, (int)($_POST['n'] ?? 200)));
        $out = ctl(['logs', $svc, (string)$n])['output'];
        echo json_encode(['ok'=>true,'log'=>trim($out ?? '')]); exit;
    }
    // Live service monitor (status, PID, uptime, RSS, CPU%, restart, port, connections)
    if ($action === 'monitor') {
        $svc = trim($_POST['svc'] ?? $_GET['svc'] ?? '');
        if (!valid_svc($svc)) { echo json_encode(['ok'=>false,'error'=>'Invalid service']); exit; }
        $raw = ctl(['monitor', $svc])['output'];
        $m = [];
        foreach (explode("\n", trim($raw ?? '')) as $ln) {
            $p = strpos($ln, '=');
            if ($p !== false) $m[substr($ln, 0, $p)] = substr($ln, $p + 1);
        }
        echo json_encode(['ok'=>true,'m'=>$m]); exit;
    }
    echo json_encode(['ok'=>false,'error'=>'Unknown action']); exit;
}

// ─── Page render — self-contained (phtml directly; not the pm_Application MVC) ──
// Note: the Extensions panel dispatches this page through plib/controllers/IndexController.php;
// that in turn includes this file. AJAX is handled above and exits,
// so only a page load reaches here.
$domains       = load_domains();
$plesk_domains = get_plesk_domains();
$look_ver      = look_version();
$bin_ok        = file_exists(LOOK_BIN) || is_link(LOOK_BIN);
$stats         = system_stats();
$releases      = release_map($domains);
include __DIR__ . '/phtml/index.phtml';
