#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> 
#include <PubSubClient.h> 
#include <DHT.h>
#include <time.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <Preferences.h> 

// ==========================================
//  ESP32 智慧農場 v10.6 (Auto-Switch Manual)
//  功能：手動介入時自動切換為手動模式 + 斷電記憶
// ==========================================

const char* ssid = "EVDS";
const char* password = "EVDS0501";

// --- [設定] MQTT 伺服器 ---
const char* mqtt_server = "192.168.0.119"; 
const int mqtt_port = 1883;                
const char* mqtt_user = "admin";                
const char* mqtt_password = "12345678"; 

// --- [設定] Discord Webhook ---
const char* discord_webhook = "https://discord.com/api/webhooks/1451100483338108989/xUJ9AdGTDRGTWvwPzPL8Qt8PPCGyar4XkBGNZ9Px39xBxNA2R39VCY--FJiuE322QmAA";

// MQTT Topics
const char* topic_data = "farm/monitor";    
const char* topic_control = "farm/control"; 

// 其他設定
String writeApiKey = "EEOBAMUB5SO5P42G"; 
const int pumpPin = 2;    
const int fertPin = 5;    
const int soilPin = 34;   
const int olPumpPin = 18; 
const int olFertPin = 19; 

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
Preferences prefs; 

// --- 狀態變數 ---
bool autoMode = true;   
bool pumpSoftAlarm = false; 
unsigned long pumpStartTime = 0;
bool pumpRunning = false;
unsigned long fertStartTime = 0;
bool fertRunning = false;
bool fertJobDoneToday = false;

// --- 狀態追蹤 ---
bool lastPumpOverloadState = false;
bool lastFertOverloadState = false;
bool lastSensorErrorState = false;

unsigned long lastUploadTime = 0;
const long uploadInterval = 60000; 
unsigned long lastMqttTime = 0;
const long mqttInterval = 1000;    

// ==========================================
//  [核心] Discord 發送函式
// ==========================================
void sendDiscord(String content) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure secureClient;
    secureClient.setInsecure(); 
    HTTPClient https;
    if (https.begin(secureClient, discord_webhook)) {
      https.addHeader("Content-Type", "application/json");
      String payload = "{\"content\":\"" + content + "\"}";
      int httpResponseCode = https.POST(payload);
      if (httpResponseCode > 0) {
        Serial.println("Discord 發送成功: " + content);
      }
      https.end();
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
  msg.trim(); 
  Serial.println("收到 MQTT 指令: [" + msg + "]");

  bool pumpOverload = (digitalRead(olPumpPin) == LOW);
  bool fertOverload = (digitalRead(olFertPin) == LOW);

  // ✅ [新增功能] 檢查是否為手動指令，如果是，且目前是自動模式，強制切手動
  if (msg.startsWith("LED")) {
      if (autoMode) {
          autoMode = false;
          prefs.putBool("is_auto", false); // 寫入記憶
          Serial.println("⚠️ 偵測到手動指令，強制切換為手動模式 (Manual)");
          sendDiscord("👋 [手動介入] 收到開關指令，系統已自動切換為：手動模式");
      }
  }

  // --- 指令解析與執行 ---
  if (msg == "STOP") {
      autoMode = false;
      prefs.putBool("is_auto", false); 
      digitalWrite(pumpPin, LOW); pumpRunning = false;
      digitalWrite(fertPin, LOW); fertRunning = false;
      Serial.println("執行：緊急停機");
      sendDiscord("🔴 [警報] 收到遠端 STOP 指令，系統已緊急停機！");
  }
  else if (msg == "AUTO_ON") {
      autoMode = true;
      prefs.putBool("is_auto", true); 
      Serial.println("執行：切換為自動模式");
      sendDiscord("🟢 系統已切換為：自動模式 (Auto)");
  }
  else if (msg == "AUTO_OFF") {
      autoMode = false;
      prefs.putBool("is_auto", false); 
      Serial.println("執行：切換為手動模式");
      sendDiscord("🟠 系統已切換為：手動模式 (Manual)");
  }
  else if (msg == "LED1_ON") { // 開水泵
      if (!pumpOverload) {
          if(fertRunning) { digitalWrite(fertPin, LOW); fertRunning = false; } 
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
          if(pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; } 
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
    
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
      Serial.println("已連接");
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

  WiFi.config(IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(8,8,8,8), IPAddress(8,8,4,4));
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.println("\nWiFi Connected");
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback); 

  // 開機讀取上次狀態
  prefs.begin("farm_config", false); 
  autoMode = prefs.getBool("is_auto", true); 
  Serial.print("系統已啟動，恢復模式為: ");
  Serial.println(autoMode ? "自動 (Auto)" : "手動 (Manual)");
  
  String bootMsg = "✅ ESP32 系統已啟動 (模式: " + String(autoMode ? "自動" : "手動") + ")";
  sendDiscord(bootMsg);
}

void loop() {
  if(WiFi.status() == WL_CONNECTED){
    
    if (!client.connected()) {
        static unsigned long lastReconnect = 0;
        if (millis() - lastReconnect > 5000) {
            lastReconnect = millis();
            reconnectMQTT();
        }
    }
    client.loop(); 

    unsigned long currentMillis = millis();
    struct tm timeinfo;
    bool timeSynced = getLocalTime(&timeinfo);

    // 每日 03:00 自動重啟
    if (timeSynced && timeinfo.tm_hour == 3 && timeinfo.tm_min == 0 && millis() > 120000) {
        Serial.println("⏰ 凌晨 3 點到了！執行每日例行重啟...");
        sendDiscord("🔄 [系統維護] 執行每日例行重啟...");
        prefs.end(); 
        delay(1000);
        ESP.restart(); 
    }

    float hum = dht.readHumidity();
    float temp = dht.readTemperature();
    int soilRaw = analogRead(soilPin);
    int soilPercent = map(soilRaw, airValue, waterValue, 0, 100);
    soilPercent = constrain(soilPercent, 0, 100);
    bool sensorError = (isnan(hum) || isnan(temp) || soilRaw == 0);

    if (isnan(hum)) hum = 0;
    if (isnan(temp)) temp = 0;

    // --- Discord 警報 ---
    if (sensorError && !lastSensorErrorState) {
        sendDiscord("⚠️ [故障] 溫濕度或土壤感測器讀取異常，請檢查線路！");
    }
    lastSensorErrorState = sensorError;

    // --- 過載保護 ---
    bool pumpOverload = (digitalRead(olPumpPin) == LOW);
    bool fertOverload = (digitalRead(olFertPin) == LOW);
    
    if (pumpOverload && !lastPumpOverloadState) {
        digitalWrite(pumpPin, LOW); pumpRunning = false;
        sendDiscord("🚨 [嚴重警報] 水泵積熱電驛跳脫 (Pump Overload)！系統已強制停機。");
    }
    lastPumpOverloadState = pumpOverload;

    if (fertOverload && !lastFertOverloadState) {
        digitalWrite(fertPin, LOW); fertRunning = false;
        sendDiscord("🚨 [嚴重警報] 施肥機積熱電驛跳脫 (Fert Overload)！系統已強制停機。");
    }
    lastFertOverloadState = fertOverload;

    if (pumpOverload && pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; }
    if (fertOverload && fertRunning) { digitalWrite(fertPin, LOW); fertRunning = false; }

    // --- 自動化邏輯 ---
    if (autoMode) {
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
    
    if (pumpRunning && ((currentMillis - pumpStartTime) / 60000 >= pumpMaxRunTime)) {
        digitalWrite(pumpPin, LOW); pumpRunning = false; pumpSoftAlarm = true;
        sendDiscord("⚠️ [超時警報] 水泵運轉超過限制時間 (10分鐘)，已強制鎖定。");
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
  delay(10); 
}
