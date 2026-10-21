#include "http_util.h"

#include "root_cas.h"

bool httpsConnect(WiFiClientSecure &client, const char *host) {
  client.setCACert(ROOT_CAS);
  client.setTimeout(20);  // segundos
  client.setHandshakeTimeout(15);
  return client.connect(host, 443);
}

bool writeChunk(WiFiClient &client, const char *data, size_t len) {
  if (!len) return true;
  char head[12];
  int n = snprintf(head, sizeof(head), "%x\r\n", (unsigned)len);
  return client.write((const uint8_t *)head, n) == (size_t)n &&
         client.write((const uint8_t *)data, len) == len && client.write((const uint8_t *)"\r\n", 2) == 2;
}

bool writeChunk(WiFiClient &client, const String &s) { return writeChunk(client, s.c_str(), s.length()); }

bool endChunks(WiFiClient &client) { return client.print("0\r\n\r\n") == 5; }

// ---------------------------------------------------------------- resposta

int HttpResponse::readByte() {
  uint32_t start = millis();
  while (!c_.available()) {
    if (!c_.connected()) return -1;
    if (millis() - start > timeoutMs_) return -1;
    delay(2);
  }
  return c_.read();
}

bool HttpResponse::readRawLine(String &line, size_t maxLen) {
  line = "";
  while (true) {
    int b = readByte();
    if (b < 0) return line.length() > 0;
    if (b == '\n') break;
    if (b != '\r' && line.length() < maxLen) line += (char)b;
  }
  return true;
}

int HttpResponse::begin(uint32_t timeoutMs) {
  timeoutMs_ = timeoutMs;
  String line;
  if (!readRawLine(line, 256) || !line.startsWith("HTTP/1.")) return -1;
  int status = line.substring(9, 12).toInt();
  while (readRawLine(line, 1024) && line.length()) {
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") > 0) chunked_ = true;
    if (lower.startsWith("content-length:")) contentLength_ = lower.substring(15).toInt();
  }
  remaining_ = chunked_ ? 0 : contentLength_;  // -1 = ate fechar a conexao
  return status;
}

int HttpResponse::read(uint8_t *buf, size_t len) {
  if (done_) return 0;
  if (chunked_ && remaining_ == 0) {
    String sizeLine;
    if (!readRawLine(sizeLine, 32)) return -1;
    if (!sizeLine.length() && !readRawLine(sizeLine, 32)) return -1;  // CRLF do chunk anterior
    remaining_ = strtol(sizeLine.c_str(), nullptr, 16);
    if (remaining_ == 0) {
      done_ = true;
      return 0;
    }
  }
  if (!chunked_ && remaining_ == 0) {
    done_ = true;
    return 0;
  }
  size_t want = len;
  if (remaining_ > 0 && (long)want > remaining_) want = remaining_;
  size_t got = 0;
  uint32_t start = millis();
  while (got == 0) {
    int avail = c_.available();
    if (avail > 0) {
      got = c_.read(buf, min((size_t)avail, want));
      break;
    }
    if (!c_.connected()) {
      done_ = true;
      return remaining_ < 0 ? 0 : -1;  // sem Content-Length, fechar = fim
    }
    if (millis() - start > timeoutMs_) return -1;
    delay(2);
  }
  if (remaining_ > 0) remaining_ -= got;
  return got;
}

bool HttpResponse::readLine(String &line, size_t maxLen) {
  line = "";
  uint8_t b;
  while (true) {
    int n = read(&b, 1);
    if (n <= 0) return line.length() > 0;
    if (b == '\n') return true;
    if (b != '\r' && line.length() < maxLen) line += (char)b;
  }
}

String HttpResponse::readAll(size_t maxLen) {
  String out;
  uint8_t buf[256];
  int n;
  while ((n = read(buf, sizeof(buf))) > 0) {
    for (int i = 0; i < n && out.length() < maxLen; i++) out += (char)buf[i];
  }
  return out;
}

// ---------------------------------------------------------------- base64

namespace {
const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void encodeTriple(const uint8_t *t, String &out) {
  out += B64[t[0] >> 2];
  out += B64[((t[0] & 0x03) << 4) | (t[1] >> 4)];
  out += B64[((t[1] & 0x0F) << 2) | (t[2] >> 6)];
  out += B64[t[2] & 0x3F];
}
}  // namespace

void Base64Encoder::feed(const uint8_t *data, size_t len, String &out) {
  out.reserve(out.length() + (len + carryLen_) * 4 / 3 + 4);
  size_t i = 0;
  while (carryLen_ && carryLen_ < 3 && i < len) carry_[carryLen_++] = data[i++];
  if (carryLen_ == 3) {
    encodeTriple(carry_, out);
    carryLen_ = 0;
  }
  for (; i + 3 <= len; i += 3) encodeTriple(data + i, out);
  while (i < len) carry_[carryLen_++] = data[i++];
}

void Base64Encoder::finish(String &out) {
  if (!carryLen_) return;
  uint8_t t[3] = {carry_[0], carryLen_ > 1 ? carry_[1] : (uint8_t)0, 0};
  out += B64[t[0] >> 2];
  out += B64[((t[0] & 0x03) << 4) | (t[1] >> 4)];
  out += carryLen_ > 1 ? B64[(t[1] & 0x0F) << 2] : '=';
  out += '=';
  carryLen_ = 0;
}

int Base64Decoder::feed(char c, uint8_t *out) {
  int v;
  if (c >= 'A' && c <= 'Z') v = c - 'A';
  else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
  else if (c >= '0' && c <= '9') v = c - '0' + 52;
  else if (c == '+') v = 62;
  else if (c == '/') v = 63;
  else return 0;  // '=', '\n', escapes etc.
  acc_ = (acc_ << 6) | v;
  bits_ += 6;
  if (bits_ >= 8) {
    bits_ -= 8;
    out[0] = (acc_ >> bits_) & 0xFF;
    return 1;
  }
  return 0;
}
