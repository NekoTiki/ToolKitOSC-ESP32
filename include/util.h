#pragma once
#include <stddef.h>
#include <stdint.h>

// Standard base64 with padding. Returns false if `out` is too small (needs 4 * ceil(len / 3) + 1).
bool base64Encode(const uint8_t* data, size_t len, char* out, size_t outSize);

// Compares two strings in time that doesn't depend on where they differ.
bool secureEquals(const char* a, const char* b);

// strncpy that always terminates.
void copyString(char* dst, size_t dstSize, const char* src);
