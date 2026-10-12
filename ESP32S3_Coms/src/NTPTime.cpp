#include "NTPTime.h"

NTPTime::NTPTime(int timezone) {
  _timezone = timezone;
  _ready    = false;
}

void NTPTime::begin() {
  long gmtOffset = _timezone * 3600L;
  configTime(gmtOffset, 0, "pool.ntp.org", "time.nist.gov");

  Serial.print("Syncing NTP");
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 20) {
    Serial.print(".");
    delay(500);
    retry++;
  }

  if (retry < 20) {
    _ready = true;
    Serial.println(" OK!");
    Serial.println("Waktu: " + getFirestore());
  } else {
    _ready = false;
    Serial.println(" GAGAL! Cek koneksi internet.");
  }
}

bool NTPTime::isReady() {
  return _ready;
}

void NTPTime::setTimezone(int timezone) {
  _timezone = timezone;
  long gmtOffset = timezone * 3600L;
  configTime(gmtOffset, 0, "pool.ntp.org");
}

String NTPTime::getTimezoneLabel() {
  return "UTC+" + String(_timezone);
}

bool NTPTime::_getTime(struct tm &timeinfo) {
  return getLocalTime(&timeinfo);
}

// ── Format: June 10, 2026 at 1:39:24 AM UTC+7 ──
String NTPTime::getFirestore() {
  struct tm t;
  if (!_getTime(t)) return "Waktu belum tersync";

  int jam24  = t.tm_hour;
  int menit  = t.tm_min;
  int detik  = t.tm_sec;
  int hari   = t.tm_mday;
  int bln    = t.tm_mon;         // 0-11
  int tahun  = t.tm_year + 1900;

  // Konversi 24h → 12h + AM/PM
  String ampm  = (jam24 < 12) ? "AM" : "PM";
  int    jam12 = jam24 % 12;
  if (jam12 == 0) jam12 = 12;

  char buf[64];
  snprintf(buf, sizeof(buf),
    "%d-%d-%d %02d:%02d",
    tahun, bln+1, hari,
    jam12, menit
  );

  return String(buf);
}

String NTPTime::getFirestoreTimestamp() {
  struct tm t;
  if (!_getTime(t)) return "";

  // Gunakan jam24 langsung dari struct tm
  int jam24 = t.tm_hour;   // ← 0-23, BUKAN jam12!
  int menit  = t.tm_min;
  int detik  = t.tm_sec;
  int hari   = t.tm_mday;
  int bln    = t.tm_mon;         // 0-11
  int tahun  = t.tm_year + 1900;

  // Konversi WIB → UTC (kurangi 7 jam)
  int utcJam = jam24 - _timezone;
  int utcHari = hari;
  int utcBln  = bln + 1;
  int utcTahun = tahun;

  // Handle kalau jam jadi negatif (ganti hari)
  if (utcJam < 0) {
    utcJam += 24;
    utcHari -= 1;
    if (utcHari < 1) {
      utcBln -= 1;
      if (utcBln < 1) { utcBln = 12; utcTahun -= 1; }
      // Hari terakhir bulan sebelumnya
      int daysInMonth[] = {31,28,31,30,31,30,31,31,30,31,30,31};
      if (utcBln == 2 && (utcTahun%4==0)) daysInMonth[1] = 29;
      utcHari = daysInMonth[utcBln - 1];
    }
  }

  char buf[30];
  snprintf(buf, sizeof(buf),
    "%04d-%02d-%02dT%02d:%02d:%02dZ",  // ← zero-padded semua
    utcTahun, utcBln, utcHari,
    utcJam, menit, detik               // ← jam24 bukan jam12!
  );
  return String(buf);
}

// ── Getter individual ──
int NTPTime::getHour()   { struct tm t; _getTime(t); return t.tm_hour; }
int NTPTime::getMinute() { struct tm t; _getTime(t); return t.tm_min;  }
int NTPTime::getSecond() { struct tm t; _getTime(t); return t.tm_sec;  }
int NTPTime::getDay()    { struct tm t; _getTime(t); return t.tm_mday; }
int NTPTime::getMonth()  { struct tm t; _getTime(t); return t.tm_mon + 1; }
int NTPTime::getYear()   { struct tm t; _getTime(t); return t.tm_year + 1900; }