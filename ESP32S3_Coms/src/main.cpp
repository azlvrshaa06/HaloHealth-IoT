/*
 * HaloHealth – ESP32-S3 Kit Box  (RTOS version)
 *
 * Core split:
 *   Core 1 → loop()     = UI only  (TFT, touch, button, battery, state machine)
 *   Core 0 → taskFirebase() = Firebase only (fetchUserID, kirimKeFirestore)
 *
 * Synchronization:
 *   xSemKirim  – binary semaphore: UI signals "please send now"
 *   xMutexUID  – mutex: protects the userID String (written Core0, read Core1)
 *   FBPayload  – plain struct snapshot: data is copied before giving the semaphore,
 *                the semaphore itself acts as a memory barrier between cores.
 *
 * Rule: NEVER call any tft.xxx() from Core0 – SPI bus corruption will result.
 *       All TFT code lives in loop() / Core1 functions only.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include "RandomID.h"
#include "NTPTime.h"

// ── Konfigurasi ──────────────────────────────────────────────────────
#define WIFI_SSID              "MinumanBadak"
#define WIFI_PASSWORD          "rhynmebobaozzzz"
#define API_KEY                "AIzaSyAOGI87LY4isdWMD5Fz1M7Mx-Sbc5wBAf4"
#define DATABASE_URL           "https://halohealth-d22c2-default-rtdb.firebaseio.com/"
#define USER_EMAIL             "asepcecep@gmail.com"
#define USER_PASSWORD          "qwerty"
#define PROJECT_ID             "halohealth-d22c2"
#define ALAT                   "Oximeter"
#define USERID_RETRY_INTERVAL   5000UL
#define WIFI_CONNECT_TIMEOUT   20000UL
#define FIREBASE_READY_TIMEOUT 20000UL
#define BUTTON_PIN              0
#define HOLD_DURATION          3000UL

// ════════════════════════════════════════════════════════════════════
//  RTOS  ─ no extra #include needed, FreeRTOS ships with Arduino ESP32
// ════════════════════════════════════════════════════════════════════
SemaphoreHandle_t xSemKirim = NULL;   // binary: UI → Firebase "send now"
SemaphoreHandle_t xMutexUID = NULL;   // mutex : protect userID string

// Payload snapshot copied by Core1 before xSemaphoreGive.
// The semaphore give/take pair is a memory barrier, so no extra lock needed.
struct FBPayload {
  int  bpm;
  int  saturasi;
  char status[20];
  char waktu[40];
  char uid[64];
};
FBPayload fbPayload;

// Result flags written by Core0, read by Core1
volatile bool fbBusy = false;
volatile bool fbDone = false;
volatile bool fbOK   = false;
char          fbErr[80] = "";

TaskHandle_t hTaskFirebase = NULL;
// ════════════════════════════════════════════════════════════════════

// ── ESP-NOW struct ────────────────────────────────────────────────────
typedef struct {
  int   bpm;
  float saturasi;
  char  status[20];
  bool  dataReady;
} SensorData;

SensorData    dataTerima;
volatile bool adaDataBaru = false;

// ── Battery ───────────────────────────────────────────────────────────
const int   analogInPin = 5;
int         sensorValue;
float       calibration  = 0.20f;
float       voltage;
int         bat_percentage;
const float vref         = 3.3f;

float mapfloat(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
int readADC(int pin, int samples = 64) {
  long sum = 0;
  for (int i = 0; i < samples; i++) { sum += analogRead(pin); delayMicroseconds(100); }
  return (int)(sum / samples);
}

// ── Objects ───────────────────────────────────────────────────────────
RandomID rid;
NTPTime  ntp(7);
String   KIT_ID  = "";
String   userID  = "";
String   lastDocID = "";

FirebaseData   fbdoQuery, fbdoWrite, fbdoGet;
FirebaseAuth   auth;
FirebaseConfig config;

TFT_eSPI tft = TFT_eSPI();
#define SCREEN_W 320
#define SCREEN_H 240

// ── Colours ───────────────────────────────────────────────────────────
#define C_BG         0x1808u
#define C_CARD       0x3A1A3Au
#define C_CARD_DARK  0x1A0A1Au
#define C_CARD_BDR   0x6A3A6Au
#define C_HDR        0x3D1A3Du
#define C_HDR_LINE   0x5A2A5Au
#define C_DIVIDER    0x5A2A5Au
#define C_BUBBLE     0x5A1A4Au
#define C_BUBBLE_BDR 0xFC9Fu

uint16_t col24(uint32_t c) {
  uint8_t r=(c>>16)&0xFF, g=(c>>8)&0xFF, b=c&0xFF;
  return ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3);
}

// ── Display data ──────────────────────────────────────────────────────
int    displayBPM    = 0;
float  displayOksi   = 0.f;
String displayWaktu  = "Menunggu data...";
String displayStatus = "";

// ── App state  (S_SENDING + S_RESULT are new) ─────────────────────────
enum AppState { S_IDLE, S_KONFIRMASI, S_SENDING, S_RESULT };
AppState appState = S_IDLE;

// ── Touch debounce ────────────────────────────────────────────────────
uint32_t lastTouchMs = 0;
#define  TOUCH_DEBOUNCE_MS 300

// ── Button positions ──────────────────────────────────────────────────
#define BTN_YA_X   20
#define BTN_YA_Y  180
#define BTN_YA_W  120
#define BTN_YA_H   36
#define BTN_TDK_X 180
#define BTN_TDK_Y 180
#define BTN_TDK_W 120
#define BTN_TDK_H  36


// ════════════════════════════════════════════════════════════════════
//  ESP-NOW CALLBACK   ← runs on Core0 WiFi task
//  IMPORTANT: removed all tft calls here — TFT is owned by Core1
// ════════════════════════════════════════════════════════════════════
void onDataReceived(const uint8_t *mac, const uint8_t *data, int len) {
  memcpy(&dataTerima, data, sizeof(dataTerima));
  if (dataTerima.dataReady) adaDataBaru = true;
  Serial.printf("[ESP-NOW] len=%d BPM:%d SpO2:%.0f ready=%d\n",
                len, dataTerima.bpm, dataTerima.saturasi, dataTerima.dataReady);
}

// ════════════════════════════════════════════════════════════════════
//  TOUCH HELPER  (Core1 only)
// ════════════════════════════════════════════════════════════════════
bool getTouchPoint(int &px, int &py) {
  uint16_t tx, ty;
  if (!tft.getTouch(&tx, &ty))                   return false;
  if (millis() - lastTouchMs < TOUCH_DEBOUNCE_MS) return false;
  lastTouchMs = millis();
  px = (int)tx; py = (int)ty;
  return true;
}

// ════════════════════════════════════════════════════════════════════
//  TFT FUNCTIONS  — all Core1 only, never called from taskFirebase
// ════════════════════════════════════════════════════════════════════
void drawStatusBar() {
  tft.fillRect(0, 0, SCREEN_W, 28, col24(C_HDR));
  int wx=14, wy=6;
  tft.drawArc(wx,wy+10,12,10,200,340,C_BUBBLE_BDR,C_BUBBLE_BDR);
  tft.drawArc(wx,wy+10, 8, 6,200,340,C_BUBBLE_BDR,C_BUBBLE_BDR);
  tft.drawArc(wx,wy+10, 4, 2,200,340,C_BUBBLE_BDR,C_BUBBLE_BDR);
  tft.fillCircle(wx,wy+10,2,C_BUBBLE_BDR);
  tft.setTextColor(C_BUBBLE_BDR, col24(C_HDR)); tft.setTextSize(1);
  tft.setCursor(30,10); tft.print("Connected");
  tft.setTextColor(TFT_WHITE, col24(C_HDR));
  tft.setCursor(SCREEN_W/2-30,10); tft.print("HaloHealth");
  tft.setCursor(SCREEN_W-80,10);   tft.printf("%d%%", bat_percentage);
  tft.drawLine(0,28,SCREEN_W,28,col24(C_HDR_LINE));
}

void drawKitBubble() {
  int bx=14,by=36,bw=SCREEN_W-28,bh=40,br=10;
  tft.fillRoundRect(bx+2,by+2,bw,bh,br,col24(C_CARD_DARK));
  tft.fillRoundRect(bx,by,bw,bh,br,col24(C_BUBBLE));
  tft.drawRoundRect(bx,by,bw,bh,br,C_BUBBLE_BDR);
  tft.setTextColor(C_BUBBLE_BDR,col24(C_BUBBLE)); tft.setTextSize(1);
  tft.setCursor(bx+14,by+6);  tft.print("Kode Kit");
  tft.setTextColor(TFT_WHITE,col24(C_BUBBLE)); tft.setTextSize(2);
  tft.setCursor(bx+14,by+18); tft.print(KIT_ID);
  tft.setTextColor(C_BUBBLE_BDR,col24(C_BUBBLE)); tft.setTextSize(1);
  tft.setCursor(bx+100,by+18);
  // Safe read of userID via mutex
  bool hasUser = false;
  if (xMutexUID != NULL) {
    xSemaphoreTake(xMutexUID, portMAX_DELAY);
    hasUser = (userID != "");
    xSemaphoreGive(xMutexUID);
  } else {
    hasUser = (userID != "");
  }
  tft.print(hasUser ? "Terhubung" : "Mencari user...");
}

void drawCard() {
  int cx=12,cy=86,cw=SCREEN_W-24,ch=100,r=14;
  tft.fillRoundRect(cx+2,cy+2,cw,ch,r,col24(C_CARD_DARK));
  tft.fillRoundRect(cx,cy,cw,ch,r,col24(C_CARD));
  tft.drawRoundRect(cx,cy,cw,ch,r,col24(C_CARD_BDR));

  tft.setTextColor(C_BUBBLE_BDR,col24(C_CARD)); tft.setTextSize(1);
  tft.setCursor(cx+18,cy+12); tft.print("DETAK JANTUNG");
  tft.setTextColor(TFT_WHITE,col24(C_CARD)); tft.setTextSize(4);
  tft.setCursor(cx+14,cy+28);
  displayBPM > 0 ? tft.print(displayBPM) : tft.print("--");
  tft.setTextColor(C_BUBBLE_BDR,col24(C_CARD)); tft.setTextSize(1);
  tft.setCursor(cx+14,cy+78); tft.print("bpm");

  tft.drawLine(cx+cw/2,cy+10,cx+cw/2,cy+ch-10,col24(C_DIVIDER));

  tft.setTextColor(C_BUBBLE_BDR,col24(C_CARD)); tft.setTextSize(1);
  tft.setCursor(cx+cw/2+14,cy+12); tft.print("SATURASI O2");
  tft.setTextColor(TFT_WHITE,col24(C_CARD)); tft.setTextSize(4);
  tft.setCursor(cx+cw/2+10,cy+28);
  displayOksi > 0 ? tft.print((int)displayOksi) : tft.print("--");
  tft.setTextColor(C_BUBBLE_BDR,col24(C_CARD)); tft.setTextSize(1);
  tft.setCursor(cx+cw/2+10,cy+78); tft.print("%");
}

void refreshCard() {
  tft.fillRect(12,86,SCREEN_W-24,100,col24(C_BG));
  drawCard();
}

void drawInfoBar() {
  int iy=196;
  tft.fillRect(0,iy,SCREEN_W,SCREEN_H-iy,col24(C_BG));
  uint16_t sc = TFT_WHITE;
  if      (displayStatus=="Normal") sc=TFT_GREEN;
  else if (displayStatus=="Rendah") sc=TFT_YELLOW;
  else if (displayStatus=="Kritis") sc=TFT_RED;
  tft.setTextColor(sc,col24(C_BG)); tft.setTextSize(2);
  tft.setCursor(14,iy+2);
  displayStatus!="" ? tft.print("Status: "+displayStatus) : tft.print("Menunggu data...");
  tft.setTextColor(C_BUBBLE_BDR,col24(C_BG)); tft.setTextSize(1);
  int ai=displayWaktu.indexOf(" at ");
  if(ai>0){
    tft.setCursor(14,iy+22); tft.print(displayWaktu.substring(0,ai));
    tft.setCursor(14,iy+32); tft.print(displayWaktu.substring(ai+4));
  } else {
    tft.setCursor(14,iy+22); tft.print(displayWaktu);
  }
}

void drawPollingDot(bool aktif) {
  tft.fillCircle(SCREEN_W-14, SCREEN_H-10, 5,
                 aktif ? (uint16_t)TFT_GREEN : col24(C_CARD_DARK));
}

void drawKonfirmasi() {
  tft.fillScreen(col24(C_BG));
  drawStatusBar();
  int cx=12,cy=36,cw=SCREEN_W-24,ch=110,r=12;
  tft.fillRoundRect(cx+2,cy+2,cw,ch,r,col24(C_CARD_DARK));
  tft.fillRoundRect(cx,cy,cw,ch,r,col24(C_CARD));
  tft.drawRoundRect(cx,cy,cw,ch,r,col24(C_CARD_BDR));
  tft.setTextColor(TFT_WHITE,col24(C_CARD)); tft.setTextSize(2);
  tft.setCursor(cx+14,cy+8); tft.print("Data Pengukuran");
  tft.setTextColor(C_BUBBLE_BDR,col24(C_CARD)); tft.setTextSize(1);
  tft.setCursor(cx+14,cy+30); tft.printf("BPM     : %d bpm", displayBPM);
  tft.setCursor(cx+14,cy+44); tft.printf("Saturasi: %.0f %%", displayOksi);
  tft.setCursor(cx+14,cy+58); tft.print("Status  : "+displayStatus);
  tft.setCursor(cx+14,cy+72);
  int ai=displayWaktu.indexOf(" at ");
  if(ai>0){
    tft.print(displayWaktu.substring(0,ai));
    tft.setCursor(cx+14,cy+84); tft.print(displayWaktu.substring(ai+4));
  } else { tft.print(displayWaktu); }

  tft.setTextColor(TFT_WHITE,col24(C_BG)); tft.setTextSize(2);
  tft.setCursor(14,155); tft.print("Kirim ke Firestore?");

  tft.fillRoundRect(BTN_YA_X, BTN_YA_Y,BTN_YA_W, BTN_YA_H, 10,TFT_RED);
  tft.setTextColor(TFT_WHITE,TFT_RED); tft.setTextSize(2);
  tft.setCursor(BTN_YA_X+35,BTN_YA_Y+10); tft.print("TIDAK");

  tft.fillRoundRect(BTN_TDK_X,BTN_TDK_Y,BTN_TDK_W,BTN_TDK_H,10,TFT_GREEN);
  tft.setTextColor(TFT_WHITE,TFT_GREEN); tft.setTextSize(2);
  tft.setCursor(BTN_TDK_X+18,BTN_TDK_Y+10); tft.print("YA");
}

// NEW – shown while Firebase task is running (non-blocking for loop)
void drawSendingScreen() {
  tft.fillScreen(col24(C_BG));
  drawStatusBar();
  tft.setTextColor(TFT_WHITE,col24(C_BG)); tft.setTextSize(2);
  tft.setCursor(SCREEN_W/2-70,80); tft.print("Mengirim...");
  // Simple animated-looking dots (static, loop redraws if wanted)
  tft.fillCircle(120,140,8,col24(C_BUBBLE));
  tft.fillCircle(160,140,8,col24(C_CARD));
  tft.fillCircle(200,140,8,col24(C_BUBBLE));
  tft.setTextColor(C_BUBBLE_BDR,col24(C_BG)); tft.setTextSize(1);
  tft.setCursor(14,170);
  tft.printf("BPM: %d | SpO2: %.0f%%", displayBPM, displayOksi);
}

// NEW – shown after Firebase task finishes
void drawResultScreen(bool ok, const char *errMsg) {
  tft.fillScreen(col24(C_BG));
  drawStatusBar();
  if (ok) {
    tft.setTextColor(TFT_GREEN,col24(C_BG)); tft.setTextSize(3);
    tft.setCursor(SCREEN_W/2-90,80); tft.print("Terkirim!");
    tft.setTextColor(C_BUBBLE_BDR,col24(C_BG)); tft.setTextSize(1);
    tft.setCursor(14,130); tft.print(displayWaktu);
  } else {
    tft.setTextColor(TFT_RED,col24(C_BG)); tft.setTextSize(2);
    tft.setCursor(14,70); tft.print("Gagal Kirim!");
    tft.setTextColor(TFT_WHITE,col24(C_BG)); tft.setTextSize(1);
    tft.setCursor(14,100);
    String e=String(errMsg); tft.print(e.length()>42?e.substring(0,42):e);
  }
}

void drawAll() {
  tft.fillScreen(col24(C_BG));
  drawStatusBar();
  drawKitBubble();
  drawCard();
  drawInfoBar();
  drawPollingDot(false);
  tft.fillRect(0,SCREEN_H-14,SCREEN_W,14,col24(C_CARD_DARK));
  tft.setTextColor(C_BUBBLE_BDR,col24(C_CARD_DARK)); tft.setTextSize(1);
  tft.setCursor(4,SCREEN_H-10);
  if (xMutexUID) {
    xSemaphoreTake(xMutexUID,portMAX_DELAY);
    String uid6=userID.substring(0,6);
    xSemaphoreGive(xMutexUID);
    tft.printf("MAC: %s | CH: %d | UID: %s",
               WiFi.macAddress().c_str(), WiFi.channel(), uid6.c_str());
  }
}


// ════════════════════════════════════════════════════════════════════
//  FIREBASE – FETCH USER ID  (called from Core0 taskFirebase)
// ════════════════════════════════════════════════════════════════════
bool fetchUserID() {
  FirebaseJson structuredQuery,from,fieldFilter,field,value,filter;
  FirebaseJsonArray fromArray;
  from.set("collectionId","users"); fromArray.add(from);
  structuredQuery.set("from",fromArray);
  field.set("fieldPath","kodeKit");
  value.set("stringValue",KIT_ID);
  fieldFilter.set("field",field); fieldFilter.set("op","EQUAL"); fieldFilter.set("value",value);
  filter.set("fieldFilter",fieldFilter);
  structuredQuery.set("where",filter);

  if (!Firebase.Firestore.runQuery(&fbdoQuery,PROJECT_ID,"","",&structuredQuery)) {
    Serial.println("fetchUserID gagal: "+fbdoQuery.errorReason());
    return false;
  }
  FirebaseJson response; response.setJsonData(fbdoQuery.payload().c_str());
  FirebaseJsonData docName; response.get(docName,"[0]/document/name");
  if (!docName.success) {
    Serial.println("User tidak ditemukan untuk kodeKit: "+KIT_ID);
    return false;
  }
  String newUID=docName.stringValue.substring(docName.stringValue.lastIndexOf("/")+1);

  // Only hold the mutex for the actual write, not during the whole Firebase call
  xSemaphoreTake(xMutexUID,portMAX_DELAY);
  userID=newUID;
  xSemaphoreGive(xMutexUID);

  Serial.println("UserID: "+newUID);
  return true;
}

// ════════════════════════════════════════════════════════════════════
//  FIREBASE – SEND  (Core0 only, zero TFT calls)
// ════════════════════════════════════════════════════════════════════
void kirimKeFirestoreRTOS() {
  fbBusy=true; fbDone=false; fbOK=false; fbErr[0]='\0';

  // Safe to read fbPayload here — semaphore take above acts as memory barrier
  String uid    = String(fbPayload.uid);
  String waktu  = String(fbPayload.waktu);
  int    bpm    = fbPayload.bpm;
  int    sat    = fbPayload.saturasi;
  String status = String(fbPayload.status);

  if (uid=="") {
    strlcpy(fbErr,"UserID kosong!",sizeof(fbErr));
    fbOK=false; fbBusy=false; fbDone=true; return;
  }
  if (!Firebase.ready()) {
    strlcpy(fbErr,"Firebase tidak ready!",sizeof(fbErr));
    fbOK=false; fbBusy=false; fbDone=true; return;
  }

  String path="users/"+uid+"/alat/"+String(ALAT)+"/riwayat";
  FirebaseJson content;
  content.set("fields/bpm/integerValue",          bpm);
  content.set("fields/saturasi/integerValue",     sat);
  content.set("fields/statusSaturasi/stringValue",status);
  content.set("fields/waktu/timestampValue",         waktu);
  
  if (Firebase.Firestore.createDocument(&fbdoWrite,PROJECT_ID,"",path.c_str(),content.raw())) {
    // Update lastRiwayatID
    FirebaseJson rj; rj.setJsonData(fbdoWrite.payload().c_str());
    FirebaseJsonData nd; rj.get(nd,"name");
    if (nd.success) {
      String newDocID=nd.stringValue.substring(nd.stringValue.lastIndexOf("/")+1);
      FirebaseJson uc; uc.set("fields/lastRiwayatID/stringValue",newDocID);
      Firebase.Firestore.patchDocument(&fbdoWrite,PROJECT_ID,"",
                                       ("users/"+uid).c_str(),uc.raw(),"lastRiwayatID");
    }
    fbOK=true;
    Serial.println("[Firebase] Berhasil dikirim!");
  } else {
    strlcpy(fbErr,fbdoWrite.errorReason().c_str(),sizeof(fbErr));
    fbOK=false;
    Serial.println("[Firebase] Gagal: "+String(fbErr));
  }
  fbBusy=false; fbDone=true;
}

// ════════════════════════════════════════════════════════════════════
//  RTOS TASK – CORE 0  (all Firebase, never touches TFT)
// ════════════════════════════════════════════════════════════════════
void taskFirebase(void *pvParameters) {
  unsigned long lastRetry = 0;

  for (;;) {  // use for(;;) instead of while(true) — same thing, classic RTOS style

    // ── 1. Background userID retry if still empty ──
    xSemaphoreTake(xMutexUID,portMAX_DELAY);
    bool noUID=(userID=="");
    xSemaphoreGive(xMutexUID);

    if (noUID && (millis()-lastRetry >= USERID_RETRY_INTERVAL)) {
      lastRetry=millis();
      fetchUserID();
    }

    // ── 2. Wait up to 100ms for a "send" signal from loop() ──
    //    100ms timeout keeps the retry loop above ticking
    if (xSemaphoreTake(xSemKirim, pdMS_TO_TICKS(100)) == pdTRUE) {
      kirimKeFirestoreRTOS();
    }

    vTaskDelay(pdMS_TO_TICKS(10));  // yield — let WiFi stack breathe
  }
}

// ════════════════════════════════════════════════════════════════════
//  GENERATE KIT ID  (Core1 only)
// ════════════════════════════════════════════════════════════════════
void generateNewKitID() {
  if (xMutexUID) {
    xSemaphoreTake(xMutexUID,portMAX_DELAY);
    userID="";
    xSemaphoreGive(xMutexUID);
  } else { userID=""; }
  lastDocID="";
  rid.generateNew();
  KIT_ID=String(rid.getID());
  Serial.println("[KIT ID BARU] "+KIT_ID);
  tft.fillRect(0,SCREEN_H-30,SCREEN_W,30,TFT_RED);
  tft.setTextColor(TFT_WHITE,TFT_RED); tft.setTextSize(1);
  tft.setCursor(14,SCREEN_H-20); tft.print("KIT ID BARU: "+KIT_ID);
  delay(3000);
  drawAll();
}

void checkButton() {
  if (digitalRead(BUTTON_PIN)==LOW) {
    unsigned long pressStart=millis();
    tft.fillRect(0,SCREEN_H-14,SCREEN_W,14,col24(C_CARD_DARK));
    tft.setTextColor(TFT_YELLOW,col24(C_CARD_DARK)); tft.setTextSize(1);
    tft.setCursor(14,SCREEN_H-10); tft.print("Tahan untuk generate KIT ID baru...");
    int barX=0;
    while (digitalRead(BUTTON_PIN)==LOW) {
      unsigned long held=millis()-pressStart;
      int prog=map((long)held,0,(long)HOLD_DURATION,0,SCREEN_W);
      tft.fillRect(barX,SCREEN_H-4,prog-barX,4,TFT_YELLOW);
      barX=prog;
      if (held>=HOLD_DURATION) {
        tft.fillRect(0,SCREEN_H-14,SCREEN_W,14,col24(C_CARD_DARK));
        generateNewKitID();
        while (digitalRead(BUTTON_PIN)==LOW);
        return;
      }
      delay(50);
    }
    tft.fillRect(0,SCREEN_H-14,SCREEN_W,14,col24(C_BG));
    drawPollingDot(false);
  }
}

void checkSerial() {
  if (!Serial.available()) return;
  String cmd=Serial.readStringUntil('\n'); cmd.trim(); cmd.toUpperCase();
  if (cmd=="NEWID") {
    generateNewKitID();
  } else if (cmd=="STATUS") {
    xSemaphoreTake(xMutexUID,portMAX_DELAY);
    String uid=userID;
    xSemaphoreGive(xMutexUID);
    Serial.println("KIT ID  : "+KIT_ID);
    Serial.println("User ID : "+(uid!=""?uid:"Belum ditemukan"));
    Serial.println("Waktu   : "+ntp.getFirestore());
    Serial.println("NTP OK  : "+String(ntp.isReady()?"Ya":"Belum"));
    Serial.printf("Free heap: %lu bytes\n",(unsigned long)esp_get_free_heap_size());
    // Stack watermark for Firebase task (bytes remaining)
    if (hTaskFirebase)
      Serial.printf("Firebase task stack remaining: %d bytes\n",
                    (int)uxTaskGetStackHighWaterMark(hTaskFirebase));
  }
}


// ════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN,INPUT_PULLUP);
  pinMode(analogInPin,INPUT);
  analogSetAttenuation(ADC_11db);

  rid.begin();
  KIT_ID=String(rid.getID());

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(col24(C_BG));
  drawStatusBar();

  // ── Create RTOS primitives BEFORE anything that uses them ──────
  xSemKirim = xSemaphoreCreateBinary();
  xMutexUID = xSemaphoreCreateMutex();
  // ───────────────────────────────────────────────────────────────

  // WiFi
  tft.setTextColor(TFT_WHITE,col24(C_BG)); tft.setTextSize(2);
  tft.setCursor(14,60); tft.print("Connecting WiFi...");
  delay(5000);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  unsigned long wifiStart=millis();
  while (WiFi.status()!=WL_CONNECTED) {
    delay(300);
    if (millis()-wifiStart>WIFI_CONNECT_TIMEOUT) ESP.restart();
  }
  tft.fillRect(14,56,SCREEN_W-28,24,col24(C_BG));
  tft.setTextColor(TFT_GREEN,col24(C_BG)); tft.setCursor(14,60); tft.print("WiFi Connected!");

  // ESP-NOW
  if (esp_now_init()!=ESP_OK) {
    tft.fillRect(14,86,SCREEN_W-28,24,col24(C_BG));
    tft.setTextColor(TFT_RED,col24(C_BG)); tft.setCursor(14,90); tft.print("ESP-NOW Gagal!");
  } else {
    esp_now_register_recv_cb([](const esp_now_recv_info_t *info,
                                const uint8_t *data, int len){
      onDataReceived(info->src_addr,data,len);
    });
    tft.fillRect(14,86,SCREEN_W-28,24,col24(C_BG));
    tft.setTextColor(TFT_GREEN,col24(C_BG)); tft.setCursor(14,90); tft.print("ESP-NOW Siap!");
    Serial.println("ESP-NOW siap. CH: "+String(WiFi.channel()));
  }
  delay(800);

  // NTP
  tft.setTextColor(TFT_WHITE,col24(C_BG)); tft.setCursor(14,90); tft.print("Syncing NTP...");
  ntp.begin();
  tft.fillRect(14,86,SCREEN_W-28,24,col24(C_BG));
  tft.setTextColor(ntp.isReady()?TFT_GREEN:TFT_YELLOW,col24(C_BG));
  tft.setCursor(14,90); tft.print(ntp.isReady()?"NTP OK!":"NTP Gagal!");

  // Firebase
  tft.setTextColor(TFT_WHITE,col24(C_BG)); tft.setCursor(14,120); tft.print("Connecting Firebase...");
  config.api_key              =API_KEY;
  config.database_url         =DATABASE_URL;
  auth.user.email             =USER_EMAIL;
  auth.user.password          =USER_PASSWORD;
  config.token_status_callback=tokenStatusCallback;
  Firebase.reconnectNetwork(true);
  fbdoQuery.setBSSLBufferSize(4096,1024);
  fbdoWrite.setBSSLBufferSize(4096,1024);
  fbdoGet.setBSSLBufferSize(4096,1024);
  Firebase.begin(&config,&auth);
  unsigned long fbStart=millis();
  while (!Firebase.ready()) {
    delay(500);
    if (millis()-fbStart>FIREBASE_READY_TIMEOUT) ESP.restart();
  }
  tft.fillRect(14,116,SCREEN_W-28,24,col24(C_BG));
  tft.setTextColor(TFT_GREEN,col24(C_BG)); tft.setCursor(14,120); tft.print("Firebase Ready!");

  // Initial userID fetch — still on Core1 here, Firebase task not started yet
  tft.setTextColor(TFT_WHITE,col24(C_BG)); tft.setCursor(14,150); tft.print("Mencari user...");
  fetchUserID();
  tft.fillRect(14,146,SCREEN_W-28,24,col24(C_BG)); tft.setCursor(14,150);
  xSemaphoreTake(xMutexUID,portMAX_DELAY);
  bool gotUID=(userID!="");
  xSemaphoreGive(xMutexUID);
  tft.setTextColor(gotUID?TFT_GREEN:TFT_RED,col24(C_BG));
  tft.print(gotUID?"User ditemukan!":"User tidak ditemukan!");

  Serial.printf("Free heap after init: %lu\n",(unsigned long)esp_get_free_heap_size());

  // ── Launch Firebase task on Core0 ──────────────────────────────
  // Stack 12 KB: Firebase + mbedTLS are heavy; bump to 16384 if you
  // see "stack overflow" panic or watermark drops below ~512 bytes
  xTaskCreatePinnedToCore(
    taskFirebase,    // function
    "TaskFirebase",  // name (shows in Serial debug)
    12288,           // stack bytes
    NULL,            // no parameters
    1,               // priority (1 = same as Arduino loop)
    &hTaskFirebase,  // handle (lets us check stack watermark)
    0                // Core 0
  );
  // ───────────────────────────────────────────────────────────────

  delay(1000);
  drawAll();
}


// ════════════════════════════════════════════════════════════════════
//  LOOP  — runs on Core1 automatically, UI only, never calls Firebase
// ════════════════════════════════════════════════════════════════════
void loop() {
  static unsigned long lastBatMs   = 0;
  static String        lastUID     = "";
  static unsigned long resultTimer = 0;

  checkButton();
  checkSerial();

  // ── Battery (Core1 ADC, safe) ────────────────────────────────────
  if (millis()-lastBatMs >= 5000) {
    lastBatMs      = millis();
    sensorValue    = readADC(analogInPin);
    voltage        = (((sensorValue*vref)/4095.f)*2+calibration);
    bat_percentage = constrain((int)mapfloat(voltage,2.5f,4.4f,0,100),0,100);
    if (appState==S_IDLE) drawStatusBar();
  }

  // ── Kit bubble refresh when taskFirebase updates userID ──────────
  xSemaphoreTake(xMutexUID,portMAX_DELAY);
  String currUID=userID;
  xSemaphoreGive(xMutexUID);
  if (currUID!=lastUID) {
    lastUID=currUID;
    if (appState==S_IDLE) drawKitBubble();
  }

  // ── New data arrived from C3 via ESP-NOW ─────────────────────────
  if (adaDataBaru && appState==S_IDLE) {
    adaDataBaru   = false;
    displayBPM    = dataTerima.bpm;
    displayOksi   = dataTerima.saturasi;
    displayStatus = String(dataTerima.status);
    displayWaktu  = ntp.getFirestore();
    refreshCard();
    drawInfoBar();
    delay(300);
    appState=S_KONFIRMASI;
    drawKonfirmasi();
  }

  // ── Confirmation touch (single clean handler) ─────────────────────
  if (appState==S_KONFIRMASI) {
    int tx,ty;
    if (getTouchPoint(tx,ty)) {
      Serial.printf("[Touch] x=%d y=%d\n",tx,ty);

      if (tx>=BTN_YA_X && tx<=BTN_YA_X+BTN_YA_W &&
          ty>=BTN_YA_Y && ty<=BTN_YA_Y+BTN_YA_H) {

        // Pack payload snapshot — do this BEFORE xSemaphoreGive (barrier)
        fbPayload.bpm      = displayBPM;
        fbPayload.saturasi = (int)displayOksi;
        strlcpy(fbPayload.status, displayStatus.c_str(), sizeof(fbPayload.status));
        strlcpy(fbPayload.waktu,  ntp.getFirestoreTimestamp().c_str(), sizeof(fbPayload.waktu));
        xSemaphoreTake(xMutexUID,portMAX_DELAY);
        strlcpy(fbPayload.uid, userID.c_str(), sizeof(fbPayload.uid));
        xSemaphoreGive(xMutexUID);

        // Show sending screen, then wake Firebase task
        appState=S_SENDING;
        drawSendingScreen();
        xSemaphoreGive(xSemKirim);  // ← taskFirebase unblocks here
      }

      if (tx>=BTN_TDK_X && tx<=BTN_TDK_X+BTN_TDK_W &&
          ty>=BTN_TDK_Y && ty<=BTN_TDK_Y+BTN_TDK_H) {
        appState=S_IDLE;
        drawAll();
      }
    }
  }

  // ── Non-blocking Firebase result check ───────────────────────────
  if (appState==S_SENDING && fbDone) {
    fbDone      = false;
    resultTimer = millis();
    drawResultScreen(fbOK,fbErr);
    appState    = S_RESULT;
  }

  // ── Auto-return to dashboard after 2 s ───────────────────────────
  if (appState==S_RESULT && millis()-resultTimer>=2000) {
    appState=S_IDLE;
    drawAll();
  }

  delay(100);
}