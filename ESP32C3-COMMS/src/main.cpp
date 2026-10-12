#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include "max30102.h"
#include "algorithm_by_RF.h"
#include "esp_wifi.h"
#include "secrets.h"   // WIFI_SSID

// MAC Address ESP32 S3
uint8_t s3Address[] = {0xF0, 0x9E, 0x9E, 0x20, 0xF4, 0x3C};

// ── Struktur data kirim ──────────────────────────────────────────
typedef struct {
  int   bpm;
  float saturasi;
  char  status[20];
  bool  dataReady;
} SensorData;

SensorData dataKirim;

// ── Konfigurasi Sensor ───────────────────────────────────────────
#define I2C_SDA 8
#define I2C_SCL 9

#define REG_INTR_ENABLE_1  0x02
#define REG_INTR_ENABLE_2  0x03
#define REG_FIFO_WR_PTR    0x04
#define REG_OVF_COUNTER    0x05
#define REG_FIFO_RD_PTR    0x06
#define REG_FIFO_CONFIG    0x08
#define REG_MODE_CONFIG    0x09
#define REG_SPO2_CONFIG    0x0A
#define REG_LED1_PA        0x0C
#define REG_LED2_PA        0x0D
#define REG_PILOT_PA       0x10
#define REG_PART_ID        0xFF

#define BUFFER_SIZE        100
#define FINGER_THRESHOLD   50000
#define SLIDE_STEP         10
#define SMOOTH_ALPHA       0.3f
#define MIN_CORREL_QUALITY 0.78f
#define SPO2_MIN_VALID     70.0f
#define SPO2_MAX_VALID     100.0f

// Interval kirim ESP-NOW (ms) — kirim saat data stabil
#define KIRIM_INTERVAL 10000

uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];
int      bufferLength = 0;

// ── State BPM ───────────────────────────────────────────────────
float    bpmSmoothed    = 0.0f;
int      newSamples     = 0;
int32_t  lastRawBpm     = -1;
float    lastPreciseBpm = -1.0f;
bool     firstReading   = true;
float    rawHrBuf[3]    = { -1, -1, -1 };
int      rawHrCount     = 0;
int      rawHrIdx       = 0;

// ── State SpO2 ───────────────────────────────────────────────────
float    spo2Smoothed     = 0.0f;
bool     firstSpo2Reading = true;
float    lastRawSpo2      = -1.0f;
float    rawSpo2Buf[3]    = { -1, -1, -1 };
int      rawSpo2Count     = 0;
int      rawSpo2Idx       = 0;

int           fingerAbsentCount = 0;
unsigned long lastKirimTime     = 0;
bool          sudahKirim        = false; // flag: sudah kirim sejak jari ditempel

// ── Helper ───────────────────────────────────────────────────────
float pushAndGetMedian3(float *buf, int *count, int *idx, float val) {
  buf[*idx] = val;
  *idx = (*idx + 1) % 3;
  if (*count < 3) (*count)++;
  if (*count == 1) return buf[0];
  if (*count == 2) return (buf[0] + buf[1]) / 2.0f;
  float a = buf[0], b = buf[1], c = buf[2];
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
  return b;
}

String getStatusSaturasi(float spo2) {
  if (spo2 <= 0)  return "Tidak ada data";
  if (spo2 >= 95) return "Normal";
  if (spo2 >= 90) return "Rendah";
  return                  "Kritis";
}

bool initSensor() {
  Wire.beginTransmission(0x57);
  if (Wire.endTransmission() != 0) {
    Serial.println("[MAX30102] ERROR: Sensor tidak ditemukan!");
    return false;
  }
  maxim_max30102_write_reg(REG_MODE_CONFIG, 0x40);
  delay(100);
  uint8_t modeReg = 0xFF;
  for (int i = 0; i < 50; i++) {
    maxim_max30102_read_reg(REG_MODE_CONFIG, &modeReg);
    if ((modeReg & 0x40) == 0) break;
    delay(10);
  }
  uint8_t dummy;
  maxim_max30102_read_reg(0x00, &dummy);
  maxim_max30102_read_reg(0x01, &dummy);
  maxim_max30102_write_reg(REG_INTR_ENABLE_1, 0xC0);
  maxim_max30102_write_reg(REG_INTR_ENABLE_2, 0x00);
  maxim_max30102_write_reg(REG_FIFO_WR_PTR,   0x00);
  maxim_max30102_write_reg(REG_OVF_COUNTER,   0x00);
  maxim_max30102_write_reg(REG_FIFO_RD_PTR,   0x00);
  maxim_max30102_write_reg(REG_FIFO_CONFIG,   0x5F);
  maxim_max30102_write_reg(REG_MODE_CONFIG,   0x03);
  maxim_max30102_write_reg(REG_SPO2_CONFIG,   0x67);
  maxim_max30102_write_reg(REG_LED1_PA,       0x5F);
  maxim_max30102_write_reg(REG_LED2_PA,       0xAF);
  maxim_max30102_write_reg(REG_PILOT_PA,      0x7F);
  Serial.println("[MAX30102] Inisialisasi berhasil.");
  return true;
}

void resetState() {
  bufferLength   = 0;
  newSamples     = 0;
  lastRawBpm     = -1;
  lastPreciseBpm = -1.0f;
  bpmSmoothed    = 0.0f;
  firstReading   = true;
  rawHrCount = 0; rawHrIdx = 0;
  spo2Smoothed     = 0.0f;
  firstSpo2Reading = true;
  lastRawSpo2      = -1.0f;
  rawSpo2Count = 0; rawSpo2Idx = 0;
  sudahKirim   = false;
  lastKirimTime = 0;
}

// ── ESP-NOW callback ─────────────────────────────────────────────
void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
  Serial.println(status == ESP_NOW_SEND_SUCCESS
    ? "ESP-NOW: Berhasil kirim ke S3!"
    : "ESP-NOW: Gagal kirim ke S3!");
}

// Scan channel router — dari referensi Random Nerd + GitHub m1cr0lab
int32_t getWiFiChannel(const char *ssid) {
  int32_t n = WiFi.scanNetworks();
  for (uint8_t i = 0; i < n; i++) {
    if (!strcmp(ssid, WiFi.SSID(i).c_str())) {
      return WiFi.channel(i);
    }
  }
  return 0; // tidak ketemu
}

void setup() {
  Serial.begin(115200);

  setCpuFrequencyMhz(80);
  btStop();
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

    // ✅ Scan dan ikuti channel router (sama dengan S3)
  int32_t ch = getWiFiChannel(WIFI_SSID);
  if (ch > 0) {
    Serial.printf("[C3] Channel router: %d\n", ch);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_promiscuous(false);
  } else {
    Serial.println("[C3] Router tidak ditemukan, pakai channel default");
  }

  // ✅ Fix TX power — 80 = 20dBm (bukan 20 = 5dBm!)
  esp_wifi_set_max_tx_power(80);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init gagal!");
    return;
  }
  esp_now_register_send_cb(onDataSent);

  // Tambah setelah esp_now_init() berhasil di C3
  Serial.printf("[DEBUG] C3 Channel: %d | MAC S3 target: %02X:%02X:%02X:%02X:%02X:%02X\n",
  WiFi.channel(),
  s3Address[0], s3Address[1], s3Address[2],
  s3Address[3], s3Address[4], s3Address[5]);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, s3Address, 6);
  peerInfo.channel = 0;       // ✅ 0 = auto ikuti channel WiFi aktif S3
  peerInfo.encrypt = false;
  peerInfo.ifidx   = WIFI_IF_STA;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Add peer gagal!");
    return;
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  initSensor();

  Serial.println("Tempelkan jari...");
}

void loop() {
  // ── Watchdog I2C ────────────────────────────────────────────────
  static unsigned long lastI2CCheck = 0;
  if (millis() - lastI2CCheck > 1000) {
    lastI2CCheck = millis();
    Wire.beginTransmission(0x57);
    if (Wire.endTransmission() != 0) {
      Serial.println("[I2C] Bus hang, reinisialisasi...");
      Wire.begin(I2C_SDA, I2C_SCL);
      Wire.setClock(400000);
      initSensor();
      resetState();
      return;
    }
  }

  // ── Baca FIFO ───────────────────────────────────────────────────
  uint8_t wrPtr = 0, rdPtr = 0;
  maxim_max30102_read_reg(REG_FIFO_WR_PTR, &wrPtr);
  maxim_max30102_read_reg(REG_FIFO_RD_PTR, &rdPtr);
  int numAvailable = (wrPtr - rdPtr) & 31;
  if (numAvailable <= 0) return;

  for (int i = 0; i < numAvailable; i++) {
    uint32_t rawRed = 0, rawIr = 0;
    if (!maxim_max30102_read_fifo(&rawRed, &rawIr)) continue;

    if (rawIr < FINGER_THRESHOLD) {
      fingerAbsentCount++;
      if (fingerAbsentCount > 50) {
        if (fingerAbsentCount == 51) {
          Serial.println("[STATUS] Jari tidak terdeteksi...");
          resetState();
        }
      }
      continue;
    }
    fingerAbsentCount = 0;

    if (bufferLength < BUFFER_SIZE) {
      irBuffer[bufferLength]  = rawIr;
      redBuffer[bufferLength] = rawRed;
      bufferLength++;
    } else {
      memmove(irBuffer,  irBuffer  + 1, (BUFFER_SIZE-1)*sizeof(uint32_t));
      memmove(redBuffer, redBuffer + 1, (BUFFER_SIZE-1)*sizeof(uint32_t));
      irBuffer[BUFFER_SIZE-1]  = rawIr;
      redBuffer[BUFFER_SIZE-1] = rawRed;
    }
    newSamples++;
  }

  // ── Hitung BPM + SpO2 ───────────────────────────────────────────
  if (bufferLength == BUFFER_SIZE && newSamples >= SLIDE_STEP) {
    newSamples = 0;

    float   n_spo2       = -1.0f;
    int8_t  spo2_valid   = 0;
    int32_t n_hr         = -1;
    int8_t  hr_valid     = 0;
    float   ratio        = 0.0f;
    float   correl       = 0.0f;
    float   n_hr_precise = -1.0f;

    rf_heart_rate_and_oxygen_saturation(
      irBuffer, BUFFER_SIZE, redBuffer,
      &n_spo2, &spo2_valid,
      &n_hr,   &hr_valid,
      &ratio,  &correl,
      &n_hr_precise
    );

    bool hrOk = hr_valid && (correl >= MIN_CORREL_QUALITY)
                && n_hr > 30 && n_hr < 220;

    if (hrOk) {
      float filtered = pushAndGetMedian3(rawHrBuf, &rawHrCount, &rawHrIdx, n_hr_precise);
      if (firstReading) { bpmSmoothed = filtered; firstReading = false; }
      else bpmSmoothed = SMOOTH_ALPHA * filtered + (1.0f - SMOOTH_ALPHA) * bpmSmoothed;
      lastRawBpm     = n_hr;
      lastPreciseBpm = n_hr_precise;
    }

    bool spo2Ok = hrOk && spo2_valid
                  && n_spo2 >= SPO2_MIN_VALID && n_spo2 <= SPO2_MAX_VALID;

    if (spo2Ok) {
      float filtered = pushAndGetMedian3(rawSpo2Buf, &rawSpo2Count, &rawSpo2Idx, n_spo2);
      if (firstSpo2Reading) { spo2Smoothed = filtered; firstSpo2Reading = false; }
      else spo2Smoothed = SMOOTH_ALPHA * filtered + (1.0f - SMOOTH_ALPHA) * spo2Smoothed;
      lastRawSpo2 = n_spo2;
    }
  }

  // ── Print status ─────────────────────────────────────────────────
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 1500) {
    lastPrint = millis();
    if (bufferLength < BUFFER_SIZE) {
      Serial.printf("[STATUS] Mengumpulkan data... (%d/%d)\n", bufferLength, BUFFER_SIZE);
    } else if (lastRawBpm < 0) {
      Serial.println("[STATUS] Sinyal tidak stabil...");
    } else {
      Serial.printf("[BPM] %.1f  [SpO2] %.1f%%\n", bpmSmoothed, spo2Smoothed);
    }
  }

  // ── Kirim ke S3 via ESP-NOW ──────────────────────────────────────
  // Kirim saat data valid + stabil + belum kirim dalam interval ini
  bool dataValid = (lastRawBpm > 0) && (lastRawSpo2 > 0) &&
                   (bpmSmoothed > 30) && (spo2Smoothed >= SPO2_MIN_VALID);

  if (dataValid && (millis() - lastKirimTime >= KIRIM_INTERVAL)) {
    lastKirimTime = millis();

    dataKirim.bpm      = (int)bpmSmoothed;
    dataKirim.saturasi = spo2Smoothed;
    String st = getStatusSaturasi(spo2Smoothed);
    strncpy(dataKirim.status, st.c_str(), sizeof(dataKirim.status) - 1);
    dataKirim.dataReady = true;

    Serial.printf("\n[KIRIM] BPM: %d | SpO2: %.1f%% | Status: %s\n",
      dataKirim.bpm, dataKirim.saturasi, dataKirim.status);

    esp_now_send(s3Address, (uint8_t*)&dataKirim, sizeof(dataKirim));
  }

  delay(100);

}