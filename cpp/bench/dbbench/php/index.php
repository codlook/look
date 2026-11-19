<?php
// mysqli persistent ("p:") — fpm worker'ları arası bağlantı havuzu (adil, per-request connect değil)
$m = new mysqli("p:".getenv("DB_HOST"), "root", getenv("DB_PASS"), "bench");
$stmt = $m->prepare("SELECT id, name, email FROM users WHERE id = ?");
$id = 42; $stmt->bind_param("i", $id); $stmt->execute();
header('Content-Type: application/json');
echo json_encode($stmt->get_result()->fetch_all(MYSQLI_ASSOC));
