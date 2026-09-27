#pragma once
#include <Arduino.h>

struct Tap {
    String uid;
    String type;
};

bool scannerBegin();

bool readTap(Tap &tap);

String timestamp();

bool logscan(const String& ts, const String& uid);

bool loggingReady();
