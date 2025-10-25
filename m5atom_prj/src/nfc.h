#ifndef NFC_H
#define NFC_H

#include <Arduino.h>
#include <Wire.h>
#include <PN532.h>
#include <PN532_I2C.h>

enum NFCStatus {
  NFC_OK,
  NFC_DISABLED,
  NFC_ERROR
};

enum CardType {
  CARD_NONE,
  CARD_FELICA,
  CARD_TYPEA
};

class NFCReader {
public:
  NFCReader();
  
  // 初期化（再試行あり）
  bool begin(int maxRetries = 3);
  
  // カード検知（戻り値：カードタイプ）
  CardType checkCard();
  
  // 最後に読み取ったカードIDを文字列で取得
  String getLastCardID();
  
  // 接続状態チェック＆再接続
  bool ensureConnection();
  
  // 現在の状態
  NFCStatus getStatus() const { return status; }
  
  // カードタイプを文字列に変換
  static const char* cardTypeToString(CardType type);

private:
  PN532_I2C* pn532i2c;
  PN532* nfc;
  NFCStatus status;
  
  String lastCardID;
  CardType lastCardType;
  unsigned long lastSeenTime;
  
  // 同じカードの連続読み取りを防ぐ
  bool isSameCard(CardType type, const String& id);
  
  // バイト配列を16進数文字列に変換
  String bytesToHexString(const uint8_t* data, uint8_t len);
};

#endif // NFC_H

