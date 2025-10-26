#include <Arduino.h>
#include <M5Unified.h>
#include <Wire.h>
#include <ESP32Servo.h> 
#include "Adafruit_VL53L0X.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <MQTTClient.h>
#include "secrets.h"
#include "nfc.h"

// システムモード定義
enum SystemMode {
  NORMAL,          // 通常状態
  WAITING_MODE     // ドア開閉待機モード
};

// センサー
Adafruit_VL53L0X lox = Adafruit_VL53L0X();

// NFC カードリーダー
NFCReader nfcReader;

// サーボ
const int SERVO_PIN = 5; 
Servo myServo;
int currentAngle = 90;
const int MOVE_DELAY = 600;

// AWS IoT Core設定
const uint16_t AWS_PORT = 8883;
const char* topicSub = "smartlock/cmd";
const char* topicPub = "smartlock/log";

// WiFi/MQTT/UDP
WiFiClientSecure wifi_s;
MQTTClient client = MQTTClient(256);
WiFiUDP udpControl;
const uint16_t UDP_PORT = 4210;

// タイミング定数
const unsigned long WAITING_TIMEOUT = 15000; // 15秒
const unsigned long WIFI_CHECK_INTERVAL = 30000; // 30秒
const unsigned long MQTT_CHECK_INTERVAL = 30000; // 30秒
const unsigned long NFC_CONNECTION_CHECK_INTERVAL = 10000; // 10秒
const unsigned long NFC_CHECK_INTERVAL = 150; // 150ms
const unsigned long DISPLAY_UPDATE_INTERVAL = 100; // 100ms
const unsigned long DOOR_DEBOUNCE_TIME = 2000; // 2秒
const unsigned long WIFI_RECONNECT_TIMEOUT = 10000; // 10秒
const unsigned long MAX_ERROR_COUNT = 5; // 連続エラー上限

// システム状態管理
SystemMode currentMode = NORMAL;
unsigned long modeStartTime = 0;

// ドア状態検知
enum DoorState { DOOR_OPEN, DOOR_CLOSE };
DoorState doorState = DOOR_OPEN;
DoorState lastDoorState = DOOR_OPEN;
unsigned long doorCloseStartTime = 0;
bool hasSeenOpenInWaitingMode = false;

// NFC状態
String lastNfcCardID = "";
CardType lastNfcCardType = CARD_NONE;
unsigned long lastNfcCheckTime = 0;

// エラーカウント
unsigned int wifiErrorCount = 0;
unsigned int nfcErrorCount = 0;

// マルチタスク管理
TaskHandle_t wifiTaskHandle = NULL;
volatile bool isWifiReconnecting = false;

// ログをAWS IoT Coreに送信
void publishLog(const String& msg) {
  if (!client.connected()) return;
  Serial.println("[LOG] " + msg);
  client.publish(topicPub, msg.c_str());
}


// サーボでドアを開ける
void openDoor() {
  myServo.write(90);
  delay(MOVE_DELAY);
  myServo.write(155);
  delay(MOVE_DELAY);
  myServo.write(90);
  delay(MOVE_DELAY);
  publishLog("Door opened");
}

// サーボでドアを閉める
void closeDoor() {
  myServo.write(90);
  delay(MOVE_DELAY);
  myServo.write(15);
  delay(MOVE_DELAY);
  myServo.write(90);
  delay(MOVE_DELAY);
  publishLog("Door closed");
}

// コマンド処理
void handleCommand(const String& payload) {
  String cmd = payload;
  cmd.trim();
  
  if (cmd == "openlock") {
    openDoor();
    currentMode = WAITING_MODE;
    modeStartTime = millis();
    hasSeenOpenInWaitingMode = false;
    publishLog("Command: openlock, switched to WAITING_MODE");
  } 
  else if (cmd == "closelock") {
    closeDoor();
    publishLog("Command: closelock");
  }
}

// MQTT受信コールバック
void onMqttMessage(String &topic, String &payload) {
  Serial.printf("[MQTT] Topic: %s, Payload: %s\n", topic.c_str(), payload.c_str());
  handleCommand(payload);
}

// WiFi/MQTT管理タスク（Core 0で並列実行）
void wifiMaintainTask(void* parameter) {
  while (true) {
    // WiFi接続チェック
    if (WiFi.status() != WL_CONNECTED) {
      isWifiReconnecting = true;
      Serial.println("[WiFi] Reconnecting...");
      
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      
      unsigned long startAttempt = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_RECONNECT_TIMEOUT) {
        delay(500);
      }
      
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[WiFi] Reconnected");
        
        // UDP再初期化
        udpControl.stop();
        udpControl.begin(UDP_PORT);
        Serial.println("[UDP] Restarted");
        
        publishLog("WiFi reconnected");
        wifiErrorCount = 0;
      } else {
        Serial.println("[WiFi] Reconnection failed");
        wifiErrorCount++;
        
        // 連続エラーが多い場合は再起動
        if (wifiErrorCount >= MAX_ERROR_COUNT) {
          Serial.println("[WiFi] Too many errors, restarting...");
          delay(2000);
          ESP.restart();
        }
      }
      
      isWifiReconnecting = false;
    } else {
      wifiErrorCount = 0;
      
      // MQTT接続チェック
      if (!client.connected()) {
        if (client.connect(THINGNAME)) {
          client.subscribe(topicSub);
          publishLog("Connected to AWS IoT");
        }
      }
    }
    
    // 30秒待機
    vTaskDelay(WIFI_CHECK_INTERVAL / portTICK_PERIOD_MS);
  }
}

// UDP受信処理
void processUdp() {
  int packetSize = udpControl.parsePacket();
  if (packetSize > 0) {
    char buffer[256];
    int len = udpControl.read(buffer, sizeof(buffer) - 1);
    if (len > 0) {
      buffer[len] = '\0';
      String payload(buffer);
      Serial.println("[UDP] " + payload);
      handleCommand(payload);
    }
  }
}

// カードID照合
bool isCardAllowed(const String& cardID) {
  for (int i = 0; i < ALLOWED_CARD_COUNT; i++) {
    if (cardID == ALLOWED_CARD_IDS[i]) {
      return true;
    }
  }
  return false;
}

// NFC処理（ポーリング間隔を設けて高速化）
void processNfc() {
  static unsigned long lastNfcCheck = 0;
  
  if (millis() - lastNfcCheck < NFC_CHECK_INTERVAL) {
    return;
  }
  lastNfcCheck = millis();
  
  NFCStatus nfcStatus = nfcReader.getStatus();
  if (nfcStatus == NFC_ERROR) {
    nfcErrorCount++;
    
    // 連続エラーが多い場合は再起動
    if (nfcErrorCount >= MAX_ERROR_COUNT) {
      Serial.println("[NFC] Too many errors, restarting...");
      M5.Display.clear();
      M5.Display.setTextColor(RED);
      M5.Display.println("NFC ERROR");
      M5.Display.println("Restarting...");
      delay(2000);
      ESP.restart();
    }
    return;
  } else if (nfcStatus != NFC_OK) {
    return;
  }
  
  nfcErrorCount = 0; // 正常時はリセット
  
  CardType cardType = nfcReader.checkCard();
  if (cardType != CARD_NONE) {
    String cardID = nfcReader.getLastCardID();
    lastNfcCardID = cardID;
    lastNfcCardType = cardType;
    
    // カードID照合
    if (isCardAllowed(cardID)) {
      publishLog("Card accepted: " + String(NFCReader::cardTypeToString(cardType)) + " ID=" + cardID);
      openDoor();
      currentMode = WAITING_MODE;
      modeStartTime = millis();
      hasSeenOpenInWaitingMode = false;
    } else {
      publishLog("Card rejected: " + String(NFCReader::cardTypeToString(cardType)) + " ID=" + cardID);
    }
  }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Lcd.setRotation(2);

  // サーボ初期化
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  myServo.attach(SERVO_PIN, 500, 2400);
  myServo.write(90);

  M5.Display.setTextSize(2);
  M5.Display.setTextColor(GREEN);
  M5.Display.println("Booting...");

  // センサー初期化
  if (!lox.begin()) {
    M5.Display.clear();
    M5.Display.setTextColor(RED);
    M5.Display.println("Sensor");
    M5.Display.println("Error!");
    while (1);
  }

  M5.Display.clear();
  M5.Display.println("Sensor OK");
  delay(500);

  // NFC初期化
  M5.Display.println("NFC...");
  bool nfcOk = nfcReader.begin(3);
  if (nfcOk) {
    M5.Display.println("NFC OK");
  } else {
    M5.Display.println("NFC Disabled");
  }
  delay(500);

  // WiFi接続
  M5.Display.println("WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }
  M5.Display.println("WiFi OK");

  // UDP開始
  udpControl.begin(UDP_PORT);

  // AWS IoT Core接続
  wifi_s.setCACert(AWS_CERT_CA);
  wifi_s.setCertificate(AWS_CERT_CRT);
  wifi_s.setPrivateKey(AWS_CERT_PRIVATE);
  client.begin(AWS_IOT_ENDPOINT, AWS_PORT, wifi_s);
  client.onMessage(onMqttMessage);
  
  // 初回MQTT接続
  if (client.connect(THINGNAME)) {
    client.subscribe(topicSub);
    publishLog("Connected to AWS IoT");
  }

  // WiFi/MQTT管理タスクをCore 0で起動
  xTaskCreatePinnedToCore(
    wifiMaintainTask,   // タスク関数
    "WiFiMaintain",     // タスク名
    8192,               // スタックサイズ
    NULL,               // パラメータ
    1,                  // 優先度
    &wifiTaskHandle,    // タスクハンドル
    0                   // Core 0で実行
  );
  
  M5.Display.clear();
  M5.Display.println("Ready!");
  delay(1000);
  publishLog("System started");
}

// ディスプレイ更新
void updateDisplay() {
  M5.Display.clear();
  M5.Display.setCursor(0, 5);
  M5.Display.setTextSize(2);

  // WiFi再接続中の表示（最優先）
  if (isWifiReconnecting) {
    M5.Display.setTextColor(YELLOW);
    M5.Display.println("WiFi");
    M5.Display.println("Reconnecting...");
    return;
  }

  // モード表示
  if (currentMode == NORMAL) {
    M5.Display.setTextColor(CYAN);
    M5.Display.println("MODE: NORMAL");
  } else {
    M5.Display.setTextColor(YELLOW);
    M5.Display.println("MODE: WAITING");
    unsigned long remaining = (WAITING_TIMEOUT - (millis() - modeStartTime)) / 1000;
    M5.Display.printf("Timer: %lus\n", remaining);
  }

  // ドア状態表示
  M5.Display.setTextSize(4);
  if (doorState == DOOR_CLOSE) {
    M5.Display.setTextColor(RED);
    M5.Display.println("CLOSE");
  } else {
    M5.Display.setTextColor(GREEN);
    M5.Display.println("OPEN");
  }
  
  // NFC状態表示（小さな文字）
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(WHITE);
  M5.Display.setCursor(0, M5.Display.height() - 40);
  
  NFCStatus nfcStatus = nfcReader.getStatus();
  if (nfcStatus == NFC_OK) {
    M5.Display.println("NFC: OK");
  } else if (nfcStatus == NFC_ERROR) {
    M5.Display.println("NFC: ERROR");
  } else {
    M5.Display.println("NFC: DISABLED");
  }
  
  // 最後に読み取ったカード情報
  if (lastNfcCardID.length() > 0) {
    M5.Display.print("Last: ");
    M5.Display.print(NFCReader::cardTypeToString(lastNfcCardType));
    M5.Display.println();
    // IDは長いので先頭8文字のみ表示
    M5.Display.print("ID: ");
    M5.Display.println(lastNfcCardID.substring(0, min(12, (int)lastNfcCardID.length())));
  }
}

void loop() {
  M5.update();

  // NFC接続維持
  static unsigned long lastNfcConnectionCheck = 0;
  if (millis() - lastNfcConnectionCheck > NFC_CONNECTION_CHECK_INTERVAL) {
    nfcReader.ensureConnection();
    lastNfcConnectionCheck = millis();
  }

  // UDP受信とMQTTメッセージ処理
  processUdp();
  client.loop();
  
  // NFC処理
  processNfc();

  // ボタン処理
  if (M5.BtnA.wasPressed()) {
    if (currentMode == NORMAL) {
      currentMode = WAITING_MODE;
      modeStartTime = millis();
      hasSeenOpenInWaitingMode = false;
      publishLog("Button pressed: switched to WAITING_MODE");
    } else {
      currentMode = NORMAL;
      publishLog("Button pressed: switched to NORMAL");
    }
  }

  // 距離センサー読み取り
  VL53L0X_RangingMeasurementData_t measure;
  lox.rangingTest(&measure, false);

  bool isCurrentlyClose = (measure.RangeStatus != 4 && measure.RangeMilliMeter < 40);

  // ドア状態判定（デバウンス）
  if (isCurrentlyClose) {
    if (doorCloseStartTime == 0) {
      doorCloseStartTime = millis();
    } else if (millis() - doorCloseStartTime >= DOOR_DEBOUNCE_TIME) {
      lastDoorState = doorState;
      doorState = DOOR_CLOSE;
    }
  } else {
    doorCloseStartTime = 0;
    lastDoorState = doorState;
    doorState = DOOR_OPEN;
  }

  // 待機モード処理
  if (currentMode == WAITING_MODE) {
    // タイムアウトチェック
    if (millis() - modeStartTime >= WAITING_TIMEOUT) {
      currentMode = NORMAL;
      publishLog("WAITING_MODE timeout: switched to NORMAL");
    }

    // CLOSE→OPEN→CLOSE検知
    if (doorState == DOOR_OPEN && lastDoorState == DOOR_CLOSE) {
      hasSeenOpenInWaitingMode = true;
      publishLog("Detected CLOSE->OPEN in WAITING_MODE");
    }

    if (doorState == DOOR_CLOSE && lastDoorState == DOOR_OPEN && hasSeenOpenInWaitingMode) {
      closeDoor();
      currentMode = NORMAL;
      hasSeenOpenInWaitingMode = false;
      publishLog("Auto-closed door after CLOSE->OPEN->CLOSE");
    }
  }

  // ディスプレイ更新（高速化のため頻度を下げる）
  static unsigned long lastDisplayUpdate = 0;
  if (millis() - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
    updateDisplay();
    lastDisplayUpdate = millis();
  }
  
  delay(10);  // 短いdelayでCPU負荷を軽減しつつ応答性を維持
}