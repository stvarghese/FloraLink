#include <stdio.h>
#include <ctype.h>
#include "commonutils.h"
#include <string.h>

void anonymize_string(char *str, uint8_t pos, size_t len)
{
    if (str == NULL || len == 0)
        return;
    size_t str_len = strlen(str);
    if (pos >= str_len)
        return; // nothing to anonymize
    size_t end = (pos + len < str_len) ? (pos + len) : str_len;
    for (size_t i = pos; i < end; i++)
    {
        str[i] = '*';
    }
}

// Trim whitespace from both ends of a string (in-place)
void str_trim(char *str)
{
    if (!str)
        return;
    // Trim leading
    char *start = str;
    while (isspace((unsigned char)*start))
        start++;
    if (start != str)
        memmove(str, start, strlen(start) + 1);
    // Trim trailing
    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1]))
        str[--len] = '\0';
}

// Print a buffer as a hex dump (for debugging)
void hex_dump(const void *buf, size_t len)
{
    const uint8_t *data = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++)
    {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0)
            printf("\n");
    }
    if (len % 16 != 0)
        printf("\n");
}

// Safe strncpy that always null-terminates
void safe_strncpy(char *dst, const char *src, size_t dst_size)
{
    if (dst_size == 0)
        return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

// Format a MAC address as a string (XX:XX:XX:XX:XX:XX)
void format_mac(const uint8_t mac[6], char *out_str, size_t out_str_len)
{
    if (out_str_len < 18)
    {
        if (out_str_len > 0)
            out_str[0] = '\0';
        return;
    }
    snprintf(out_str, out_str_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Convert string to int with error checking. Returns 0 on success, -1 on error.
int str_to_int(const char *str, int *out)
{
    if (!str || !out)
        return -1;
    char *endptr;
    long val = strtol(str, &endptr, 10);
    if (*endptr != '\0')
        return -1;
    *out = (int)val;
    return 0;
}

// Convert int to string. Returns pointer to buf, or NULL on error.
char *int_to_str(int value, char *buf, size_t bufsize)
{
    if (!buf || bufsize == 0)
        return NULL;
    snprintf(buf, bufsize, "%d", value);
    return buf;
}

// URL encode (simple, encodes space as %20, etc.)
int url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t si = 0, di = 0;
    if (!src || !dst || dst_size == 0)
        return -1;
    while (src[si] && di + 4 < dst_size)
    {
        unsigned char c = (unsigned char)src[si];
        if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
        {
            dst[di++] = c;
        }
        else
        {
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0xF];
        }
        si++;
    }
    dst[di] = '\0';
    return (int)di;
}

// URL decode (simple, stops at first invalid encoding)
int url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t si = 0, di = 0;
    if (!src || !dst || dst_size == 0)
        return -1;
    while (src[si] && di + 1 < dst_size)
    {
        if (src[si] == '%' && src[si + 1] && src[si + 2])
        {
            char hex[3] = {src[si + 1], src[si + 2], 0};
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 3;
        }
        else if (src[si] == '+')
        {
            dst[di++] = ' ';
            si++;
        }
        else
        {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
    return (int)di;
}
