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
//  ESP32 智慧農場 v11.0 (RS485 Upgrade)
//  功能：RS485土壤感測 + 斷電記憶 + 接觸器回授保險
// ==========================================

const char* ssid = "EVDS";
const char* password = "EVDS0501";

// --- [設定] MQTT 伺服器 ---
const char* mqtt_server = "192.168.0.119"; 
const int mqtt_port = 1883;                
const char* mqtt_user = "admin";                
const char* mqtt_password = "12345678"; 

// --- [設定] Discord Webhook ---
const char* discord_webhook = "https://discord.com/";

// MQTT Topics
const char* topic_data = "farm/monitor";    
const char* topic_control = "farm/control"; 

// 其他設定
String writeApiKey = " "; 
const int pumpPin = 17;    
const int fertPin = 5;    
// const int soilPin = 34; // [移除] 舊類比腳位
const int olPumpPin = 18; 
const int olFertPin = 19; 

// [新增] RS485 定義
#define RX_PIN 26      // 連接 MAX485 RO
#define TX_PIN 27      // 連接 MAX485 DI
#define DE_RE_PIN 14   // 連接 MAX485 DE & RE
HardwareSerial rs485Serial(2); // 使用 UART2

// RS485 查詢指令 (Modbus RTU)
// 查詢地址01, 功能碼03, 起始暫存器0000, 讀取長度4個 (水分,溫度,EC,PH或鹽分)
// 請依照你的感測器說明書確認查詢碼，以下為通用型 NPK/5合1 感測器指令
const byte soilQuery[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x04, 0x44, 0x09}; 

// 電磁接觸器回授
const int fbPumpPin = 12;   
const int fbFertPin = 13;   

#define DHTPIN 4
#define DHTTYPE DHT11

const int soilLow = 20;
const int soilHigh = 80;
const int fertHour = 8;
const int fertDuration = 10;
const int pumpMaxRunTime = 10;
// const int airValue = 4095;   // [移除]
// const int waterValue = 1500; // [移除]

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 28800; 
const int   daylightOffset_sec = 0;

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient); 
Preferences prefs; 

// --- 變數 ---
// [新增] 土壤數值變數
float soil_hum = 0.0;
float soil_temp = 0.0;
int soil_ec = 0;
int soil_salinity = 0; // 或 PH，視感測器而定

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
bool lastFbPumpError = false;
bool lastFbFertError = false;

unsigned long stateChangeTime = 0;

unsigned long lastUploadTime = 0;
const long uploadInterval = 60000; 
unsigned long lastMqttTime = 0;
const long mqttInterval = 1000;    

// ==========================================
//  Discord 發送函式
// ==========================================
void sendDiscord(String content) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure secureClient;
    secureClient.setInsecure(); 
    HTTPClient https;
    if (https.begin(secureClient, discord_webhook)) {
      https.addHeader("Content-Type", "application/json");
      String payload = "{\"content\":\"" + content + "\"}";
      https.POST(payload);
      https.end();
    }
  }
}

// ==========================================
//  [新增] RS485 土壤數據讀取函式
// ==========================================
bool readSoilSensor() {
  digitalWrite(DE_RE_PIN, HIGH); // 切換為傳送模式 (TX)
  delay(10);
  rs485Serial.write(soilQuery, sizeof(soilQuery));
  rs485Serial.flush(); // 等待傳送完成
  digitalWrite(DE_RE_PIN, LOW);  // 切換為接收模式 (RX)
  
  // 等待回應 (最多等待 500ms)
  unsigned long timeout = millis();
  while (rs485Serial.available() < 13) { // 預期收到 13 Bytes (地址+功能+字節數+數據*4+CRC)
    if (millis() - timeout > 500) {
      return false; // 超時
    }
    delay(10);
  }

  byte buf[13];
  rs485Serial.readBytes(buf, 13);

  // 簡單檢查回應頭 (地址01, 功能03, 字節數08)
  if (buf[0] == 0x01 && buf[1] == 0x03 && buf[2] == 0x08) {
    // 解析數據 (依照通用協議: Hum, Temp, EC, Salinity/PH)
    // 數值通常為 Big Endian，且部分數值需除以100
    soil_temp = (buf[3] << 8 | buf[4]) / 100.0;
    soil_hum = (buf[5] << 8 | buf[6]) / 100.0;
    soil_ec = (buf[7] << 8 | buf[8]);
    soil_salinity = (buf[9] << 8 | buf[10]); // 如果是鹽分通常是 mg/L，如果是 PH 則是 /10.0

    return true; // 讀取成功
  }
  
  return false; // 數據校驗錯誤
}

// ==========================================
//  檢查電磁接觸器回授狀態
// ==========================================
void checkFeedback() {
    bool isPumpRealOn = (digitalRead(fbPumpPin) == LOW);
    bool isFertRealOn = (digitalRead(fbFertPin) == LOW);

    if (millis() - stateChangeTime < 2000) return;

    if (pumpRunning != isPumpRealOn) {
        if (!lastFbPumpError) {
            String msg = pumpRunning ? "⚠️ [回授異常] 水泵啟動失敗！" : "🚨 [危險警報] 水泵異常運轉！";
            Serial.println(msg);
            sendDiscord(msg);
            lastFbPumpError = true;
        }
    } else {
        lastFbPumpError = false;
    }

    if (fertRunning != isFertRealOn) {
        if (!lastFbFertError) {
            String msg = fertRunning ? "⚠️ [回授異常] 施肥機啟動失敗！" : "🚨 [危險警報] 施肥機異常運轉！";
            Serial.println(msg);
            sendDiscord(msg);
            lastFbFertError = true;
        }
    } else {
        lastFbFertError = false;
    }
}

// ==========================================
//  MQTT 回調函式
// ==========================================
void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  msg.trim(); 
  Serial.println("收到 MQTT: " + msg);

  bool pumpOverload = (digitalRead(olPumpPin) == LOW);
  bool fertOverload = (digitalRead(olFertPin) == LOW);

  if (msg.startsWith("LED") && autoMode) {
      autoMode = false;
      prefs.putBool("is_auto", false);
      sendDiscord("👋 [手動介入] 切換為手動模式");
  }
  
  if (msg == "STOP") {
      autoMode = false; prefs.putBool("is_auto", false); 
      digitalWrite(pumpPin, LOW); pumpRunning = false;
      digitalWrite(fertPin, LOW); fertRunning = false;
      stateChangeTime = millis();
      sendDiscord("🔴 [警報] 系統緊急停機！");
  }
  else if (msg == "AUTO_ON") {
      autoMode = true; prefs.putBool("is_auto", true); 
      sendDiscord("🟢 切換為自動模式");
  }
  else if (msg == "AUTO_OFF") {
      autoMode = false; prefs.putBool("is_auto", false); 
      sendDiscord("🟠 切換為手動模式");
  }
  else if (msg == "LED1_ON") { 
      if (!pumpOverload) {
          if(fertRunning) { digitalWrite(fertPin, LOW); fertRunning = false; } 
          digitalWrite(pumpPin, HIGH); pumpRunning = true; pumpStartTime = millis();
          stateChangeTime = millis();
      }
  }
  else if (msg == "LED1_OFF") { 
      digitalWrite(pumpPin, LOW); pumpRunning = false;
      stateChangeTime = millis();
  }
  else if (msg == "LED2_ON") { 
      if (!fertOverload) {
          if(pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; } 
          digitalWrite(fertPin, HIGH); fertRunning = true; fertStartTime = millis();
          stateChangeTime = millis();
      }
  }
  else if (msg == "LED2_OFF") { 
      digitalWrite(fertPin, LOW); fertRunning = false;
      stateChangeTime = millis();
  }
}

void reconnectMQTT() {
  if (!client.connected()) {
    String clientId = "ESP32-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
      client.subscribe(topic_control);
    }
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  
  // [新增] 初始化 RS485
  pinMode(DE_RE_PIN, OUTPUT);
  digitalWrite(DE_RE_PIN, LOW); // 預設接收模式
  rs485Serial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
  
  pinMode(pumpPin, OUTPUT); pinMode(fertPin, OUTPUT);
  digitalWrite(pumpPin, LOW); digitalWrite(fertPin, LOW); 
  
  pinMode(olPumpPin, INPUT_PULLUP); pinMode(olFertPin, INPUT_PULLUP);
  pinMode(fbPumpPin, INPUT_PULLUP); pinMode(fbFertPin, INPUT_PULLUP);

  stateChangeTime = millis();
  dht.begin();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback); 

  prefs.begin("farm_config", false); 
  autoMode = prefs.getBool("is_auto", true); 
  
  sendDiscord("✅ ESP32 系統已啟動 (RS485版)");
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

    if (timeSynced && timeinfo.tm_hour == 3 && timeinfo.tm_min == 0 && millis() > 120000) {
        prefs.end(); delay(1000); ESP.restart(); 
    }

    // --- 讀取環境數據 ---
    float airHum = dht.readHumidity();
    float airTemp = dht.readTemperature();
    if (isnan(airHum)) airHum = 0;
    if (isnan(airTemp)) airTemp = 0;

    // --- [修改] 讀取 RS485 土壤感測器 ---
    static unsigned long lastSensorRead = 0;
    bool rs485Success = true;
    
    // 每 2 秒讀取一次感測器，避免過度查詢阻塞
    if (currentMillis - lastSensorRead > 2000) {
        rs485Success = readSoilSensor();
        lastSensorRead = currentMillis;
    }

    // 判斷感測器是否故障 (DHT 或 RS485 讀取失敗)
    // 這裡我們把 soil_hum 視為主要控制依據
    bool sensorError = (isnan(airHum) || !rs485Success || soil_hum == 0.0);

    if (sensorError && !lastSensorErrorState) {
        sendDiscord("⚠️ [故障] 溫濕度或 RS485 土壤感測器讀取失敗！");
    }
    lastSensorErrorState = sensorError;

    // --- 過載保護 ---
    bool pumpOverload = (digitalRead(olPumpPin) == LOW);
    bool fertOverload = (digitalRead(olFertPin) == LOW);
    
    if (pumpOverload && !lastPumpOverloadState) {
        digitalWrite(pumpPin, LOW); pumpRunning = false;
        stateChangeTime = currentMillis; 
        sendDiscord("🚨 [警報] 水泵積熱電驛跳脫！");
    }
    lastPumpOverloadState = pumpOverload;

    if (fertOverload && !lastFertOverloadState) {
        digitalWrite(fertPin, LOW); fertRunning = false;
        stateChangeTime = currentMillis; 
        sendDiscord("🚨 [警報] 施肥機積熱電驛跳脫！");
    }
    lastFertOverloadState = fertOverload;

    if (pumpOverload && pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; stateChangeTime = currentMillis;}
    if (fertOverload && fertRunning) { digitalWrite(fertPin, LOW); fertRunning = false; stateChangeTime = currentMillis;}

    checkFeedback(); 

    // --- 自動化邏輯 (使用 soil_hum 替代舊的 soilPercent) ---
    if (autoMode) {
        if (timeSynced && !fertOverload) { 
          if (timeinfo.tm_hour == fertHour && timeinfo.tm_min == 0 && !fertRunning && !fertJobDoneToday) {
            if (pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; } 
            digitalWrite(fertPin, HIGH); fertRunning = true; fertStartTime = currentMillis; fertJobDoneToday = true;
            stateChangeTime = currentMillis; 
            sendDiscord("💧 [自動] 開始施肥");
          }
          if (fertRunning && (currentMillis - fertStartTime) >= (fertDuration * 60 * 1000)) {
               digitalWrite(fertPin, LOW); fertRunning = false;
               stateChangeTime = currentMillis; 
               sendDiscord("✅ [自動] 施肥完成");
          }
          if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) fertJobDoneToday = false;
        }
        
        if (!pumpSoftAlarm && !pumpOverload && !sensorError) { 
          // 這裡直接使用 soil_hum (RS485讀到的百分比)
          if (soil_hum < soilLow && !pumpRunning && !fertRunning) {
            digitalWrite(pumpPin, HIGH); pumpRunning = true; pumpStartTime = currentMillis;
            stateChangeTime = currentMillis; 
          }
          else if (soil_hum > soilHigh && pumpRunning) {
            digitalWrite(pumpPin, LOW); pumpRunning = false;
            stateChangeTime = currentMillis; 
          }
        } else if ((sensorError || pumpOverload) && pumpRunning) {
            digitalWrite(pumpPin, LOW); pumpRunning = false;
            stateChangeTime = currentMillis; 
        }
    } 
    
    if (pumpRunning && ((currentMillis - pumpStartTime) / 60000 >= pumpMaxRunTime)) {
        digitalWrite(pumpPin, LOW); pumpRunning = false; pumpSoftAlarm = true;
        stateChangeTime = currentMillis; 
        sendDiscord("⚠️ [超時] 水泵運轉過久鎖定");
    }

    int status = 0;
    if (pumpRunning)   status |= 1;
    if (fertRunning)   status |= 2;
    if (pumpSoftAlarm) status |= 4;
    if (sensorError)   status |= 8;
    if (autoMode)      status |= 16;
    if (pumpOverload)  status |= 32;
    if (fertOverload)  status |= 64;
    if (lastFbPumpError) status |= 128; 
    if (lastFbFertError) status |= 256; 

    // --- MQTT 發送數據 (包含新要素) ---
    if (currentMillis - lastMqttTime >= mqttInterval) {
        lastMqttTime = currentMillis;
        if (client.connected()) {
            // [修改] JSON 格式加入 soil_temp, ec, salinity
            String json = "{\"temp\":" + String(airTemp, 1) + 
                          ",\"hum\":" + String(airHum, 1) + 
                          ",\"soil_hum\":" + String(soil_hum, 1) + 
                          ",\"soil_temp\":" + String(soil_temp, 1) + 
                          ",\"ec\":" + String(soil_ec) + 
                          ",\"salinity\":" + String(soil_salinity) + 
                          ",\"status\":" + String(status) + "}";
            client.publish(topic_data, json.c_str());
        }
    }

    // --- ThingSpeak 上傳 (欄位需自行對應) ---
    if (currentMillis - lastUploadTime >= uploadInterval) {
      lastUploadTime = currentMillis;
      // 注意：ThingSpeak 欄位有限，這裡示範將新數據填入 Field 5, 6
      String url = "http://api.thingspeak.com/update?api_key=" + writeApiKey + 
                   "&field1=" + String(airTemp) + "&field2=" + String(airHum) + 
                   "&field3=" + String(soil_hum) + "&field4=" + String(status) +
                   "&field5=" + String(soil_ec) + "&field6=" + String(soil_temp); 
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
