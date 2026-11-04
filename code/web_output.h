#ifndef WEB_OUTPUT_H
#define WEB_OUTPUT_H

#include "web_stream.h"

// Raw is only for firmware-owned markup, never configuration or remote data.
enum class WebEncoding { HtmlText, HtmlAttribute, JsString, Raw };
struct WebOutputValue {
  const char* token;
  const char* value;
  WebEncoding encoding = WebEncoding::HtmlText;
};

class WebOutput : public WebStream {
public:
  explicit WebOutput(Sink sink) : WebStream(sink) {}

  // JsString emits a complete quoted literal, safe inside an HTML script element.
  // Escaping '<' also prevents </script>, <!-- and script double-escape transitions.
  bool encoded(const char* data, WebEncoding encoding = WebEncoding::HtmlText) {
    if (!data) data = "";
    if (encoding == WebEncoding::Raw) return text(data);
    const bool js = encoding == WebEncoding::JsString;
    if (js && !text("\"")) return false;
    const char* end = data + strlen(data);
    const char* run = data;
    while (data < end) {
      const unsigned char c = static_cast<unsigned char>(*data);
      const char* escaped = nullptr;
      char control[] = "\\u0000";
      size_t consumed = 1;
      if (js) {
        switch (c) {
          case '"': escaped = "\\\""; break;
          case '\'': escaped = "\\u0027"; break;
          case '\\': escaped = "\\\\"; break;
          case '<': escaped = "\\u003c"; break;
          case '>': escaped = "\\u003e"; break;
          case '&': escaped = "\\u0026"; break;
          case '\n': escaped = "\\n"; break;
          case '\r': escaped = "\\r"; break;
          case '\t': escaped = "\\t"; break;
          default:
            if (c < 0x20) {
              const char* hex = "0123456789abcdef";
              control[4] = hex[c >> 4]; control[5] = hex[c & 15];
              escaped = control;
            } else if (end - data >= 3 && c == 0xe2 &&
                       static_cast<unsigned char>(data[1]) == 0x80 &&
                       (static_cast<unsigned char>(data[2]) == 0xa8 ||
                        static_cast<unsigned char>(data[2]) == 0xa9)) {
              escaped = static_cast<unsigned char>(data[2]) == 0xa8 ? "\\u2028" : "\\u2029";
              consumed = 3;
            }
        }
      } else {
        switch (c) {
          case '&': escaped = "&amp;"; break;
          case '<': escaped = "&lt;"; break;
          case '>': escaped = "&gt;"; break;
          case '\r': escaped = "&#13;"; break;
          case '"': if (encoding == WebEncoding::HtmlAttribute) escaped = "&quot;"; break;
          case '\'': if (encoding == WebEncoding::HtmlAttribute) escaped = "&#39;"; break;
          case '\n': if (encoding == WebEncoding::HtmlAttribute) escaped = "&#10;"; break;
          case '\t': if (encoding == WebEncoding::HtmlAttribute) escaped = "&#9;"; break;
        }
      }
      if (escaped) {
        if (!write(run, data - run) || !text(escaped)) return false;
        data += consumed;
        run = data;
      } else {
        ++data;
      }
    }
    return write(run, end - run) && (!js || text("\""));
  }

  // Only scan the original template: user text resembling %TOKENS% is opaque.
  bool render(const char* data, size_t length, const WebOutputValue* values, size_t count) {
    const char* end = data + length;
    while (data < end) {
      const char* percent = static_cast<const char*>(memchr(data, '%', end - data));
      if (!percent) return write(data, end - data);
      if (!write(data, percent - data)) return false;
      bool matched = false;
      for (size_t i = 0; i < count; ++i) {
        const size_t tokenLength = strlen(values[i].token);
        if (tokenLength && tokenLength <= static_cast<size_t>(end - percent) &&
            memcmp(percent, values[i].token, tokenLength) == 0) {
          if (!encoded(values[i].value, values[i].encoding)) return false;
          data = percent + tokenLength;
          matched = true;
          break;
        }
      }
      if (!matched) {
        if (!write(percent, 1)) return false;
        data = percent + 1;
      }
    }
    return true;
  }
};

#endif // WEB_OUTPUT_H
