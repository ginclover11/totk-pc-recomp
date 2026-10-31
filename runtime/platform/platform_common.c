// The parts of platform.h written in terms of the rest of it, the same on every system.
#include "platform.h"

// The owner is read without the lock: only this thread can have stored its own id there, so the comparison is decided.
void plat_rmutex_lock(PlatRecMutex *r) {
    uint32_t me = plat_thread_id();
    if (__atomic_load_n(&r->owner, __ATOMIC_RELAXED) != me) {
        plat_mutex_lock(&r->m);
        __atomic_store_n(&r->owner, me, __ATOMIC_RELAXED);
    }
    r->depth++;
}

int plat_rmutex_trylock(PlatRecMutex *r) {
    uint32_t me = plat_thread_id();
    if (__atomic_load_n(&r->owner, __ATOMIC_RELAXED) != me) {
        if (!plat_mutex_trylock(&r->m)) return 0;
        __atomic_store_n(&r->owner, me, __ATOMIC_RELAXED);
    }
    r->depth++;
    return 1;
}

void plat_rmutex_unlock(PlatRecMutex *r) {
    if (--r->depth) return;
    __atomic_store_n(&r->owner, 0, __ATOMIC_RELAXED);
    plat_mutex_unlock(&r->m);
}

size_t plat_utf8_to_utf16(const char *str, uint16_t *out, size_t cap) {
    const unsigned char *s = (const unsigned char *)str;
    size_t n = 0;
    for (;;) {
        uint32_t c = *s++;
        int more = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : -1;
        if (more < 0) return 0;
        if (more) c &= 0x3fu >> more;
        for (int i = 0; i < more; i++) {
            if ((*s & 0xc0) != 0x80) return 0;
            c = c << 6 | (*s++ & 0x3f);
        }
        if (c >= 0x10000) {
            if (c > 0x10ffff || n + 2 > cap) return 0;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xd800 + (c >> 10));
            out[n++] = (uint16_t)(0xdc00 + (c & 0x3ff));
            continue;
        }
        if (n + 1 > cap) return 0;
        out[n++] = (uint16_t)c;
        if (!c) return n;
    }
}
