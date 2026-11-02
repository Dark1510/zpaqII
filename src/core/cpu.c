/* zpaqII - runtime CPU feature detection.
 *
 * The portable C implementations are always compiled in; this only decides
 * which one the hot loops use.  Every supported platform answers with its own
 * mechanism, and "no features" is always a valid answer.
 */
#include "zq_internal.h"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || \
    defined(_M_IX86)
#define ZQ_CPU_X86 1
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define ZQ_CPU_ARM64 1
#endif

#if defined(ZQ_CPU_X86)
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif
#endif

#if defined(ZQ_CPU_ARM64) && defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_SHA1
#define HWCAP_SHA1 (1 << 5)
#endif
#ifndef HWCAP_SHA2
#define HWCAP_SHA2 (1 << 6)
#endif
#endif

#if defined(ZQ_CPU_ARM64) && defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#if defined(ZQ_CPU_ARM64) && defined(_WIN32)
#include <windows.h>
#endif

static unsigned zq_cpu_probe(void) {
  unsigned f = 0;

#if defined(ZQ_CPU_X86)
  /* CPUID leaf 7, sub-leaf 0, EBX bit 29 = SHA extensions. */
#if defined(_MSC_VER)
  {
    int r[4];
    __cpuidex(r, 7, 0);
    if (r[1] & (1 << 29)) f |= ZQ_CPU_SHA1;
  }
#elif defined(__GNUC__) || defined(__clang__)
  {
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid_max(0, 0) >= 7) {
      __cpuid_count(7, 0, eax, ebx, ecx, edx);
      if (ebx & (1u << 29)) f |= ZQ_CPU_SHA1;
    }
  }
#endif
#endif

#if defined(ZQ_CPU_ARM64)
#if defined(__linux__)
  {
    unsigned long hw = getauxval(AT_HWCAP);
    if (hw & HWCAP_SHA1) f |= ZQ_CPU_SHA1;
    if (hw & HWCAP_SHA2) f |= ZQ_CPU_SHA2;
  }
#elif defined(__APPLE__)
  {
    int v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname("hw.optional.arm.FEAT_SHA1", &v, &len, NULL, 0) == 0 &&
        v)
      f |= ZQ_CPU_SHA1;
  }
#elif defined(_WIN32)
  {
    if (IsProcessorFeaturePresent(
            PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE))
      f |= ZQ_CPU_SHA1 | ZQ_CPU_SHA2;
  }
#endif
#endif

  return f;
}

unsigned zq_cpu_features(void) {
  /* Computed once.  Two threads may race here and compute the same value. */
  static unsigned cache = 0;
  static int done = 0;
  if (!done) {
    cache = zq_cpu_probe();
    done = 1;
  }
  return cache;
}
