/*
 * Lace
 * Declaration wrappers for the public headers: everything declared between
 * LACE_BEGIN_DECLS and LACE_END_DECLS is exported from the shared library
 * (built with -fvisibility=hidden, so nothing else is) and has C linkage
 * in C++.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_EXPORT_H
#define LACE_EXPORT_H

#ifdef __cplusplus
#define LACE_BEGIN_DECLS                                                       \
  _Pragma("GCC visibility push(default)") extern "C" {
#define LACE_END_DECLS                                                         \
  }                                                                            \
  _Pragma("GCC visibility pop")
#else
#define LACE_BEGIN_DECLS _Pragma("GCC visibility push(default)")
#define LACE_END_DECLS _Pragma("GCC visibility pop")
#endif

#endif /* LACE_EXPORT_H */
