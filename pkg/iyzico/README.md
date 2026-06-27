# iyzico — LOOK iyzipay paketi

İyzico ödeme sistemi entegrasyonu. IYZWSv2 imzalama, 3DS, iptal/iade, kart saklama.

## Kurulum

```bash
look install github.com/codlook/iyzico
```

## Hızlı Başlangıç

```lk
use "pkg/codlook/iyzico/iyzico.lk";

$iyzico = iyzico_config("api-key", "secret-key", true); // true = sandbox

$result = iyzico_pay($iyzico, [
    "conversationId" => "siparis-123",
    "price"          => "100.0",
    "paidPrice"      => "100.0",
    "paymentCard"    => [
        "cardHolderName" => "Ad Soyad",
        "cardNumber"     => "5528790000000008",
        "expireMonth"    => "12",
        "expireYear"     => "2030",
        "cvc"            => "123",
        "registerCard"   => 0
    ],
    "buyer" => [
        "id"                  => "BY789",
        "name"                => "Ad",
        "surname"             => "Soyad",
        "gsmNumber"           => "+905350000000",
        "email"               => "musteri@example.com",
        "identityNumber"      => "74300864791",
        "registrationAddress" => "Adres",
        "ip"                  => "85.34.78.112",
        "city"                => "Istanbul",
        "country"             => "Turkey"
    ],
    "shippingAddress" => [
        "contactName" => "Ad Soyad",
        "city"        => "Istanbul",
        "country"     => "Turkey",
        "address"     => "Adres"
    ],
    "billingAddress" => [
        "contactName" => "Ad Soyad",
        "city"        => "Istanbul",
        "country"     => "Turkey",
        "address"     => "Adres"
    ],
    "basketItems" => [
        ["id" => "BI101", "name" => "Ürün", "category1" => "Elektronik",
         "itemType" => "PHYSICAL", "price" => "100.0"]
    ]
]);

if (iyzico_success($result)) {
    print("Ödeme başarılı! paymentId: " . $result["paymentId"]);
} else {
    print("Hata: " . iyzico_error($result));
}
```

## Fonksiyonlar

### Yapılandırma
| Fonksiyon | Açıklama |
|-----------|----------|
| `iyzico_config($api_key, $secret_key, $sandbox)` | Config objesi oluşturur |

### Ödeme
| Fonksiyon | Açıklama |
|-----------|----------|
| `iyzico_pay($config, $request)` | Direkt ödeme (3DS'siz) |
| `iyzico_pay_retrieve($config, $payment_id, $conv_id)` | Ödeme sorgula |
| `iyzico_3ds_init($config, $request)` | 3DS başlat — htmlContent döner |
| `iyzico_3ds_complete($config, $conv_id, $result)` | 3DS callback sonrası tamamla |

### İptal / İade
| Fonksiyon | Açıklama |
|-----------|----------|
| `iyzico_cancel($config, $payment_id, $conv_id, $reason)` | Ödeme iptal |
| `iyzico_refund($config, $transaction_id, $price, $conv_id)` | Kısmi/tam iade |

### Kart Saklama
| Fonksiyon | Açıklama |
|-----------|----------|
| `iyzico_card_save($config, $external_id, $email, $card)` | Kart kaydet |
| `iyzico_card_list($config, $card_user_key)` | Kayıtlı kartlar |
| `iyzico_card_delete($config, $card_user_key, $card_token)` | Kart sil |

### Diğer
| Fonksiyon | Açıklama |
|-----------|----------|
| `iyzico_installments($config, $bin, $price)` | Taksit seçenekleri |
| `iyzico_success($result)` → bool | Başarı kontrolü |
| `iyzico_error($result)` → string\|null | Hata mesajı |
