#include "util.h"

#include <string.h>

bool base64Encode(const uint8_t* data, size_t len, char* out, size_t outSize) {
  static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  size_t needed = 4 * ((len + 2) / 3) + 1;
  if (outSize < needed) return false;

  size_t o = 0;
  for (size_t i = 0; i < len; i += 3) {
    uint32_t chunk = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) chunk |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) chunk |= data[i + 2];

    out[o++] = kAlphabet[(chunk >> 18) & 0x3f];
    out[o++] = kAlphabet[(chunk >> 12) & 0x3f];
    out[o++] = i + 1 < len ? kAlphabet[(chunk >> 6) & 0x3f] : '=';
    out[o++] = i + 2 < len ? kAlphabet[chunk & 0x3f] : '=';
  }
  out[o] = '\0';
  return true;
}

bool secureEquals(const char* a, const char* b) {
  size_t la = strlen(a);
  size_t lb = strlen(b);
  unsigned char diff = la != lb;
  for (size_t i = 0; i < la; i++) diff |= a[i] ^ b[i % (lb ? lb : 1)];
  return diff == 0 && lb > 0;
}

void copyString(char* dst, size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  size_t n = strlen(src);
  if (n >= dstSize) n = dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}
