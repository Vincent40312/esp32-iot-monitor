#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> 
#include <PubSubClient.h> 
#include <DHT.h>
#include <time.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ==========================================
//  ESP32 智慧農場 v10.4 (MQTT + Discord 版)
//  功能：上傳數據 + 接收指令 + Discord 警報通知
// ==========================================

const char* ssid = "EVDS";
const char* password = "EVDS0501";

// --- [設定] MQTT 伺服器 (樹梅派) ---
const char* mqtt_server = "192.168.0.119"; // 請修改為樹梅派 IP
const int mqtt_port = 1883;                // ESP32 走 TCP Port 1883
const char* mqtt_user = "admin";                // 若有設帳密請填入
const char* mqtt_password = "12345678"; 

// --- [設定] Discord Webhook ---
// 請在 Discord 頻道設定 -> 整合 -> Webhooks 建立並複製網址
const char* discord_webhook = "https://discord.com/api/webhooks/1451100483338108989/xUJ9AdGTDRGTWvwPzPL8Qt8PPCGyar4XkBGNZ9Px39xBxNA2R39VCY--FJiuE322QmAA";

// MQTT Topics
const char* topic_data = "farm/monitor";    // 發送：數據
const char* topic_control = "farm/control"; // 接收：指令

// 其他設定
String writeApiKey = "EEOBAMUB5SO5P42G"; 
const int pumpPin = 2;    
const int fertPin = 5;    
const int soilPin = 34;   
const int olPumpPin = 18; // 積熱電驛 (水泵)
const int olFertPin = 19; // 積熱電驛 (施肥)

#define DHTPIN 4
#define DHTTYPE DHT11

const int soilLow = 20;
const int soilHigh = 80;
const int fertHour = 8;
const int fertDuration = 10;
const int pumpMaxRunTime = 10;
const int airValue = 4095;
const int waterValue = 1500;

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 28800; 
const int   daylightOffset_sec = 0;

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient); 

// --- 狀態變數 ---
bool autoMode = true;   
bool pumpSoftAlarm = false; 
unsigned long pumpStartTime = 0;
bool pumpRunning = false;
unsigned long fertStartTime = 0;
bool fertRunning = false;
bool fertJobDoneToday = false;

// --- 狀態追蹤 (防止 Discord 洗版用) ---
bool lastPumpOverloadState = false;
bool lastFertOverloadState = false;
bool lastSensorErrorState = false;

unsigned long lastUploadTime = 0;
const long uploadInterval = 60000; // ThingSpeak 備份上傳
unsigned long lastMqttTime = 0;
const long mqttInterval = 1000;    // MQTT 每秒上傳

// ==========================================
//  [核心] Discord 發送函式 (HTTPS)
// ==========================================
void sendDiscord(String content) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure secureClient;
    secureClient.setInsecure(); // 忽略 SSL 憑證驗證 (必要，否則 ESP32 連不上 Discord)
    
    HTTPClient https;
    if (https.begin(secureClient, discord_webhook)) {
      https.addHeader("Content-Type", "application/json");
      
      // 構建 JSON payload
      String payload = "{\"content\":\"" + content + "\"}";
      
      int httpResponseCode = https.POST(payload);
      if (httpResponseCode > 0) {
        Serial.println("Discord 發送成功: " + content);
      } else {
        Serial.print("Discord 發送失敗, Error code: ");
        Serial.println(httpResponseCode);
      }
      https.end();
    } else {
      Serial.println("無法連接 Discord 伺服器");
    }
  }
}

// ==========================================
//  [核心] MQTT 接收訊息回調函式 (Callback)
// ==========================================
void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  msg.trim(); // 去除前後空白
  Serial.println("收到 MQTT 指令: [" + msg + "]");

  // 檢查硬體過載狀態
  bool pumpOverload = (digitalRead(olPumpPin) == LOW);
  bool fertOverload = (digitalRead(olFertPin) == LOW);

  // --- 指令解析與執行 ---
  if (msg == "STOP") {
      autoMode = false;
      digitalWrite(pumpPin, LOW); pumpRunning = false;
      digitalWrite(fertPin, LOW); fertRunning = false;
      Serial.println("執行：緊急停機");
      sendDiscord("🔴 [警報] 收到遠端 STOP 指令，系統已緊急停機！");
  }
  else if (msg == "AUTO_ON") {
      autoMode = true;
      Serial.println("執行：切換為自動模式");
      sendDiscord("🟢 系統已切換為：自動模式 (Auto)");
  }
  else if (msg == "AUTO_OFF") {
      autoMode = false;
      Serial.println("執行：切換為手動模式");
      sendDiscord("🟠 系統已切換為：手動模式 (Manual)");
  }
  else if (msg == "LED1_ON") { // 開水泵
      if (!pumpOverload) {
          if(fertRunning) { digitalWrite(fertPin, LOW); fertRunning = false; } // 互斥
          digitalWrite(pumpPin, HIGH); pumpRunning = true; pumpStartTime = millis();
          Serial.println("執行：水泵開啟");
      } else {
          Serial.println("拒絕：水泵過載中");
          sendDiscord("⚠️ [拒絕] 嘗試開啟水泵失敗：過載保護中");
      }
  }
  else if (msg == "LED1_OFF") { // 關水泵
      digitalWrite(pumpPin, LOW); pumpRunning = false;
      Serial.println("執行：水泵關閉");
  }
  else if (msg == "LED2_ON") { // 開施肥
      if (!fertOverload) {
          if(pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; } // 互斥
          digitalWrite(fertPin, HIGH); fertRunning = true; fertStartTime = millis();
          Serial.println("執行：施肥開啟");
      } else {
          Serial.println("拒絕：施肥過載中");
          sendDiscord("⚠️ [拒絕] 嘗試開啟施肥失敗：過載保護中");
      }
  }
  else if (msg == "LED2_OFF") { // 關施肥
      digitalWrite(fertPin, LOW); fertRunning = false;
      Serial.println("執行：施肥關閉");
  }
}

// --- MQTT 重連 ---
void reconnectMQTT() {
  if (!client.connected()) {
    Serial.print("嘗試連接 MQTT...");
    String clientId = "ESP32-" + String(random(0xffff), HEX);
    
    // 如果有帳密: client.connect(clientId.c_str(), mqtt_user, mqtt_password)
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
      Serial.println("已連接");
      
      // [重要] 連線成功後，一定要訂閱控制主題
      client.subscribe(topic_control);
      Serial.println("已訂閱主題: " + String(topic_control));
      
    } else {
      Serial.print("失敗 rc="); Serial.print(client.state());
    }
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  
  pinMode(pumpPin, OUTPUT); pinMode(fertPin, OUTPUT);
  digitalWrite(pumpPin, LOW); digitalWrite(fertPin, LOW); 
  pinMode(soilPin, INPUT);
  pinMode(olPumpPin, INPUT_PULLUP); pinMode(olFertPin, INPUT_PULLUP);

  dht.begin();

  // 強制 DNS
  WiFi.config(IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(8,8,8,8), IPAddress(8,8,4,4));
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\nWiFi Connected");
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // 設定 MQTT Server 與 Callback
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback); // 註冊接收函式

  sendDiscord("✅ ESP32 智慧農場系統已啟動連線");
}

void loop() {
  if(WiFi.status() == WL_CONNECTED){
    
    // 保持 MQTT 連線
    if (!client.connected()) {
        static unsigned long lastReconnect = 0;
        if (millis() - lastReconnect > 5000) {
            lastReconnect = millis();
            reconnectMQTT();
        }
    }
    client.loop(); // 這是接收指令的關鍵，必須一直執行

    unsigned long currentMillis = millis();
    struct tm timeinfo;
    bool timeSynced = getLocalTime(&timeinfo);

    float hum = dht.readHumidity();
    float temp = dht.readTemperature();
    int soilRaw = analogRead(soilPin);
    int soilPercent = map(soilRaw, airValue, waterValue, 0, 100);
    soilPercent = constrain(soilPercent, 0, 100);
    bool sensorError = (isnan(hum) || isnan(temp) || soilRaw == 0);

    // [Fix] 防止 JSON 解析錯誤: 若數值為 NaN，強制設為 0
    // 這很重要，因為 "nan" 字串在 JSON 中是不合法的，會導致網頁端 JSON.parse() 失敗
    if (isnan(hum)) hum = 0;
    if (isnan(temp)) temp = 0;

    // --- Discord 警報邏輯 (狀態邊緣檢測，避免洗版) ---
    // 1. 感測器故障檢測
    if (sensorError && !lastSensorErrorState) {
        sendDiscord("⚠️ [故障] 溫濕度或土壤感測器讀取異常，請檢查線路！");
    }
    lastSensorErrorState = sensorError;

    // --- 過載保護 ---
    bool pumpOverload = (digitalRead(olPumpPin) == LOW);
    bool fertOverload = (digitalRead(olFertPin) == LOW);
    
    // 2. 水泵過載檢測
    if (pumpOverload && !lastPumpOverloadState) {
        digitalWrite(pumpPin, LOW); pumpRunning = false;
        sendDiscord("🚨 [嚴重警報] 水泵積熱電驛跳脫 (Pump Overload)！系統已強制停機。");
    }
    lastPumpOverloadState = pumpOverload;

    // 3. 施肥過載檢測
    if (fertOverload && !lastFertOverloadState) {
        digitalWrite(fertPin, LOW); fertRunning = false;
        sendDiscord("🚨 [嚴重警報] 施肥機積熱電驛跳脫 (Fert Overload)！系統已強制停機。");
    }
    lastFertOverloadState = fertOverload;

    // 確保狀態同步 (如果手動復歸，也要關閉馬達以防誤動作)
    if (pumpOverload && pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; }
    if (fertOverload && fertRunning) { digitalWrite(fertPin, LOW); fertRunning = false; }

    // --- 自動化邏輯 ---
    if (autoMode) {
        // [施肥]
        if (timeSynced && !fertOverload) { 
          if (timeinfo.tm_hour == fertHour && timeinfo.tm_min == 0 && !fertRunning && !fertJobDoneToday) {
            if (pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; } 
            digitalWrite(fertPin, HIGH); fertRunning = true; fertStartTime = currentMillis; fertJobDoneToday = true;
            sendDiscord("💧 [自動排程] 開始執行施肥作業");
          }
          if (fertRunning && (currentMillis - fertStartTime) >= (fertDuration * 60 * 1000)) {
               digitalWrite(fertPin, LOW); fertRunning = false;
               sendDiscord("✅ [自動排程] 施肥作業完成");
          }
          if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) fertJobDoneToday = false;
        }
        // [澆水]
        if (!pumpSoftAlarm && !pumpOverload && !sensorError) { 
          if (soilPercent < soilLow && !pumpRunning && !fertRunning) {
            digitalWrite(pumpPin, HIGH); pumpRunning = true; pumpStartTime = currentMillis;
          }
          else if (soilPercent > soilHigh && pumpRunning) {
            digitalWrite(pumpPin, LOW); pumpRunning = false;
          }
        } else if ((sensorError || pumpOverload) && pumpRunning) {
            digitalWrite(pumpPin, LOW); pumpRunning = false;
        }
    } 
    
    // 4. 超時鎖定警報
    if (pumpRunning && ((currentMillis - pumpStartTime) / 60000 >= pumpMaxRunTime)) {
        digitalWrite(pumpPin, LOW); pumpRunning = false; pumpSoftAlarm = true;
        sendDiscord("⚠️ [超時警報] 水泵運轉超過限制時間 (10分鐘)，已強制鎖定。請檢查管線是否破裂或土壤感測器失效。");
    }

    int status = 0;
    if (pumpRunning)   status |= 1;
    if (fertRunning)   status |= 2;
    if (pumpSoftAlarm) status |= 4;
    if (sensorError)   status |= 8;
    if (autoMode)      status |= 16;
    if (pumpOverload)  status |= 32;
    if (fertOverload)  status |= 64;

    // --- MQTT 發送數據 (每秒) ---
    if (currentMillis - lastMqttTime >= mqttInterval) {
        lastMqttTime = currentMillis;
        if (client.connected()) {
            String json = "{\"temp\":" + String(temp, 1) + 
                          ",\"hum\":" + String(hum, 1) + 
                          ",\"soil\":" + String(soilPercent) + 
                          ",\"status\":" + String(status) + "}";
            client.publish(topic_data, json.c_str());
        }
    }

    // --- ThingSpeak 備份上傳 (每分鐘) ---
    // 修正: 統一使用 lastUploadTime 變數
    if (currentMillis - lastUploadTime >= uploadInterval) {
      lastUploadTime = currentMillis;
      String url = "http://api.thingspeak.com/update?api_key=" + writeApiKey + 
                   "&field1=" + String(temp) + "&field2=" + String(hum) + 
                   "&field3=" + String(soilPercent) + "&field4=" + String(status); 
      HTTPClient http;
      http.begin(url.c_str());
      http.GET();
      http.end();
    }
  } else {
      WiFi.disconnect(); WiFi.reconnect(); delay(1000);
  }
  delay(10); // 縮短 delay 以加快 MQTT 響應
}
