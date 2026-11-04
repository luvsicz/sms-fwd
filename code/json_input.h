#ifndef JSON_INPUT_H
#define JSON_INPUT_H

#include <Arduino.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Strict, non-recursive JSON subset: one object containing strings, bools,
// native ints or string arrays. All fields are validated, including unused ones.
// Decoded text lives in one bounded String; no input references are retained.
class JsonInput {
public:
  enum : size_t {
    MAX_INPUT_BYTES = 8192,
    MAX_FIELDS = 16,
    MAX_LIST_ITEMS = 100
  };

  JsonInput() : count_(0), valid_(false) {}
  // Prevent implicit String copies from silently losing data on allocation failure.
  JsonInput(const JsonInput&) = delete;
  JsonInput& operator=(const JsonInput&) = delete;

  // A failed parse invalidates the entire result, including any previous input.
  bool parse(const String& input) {
    valid_ = false;
    count_ = 0;
    storage_.remove(0);
    const size_t length = input.length();
    const char* data = input.c_str();
    if (length < 2 || length > MAX_INPUT_BYTES || !data
        || memchr(data, '\0', length)
        || !storage_.reserve(static_cast<unsigned int>(length))) return false;
    Reader reader(data, length, *this);
    if (!reader.object()) {
      count_ = 0;
      storage_.remove(0);
      return false;
    }
    valid_ = true;
    return true;
  }

  // Getters are strict (no coercion); missing keys/type mismatches leave out alone.
  bool getString(const char* key, String& out) const {
    const Field* field = find(key);
    return field && field->type == Type::TEXT && copyText(*field, out);
  }

  bool getBool(const char* key, bool& out) const {
    const Field* field = find(key);
    if (!field || field->type != Type::BOOLEAN) return false;
    out = field->scalar != 0;
    return true;
  }

  bool getInt(const char* key, int& out) const {
    const Field* field = find(key);
    if (!field || field->type != Type::INTEGER) return false;
    out = field->scalar;
    return true;
  }

  // Arrays are joined with commas, including empty items; strings pass through.
  bool getStringList(const char* key, String& out) const {
    const Field* field = find(key);
    return field && (field->type == Type::TEXT || field->type == Type::LIST)
           && copyText(*field, out);
  }

  bool has(const char* key) const { return find(key) != nullptr; }

private:
  enum class Type : uint8_t { TEXT, BOOLEAN, INTEGER, LIST };
  struct Field {
    size_t keyOffset;
    size_t keyLength;
    size_t textOffset;
    size_t textLength;
    int scalar;
    Type type;
  };

  const Field* find(const char* key) const {
    if (!valid_ || !key) return nullptr;
    size_t length = 0;
    while (length <= MAX_INPUT_BYTES && key[length]) ++length;
    if (length > MAX_INPUT_BYTES) return nullptr;
    for (size_t i = 0; i < count_; ++i) {
      const Field& field = fields_[i];
      if (length == field.keyLength
          && memcmp(storage_.c_str() + field.keyOffset, key, length) == 0)
        return &field;
    }
    return nullptr;
  }

  bool copyText(const Field& field, String& out) const {
    // reserve(0) is intentional: even an empty result must not hide OOM.
    if (!out.reserve(static_cast<unsigned int>(field.textLength))) return false;
    out.remove(0);
    return field.textLength == 0
           || (out.concat(storage_.c_str() + field.textOffset,
                          static_cast<unsigned int>(field.textLength))
               && out.length() == field.textLength);
  }

  class Reader {
  public:
    Reader(const char* data, size_t length, JsonInput& owner)
      : data_(data), length_(length), pos_(0), owner_(owner) {}

    bool object() {
      space();
      if (!take('{')) return false;
      space();
      if (!take('}')) {
        for (;;) {
          if (owner_.count_ == MAX_FIELDS) return false;
          Field& field = owner_.fields_[owner_.count_];
          field.keyOffset = owner_.storage_.length();
          if (!string()) return false;
          field.keyLength = owner_.storage_.length() - field.keyOffset;
          for (size_t i = 0; i < owner_.count_; ++i) {
            const Field& previous = owner_.fields_[i];
            if (field.keyLength == previous.keyLength
                && memcmp(owner_.storage_.c_str() + field.keyOffset,
                          owner_.storage_.c_str() + previous.keyOffset,
                          field.keyLength) == 0) return false;
          }
          space();
          if (!take(':')) return false;
          space();
          field.textOffset = owner_.storage_.length();
          if (!value(field)) return false;
          field.textLength = owner_.storage_.length() - field.textOffset;
          ++owner_.count_;
          space();
          if (take('}')) break;
          if (!take(',')) return false;
          space();
        }
      }
      space();
      return pos_ == length_;
    }

  private:
    bool take(char c) {
      if (pos_ == length_ || data_[pos_] != c) return false;
      ++pos_;
      return true;
    }

    void space() {
      while (pos_ < length_) {
        const char c = data_[pos_];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++pos_;
      }
    }

    bool append(const char* data, size_t length) {
      const size_t before = owner_.storage_.length();
      return length <= MAX_INPUT_BYTES - before
             && (length == 0
                 || (owner_.storage_.concat(data, static_cast<unsigned int>(length))
                     && owner_.storage_.length() == before + length));
    }

    bool codepoint(uint32_t value) {
      if (!value || value > 0x10FFFFUL || (value >= 0xD800 && value <= 0xDFFF))
        return false;
      char bytes[4];
      size_t length;
      if (value < 0x80) {
        bytes[0] = static_cast<char>(value);
        length = 1;
      } else if (value < 0x800) {
        bytes[0] = static_cast<char>(0xC0 | (value >> 6));
        bytes[1] = static_cast<char>(0x80 | (value & 0x3F));
        length = 2;
      } else if (value < 0x10000UL) {
        bytes[0] = static_cast<char>(0xE0 | (value >> 12));
        bytes[1] = static_cast<char>(0x80 | ((value >> 6) & 0x3F));
        bytes[2] = static_cast<char>(0x80 | (value & 0x3F));
        length = 3;
      } else {
        bytes[0] = static_cast<char>(0xF0 | (value >> 18));
        bytes[1] = static_cast<char>(0x80 | ((value >> 12) & 0x3F));
        bytes[2] = static_cast<char>(0x80 | ((value >> 6) & 0x3F));
        bytes[3] = static_cast<char>(0x80 | (value & 0x3F));
        length = 4;
      }
      return append(bytes, length);
    }

    bool hex4(uint32_t& value) {
      if (length_ - pos_ < 4) return false;
      value = 0;
      for (size_t i = 0; i < 4; ++i) {
        const char c = data_[pos_++];
        unsigned int nibble;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else return false;
        value = (value << 4) | nibble;
      }
      return true;
    }

    bool unicode() {
      uint32_t value;
      if (!hex4(value)) return false;
      if (value >= 0xD800 && value <= 0xDBFF) {
        uint32_t low;
        if (!take('\\') || !take('u') || !hex4(low)
            || low < 0xDC00 || low > 0xDFFF) return false;
        value = 0x10000UL + ((value - 0xD800) << 10) + (low - 0xDC00);
      }
      return codepoint(value);
    }

    // Called after consuming a non-ASCII lead byte. Re-encode only canonical UTF-8.
    bool utf8(uint8_t lead) {
      uint32_t value;
      uint32_t minimum;
      size_t remaining;
      if (lead >= 0xC2 && lead <= 0xDF) {
        value = lead & 0x1F;
        minimum = 0x80;
        remaining = 1;
      } else if (lead >= 0xE0 && lead <= 0xEF) {
        value = lead & 0x0F;
        minimum = 0x800;
        remaining = 2;
      } else if (lead >= 0xF0 && lead <= 0xF4) {
        value = lead & 0x07;
        minimum = 0x10000UL;
        remaining = 3;
      } else return false;
      if (remaining > length_ - pos_) return false;
      while (remaining--) {
        const uint8_t next = static_cast<uint8_t>(data_[pos_++]);
        if ((next & 0xC0) != 0x80) return false;
        value = (value << 6) | (next & 0x3F);
      }
      return value >= minimum && codepoint(value);
    }

    bool string() {
      if (!take('"')) return false;
      while (pos_ < length_) {
        const uint8_t c = static_cast<uint8_t>(data_[pos_++]);
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c == '\\') {
          if (pos_ == length_) return false;
          const char escaped = data_[pos_++];
          char decoded;
          switch (escaped) {
            case '"': case '\\': case '/': decoded = escaped; break;
            case 'b': decoded = '\b'; break;
            case 'f': decoded = '\f'; break;
            case 'n': decoded = '\n'; break;
            case 'r': decoded = '\r'; break;
            case 't': decoded = '\t'; break;
            case 'u':
              if (!unicode()) return false;
              continue;
            default: return false;
          }
          if (!append(&decoded, 1)) return false;
        } else if (c < 0x80) {
          if (!append(data_ + pos_ - 1, 1)) return false;
        } else if (!utf8(c)) return false;
      }
      return false;
    }

    bool literal(const char* text, size_t length) {
      if (length > length_ - pos_ || memcmp(data_ + pos_, text, length) != 0)
        return false;
      pos_ += length;
      return true;
    }

    static bool digit(char c) { return c >= '0' && c <= '9'; }

    bool integer(int& result) {
      const bool negative = take('-');
      if (pos_ == length_ || !digit(data_[pos_])) return false;
      const size_t start = pos_;
      const unsigned int limit = negative
        ? static_cast<unsigned int>(-(INT_MIN + 1)) + 1U
        : static_cast<unsigned int>(INT_MAX);
      unsigned int magnitude = 0;
      while (pos_ < length_ && digit(data_[pos_])) {
        if (pos_ != start && data_[start] == '0') return false;
        const unsigned int next = static_cast<unsigned int>(data_[pos_] - '0');
        if (magnitude > (limit - next) / 10U) return false;
        magnitude = magnitude * 10U + next;
        ++pos_;
      }
      result = negative
        ? (magnitude == limit ? INT_MIN : -static_cast<int>(magnitude))
        : static_cast<int>(magnitude);
      return true; // The object parser enforces the following delimiter.
    }

    bool list() {
      if (!take('[')) return false;
      space();
      if (take(']')) return true;
      size_t items = 0;
      for (;;) {
        if (items == MAX_LIST_ITEMS || (items && !append(",", 1))) return false;
        if (!string()) return false;
        ++items;
        space();
        if (take(']')) return true;
        if (!take(',')) return false;
        space();
      }
    }

    bool value(Field& field) {
      if (pos_ == length_) return false;
      switch (data_[pos_]) {
        case '"':
          field.type = Type::TEXT;
          return string();
        case '[':
          field.type = Type::LIST;
          return list();
        case 't':
          field.type = Type::BOOLEAN;
          field.scalar = 1;
          return literal("true", 4);
        case 'f':
          field.type = Type::BOOLEAN;
          field.scalar = 0;
          return literal("false", 5);
        default:
          field.type = Type::INTEGER;
          return integer(field.scalar); // Rejects null, objects, floats and exponents.
      }
    }

    const char* data_;
    size_t length_;
    size_t pos_;
    JsonInput& owner_;
  };

  String storage_;
  Field fields_[MAX_FIELDS] = {};
  size_t count_;
  bool valid_;
};

#endif // JSON_INPUT_H
