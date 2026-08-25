// Alpine/musl static-build-only compatibility shim -- NOT part of the
// regular src/ build (glibc already provides these symbols natively, so
// linking this into a normal Ubuntu/glibc build would fail with
// "multiple definition"; it's compiled and linked only by docker/Dockerfile).
//
// On Alpine, g++'s libstdc++ headers still emit calls to glibc-style
// _FORTIFY_SOURCE wrapper symbols (__printf_chk, __snprintf_chk, etc.) at
// -O2, and libc.musl's own strtol is called via the C23-renamed
// __isoc23_strtol entry point -- but musl's *static* libc archive doesn't
// provide any of these, only glibc does. These are thin pass-throughs to
// the real (non-"_chk") functions: we don't rely on _FORTIFY_SOURCE's
// extra runtime bounds-checking for correctness anywhere in nshgeoip (every
// buffer/length is already explicitly sized and checked in src/), so
// dropping that extra check changes nothing observable, it just satisfies
// the linker.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

extern "C" {

int __printf_chk(int /*flag*/, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vprintf(format, ap);
    va_end(ap);
    return ret;
}

int __fprintf_chk(FILE *stream, int /*flag*/, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vfprintf(stream, format, ap);
    va_end(ap);
    return ret;
}

int __snprintf_chk(char *s, size_t maxlen, int /*flag*/, size_t /*slen*/, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(s, maxlen, format, ap);
    va_end(ap);
    return ret;
}

long __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}

} // extern "C"
