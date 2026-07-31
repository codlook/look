<?php
/**
 * LOOK Language — Plesk Extension
 * AJAX: action parametresi ile JSON doner
 * Page: pm_Application->run() ile Plesk frame icinde render edilir
 */

define('LOOK_OPT',     '/opt/look');
define('LOOK_CONF',    LOOK_OPT . '/conf');
define('LOOK_BIN',     LOOK_OPT . '/lk-fcgi');
define('SCRIPTS_DIR',  __DIR__ . '/scripts');
define('BASE_PORT',    9100);

// State (domains.json) — Plesk'in panel-yazilabilir modul var dizininde tutulur.
// /opt/look/conf root'a ait (dnf/RPM binary klasoru); panel psaadm olarak calisir
// ve oraya YAZAMAZ → domain eklense de kaydedilmez, listede gorunmezdi. Bu dizin
// psaadm sahipli. CLI/fallback icin /opt/look/conf'a duser.
define('LOOK_VAR', is_dir('/usr/local/psa/var/modules/look-lang')
    ? '/usr/local/psa/var/modules/look-lang'
    : LOOK_CONF);
define('DOMAINS_JSON', LOOK_VAR . '/domains.json');

function auto_setup(): void {
    if (!is_dir(LOOK_OPT))  mkdir(LOOK_OPT,  0755, true);
    if (!is_dir(LOOK_CONF)) mkdir(LOOK_CONF, 0755, true);

    // Binary kurulumu (Plesk kök post-install hook'unu çalıştırmadığı için burada).
    // RHEL/AlmaLinux: RPM (dnf) → 'dnf update look-lang' yolu; aksi halde static binary.
    if (!file_exists(LOOK_BIN) && !is_link(LOOK_BIN)) {
        $rpm = __DIR__ . "/look-lang.rpm";
        $installed = false;
        if (file_exists($rpm) && (trim(@shell_exec('command -v dnf')) || trim(@shell_exec('command -v yum')))) {
            $mgr = trim(@shell_exec('command -v dnf')) ? 'dnf' : 'yum';
            @shell_exec("$mgr install -y " . escapeshellarg($rpm) . " 2>&1 || rpm -Uvh --force " . escapeshellarg($rpm) . " 2>&1");
            // /opt/look yollarını sistem binary'sine symlink (scriptler /opt/look kullanır)
            if (file_exists('/usr/bin/lk-fcgi')) {
                foreach (['lk','lk-fcgi','lk-cgi'] as $b) { @symlink("/usr/bin/$b", LOOK_OPT."/$b"); }
                $installed = true;
            }
        }
        if (!$installed) {  // fallback: bundle'lanan portatif static binary
            foreach (['lk-fcgi','lk','lk-cgi'] as $b) {
                $src = __DIR__ . "/bin/$b";
                if (file_exists($src)) { copy($src, LOOK_OPT."/$b"); chmod(LOOK_OPT."/$b", 0755); }
            }
        }
    }

    // sudoers — panel enable.sh/disable.sh + systemctl'i sudo ile çalıştırabilsin.
    // logs.sh eklendiginden mevcut kurulumlarda da yeniden yazilir (surum farki).
    // Scriptler '/bin/bash <script>' ile calisir — Plesk zip'i PHP ile acip unix
    // exec bitini striplediginden dosyalar 644 kalir; bash interpreter ile cagirinca
    // exec biti gerekmez. Bu yuzden sudoers de bash-formunda.
    $sudo = "psaadm ALL=(root) NOPASSWD: /bin/bash " . SCRIPTS_DIR . "/enable.sh *, /bin/bash " . SCRIPTS_DIR . "/disable.sh *, "
          . "/bin/bash " . SCRIPTS_DIR . "/status.sh *, /bin/bash " . SCRIPTS_DIR . "/logs.sh *, /bin/bash " . SCRIPTS_DIR . "/monitor.sh *, "
          . "/bin/systemctl start look-*, /bin/systemctl stop look-*, "
          . "/bin/systemctl restart look-*, /bin/systemctl enable look-*, /bin/systemctl disable look-*, "
          . "/bin/systemctl daemon-reload, /usr/local/psa/admin/sbin/websrvmng, /usr/local/psa/admin/bin/httpdmng\n";
    $cur = @file_get_contents('/etc/sudoers.d/look-lang');
    if ($cur === false || strpos($cur, '/bin/bash') === false) {
        @mkdir('/etc/sudoers.d', 0755, true);
        @file_put_contents('/etc/sudoers.d/look-lang', $sudo);
        @chmod('/etc/sudoers.d/look-lang', 0440);
    }

    // Scriptler zaten 0755 kurulur; panel psaadm olarak root dosyalarini chmod
    // edemez → warning uretir. Yalnizca calistirilamaz olan varsa dene, sessizce.
    foreach (glob(SCRIPTS_DIR . '/*.sh') ?: [] as $sh) if (!is_executable($sh)) @chmod($sh, 0755);

    // State dizini + eski /opt/look/conf/domains.json'dan tek seferlik tasima
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
// Domain kaydini domains.json'dan bul (guvenli — istemciden ham yol ALINMAZ).
function find_domain(array $domains, string $name): ?array {
    foreach ($domains as $d) if (($d['domain'] ?? '') === $name) return $d;
    return null;
}
// Bir domain'in kayitli script yolunu dogrula: /var/www/vhosts altinda + .lk uzantili.
function safe_script_path(?array $d): ?string {
    if (!$d || empty($d['script'])) return null;
    $p = $d['script'];
    if (substr($p, -3) !== '.lk') return null;
    if (strpos($p, '/var/www/vhosts/') !== 0) return null;
    if (strpos($p, '..') !== false) return null;
    return $p;
}
function run_script(string $script, string $args): array {
    $cmd = 'sudo /bin/bash ' . escapeshellarg(SCRIPTS_DIR . '/' . $script) . ' ' . $args . ' 2>&1';
    return ['output' => trim(shell_exec($cmd) ?? '')];
}
function svc_status(string $svc): array {
    $out = trim(shell_exec('sudo /bin/bash ' . escapeshellarg(SCRIPTS_DIR . '/status.sh') . ' ' . escapeshellarg($svc) . ' 2>&1') ?? '');
    if (str_starts_with($out, 'active:')) return ['state'=>'active','pid'=>substr($out,7)];
    return ['state' => $out ?: 'unknown', 'pid' => ''];
}
// GUVENLIK: svc her zaman 'look-' + alfanumerik/dash (enable.sh SVC_NAME uretimi:
// domain'deki [^a-z0-9] -> '-'). 'start' eylemi svc'yi HAM 'sudo systemctl start'a
// veriyor; sudoers 'systemctl start look-*' wildcard'inda '*' argumanda '/' de eslesir,
// systemctl '/' iceren argi PATH olarak yukler -> 'look-../../tmp/evil.service' saldirgan
// unit'ini ROOT baslatir. Slash/'..' iceren svc'yi reddet (path-injection kapanir).
function valid_svc(string $svc): bool {
    return (bool)preg_match('/^look-[a-z0-9-]+$/', $svc);
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
    // "LOOK 1.0.0 (linux/amd64)" -> "1.0.0" (phtml zaten "LOOK " ön eki koyar)
    $raw = trim(shell_exec(LOOK_OPT . '/lk --version 2>/dev/null | head -1') ?? '');
    if (preg_match('/([0-9]+\.[0-9]+\.[0-9]+)/', $raw, $m)) return $m[1];
    return $raw ?: '—';
}

// Sistem istatistikleri — üst bar (CPU/RAM/disk/load/uptime)
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
    // CPU (kısa örnekleme yerine loadavg/core ile yaklaşık)
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

    // GUVENLIK — CSRF: eylemler servis kurar/siler, root scriptleri cagirir, site .lk
    // dosyasina yazar (save_script -> enable.sh restart -> yazilan kod calisir). Token
    // korumasi olmadan, oturumu acik yoneticiye kotu sayfa actirarak capraz-site tetiklenebilir.
    // Ustelik $action GET'ten de okunuyordu (<img src=?action=delete>).
    // FAIL-SAFE (denylist DEGIL allowlist): guard'i ISTISNASIZ HER eyleme uygula. Eylem
    // adlarini saymak yerine ('$MUTATING' listesi save_script/delete'i kacirmisti — allowlist
    // fail-open) tersine cevir: lkApi() zaten TUM eylemleri POST+same-origin gonderiyor, o
    // yuzden guard-all paneli KIRMAZ ve gelecekte eklenecek her eylem otomatik korunur.
    // read_script'i de kapsar (site kaynagi capraz-site okunamaz). (1) YALNIZ POST;
    // (2) same-origin (Origin/Referer host == Host); ikisi de yoksa fail-closed 403.
    if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') {
        http_response_code(405);
        echo json_encode(['ok'=>false,'error'=>'POST required']); exit;
    }
    // HTTP_HOST porti icerir (host:8443) ama parse_url(...HOST) PORTSUZ doner ->
    // ikisini de host-only'ye indir, yoksa mesru panel istegi (8443) bloke olur.
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
        echo json_encode(['ok'=>true,'domains'=>$domains,'version'=>look_version()]); exit;
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
        $workers = max(1, (int)($_POST['workers'] ?? 4));
        $mode    = in_array($_POST['mode'] ?? 'fcgi', ['fcgi','http']) ? $_POST['mode'] : 'fcgi';
        $port    = (int)($_POST['port']   ?? next_free_port($domains));
        if (!$domain) { echo json_encode(['ok'=>false,'error'=>'Domain is required']); exit; }
        // GUVENLIK: domain/script HAM olarak enable.sh'a, oradan tirnaksiz heredoc ile
        // systemd unit'ine geciyor (sudo systemctl restart -> root). Format dogrulamasi
        // olmadan gomulu satir-sonu iceren bir domain, unit'e kendi [Service]/ExecStartPre
        // direktifini yazip root komut kosturur (panel kullanicisi -> root). Kati dogrula:
        if (!preg_match('/^(?=.{1,253}$)[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)+$/i', $domain)) {
            echo json_encode(['ok'=>false,'error'=>'Invalid domain']); exit;
        }
        if ($script !== '' &&
            (!preg_match('#^/var/www/vhosts/[A-Za-z0-9._/-]+\.lk$#', $script) || str_contains($script, '..'))) {
            echo json_encode(['ok'=>false,'error'=>'Invalid script path']); exit;
        }
        if (!$script) {
            $parent = implode('.', array_slice(explode('.', $domain), 1));
            $script = is_dir("/var/www/vhosts/$parent/$domain")
                ? "/var/www/vhosts/$parent/$domain/index.lk"
                : "/var/www/vhosts/$domain/httpdocs/index.lk";
        }
        $svc    = 'look-' . preg_replace('/[^a-z0-9]/', '-', strtolower($domain));
        $result = run_script('enable.sh',
            escapeshellarg($domain)  . ' ' . escapeshellarg($script)  . ' ' .
            (int)$workers            . ' ' . escapeshellarg($mode)    . ' ' . (int)$port);
        if (str_contains($result['output'], 'OK:')) {
            $entry = compact('domain','script','workers','mode','port','svc');
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
        shell_exec('sudo /bin/systemctl start ' . escapeshellarg($svc) . ' 2>&1');
        $st = svc_status($svc);
        echo json_encode(['ok' => $st['state']==='active', 'state'=>$st['state']]); exit;
    }
    if ($action === 'stop') {
        $svc    = trim($_POST['svc']    ?? '');
        $domain = trim($_POST['domain'] ?? '');
        if (!valid_svc($svc)) { echo json_encode(['ok'=>false,'error'=>'Invalid service']); exit; }
        $result = run_script('disable.sh', escapeshellarg($svc) . ' ' . escapeshellarg($domain));
        $st     = svc_status($svc);
        echo json_encode(['ok'=>true,'state'=>$st['state'],'out'=>$result['output']]); exit;
    }
    if ($action === 'restart') {
        $domain = trim($_POST['domain'] ?? '');
        $idx    = array_search($domain, array_column($domains, 'domain'));
        if ($idx === false) { echo json_encode(['ok'=>false,'error'=>'Domain not found']); exit; }
        $d      = $domains[$idx];
        $result = run_script('enable.sh',
            escapeshellarg($d['domain']) . ' ' . escapeshellarg($d['script']) . ' ' .
            (int)$d['workers']           . ' ' . escapeshellarg($d['mode'])   . ' ' . (int)$d['port']);
        $st = svc_status($d['svc']);
        echo json_encode(['ok' => str_contains($result['output'],'OK:'), 'state'=>$st['state'], 'out'=>$result['output']]); exit;
    }
    if ($action === 'delete') {
        $domain = trim($_POST['domain'] ?? '');
        $svc    = trim($_POST['svc']    ?? '');
        if (!valid_svc($svc)) { echo json_encode(['ok'=>false,'error'=>'Invalid service']); exit; }
        run_script('disable.sh', escapeshellarg($svc) . ' ' . escapeshellarg($domain));
        $domains = array_values(array_filter($domains, fn($d) => $d['domain'] !== $domain));
        save_domains($domains);
        echo json_encode(['ok'=>true]); exit;
    }
    // Canli sistem metrikleri (dashboard sparkline'lari icin periyodik cekilir)
    if ($action === 'sysstats') {
        echo json_encode(system_stats()); exit;
    }
    // index.lk icerigini oku (yalnizca kayitli domain'in guvenli yolu)
    if ($action === 'read_script') {
        $domain = trim($_POST['domain'] ?? $_GET['domain'] ?? '');
        $path   = safe_script_path(find_domain($domains, $domain));
        if (!$path) { echo json_encode(['ok'=>false,'error'=>'Invalid domain']); exit; }
        if (!file_exists($path)) { echo json_encode(['ok'=>true,'content'=>'','path'=>$path,'exists'=>false]); exit; }
        echo json_encode(['ok'=>true,'content'=>file_get_contents($path),'path'=>$path,'exists'=>true]); exit;
    }
    // index.lk kaydet + servisi yeniden baslat (kod editoru)
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
        // yeniden deploy — enable.sh restart mantigi (bytecode cache'i tazeler)
        $result = run_script('enable.sh',
            escapeshellarg($d['domain']) . ' ' . escapeshellarg($d['script']) . ' ' .
            (int)$d['workers'] . ' ' . escapeshellarg($d['mode']) . ' ' . (int)$d['port']);
        echo json_encode(['ok'=>str_contains($result['output'],'OK:'),'out'=>$result['output']]); exit;
    }
    // Servis loglari (journalctl — logs.sh sudo ile)
    if ($action === 'logs') {
        $svc = trim($_POST['svc'] ?? $_GET['svc'] ?? '');
        if (!valid_svc($svc)) { echo json_encode(['ok'=>false,'error'=>'Invalid service']); exit; }
        $n   = min(1000, max(20, (int)($_POST['n'] ?? 200)));
        $out = shell_exec('sudo /bin/bash ' . escapeshellarg(SCRIPTS_DIR . '/logs.sh') . ' ' . escapeshellarg($svc) . ' ' . (int)$n . ' 2>&1');
        echo json_encode(['ok'=>true,'log'=>trim($out ?? '')]); exit;
    }
    // Canli servis monitoru (durum, PID, uptime, RSS, CPU%, restart, port, baglanti)
    if ($action === 'monitor') {
        $svc = trim($_POST['svc'] ?? $_GET['svc'] ?? '');
        if (!valid_svc($svc)) { echo json_encode(['ok'=>false,'error'=>'Invalid service']); exit; }
        $raw = shell_exec('sudo /bin/bash ' . escapeshellarg(SCRIPTS_DIR . '/monitor.sh') . ' ' . escapeshellarg($svc) . ' 2>&1');
        $m = [];
        foreach (explode("\n", trim($raw ?? '')) as $ln) {
            $p = strpos($ln, '=');
            if ($p !== false) $m[substr($ln, 0, $p)] = substr($ln, $p + 1);
        }
        echo json_encode(['ok'=>true,'m'=>$m]); exit;
    }
    echo json_encode(['ok'=>false,'error'=>'Unknown action']); exit;
}

// ─── Page render — self-contained (phtml doğrudan; pm_Application MVC değil) ──
// Not: Extensions paneli bu sayfayı plib/controllers/IndexController.php üzerinden
// dispatch eder; o da bu dosyayı include eder. AJAX yukarıda handle edilip exit
// eder, buraya yalnız sayfa yüklemede gelinir.
$domains       = load_domains();
$plesk_domains = get_plesk_domains();
$look_ver      = look_version();
$bin_ok        = file_exists(LOOK_BIN) || is_link(LOOK_BIN);
$stats         = system_stats();
include __DIR__ . '/phtml/index.phtml';
