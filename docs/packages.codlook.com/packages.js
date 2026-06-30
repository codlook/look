// packages.codlook.com — paket ve modül verisi
// TODO: gerçek API'ye geçince bu dosyayı kaldır,
//       loadData() içindeki fetch('/api/packages') satırını aç.

const PACKAGES_DATA = {
  "modules": [
    {
      "name": "jwt",
      "description": "JSON Web Token oluştur, doğrula ve çöz. jwt_sign / jwt_verify / jwt_decode. HS256 ve HS512 desteği, middleware pattern ile kullanım.",
      "author": "codlook",
      "github": "https://github.com/codlook/look-modules/tree/main/jwt",
      "install": "lk module install github.com/codlook/look-modules/jwt",
      "use": "use jwt",
      "tags": ["auth", "token", "security"],
      "version": "1.0.0",
      "approved": true,
      "icon": "🔐",
      "color": "purple"
    },
    {
      "name": "http",
      "description": "Dışa HTTP istemcisi. GET, POST, PUT, DELETE, JSON body, form-data, özel header, timeout ve redirect desteği.",
      "author": "codlook",
      "github": "https://github.com/codlook/look-modules/tree/main/http",
      "install": "lk module install github.com/codlook/look-modules/http",
      "use": "use http",
      "tags": ["http", "api", "client"],
      "version": "1.0.0",
      "approved": true,
      "icon": "🌐",
      "color": "blue"
    },
    {
      "name": "crypto",
      "description": "SHA256, SHA512, HMAC-SHA256, PBKDF2, AES-256-GCM, UUID v4. Şifreleme, hash ve güvenli rastgele veri üretimi.",
      "author": "codlook",
      "github": "https://github.com/codlook/look-modules/tree/main/crypto",
      "install": "lk module install github.com/codlook/look-modules/crypto",
      "use": "use crypto",
      "tags": ["crypto", "hash", "security", "uuid"],
      "version": "1.0.0",
      "approved": true,
      "icon": "🔑",
      "color": "orange"
    },
    {
      "name": "mail",
      "description": "SMTP üzerinden e-posta gönder. HTML ve düz metin, ekler, CC/BCC, kimlik doğrulama desteği.",
      "author": "codlook",
      "github": "https://github.com/codlook/look-modules/tree/main/mail",
      "install": "lk module install github.com/codlook/look-modules/mail",
      "use": "use mail",
      "tags": ["email", "smtp", "notification"],
      "version": "1.0.0",
      "approved": true,
      "icon": "📬",
      "color": "teal"
    },
    {
      "name": "cache",
      "description": "Bellek içi önbellek. TTL desteği, get / set / delete / flush / exists. Aynı süreçteki tüm istekler arasında paylaşılır.",
      "author": "codlook",
      "github": "https://github.com/codlook/look-modules/tree/main/cache",
      "install": "lk module install github.com/codlook/look-modules/cache",
      "use": "use cache",
      "tags": ["cache", "performance", "ttl"],
      "version": "1.0.0",
      "approved": true,
      "icon": "⚡",
      "color": "green"
    },
    {
      "name": "queue",
      "description": "Bellek içi iş kuyruğu. push / pop / size / flush. jobs:: ile birlikte arka plan görevleri için idealdir.",
      "author": "codlook",
      "github": "https://github.com/codlook/look-modules/tree/main/queue",
      "install": "lk module install github.com/codlook/look-modules/queue",
      "use": "use queue",
      "tags": ["queue", "async", "background"],
      "version": "1.0.0",
      "approved": true,
      "icon": "🔄",
      "color": "purple"
    }
  ],
  "packages": [
    {
      "name": "firebase",
      "description": "Firebase entegrasyonu — Firestore CRUD, Authentication (email/password, token doğrulama), Realtime Database. Google Firebase servislerine LOOK'tan doğrudan eriş.",
      "author": "codlook",
      "github": "https://github.com/codlook/look-packages/tree/main/firebase",
      "install": "lk install github.com/codlook/look-packages/firebase",
      "use": "use \"pkg/firebase\"",
      "tags": ["firebase", "database", "auth", "google"],
      "version": "1.0.0",
      "approved": true,
      "icon": "🔥",
      "color": "orange"
    }
  ]
};
