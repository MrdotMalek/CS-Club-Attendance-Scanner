#include "scanner.h"
#include <SPI.h>
#include <MFRC522.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <time.h>

//Credentials are in secrets.h which is gitignored.
//If you want to use this code, create a secrets.h file with your wifi credentials.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #define wifi_ssid ""
  #define wifi_password ""
#endif

//Everything that is static is private to this file
static const int PIN_SCK    = 41;
static const int PIN_MOSI   = 42;
static const int PIN_MISO   =  2;
static const int PIN_SS     =  1;
static const int PIN_RST    =  21;
static const int SD_CMD     =  38;
static const int SD_CLK     =  39;
static const int SD_D0      =  40;

static const int status_time = 3000; //Used to let user know the reader is still active
static const int wifi_timeout = 10000; //10 seconds to connect to wifi

static const char* timezone = "EST5EDT,M3.2.0,M11.1.0"; //POSIX timezone string for Eastern Time (US & Canada) with daylight saving time rules
static const char* log_path = "/attendance_log.csv"; //writes to this file on the SD card

MFRC522 rfid(PIN_SS, PIN_RST); //Initializes reader object
static bool sdReady = false; //Flag to indicate if SD card is ready for writing
static bool clockSet = false; //Flag to indicate if clock has been set for timestamping


//Formats the time in the way excel reads it
String timestamp() {
  time_t now = time(nullptr); //gets time from esp32
  struct tm tmv; //holds a calendar time structure
  localtime_r(&now, &tmv); //converts time_t to calendar time structure
  char buff[20];
  strftime(buff, sizeof(buff), "%Y-%m-%d %H:%M:%S", &tmv); //stores the data from tmv in YYYY-MM-DD HH:MM:SS format
  return String(buff);
}

bool loggingReady() {
    return sdReady; //Returns true if the SD card is ready.
}

//Mounts the SD card and checks if it is present. Returns true if it is ready for writing, false if not.
static bool initSD() {
  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 5)) { 
    Serial.println("SD card mount failed");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("No SD_MMC card attached");
    return false;
  }
  Serial.printf("SD_MMC Card Size: %lluMB\n", 
    SD_MMC.cardSize() / (1024ULL * 1024ULL)); //If mount is successful, prints the size of the SD card in MB
  return true;
}

//Wifi Status helper function to print the status of the wifi connection
static const char* wifiStatusName(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "NO_SSID_AVAIL (network not found)";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED (wrong password / rejected)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "unknown";
  }
}
//Connects to wifi and sets the clock for timestamping, then disconnects.
static void syncClock() {
  if (strlen(wifi_ssid) == 0) {
    Serial.println("No wifi ssid provided, skipping clock sync");
    return;
  }

  Serial.printf("Connecting to wifi: %s\n", wifi_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_password);


  unsigned long t0 = millis();
  wl_status_t status = WL_IDLE_STATUS;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) { //10 second timeout
    wl_status_t newStatus = WiFi.status();
    if (newStatus != status) {
      status = newStatus;
      Serial.printf("WiFi status: %d\n", status);
    }
    delay(250);
    Serial.print(".");
  }
  Serial.println();

    

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("Wi-Fi FAILED after %lu ms - %s\n",
                  millis() - t0, wifiStatusName(WiFi.status()));
    WiFi.mode(WIFI_OFF);
    return;
  }

  //Prints the connection info and sets the clock
  Serial.printf("Wi-Fi CONNECTED in %lu ms\n", millis() - t0);
  Serial.printf("  IP:      %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("  gateway: %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("  DNS:     %s\n", WiFi.dnsIP().toString().c_str());
  Serial.printf("  RSSI:    %d dBm\n", WiFi.RSSI());

  //Check if DNS is working by resolving pool.ntp.org
  IPAddress ntpIP;
  if (WiFi.hostByName("pool.ntp.org", ntpIP))
    Serial.printf("  DNS OK:  pool.ntp.org -> %s\n", ntpIP.toString().c_str());
  else
    Serial.println(F("  DNS FAILED - connected but no working internet"));

  configTzTime(timezone, "pool.ntp.org", "time.nist.gov");

  struct tm tmv;
  if (getLocalTime(&tmv, 10000)) {
    clockSet = true;
    Serial.printf("Clock set: %s\n", timestamp().c_str());
  } else {
    Serial.println("Failed to get time from NTP server");
  }

  WiFi.disconnect(true); //Disconnects from wifi to save power and reduce interference with RFID reader
  WiFi.mode(WIFI_OFF);
}

//Write the header row only if the file doesnt exit
static void ensureLogFile() {
  if (SD_MMC.exists(log_path)) {
    Serial.println("Logging into existing file");
    return;
  }
  File f = SD_MMC.open(log_path, FILE_WRITE);
  if (!f) {
    Serial.println("Failed to create log file");
    sdReady = false;
    return;
  }
  f.println("Timestamp,UID,Type");
  f.close();
  Serial.println("Log file created");
}

//Adds a line to the file then closes it to ensure buffered rows wont die without power
bool logScan (const String& ts, const String& uid) {
  if (!sdReady) {
    Serial.println(F("SD card not ready, cannot log scan"));
    return false;
  }

  File f = SD_MMC.open(log_path, FILE_APPEND);
  if (!f) {
    Serial.printf(" logScan: Open failed. Exists=%d used=%llu/%llu bytes \n", 
                  SD_MMC.exists(log_path), 
                  SD_MMC.usedBytes(), 
                  SD_MMC.totalBytes());
    return false;
  }

  size_t n = f.print(ts);
  n += f.print(",");
  n += f.println(uid);
  f.close();

  if (n == 0) {
    Serial.println(F("logScan: Write failed ,likely write protected SD card"));
    return false;
  }

  Serial.printf(" logged %u bytes\n", (unsigned)n);
  return true;
}

bool cardPresent() {
  byte atr[2];
  byte atrSize = sizeof(atr);
  MFRC522::StatusCode s = rfid.PICC_WakeupA(atr, &atrSize); //Wakes up any asleep RFID tags and returns its ATQA to byte atr.
  return (s == MFRC522::STATUS_OK || s == MFRC522::STATUS_COLLISION); //Returns true of 1 or more cards are detected.
}

//Converts the RFID id from raw binary into readable hexadecimal
String uidString() {
  String s;
  for (byte i = 0; i < rfid.uid.size; i++) { 
    if (rfid.uid.uidByte[i] < 0x10) {
      s += '0'; //Adds leading zeros to the one digit byte values.
    }
    s += String(rfid.uid.uidByte[i], HEX); //Converts the raw byte into hexadecimal
  }
  s.toUpperCase();
  return s;
}

bool scannerBegin() {
  //Initialzes Reader, SPI, SD card, and serial
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
  SD_MMC.setPins(SD_CLK, SD_CMD , SD_D0);
  rfid.PCD_Init();
  delay(1000);

  //Status lines to make debugging easier
  Serial.println();
  Serial.println(F("=== hardware check ==="));
  Serial.printf("SCK=%d  MOSI=%d  MISO=%d  SS=%d  RST=%d\n",
                PIN_SCK, PIN_MOSI, PIN_MISO, PIN_SS, PIN_RST);
  Serial.printf("Built %s %s\n", __DATE__, __TIME__); //Makes sure that the file running is the most recent

  //Test to ensure the reader can properly send info to serial
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg); //Returns hardware version
  Serial.printf("Version register: 0x%02X\n", v); //Formats the version to hexadecimal with 2 digits
  if (v == 0x00 || v == 0xFF) { //These values indicate no response
    Serial.println(F("SPI dead"));
    while (true) delay(1000);
  }
  Serial.println(F("SPI good"));

  
  //Turns sensitivity to highest settings
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
  //Reader needs to reset in order for change to take effect
  rfid.PCD_AntennaOff();
  rfid.PCD_AntennaOn();

  sdReady = initSD();
  syncClock();
  if (sdReady) {
    ensureLogFile();
  }

  if (!clockSet) {
    Serial.println(F("Clock not set, timestamps will be inaccurate."));
  }

  Serial.println(F("Antenna on at max gain."));
  Serial.println(F("Lay a tag flat on the spiral trace.\n"));

  return sdReady;
}

bool readTap(Tap& tap) {
if (!cardPresent() || !rfid.PICC_ReadCardSerial()) return false; 

    String uid = uidString();
    static String lastUid = ""; //Static variable to hold the last UID read
    static unsigned long lastReadTime = 0; //Static variable to hold the last read time

    bool isNewRead = (uid != lastUid) || (millis() - lastReadTime > 1000); //Check if the read is new

    if (isNewRead) {
        tap.uid = uid;
        tap.type = rfid.PICC_GetTypeName(rfid.PICC_GetType(rfid.uid.sak));
    }

    lastUid = uid; //Update the last UID read
    lastReadTime = millis(); //Update the last read time

    rfid.PICC_HaltA(); //Halt the card to prevent it from being read again immediately
    rfid.PCD_StopCrypto1(); //Stop encryption on the reader
    return isNewRead; //Return true if the read is new, false otherwise}
}
