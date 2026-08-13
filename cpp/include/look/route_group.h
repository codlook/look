// route_group.h — route::group() setup-zamanı context'i (Go/chi tarzı prefix + middleware kalıtımı).
// route::group("/admin", [$auth], fn) → fn() içindeki route() çağrıları prefix'i ve middleware'leri
// MİRAS alır. SADECE kayıt-zamanı dönüşümü: dispatch'e dokunmaz (prefix normal eşleşir, mw'ler zaten
// çalışır). Nesting stack ile doğal (Go/chi). name/namespace/domain YOK — bilinçli minimal.
// Tek tanım (route_group.cpp) → interpreter.cpp ve http_main.cpp paylaşır (drift yok).
#pragma once
#include <string>
#include <vector>
#include "look/interpreter.h"   // look::Value

namespace look {

// Grup çerçevesini yığına it (route::group girişinde). mws: bu grubun middleware'leri.
void route_group_push(const std::string& prefix, std::vector<Value> mws);
// Çerçeveyi çıkar (route::group çıkışında — closure çağrısı bittiğinde).
void route_group_pop();
// Şu an bir grup içinde miyiz?
bool route_group_active();
// Tüm aktif grup prefix'lerinin birleşimi (en dış → en iç), ör. "/admin" + "/users" = "/admin/users".
std::string route_group_prefix();
// Tüm aktif grup middleware'leri, en-dış-önce sırayla düzleştirilmiş.
std::vector<Value> route_group_middlewares();

} // namespace look
