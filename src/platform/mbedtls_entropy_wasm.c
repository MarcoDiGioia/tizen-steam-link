/*
 * mbedTLS entropy for the WASM build (MBEDTLS_ENTROPY_HARDWARE_ALT).
 *
 * The default source reads /dev/urandom. In Emscripten's pthreads build every file syscall
 * from a worker is proxied, synchronously, to the browser main thread, and ihslib seeds a
 * fresh DRBG for each encrypted control message. crypto.getRandomValues() runs on the
 * calling worker instead. It can't fill a view of the shared heap directly, hence the copy.
 */
#include <emscripten.h>
#include <stddef.h>

#include "mbedtls/entropy.h"

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void) data;
    int ok = EM_ASM_INT(
            {
                var c = typeof crypto !== 'undefined' ? crypto : null;
                if (!c || !c.getRandomValues) return 0;
                var tmp = new Uint8Array($1);
                for (var off = 0; off < $1; off += 65536) {
                    c.getRandomValues(tmp.subarray(off, Math.min($1, off + 65536)));
                }
                HEAPU8.set(tmp, $0);
                return 1;
            },
            output, len);
    if (!ok) {
        *olen = 0;
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    *olen = len;
    return 0;
}
