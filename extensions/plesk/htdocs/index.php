<?php
$scripts_dir  = dirname(__FILE__) . '/scripts';
$conf_dir     = '/opt/look/conf';
$lk_binary    = '/opt/look/lk-fcgi';
$domains_file = $conf_dir . '/domains.json';

/* ---------- yardimci ---------- */
function run_bg($sd, $sc, $args) {
    $a = array();
    foreach ($args as $v) $a[] = escapeshellarg($v);
    exec('sudo ' . escapeshellarg($sd.'/'.$sc) . ' ' . implode(' ',$a) . ' >/tmp/look_last.log 2>&1 &');
}
function load_domains($f) {
    if (!file_exists($f)) return array();
    $d = json_decode(file_get_contents($f), true);
    return is_array($d) ? $d : array();
}
function save_domains($cd, $f, $list) {
    if (!is_dir($cd)) mkdir($cd, 0755, true);
    file_put_contents($f, json_encode(array_values($list), JSON_PRETTY_PRINT));
}
function find_idx($list, $dom) {
    foreach ($list as $i => $d) if (isset($d['domain']) && $d['domain']===$dom) return $i;
    return -1;
}
function svc_name_auto($dom) {
    return 'look-'.preg_replace('/[^a-z0-9-]/','–',strtolower($dom));
}
function svc_name_clean($dom) {
    return 'look-'.preg_replace('/[^a-z0-9-]/','–',strtolower(str_replace('.','-',$dom)));
}
function enable_svc_name($dom) {
    // enable.sh ile birebir: tr lower + sed s/[^a-z0-9-]/-/g
    return 'look-' . preg_replace('/[^a-z0-9-]/', '-', strtolower($dom));
}
function svc_of($d) {
    if (isset($d['svc']) && $d['svc']) return $d['svc'];
    return enable_svc_name(isset($d['domain']) ? $d['domain'] : '');
}
function svc_state($svc) {
    $o=array(); exec('/bin/systemctl is-active '.escapeshellarg($svc).' 2>/dev/null',$o);
    return isset($o[0]) ? trim($o[0]) : 'unknown';
}
function svc_pid($svc) {
    $o=array(); exec('/bin/systemctl show '.escapeshellarg($svc).' --property=MainPID 2>/dev/null',$o);
    foreach ($o as $l) { if (strpos($l,'MainPID=')===0) return trim(substr($l,8)); }
    return '';
}
function lk_version($bin) {
    if (!file_exists($bin)) return 'binary yok';
    $o=array(); exec(escapeshellarg($bin).' version 2>&1',$o);
    return isset($o[0]) ? $o[0] : '?';
}
function next_free_port($used) {
    $taken=array(); $o=array();
    exec("ss -tlnp 2>/dev/null | awk '{print \$4}' | grep -oP ':\K[0-9]+'",$o);
    foreach ($o as $p) $taken[]=(int)$p;
    for ($p=9000;$p<9200;$p++) if(!in_array($p,$used)&&!in_array($p,$taken)) return $p;
    return 9100;
}
function plesk_domains() {
    $o=array(); exec('plesk bin site --list 2>/dev/null',$o);
    return array_values(array_filter(array_map('trim',$o)));
}
function vhost_root($dom) {
    $d='/var/www/vhosts/'.$dom.'/httpdocs';
    if (is_dir($d)) return $d;
    $parts=explode('.',$dom);
    for ($i=1;$i<count($parts)-1;$i++) {
        $p='/var/www/vhosts/'.implode('.',array_slice($parts,$i)).'/'.$dom.'/httpdocs';
        if (is_dir($p)) return $p;
    }
    return $d;
}
function sys_info() {
    $i=array();
    $o=array(); exec("top -bn1 2>/dev/null | grep 'Cpu(s)' | awk '{print \$2+\$4}'",$o);
    $i['cpu']=isset($o[0])?round((float)$o[0],1).'%':'?';
    $o=array(); exec("free -m 2>/dev/null | awk '/^Mem/{printf \"%d / %d MB\",\$3,\$2}'",$o);
    $i['ram']=isset($o[0])?$o[0]:'?';
    $o=array(); exec("df -h / 2>/dev/null | awk 'NR==2{print \$3\" / \"\$2\" (\"\$5\")\"}'",$o);
    $i['disk']=isset($o[0])?$o[0]:'?';
    $o=array(); exec("uptime -p 2>/dev/null",$o);
    $i['uptime']=isset($o[0])?$o[0]:'?';
    $o=array(); exec("cat /proc/loadavg 2>/dev/null",$o);
    $parts=isset($o[0])?explode(' ',$o[0]):array();
    $i['load']=count($parts)>=3?$parts[0].' '.$parts[1].' '.$parts[2]:'?';
    $o=array(); exec("grep 'PRETTY_NAME' /etc/os-release 2>/dev/null | cut -d= -f2 | tr -d '\"'",$o);
    $i['os']=isset($o[0])?$o[0]:'?';
    $o=array(); exec("nproc 2>/dev/null",$o);
    $i['cpu_count']=isset($o[0])?trim($o[0]):'?';
    return $i;
}

/* ---------- POST / PRG ---------- */
if ($_SERVER['REQUEST_METHOD']==='POST') {
    $action  = isset($_POST['action']) ? $_POST['action'] : '';
    $domains = load_domains($domains_file);

    if ($action==='enable' || $action==='update') {
        $dom     = trim(isset($_POST['domain'])  ? $_POST['domain']  : '');
        $script  = trim(isset($_POST['script'])  ? $_POST['script']  : '');
        $workers = max(1,(int)(isset($_POST['workers'])?$_POST['workers']:4));
        $mode    = (isset($_POST['mode'])&&$_POST['mode']==='http') ? 'http' : 'fcgi';
        $port    = (int)(isset($_POST['port'])?$_POST['port']:9000);
        if ($script==='') $script=vhost_root($dom).'/index.lk';
        if (preg_match('/^[a-zA-Z0-9._-]+$/',$dom)) {
            $idx = find_idx($domains,$dom);
            // enable.sh her zaman domain-tabanli isim olusturur; svc'yi buna guncelle
            $svc = enable_svc_name($dom);
            run_bg($scripts_dir,'enable.sh',array($dom,$script,(string)$workers,$mode,(string)$port));
            sleep(2);
            $entry=array('domain'=>$dom,'script'=>$script,'workers'=>$workers,'mode'=>$mode,'port'=>$port,'svc'=>$svc);
            if ($idx>=0) $domains[$idx]=$entry; else $domains[]=$entry;
            save_domains($conf_dir,$domains_file,$domains);
            header('Location: ?ok='.urlencode($dom.' baslatildi')); exit;
        }
    } elseif ($action==='disable') {
        $dom = trim(isset($_POST['domain'])?$_POST['domain']:'');
        $idx = find_idx($domains,$dom);
        $svc = ($idx>=0&&isset($domains[$idx]['svc'])&&$domains[$idx]['svc']) ? $domains[$idx]['svc']
             : enable_svc_name($dom);
        // Sadece servisi durdur — domain listeden silinmez, tekrar baslatilabilir
        exec('sudo /bin/systemctl stop '.escapeshellarg($svc).' 2>/dev/null');
        header('Location: ?ok='.urlencode($dom.' durduruldu')); exit;
    } elseif ($action==='remove') {
        $dom = trim(isset($_POST['domain'])?$_POST['domain']:'');
        $idx = find_idx($domains,$dom);
        $svc = ($idx>=0&&isset($domains[$idx]['svc'])&&$domains[$idx]['svc']) ? $domains[$idx]['svc']
             : enable_svc_name($dom);
        run_bg($scripts_dir,'disable.sh',array($dom,$svc));
        if ($idx>=0) { array_splice($domains,$idx,1); save_domains($conf_dir,$domains_file,$domains); }
        header('Location: ?ok='.urlencode($dom.' kaldirildi')); exit;
    } elseif ($action==='restart') {
        $dom = trim(isset($_POST['domain'])?$_POST['domain']:'');
        $idx = find_idx($domains,$dom);
        if ($idx>=0) {
            $d=$domains[$idx];
            // enable.sh ile yeniden baslat (svc dosyasi silinmisse de calisir)
            run_bg($scripts_dir,'enable.sh',array(
                $d['domain'],
                isset($d['script'])?$d['script']:vhost_root($dom).'/index.lk',
                (string)(isset($d['workers'])?$d['workers']:4),
                isset($d['mode'])?$d['mode']:'fcgi',
                (string)(isset($d['port'])?$d['port']:9000)
            ));
            sleep(3);
        }
        header('Location: ?ok='.urlencode($dom.' baslatildi')); exit;
    }
    header('Location: ?'); exit;
}

/* ---------- Sayfa verisi ---------- */
$active      = load_domains($domains_file);
$all_domains = plesk_domains();
$version     = lk_version($lk_binary);
$sys         = sys_info();

$used_ports=array();
foreach ($active as $d) if (isset($d['port'])) $used_ports[]=(int)$d['port'];
$np=next_free_port($used_ports);

$active_names=array();
foreach ($active as $d) $active_names[]=isset($d['domain'])?$d['domain']:'';

$vhost_map=array();
foreach ($all_domains as $pd) $vhost_map[$pd]=vhost_root($pd).'/index.lk';
$port_map=array();
foreach ($active as $d) if (isset($d['domain'],$d['port'])) $port_map[$d['domain']]=(int)$d['port'];

$flash    = isset($_GET['ok'])   ? htmlspecialchars($_GET['ok'])   : '';
$edit_dom = isset($_GET['edit']) ? $_GET['edit'] : '';
$last_log = file_exists('/tmp/look_last.log') ? trim(file_get_contents('/tmp/look_last.log')) : '';
?>
<!DOCTYPE html>
<html lang="tr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LOOK Language</title>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css">
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/bootstrap-icons@1.11.3/font/bootstrap-icons.min.css">
<style>
:root{--brand:#1e3a5f}
body{background:#eef1f5;font-size:14px}
.topbar{background:var(--brand);color:#fff;padding:10px 20px;display:flex;align-items:center;gap:10px}
.topbar h1{font-size:15px;font-weight:700;margin:0}
.topbar .ver{font-size:11px;opacity:.45;margin-left:auto}
.sysbar{background:#25415f;color:#c8ddf0;padding:6px 20px;display:flex;flex-wrap:wrap;gap:0}
.sysbar .si{display:flex;align-items:center;gap:5px;padding:0 16px 0 0;border-right:1px solid rgba(255,255,255,.1);margin-right:16px}
.sysbar .si:last-child{border-right:none;margin-left:auto}
.sysbar .lbl{font-size:10px;opacity:.55;text-transform:uppercase;letter-spacing:.5px}
.sysbar .val{font-size:12px;font-weight:600}
.page{max-width:1080px;margin:20px auto;padding:0 14px}
.domain-card{border:1px solid #d5dce6;border-radius:8px;background:#fff;margin-bottom:12px}
.domain-card .head{padding:12px 16px;display:flex;align-items:center;gap:10px;border-bottom:1px solid #edf0f4}
.domain-card .body{padding:14px 16px}
.domain-card .field{display:flex;flex-direction:column;gap:2px}
.domain-card .field .lbl{font-size:10px;text-transform:uppercase;letter-spacing:.5px;color:#8a95a3}
.domain-card .field .val{font-size:13px;font-weight:500;color:#1e2d3d}
.pill-active  {background:#d1f5dc;color:#145a2e;font-size:11px;padding:2px 10px;border-radius:20px;font-weight:600}
.pill-inactive{background:#fff3cd;color:#7a5800;font-size:11px;padding:2px 10px;border-radius:20px;font-weight:600}
.pill-unknown {background:#e4e6ea;color:#4a5568;font-size:11px;padding:2px 10px;border-radius:20px;font-weight:600}
.edit-block{background:#f7f9fc;border-top:1px solid #e3e8ee;padding:16px;border-radius:0 0 8px 8px}
.log-pre{background:#111c2b;color:#8fb8d8;font-size:11px;padding:12px;border-radius:6px;max-height:140px;overflow:auto;margin:0;white-space:pre-wrap;word-break:break-all}
.btn-icon{padding:4px 10px;font-size:12px}
</style>
</head>
<body>

<div class="topbar">
  <svg width="24" height="24" viewBox="0 0 32 32"><circle cx="16" cy="16" r="16" fill="#3d8ef8"/><text x="8" y="21" font-size="14" fill="#fff" font-weight="bold">L</text></svg>
  <h1>LOOK Language</h1>
  <span class="ver"><?php echo htmlspecialchars($version); ?></span>
</div>

<div class="sysbar">
  <div class="si"><span class="lbl">CPU</span><span class="val"><?php echo htmlspecialchars($sys['cpu']); ?> <small style="opacity:.6">(<?php echo $sys['cpu_count']; ?> core)</small></span></div>
  <div class="si"><span class="lbl">RAM</span><span class="val"><?php echo htmlspecialchars($sys['ram']); ?></span></div>
  <div class="si"><span class="lbl">Disk</span><span class="val"><?php echo htmlspecialchars($sys['disk']); ?></span></div>
  <div class="si"><span class="lbl">Load</span><span class="val"><?php echo htmlspecialchars($sys['load']); ?></span></div>
  <div class="si"><span class="lbl">Uptime</span><span class="val"><?php echo htmlspecialchars($sys['uptime']); ?></span></div>
  <div class="si"><span class="lbl">OS</span><span class="val"><?php echo htmlspecialchars($sys['os']); ?></span></div>
</div>

<div class="page">

<?php if ($flash): ?>
<div class="alert alert-success alert-dismissible fade show mt-3 py-2" role="alert">
  <i class="bi bi-check-circle me-1"></i><?php echo $flash; ?>
  <button type="button" class="btn-close" data-bs-dismiss="alert"></button>
</div>
<?php endif; ?>

<!-- Aktif Domainler -->
<div class="d-flex align-items-center justify-content-between mt-3 mb-2">
  <h6 class="fw-bold mb-0 text-dark">Aktif LOOK Domainleri <span class="badge bg-secondary"><?php echo count($active); ?></span></h6>
  <a href="?" class="btn btn-sm btn-outline-secondary btn-icon"><i class="bi bi-arrow-clockwise"></i> Yenile</a>
</div>

<?php if (empty($active)): ?>
<div class="domain-card p-3 text-muted"><i class="bi bi-info-circle me-1"></i>Henuz aktif domain yok. Asagidan ekle.</div>
<?php endif; ?>

<?php foreach ($active as $d):
  $dom   = isset($d['domain'])  ? $d['domain']  : '';
  $svc   = svc_of($d);
  $state = svc_state($svc);
  $pill  = ($state==='active') ? 'pill-active' : (($state==='inactive') ? 'pill-inactive' : 'pill-unknown');
  $pid   = ($state==='active') ? svc_pid($svc) : '';
  $is_edit = ($edit_dom===$dom);
?>
<div class="domain-card">
  <div class="head">
    <i class="bi bi-globe text-primary"></i>
    <strong><?php echo htmlspecialchars($dom); ?></strong>
    <span class="<?php echo $pill; ?>"><?php echo $state; ?><?php if($pid) echo ' · PID '.$pid; ?></span>
    <div class="ms-auto d-flex gap-2">
      <a href="?edit=<?php echo $is_edit?'':urlencode($dom); ?>" class="btn btn-sm btn-outline-primary btn-icon">
        <i class="bi bi-pencil"></i> <?php echo $is_edit?'Kapat':'Duzenle'; ?>
      </a>
      <form class="d-inline" method="post">
        <input type="hidden" name="action" value="restart">
        <input type="hidden" name="domain" value="<?php echo htmlspecialchars($dom); ?>">
        <?php if ($state==='active'): ?>
        <button class="btn btn-sm btn-warning btn-icon" type="submit"><i class="bi bi-arrow-repeat"></i> Yeniden Baslat</button>
        <?php else: ?>
        <button class="btn btn-sm btn-success btn-icon" type="submit"><i class="bi bi-play-fill"></i> Baslat</button>
        <?php endif; ?>
      </form>
      <form class="d-inline" method="post">
        <input type="hidden" name="action" value="disable">
        <input type="hidden" name="domain" value="<?php echo htmlspecialchars($dom); ?>">
        <button class="btn btn-sm btn-danger btn-icon" type="submit"
          <?php if($state!=='active') echo 'disabled'; ?>><i class="bi bi-stop-circle"></i> Durdur</button>
      </form>
      <form class="d-inline" method="post" onsubmit="return confirm('<?php echo addslashes($dom); ?> listeden silinsin mi?')">
        <input type="hidden" name="action" value="remove">
        <input type="hidden" name="domain" value="<?php echo htmlspecialchars($dom); ?>">
        <button class="btn btn-sm btn-outline-danger btn-icon" type="submit"><i class="bi bi-trash"></i> Sil</button>
      </form>
    </div>
  </div>
  <div class="body">
    <div class="row g-3">
      <div class="col-md-5">
        <div class="field">
          <span class="lbl">Script Dosyasi</span>
          <span class="val" title="<?php echo htmlspecialchars(isset($d['script'])?$d['script']:''); ?>">
            <?php $sc=isset($d['script'])?$d['script']:''; echo htmlspecialchars(strlen($sc)>60?'…'.substr($sc,-57):$sc); ?>
          </span>
        </div>
      </div>
      <div class="col-md-2">
        <div class="field"><span class="lbl">Port</span><span class="val"><?php echo isset($d['port'])?(int)$d['port']:'-'; ?></span></div>
      </div>
      <div class="col-md-2">
        <div class="field"><span class="lbl">Workers</span><span class="val"><?php echo isset($d['workers'])?(int)$d['workers']:'-'; ?></span></div>
      </div>
      <div class="col-md-2">
        <div class="field"><span class="lbl">Mod</span><span class="val"><?php echo htmlspecialchars(isset($d['mode'])?$d['mode']:'fcgi'); ?></span></div>
      </div>
      <div class="col-md-1">
        <div class="field"><span class="lbl">Servis</span><span class="val" style="font-size:11px"><?php echo htmlspecialchars($svc); ?></span></div>
      </div>
    </div>
  </div>
  <?php if ($is_edit): ?>
  <div class="edit-block">
    <div class="fw-semibold mb-3"><i class="bi bi-pencil-square me-1"></i>Ayarlar — <?php echo htmlspecialchars($dom); ?></div>
    <form method="post">
      <input type="hidden" name="action" value="update">
      <input type="hidden" name="domain" value="<?php echo htmlspecialchars($dom); ?>">
      <div class="row g-3">
        <div class="col-md-6">
          <label class="form-label">Script Yolu (.lk dosyasi)</label>
          <input type="text" class="form-control form-control-sm" name="script"
            value="<?php echo htmlspecialchars(isset($d['script'])?$d['script']:''); ?>">
        </div>
        <div class="col-md-2">
          <label class="form-label">Port</label>
          <input type="number" class="form-control form-control-sm" name="port"
            value="<?php echo isset($d['port'])?(int)$d['port']:9000; ?>">
        </div>
        <div class="col-md-2">
          <label class="form-label">Workers</label>
          <input type="number" class="form-control form-control-sm" name="workers"
            value="<?php echo isset($d['workers'])?(int)$d['workers']:4; ?>" min="1" max="128">
        </div>
        <div class="col-md-2">
          <label class="form-label">Mod</label>
          <select class="form-select form-select-sm" name="mode">
            <option value="fcgi" <?php echo (!isset($d['mode'])||$d['mode']==='fcgi')?'selected':''; ?>>FastCGI</option>
            <option value="http" <?php echo (isset($d['mode'])&&$d['mode']==='http')?'selected':''; ?>>HTTP Proxy</option>
          </select>
        </div>
        <div class="col-12 d-flex gap-2 pt-1 align-items-center">
          <button class="btn btn-primary btn-sm" type="submit"><i class="bi bi-save me-1"></i>Kaydet ve Yeniden Baslat</button>
          <a href="?" class="btn btn-outline-secondary btn-sm">Iptal</a>
        </div>
      </div>
    </form>
  </div>
  <?php endif; ?>
</div>
<?php endforeach; ?>

<!-- Domain Ekle -->
<div class="card mt-2 mb-3">
  <div class="card-header fw-semibold"><i class="bi bi-plus-circle me-1"></i>Domain Ekle</div>
  <div class="card-body">
    <form method="post">
      <input type="hidden" name="action" value="enable">
      <div class="row g-3">
        <div class="col-md-5">
          <label class="form-label fw-semibold">Domain</label>
          <select class="form-select" name="domain" id="domSel" onchange="fillForm(this.value)" required>
            <option value="">-- Domain sec --</option>
            <?php foreach ($all_domains as $pd): ?>
            <option value="<?php echo htmlspecialchars($pd); ?>"
              <?php if(in_array($pd,$active_names)) echo 'style="color:#0d6efd;font-weight:600"'; ?>>
              <?php echo htmlspecialchars($pd); echo in_array($pd,$active_names)?' ✓':''; ?>
            </option>
            <?php endforeach; ?>
          </select>
          <div class="form-text">✓ isaretliler zaten aktif</div>
        </div>
        <div class="col-md-7">
          <label class="form-label fw-semibold">Script Yolu (.lk dosyasi)</label>
          <input type="text" class="form-control" name="script" id="scriptPath"
            placeholder="Bos birakila bilir — domain secince otomatik dolar">
        </div>
        <div class="col-md-3">
          <label class="form-label fw-semibold">Port</label>
          <input type="number" class="form-control" name="port" id="portField" value="<?php echo $np; ?>">
        </div>
        <div class="col-md-3">
          <label class="form-label fw-semibold">Workers</label>
          <input type="number" class="form-control" name="workers" value="4" min="1" max="128">
        </div>
        <div class="col-md-3">
          <label class="form-label fw-semibold">Mod</label>
          <select class="form-select" name="mode">
            <option value="fcgi">FastCGI</option>
            <option value="http">HTTP Proxy</option>
          </select>
        </div>
        <div class="col-md-3 d-flex align-items-end">
          <button class="btn btn-primary w-100" type="submit"><i class="bi bi-play-fill me-1"></i>Baslat</button>
        </div>
      </div>
    </form>
  </div>
</div>

<!-- Log -->
<?php if ($last_log): ?>
<div class="card mb-4">
  <div class="card-header fw-semibold py-2"><i class="bi bi-terminal me-1"></i>Son Islem Logu</div>
  <div class="card-body p-2">
    <pre class="log-pre"><?php echo htmlspecialchars($last_log); ?></pre>
  </div>
</div>
<?php endif; ?>

</div>

<script src="https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.min.js"></script>
<script>
var vmap=<?php echo json_encode($vhost_map); ?>;
var pmap=<?php echo json_encode($port_map); ?>;
var np=<?php echo (int)$np; ?>;
function fillForm(d){
  document.getElementById('scriptPath').value = d?(vmap[d]||''):'';
  document.getElementById('portField').value  = d&&pmap[d]?pmap[d]:np;
}
</script>
</body>
</html>
