// secrets.h

#include <pgmspace.h>

#define SECRET
#define THINGNAME "smartlock" // AWS IoTで作成したモノの名前
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";
const char AWS_IOT_ENDPOINT[] = "";

// https://www.amazontrust.com/repository/AmazonRootCA1.pem を開いて中身をコピペ
static const char AWS_CERT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
-----END CERTIFICATE-----
)EOF";

// --- Device Certificate ---
// xxxxxxxxxx-certificate.pem.crt
static const char AWS_CERT_CRT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
-----END CERTIFICATE-----
)EOF";

// --- Device Private Key ---
// xxxxxxxxxx-private.pem.key
static const char AWS_CERT_PRIVATE[] PROGMEM = R"EOF(
-----BEGIN RSA PRIVATE KEY-----
-----END RSA PRIVATE KEY-----
)EOF";

// --- NFC Card IDs (16進数文字列) ---
// FeliCaの場合：16桁（8バイト）、TypeAの場合：8〜20桁（4〜10バイト）
// 例: "0123456789ABCDEF"
const char* ALLOWED_CARD_IDS[] = {
    "0123456789ABCDEF"
};
const int ALLOWED_CARD_COUNT = sizeof(ALLOWED_CARD_IDS) / sizeof(ALLOWED_CARD_IDS[0]);