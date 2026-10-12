#ifndef RANDOM_ID_H
#define RANDOM_ID_H

#include <Arduino.h>
#include <EEPROM.h>

class RandomID {
  public:
    RandomID(int eepromAddr = 0);
    void    begin();
    int     getID();
    int     generateNew();
    bool    isNew();
    void    reset();
    int     getCount();

  private:
    int      _addr;
    int      _addrFlag;
    int      _addrCount;
    int16_t  _id;      // ← int16_t = 2 bytes
    int16_t  _count;   // ← int16_t = 2 bytes
    bool     _isNew;

    void _loadFromEEPROM();
    void _saveToEEPROM();
};

#endif