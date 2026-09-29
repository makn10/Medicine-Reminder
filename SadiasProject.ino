#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <WiFiManager.h>          // https://github.com/tzapu/WiFiManager  (install "WiFiManager" by tzapu via Library Manager)
#include "AudioFileSourceHTTPStream.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// ==========================================
// CONFIGURATIONS (AP Portal & Pins)
// ==========================================
const char* apSSID = "MySmartDevice-Setup";   // Hotspot name shown when device needs Wi-Fi setup

// I2C Pins (LCD & RTC)
#define I2C_SDA         21
#define I2C_SCL         22

// I2S Audio Pins (MAX98357A)
#define I2S_BCLK        26
#define I2S_LRC         4
#define I2S_DOUT        17

// UI Pins
#define LED_PIN         16
#define BUZZER_PIN      2
#define BUTTON_PIN      15

// ==========================================
// STRUCTURE & CONFIG STORAGE (EEPROM)
// ==========================================
#define EEPROM_SIZE 1024

struct ScheduleConfig {
  int morningHour;
  int morningMinute;
  char morningBox[16];
  char morningMed[24];

  int noonHour;
  int noonMinute;
  char noonBox[16];
  char noonMed[24];

  int nightHour;
  int nightMinute;
  char nightBox[16];
  char nightMed[24];

  // Pushover Credentials
  char pushoverUserKey[40];
  char pushoverApiToken[40];
};

ScheduleConfig config;

// ==========================================
// OBJECTS & VARIABLES
// ==========================================
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS3231 rtc;
WebServer server(80);

WiFiClientSecure secured_client; 

AudioGeneratorMP3 *mp3 = NULL;
AudioFileSourceHTTPStream *file = NULL;
AudioOutputI2S *out = NULL;

// State Variables
bool alarmActive = false;
String currentMedicine = "";
int lastTriggeredMinute = -1;
int lastTriggeredSlot = -1; 

unsigned long previousMillis = 0;
unsigned long lastLcdUpdate = 0;
unsigned long alarmStartTime = 0;    
bool isSpeaking = false;             
bool blinkState = LOW;

String singleVoiceMsg = "";

// ==========================================
// FUNCTION PROTOTYPES
// ==========================================
void sendPushoverNotification(String message);
void loadDefaultConfig();
void saveConfigToEEPROM();
void loadConfigFromEEPROM();
void handleRoot();
void handleSave();
void handleSetTime();
void handleTestAlarm();
String urlEncode(String str);
void displayTime(DateTime now);
void checkSchedule(DateTime now);
void buildVoiceList(String box, String med);
void startAlarmSession(String box, String med, int currentMin);
void playCurrentVoice();
void stopAlarm();

// ==========================================
// MEDICINE DASHBOARD HTML
// ==========================================
const char MEDICINE_DASHBOARD_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Smart Medicine 3D Hub</title>
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&display=swap" rel="stylesheet">
  <style>
    :root {
      --bg-gradient: linear-gradient(135deg, #040814 0%, #0d162d 50%, #050b1a 100%);
      --card-bg: rgba(15, 23, 42, 0.85);
      --card-border: rgba(255, 255, 255, 0.12);
      --text-main: #f8fafc;
      --text-sub: #94a3b8;
      --accent-morning: #f59e0b;
      --accent-noon: #3b82f6;
      --accent-night: #8b5cf6;
      --accent-pushover: #00aced;
    }
    * { box-sizing: border-box; }
    body { font-family: 'Inter', sans-serif; background: var(--bg-gradient); color: var(--text-main); margin: 0; padding: 20px; display: flex; justify-content: center; }
    .container { width: 100%; max-width: 540px; background: var(--card-bg); border: 1px solid var(--card-border); border-radius: 24px; padding: 30px; box-shadow: 0 30px 60px rgba(0, 0, 0, 0.7); }
    .header-box { text-align: center; margin-bottom: 25px; }
    .header-box h2 { margin: 0 0 8px 0; font-size: 26px; background: linear-gradient(to right, #60a5fa, #a78bfa); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
    .devtime { background: rgba(16, 185, 129, 0.08); border: 1px solid rgba(16, 185, 129, 0.3); padding: 16px; border-radius: 16px; margin-bottom: 25px; }
    .sync-btn { background: linear-gradient(135deg, #059669 0%, #10b981 100%); color: white; border: none; padding: 10px 16px; width: 100%; font-weight: 600; border-radius: 10px; cursor: pointer; margin-top: 10px; }
    .slot { background: rgba(255, 255, 255, 0.03); border: 1px solid var(--card-border); padding: 20px; margin-bottom: 20px; border-radius: 18px; }
    .slot-morning { border-left: 6px solid var(--accent-morning); }
    .slot-noon    { border-left: 6px solid var(--accent-noon); }
    .slot-night   { border-left: 6px solid var(--accent-night); }
    .slot-pushover{ border-left: 6px solid var(--accent-pushover); }
    .slot h3 { margin: 0 0 15px 0; font-size: 18px; }
    label { display: block; margin-top: 10px; font-weight: 500; font-size: 12px; color: var(--text-sub); text-transform: uppercase; }
    input { width: 100%; padding: 10px 14px; margin-top: 5px; background: rgba(0, 0, 0, 0.5); border: 1px solid var(--card-border); color: var(--text-main); border-radius: 10px; font-size: 15px; outline: none; }
    .time-group { display: flex; gap: 12px; }
    .time-group > div { flex: 1; }
    .test-btn { background: rgba(255, 255, 255, 0.08); color: var(--text-main); border: 1px solid var(--card-border); font-size: 13px; padding: 9px; width: 100%; border-radius: 10px; cursor: pointer; margin-top: 14px; }
    .save-main-btn { background: linear-gradient(135deg, #3b82f6 0%, #8b5cf6 100%); color: white; border: none; padding: 14px; width: 100%; font-size: 16px; font-weight: 600; border-radius: 14px; cursor: pointer; margin-top: 10px; }
  </style>
</head>
<body>
  <div class="container">
    <div class="header-box">
      <h2>💊 Smart Medicine Hub</h2>
      <p>3D IoT Automated Dispenser Dashboard</p>
    </div>
    <div class="devtime">
      <p><span>📟 Device RTC Time:</span> <strong id="clock-display">%DEVICE_TIME%</strong></p>
      <button type="button" class="sync-btn" onclick="syncTime()">🕒 Sync Local Phone/PC Time</button>
    </div>
    <form action="/save" method="POST">
      <div class="slot slot-morning">
        <h3>🌅 Morning Schedule</h3>
        <div class="time-group">
          <div><label>Hour (0-23)</label><input type="number" name="m_hr" min="0" max="23" value="%MORNING_HR%" required></div>
          <div><label>Minute (0-59)</label><input type="number" name="m_min" min="0" max="59" value="%MORNING_MIN%" required></div>
        </div>
        <label>Medicine Compartment Box</label><input type="text" name="m_box" value="%MORNING_BOX%" required>
        <label>Prescribed Medicine Name</label><input type="text" name="m_med" value="%MORNING_MED%" required>
        <button type="button" class="test-btn" onclick="testAlarm('morning')">🔔 Test Morning Alarm</button>
      </div>
      <div class="slot slot-noon">
        <h3>☀️ Noon Schedule</h3>
        <div class="time-group">
          <div><label>Hour (0-23)</label><input type="number" name="n_hr" min="0" max="23" value="%NOON_HR%" required></div>
          <div><label>Minute (0-59)</label><input type="number" name="n_min" min="0" max="59" value="%NOON_MIN%" required></div>
        </div>
        <label>Medicine Compartment Box</label><input type="text" name="n_box" value="%NOON_BOX%" required>
        <label>Prescribed Medicine Name</label><input type="text" name="n_med" value="%NOON_MED%" required>
        <button type="button" class="test-btn" onclick="testAlarm('noon')">🔔 Test Noon Alarm</button>
      </div>
      <div class="slot slot-night">
        <h3>🌙 Night Schedule</h3>
        <div class="time-group">
          <div><label>Hour (0-23)</label><input type="number" name="ni_hr" min="0" max="23" value="%NIGHT_HR%" required></div>
          <div><label>Minute (0-59)</label><input type="number" name="ni_min" min="0" max="59" value="%NIGHT_MIN%" required></div>
        </div>
        <label>Medicine Compartment Box</label><input type="text" name="ni_box" value="%NIGHT_BOX%" required>
        <label>Prescribed Medicine Name</label><input type="text" name="ni_med" value="%NIGHT_MED%" required>
        <button type="button" class="test-btn" onclick="testAlarm('night')">🔔 Test Night Alarm</button>
      </div>
      <div class="slot slot-pushover">
        <h3>🔔 Pushover App API Settings</h3>
        <label>Pushover User Key</label><input type="text" name="po_user_key" value="%PO_USER_KEY%" placeholder="Enter User Key">
        <label>Pushover API Token / App Token</label><input type="text" name="po_api_token" value="%PO_API_TOKEN%" placeholder="Enter App Token">
      </div>
      <button type="submit" class="save-main-btn">💾 Save Configurations</button>
    </form>
  </div>
  <script>
    function syncTime() {
      var epoch = Math.floor(Date.now() / 1000);
      fetch('/settime?epoch=' + epoch).then(function() { location.reload(); });
    }
    function testAlarm(slot) {
      fetch('/testalarm?slot=' + slot).then(r => r.text()).then(msg => alert(msg));
    }
  </script>
</body>
</html>
)rawliteral";

void setup() {
  Serial.begin(115200);
  delay(1000);

  EEPROM.begin(EEPROM_SIZE);

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Smart Medicine");
  lcd.setCursor(0, 1);
  lcd.print("Checking WiFi...");

  // ==========================================
  // WIFI CONNECTION (WiFiManager captive portal)
  // ==========================================
  // If there are no saved credentials (or they fail), WiFiManager starts a
  // Wi-Fi hotspot named apSSID. Connect your phone/PC to it and a captive
  // portal page pops up automatically (or open 192.168.4.1 manually) where
  // you pick your router's SSID and enter the password. Once saved, the
  // ESP32 reboots the Wi-Fi stack and connects normally; credentials are
  // remembered for future boots.
  WiFiManager wm;
  // wm.resetSettings(); // uncomment once to force-clear saved Wi-Fi creds for testing

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connect to WiFi:");
  lcd.setCursor(0, 1);
  lcd.print(apSSID);

  bool connected = wm.autoConnect(apSSID);

  if (!connected) {
    Serial.println("Failed to connect to Wi-Fi and hit portal timeout. Rebooting...");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Setup Failed");
    lcd.setCursor(0, 1);
    lcd.print("Rebooting...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("Connected to Wi-Fi successfully!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("IP:");
  lcd.print(WiFi.localIP());
  secured_client.setInsecure();

  loadConfigFromEEPROM();

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/settime", HTTP_GET, handleSetTime);
  server.on("/testalarm", HTTP_GET, handleTestAlarm);
  server.begin();
  Serial.println("HTTP server started");

  if (!rtc.begin()) {
    while (1); 
  }
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(1.0);

  delay(2000);
  lcd.clear();
}

void loop() {
  server.handleClient();

  if (alarmActive && digitalRead(BUTTON_PIN) == LOW) {
    delay(30); 
    if (digitalRead(BUTTON_PIN) == LOW) {
      stopAlarm();
      return; 
    }
  }

  DateTime now = rtc.now();

  if (mp3 && mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      delete mp3;
      delete file;
      mp3 = NULL;
      file = NULL;
      isSpeaking = false; 
      delay(1500); 
    }
  }

  if (!alarmActive) {
    if (millis() - lastLcdUpdate >= 1000) {
      lastLcdUpdate = millis();
      displayTime(now);
    }
    checkSchedule(now);
  } else {
    if (millis() - alarmStartTime >= 60000) {
      stopAlarm();
      return;
    }

    if (millis() - previousMillis >= 500) {
      previousMillis = millis();
      blinkState = !blinkState;
      digitalWrite(LED_PIN, blinkState); 

      if (!isSpeaking && mp3 == NULL) {
        digitalWrite(BUZZER_PIN, blinkState); 
      } else {
        digitalWrite(BUZZER_PIN, LOW); 
      }
    }

    if (!isSpeaking && WiFi.status() == WL_CONNECTED && mp3 == NULL) {
      playCurrentVoice();
    }
  }
}

void loadDefaultConfig() {
  config.morningHour = 8;
  config.morningMinute = 0;
  strcpy(config.morningBox, "Box 1");
  strcpy(config.morningMed, "Napa");

  config.noonHour = 14;
  config.noonMinute = 0;
  strcpy(config.noonBox, "Box 2");
  strcpy(config.noonMed, "Vitamin C");

  config.nightHour = 22;
  config.nightMinute = 0;
  strcpy(config.nightBox, "Box 3");
  strcpy(config.nightMed, "Paracetamol");

  strcpy(config.pushoverUserKey, "");
  strcpy(config.pushoverApiToken, "");
}

void saveConfigToEEPROM() {
  EEPROM.put(0, config);
  EEPROM.commit();
}

void loadConfigFromEEPROM() {
  EEPROM.get(0, config);
  if (config.morningHour > 23 || config.noonHour > 23 || config.nightHour > 23) {
    loadDefaultConfig();
    saveConfigToEEPROM();
  }
}

void handleRoot() {
  String html = MEDICINE_DASHBOARD_PAGE;

  DateTime now = rtc.now();
  char timeBuf[25];
  snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d",
           now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
  html.replace("%DEVICE_TIME%", String(timeBuf));

  html.replace("%MORNING_HR%", String(config.morningHour));
  html.replace("%MORNING_MIN%", String(config.morningMinute));
  html.replace("%MORNING_BOX%", String(config.morningBox));
  html.replace("%MORNING_MED%", String(config.morningMed));

  html.replace("%NOON_HR%", String(config.noonHour));
  html.replace("%NOON_MIN%", String(config.noonMinute));
  html.replace("%NOON_BOX%", String(config.noonBox));
  html.replace("%NOON_MED%", String(config.noonMed));

  html.replace("%NIGHT_HR%", String(config.nightHour));
  html.replace("%NIGHT_MIN%", String(config.nightMinute));
  html.replace("%NIGHT_BOX%", String(config.nightBox));
  html.replace("%NIGHT_MED%", String(config.nightMed));

  html.replace("%PO_USER_KEY%", String(config.pushoverUserKey));
  html.replace("%PO_API_TOKEN%", String(config.pushoverApiToken));

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("m_hr")) config.morningHour = server.arg("m_hr").toInt();
  if (server.hasArg("m_min")) config.morningMinute = server.arg("m_min").toInt();
  if (server.hasArg("m_box")) server.arg("m_box").toCharArray(config.morningBox, sizeof(config.morningBox));
  if (server.hasArg("m_med")) server.arg("m_med").toCharArray(config.morningMed, sizeof(config.morningMed));

  if (server.hasArg("n_hr")) config.noonHour = server.arg("n_hr").toInt();
  if (server.hasArg("n_min")) config.noonMinute = server.arg("n_min").toInt();
  if (server.hasArg("n_box")) server.arg("n_box").toCharArray(config.noonBox, sizeof(config.noonBox));
  if (server.hasArg("n_med")) server.arg("n_med").toCharArray(config.noonMed, sizeof(config.noonMed));

  if (server.hasArg("ni_hr")) config.nightHour = server.arg("ni_hr").toInt();
  if (server.hasArg("ni_min")) config.nightMinute = server.arg("ni_min").toInt();
  if (server.hasArg("ni_box")) server.arg("ni_box").toCharArray(config.nightBox, sizeof(config.nightBox));
  if (server.hasArg("ni_med")) server.arg("ni_med").toCharArray(config.nightMed, sizeof(config.nightMed));

  if (server.hasArg("po_user_key")) server.arg("po_user_key").toCharArray(config.pushoverUserKey, sizeof(config.pushoverUserKey));
  if (server.hasArg("po_api_token")) server.arg("po_api_token").toCharArray(config.pushoverApiToken, sizeof(config.pushoverApiToken));

  saveConfigToEEPROM();

  String successHtml =
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<meta http-equiv='refresh' content='2;url=/'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>"
    "body{font-family:'Inter',sans-serif;background:#0f172a;color:#f8fafc;margin:0;display:flex;align-items:center;justify-content:center;height:100vh;}"
    ".box{background:rgba(255,255,255,0.05);backdrop-filter:blur(12px);padding:30px;border-radius:20px;border:1px solid rgba(255,255,255,0.1);text-align:center;box-shadow:0 20px 40px rgba(0,0,0,0.5);}"
    "h2{color:#10b981;margin-top:0;}"
    "p{color:#94a3b8;}"
    "</style></head><body>"
    "<div class='box'><h2>✅ Successfully Saved!</h2><p>Updating dispenser configurations...</p></div>"
    "</body></html>";
  server.send(200, "text/html", successHtml);
}

void handleSetTime() {
  if (server.hasArg("epoch")) {
    uint32_t epoch = strtoul(server.arg("epoch").c_str(), NULL, 10);
    const uint32_t SECONDS_1970_TO_2000 = 946684800UL;
    if (epoch > SECONDS_1970_TO_2000) {
      rtc.adjust(DateTime(epoch - SECONDS_1970_TO_2000));
      server.send(200, "text/plain", "Device time updated");
      return;
    }
  }
  server.send(400, "text/plain", "Bad Request");
}

void handleTestAlarm() {
  if (alarmActive) {
    server.send(200, "text/plain", "An alarm is already running on the device.");
    return;
  }
  String slot = server.hasArg("slot") ? server.arg("slot") : "";
  if (slot == "morning") {
    startAlarmSession(config.morningBox, config.morningMed, config.morningMinute);
  } else if (slot == "noon") {
    startAlarmSession(config.noonBox, config.noonMed, config.noonMinute);
  } else if (slot == "night") {
    startAlarmSession(config.nightBox, config.nightMed, config.nightMinute);
  } else {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  server.send(200, "text/plain", "Test alarm triggered on ESP32 device.");
}

void displayTime(DateTime now) {
  lcd.setCursor(0, 0);
  lcd.print("Time: ");
  
  int hr = now.hour();
  String ampm = (hr >= 12) ? "PM" : "AM";
  if (hr == 0) hr = 12;
  else if (hr > 12) hr -= 12;

  if (hr < 10) lcd.print('0');
  lcd.print(hr);
  lcd.print(':');
  
  if (now.minute() < 10) lcd.print('0');
  lcd.print(now.minute());
  lcd.print(':');
  
  if (now.second() < 10) lcd.print('0');
  lcd.print(now.second());
  lcd.print(" ");
  lcd.print(ampm);
  
  lcd.setCursor(0, 1);
  lcd.print("Status: Waiting ");
}

void checkSchedule(DateTime now) {
  int hr = now.hour();
  int min = now.minute();
  
  if (min != lastTriggeredMinute) {
    lastTriggeredSlot = 0; 
  }

  if (hr == config.morningHour && min == config.morningMinute && lastTriggeredSlot != 1) {
    lastTriggeredSlot = 1;
    lastTriggeredMinute = min;
    startAlarmSession(config.morningBox, config.morningMed, min);
  }
  else if (hr == config.noonHour && min == config.noonMinute && lastTriggeredSlot != 2) {
    lastTriggeredSlot = 2;
    lastTriggeredMinute = min;
    startAlarmSession(config.noonBox, config.noonMed, min);
  }
  else if (hr == config.nightHour && min == config.nightMinute && lastTriggeredSlot != 3) {
    lastTriggeredSlot = 3;
    lastTriggeredMinute = min;
    startAlarmSession(config.nightBox, config.nightMed, min);
  }
}

void buildVoiceList(String box, String med) {
  singleVoiceMsg = "আসসালামু আলাইকুম। ওষুধ খাওয়ার সময় হয়েছে! দ্রুত " + box + " নম্বর বক্স থেকে " + med + " ওষুধটি বের করে খেয়ে নিন এবং সাথে এক গ্লাস পানি পান করুন।";
}

void startAlarmSession(String box, String med, int currentMin) {
  alarmActive = true;
  currentMedicine = box + " (" + med + ")";
  alarmStartTime = millis();        
  isSpeaking = false;

  buildVoiceList(box, med);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Take Medicine:");
  lcd.setCursor(0, 1);
  lcd.print(currentMedicine);

  if (WiFi.status() == WL_CONNECTED) {
    String message = "⚠️ রিমাইন্ডার: ঔষধ খাওয়ার সময় হয়েছে!\n💊 বিবরণ: " + currentMedicine;
    sendPushoverNotification(message);
  }
}

void sendPushoverNotification(String message) {
  String userKey = String(config.pushoverUserKey);
  String apiToken = String(config.pushoverApiToken);

  if (userKey.length() > 5 && apiToken.length() > 5) {
    if (secured_client.connect("api.pushover.net", 443)) {
      String postData = "token=" + apiToken + "&user=" + userKey + "&message=" + urlEncode(message);
      
      secured_client.print(String("POST /1/messages.json HTTP/1.1\r\n") +
                           "Host: api.pushover.net\r\n" +
                           "Content-Type: application/x-www-form-urlencoded\r\n" +
                           "Content-Length: " + String(postData.length()) + "\r\n" +
                           "Connection: close\r\n\r\n" +
                           postData);
      
      unsigned long timeout = millis();
      while (secured_client.connected()) {
        String line = secured_client.readStringUntil('\n');
        if (line == "\r") break;
        if (millis() - timeout > 5000) break;
      }
      secured_client.stop();
    }
  }
}

void playCurrentVoice() {
  isSpeaking = true; 
  digitalWrite(BUZZER_PIN, LOW); 

  String encodedText = urlEncode(singleVoiceMsg);
  String tts_url = "http://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=bn&q=" + encodedText;

  if (mp3) {
    mp3->stop();
    delete mp3;
    delete file;
    mp3 = NULL;
    file = NULL;
  }

  file = new AudioFileSourceHTTPStream(tts_url.c_str());
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(1.0);
  
  mp3 = new AudioGeneratorMP3();
  if (mp3->begin(file, out)) {
    Serial.println("Speaking: " + singleVoiceMsg);
  } else {
    Serial.println("MP3 Begin Failed!");
    isSpeaking = false;
  }
}

String urlEncode(String str) {
  String encoded = "";
  char c;
  char code0;
  char code1;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encoded += "+";
    } else if (isalnum(c)) {
      encoded += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) code0 = c - 10 + 'A';
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}

void stopAlarm() {
  alarmActive = false;
  
  if (mp3 && mp3->isRunning()) {
    mp3->stop();
    delete mp3;
    delete file;
    mp3 = NULL;
    file = NULL;
  }
  
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  isSpeaking = false;
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Medicine Taken!");
  lcd.setCursor(0, 1);
  lcd.print("Good Job!");
  
  if (WiFi.status() == WL_CONNECTED) {
    String successMsg = "✅ ইউজার সময়মতো " + currentMedicine + " ওষুধটি খেয়ে ফেলেছেন।";
    sendPushoverNotification(successMsg);
  }

  delay(3000);
  lcd.clear();
}
