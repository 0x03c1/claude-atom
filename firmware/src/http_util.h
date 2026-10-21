#pragma once
// HTTP/1.1 minimo sobre TLS, feito para streaming com pouca RAM:
// corpo de requisicao chunked, leitura de corpo chunked ou com Content-Length,
// e base64 incremental (codifica/decodifica em pedacos).

#include <Arduino.h>
#include <WiFiClientSecure.h>

bool httpsConnect(WiFiClientSecure &client, const char *host);

// Envia um pedaco no formato Transfer-Encoding: chunked.
bool writeChunk(WiFiClient &client, const char *data, size_t len);
bool writeChunk(WiFiClient &client, const String &s);
bool endChunks(WiFiClient &client);

class HttpResponse {
 public:
  explicit HttpResponse(WiFiClient &client) : c_(client) {}

  // Le status e cabecalhos. Retorna o codigo HTTP (ou -1 em timeout/erro).
  int begin(uint32_t timeoutMs = 20000);
  long contentLength() const { return contentLength_; }

  // Le ate `len` bytes do corpo (ja sem chunking). 0 = fim, -1 = erro.
  int read(uint8_t *buf, size_t len);
  // Le uma linha do corpo (sem \r\n). false no fim do corpo.
  bool readLine(String &line, size_t maxLen = 4096);
  // Le o corpo inteiro (limitado) para respostas pequenas.
  String readAll(size_t maxLen = 8192);

 private:
  int readByte();
  bool readRawLine(String &line, size_t maxLen);

  WiFiClient &c_;
  bool chunked_ = false;
  long contentLength_ = -1;
  long remaining_ = 0;  // bytes restantes do chunk atual ou do corpo
  bool done_ = false;
  uint32_t timeoutMs_ = 20000;
};

// Base64 incremental: alimente bytes, receba texto. Guarda o resto (<3 bytes).
class Base64Encoder {
 public:
  // Codifica `len` bytes e acrescenta em `out`.
  void feed(const uint8_t *data, size_t len, String &out);
  void finish(String &out);

 private:
  uint8_t carry_[3];
  size_t carryLen_ = 0;
};

class Base64Decoder {
 public:
  // Decodifica um caractere; quando completa bytes, escreve em out e retorna quantos (0-3).
  int feed(char c, uint8_t *out);

 private:
  uint32_t acc_ = 0;
  int bits_ = 0;
};
