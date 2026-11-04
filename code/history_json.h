#ifndef HISTORY_JSON_H
#define HISTORY_JSON_H

#include <Arduino.h>
#include <stdint.h>
#include <string.h>

// Incremental UTF-8 validation; no heap allocation and no replacement/truncation.
class HistoryUtf8 {
  uint32_t value_ = 0, minimum_ = 0;
  uint8_t remaining_ = 0;
public:
  bool complete() const { return remaining_ == 0; }
  bool consume(uint8_t c) {
    if (remaining_) {
      if ((c & 0xC0) != 0x80) return false;
      value_ = (value_ << 6) | (c & 0x3F);
      if (--remaining_) return true;
      return value_ >= minimum_ && value_ <= 0x10FFFF && !(value_ >= 0xD800 && value_ <= 0xDFFF);
    }
    if (c < 0x80) return true;
    if (c >= 0xC2 && c <= 0xDF) { value_ = c & 0x1F; minimum_ = 0x80; remaining_ = 1; }
    else if (c >= 0xE0 && c <= 0xEF) { value_ = c & 0x0F; minimum_ = 0x800; remaining_ = 2; }
    else if (c >= 0xF0 && c <= 0xF4) { value_ = c & 7; minimum_ = 0x10000; remaining_ = 3; }
    else return false;
    return true;
  }
};

// The history format is a flat object of string fields: t/s/m (SMS), t/n (call).
// Validate directly while scanning file chunks, without retaining whole JSON lines.
class HistoryJsonLine {
  enum State { Start, KeyStart, Key, Colon, ValueStart, Value, AfterValue, End } state_ = Start;
  uint8_t expected_, seen_ = 0, key_ = 0, unicode_ = 0;
  bool valid_ = true, escape_ = false;
  HistoryUtf8 utf8_;
  static bool space(uint8_t c) { return c == ' ' || c == '\t' || c == '\r'; }
public:
  explicit HistoryJsonLine(bool sms) : expected_(sms ? 7 : 9) {}
  bool complete() const { return valid_ && state_ == End && seen_ == expected_; }
  void consume(uint8_t c) {
    if (!valid_) return;
    if (state_ == Value) {
      if (unicode_) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) valid_ = false;
        --unicode_;
      } else if (escape_) {
        escape_ = false;
        if (c == 'u') unicode_ = 4;
        else if (!strchr("\"\\/bfnrt", c) || c == 0) valid_ = false;
      } else if (!utf8_.complete()) {
        valid_ = utf8_.consume(c);
      } else if (c == '"') state_ = AfterValue;
      else if (c == '\\') escape_ = true;
      else valid_ = c >= 0x20 && utf8_.consume(c);
      return;
    }
    if (space(c) && state_ != Key) return;
    switch (state_) {
      case Start: valid_ = c == '{'; state_ = KeyStart; break;
      case KeyStart: valid_ = c == '"'; key_ = 0; state_ = Key; break;
      case Key:
        if (c == '"') {
          valid_ = key_ && (expected_ & key_) && !(seen_ & key_);
          seen_ |= key_; state_ = Colon;
        } else if (key_) valid_ = false;
        else { key_ = c == 't' ? 1 : c == 's' ? 2 : c == 'm' ? 4 : c == 'n' ? 8 : 0; valid_ = key_ != 0; }
        break;
      case Colon: valid_ = c == ':'; state_ = ValueStart; break;
      case ValueStart: valid_ = c == '"'; state_ = Value; break;
      case AfterValue:
        if (c == ',') state_ = KeyStart;
        else if (c == '}') state_ = End;
        else valid_ = false;
        break;
      default: valid_ = false; break;
    }
  }
};

inline bool historyQuotedSize(const char* text, size_t limit, size_t& encoded) {
  if (!text) return false;
  const size_t length = strnlen(text, limit + 1);
  if (length > limit) return false;
  HistoryUtf8 utf8;
  encoded = 2;
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    if (!utf8.consume(c)) return false;
    encoded += c < 0x20 ? 6 : c == '"' || c == '\\' ? 2 : 1;
  }
  return utf8.complete();
}

inline bool historyAppendQuoted(String& output, const char* text) {
  if (!output.concat("\"", 1)) return false;
  for (const char* p = text; *p; ++p) {
    const uint8_t c = static_cast<uint8_t>(*p);
    if (c < 0x20) {
      const char* hex = "0123456789abcdef";
      const char escaped[] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
      if (!output.concat(escaped, sizeof(escaped))) return false;
    } else {
      if ((c == '"' || c == '\\') && !output.concat("\\", 1)) return false;
      if (!output.concat(p, 1)) return false;
    }
  }
  return output.concat("\"", 1);
}

#endif // HISTORY_JSON_H
