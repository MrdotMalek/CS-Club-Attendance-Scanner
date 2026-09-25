#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <sd_read_write.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <time.h>

#define PIN_SCK    41
#define PIN_MOSI   42
#define PIN_MISO    2
#define PIN_SS      1
#define PIN_RST    21
#define SD_MMC_CMD 38
#define SD_MMC_CLK 39
#define SD_MMC_D0  40

#define status_time 3000 //Used to let user know the reader is still active

MFRC522 rfid(PIN_SS, PIN_RST); //Initializes reader object

//Wifi setup once to set clock for SD card timestamping.

const char* wifi_ssid = "";
const char* wifi_password = "";
const char* timezone = "EST5EDT,M3.2.0,M11.1.0";
const char* log_path = "/attendance_log.csv";

bool sdReady = false; //Flag to indicate if SD card is ready for writing

//Formats the time in the way excel reads it
String timestamp() {
  time_t now = time(nullptr); //gets time from esp32
  struct tm tmv; //holds a calendar time structure
  localtime_r(&now, &tmv); //converts time_t to calendar time structure
  char buff[20];
  strftime(buff, sizeof(buff), "%Y-%m-%d %H:%M:%S", &tmv); //stores the data from tmv in YYYY-MM-DD HH:MM:SS format
  return String(buff);
}

//Mounts the SD card and checks if it is present. Returns true if it is ready for writing, false if not.
bool initSD() {
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

//Connects to wifi and sets the clock for timestamping, then disconnects.
void syncClock() {
  if (strlen(wifi_ssid) == 0) {
    Serial.println("No wifi ssid provided, skipping clock sync");
    return;
  }

  WiFi.begin(wifi_ssid, wifi_password);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) { //10 second timeout
    delay(250);
    Serial.print(".");
    }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Failed to connect to wifi, skipping clock sync");
    return;
  }
  Serial.println("Connected to wifi, syncing clock...");

  configTzTime(timezone, "pool.ntp.org", "time.nist.gov");
  struct tm tmv;
  if (getLocalTime(&tmv, 10000)) {
    Serial.printf("Clock set: %s\n", timestamp().c_str());
  } else {
    Serial.println("Failed to get time from NTP server");
  }

  WiFi.disconnect(true); //Disconnects from wifi to save power and reduce interference with RFID reader
  WiFi.mode(WIFI_OFF);
}

//Write the header row only if the file doesnt exit
void ensureLogFile() {
  if (SD_MMC.exists(log_path)) {
    Serial.println("Log file already exists");
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
  if (!sdReady) return false;
  File f = SD_MMC.open(log_path, FILE_APPEND);
  if (!f) {
    Serial.println("Failed to open log file for appending");
    return false;
  }
  f.print(ts); f.print(","); f.print(uid); f.close();
  return true;
}


void setup() {
  //Initialzes Reader, SPI, SD card, and serial
  Serial.begin(115200);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
  SD_MMC.setPins(SD_MMC_CLK, SD_MMC_CMD , SD_MMC_D0);
  rfid.PCD_Init();
  delay(1000);

  //Initializes SD card and creates a file if it doesn't exist
  if (!SD_MMC.begin("/sdcard", true, true, SDMMC_FREQ_DEFAULT, 5)) {
    Serial.println("SD card mount failed");
    return;
  }

  //Checks to see if an SD card is present and prints if there isnt.  
  uint8_t cardType = SD_MMC.cardType();
  if(cardType == CARD_NONE){
    Serial.println("No SD_MMC card attached");
    return;
  }
  //Prints SD card size
  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("SD_MMC Card Size: %lluMB\n", cardSize);
  

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
  Serial.println(F("Antenna on at max gain."));
  Serial.println(F("Lay a tag flat on the spiral trace.\n"));
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

void loop() {
  if (cardPresent() && rfid.PICC_ReadCardSerial()) {
    String uid = uidString();

    static String lastUid = ""; //Use static so that it doesnt get erased every loop
    static unsigned long lastSeen = 0;

    if (uid != lastUid || millis() - lastSeen > status_time) { //Only prints if the card is diff or 3 secs have passed
      Serial.print(F("UID: ")); Serial.print(uid);
      Serial.print(F("   type: "));
      Serial.println(rfid.PICC_GetTypeName(rfid.PICC_GetType(rfid.uid.sak)));
    }
    lastUid  = uid; //Saved outside  the loop so that it refreshes everytime whether it prints or not.
    lastSeen = millis();

    rfid.PICC_HaltA(); //Tells the card to stop responding so the reader can scan the next one
    rfid.PCD_StopCrypto1(); //Clears the encryption state of the reader when it reads the card
  }

  //Periodically prints to show that the reader is still online
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat > status_time) {
    Serial.println(F("...listening"));
    lastBeat = millis();
  }
}

// put function definitions here:

