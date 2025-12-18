#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h> // 引入安全連線庫 (HTTPS)
#include <DHT.h>
#include <time.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ==========================================
//  ESP32 智慧農場 (Discord 通知版) v8.2
//  功能：積熱電驛監控 + 互斥鎖 + Discord 全狀態通知
// ==========================================

const char* ssid = "EVDS";
const char* password = "EVDS0501";

//const char* ssid = "iPhone";
//const char* password = "11111111";

String writeApiKey = "EEOBAMUB5SO5P42G"; 
String talkBackID = "55962";
String talkBackKey = "T8LJBZEQZCQURIGI";

// [設定] 請在此填入您的 Discord Webhook 網址
const char* discord_webhook = "https://discord.com/api/webhooks/1450540646417961113/PBjCQtc-VlIA7cSeggc2R4ZnCcmDlqUtw31UtR9QM40e-HX1pLQD-2HjVoJPLbDLQsp7";
const char* discord_webhook_2 = "https://discord.com/api/webhooks/1451100483338108989/xUJ9AdGTDRGTWvwPzPL8Qt8PPCGyar4XkBGNZ9Px39xBxNA2R39VCY--FJiuE322QmAA";

// --- 硬體腳位 ---
const int pumpPin = 2;    
const int fertPin = 5;    
const int soilPin = 34;   
const int olPumpPin = 18; 
const int olFertPin = 19; 

#define DHTPIN 4
#define DHTTYPE DHT11

// --- 參數 ---
const int soilLow = 20;
const int soilHigh = 80;
const int fertHour = 8;
const int fertDuration = 10;
const int pumpMaxRunTime = 10;

// --- NTP ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 28800; 
const int   daylightOffset_sec = 0;

const int airValue = 4095;
const int waterValue = 1500;

DHT dht(DHTPIN, DHTTYPE);

// --- 狀態變數 ---
bool autoMode = true;   
bool pumpSoftAlarm = false; 
unsigned long pumpStartTime = 0;
bool pumpRunning = false;
unsigned long fertStartTime = 0;
bool fertRunning = false;
bool fertJobDoneToday = false;

unsigned long lastUploadTime = 0;
const long uploadInterval = 60000; 
unsigned long lastTbCheck = 0;
const long tbInterval = 5000;

// --- [核心修正] 發送 Discord 通知 ---
void sendDiscordAlert(String msg) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure(); // 略過 SSL 驗證，這對 ESP32 連 Discord 很重要
    
    HTTPClient https;
    // 建立連線
    if (https.begin(client, discord_webhook)) {
      https.addHeader("Content-Type", "application/json");
      
      // [修正] JSON 特殊字元轉義
      // 如果訊息裡有 \n (換行) 或 " (雙引號)，必須轉成 \\n 和 \"，否則 Discord 會拒收 (HTTP 400)
      String escapedMsg = "";
      for (int i = 0; i < msg.length(); i++) {
        char c = msg[i];
        if (c == '\n') escapedMsg += "\\n";
        else if (c == '"') escapedMsg += "\\\"";
        else escapedMsg += c;
      }

      // 組合 JSON
      String jsonPayload = "{\"content\":\"" + escapedMsg + "\"}";
      
      // 發送請求
      int httpCode = https.POST(jsonPayload);
      
      // [修正] 詳細的錯誤診斷
      // Discord 成功通常回傳 204 (No Content)
      if (httpCode >= 200 && httpCode < 300) {
        Serial.println("Discord 發送成功 (HTTP " + String(httpCode) + "): " + msg);
      } else {
        Serial.print("Discord 發送失敗! HTTP Code: ");
        Serial.println(httpCode);
        // 印出伺服器回傳的錯誤內容 (例如 JSON error)
        if(httpCode > 0) Serial.println("回應內容: " + https.getString());
      }
      https.end();
    } else {
      Serial.println("無法連接 Discord 伺服器 (DNS 或網路問題)");
    }
  } else {
    Serial.println("WiFi 未連線，無法發送通知");
  }
}

void sendDiscordAlert_2(String msg) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure client;
    client.setInsecure(); // 略過 SSL 驗證，這對 ESP32 連 Discord 很重要
    
    HTTPClient https;
    // 建立連線
    if (https.begin(client, discord_webhook_2)) {
      https.addHeader("Content-Type", "application/json");
      
      // [修正] JSON 特殊字元轉義
      // 如果訊息裡有 \n (換行) 或 " (雙引號)，必須轉成 \\n 和 \"，否則 Discord 會拒收 (HTTP 400)
      String escapedMsg = "";
      for (int i = 0; i < msg.length(); i++) {
        char c = msg[i];
        if (c == '\n') escapedMsg += "\\n";
        else if (c == '"') escapedMsg += "\\\"";
        else escapedMsg += c;
      }

      // 組合 JSON
      String jsonPayload = "{\"content\":\"" + escapedMsg + "\"}";
      
      // 發送請求
      int httpCode = https.POST(jsonPayload);
      
      // [修正] 詳細的錯誤診斷
      // Discord 成功通常回傳 204 (No Content)
      if (httpCode >= 200 && httpCode < 300) {
        Serial.println("Discord 發送成功 (HTTP " + String(httpCode) + "): " + msg);
      } else {
        Serial.print("Discord 發送失敗! HTTP Code: ");
        Serial.println(httpCode);
        // 印出伺服器回傳的錯誤內容 (例如 JSON error)
        if(httpCode > 0) Serial.println("回應內容: " + https.getString());
      }
      https.end();
    } else {
      Serial.println("無法連接 Discord 伺服器 (DNS 或網路問題)");
    }
  } else {
    Serial.println("WiFi 未連線，無法發送通知");
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  
  pinMode(pumpPin, OUTPUT);
  pinMode(fertPin, OUTPUT);
  digitalWrite(pumpPin, LOW); 
  digitalWrite(fertPin, LOW); 

  pinMode(soilPin, INPUT);
  pinMode(olPumpPin, INPUT_PULLUP);
  pinMode(olFertPin, INPUT_PULLUP);

  dht.begin();

  Serial.print("連線 WiFi ");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi 已連接");
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // 開機通知
  sendDiscordAlert("✅ **系統上線通知**\nESP32 智慧農場 v8.1 已啟動！(JSON格式修正版)");
  sendDiscordAlert_2("✅ **系統上線通知**\nESP32 智慧農場 v8.1 已啟動！(JSON格式修正版)");
  }

void loop() {
  if(WiFi.status() == WL_CONNECTED){
    unsigned long currentMillis = millis();
    struct tm timeinfo;
    bool timeSynced = getLocalTime(&timeinfo);

    float hum = dht.readHumidity();
    float temp = dht.readTemperature();
    int soilRaw = analogRead(soilPin);
    int soilPercent = map(soilRaw, airValue, waterValue, 0, 100);
    soilPercent = constrain(soilPercent, 0, 100);
    
    bool sensorError = (isnan(hum) || isnan(temp) || soilRaw == 0);

    // --- 過載偵測邏輯 ---
    bool pumpOverload = (digitalRead(olPumpPin) == LOW);
    bool fertOverload = (digitalRead(olFertPin) == LOW);

    if (pumpOverload && pumpRunning) {
        digitalWrite(pumpPin, LOW);
        pumpRunning = false;
        String msg = "⚠️ **嚴重警報：水泵跳脫！**\n偵測到積熱電驛動作 (Pump Trip)，已緊急停機。";
        Serial.println(msg);
        sendDiscordAlert(msg);
        sendDiscordAlert_2(msg);
    }
    if (fertOverload && fertRunning) {
        digitalWrite(fertPin, LOW);
        fertRunning = false;
        String msg = "⚠️ **嚴重警報：施肥機跳脫！**\n偵測到積熱電驛動作 (Fert Trip)，已緊急停機。";
        Serial.println(msg);
        sendDiscordAlert(msg);
        sendDiscordAlert_2(msg);
    }

    // --- 自動化邏輯 ---
    if (autoMode) {
        // [施肥]
        if (timeSynced && !fertOverload) { 
          if (timeinfo.tm_hour == fertHour && timeinfo.tm_min == 0 && !fertRunning && !fertJobDoneToday) {
            if (pumpRunning) { 
                digitalWrite(pumpPin, LOW); pumpRunning = false; 
                sendDiscordAlert("🛑 **狀態更新**\n水泵已暫停 (為執行施肥排程)");
                sendDiscordAlert_2("🛑 **狀態更新**\n水泵已暫停 (為執行施肥排程)");
            } 
            digitalWrite(fertPin, HIGH);
            fertRunning = true;
            fertStartTime = currentMillis;
            fertJobDoneToday = true;
            sendDiscordAlert("🧪 **施肥啟動**\n定時施肥任務開始 (預計 10 分鐘)");
            sendDiscordAlert_2("🧪 **施肥啟動**\n定時施肥任務開始 (預計 10 分鐘)");
          }
          if (fertRunning && (currentMillis - fertStartTime) >= (fertDuration * 60 * 1000)) {
               digitalWrite(fertPin, LOW);
               fertRunning = false;
               sendDiscordAlert("✅ **施肥完成**\n定時任務結束，施肥機已關閉。");
               sendDiscordAlert_2("✅ **施肥完成**\n定時任務結束，施肥機已關閉。");
          }
          if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) fertJobDoneToday = false;
        }

        // [澆水]
        if (!pumpSoftAlarm && !pumpOverload && !sensorError) { 
          if (soilPercent < soilLow && !pumpRunning && !fertRunning) {
            digitalWrite(pumpPin, HIGH);
            pumpRunning = true;
            pumpStartTime = currentMillis;
            String msg = "💧 **補水啟動**\n偵測到土壤乾燥 (" + String(soilPercent) + "%)，水泵已開啟。";
            sendDiscordAlert(msg);
            sendDiscordAlert_2(msg);
          }
          else if (soilPercent > soilHigh && pumpRunning) {
            digitalWrite(pumpPin, LOW);
            pumpRunning = false;
            String msg = "🛑 **補水完成**\n土壤濕度達標 (" + String(soilPercent) + "%)，水泵已關閉。";
            sendDiscordAlert(msg);
            sendDiscordAlert_2(msg);
          }
        } else if ((sensorError || pumpOverload) && pumpRunning) {
            digitalWrite(pumpPin, LOW);
            pumpRunning = false;
            sendDiscordAlert("⚠️ **異常停機**\n因感測器故障或過載，水泵已強制關閉。");
            sendDiscordAlert_2("⚠️ **異常停機**\n因感測器故障或過載，水泵已強制關閉。");
        }
    } 

    // --- 軟體安全警報 (運轉超時) ---
    if (pumpRunning && ((currentMillis - pumpStartTime) / 60000 >= pumpMaxRunTime)) {
        digitalWrite(pumpPin, LOW);
        pumpRunning = false;
        pumpSoftAlarm = true;
        sendDiscordAlert("⏳ **超時警告**\n水泵運轉超過 10 分鐘，系統已強制鎖定保護！");
        sendDiscordAlert_2("⏳ **超時警告**\n水泵運轉超過 10 分鐘，系統已強制鎖定保護！");
    }

    // --- TalkBack 手動控制 ---
    if (currentMillis - lastTbCheck > tbInterval) { 
        lastTbCheck = currentMillis;
        String tbUrl = "http://api.thingspeak.com/talkbacks/" + talkBackID + "/commands/execute?api_key=" + talkBackKey;
        HTTPClient httpTb;
        httpTb.begin(tbUrl.c_str());
        if (httpTb.GET() == 200) {
            String cmd = httpTb.getString();
            cmd.trim();
            if (cmd.length() > 0) {
                Serial.println("收到指令: [" + cmd + "]");
                
                if (cmd == "STOP") {
                    autoMode = false;
                    if(pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; }
                    if(fertRunning) { digitalWrite(fertPin, LOW); fertRunning = false; }
                    sendDiscordAlert("🚨 **緊急停止**\n收到遠端 STOP 指令，所有設備已停機並切換為手動模式。");
                    sendDiscordAlert_2("🚨 **緊急停止**\n收到遠端 STOP 指令，所有設備已停機並切換為手動模式。");
                }
                else if (cmd == "AUTO_ON") {
                    autoMode = true;
                    sendDiscordAlert("🔄 **模式切換**\n系統已切換為：自動模式 (Auto)");
                    sendDiscordAlert_2("🔄 **模式切換**\n系統已切換為：自動模式 (Auto)");
                }
                else if (cmd == "AUTO_OFF") {
                    autoMode = false;
                    sendDiscordAlert("🖐️ **模式切換**\n系統已切換為：手動模式 (Manual)");
                    sendDiscordAlert_2("🖐️ **模式切換**\n系統已切換為：手動模式 (Manual)");
                }
                
                // 手動控制 (已加入通知)
                else if (cmd == "LED1_ON") { 
                    if (!pumpOverload) { 
                        if(fertRunning) { 
                            digitalWrite(fertPin, LOW); fertRunning = false; 
                            sendDiscordAlert("🛑 施肥機已關閉 (互斥保護)");
                            sendDiscordAlert_2("🛑 施肥機已關閉 (互斥保護)");
                        }
                        digitalWrite(pumpPin, HIGH); pumpRunning = true; pumpStartTime = millis(); 
                        sendDiscordAlert("📱 **遠端操作**\n水泵已手動開啟 (Pump ON)");
                        sendDiscordAlert_2("📱 **遠端操作**\n水泵已手動開啟 (Pump ON)");
                    } else { Serial.println("拒絕：水泵過載"); }
                }
                else if (cmd == "LED1_OFF") { 
                    digitalWrite(pumpPin, LOW); pumpRunning = false; 
                    sendDiscordAlert("📱 **遠端操作**\n水泵已手動關閉 (Pump OFF)");
                    sendDiscordAlert_2("📱 **遠端操作**\n水泵已手動關閉 (Pump OFF)");
                }
                else if (cmd == "LED2_ON") { 
                    if (!fertOverload) { 
                        if(pumpRunning) { 
                            digitalWrite(pumpPin, LOW); pumpRunning = false; 
                            sendDiscordAlert("🛑 水泵已關閉 (互斥保護)");
                            sendDiscordAlert_2("🛑 水泵已關閉 (互斥保護)");
                        }
                        digitalWrite(fertPin, HIGH); fertRunning = true; fertStartTime = millis(); 
                        sendDiscordAlert("📱 **遠端操作**\n施肥機已手動開啟 (Fert ON)");
                        sendDiscordAlert_2("📱 **遠端操作**\n施肥機已手動開啟 (Fert ON)");
                    } else { Serial.println("拒絕：施肥過載"); }
                }
                else if (cmd == "LED2_OFF") { 
                    digitalWrite(fertPin, LOW); fertRunning = false; 
                    sendDiscordAlert("📱 **遠端操作**\n施肥機已手動關閉 (Fert OFF)");
                    sendDiscordAlert_2("📱 **遠端操作**\n施肥機已手動關閉 (Fert OFF)");
                }
            }
        }
        httpTb.end();
    }

    // --- 數據上傳 ---
    if (currentMillis - lastUploadTime >= uploadInterval) {
      lastUploadTime = currentMillis;
      
      int status = 0;
      if (pumpRunning)   status |= 1;
      if (fertRunning)   status |= 2;
      if (pumpSoftAlarm) status |= 4;
      if (sensorError)   status |= 8;
      if (autoMode)      status |= 16;
      if (pumpOverload)  status |= 32; 
      if (fertOverload)  status |= 64; 

      String url = "http://api.thingspeak.com/update?api_key=" + writeApiKey + 
                   "&field1=" + String(temp) + 
                   "&field2=" + String(hum) + 
                   "&field3=" + String(soilPercent) +
                   "&field4=" + String(status); 
      
      HTTPClient http;
      http.begin(url.c_str());
      http.GET();
      http.end();
      Serial.println("數據上傳完成");
    }
  } else {
      Serial.println("WiFi 斷線，嘗試重連...");
      WiFi.disconnect();
      WiFi.reconnect();
      delay(1000);
  }
  delay(100);
}
