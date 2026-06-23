#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <time.h> // 引入時間函式庫

// ==========================================
//  ESP32 智慧農場 (全自動化版)
//  功能：自動澆水、定時施肥、超時警報
// ==========================================

const char* ssid = "EVDS";
const char* password = "EVDS0501";

String writeApiKey = "EEOBAMUB5SO5P42G"; 
String talkBackID = "55962";
String talkBackKey = "T8LJBZEQZCQURIGI";

// --- 硬體腳位 ---
const int pumpPin = 2;    // 水泵
const int fertPin = 5;    // 施肥
const int soilPin = 34;   // 土壤
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

// 校準
const int airValue = 4095;
const int waterValue = 1500;

DHT dht(DHTPIN, DHTTYPE);

// --- 狀態變數 ---
unsigned long pumpStartTime = 0;
bool pumpRunning = false;
bool pumpAlarm = false;

unsigned long fertStartTime = 0;
bool fertRunning = false;
bool fertJobDoneToday = false;

unsigned long lastUploadTime = 0;
const long uploadInterval = 20000; 

void setup() {
  Serial.begin(115200);
  pinMode(pumpPin, OUTPUT);
  pinMode(fertPin, OUTPUT);
  pinMode(soilPin, INPUT);
  dht.begin();

  WiFi.begin(ssid, password);
  Serial.print("連線 WiFi ");
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

    // 1. 讀取感測器 & 異常偵測
    float hum = dht.readHumidity();
    float temp = dht.readTemperature();
    int soilRaw = analogRead(soilPin);
    
    bool sensorError = false;

    // 檢查 DHT 是否 NaN (Not a Number)
    if (isnan(hum) || isnan(temp)) {
      Serial.println("[警告] DHT11 讀取失敗！");
      sensorError = true;
      // 錯誤時給預設值避免計算錯誤，但標記 Error
      hum = 0; 
      temp = 0;
    }

    // 檢查土壤感測器是否斷線 (通常斷線會是極端值，如 0 或 4095)
    // 這裡假設如果讀到完全的 0 (短路) 視為異常
    if (soilRaw == 0) {
       Serial.println("[警告] 土壤感測器異常 (數值 0)");
       sensorError = true;
    }

    int soilPercent = map(soilRaw, airValue, waterValue, 0, 100);
    soilPercent = constrain(soilPercent, 0, 100);

    // ==========================================
    //  邏輯 C: 定時施肥 (優先)
    // ==========================================
    if (timeSynced) {
      if (timeinfo.tm_hour == fertHour && timeinfo.tm_min == 0 && !fertRunning && !fertJobDoneToday) {
        if (pumpRunning) {
            digitalWrite(pumpPin, LOW);
            pumpRunning = false;
        }
        digitalWrite(fertPin, HIGH);
        fertRunning = true;
        fertStartTime = currentMillis;
        fertJobDoneToday = true;
      }
      if (fertRunning && (currentMillis - fertStartTime) >= (fertDuration * 60 * 1000)) {
           digitalWrite(fertPin, LOW);
           fertRunning = false;
      }
      if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) {
        fertJobDoneToday = false;
      }
    }

    // ==========================================
    //  邏輯 A: 自動澆水 (若感測器壞掉，禁止自動澆水)
    // ==========================================
    if (!pumpAlarm && !sensorError) { // 加入 !sensorError 保護
      if (soilPercent < soilLow && !pumpRunning && !fertRunning) {
        digitalWrite(pumpPin, HIGH);
        pumpRunning = true;
        pumpStartTime = currentMillis;
      }
      else if (soilPercent > soilHigh && pumpRunning) {
        digitalWrite(pumpPin, LOW);
        pumpRunning = false;
      }
    } else if (sensorError && pumpRunning) {
        // 如果運轉中發現感測器壞了，為了安全，強制停機
        Serial.println("感測器異常，強制停止水泵！");
        digitalWrite(pumpPin, LOW);
        pumpRunning = false;
    }

    // ==========================================
    //  邏輯 B: 水泵安全警報
    // ==========================================
    if (pumpRunning) {
      if ((currentMillis - pumpStartTime) / 60000 >= pumpMaxRunTime) {
        digitalWrite(pumpPin, LOW);
        pumpRunning = false;
        pumpAlarm = true;
      }
    }

    // ==========================================
    //  上傳數據 (位元組合狀態碼)
    // ==========================================
    if (currentMillis - lastUploadTime >= uploadInterval) {
      lastUploadTime = currentMillis;

      // 組合狀態碼 (Bitmask)
      // Bit 0 (1): 水泵開
      // Bit 1 (2): 施肥開
      // Bit 2 (4): 警報鎖定
      // Bit 3 (8): 感測器異常
      int status = 0;
      if (pumpRunning) status |= 1;  // 等同 status = status + 1
      if (fertRunning) status |= 2;  // 等同 status = status + 2
      if (pumpAlarm)   status |= 4;  // 等同 status = status + 4
      if (sensorError) status |= 8;  // 等同 status = status + 8

      String url = "http://api.thingspeak.com/update?api_key=" + writeApiKey + 
                   "&field1=" + String(temp) + 
                   "&field2=" + String(hum) + 
                   "&field3=" + String(soilPercent) +
                   "&field4=" + String(status); 
      
      HTTPClient http;
      http.begin(url.c_str());
      int httpCode = http.GET();
      http.end();
      Serial.printf("上傳數據... Status Code: %d (Binary: %s)\n", status, String(status, BIN).c_str());
    }
  }

  delay(1000);
}
