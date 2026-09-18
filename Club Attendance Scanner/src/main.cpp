#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>

#define PIN_SCK   41
#define PIN_MOSI  42
#define PIN_MISO   2
#define PIN_SS     1
#define PIN_RST   21

#define status_time 3000 //Used to let user know the reader is still active

MFRC522 rfid(PIN_SS, PIN_RST); //Initializes reader object

void setup() {
  //Initialzes Reader, SPI, and serial
  Serial.begin(115200);
  delay(1000); //Delay to allow hardware check to pass without getting cutoff by startup
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_SS);
  rfid.PCD_Init();
  delay(50);

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

