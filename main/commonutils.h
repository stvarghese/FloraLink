#ifndef COMMONUTILS_H
#define COMMONUTILS_H

#include <stddef.h>
#include <stdint.h>

// Anonymize a string from position 'pos' for 'len' characters (replace with '*')
void anonymize_string(char *str, uint8_t pos, size_t len);

// Trim whitespace from both ends of a string (in-place)
void str_trim(char *str);

// Print a buffer as a hex dump (for debugging)
void hex_dump(const void *buf, size_t len);

// Safe strncpy that always null-terminates
void safe_strncpy(char *dst, const char *src, size_t dst_size);

// Format a MAC address as a string (XX:XX:XX:XX:XX:XX)
void format_mac(const uint8_t mac[6], char *out_str, size_t out_str_len);

// Convert string to int with error checking. Returns 0 on success, -1 on error.
int str_to_int(const char *str, int *out);

// Convert int to string. Returns pointer to buf, or NULL on error.
char *int_to_str(int value, char *buf, size_t bufsize);

// URL encode/decode helpers
int url_encode(const char *src, char *dst, size_t dst_size);
int url_decode(const char *src, char *dst, size_t dst_size);

#endif // COMMONUTILS_H
