/*
 * Lace
 * Platform Abstraction Layer
 *
 * Provides platform detection macros and common platform utilities.
 * Consolidates platform-specific code in one place.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_PLATFORM_H
#define LACE_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Platform Detection
 * ============================================================================
 */

/* Operating System */
#if defined(_WIN32) || defined(_WIN64)
#define LACE_OS_WINDOWS 1
#define LACE_OS_NAME "Windows"
#elif defined(__APPLE__) && defined(__MACH__)
#define LACE_OS_MACOS 1
#define LACE_OS_NAME "macOS"
#define LACE_OS_POSIX 1
#elif defined(__linux__)
#define LACE_OS_LINUX 1
#define LACE_OS_NAME "Linux"
#define LACE_OS_POSIX 1
#elif defined(__FreeBSD__)
#define LACE_OS_FREEBSD 1
#define LACE_OS_NAME "FreeBSD"
#define LACE_OS_POSIX 1
#elif defined(__NetBSD__)
#define LACE_OS_NETBSD 1
#define LACE_OS_NAME "NetBSD"
#define LACE_OS_POSIX 1
#elif defined(__OpenBSD__)
#define LACE_OS_OPENBSD 1
#define LACE_OS_NAME "OpenBSD"
#define LACE_OS_POSIX 1
#elif defined(__DragonFly__)
#define LACE_OS_DRAGONFLY 1
#define LACE_OS_NAME "DragonFly BSD"
#define LACE_OS_POSIX 1
#elif defined(__unix__)
#define LACE_OS_UNIX 1
#define LACE_OS_NAME "Unix"
#define LACE_OS_POSIX 1
#else
#define LACE_OS_UNKNOWN 1
#define LACE_OS_NAME "Unknown"
#endif

/* Architecture */
#if defined(__x86_64__) || defined(_M_X64)
#define LACE_ARCH_X64 1
#define LACE_ARCH_NAME "x86_64"
#elif defined(__i386__) || defined(_M_IX86)
#define LACE_ARCH_X86 1
#define LACE_ARCH_NAME "x86"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define LACE_ARCH_ARM64 1
#define LACE_ARCH_NAME "arm64"
#elif defined(__arm__) || defined(_M_ARM)
#define LACE_ARCH_ARM 1
#define LACE_ARCH_NAME "arm"
#else
#define LACE_ARCH_UNKNOWN 1
#define LACE_ARCH_NAME "unknown"
#endif

/* Compiler */
#if defined(__clang__)
#define LACE_COMPILER_CLANG 1
#define LACE_COMPILER_NAME "Clang"
#elif defined(__GNUC__)
#define LACE_COMPILER_GCC 1
#define LACE_COMPILER_NAME "GCC"
#elif defined(_MSC_VER)
#define LACE_COMPILER_MSVC 1
#define LACE_COMPILER_NAME "MSVC"
#else
#define LACE_COMPILER_UNKNOWN 1
#define LACE_COMPILER_NAME "Unknown"
#endif

/* ============================================================================
 * Platform-Specific Attributes
 * ============================================================================
 */

/* Export/Import for shared libraries */
#ifdef LACE_OS_WINDOWS
#ifdef LACE_BUILD_DLL
#define LACE_API __declspec(dllexport)
#elif defined(LACE_USE_DLL)
#define LACE_API __declspec(dllimport)
#else
#define LACE_API
#endif
#else
#ifdef LACE_BUILD_DLL
#define LACE_API __attribute__((visibility("default")))
#else
#define LACE_API
#endif
#endif

/* Inline hint */
#if defined(LACE_COMPILER_MSVC)
#define LACE_INLINE __forceinline
#else
#define LACE_INLINE static inline __attribute__((always_inline))
#endif

/* No-return attribute */
#if defined(LACE_COMPILER_MSVC)
#define LACE_NORETURN __declspec(noreturn)
#else
#define LACE_NORETURN __attribute__((noreturn))
#endif

/* Unused parameter/variable */
#define LACE_UNUSED(x) (void)(x)

/* Likely/Unlikely branch hints */
#if defined(LACE_COMPILER_GCC) || defined(LACE_COMPILER_CLANG)
#define LACE_LIKELY(x) __builtin_expect(!!(x), 1)
#define LACE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LACE_LIKELY(x) (x)
#define LACE_UNLIKELY(x) (x)
#endif

/* ============================================================================
 * Path Separators
 * ============================================================================
 */

#ifdef LACE_OS_WINDOWS
#define LACE_PATH_SEP '\\'
#define LACE_PATH_SEP_STR "\\"
#define LACE_PATH_LIST_SEP ';'
#else
#define LACE_PATH_SEP '/'
#define LACE_PATH_SEP_STR "/"
#define LACE_PATH_LIST_SEP ':'
#endif

/* ============================================================================
 * Platform Functions
 * ============================================================================
 */

/* Get the user's home directory path */
const char *platform_get_home_dir(void);

/* Get the application config directory (creates if needed) */
const char *platform_get_config_dir(void);

/* Get the application data directory (creates if needed) */
const char *platform_get_data_dir(void);

/* Check if a file exists */
bool platform_file_exists(const char *path);

/* Check if a directory exists */
bool platform_dir_exists(const char *path);

/* Create a directory (and parents if needed) */
bool platform_mkdir(const char *path);

/* Replace path's contents atomically with data, owner-only (0600): written
 * to a temp file in the same directory, flushed, then renamed over path, so
 * a crash or full disk leaves the old file intact. A symlinked path is
 * followed. Returns false with errno set on failure. */
bool platform_write_file_private(const char *path, const char *data,
                                 size_t len);

/* Read a whole file as a NUL-terminated string (an empty file gives "").
 * Larger than max_bytes fails with EFBIG. Returns NULL with errno set on
 * failure. The files hold passwords and connection strings: free the
 * result with str_secure_free(). */
char *platform_read_file(const char *path, size_t max_bytes);

/* ============================================================================
 * Terminal/Console Functions
 * ============================================================================
 */

/* ============================================================================
 * Include Platform-Specific Threading
 * ============================================================================
 */

#include "thread.h"

#endif /* LACE_PLATFORM_H */
