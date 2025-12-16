#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <time.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ==========================================
//  ESP32 智慧農場 (過載保護旗艦版) v7.0
//  功能：積熱電驛監控 + 互斥鎖 + 1分鐘上傳
// ==========================================

const char* ssid = "EVDS";
const char* password = "EVDS0501";

String writeApiKey = "EEOBAMUB5SO5P42G"; 
String talkBackID = "55962";
String talkBackKey = "T8LJBZEQZCQURIGI";

// --- 硬體腳位 ---
const int pumpPin = 2;    // 水泵輸出
const int fertPin = 5;    // 施肥輸出
const int soilPin = 34;   // 土壤輸入
// [新增] 過載偵測腳位 (接積熱電驛 NO 97-98)
const int olPumpPin = 18; // 水泵過載訊號
const int olFertPin = 19; // 施肥過載訊號

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

// --- 校準 ---
const int airValue = 4095;
const int waterValue = 1500;

DHT dht(DHTPIN, DHTTYPE);

// --- 狀態變數 ---
bool autoMode = true;   
bool pumpSoftAlarm = false; // 軟體超時鎖定
unsigned long pumpStartTime = 0;
bool pumpRunning = false;
unsigned long fertStartTime = 0;
bool fertRunning = false;
bool fertJobDoneToday = false;

// 計時器
unsigned long lastUploadTime = 0;
const long uploadInterval = 60000; // 1分鐘上傳
unsigned long lastTbCheck = 0;
const long tbInterval = 2000;      // 2秒檢查指令

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  
  // 輸出腳位初始化 (預設關閉)
  pinMode(pumpPin, OUTPUT);
  pinMode(fertPin, OUTPUT);
  digitalWrite(pumpPin, LOW); 
  digitalWrite(fertPin, LOW); 

  // 輸入腳位初始化
  pinMode(soilPin, INPUT);
  // [新增] 設定過載腳位為輸入上拉 (平常 HIGH, 跳脫時接地變 LOW)
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

    // --- [新增] 過載偵測邏輯 ---
    // LOW 代表積熱電驛跳脫 (導通接地)
    bool pumpOverload = (digitalRead(olPumpPin) == LOW);
    bool fertOverload = (digitalRead(olFertPin) == LOW);

    if (pumpOverload && pumpRunning) {
        digitalWrite(pumpPin, LOW);
        pumpRunning = false;
        Serial.println("!!! 嚴重警報：水泵過載跳脫 (Pump Trip) !!!");
    }
    if (fertOverload && fertRunning) {
        digitalWrite(fertPin, LOW);
        fertRunning = false;
        Serial.println("!!! 嚴重警報：施肥機過載跳脫 (Fert Trip) !!!");
    }

    // --- 自動化邏輯 ---
    // 只有在 自動模式 + 無軟體警報 + 無硬體過載 + 感測器正常 時才執行
    if (autoMode) {
        // [施肥]
        if (timeSynced && !fertOverload) { // 沒過載才準開
          if (timeinfo.tm_hour == fertHour && timeinfo.tm_min == 0 && !fertRunning && !fertJobDoneToday) {
            if (pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; } // 互斥
            digitalWrite(fertPin, HIGH);
            fertRunning = true;
            fertStartTime = currentMillis;
            fertJobDoneToday = true;
          }
          if (fertRunning && (currentMillis - fertStartTime) >= (fertDuration * 60 * 1000)) {
               digitalWrite(fertPin, LOW);
               fertRunning = false;
          }
          if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) fertJobDoneToday = false;
        }

        // [澆水]
        if (!pumpSoftAlarm && !pumpOverload && !sensorError) { // 沒過載才準開
          if (soilPercent < soilLow && !pumpRunning && !fertRunning) {
            digitalWrite(pumpPin, HIGH);
            pumpRunning = true;
            pumpStartTime = currentMillis;
          }
          else if (soilPercent > soilHigh && pumpRunning) {
            digitalWrite(pumpPin, LOW);
            pumpRunning = false;
          }
        } else if ((sensorError || pumpOverload) && pumpRunning) {
            digitalWrite(pumpPin, LOW);
            pumpRunning = false;
        }
    } 

    // --- 軟體安全警報 (運轉超時) ---
    if (pumpRunning && ((currentMillis - pumpStartTime) / 60000 >= pumpMaxRunTime)) {
        digitalWrite(pumpPin, LOW);
        pumpRunning = false;
        pumpSoftAlarm = true;
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
                    digitalWrite(pumpPin, LOW); pumpRunning = false;
                    digitalWrite(fertPin, LOW); fertRunning = false;
                }
                else if (cmd == "AUTO_ON") autoMode = true;
                else if (cmd == "AUTO_OFF") autoMode = false;
                
                // 手動控制 (互斥鎖 + 過載保護)
                else if (cmd == "LED1_ON") { 
                    if (!pumpOverload) { // 沒過載才準開
                        if(fertRunning) { digitalWrite(fertPin, LOW); fertRunning = false; }
                        digitalWrite(pumpPin, HIGH); pumpRunning = true; pumpStartTime = millis(); 
                    } else { Serial.println("拒絕開啟：水泵過載中"); }
                }
                else if (cmd == "LED1_OFF") { digitalWrite(pumpPin, LOW); pumpRunning = false; }
                else if (cmd == "LED2_ON") { 
                    if (!fertOverload) { // 沒過載才準開
                        if(pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; }
                        digitalWrite(fertPin, HIGH); fertRunning = true; fertStartTime = millis(); 
                    } else { Serial.println("拒絕開啟：施肥過載中"); }
                }
                else if (cmd == "LED2_OFF") { digitalWrite(fertPin, LOW); fertRunning = false; }
            }
        }
        httpTb.end();
    }

    // --- 數據上傳 ---
    if (currentMillis - lastUploadTime >= uploadInterval) {
      lastUploadTime = currentMillis;
      
      // 狀態碼編碼 (Bitmask)
      // 1:水泵, 2:施肥, 4:軟體警報, 8:感測異常, 16:自動模式
      // 32:水泵過載, 64:施肥過載
      int status = 0;
      if (pumpRunning)   status |= 1;
      if (fertRunning)   status |= 2;
      if (pumpSoftAlarm) status |= 4;
      if (sensorError)   status |= 8;
      if (autoMode)      status |= 16;
      if (pumpOverload)  status |= 32; // Bit 5
      if (fertOverload)  status |= 64; // Bit 6

      String url = "http://api.thingspeak.com/update?api_key=" + writeApiKey + 
                   "&field1=" + String(temp) + 
                   "&field2=" + String(hum) + 
                   "&field3=" + String(soilPercent) +
                   "&field4=" + String(status); 
      
      HTTPClient http;
      http.begin(url.c_str());
      http.GET();
      http.end();
      Serial.println("數據上傳完成 (Status=" + String(status) + ")");
    }
  }
  delay(100);
}
