#ifndef WEB_STREAM_H
#define WEB_STREAM_H

#include <stddef.h>
#include <string.h>

struct WebTemplateValue {
  const char* token;
  const char* value;
  bool stripNewlines = false;
};

// The sink must consume the bytes synchronously. No heap allocation and no
// zero-length writes: only the HTTP handler may send the terminating chunk.
class WebStream {
public:
  static const size_t BUFFER_SIZE = 1024;
  typedef bool (*Sink)(const char*, size_t);

  explicit WebStream(Sink sink) : sink_(sink), used_(0), ok_(true) {}

  bool flush() {
    if (!ok_) return false;
    if (used_ != 0) {
      ok_ = sink_(buffer_, used_);
      used_ = 0;
    }
    return ok_;
  }

  bool write(const char* data, size_t length) {
    while (length && ok_) {
      size_t count = BUFFER_SIZE - used_;
      if (count > length) count = length;
      memcpy(buffer_ + used_, data, count);
      used_ += count;
      data += count;
      length -= count;
      if (used_ == BUFFER_SIZE) flush();
    }
    return ok_;
  }

  bool text(const char* value) {
    return write(value, strlen(value));
  }

  bool value(const char* data, bool stripNewlines) {
    if (!stripNewlines) return text(data);
    while (*data && ok_) {
      size_t length = strcspn(data, "\r\n");
      write(data, length);
      data += length;
      if (*data) ++data;
    }
    return ok_;
  }

  // Scan only the original template. Percent escapes, CSS percentages, modulo
  // operators and unknown tokens remain literal; replacement text is opaque.
  bool render(const char* data, size_t length,
              const WebTemplateValue* values, size_t count) {
    const char* end = data + length;
    while (data < end && ok_) {
      const char* percent = static_cast<const char*>(memchr(data, '%', end - data));
      if (!percent) return write(data, end - data);
      if (!write(data, percent - data)) return false;
      bool matched = false;
      for (size_t i = 0; i < count; ++i) {
        size_t tokenLength = strlen(values[i].token);
        if (tokenLength && tokenLength <= static_cast<size_t>(end - percent)
            && memcmp(percent, values[i].token, tokenLength) == 0) {
          value(values[i].value, values[i].stripNewlines);
          data = percent + tokenLength;
          matched = true;
          break;
        }
      }
      if (!matched) {
        write(percent, 1);
        data = percent + 1;
      }
    }
    return ok_;
  }

private:
  Sink sink_;
  char buffer_[BUFFER_SIZE];
  size_t used_;
  bool ok_;
};

#endif
