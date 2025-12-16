#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <time.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ==========================================
//  ESP32 智慧農場 (含緊急停機功能)
//  功能：強制單一運作 + 自動/手動 + 緊急停機
// ==========================================

const char* ssid = "oao";
const char* password = "0936698942";

String writeApiKey = "你的_WRITE_API_KEY"; 
String talkBackID = "你的_TALKBACK_ID";
String talkBackKey = "你的_TALKBACK_API_KEY";

const int pumpPin = 2;    // 水泵
const int fertPin = 5;    // 施肥
const int soilPin = 34;   // 土壤
#define DHTPIN 4
#define DHTTYPE DHT11

const int soilLow = 20;
const int soilHigh = 80;
const int fertHour = 8;
const int fertDuration = 10;
const int pumpMaxRunTime = 10;

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 28800; 
const int   daylightOffset_sec = 0;

const int airValue = 4095;
const int waterValue = 1500;

DHT dht(DHTPIN, DHTTYPE);

bool autoMode = true;   
bool pumpAlarm = false; 
unsigned long pumpStartTime = 0;
bool pumpRunning = false;
unsigned long fertStartTime = 0;
bool fertRunning = false;
bool fertJobDoneToday = false;

unsigned long lastUploadTime = 0;
const long uploadInterval = 20000; 

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  
  pinMode(pumpPin, OUTPUT);
  pinMode(fertPin, OUTPUT);
  digitalWrite(pumpPin, LOW); 
  digitalWrite(fertPin, LOW); 

  pinMode(soilPin, INPUT);
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

    // --- 自動化邏輯 ---
    if (autoMode) {
        // [施肥]
        if (timeSynced) {
          if (timeinfo.tm_hour == fertHour && timeinfo.tm_min == 0 && !fertRunning && !fertJobDoneToday) {
            if (pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; Serial.println("自動互斥：關閉水泵以進行施肥"); }
            digitalWrite(fertPin, HIGH);
            fertRunning = true;
            fertStartTime = currentMillis;
            fertJobDoneToday = true;
            Serial.println("自動化: 啟動定時施肥");
          }
          if (fertRunning && (currentMillis - fertStartTime) >= (fertDuration * 60 * 1000)) {
               digitalWrite(fertPin, LOW);
               fertRunning = false;
               Serial.println("自動化: 施肥結束");
          }
          if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) fertJobDoneToday = false;
        }

        // [澆水]
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
            digitalWrite(pumpPin, LOW);
            pumpRunning = false;
        }
    } 

    // --- 安全警報 ---
    if (pumpRunning && ((currentMillis - pumpStartTime) / 60000 >= pumpMaxRunTime)) {
        digitalWrite(pumpPin, LOW);
        pumpRunning = false;
        pumpAlarm = true;
        Serial.println("警報: 運轉超時");
    }

    // --- TalkBack 手動控制 (含緊急停機) ---
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
                
                // --- 緊急停機指令 (新增) ---
                if (cmd == "STOP") {
                    autoMode = false; // 切回手動防止自動重啟
                    digitalWrite(pumpPin, LOW); pumpRunning = false;
                    digitalWrite(fertPin, LOW); fertRunning = false;
                    Serial.println("!!! 緊急停機：關閉所有設備與自動化 !!!");
                }
                // --- 其他指令 ---
                else if (cmd == "AUTO_ON") { autoMode = true; Serial.println("-> 切換: 自動"); }
                else if (cmd == "AUTO_OFF") { autoMode = false; Serial.println("-> 切換: 手動"); }
                else if (cmd == "LED1_ON") { 
                    if(fertRunning) { digitalWrite(fertPin, LOW); fertRunning = false; Serial.println("手動互斥：關閉施肥"); }
                    digitalWrite(pumpPin, HIGH); pumpRunning = true; pumpStartTime = millis(); 
                }
                else if (cmd == "LED1_OFF") { digitalWrite(pumpPin, LOW); pumpRunning = false; }
                else if (cmd == "LED2_ON") { 
                    if(pumpRunning) { digitalWrite(pumpPin, LOW); pumpRunning = false; Serial.println("手動互斥：關閉水泵"); }
                    digitalWrite(fertPin, HIGH); fertRunning = true; fertStartTime = millis(); 
                }
                else if (cmd == "LED2_OFF") { digitalWrite(fertPin, LOW); fertRunning = false; }
            }
        }
        httpTb.end();
    }

    // --- 上傳數據 ---
    if (currentMillis - lastUploadTime >= uploadInterval) {
      lastUploadTime = currentMillis;
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
    }
  }
  delay(100);
}
