#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <time.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ==========================================
//  ESP32 智慧農場 (最終完美版)
//  功能：自動/手動切換 + 施肥排程 + 安全防護
// ==========================================

const char* ssid = "EVDS";
const char* password = "EVDS0501";

String writeApiKey = "EEOBAMUB5SO5P42G"; 
String talkBackID = "55962";
String talkBackKey = "T8LJBZEQZCQURIGI";

// --- 硬體腳位 ---
const int pumpPin = 2;    // 水泵 (LED1)
const int fertPin = 5;    // 施肥 (LED2)
const int soilPin = 34;   // 土壤感測器
#define DHTPIN 4
#define DHTTYPE DHT11

// --- 自動化參數 ---
const int soilLow = 20;       // 土壤低於 20% 開水泵
const int soilHigh = 80;      // 土壤高於 80% 關水泵
const int fertHour = 8;       // 施肥時間：早上 8 點
const int fertDuration = 10;  // 施肥持續時間：10 分鐘
const int pumpMaxRunTime = 10;// 水泵最長運轉：10 分鐘

// --- 網路校時 (NTP) ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 28800; // GMT+8
const int   daylightOffset_sec = 0;

// --- 土壤校準 (5V供電建議值) ---
const int airValue = 4095;
const int waterValue = 1500;

DHT dht(DHTPIN, DHTTYPE);

// --- 系統狀態變數 ---
bool autoMode = true;   // 預設自動模式
bool pumpAlarm = false; // 水泵超時鎖定
unsigned long pumpStartTime = 0;
bool pumpRunning = false;
unsigned long fertStartTime = 0;
bool fertRunning = false;
bool fertJobDoneToday = false;

unsigned long lastUploadTime = 0;
const long uploadInterval = 20000; // 20秒上傳一次

void setup() {
  // 1. 關閉 Brownout 偵測 (防止電流大時重啟)
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(1000);

  pinMode(pumpPin, OUTPUT);
  pinMode(fertPin, OUTPUT);
  pinMode(soilPin, INPUT);
  dht.begin();

  Serial.print("連線 WiFi ");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi 已連接");
  
  // 2. 啟動網路校時
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
}

void loop() {
  if(WiFi.status() == WL_CONNECTED){
    unsigned long currentMillis = millis();
    struct tm timeinfo;
    bool timeSynced = getLocalTime(&timeinfo);

    // --- A. 讀取感測器 & 健檢 ---
    float hum = dht.readHumidity();
    float temp = dht.readTemperature();
    int soilRaw = analogRead(soilPin);
    int soilPercent = map(soilRaw, airValue, waterValue, 0, 100);
    soilPercent = constrain(soilPercent, 0, 100);
    
    // 異常判斷：DHT 讀失敗 或 土壤讀到 0 (斷線/短路)
    bool sensorError = (isnan(hum) || isnan(temp) || soilRaw == 0);

    // --- B. 自動化邏輯 (僅在 autoMode=true 執行) ---
    if (autoMode) {
        // [施肥邏輯] - 優先權最高
        if (timeSynced) {
          // 時間到 -> 啟動
          if (timeinfo.tm_hour == fertHour && timeinfo.tm_min == 0 && !fertRunning && !fertJobDoneToday) {
            if (pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; } // 互斥：關水泵
            digitalWrite(fertPin, HIGH);
            fertRunning = true;
            fertStartTime = currentMillis;
            fertJobDoneToday = true;
            Serial.println("自動化: 啟動定時施肥");
          }
          // 時間滿 -> 停止
          if (fertRunning && (currentMillis - fertStartTime) >= (fertDuration * 60 * 1000)) {
               digitalWrite(fertPin, LOW);
               fertRunning = false;
               Serial.println("自動化: 施肥結束");
          }
          // 隔天重置任務
          if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) fertJobDoneToday = false;
        }

        // [澆水邏輯] - 僅在沒警報、沒施肥、感測器正常時執行
        if (!pumpAlarm && !sensorError) {
          if (soilPercent < soilLow && !pumpRunning && !fertRunning) {
            digitalWrite(pumpPin, HIGH);
            pumpRunning = true;
            pumpStartTime = currentMillis;
            Serial.println("自動化: 土壤太乾，啟動水泵");
          }
          else if (soilPercent > soilHigh && pumpRunning) {
            digitalWrite(pumpPin, LOW);
            pumpRunning = false;
            Serial.println("自動化: 濕度足夠，關閉水泵");
          }
        } else if (sensorError && pumpRunning) {
            digitalWrite(pumpPin, LOW); // 感測器壞掉強制停機
            pumpRunning = false;
        }
    } 

    // --- C. 安全警報 (全域監控) ---
    if (pumpRunning && ((currentMillis - pumpStartTime) / 60000 >= pumpMaxRunTime)) {
        digitalWrite(pumpPin, LOW);
        pumpRunning = false;
        pumpAlarm = true; // 鎖定
        Serial.println("警報: 運轉超時，強制關機鎖定");
    }

    // --- D. TalkBack 指令檢查 (每2秒) ---
    static unsigned long lastTbCheck = 0;
    if (currentMillis - lastTbCheck > 2000) { 
        lastTbCheck = currentMillis;
        String tbUrl = "http://api.thingspeak.com/talkbacks/" + talkBackID + "/commands/execute?api_key=" + talkBackKey;
        HTTPClient httpTb;
        httpTb.begin(tbUrl.c_str());
        if (httpTb.GET() == 200) {
            String cmd = httpTb.getString();
            cmd.trim();
            if (cmd.length() > 0) {
                Serial.println("收到指令: [" + cmd + "]");
                
                // 模式切換
                if (cmd == "AUTO_ON") { autoMode = true; Serial.println("-> 切換: 自動"); }
                else if (cmd == "AUTO_OFF") { autoMode = false; Serial.println("-> 切換: 手動"); }
                // 手動控制 (會暫時覆蓋狀態，直到自動邏輯再次介入或保持手動)
                else if (cmd == "LED1_ON") { digitalWrite(pumpPin, HIGH); pumpRunning = true; pumpStartTime = millis(); }
                else if (cmd == "LED1_OFF") { digitalWrite(pumpPin, LOW); pumpRunning = false; }
                else if (cmd == "LED2_ON") { digitalWrite(fertPin, HIGH); fertRunning = true; fertStartTime = millis(); }
                else if (cmd == "LED2_OFF") { digitalWrite(fertPin, LOW); fertRunning = false; }
            }
        }
        httpTb.end();
    }

    // --- E. 數據上傳 (每20秒) ---
    if (currentMillis - lastUploadTime >= uploadInterval) {
      lastUploadTime = currentMillis;

      // 狀態碼編碼 (Bitmask)
      // 1:水泵, 2:施肥, 4:警報, 8:異常, 16:自動模式
      int status = 0;
      if (pumpRunning) status |= 1;
      if (fertRunning) status |= 2;
      if (pumpAlarm)   status |= 4;
      if (sensorError) status |= 8;
      if (autoMode)    status |= 16; 

      String url = "http://api.thingspeak.com/update?api_key=" + writeApiKey + 
                   "&field1=" + String(temp) + 
                   "&field2=" + String(hum) + 
                   "&field3=" + String(soilPercent) +
                   "&field4=" + String(status); 
      
      HTTPClient http;
      http.begin(url.c_str());
      http.GET();
      http.end();
      Serial.println("數據上傳完成, Status=" + String(status));
    }
  }
  delay(100);
}
