#include "RandomID.h"

#define MAGIC_FLAG  42
#define EEPROM_SIZE 6

RandomID::RandomID(int eepromAddr) {
  _addr      = eepromAddr;
  _addrFlag  = eepromAddr + 2;
  _addrCount = eepromAddr + 4;
  _id        = 0;
  _count     = 0;
  _isNew     = false;
}

void RandomID::begin() {
  EEPROM.begin(EEPROM_SIZE);

  // ← TAMBAH INI UNTUK DEBUG
  int16_t flag, id, count;
  EEPROM.get(_addr,      id);
  EEPROM.get(_addrFlag,  flag);
  EEPROM.get(_addrCount, count);

  Serial.println("=== EEPROM DEBUG ===");
  Serial.println("Addr ID    : " + String(_addr));
  Serial.println("Addr Flag  : " + String(_addrFlag));
  Serial.println("Addr Count : " + String(_addrCount));
  Serial.println("ID terbaca : " + String(id));
  Serial.println("Flag terbaca: " + String(flag));
  Serial.println("Count terbaca: " + String(count));
  Serial.println("Magic Flag : " + String(MAGIC_FLAG));
  Serial.println("Flag match?: " + String(flag == MAGIC_FLAG ? "YA ✓" : "TIDAK ✗"));
  Serial.println("====================");

  if (flag != MAGIC_FLAG) {
    _isNew = true;
    generateNew();
  } else {
    _isNew = false;
    _loadFromEEPROM();
  }
}

int RandomID::getID() {
  return _id;
}

int RandomID::generateNew() {
  randomSeed(esp_random());
  _id = random(1000, 9999);
  _count++;
  _isNew = true;
  _saveToEEPROM();
  return _id;
}

bool RandomID::isNew() {
  return _isNew;
}

void RandomID::reset() {
  int flag = 0;
  int count = 0;
  int id = 0;
  EEPROM.put(_addr, id);
  EEPROM.put(_addrFlag, flag);
  EEPROM.put(_addrCount, count);
  EEPROM.commit();
  _id    = 0;
  _count = 0;
  _isNew = false;
}

int RandomID::getCount() {
  return _count;
}

void RandomID::_loadFromEEPROM() {
  EEPROM.get(_addr, _id);
  EEPROM.get(_addrCount, _count);
}

void RandomID::_saveToEEPROM() {
  int flag = MAGIC_FLAG;
  EEPROM.put(_addr, _id);
  EEPROM.put(_addrFlag, flag);
  EEPROM.put(_addrCount, _count);
  EEPROM.commit();
}