/* Applied on top of mbedTLS's default config (MBEDTLS_USER_CONFIG_FILE). */
#if defined(__EMSCRIPTEN__)
/* fastcomp (Samsung's Emscripten 1.39) miscompiles the empty inline-asm optimization
 * barriers in mbedTLS's constant-time code: asm2wasm then aborts on an undeclared local.
 * Plain C fallbacks are used without MBEDTLS_HAVE_ASM. */
#undef MBEDTLS_HAVE_ASM
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_AESCE_C
#undef MBEDTLS_PADLOCK_C
/* Entropy from crypto.getRandomValues() on the calling thread instead of /dev/urandom,
 * which pthreads would proxy to the browser main thread (src/platform/mbedtls_entropy_wasm.c). */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#endif
