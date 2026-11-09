/*
 * platform.h - board-specific code for geotrio2mqtt
 * -------------------------------------------------------------------------
 * Everything that differs between ESP8266 and ESP32 lives in this file.
 * The main sketch (geotrio2mqtt.ino) is shared and doesn't need editing
 * to change boards - the right section below is picked automatically
 * from the board selected in Tools > Board.
 *
 * To add another board, add a new #elif section providing the same
 * names: WebServerT, MeterSerial, LOG and the platform*() functions.
 * -------------------------------------------------------------------------
 */
#pragma once

#if defined(ESP8266)
// ===================== ESP8266 (e.g. NodeMCU) =====================
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
typedef ESP8266WebServer WebServerT;

// Meter input: UART0 RX, swapped onto GPIO13 (NodeMCU D7).
// Wire Trio II J8 (TX) -> D7.
#define MeterSerial Serial
// Debug log: UART1 is TX-only on GPIO2 (NodeMCU D4), because UART0 is busy
// reading the meter. Connect a USB-serial adaptor's RX to D4 to see it.
#define LOG Serial1

inline void platformBeginLog() { Serial1.begin(115200); }
inline void platformBeginMeter(long baud) {
  Serial.setRxBufferSize(1024);   // must be before begin()
  Serial.begin(baud);
  Serial.swap();                  // UART0 RX -> GPIO13 (D7)
}
inline String platformMeterPin() { return "GPIO13 (D7)"; }
inline bool platformBeginFS() {
  if(LittleFS.begin()) return true;
  LittleFS.format();              // first boot: format, then mount
  return LittleFS.begin();
}
inline uint32_t platformChipId() { return ESP.getChipId(); }
inline uint32_t platformHeapTotal() { return ESP.getFreeHeap()+ESP.getMaxFreeBlockSize(); }
inline void platformSetHostname(const char*n) { WiFi.hostname(n); }   // before WiFi.begin()
inline void platformLoop() { MDNS.update(); }                          // ESP8266 mDNS needs polling
inline void platformConfigTime(const char*tz) { configTime(tz,"pool.ntp.org","time.google.com"); }
// file system space, and listing the files in its root folder (names without a leading '/')
inline void platformFSInfo(size_t&used,size_t&total) {
  FSInfo i;
  LittleFS.info(i);
  used=i.usedBytes;
  total=i.totalBytes;
}
template<typename F> void platformListFiles(F fn) {
  Dir d=LittleFS.openDir("/");
  while(d.next()) fn(d.fileName());
}

#elif defined(ESP32)
// ===================== ESP32 (e.g. Seeed XIAO ESP32S3) =====================
// For boards with native USB (ESP32-S3/C3): Tools > USB CDC On Boot > Enabled,
// so debug logs appear in the Serial Monitor over USB.
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
typedef WebServer WebServerT;

// Meter input pin. ESP32 UART pins can be mapped to any free GPIO.
// Default GPIO7 = Seeed XIAO ESP32S3 pin D8. Wire Trio II J8 (TX) here.
#ifndef METER_RX_PIN
#define METER_RX_PIN 7
#endif
#define MeterSerial Serial1
// Debug log: USB serial
#define LOG Serial

inline void platformBeginLog() { Serial.begin(115200); }
inline void platformBeginMeter(long baud) {
  Serial1.setRxBufferSize(1024);  // must be before begin()
  Serial1.begin(baud,SERIAL_8N1,METER_RX_PIN,-1);   // RX only
}
inline String platformMeterPin() { return "GPIO"+String(METER_RX_PIN); }
inline bool platformBeginFS() { return LittleFS.begin(true); }   // true = format if mount fails
// ESP32 has no getChipId(); use part of the factory MAC instead
inline uint32_t platformChipId() { return (uint32_t)(ESP.getEfuseMac()>>24); }
inline uint32_t platformHeapTotal() { return ESP.getHeapSize(); }
inline void platformSetHostname(const char*n) { WiFi.setHostname(n); }   // before WiFi.begin()
inline void platformLoop() {}
inline void platformConfigTime(const char*tz) { configTzTime(tz,"pool.ntp.org","time.google.com"); }
// file system space, and listing the files in its root folder (names without a leading '/')
inline void platformFSInfo(size_t&used,size_t&total) {
  used=LittleFS.usedBytes();
  total=LittleFS.totalBytes();
}
template<typename F> void platformListFiles(F fn) {
  File root=LittleFS.open("/");
  if(!root) return;
  for(File f=root.openNextFile();f;f=root.openNextFile()) {
    String n=f.name();
    f.close();
    if(n.startsWith("/")) n=n.substring(1);
    fn(n);
  }
}

#else
#error "geotrio2mqtt supports ESP8266 and ESP32 boards - see platform.h"
#endif
