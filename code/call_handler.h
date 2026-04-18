#ifndef CALL_HANDLER_H
#define CALL_HANDLER_H

#include <Arduino.h>

String parseClipNumber(const String& line);
bool parseClccInfo(const String& line, String& number, int& dir, int& stat);
void processIncomingCall(const char* caller);
bool handleCallUrc(const String& line);

#endif // CALL_HANDLER_H

