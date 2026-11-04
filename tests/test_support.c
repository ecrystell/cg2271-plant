#include <stddef.h>
void *memset(void *v, int c, size_t n)
{
    unsigned char *p = v;
    while (n--) *p++ = (unsigned char)c;
    return v;
}
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    for (size_t i = 0; i < n; i++) a[i] = b[i];
    return d;
}
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
