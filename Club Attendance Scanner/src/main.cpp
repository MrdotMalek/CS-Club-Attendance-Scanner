#include <Arduino.h>
#include "scanner.h"

static const unsigned long status_time = 3000; //Used to let user know the reader is still active

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!scannerBegin()) {
    Serial.println(F("Scanner initialization failed. Halting."));
    while (true) delay(1000);
  }
}

void loop() {
  Tap tap;
  if (readTap(tap)) {
    String ts = timestamp();
    Serial.printf("%s UID: %s type: %s\n",
                  ts.c_str(), tap.uid.c_str(), tap.type.c_str());
    if (logScan(ts, tap.uid)) {
      Serial.println(F("Failed to log scan"));
    }
  }

  static unsigned long lastStatusTime = 0;
  if (millis() - lastStatusTime > status_time) {
    Serial.println(F("Scanner active"));
    lastStatusTime = millis();
  }
}

