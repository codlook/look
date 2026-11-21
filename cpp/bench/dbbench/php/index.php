<?php
// mysqli persistent ("p:"): connections are kept across requests in each FPM worker (a pool, as in the other apps — not a connect per request)
$m = new mysqli("p:".getenv("DB_HOST"), "root", getenv("DB_PASS"), "bench");
$stmt = $m->prepare("SELECT id, name, email FROM users WHERE id = ?");
$id = 42; $stmt->bind_param("i", $id); $stmt->execute();
header('Content-Type: application/json');
echo json_encode($stmt->get_result()->fetch_all(MYSQLI_ASSOC));
