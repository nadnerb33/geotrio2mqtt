/*
 * geotrio2mqtt.ino - https://github.com/nadnerb33/geotrio2mqtt/
 * -------------------------------------------------------------------------
 * Geo Trio II smart meter display -> MQTT
 *
 * HARDWARE REQUIREMENTS:
 * - Geo Trio II in-home display (with or without stock wifi module)
 * - ESP8266 (or ESP32 etc)
 * - Soldering iron
 * - wire
 * - (Optional) USB-to-serial adaptor if you're using a bare ESP board or the existing Trio II wifi module (ESP8285)
 *
 * BOARD-SPECIFIC CODE:
 * This file is shared by ESP8266 and ESP32 boards. The board-specific parts (libraries,
 * meter RX pin, debug log port) are in platform.h, picked automatically from the board
 * selected in Tools > Board.
 * 
 * SOFTWARE REQUIREMENTS:
 *  - Arduino IDE
 *  - ESP8266WiFi
 *  - ESP8266WebServer
 *  - DNSServer
 *  - LittleFS
 *  - ArduinoJson (>=7)
 *  - PubSubClient (>=2.8)
 *  (ESP32: the WiFi, WebServer and ESPmDNS libraries from the ESP32 board package are used instead)
 *  
 * COMPILING & FLASHING:
 * - Install required packages in Arduino IDE (Tools > Manage Libraries)
 * - Add ESP8266 boards repository:
 *     - Go to: File > Preferences > Additional Boards Manager URLs
 *     - Paste http://arduino.esp8266.com/stable/package_esp8266com_index.json
 *     - (ESP32: paste https://espressif.github.io/arduino-esp32/package_esp32_index.json)
 * - Tools > Board > Board Manager > ESP8266 > Install
 * - Tools > Board > ESP8266 Boards > Then pick your particular variant
 * - (ESP32-S3/C3 boards with native USB: Tools > USB CDC On Boot > Enabled)
 * - Tools > Port > Select Serial port / USB (Make sure your USB serial adaptor is plugged in first)
 * - Configure WiFi SSID & password in .ino file
 * - Sketch > Verify/Compile
 * - Put your ESP into flash mode (depends on board, could be automatic, could have a flash/reset button or ground GPIO0 etc)
 * - Sketch > Upload 
 *
 * WIRING (See images for locations):
 * - Trio II J8 (TX) <> ESP RX (Pin D7/GPIO13 for Nodemcu)
 *   (Seeed XIAO ESP32S3: pin D8/GPIO7 - other ESP32 pins can be set with METER_RX_PIN in platform.h)
 * - GND <> GND
 * - 5V <> 5V (or 3V3 <> 3V3 depending on your board)
 * 
 * SETUP:
 * If no WiFi was configured in this script then the device will create a hotspot called geotrio2mqtt.
 * Once connected to it (no password), go to 192.168.4.1 to configure your WiFi SSID/password, MQTT topic etc and then reboot.
 * The ESP should then connect to your WiFi and start publishing MQTT messages.
 * Note: the WiFi SSID, password and topic etc are stored in perisitent memory 
 * meaning once set you can reflash the ESP and it will reconnect to the same WiFi without requiring setup again.
 * The settings page also sets the device's network name (default http://geotrio2mqtt.local) and the
 * full scale of the web page's electricity dial and gas flame.
 *
 * MQTT Topics:
 * - <base>/elec/live_power_w
 * - <base>/elec/meter
 * - <base>/gas/meter
 * - <base>/elec/today_kwh, week_kwh, month_kwh (and the same for gas)
 * - <base>/elec/cost_today, cost_week, cost_month (GBP incl. standing charge, and the same for gas)
 * - <base>/elec/unit_rate, standing_charge (GBP, from the hub's hourly price messages, and the same for gas)
 * - <base>/gas/calorific_value, conversion_factor
 * - <base>/status (online/LWT offline)
 * Note: Elec topics are published every 10 seconds while gas every 2 minutes
 * This is the frequency transmitted by the smart meter itself so can't be changed AFAIK.
 * Also, despite gas readings being published every 2 minutes I believe they only update every 30 minutes.
 *
 * KNOWN ISSUES:
 * I noticed some extremely large erroneous geotrio/gas/meter values. These seemed to occur every few hours but the previous and next values were correct.
 * I recommend setting a filter in HomeAssistant to ignore these values. (I didn't see any erroneous values for the electricity topics but it may be worth apply the same fix to these too)
 * e.g in mqtt.yaml
 *   - sensor:
        - name: "Gas Meter"
          unique_id: gas_meter
          state_topic: "geotrio/gas/meter"
          unit_of_measurement: "m³"
          device_class: gas
          state_class: total_increasing
          value_template: >-
            {% set reading = value | float(none) %}
            {% if reading is not none and 0 <= reading <= 1000000 %}
              {{ reading }}
            {% else %}
              {{ this.state }}
            {% endif %}
 * -------------------------------------------------------------------------
 */
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <time.h>
#include "platform.h"

// ======================= settings you may want to change =======================

// Default network name (changeable on the Settings page): shown in your router
// and reachable as http://<name>.local
#define HOSTNAME "geotrio2mqtt"

// Optional password for the Settings page (changeable on the Settings page). Empty = no
// password. When set, the browser asks for it: user name "admin" and this password.
#define SETTINGS_PASSWORD ""
// Forgotten the Settings password? Set this to 1 and upload: the password is removed at
// start-up (nothing else changes). Then set it back to 0 and upload again.
#define CLEAR_SETTINGS_PASSWORD 0

// Meter scaling. The meter reports its own multiplier/divisor (ZCL attributes
// 0x0301/0x0302) when the Trio II starts up; geotrio2mqtt learns them, saves them
// and uses them from then on. These defaults are used until then.
// If your readings are 10x (or 100x) out, power-cycle the Trio II once while the
// ESP is running so it re-sends its scaling.
#define DEFAULT_METER_MULTIPLIER 1
#define DEFAULT_ELEC_DIVISOR 1000
#define DEFAULT_GAS_DIVISOR 1000
// Gas today/week/month usage (sent by the meter in kWh) is divided by this.
#define GAS_KWH_DIVISOR 1000.0
// Decimal places in the raw cost values. These vary between installations, and the
// Trio II's own format attribute (0x0500) can't be relied on, so geotrio2mqtt works
// them out by comparing "cost today" with usage x unit rate + standing charge, then
// saves the result. These defaults are used until then.
#define DEFAULT_ELEC_COST_DP 5
#define DEFAULT_GAS_COST_DP 5
// A decimal-places setting matches if cost is within this factor of the expected figure
// (must stay below 3.16 so that only one setting can match).
#define COST_MATCH_FACTOR 2.5
// Once checked, the setting only changes if it disagrees for this long (minutes), so
// usage and cost resetting at slightly different times around midnight can't change it.
#define COST_DP_CONFIRM_MIN 30
// Fixed decimal places for raw costs, overriding the automatic check: "" = automatic,
// or "0" to "7". These are the DEFAULTS - they can also be changed on the device's
// Settings page (Meter section) without re-uploading.
#define ELEC_COST_DP_OVERRIDE ""
#define GAS_COST_DP_OVERRIDE ""

// Web page "Energy now" display (kW). These are the DEFAULTS - they can also be
// changed on the device's Settings page (Display section) without re-uploading.
// electricity dial needle reaches the end at this power (linear scale)
#define ELEC_DIAL_MAX_KW 15
// gas flame grows to full size at this power
#define GAS_FLAME_MAX_KW 25

// No change in gas kWh-today for this long = gas off (0 kW on the web page)
#define GAS_IDLE_MS (35UL*60000UL)

// DEBUG: 1 = log every meter frame the code ignores (as hex) to the debug log
// port (see platform.h). Useful for finding new data in the meter's messages.
#define DUMP_UNDECODED 0

// Local time zone (POSIX format), used for the history charts' days and months.
// UK: GMT in winter, BST (+1h) from the last Sunday in March to the last Sunday in October.
#define TIMEZONE "GMT0BST,M3.5.0/1,M10.5.0"
// History for the web page charts (1-minute power per day, daily and monthly kWh/cost) is
// kept in flash, saved this often (minutes). It may use up to HIST_FS_MAX_PCT of the file
// system; beyond that the oldest history files are deleted, leaving room for other things.
#define HIST_SAVE_MIN 30
#define HIST_FS_MAX_PCT 80

// ===============================================================================

#define STR_(x) #x
#define STR(x) STR_(x)

struct Config {
  // Add your wifi details here
  char ssid[33] = "", wpass[65] = "";
  
  // MQTT broker details
  char host[40] = "192.168.0.33", port[6] = "1883", user[32] = "", mpass[32] = "";
  
  // MQTT topic base e.g <geotrio>/elec/live_power_w
  char base[48] = "geotrio";
  
  char baud[8] = "115200";

  // Device network name (letters, numbers, hyphens)
  char name[33] = HOSTNAME;

  // Web page display: full scale of the electricity dial and gas flame (kW)
  char emax[8] = STR(ELEC_DIAL_MAX_KW), gmax[8] = STR(GAS_FLAME_MAX_KW);

  // Fixed decimal places for raw costs (empty = work them out automatically)
  char ecdp[3] = ELEC_COST_DP_OVERRIDE, gcdp[3] = GAS_COST_DP_OVERRIDE;

  // Settings page password (empty = none)
  char apass[33] = SETTINGS_PASSWORD;
} cfg;
const char *CFG="/config.json";

// History record: [elec kWh, gas kWh, elec GBP, gas GBP], NaN = no data. Defined here, before
// the first function, because the Arduino IDE inserts its automatic function prototypes there.
struct HVal { float v[4]; };

WebServerT web(80);
DNSServer dns;
WiFiClient net;
PubSubClient mqtt(net);
bool apMode=false;
char statusTopic[64], nodeId[20];

// ---------- readings ----------
volatile double vElecW=NAN, vElecK=NAN, vGasK=NAN;
// usage in kWh: [0]=elec, [1]=gas; [0]=today, [1]=this week, [2]=this month
double vHist[2][3]={{NAN,NAN,NAN},{NAN,NAN,NAN}};
// cost in GBP incl. standing charge: same layout as vHist
double vCost[2][3]={{NAN,NAN,NAN},{NAN,NAN,NAN}};
// decimal places of raw cost values per fuel, verified against the tariff by checkCostDp()
uint8_t costDp[2]={DEFAULT_ELEC_COST_DP,DEFAULT_GAS_COST_DP};
bool costChecked[2]={false,false};    // true = costDp has been verified against the tariff
int8_t costDpCand[2]={-1,-1};         // a different value the tariff check currently suggests
unsigned long costDpSince[2]={0,0};   // when that suggestion started
int16_t costFmt[2]={-1,-1};           // raw attr 0x0500 as last sent by the meter (diagnostics only)
unsigned long tElecW=0, tElecK=0, tGasK=0, bootMs=0, lastMqtt=0, lastWifi=0;

// Meter scaling per fuel ([0]=elec, [1]=gas): value = raw * mult / div
uint32_t mMult[2]={DEFAULT_METER_MULTIPLIER,DEFAULT_METER_MULTIPLIER};
uint32_t mDiv[2]={DEFAULT_ELEC_DIVISOR,DEFAULT_GAS_DIVISOR};
bool mLearned[2]={false,false};   // true = scaling came from the meter
const char *METERF="/meter.json";
double scale(int fi) { return (double)mMult[fi]/(double)mDiv[fi]; }

// Gas only updates every ~10-30 min, so the last gas values are kept in flash and
// shown straight after a restart. gasDirty = a gas value changed since the last save.
const char *GASF="/gas.json";
bool gasDirty=false, gasRestored=false;
unsigned long lastGasSave=0;
// real time (UTC seconds) the gas meter reading last changed; 0 = unknown. Survives restarts.
uint32_t gasEpoch=0;

// Gas "power now" (kW): the meter doesn't send it, so it's the average since the previous
// change in gas kWh-today (like the Trio display does). tGasChg = millis() of last change.
double vGasP=NAN;
unsigned long tGasChg=0;
// real time (UTC seconds) of that last change, saved with vGasP so after a restart the flame
// shows straight away, goes idle correctly and the next average is measured from it
uint32_t gasPEpoch=0;

// Tariff, from the price messages the hub sends about once an hour ([0]=elec, [1]=gas).
// Saved to flash so it shows straight after a restart.
double vRate[2]={NAN,NAN};       // unit rate, GBP/kWh
double vStanding[2]={NAN,NAN};   // standing charge, GBP/day
double vCV=NAN, vCF=NAN;         // gas calorific value (MJ/m3) and volume correction factor
bool tariffDirty=false;
const char *TARIFFF="/tariff.json";

// diagnostics: raw bytes seen on the meter UART, complete frames, frames we decoded
unsigned long rxBytes=0, rxFrames=0, rxDecoded=0;

// current UTC time in seconds, or 0 if the clock hasn't synced from the internet yet
uint32_t nowEpoch() {
  time_t t=time(nullptr);
  return t>1600000000 ? (uint32_t)t : 0;
}

// a display max setting in kW, falling back to the #define default if blank/invalid
double kwSetting(const char*s,double def) {
  double v=atof(s);
  return (v>0 && v<1000) ? v : def;
}

// seconds since gas kWh-today last changed (this session, or the saved real time after a
// restart); -1 if unknown
long gasChgAge() {
  if(tGasChg) return (long)((millis()-tGasChg)/1000);
  uint32_t now=nowEpoch();
  if(gasPEpoch && now>=gasPEpoch) return (long)(now-gasPEpoch);
  return -1;
}

// gas power now (kW) for the web page and history: 0 if gas kWh hasn't moved for GAS_IDLE_MS
double gasPowerNow() {
  double gp=vGasP;
  long age=gasChgAge();
  if(!isnan(gp) && age>=0 && (unsigned long)age*1000UL>GAS_IDLE_MS) gp=0;
  return gp;
}

// ---------- history for the web page charts ----------
// Stored in LittleFS (up to HIST_FS_MAX_PCT of it - the oldest files are deleted beyond that):
//  /pYYYYMMDD.bin  1-minute samples for one local day: int16 elec W[1440], then int16 gas kW x100[1440]
//  /dYYYY.bin      daily totals for one year: 366 x HVal
//  /mYYYY.bin      monthly totals for one year: 12 x HVal
// HVal = [elec kWh, gas kWh, elec GBP, gas GBP], NaN = no data.
// Today's samples and this year's totals are held in RAM and saved every HIST_SAVE_MIN minutes
// and when the day / year changes. Days are local days (TIMEZONE); "day keys" are days since 1970.
#define HNOVAL INT16_MIN
// (struct HVal is defined near the top of the file, before the first function, so the
// Arduino IDE's automatic function prototypes can use it)
int16_t hDayE[1440], hDayG[1440];  // today's samples, by local minute of the day
uint16_t hToday=0;                 // day key of hDayE/G (0 = not started yet)
bool hDayDirty=false;
HVal hYD[366], hYM[12];            // this year's daily / monthly totals
int hYear=0;                       // year of hYD/hYM (0 = not loaded yet)
bool hYearDirty=false;
uint32_t hMin=0;                   // UTC minute currently being sampled
double hESum=0;                    // elec readings in the current minute (averaged)
uint16_t hECnt=0;
unsigned long lastHistSave=0;
// previous-day / previous-month electricity figures the meter sends when the Trio II starts
// up; held until the clock is synced, then filed under the right days/months
uint64_t pDayE[8], pMonE[13];
double pDayC[8], pMonC[13];
uint16_t pDayEm=0, pMonEm=0, pDayCm=0, pMonCm=0;   // bitmasks of which are waiting

// days since 1970-01-01 for a calendar date, and back again (H. Hinnant's algorithms)
int32_t daysFromCivil(int y,unsigned m,unsigned d) {
  y-=m<=2;
  int era=(y>=0?y:y-399)/400;
  unsigned yoe=(unsigned)(y-era*400);
  unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
  unsigned doe=yoe*365+yoe/4-yoe/100+doy;
  return era*146097+(int32_t)doe-719468;
}
void civilFromDays(int32_t z,int&y,unsigned&m,unsigned&d) {
  z+=719468;
  int era=(z>=0?z:z-146096)/146097;
  unsigned doe=(unsigned)(z-era*146097);
  unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
  unsigned doy=doe-(365*yoe+yoe/4-yoe/100);
  unsigned mp=(5*doy+2)/153;
  d=doy-(153*mp+2)/5+1;
  m=mp<10?mp+3:mp-9;
  y=(int)yoe+era*400+(m<=2);
}
// local time for a UTC time (seconds); false if the clock isn't synced
bool localAt(uint32_t utc,struct tm&tm) {
  if(!utc) return false;
  time_t t=utc;
  localtime_r(&t,&tm);
  return true;
}
uint16_t dayKey(const struct tm&tm) { return (uint16_t)daysFromCivil(tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday); }
uint16_t todayKey() {
  struct tm tm;
  return localAt(nowEpoch(),tm) ? dayKey(tm) : 0;
}
void hvClear(HVal*a,int n) { for(int i=0;i<n;i++) for(int j=0;j<4;j++) a[i].v[j]=NAN; }
String dayFile(uint16_t key) {
  int y; unsigned m,d;
  civilFromDays(key,y,m,d);
  char b[20];
  snprintf(b,sizeof b,"/p%04d%02u%02u.bin",y,m,d);
  return b;
}
String yearFile(char kind,int y) {
  char b[16];
  snprintf(b,sizeof b,"/%c%04d.bin",kind,y);
  return b;
}
// read n records into a (NaN if the file is missing or the wrong size)
void readRecs(const String&path,HVal*a,int n) {
  hvClear(a,n);
  File f=LittleFS.open(path,"r");
  if(f && f.size()==sizeof(HVal)*n) f.read((uint8_t*)a,sizeof(HVal)*n);
  if(f) f.close();
}
void writeRecs(const String&path,const HVal*a,int n) {
  File f=LittleFS.open(path,"w");
  if(f) {
    f.write((const uint8_t*)a,sizeof(HVal)*n);
    f.close();
  }
}
void saveYear() {
  if(!hYear) return;
  writeRecs(yearFile('d',hYear),hYD,366);
  writeRecs(yearFile('m',hYear),hYM,12);
  hYearDirty=false;
}
void useYear(int y) {   // make y the year held in RAM
  if(y==hYear) return;
  if(hYearDirty) saveYear();
  hYear=y;
  readRecs(yearFile('d',y),hYD,366);
  readRecs(yearFile('m',y),hYM,12);
}
// set one value of a daily (mon=false, idx=day of year) or monthly (mon=true, idx=month 0-11)
// record: in RAM for this year, otherwise read-modify-write that year's file
void histStore(bool mon,int y,int idx,int field,double v) {
  if(isnan(v)) return;
  if(y==hYear) {
    HVal&r=mon ? hYM[idx] : hYD[idx];
    if(r.v[field]!=(float)v) {
      r.v[field]=(float)v;
      hYearDirty=true;
    }
    return;
  }
  int n=mon?12:366;
  HVal*a=new HVal[n];
  String path=yearFile(mon?'m':'d',y);
  readRecs(path,a,n);
  if(a[idx].v[field]!=(float)v) {
    a[idx].v[field]=(float)v;
    writeRecs(path,a,n);
  }
  delete[] a;
}
void histSetDay(uint16_t key,int field,double v) {
  int y; unsigned m,d;
  civilFromDays(key,y,m,d);
  histStore(false,y,key-daysFromCivil(y,1,1),field,v);
}
void histSetMonth(int32_t mkey,int field,double v) {   // mkey = year*12 + month(0-11)
  histStore(true,mkey/12,mkey%12,field,v);
}
// a live "today"/"this month" value from the meter: fi 0=elec 1=gas, cost 0=kWh 1=GBP,
// period 0=today 2=this month (1=week isn't charted)
void histLive(int fi,int cost,int period,double v) {
  struct tm tm;
  if(period==1 || !localAt(nowEpoch(),tm)) return;
  useYear(tm.tm_year+1900);
  if(period==0) histSetDay(dayKey(tm),fi+cost*2,v);
  else histSetMonth((tm.tm_year+1900)*12+tm.tm_mon,fi+cost*2,v);
}
// file the meter's previous-day/month electricity figures once the clock is synced
void histApplyPending() {
  if(!(pDayEm|pMonEm|pDayCm|pMonCm)) return;
  struct tm tm;
  if(!localAt(nowEpoch(),tm)) return;
  useYear(tm.tm_year+1900);
  uint16_t day=dayKey(tm);
  int32_t mon=(tm.tm_year+1900)*12+tm.tm_mon;
  for(int k=0;k<8;k++) {
    if(pDayEm&(1<<k)) histSetDay(day-1-k,0,pDayE[k]*scale(0));
    if(pDayCm&(1<<k)) histSetDay(day-1-k,2,pDayC[k]);
  }
  for(int k=0;k<13;k++) {
    if(pMonEm&(1<<k)) histSetMonth(mon-1-k,0,pMonE[k]*scale(0));
    if(pMonCm&(1<<k)) histSetMonth(mon-1-k,2,pMonC[k]);
  }
  pDayEm=pMonEm=pDayCm=pMonCm=0;
}
void saveDay() {
  if(!hToday) return;
  File f=LittleFS.open(dayFile(hToday),"w");
  if(f) {
    f.write((const uint8_t*)hDayE,sizeof hDayE);
    f.write((const uint8_t*)hDayG,sizeof hDayG);
    f.close();
  }
  hDayDirty=false;
}
void loadDay(uint16_t key) {   // start recording day key (keeping anything already saved for it)
  for(int i=0;i<1440;i++) hDayE[i]=hDayG[i]=HNOVAL;
  File f=LittleFS.open(dayFile(key),"r");
  if(f && f.size()==sizeof hDayE+sizeof hDayG) {
    f.read((uint8_t*)hDayE,sizeof hDayE);
    f.read((uint8_t*)hDayG,sizeof hDayG);
  }
  if(f) f.close();
  hToday=key;
}
int16_t clamp16(double v) {
  long x=lround(v);
  return (int16_t)(x<-32000 ? -32000 : x>32000 ? 32000 : x);
}
// called every loop: when a new minute starts, store the minute that just ended
void histTick() {
  uint32_t now=nowEpoch();
  if(!now) return;
  uint32_t m=now/60;
  if(!hMin) hMin=m;
  if(m==hMin) return;
  struct tm tm;
  localAt(hMin*60,tm);
  uint16_t key=dayKey(tm);
  if(key!=hToday) {                // new day: save the old one, start (or resume) the new one
    if(hDayDirty) saveDay();
    loadDay(key);
  }
  int i=tm.tm_hour*60+tm.tm_min;
  double gp=gasPowerNow();
  hDayE[i]=hECnt ? clamp16(hESum/hECnt) : HNOVAL;
  hDayG[i]=isnan(gp) ? HNOVAL : clamp16(gp*100);
  hDayDirty=true;
  hMin=m;
  hESum=0;
  hECnt=0;
}
// delete the oldest history files while the file system is over HIST_FS_MAX_PCT full:
// 1-minute day files first, then daily, then monthly - never today's or this year's
void histPrune() {
  for(int guard=0;guard<100;guard++) {
    size_t used=0,total=0;
    platformFSInfo(used,total);
    if(!total || used*100<=total*(size_t)HIST_FS_MAX_PCT) return;
    String keep[3]={dayFile(hToday).substring(1),yearFile('d',hYear).substring(1),yearFile('m',hYear).substring(1)};
    const char kinds[3]={'p','d','m'};
    String oldest;
    for(int k=0;k<3 && !oldest.length();k++) {
      platformListFiles([&](const String&n) {
        if(n.length()>4 && n[0]==kinds[k] && n.endsWith(".bin") && isdigit((unsigned char)n[1]) &&
           n!=keep[k] && (!oldest.length() || n<oldest)) oldest=n;
      });
    }
    if(!oldest.length()) return;
    LittleFS.remove("/"+oldest);
    LOG.printf("history: storage over %d%%, removed %s\n",HIST_FS_MAX_PCT,oldest.c_str());
  }
}
void histSave() {
  if(hDayDirty) saveDay();
  if(hYearDirty) saveYear();
  histPrune();
}
// get the daily (mon=false, key=day key) or monthly (mon=true, key=year*12+month) record,
// from RAM for this year or from the file (f/fy cache the open year file between calls)
void histGet(bool mon,int32_t key,HVal&out,File&f,int&fy) {
  int y,idx;
  if(mon) {
    y=key/12;
    idx=key%12;
  } else {
    unsigned m,d;
    civilFromDays(key,y,m,d);
    idx=key-daysFromCivil(y,1,1);
  }
  for(int j=0;j<4;j++) out.v[j]=NAN;
  if(y==hYear) {
    out=mon ? hYM[idx] : hYD[idx];
    return;
  }
  if(y!=fy) {
    if(f) f.close();
    fy=y;
    f=LittleFS.open(yearFile(mon?'m':'d',y),"r");
    if(f && f.size()!=sizeof(HVal)*(mon?12:366)) f.close();
  }
  if(f && f.seek(sizeof(HVal)*idx)) f.read((uint8_t*)&out,sizeof out);
}

// ---------- config persistence ----------
void loadCfg() {
  if(!platformBeginFS()) return;
  File f=LittleFS.open(CFG,"r");
  if(!f) return;
  JsonDocument d;
  if(!deserializeJson(d,f)) {
    strlcpy(cfg.ssid,d["ssid"]|cfg.ssid,sizeof cfg.ssid);
    strlcpy(cfg.wpass,d["wpass"]|cfg.wpass,sizeof cfg.wpass);
    strlcpy(cfg.host,d["host"]|cfg.host,sizeof cfg.host);
    strlcpy(cfg.port,d["port"]|cfg.port,sizeof cfg.port);
    strlcpy(cfg.user,d["user"]|cfg.user,sizeof cfg.user);
    strlcpy(cfg.mpass,d["mpass"]|cfg.mpass,sizeof cfg.mpass);
    strlcpy(cfg.base,d["base"]|cfg.base,sizeof cfg.base);
    strlcpy(cfg.baud,d["baud"]|cfg.baud,sizeof cfg.baud);
    strlcpy(cfg.name,d["name"]|cfg.name,sizeof cfg.name);
    strlcpy(cfg.emax,d["emax"]|cfg.emax,sizeof cfg.emax);
    strlcpy(cfg.gmax,d["gmax"]|cfg.gmax,sizeof cfg.gmax);
    strlcpy(cfg.ecdp,d["ecdp"]|cfg.ecdp,sizeof cfg.ecdp);
    strlcpy(cfg.gcdp,d["gcdp"]|cfg.gcdp,sizeof cfg.gcdp);
    strlcpy(cfg.apass,d["apass"]|cfg.apass,sizeof cfg.apass);
  }
  f.close();
#if CLEAR_SETTINGS_PASSWORD
  if(cfg.apass[0]) { cfg.apass[0]=0; saveCfg(); LOG.println("Settings password removed"); }
#endif
}

void saveCfg() {
  JsonDocument d;
  d["ssid"]=cfg.ssid;
  d["wpass"]=cfg.wpass;
  d["host"]=cfg.host;
  d["port"]=cfg.port;
  d["user"]=cfg.user;
  d["mpass"]=cfg.mpass;
  d["base"]=cfg.base;
  d["baud"]=cfg.baud;
  d["name"]=cfg.name;
  d["emax"]=cfg.emax;
  d["gmax"]=cfg.gmax;
  d["ecdp"]=cfg.ecdp;
  d["gcdp"]=cfg.gcdp;
  d["apass"]=cfg.apass;
  File f=LittleFS.open(CFG,"w");
  if(f) {
    serializeJson(d,f);
    f.close();
  }
}

// ---------- meter scaling persistence ----------
void saveMeter() {
  JsonDocument d;
  for(int i=0;i<2;i++) {
    d["m"][i]=mMult[i];
    d["d"][i]=mDiv[i];
    d["l"][i]=mLearned[i];
    d["cd"][i]=costDp[i];
    d["cc"][i]=costChecked[i];
  }
  File f=LittleFS.open(METERF,"w");
  if(f) {
    serializeJson(d,f);
    f.close();
  }
}

void loadMeter() {
  File f=LittleFS.open(METERF,"r");
  if(!f) return;
  JsonDocument d;
  if(!deserializeJson(d,f)) {
    for(int i=0;i<2;i++) {
      uint32_t m=d["m"][i]|0UL, v=d["d"][i]|0UL;
      if(m && v) {
        mMult[i]=m;
        mDiv[i]=v;
        mLearned[i]=d["l"][i]|false;
      }
      int cd=d["cd"][i]|-1;
      if(cd>=0 && cd<=7) {
        costDp[i]=cd;
        costChecked[i]=d["cc"][i]|false;
      }
    }
  }
  f.close();
}

// ---------- last gas values persistence ----------
void saveGas() {
  JsonDocument d;
  d["k"]=(double)vGasK;                  // NaN is written as null
  d["t"]=gasEpoch;                       // when it changed (UTC seconds, 0 = unknown)
  d["p"]=vGasP;                          // gas power now (kW) ...
  d["pt"]=gasPEpoch;                     // ... and when gas kWh-today last changed
  JsonArray h=d["h"].to<JsonArray>();
  JsonArray c=d["c"].to<JsonArray>();
  for(int i=0;i<3;i++) {
    h.add(vHist[1][i]);
    c.add(vCost[1][i]);
  }
  File f=LittleFS.open(GASF,"w");
  if(f) {
    serializeJson(d,f);
    f.close();
  }
}

void loadGas() {
  File f=LittleFS.open(GASF,"r");
  if(!f) return;
  JsonDocument d;
  if(!deserializeJson(d,f)) {
    vGasK=d["k"]|(double)NAN;
    gasEpoch=d["t"]|(uint32_t)0;
    vGasP=d["p"]|(double)NAN;
    gasPEpoch=d["pt"]|(uint32_t)0;
    for(int i=0;i<3;i++) {
      vHist[1][i]=d["h"][i]|(double)NAN;
      vCost[1][i]=d["c"][i]|(double)NAN;
    }
    gasRestored=true;
  }
  f.close();
}

// ---------- tariff persistence ----------
void saveTariff() {
  JsonDocument d;
  for(int i=0;i<2;i++) {
    d["r"][i]=vRate[i];                  // NaN is written as null
    d["s"][i]=vStanding[i];
  }
  d["cv"]=vCV;
  d["cf"]=vCF;
  File f=LittleFS.open(TARIFFF,"w");
  if(f) {
    serializeJson(d,f);
    f.close();
  }
}

void loadTariff() {
  File f=LittleFS.open(TARIFFF,"r");
  if(!f) return;
  JsonDocument d;
  if(!deserializeJson(d,f)) {
    for(int i=0;i<2;i++) {
      vRate[i]=d["r"][i]|(double)NAN;
      vStanding[i]=d["s"][i]|(double)NAN;
    }
    vCV=d["cv"]|(double)NAN;
    vCF=d["cf"]|(double)NAN;
  }
  f.close();
}

void pub(const char*sub,double v,bool ret,int dp=3) {
  if(!mqtt.connected()) return;
  char tp[96],pl[24];
  snprintf(tp,sizeof tp,"%s/%s",cfg.base,sub);
  dtostrf(v,0,dp,pl);
  mqtt.publish(tp,pl,ret);
}

// ---------- ZCL frame decode ----------
static uint8_t fr[256];
static size_t fl=0;
static bool inF=false,esc=false;
static const uint8_t TAG[4]       = {0x01, 0x09, 0x07, 0x02};
static const uint8_t TAG_PREPAY[4] = {0x01, 0x09, 0x07, 0x05};
static const uint8_t DEM[4]        = {0x00, 0x04, 0x00, 0x2A};
static const uint8_t SUM[4]        = {0x00, 0x00, 0x00, 0x25};
static const uint8_t TAG_PRICE[4]  = {0x01, 0x09, 0x07, 0x00};   // price cluster 0x0700
static const uint8_t UOM[4]        = {0x00, 0x03, 0x00, 0x30};   // 0x0300 unit of measure
int fseq(const uint8_t*b,size_t n,const uint8_t*p) {
  if(n<4)return -1;
  for(size_t i=0;i+4<=n;i++) if(!memcmp(b+i,p,4)) return (int)i;
  return -1;
}
int32_t i24(const uint8_t*p) {
  int32_t v=p[0]|(p[1]<<8)|(p[2]<<16);
  if(v&0x800000)v-=(1<<24);
  return v;
}
uint64_t u48(const uint8_t*p) {
  uint64_t v=0;
  for(int i=5;i>=0;i--) v=(v<<8)|p[i];
  return v;
}

// Find a ZCL attribute record [id lo][id hi][status 00][type] with an unsigned
// integer type (0x20 uint8 .. 0x27 uint64) and return its little-endian value.
bool readUAttr(const uint8_t*f,size_t n,uint16_t id,uint64_t&out) {
  for(size_t i=0;i+4<=n;i++) {
    if(f[i]==(id&0xFF)&&f[i+1]==(id>>8)&&f[i+2]==0x00&&f[i+3]>=0x20&&f[i+3]<=0x27) {
      size_t len=f[i+3]-0x1F;
      if(i+4+len>n) return false;
      uint64_t v=0;
      for(int k=(int)len-1;k>=0;k--) v=(v<<8)|f[i+4+k];
      out=v;
      return true;
    }
  }
  return false;
}

// Decimal places fixed on the Settings page for this fuel, or -1 for automatic
// (an empty or invalid setting counts as automatic).
int costOverride(int fi) {
  const char*s=fi?cfg.gcdp:cfg.ecdp;
  return (s[0]>='0' && s[0]<='7' && !s[1]) ? s[0]-'0' : -1;
}

// Work out the decimal places of a fuel's raw costs by finding the power of ten that
// brings "cost today" close to usage today x unit rate + one day's standing charge.
// Does nothing until usage and tariff are known, or if no setting is a clear match.
void checkCostDp(int fi,uint64_t raw) {
  double want=vHist[fi][0]*vRate[fi]+vStanding[fi];
  if(raw==0 || isnan(want) || want<=0) return;
  int dp=-1;
  for(int i=0;i<=7;i++) {
    double r=(double)raw/pow(10,i)/want;
    if(r>=1.0/COST_MATCH_FACTOR && r<=COST_MATCH_FACTOR) {
      dp=i;
      break;
    }
  }
  if(dp<0) return;
  if(dp==costDp[fi]) {
    costDpCand[fi]=-1;
    if(!costChecked[fi]) {
      costChecked[fi]=true;
      saveMeter();
    }
    return;
  }
  if(costChecked[fi]) {               // already verified: change only on sustained disagreement
    if(costDpCand[fi]!=dp) {
      costDpCand[fi]=dp;
      costDpSince[fi]=millis();
      return;
    }
    if(millis()-costDpSince[fi]<COST_DP_CONFIRM_MIN*60000UL) return;
  }
  LOG.printf("%s cost decimal places: %u -> %d (checked against tariff)\n",
    fi?"gas":"elec",(unsigned)costDp[fi],dp);
  costDp[fi]=dp;
  costChecked[fi]=true;
  costDpCand[fi]=-1;
  saveMeter();
}

// Prepayment cluster (0x0705) frames: cost of energy used today / this week / this month
// (attrs 0x051C / 0x0530 / 0x0540), including standing charge.
void handlePrepay(const uint8_t*f,size_t n) {
  int t=fseq(f,n,TAG_PREPAY);
  if(t<0||(size_t)(t+4)>=n) return;
  uint8_t ch=f[t+4];
  if(ch!=1&&ch!=2) return;
  rxDecoded++;
  int fi=ch-1;
  // attr 0x0500 (HistoricalCostConsumptionFormatting) doesn't match the real decimal
  // places on some installations, so it is only recorded for diagnostics
  uint8_t fmt;
  if(readAttr8(f,n,0x0500,fmt) && fmt!=costFmt[fi]) {
    costFmt[fi]=fmt;
    LOG.printf("%s cost format attr 0x0500 = 0x%02X\n",fi?"gas":"elec",fmt);
  }
  uint64_t rawToday;
  if(readUAttr(f,n,0x051C,rawToday)) checkCostDp(fi,rawToday);
  int ov=costOverride(fi);
  double div=pow(10,ov>=0?ov:costDp[fi]);
  static const uint16_t CID[3]={0x051C,0x0530,0x0540};
  static const char*CNM[3]={"cost_today","cost_week","cost_month"};
  char sub[32];
  for(int i=0;i<3;i++) {
    uint64_t raw;
    if(readUAttr(f,n,CID[i],raw)) {
      double v=raw/div;
      if(fi==1 && v!=vCost[fi][i]) gasDirty=true;
      vCost[fi][i]=v;
      snprintf(sub,sizeof sub,"%s/%s",ch==1?"elec":"gas",CNM[i]);
      pub(sub,v,true);
      histLive(fi,1,i,v);
    }
  }
  // electricity cost for previous days (0x051E, 0x0520..0x052C) and months (0x0542..0x055A),
  // sent when the Trio II starts up - filed in the history charts
  if(fi==0) {
    for(int k=0;k<8;k++) {
      uint64_t raw;
      if(readUAttr(f,n,k?0x0520+2*(k-1):0x051E,raw)) { pDayC[k]=raw/div; pDayCm|=1<<k; }
    }
    for(int k=0;k<13;k++) {
      uint64_t raw;
      if(readUAttr(f,n,0x0542+2*k,raw)) { pMonC[k]=raw/div; pMonCm|=1<<k; }
    }
  }
}

uint32_t le32(const uint8_t*p) {
  return p[0]|(p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
double pow10i(int d) {
  double v=1;
  while(d-->0) v*=10;
  return v;
}
// Like readUAttr, for an 8-bit bitmap attribute (type 0x18)
bool readAttr8(const uint8_t*f,size_t n,uint16_t id,uint8_t&out) {
  for(size_t i=0;i+5<=n;i++) {
    if(f[i]==(id&0xFF)&&f[i+1]==(id>>8)&&f[i+2]==0x00&&f[i+3]==0x18) {
      out=f[i+4];
      return true;
    }
  }
  return false;
}
// store + publish one tariff value (GBP or MJ/m3), flagging a save if it changed
void setTariff(double&var,double v,const char*fuel,const char*name,int dp) {
  if(v!=var) {
    var=v;
    tariffDirty=true;
  }
  char sub[40];
  snprintf(sub,sizeof sub,"%s/%s",fuel,name);
  pub(sub,v,true,dp);
}

// Price cluster (0x0700) frames - sent by the hub about once an hour.
// Electricity uses endpoint 1, gas endpoint 3 (2 also accepted as gas).
//  - PublishPrice (cmd 0x00): unit rate
//  - PublishTariffInformation (cmd 0x04): standing charge
//  - PublishConversionFactor / PublishCalorificValue (cmds 0x02 / 0x03), or a read-attributes
//    response with attrs 0x0302/0x0303 and 0x0304/0x0306: gas volume correction and calorific value
// Each value carries its own number of decimal places ("trailing digits", high nibble).
bool handlePrice(const uint8_t*f,size_t n) {
  int t=fseq(f,n,TAG_PRICE);
  if(t<0||(size_t)(t+22)>n) return false;
  uint8_t ep=f[t+4];
  int fi=(ep==1)?0:(ep==2||ep==3)?1:-1;
  if(fi<0) return false;
  // ZCL frame: length byte at t+18, then frame control, [manufacturer code], sequence, command
  size_t L=f[t+18], z=t+19;
  if(z+L>n) return false;
  const uint8_t*zcl=f+z;
  uint8_t fc=zcl[0];
  size_t h=(fc&0x04)?5:3;
  if(L<h) return false;
  uint8_t cmd=zcl[h-1];
  const uint8_t*p=zcl+h;
  size_t pl=L-h;
  rxDecoded++;
  const char*nm=fi?"gas":"elec";
  if((fc&0x03)==1) {                  // cluster-specific command
    if(cmd==0x00 && pl>5) {           // PublishPrice
      size_t o=5+p[4];                // after ProviderID + RateLabel
      if(o+23<=pl) setTariff(vRate[fi],le32(p+o+19)/pow10i(p[o+11]>>4),nm,"unit_rate",5);
    }
    else if(cmd==0x04 && pl>18) {     // PublishTariffInformation
      size_t o=18+p[17];              // after IDs, StartTime, TariffType + TariffLabel
      if(o+10<=pl) setTariff(vStanding[fi],le32(p+o+6)/pow10i(p[o+5]>>4),nm,"standing_charge",5);
    }
    else if(cmd==0x02 && fi==1 && pl>=13) {   // PublishConversionFactor
      setTariff(vCF,le32(p+8)/pow10i(p[12]>>4),"gas","conversion_factor",5);
    }
    else if(cmd==0x03 && fi==1 && pl>=14) {   // PublishCalorificValue
      setTariff(vCV,le32(p+8)/pow10i(p[13]>>4),"gas","calorific_value",2);
    }
  }
  else if(cmd==0x01 && fi==1) {        // read attributes response (commodity attributes)
    uint64_t v;
    uint8_t td;
    if(readUAttr(p,pl,0x0304,v) && readAttr8(p,pl,0x0306,td))
      setTariff(vCV,v/pow10i(td>>4),"gas","calorific_value",2);
    if(readUAttr(p,pl,0x0302,v) && readAttr8(p,pl,0x0303,td))
      setTariff(vCF,v/pow10i(td>>4),"gas","conversion_factor",5);
  }
  return true;
}

// Metering cluster (0x0702) frame carrying the meter's formatting attributes
// (sent when the Trio II starts up): learn multiplier 0x0301 / divisor 0x0302.
void learnScaling(const uint8_t*f,size_t n,int fi) {
  if(fseq(f,n,UOM)<0) return;
  uint64_t m=0,d=0;
  bool changed=false;
  if(readUAttr(f,n,0x0302,d) && d>0 && d<=0xFFFFFF && (d!=mDiv[fi] || !mLearned[fi])) {
    mDiv[fi]=(uint32_t)d;
    changed=true;
  }
  if(readUAttr(f,n,0x0301,m) && m>0 && m<=0xFFFFFF && (m!=mMult[fi] || !mLearned[fi])) {
    mMult[fi]=(uint32_t)m;
    changed=true;
  }
  if(changed) {
    mLearned[fi]=true;
    saveMeter();
    LOG.printf("%s scaling from meter: x%lu / %lu\n",fi?"gas":"elec",
      (unsigned long)mMult[fi],(unsigned long)mDiv[fi]);
  }
}

void handle(const uint8_t*f,size_t n) {
  if(n<6||f[0]!=0x80||f[1]!=0x02) return;
  int t=fseq(f,n,TAG);
  if(t<0) {
    if(!handlePrice(f,n)) handlePrepay(f,n);
    return;
  }
  if((size_t)(t+4)>=n) return;
  uint8_t ch=f[t+4];
  const char*nm=(ch==1)?"elec":(ch==2)?"gas":0;
  if(!nm) return;
  rxDecoded++;
  int fi=ch-1;
  learnScaling(f,n,fi);
  char sub[32];
  int d=fseq(f,n,DEM);
  if(d>=0&&(size_t)(d+7)<=n) {
    // demand is in kW after scaling; published in W
    double w=i24(f+d+4);
    if(ch==1) w*=scale(0)*1000.0;
    snprintf(sub,sizeof sub,"%s/live_power_w",nm);
    pub(sub,w,false);
    if(ch==1) {
      vElecW=w;
      tElecW=millis();
      hESum+=w;     // averaged into the history chart's 1-minute samples
      hECnt++;
    }
  }
  int s=fseq(f,n,SUM);
  if(s>=0&&(size_t)(s+10)<=n) {
    double k=u48(f+s+4)*scale(fi);   // kWh (elec) or m3 (gas)
    snprintf(sub,sizeof sub,"%s/meter",nm);
    pub(sub,k,true);
    if(ch==1) {
      vElecK=k;
      tElecK=millis();
    } else  {
      if(k!=vGasK) {
        gasDirty=true;
        gasEpoch=nowEpoch();   // real time the reading changed (0 if clock not synced yet)
      }
      vGasK=k;
      tGasK=millis();
    }
  }
  // Today / this week / this month consumption.
  // Elec: Historical Consumption attrs 0x0401/0x0430/0x0440 (meter units -> kWh via scaling)
  // Gas:  Alternative Historical Consumption attrs 0x0C01/0x0C30/0x0C40 (kWh, /GAS_KWH_DIVISOR)
  static const uint16_t HID[2][3]={{0x0401,0x0430,0x0440},{0x0C01,0x0C30,0x0C40}};
  static const char*HNM[3]={"today_kwh","week_kwh","month_kwh"};
  for(int i=0;i<3;i++) {
    uint64_t raw;
    if(readUAttr(f,n,HID[fi][i],raw)) {
      double v=(ch==1) ? raw*scale(0) : raw/GAS_KWH_DIVISOR;
      if(fi==1 && v!=vHist[fi][i]) {
        gasDirty=true;
        // gas today changed: average kW since the previous change (this session, or the
        // saved one from before a restart - using its saved real time)
        if(i==0) {
          double prev=vHist[1][0];
          long age=gasChgAge();
          if(age>0 && !isnan(prev) && v>=prev) vGasP=(v-prev)/(age/3600.0);
          tGasChg=millis();   // first change (or midnight reset) just sets the baseline
          gasPEpoch=nowEpoch();
        }
      }
      vHist[fi][i]=v;
      snprintf(sub,sizeof sub,"%s/%s",nm,HNM[i]);
      pub(sub,v,true);
      histLive(fi,0,i,v);
    }
  }
  // electricity kWh for previous days (0x0403, 0x0420..0x042C) and months (0x0442..0x045A),
  // sent when the Trio II starts up - kept raw and filed in the history charts once the
  // clock is synced (so the scaling from the same burst is used)
  if(ch==1) {
    static const uint16_t PD[8]={0x0403,0x0420,0x0422,0x0424,0x0426,0x0428,0x042A,0x042C};
    for(int k=0;k<8;k++) {
      uint64_t raw;
      if(readUAttr(f,n,PD[k],raw)) { pDayE[k]=raw; pDayEm|=1<<k; }
    }
    for(int k=0;k<13;k++) {
      uint64_t raw;
      if(readUAttr(f,n,0x0442+2*k,raw)) { pMonE[k]=raw; pMonEm|=1<<k; }
    }
  }
}

void feed(uint8_t b) {
  rxBytes++;
  if(esc) {
    if(inF&&fl<sizeof fr) fr[fl++]=b^0x08;
    esc=false;
    return;
  }
  if(b==0x01) {
    inF=true;
    fl=0;
    esc=false;
  }
  else if(b==0x03) {
    if(inF) {
      rxFrames++;
      unsigned long before=rxDecoded;
      handle(fr,fl);
#if DUMP_UNDECODED
      // only frames we did NOT decode, skipping the frequent 7F FF / 80 08 / 80 09
      // housekeeping traffic. Prefixed with uptime in seconds to see when they arrived.
      bool housekeeping=fl>=2 && ((fr[0]==0x7F && fr[1]==0xFF) ||
                                  (fr[0]==0x80 && (fr[1]==0x08 || fr[1]==0x09)));
      if(rxDecoded==before && !housekeeping) {
        LOG.printf("[up %lus] IGNORED len=%u:",millis()/1000,(unsigned)fl);
        for(size_t i=0;i<fl;i++) LOG.printf(" %02X",fr[i]);
        LOG.println();
      }
#else
      (void)before;
#endif
    }
    inF=false;
    fl=0;
    esc=false;
  }
  else if(b==0x02) {
    if(inF) esc=true;
  }
  else if(inF&&fl<sizeof fr) fr[fl++]=b;
}

// ---------- web UI ----------
// human-friendly age, e.g. "12s ago", "25 min ago", "2 h ago", "3 days ago"
String fmtAge(unsigned long s) {
  if(s<60) return String(s)+"s ago";
  if(s<3600) return String(s/60)+" min ago";
  if(s<86400) return String(s/3600)+" h ago";
  return String(s/86400)+(s<172800?" day ago":" days ago");
}
String ageStr(unsigned long t) {
  if(!t) return "never";
  return fmtAge((millis()-t)/1000);
}
String numStr(double v,int dp) {
  if(isnan(v)) return "null";   // must be valid JSON, or the page's fetch fails
  char b[24];
  dtostrf(v,0,dp,b);
  return b;
}
String scaleStr(int fi) {
  String s=(fi?"gas ":"elec ");
  if(mMult[fi]!=1) s+="x"+String(mMult[fi])+" ";
  s+="/"+String(mDiv[fi])+(mLearned[fi]?" (from meter)":" (default)");
  int ov=costOverride(fi);
  s+=", cost "+String(ov>=0?ov:(int)costDp[fi])+" dp"+(ov>=0?" (manual)":costChecked[fi]?" (checked)":" (default)");
  if(costFmt[fi]>=0) s+=" [0x0500=0x"+String((int)costFmt[fi],HEX)+"]";
  return s;
}
const char PAGE_HEAD[] PROGMEM =
"<!doctype html><title>geotrio2mqtt</title><meta name=viewport content='width=device-width,initial-scale=1'>"
"<style>body{font-family:system-ui,Arial;margin:0;background:#101317;color:#e8eaed}"
".w{max-width:620px;margin:0 auto;padding:18px}h1{font-size:19px}"
".card{background:#1a1f26;border:1px solid #2a313b;border-radius:12px;padding:16px;margin:12px 0}"
".units{font-size:22px}"
".big{font-size:28px;font-weight:600}.sub{color:#8a93a0;font-size:12px}"
"a{color:#6fb3ff}label{display:block;margin:8px 0 2px;font-size:13px;color:#b9c2cd}"
"input{width:100%;padding:8px;border-radius:8px;border:1px solid #2a313b;background:#0d1014;color:#e8eaed;box-sizing:border-box}"
"button{margin-top:14px;padding:10px 16px;border:0;border-radius:8px;background:#2f6fed;color:#fff;font-size:15px;cursor:pointer}"
".row{display:flex;gap:12px}.row>div{flex:1}</style>";
void sendLiveJson() {
  String j="{";
  j+="\"elecW\":"+numStr(vElecW,0)+",\"elecWage\":\""+ageStr(tElecW)+"\",";
  j+="\"elecK\":"+numStr(vElecK,3)+",\"elecKage\":\""+ageStr(tElecK)+"\",";
  // After a restart, the saved gas reading's age comes from its saved real-time
  // timestamp; fall back to a label if the clock hasn't synced yet.
  String gasAge;
  if(tGasK) gasAge=ageStr(tGasK);
  else if(gasRestored && !isnan(vGasK)) {
    uint32_t now=nowEpoch();
    if(now && gasEpoch && now>=gasEpoch) gasAge=fmtAge(now-gasEpoch);
    else gasAge="from before restart";
  }
  else gasAge="never";
  j+="\"gasK\":"+numStr(vGasK,3)+",\"gasKage\":\""+gasAge+"\",";
  // gas power now (kW): 0 if gas kWh hasn't moved for GAS_IDLE_MS
  double gp=gasPowerNow();
  long gpAge=gasChgAge();
  j+="\"gasP\":"+numStr(gp,2)+",\"gasPage\":\""+(gpAge<0 ? String("never") : fmtAge(gpAge))+"\",";
  // h: [[elec today,week,month],[gas today,week,month]] in kWh
  j+="\"h\":[";
  for(int a=0;a<2;a++) {
    j+=(a?",[":"[");
    for(int b=0;b<3;b++) {
      if(b) j+=",";
      j+=numStr(vHist[a][b],3);
    }
    j+="]";
  }
  j+="],";
  // c: same layout as h, cost in GBP
  j+="\"c\":[";
  for(int a=0;a<2;a++) {
    j+=(a?",[":"[");
    for(int b=0;b<3;b++) {
      if(b) j+=",";
      j+=numStr(vCost[a][b],2);
    }
    j+="]";
  }
  j+="],";
  // t: [[elec unit rate p/kWh, elec standing p/day],[gas ...]]; cv MJ/m3, cf volume correction
  j+="\"t\":[";
  for(int a=0;a<2;a++) {
    j+=(a?",[":"[");
    j+=numStr(vRate[a]*100,3)+","+numStr(vStanding[a]*100,3)+"]";
  }
  j+="],\"cv\":"+numStr(vCV,2)+",\"cf\":"+numStr(vCF,5)+",";
  j+="\"scale\":\""+scaleStr(0)+", "+scaleStr(1)+"\",";
  j+="\"rssi\":"+String(WiFi.RSSI())+",";
  j+="\"ssid\":\""+String(WiFi.status()==WL_CONNECTED?WiFi.SSID():"")+"\",";
  j+="\"mqtt\":"+String(mqtt.connected()?1:0)+",";
  j+="\"up\":"+String((millis()-bootMs)/1000)+",";
  j+="\"heap\":"+String(ESP.getFreeHeap())+",";
  j+="\"rxB\":"+String(rxBytes)+",\"rxF\":"+String(rxFrames)+",\"rxD\":"+String(rxDecoded)+",";
  j+="\"heapTotal\":"+String(platformHeapTotal());
  j+="}";
  web.send(200,"application/json",j);
}
// /hist: chart data, streamed in chunks to keep RAM use low. Always includes "today" (day key,
// 0 if the clock isn't synced). Day keys are local days since 1970; month keys are year*12+month.
//  /hist?s=day&d=<day key>           -> "day", "e":[elec W x1440], "g":[gas kW x100 x1440] (by minute
//                                       of the day, null = no data; empty if none saved), "t":[totals]
//  /hist?s=days&from=<day key>&n=<n> -> "from", "r":[[elec kWh, gas kWh, elec GBP, gas GBP], ...]
//  /hist?s=months&from=<month key>&n=<n> -> the same per month
void sendHist() {
  String s=web.arg("s");
  web.setContentLength(CONTENT_LENGTH_UNKNOWN);
  web.send(200,"application/json","");
  String b;
  b.reserve(1400);
  auto flush=[&](bool force) {
    if(force || b.length()>1200) {
      web.sendContent(b);
      b="";
    }
  };
  auto rec=[&](const HVal&r) {
    b+="[";
    for(int j=0;j<4;j++) {
      if(j) b+=",";
      b+=numStr(r.v[j],j<2?3:2);
    }
    b+="]";
  };
  uint16_t today=todayKey();
  b="{\"today\":"+String(today);
  File f;
  int fy=-1;
  if(s=="day") {
    long key=web.arg("d").toInt();
    if(key<=0) key=today;
    b+=",\"day\":"+String(key);
    bool ram=key==hToday && hToday;
    File df;
    if(!ram && key>0) {
      df=LittleFS.open(dayFile(key),"r");
      if(df && df.size()!=sizeof hDayE+sizeof hDayG) df.close();
    }
    for(int k=0;k<2;k++) {
      b+=k ? "],\"g\":[" : ",\"e\":[";
      for(int i=0;(ram || df) && i<1440;i++) {
        int16_t v=HNOVAL;
        if(ram) v=k ? hDayG[i] : hDayE[i];
        else df.read((uint8_t*)&v,sizeof v);   // file holds all elec values, then all gas
        if(i) b+=",";
        b+=(v==HNOVAL) ? String("null") : String(v);
        flush(false);
      }
    }
    if(df) df.close();
    b+="],\"t\":";
    HVal t;
    histGet(false,key,t,f,fy);
    rec(t);
  } else {
    bool mon=s=="months";
    long from=web.arg("from").toInt();
    long n=web.arg("n").toInt();
    if(n<0) n=0;
    if(n>(mon?240:400)) n=mon?240:400;
    b+=",\"from\":"+String(from)+",\"r\":[";
    for(long i=0;from>0 && i<n;i++) {
      HVal r;
      histGet(mon,from+i,r,f,fy);
      if(i) b+=",";
      rec(r);
      flush(false);
    }
    b+="]";
  }
  if(f) f.close();
  b+="}";
  flush(true);
  web.sendContent("");   // end of chunked response
}

void handleRoot() {
  String h=FPSTR(PAGE_HEAD);
  h+="<div class=w><h1>geotrio2mqtt</h1>";
  // "Energy now": electricity dial + gas flame, like the Trio II display
  h+="<div class=row>"
  "<div class=card style='text-align:center'><div class=sub>Electricity now</div>"
  "<svg viewBox='0 0 200 170' style='width:100%;max-width:230px'>"
  "<g id=arc></g>"
  "<g id=ndl style='transform-origin:100px 100px;transform:rotate(135deg);transition:transform .8s'>"
  "<line x1=100 y1=100 x2=168 y2=100 stroke='#d6ecff' stroke-width=6 stroke-linecap=round /></g>"
  "<circle cx=100 cy=100 r=12 fill='#6fb3ff' stroke='#1a1f26' stroke-width=3 /></svg>"
  "<div class=big><span id=ew>---</span><span class=units> kW</span></div>"
  "<div class=sub id=ewa></div></div>"
  "<div class=card style='text-align:center'><div class=sub>Gas now</div>"
  "<svg viewBox='0 0 200 170' style='width:100%;max-width:230px'>"
  "<defs><linearGradient id=fg x1=0 y1=1 x2=0 y2=0>"
  "<stop offset=0 stop-color='#1e4fd8'/><stop offset=1 stop-color='#5aa0ff'/></linearGradient></defs>"
  "<g id=flm style='transform-origin:100px 162px;opacity:.35;filter:grayscale(1);"
  "transition:opacity .8s,filter .8s,transform .8s'>"
  "<path fill='url(#fg)' d='M100 10C112 45 150 70 150 112C150 142 128 162 100 162C72 162 50 142 50 112"
  "C50 88 64 70 76 52C78 74 86 86 96 92C92 64 94 36 100 10Z'/>"
  "<path fill='#a8d4ff' opacity=.75 d='M102 70C108 92 126 104 126 126C126 144 115 154 100 154"
  "C85 154 74 144 74 128C74 114 82 104 88 96C90 110 95 118 101 121C98 104 99 88 102 70Z'/></g></svg>"
  "<div class=big><span id=gw>---</span><span class=units> kW</span></div>"
  "<div class=sub id=gwa></div></div></div>";
  h+="<div class=row>"
  "<div class=card><div class=sub>Electric meter</div><div class=big><span id=ek>---</span><span class=units> kWh</span></div>"
  "<div class=sub><span id=eka></span></div></div>"
  "<div class=card><div class=sub>Gas meter</div><div class=big><span id=gk>---</span><span class=units> m&#179;</span></div>"
  "<div class=sub><span id=gka></span></div></div></div>";
  h+="<div class=card><div class=sub>Usage (kWh)</div>"
  "<table style='width:100%;text-align:right;font-size:18px;margin-top:6px'>"
  "<tr class=sub><th></th><th>Today</th><th>Week</th><th>Month</th></tr>"
  "<tr><th style='text-align:left'>Elec</th><td id=hED>---</td><td id=hEW>---</td><td id=hEM>---</td></tr>"
  "<tr><th style='text-align:left'>Gas</th><td id=hGD>---</td><td id=hGW>---</td><td id=hGM>---</td></tr>"
  "</table></div>";
  h+="<div class=card><div class=sub>Cost (&pound;, incl. standing charge)</div>"
  "<table style='width:100%;text-align:right;font-size:18px;margin-top:6px'>"
  "<tr class=sub><th></th><th>Today</th><th>Week</th><th>Month</th></tr>"
  "<tr><th style='text-align:left'>Elec</th><td id=cED>---</td><td id=cEW>---</td><td id=cEM>---</td></tr>"
  "<tr><th style='text-align:left'>Gas</th><td id=cGD>---</td><td id=cGW>---</td><td id=cGM>---</td></tr>"
  "</table></div>";
  h+="<div class=card><div class=sub>Tariff (from meter)</div>"
  "<table style='width:100%;text-align:right;font-size:18px;margin-top:6px'>"
  "<tr class=sub><th></th><th>Unit rate (p/kWh)</th><th>Standing (p/day)</th></tr>"
  "<tr><th style='text-align:left'>Elec</th><td id=tER>---</td><td id=tES>---</td></tr>"
  "<tr><th style='text-align:left'>Gas</th><td id=tGR>---</td><td id=tGS>---</td></tr>"
  "</table><div class=sub style='margin-top:8px'>Gas calorific value <span id=gcv>---</span> MJ/m&#179;, "
  "volume correction <span id=gcf>---</span></div></div>";
  // History card: scale buttons, period navigation and chart (drawn by hv()/hdraw() from /hist)
  h+="<style>.hb{display:inline-block;padding:4px 10px;margin:0 4px 6px 0;border:1px solid #2a313b;"
  "border-radius:14px;color:#b9c2cd;text-decoration:none;font-size:13px}.hb.on{background:#2f6fed;"
  "border-color:#2f6fed;color:#fff}.hn{font-size:24px;text-decoration:none;padding:0 12px}</style>";
  h+="<div class=card><div class=sub style='margin-bottom:8px'>History &nbsp; "
  "<span style='color:#6fb3ff'>&#9632; elec</span> <span style='color:#ff9f43'>&#9632; gas</span>"
  "<span style='float:right'>Show: <a href='javascript:sU(0)'>kWh</a> | <a href='javascript:sU(1)'>&pound;</a></span></div>"
  "<div><a class=hb id=s0 href='javascript:sc(0)'>Day</a><a class=hb id=s1 href='javascript:sc(1)'>Week</a>"
  "<a class=hb id=s2 href='javascript:sc(2)'>Month</a><a class=hb id=s3 href='javascript:sc(3)'>Year</a>"
  "<a class=hb id=s4 href='javascript:sc(4)'>All</a></div>"
  "<div style='display:flex;align-items:center;justify-content:space-between;margin:4px 0'>"
  "<a class=hn href='javascript:nv(-1)'>&lsaquo;</a><b id=ht>---</b>"
  "<a class=hn id=nx href='javascript:nv(1)'>&rsaquo;</a></div>"
  "<svg id=hc viewBox='0 0 600 190' style='width:100%;touch-action:pan-y'></svg>"
  "<div class=sub id=hs style='margin-top:4px'></div></div>";

  h+="<div class=card><div class=sub>Status</div>"
  "<div id=st class=sub></div></div>";
  h+="<a href=/config><button type=button>Settings</button></a>";
  h+="<div class=sub style='text-align:center;margin-top:24px'>"
  "<a href='https://github.com/nadnerb33/geotrio2mqtt'>"
  "https://github.com/nadnerb33/geotrio2mqtt</a></div>";
  // dial / flame full scale from Settings, defaulting to the #defines (EMAX in W, GMAX in kW)
  h+="<script>const EMAX="+String(kwSetting(cfg.emax,ELEC_DIAL_MAX_KW)*1000,0)
     +",GMAX="+String(kwSetting(cfg.gmax,GAS_FLAME_MAX_KW),2)+";</script>";
  h+="<script>"
  "function fmtUp(s){"
  "if(s<60)return s.toLocaleString()+' sec(s)';"
  "if(s<3600)return Math.floor(s/60).toLocaleString()+' min(s)';"
  "if(s<86400)return Math.floor(s/3600).toLocaleString()+' hr(s)';"
  "return Math.floor(s/86400).toLocaleString()+' day(s)';"
  "}"
  // draw the dial arc once: 270 degree sweep, green -> red
  "(()=>{const N=36,r=78,P=a=>{a*=Math.PI/180;"
  "return (100+r*Math.cos(a)).toFixed(1)+' '+(100+r*Math.sin(a)).toFixed(1)};let s='';"
  "for(let i=0;i<N;i++){const a0=135+270*i/N,a1=Math.min(135+270*(i+1)/N+0.6,405);"
  "s+=`<path d='M${P(a0)} A${r} ${r} 0 0 1 ${P(a1)}' fill='none' stroke-width='16' "
  "stroke='hsl(${120-120*i/(N-1)},75%,48%)'/>`}"
  "document.getElementById('arc').innerHTML=s})();"
  "async function u(){try{"
  "let r=await fetch('/live');let d=await r.json();"
  "const n=v=>v==null?'---':v.toLocaleString();"
  // electricity dial (linear scale, 0..EMAX) + exact figure in kW
  "if(d.elecW!=null){"
  "const f=Math.min(Math.max(d.elecW,0),EMAX)/EMAX;"
  "ndl.style.transform='rotate('+(135+270*f)+'deg)';"
  "ew.textContent=(d.elecW/1000).toLocaleString(undefined,{minimumFractionDigits:3,maximumFractionDigits:3});}"
  "ewa.textContent=d.elecWage;"
  // gas flame: bright and bigger when gas is in use, grey when idle/unknown
  "const gp=d.gasP,on=gp!=null&&gp>0.05;"
  "gw.textContent=gp==null?'---':gp.toLocaleString(undefined,{minimumFractionDigits:2,maximumFractionDigits:2});"
  "flm.style.opacity=on?1:.35;flm.style.filter=on?'none':'grayscale(1)';"
  "flm.style.transform='scale('+(on?0.75+0.25*Math.min(gp/GMAX,1):0.75)+')';"
  "gwa.textContent=gp==null?'waiting for gas updates':'average, updated '+d.gasPage;"
  "ek.textContent=n(d.elecK);"
  "eka.textContent=d.elecKage;"
  "gk.textContent=n(d.gasK);"
  "gka.textContent=d.gasKage;"
  "const n1=v=>v==null?'---':v.toLocaleString(undefined,{maximumFractionDigits:1});"
  "[[hED,hEW,hEM],[hGD,hGW,hGM]].forEach((r,a)=>r.forEach((c,b)=>c.textContent=n1(d.h[a][b])));"
  "const n2=v=>v==null?'---':v.toLocaleString(undefined,{minimumFractionDigits:2,maximumFractionDigits:2});"
  "[[cED,cEW,cEM],[cGD,cGW,cGM]].forEach((r,a)=>r.forEach((c,b)=>c.textContent=n2(d.c[a][b])));"
  "const n3=v=>v==null?'---':v.toLocaleString(undefined,{minimumFractionDigits:2,maximumFractionDigits:3});"
  "[[tER,tES],[tGR,tGS]].forEach((r,a)=>r.forEach((c,b)=>c.textContent=n3(d.t[a][b])));TR=[d.t[0][0],d.t[1][0]];"
  "gcv.textContent=n(d.cv);gcf.textContent=d.cf==null?'---':d.cf.toFixed(5);"
  "let wifi=d.ssid?"
  "'WiFi: '+d.ssid+' ('+d.rssi+' dBm)'"
  ":'Access Point Mode';"
  "let mem=(d.heap/1024).toLocaleString(undefined,{maximumFractionDigits:1});"
  "st.textContent=wifi+' - MQTT '+(d.mqtt?'connected':'down')+"
  "' - up '+fmtUp(d.up)+' - Memory '+mem+' KB free'+"
  "' - Meter RX: '+d.rxB+' bytes, '+d.rxF+' frames, '+d.rxD+' decoded'+"
  "' - Scaling: '+d.scale;"
  "}catch(e){}}"
  "u();setInterval(u,2000);"
  // ---- History card ----
  // S = scale (0 day, 1 week, 2 month, 3 year, 4 all), A = day key the view is anchored on,
  // TD = today's day key, U = 0 kWh / 1 GBP, HD = last /hist reply, IT = bars on screen
  "const MN=['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'],"
  "WD=['Mon','Tue','Wed','Thu','Fri','Sat','Sun'];"
  "let S=0,A=0,TD=0,U=0,HD=null,IT=[],HV=null,TR=[null,null];"
  "const KD=(y,m,d)=>Date.UTC(y,m,d)/864e5,DT=k=>new Date(k*864e5),"
  "FD=(k,o)=>DT(k).toLocaleDateString(undefined,Object.assign({timeZone:'UTC'},o)),"
  "F=v=>U?'\\u00a3'+v.toFixed(2):v.toFixed(1)+' kWh';"
  "function sc(x){S=x;hv()}"
  "function sU(x){U=x;hdraw()}"
  "function hg(i){if(IT[i])IT[i].go()}"
  "function hmsg(m){hc.innerHTML=`<text x='300' y='95' text-anchor='middle' font-size='16' fill='#8a93a0'>${m}</text>`;hs.textContent=''}"
  "function nv(d){const t=DT(A),y=t.getUTCFullYear(),m=t.getUTCMonth();"
  "A=[A+d,A+7*d,KD(y,m+d,1),KD(y+d,0,1),KD(y+10*d,0,1)][S];if(TD&&A>TD)A=TD;hv()}"
  "function nxOk(t){const y=t.getUTCFullYear(),m=t.getUTCMonth();"
  "return[A<TD,A-(t.getUTCDay()+6)%7+7<=TD,KD(y,m+1,1)<=TD,KD(y+1,0,1)<=TD,KD(y+1,0,1)<=TD][S]}"
  // fetch the period around A for scale S, then draw it
  "async function hv(){for(let i=0;i<5;i++)document.getElementById('s'+i).className='hb'+(i==S?' on':'');"
  "if(!A){try{const r=await fetch('/hist?s=day');const d=await r.json();TD=A=d.today}catch(e){}}"
  "if(!A){ht.textContent='---';nx.style.visibility='hidden';hmsg('waiting for the clock to sync');return}"
  "const t=DT(A),y=t.getUTCFullYear(),m=t.getUTCMonth();let u,ti;"
  "if(S==0){u='/hist?s=day&d='+A;ti=FD(A,{weekday:'short',day:'numeric',month:'short',year:'numeric'})}"
  "else if(S==1){const s=A-(t.getUTCDay()+6)%7;u='/hist?s=days&n=7&from='+s;"
  "ti=FD(s,{day:'numeric',month:'short'})+' \\u2013 '+FD(s+6,{day:'numeric',month:'short',year:'numeric'})}"
  "else if(S==2){const s=KD(y,m,1);u='/hist?s=days&from='+s+'&n='+(KD(y,m+1,1)-s);ti=FD(A,{month:'long',year:'numeric'})}"
  "else if(S==3){u='/hist?s=months&n=12&from='+y*12;ti=''+y}"
  "else{u='/hist?s=months&n=120&from='+(y-9)*12;ti=(y-9)+' \\u2013 '+y}"
  "ht.textContent=ti;"
  "try{const r=await fetch(u);HD=await r.json();TD=HD.today;HD.S=S;HD.y=y}catch(e){return}"
  "nx.style.visibility=nxOk(t)?'visible':'hidden';hdraw()}"
  // bars for week/month (days), year (months) and all (years, summed from months)
  "function hdraw(){const d=HD;HV=null;if(!d)return;if(d.S==0)return hday(d);const y=d.y;IT=[];"
  "if(d.S==1||d.S==2)d.r.forEach((v,i)=>{const k=d.from+i;"
  "IT.push({l:d.S==1?WD[i]:''+DT(k).getUTCDate(),v,go:()=>{S=0;A=k;hv()}})});"
  "if(d.S==3)d.r.forEach((v,i)=>IT.push({l:MN[i],v,go:()=>{S=2;A=KD(y,i,1);hv()}}));"
  "if(d.S==4)for(let j=0;j<10;j++){const v=[0,1,2,3].map(q=>{let s=null;"
  "for(let i=0;i<12;i++){const x=d.r[j*12+i][q];if(x!=null)s=(s||0)+x}return s});"
  "if(IT.length||v.some(x=>x!=null)){const yy=y-9+j;IT.push({l:''+yy,v,go:()=>{S=3;A=KD(yy,0,1);hv()}})}}"
  "hbars()}"
  "function hbars(){const o=U?2:0,N=IT.length,W=600,B=160,bw=W/Math.max(N,1);let mx=0;"
  "IT.forEach(x=>{mx=Math.max(mx,x.v[o]||0,x.v[o+1]||0)});"
  "if(!mx)return hmsg('no data for this period');"
  "let s=`<line x1='0' y1='${B}' x2='${W}' y2='${B}' stroke='#2a313b'/>"
  "<text x='2' y='15' font-size='16' fill='#8a93a0'>${F(mx)}</text>`;"
  "IT.forEach((x,i)=>{const X=i*bw;for(let j=0;j<2;j++){const v=x.v[o+j];if(v==null)continue;const h=v/mx*(B-22);"
  "s+=`<rect x='${(X+bw*.1+j*bw*.4).toFixed(1)}' y='${(B-h).toFixed(1)}' width='${(bw*.38).toFixed(1)}' "
  "height='${h.toFixed(1)}' fill='${j?'#ff9f43':'#6fb3ff'}' style='cursor:pointer' onclick='hg(${i})'>"
  "<title>${x.l} ${j?'gas':'elec'}: ${F(v)}</title></rect>`}"
  "if((N-1-i)%Math.ceil(N/8)==0)s+=`<text x='${(i?i<N-1?X+bw/2:X+bw:X).toFixed(1)}' y='${B+20}' "
  "text-anchor='${i?i<N-1?'middle':'end':'start'}' font-size='16' fill='#8a93a0'>${x.l}</text>`});"
  "hc.innerHTML=s;const T=j=>{let t=null;IT.forEach(x=>{const v=x.v[o+j];if(v!=null)t=(t||0)+v});return t==null?'---':F(t)};"
  "hs.textContent='Total: elec '+T(0)+', gas '+T(1)+' \\u2013 click a bar for more detail'}"
  // one day: elec (W) and gas (kW x100) per minute as lines, each on its own scale: kW, or in
  // GBP mode GBP per hour at the current unit rates (standing charges aren't included)
  // (elec max top-left in blue, gas max top-right in orange) so a boiler doesn't flatten elec
  "function hday(d){const e=d.e,g=d.g,n=1440,W=600,B=160;IT=[];"
  "if(!e.length||!e.concat(g).some(v=>v!=null)){hmsg('no minute-by-minute data for this day');hdt(d);return}"
  "if(U&&(TR[0]==null||TR[1]==null)){hmsg('waiting for the unit rates from the meter');hdt(d);return}"
  // qe/qg: divide a sample by this to get the value shown (kW, or GBP per hour)
  "const qe=U?1e5/TR[0]:1000,qg=U?1e4/TR[1]:100;"
  "const M=(a,q)=>{let m=U?.05:.5;a.forEach(v=>{if(v!=null)m=Math.max(m,v/q)});return U?Math.ceil(m*10)/10:Math.ceil(m)},me=M(e,qe),mg=M(g,qg);"
  "const ML=v=>U?'\\u00a3'+v.toFixed(2)+'/h':v+' kW';"
  "const X=i=>(i*W/(n-1)).toFixed(1);"
  "const P=(a,q,mx)=>{let s='',on=0;for(let i=0;i<n;i++){if(a[i]==null){on=0;continue}"
  "s+=(on?'L':'M')+X(i)+' '+(B-a[i]/q/mx*(B-22)).toFixed(1);on=1}return s};"
  "let s=`<line x1='0' y1='${B}' x2='${W}' y2='${B}' stroke='#2a313b'/>"
  "<text x='2' y='15' font-size='16' fill='#6fb3ff'>${ML(me)}</text>"
  "<text x='${W-2}' y='15' text-anchor='end' font-size='16' fill='#ff9f43'>${ML(mg)}</text>`;"
  "for(let q=0;q<=4;q++)s+=`<text x='${X(Math.min(q*360,n-1))}' y='${B+20}' "
  "text-anchor='${q?q<4?'middle':'end':'start'}' font-size='16' fill='#8a93a0'>${('0'+q*6).slice(-2)}:00</text>`;"
  "s+=`<path d='${P(g,qg,mg)}' fill='none' stroke='#ff9f43' stroke-width='1.5'/>"
  "<path d='${P(e,qe,me)}' fill='none' stroke='#6fb3ff' stroke-width='1.5'/>"
  // hover marker: a guide line, a dot on each line and the values at that minute (see hmv)
  "<line id=hqL y1='22' y2='${B}' stroke='#8a93a0' stroke-dasharray='3 3' visibility='hidden'/>"
  "<circle id=hqG r='4' fill='#ff9f43' stroke='#000' visibility='hidden'/>"
  "<circle id=hqE r='4' fill='#6fb3ff' stroke='#000' visibility='hidden'/>"
  "<text id=hqT x='300' y='15' text-anchor='middle' font-size='15' fill='#cfd6df'></text>`;"
  "HV={e,g,me,mg,qe,qg,U};hc.innerHTML=s;hdt(d)}"
  // mouse/finger over the day chart: move the dots to that minute and show its values
  "const hq=id=>document.getElementById(id);"
  "function hmv(ev){if(!HV||!hq('hqL'))return;const n=1440,W=600,B=160,r=hc.getBoundingClientRect(),"
  "i=Math.max(0,Math.min(n-1,Math.round((ev.clientX-r.left)/r.width*(n-1)))),x=i*W/(n-1);"
  "const D=(id,v,q,mx)=>{const c=hq(id);c.setAttribute('visibility',v==null?'hidden':'visible');"
  "if(v!=null){c.setAttribute('cx',x);c.setAttribute('cy',B-v/q/mx*(B-22))}};"
  "const e=HV.e[i],g=HV.g[i],k=v=>v==null?'---':HV.U?'\\u00a3'+v.toFixed(3)+'/h':v.toFixed(2)+' kW';"
  "hq('hqL').setAttribute('x1',x);hq('hqL').setAttribute('x2',x);hq('hqL').setAttribute('visibility','visible');"
  "D('hqE',e,HV.qe,HV.me);D('hqG',g,HV.qg,HV.mg);"
  "hq('hqT').textContent=('0'+Math.floor(i/60)).slice(-2)+':'+('0'+i%60).slice(-2)"
  "+' \\u00b7 elec '+k(e==null?null:e/HV.qe)+' \\u00b7 gas '+k(g==null?null:g/HV.qg)}"
  "function hlv(){if(!hq('hqL'))return;['hqL','hqE','hqG'].forEach(id=>hq(id).setAttribute('visibility','hidden'));"
  "hq('hqT').textContent=''}"
  "hc.addEventListener('pointermove',hmv);hc.addEventListener('pointerleave',hlv);"
  // day total under the chart
  "function hdt(d){const t=d.t||[],a=t[U?2:0],b=t[U?3:1];"
  "hs.textContent=a==null&&b==null?'':'Day total: elec '+(a==null?'---':F(a))+', gas '+(b==null?'---':F(b))}"
  "hv();setInterval(()=>{if(TD&&A>=TD-31)hv()},60000);"

  "</script></div>";
  web.send(200,"text/html",h);
}
String field(const char*lbl,const char*name,const char*val,const char*type="text") {
  return "<label>"+String(lbl)+"</label><input name="+name+" type="+type+" value='"+String(val)+"'>";
}
// Settings page password: if one is set, the browser must send user "admin" + that password.
// Returns false (and asks the browser for them) when it doesn't.
bool settingsAuth() {
  if(!cfg.apass[0] || web.authenticate("admin",cfg.apass)) return true;
  web.requestAuthentication(BASIC_AUTH,"geotrio2mqtt settings");
  return false;
}
void handleConfig() {
  if(!settingsAuth()) return;
  String h=FPSTR(PAGE_HEAD);
  h+="<div class=w><h1>Settings</h1><form method=POST action=/save>";
  h+="<div class=card><div class=sub>Display</div>";
  h+="<div class=row><div>"+field("Electricity dial max (kW)","emax",cfg.emax)+"</div>"
     "<div>"+field("Gas flame max (kW)","gmax",cfg.gmax)+"</div></div>";
  h+="<div class=sub style='margin-top:6px'>Dial: the needle reaches the end at this power. "
     "Flame: grows to full size at this power.</div>";
  h+="</div><div class=card>";
  h+="<div class=sub>WiFi</div>";
  h+=field("SSID","ssid",cfg.ssid)+field("Password","wpass",cfg.wpass,"password");
  h+=field("Device name (letters, numbers, hyphens)","name",cfg.name);
  h+="</div><div class=card><div class=sub>MQTT broker</div>";
  h+="<div class=row><div>"+field("Host","host",cfg.host)+"</div><div>"+field("Port","port",cfg.port)+"</div></div>";
  h+="<div class=row><div>"+field("User","user",cfg.user)+"</div><div>"+field("Password","mpass",cfg.mpass,"password")+"</div></div>";
  h+=field("Topic base","base",cfg.base);
  h+="</div><div class=card><div class=sub>Meter</div>"+field("Baud","baud",cfg.baud);
  h+="<div class=row><div>"+field("Electricity cost decimal places","ecdp",cfg.ecdp,"number min=0 max=7")+"</div>"
     "<div>"+field("Gas cost decimal places","gcdp",cfg.gcdp,"number min=0 max=7")+"</div></div>";
  h+="<div class=sub style='margin-top:6px'>Leave empty to work these out automatically from your tariff. "
     "Only set them if costs are out by a factor of 10, 100 or 1000.</div></div>";
  h+="<div class=card><div class=sub>Security</div>"+field("Settings page password","apass",cfg.apass,"password");
  h+="<div class=sub style='margin-top:6px'>Leave empty for no password. When set, opening Settings "
     "asks for user name <b>admin</b> and this password.</div></div>";
  h+="<button type=submit>Save &amp; reboot</button> &nbsp; <a href=/>Cancel</a></form></div>";
  web.send(200,"text/html",h);
}
void handleSave() {
  if(!settingsAuth()) return;
  auto g=[&](const char*n,char*dst,size_t sz) {
    if(web.hasArg(n)) strlcpy(dst,web.arg(n).c_str(),sz);
  };
  g("ssid",cfg.ssid,sizeof cfg.ssid);
  g("wpass",cfg.wpass,sizeof cfg.wpass);
  g("host",cfg.host,sizeof cfg.host);
  g("port",cfg.port,sizeof cfg.port);
  g("user",cfg.user,sizeof cfg.user);
  g("mpass",cfg.mpass,sizeof cfg.mpass);
  g("base",cfg.base,sizeof cfg.base);
  g("baud",cfg.baud,sizeof cfg.baud);
  g("name",cfg.name,sizeof cfg.name);
  g("emax",cfg.emax,sizeof cfg.emax);
  g("gmax",cfg.gmax,sizeof cfg.gmax);
  g("ecdp",cfg.ecdp,sizeof cfg.ecdp);
  g("gcdp",cfg.gcdp,sizeof cfg.gcdp);
  g("apass",cfg.apass,sizeof cfg.apass);
  // network names may only contain a-z, 0-9 and '-', and can't start or end with '-'
  size_t o=0;
  for(size_t i=0;cfg.name[i];i++) {
    char c=tolower((unsigned char)cfg.name[i]);
    if(!isalnum((unsigned char)c)) c='-';
    if(c=='-' && (o==0 || cfg.name[o-1]=='-')) continue;   // no leading/double hyphens
    cfg.name[o++]=c;
  }
  while(o>0 && cfg.name[o-1]=='-') o--;                    // no trailing hyphen
  cfg.name[o]=0;
  if(!o) strlcpy(cfg.name,HOSTNAME,sizeof cfg.name);
  saveCfg();
  web.send(200,"text/html","<meta http-equiv=refresh content='4;url=/'>Saved. Rebooting...");
  delay(400);
  ESP.restart();
}
void startWeb() {
  web.on("/",handleRoot);
  web.on("/live",sendLiveJson);
  web.on("/hist",sendHist);
  web.on("/config",handleConfig);
  web.on("/save",HTTP_POST,handleSave);
  web.onNotFound([](){ if(apMode){ web.sendHeader("Location","/"); web.send(302,"text/plain",""); }
  else web.send(404,"text/plain","404"); });
  web.begin();
}
void startAP() {
  apMode=true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("geotrio2mqtt");
  dns.start(53,"*",WiFi.softAPIP());
}
void setup() {
  bootMs=millis();
  pinMode(0,INPUT_PULLUP);
  // FLASH button -> force setup AP at runtime
  snprintf(nodeId,sizeof nodeId,"geo%06x",(unsigned)(platformChipId()&0xFFFFFF));
  platformBeginLog();
  loadCfg();
  loadGas();
  loadMeter();
  loadTariff();
  snprintf(statusTopic,sizeof statusTopic,"%s/status",cfg.base);
  platformBeginMeter(atol(cfg.baud));
  LOG.printf("geotrio2mqtt node %s, meter RX on %s @ %s baud\n",nodeId,platformMeterPin().c_str(),cfg.baud);
  LOG.printf("scaling: %s, %s\n",scaleStr(0).c_str(),scaleStr(1).c_str());
  if(cfg.ssid[0]) {
    platformSetHostname(cfg.name);   // must be set before the WiFi starts (DHCP sends this name)
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid,cfg.wpass);
    for(int i=0;i<40 && WiFi.status()!=WL_CONNECTED;i++) delay(250);
    // internet time, used to timestamp the saved gas reading and for the history charts
    platformConfigTime(TIMEZONE);
  }
  if(WiFi.status()!=WL_CONNECTED) {
    LOG.println("WiFi not connected - starting AP 'geotrio2mqtt' (192.168.4.1)");
    startAP();
  } else {
    LOG.print("WiFi connected, IP ");
    LOG.println(WiFi.localIP());
    LOG.print("MAC ");
    LOG.println(WiFi.macAddress());
    if(MDNS.begin(cfg.name)) {
      MDNS.addService("http","tcp",80);
      LOG.printf("Web page: http://%s.local\n",cfg.name);
    }
  }
  startWeb();
  mqtt.setServer(cfg.host,atoi(cfg.port));
}
void mqttTry() {
  String id=String(nodeId);
  if(mqtt.connect(id.c_str(), cfg.user[0]?cfg.user:0, cfg.user[0]?cfg.mpass:0,
  statusTopic,0,true,"offline")) {
    mqtt.publish(statusTopic,"online",true);
  }
}
void loop() {
  // hold FLASH (GPIO0) briefly at runtime to drop into the setup AP/portal
  if(digitalRead(0)==LOW) {
    delay(50);
    if(digitalRead(0)==LOW && !apMode) {
      startAP();
    }
  }
  platformLoop();
  // history charts: close 1-minute samples, file pending meter history, save periodically
  histTick();
  histApplyPending();
  if(!hYear) {                     // once the clock is synced, load this year's totals
    struct tm tm;
    if(localAt(nowEpoch(),tm)) useYear(tm.tm_year+1900);
  }
  if(hMin && millis()-lastHistSave>HIST_SAVE_MIN*60000UL) {
    lastHistSave=millis();
    histSave();
  }
#if DUMP_UNDECODED
  // every 10s, log meter RX counters
  static unsigned long lastDiag=0;
  if(millis()-lastDiag>10000) {
    lastDiag=millis();
    LOG.printf("meter RX: %lu bytes, %lu frames, %lu decoded | elecW=%.0f elecK=%.3f gasK=%.3f\n",
      rxBytes,rxFrames,rxDecoded,(double)vElecW,(double)vElecK,(double)vGasK);
  }
#endif
  // save changed gas / tariff values, at most once a minute (limits flash wear)
  if((gasDirty || tariffDirty) && millis()-lastGasSave>60000) {
    lastGasSave=millis();
    if(gasDirty) saveGas();
    if(tariffDirty) saveTariff();
    gasDirty=tariffDirty=false;
  }
  if(apMode) {
    dns.processNextRequest();
    web.handleClient();
    while(MeterSerial.available()) feed((uint8_t)MeterSerial.read());
    return;
  }
  web.handleClient();
  if(WiFi.status()!=WL_CONNECTED) {
    // the WiFi stack reconnects by itself; only nudge it occasionally
    // (calling reconnect() too often can stop it ever connecting)
    if(millis()-lastWifi>10000) {
      lastWifi=millis();
      WiFi.reconnect();
    }
    while(MeterSerial.available()) feed((uint8_t)MeterSerial.read());
    delay(10);
    return;
  }
  if(!mqtt.connected()) {
    if(millis()-lastMqtt>3000) {
      lastMqtt=millis();
      mqttTry();
    }
  }
  else mqtt.loop();
  while(MeterSerial.available()) feed((uint8_t)MeterSerial.read());
}
