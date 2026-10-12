#ifndef NTP_TIME_H
#define NTP_TIME_H

#include <Arduino.h>
#include <time.h>

// Konstanta timezone
#define TZ_WIB  7   // UTC+7
#define TZ_WITA 8   // UTC+8
#define TZ_WIT  9   // UTC+9
#define TZ_UTC  0   // UTC+0

class NTPTime {
  public:
    NTPTime(int timezone = TZ_WIB);   // default UTC

    void    begin();                   // sync NTP, panggil setelah WiFi connect
    bool    isReady();                 // cek apakah waktu sudah tersync
    String  getFirestore();            // "June 10, 2026 at 1:39:24 AM UTC+7"
    String  getTimezoneLabel();        // "UTC+7", "UTC+8", dst
    int     getHour();                 // 0-23
    int     getMinute();               // 0-59
    int     getSecond();               // 0-59
    int     getDay();                  // 1-31
    int     getMonth();                // 1-12
    int     getYear();                 // 2024, 2025, dst
    void    setTimezone(int timezone); // ganti timezone saat runtime
    String getFirestoreTimestamp();


  private:
    int     _timezone;
    bool    _ready;

    const char* _monthName[12] = {
      "January", "February", "March",    "April",
      "May",     "June",     "July",     "August",
      "September","October", "November", "December"
    };

    bool _getTime(struct tm &timeinfo);
};

#endif