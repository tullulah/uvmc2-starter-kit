#ifndef _AAE_CTYPE_H
#define _AAE_CTYPE_H
/* Freestanding ctype shim. AAE #includes this but the Asteroids file set uses
 * none of its functions; provide the common ones as constexpr-style inlines. */
static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isalpha(int c) { return (c|32) >= 'a' && (c|32) <= 'z'; }
static inline int isalnum(int c) { return isdigit(c) || isalpha(c); }
static inline int isspace(int c) { return c==' '||c=='\t'||c=='\n'||c=='\r'||c=='\f'||c=='\v'; }
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int toupper(int c) { return islower(c) ? c - 32 : c; }
static inline int tolower(int c) { return isupper(c) ? c + 32 : c; }
#endif
