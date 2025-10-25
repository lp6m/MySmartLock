#include "nfc.h"

#define I2C_SDA_PIN 2
#define I2C_SCL_PIN 1
#define CARD_COOLDOWN_MS 2000  // 同じカードの連続読み取り防止

NFCReader::NFCReader() : pn532i2c(nullptr), nfc(nullptr), status(NFC_DISABLED), 
                         lastCardType(CARD_NONE), lastSeenTime(0) {
}

bool NFCReader::begin(int maxRetries) {
  // I2C初期化
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(50000UL);
  
  // PN532初期化（再試行あり）
  for (int attempt = 0; attempt < maxRetries; attempt++) {
    pn532i2c = new PN532_I2C(Wire);
    nfc = new PN532(*pn532i2c);
    
    nfc->begin();
    delay(100);
    
    uint32_t ver = nfc->getFirmwareVersion();
    if (ver) {
      // 初期化成功
      Serial.printf("[NFC] PN5%02X FW %d.%d initialized\n", 
                    (ver>>24)&0xFF, (ver>>16)&0xFF, (ver>>8)&0xFF);
      
      nfc->setPassiveActivationRetries(0xFF);
      nfc->SAMConfig();
      
      status = NFC_OK;
      return true;
    }
    
    // 失敗したらクリーンアップして再試行
    delete nfc;
    delete pn532i2c;
    nfc = nullptr;
    pn532i2c = nullptr;
    
    Serial.printf("[NFC] Init attempt %d failed, retrying...\n", attempt + 1);
    delay(500);
  }
  
  // 初期化失敗
  Serial.println("[NFC] Init failed after retries. NFC disabled.");
  status = NFC_DISABLED;
  return false;
}

bool NFCReader::ensureConnection() {
  if (status == NFC_DISABLED) {
    return false;
  }
  
  // 定期的に接続確認（getFirmwareVersionでチェック）
  uint32_t ver = nfc->getFirmwareVersion();
  if (!ver) {
    Serial.println("[NFC] Connection lost. Attempting to reconnect...");
    status = NFC_ERROR;
    
    // 再初期化試行
    delete nfc;
    delete pn532i2c;
    
    if (begin(3)) {
      Serial.println("[NFC] Reconnected successfully");
      return true;
    } else {
      Serial.println("[NFC] Reconnection failed. NFC disabled.");
      status = NFC_DISABLED;
      return false;
    }
  }
  
  if (status == NFC_ERROR) {
    status = NFC_OK;
  }
  return true;
}

CardType NFCReader::checkCard() {
  if (status != NFC_OK || nfc == nullptr) {
    return CARD_NONE;
  }
  
  // 1) FeliCa検知（タイムアウト短縮：10ms）
  {
    uint8_t idm[8], pmm[8];
    uint16_t sysCodeResp = 0;
    uint8_t ok = nfc->felica_Polling(0xFFFF, 0x01, idm, pmm, &sysCodeResp, 10);
    
    if (ok == 1) {
      String cardID = bytesToHexString(idm, 8);
      if (!isSameCard(CARD_FELICA, cardID)) {
        lastCardID = cardID;
        lastCardType = CARD_FELICA;
        lastSeenTime = millis();
        return CARD_FELICA;
      }
    }
  }
  
  // 2) Type A検知（タイムアウト短縮：10ms）
  {
    uint8_t uid[10] = {0};
    uint8_t uidLen = 0;
    bool ok = nfc->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 10);
    
    if (ok && uidLen > 0) {
      String cardID = bytesToHexString(uid, uidLen);
      if (!isSameCard(CARD_TYPEA, cardID)) {
        lastCardID = cardID;
        lastCardType = CARD_TYPEA;
        lastSeenTime = millis();
        return CARD_TYPEA;
      }
    }
  }
  
  return CARD_NONE;
}

String NFCReader::getLastCardID() {
  return lastCardID;
}

bool NFCReader::isSameCard(CardType type, const String& id) {
  if (type != lastCardType) {
    return false;
  }
  if (id != lastCardID) {
    return false;
  }
  if (millis() - lastSeenTime > CARD_COOLDOWN_MS) {
    return false;
  }
  return true;
}

String NFCReader::bytesToHexString(const uint8_t* data, uint8_t len) {
  String result = "";
  for (uint8_t i = 0; i < len; i++) {
    if (data[i] < 0x10) {
      result += "0";
    }
    result += String(data[i], HEX);
  }
  result.toUpperCase();
  return result;
}

const char* NFCReader::cardTypeToString(CardType type) {
  switch (type) {
    case CARD_FELICA: return "FeliCa";
    case CARD_TYPEA:  return "TypeA";
    case CARD_NONE:   return "None";
    default:          return "Unknown";
  }
}

