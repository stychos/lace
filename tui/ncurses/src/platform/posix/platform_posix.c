/*
 * Lace
 * Platform Implementation - POSIX (macOS, Linux, BSD)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "../platform.h"

#ifdef LACE_OS_POSIX

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef LACE_OS_MACOS
#include <sys/syslimits.h>
#endif

#ifdef LACE_OS_LINUX
#include <linux/limits.h>
#endif

/* Static buffers for path functions */
static char s_home_dir[PATH_MAX];
static char s_config_dir[PATH_MAX];
static char s_data_dir[PATH_MAX];

const char *platform_get_home_dir(void) {
  if (s_home_dir[0] != '\0') {
    return s_home_dir;
  }

  /* Try HOME environment variable first */
  const char *home = getenv("HOME");
  if (home && home[0] != '\0') {
    snprintf(s_home_dir, sizeof(s_home_dir), "%s", home);
    return s_home_dir;
  }

  /* Fall back to passwd entry */
  struct passwd *pw = getpwuid(getuid());
  if (pw && pw->pw_dir) {
    snprintf(s_home_dir, sizeof(s_home_dir), "%s", pw->pw_dir);
    return s_home_dir;
  }

  return NULL;
}

const char *platform_get_config_dir(void) {
  if (s_config_dir[0] != '\0') {
    return s_config_dir;
  }

  const char *home = platform_get_home_dir();
  if (!home) {
    return NULL;
  }

#ifdef LACE_OS_MACOS
  /* macOS: ~/Library/Application Support/lace */
  snprintf(s_config_dir, sizeof(s_config_dir),
           "%s/Library/Application Support/lace", home);
#else
  /* Linux/BSD: ~/.config/lace (XDG spec) */
  const char *xdg_config = getenv("XDG_CONFIG_HOME");
  if (xdg_config && xdg_config[0] != '\0') {
    snprintf(s_config_dir, sizeof(s_config_dir), "%s/lace", xdg_config);
  } else {
    snprintf(s_config_dir, sizeof(s_config_dir), "%s/.config/lace", home);
  }
#endif

  /* Create directory if it doesn't exist */
  platform_mkdir(s_config_dir);

  return s_config_dir;
}

const char *platform_get_data_dir(void) {
  if (s_data_dir[0] != '\0') {
    return s_data_dir;
  }

  const char *home = platform_get_home_dir();
  if (!home) {
    return NULL;
  }

#ifdef LACE_OS_MACOS
  /* macOS: ~/Library/Application Support/lace */
  snprintf(s_data_dir, sizeof(s_data_dir),
           "%s/Library/Application Support/lace", home);
#else
  /* Linux/BSD: ~/.local/share/lace (XDG spec) */
  const char *xdg_data = getenv("XDG_DATA_HOME");
  if (xdg_data && xdg_data[0] != '\0') {
    snprintf(s_data_dir, sizeof(s_data_dir), "%s/lace", xdg_data);
  } else {
    snprintf(s_data_dir, sizeof(s_data_dir), "%s/.local/share/lace", home);
  }
#endif

  /* Create directory if it doesn't exist */
  platform_mkdir(s_data_dir);

  return s_data_dir;
}

bool platform_file_exists(const char *path) {
  if (!path) {
    return false;
  }
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool platform_dir_exists(const char *path) {
  if (!path) {
    return false;
  }
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

char *platform_read_file(const char *path, size_t max_bytes) {
  if (!path) {
    errno = EINVAL;
    return NULL;
  }
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  struct stat st;
  if (fstat(fileno(f), &st) == 0 && st.st_size > 0 &&
      (unsigned long long)st.st_size > max_bytes) {
    fclose(f);
    errno = EFBIG;
    return NULL;
  }
  /* Read to EOF rather than trusting the size: a short read is not the end */
  size_t cap = st.st_size > 0 ? (size_t)st.st_size + 1 : 4096;
  char *buf = malloc(cap);
  size_t len = 0;
  while (buf) {
    if (len + 1 >= cap) {
      if (cap > max_bytes) {
        memset(buf, 0, cap);
        free(buf);
        buf = NULL;
        errno = EFBIG;
        break;
      }
      /* Copy and wipe rather than realloc: no stray copy of a password */
      char *grown = malloc(cap * 2);
      if (grown)
        memcpy(grown, buf, len);
      memset(buf, 0, cap);
      free(buf);
      buf = grown;
      cap *= 2;
      if (!buf)
        break;
    }
    size_t n = fread(buf + len, 1, cap - len - 1, f);
    len += n;
    if (n == 0) {
      if (ferror(f)) {
        int saved = errno ? errno : EIO;
        memset(buf, 0, cap);
        free(buf);
        buf = NULL;
        errno = saved;
      }
      break;
    }
  }
  fclose(f);
  if (buf)
    buf[len] = '\0';
  return buf;
}

bool platform_write_file_private(const char *path, const char *data,
                                 size_t len) {
  if (!path || (!data && len > 0)) {
    errno = EINVAL;
    return false;
  }
  /* Write next to the real file, so rename stays on one filesystem and a
   * symlinked config keeps its link */
  char *real = realpath(path, NULL);
  const char *target = real ? real : path;
  size_t tlen = strlen(target) + 16;
  char *tmp = malloc(tlen);
  if (!tmp) {
    free(real);
    errno = ENOMEM;
    return false;
  }
  snprintf(tmp, tlen, "%s.tmpXXXXXX", target);
  int fd = mkstemp(tmp); /* Created 0600 */
  if (fd < 0) {
    int e = errno;
    free(tmp);
    free(real);
    errno = e;
    return false;
  }
  fchmod(fd, 0600);
  bool ok = true;
  for (size_t off = 0; ok && off < len;) {
    ssize_t n = write(fd, data + off, len - off);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      ok = false;
    else
      off += (size_t)n;
  }
  if (ok && fsync(fd) != 0)
    ok = false;
  int e = errno;
  if (close(fd) != 0 && ok) {
    ok = false;
    e = errno;
  }
  if (ok && rename(tmp, target) != 0) {
    ok = false;
    e = errno;
  }
  if (!ok)
    unlink(tmp);
  free(tmp);
  free(real);
  errno = ok ? 0 : e;
  return ok;
}

bool platform_mkdir(const char *path) {
  if (!path || path[0] == '\0') {
    return false;
  }

  /* Check if already exists */
  if (platform_dir_exists(path)) {
    return true;
  }

  /* Create parent directories */
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);

  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (!platform_dir_exists(tmp)) {
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
          return false;
        }
      }
      *p = '/';
    }
  }

  /* Create final directory */
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
    return false;
  }

  return true;
}

#endif /* LACE_OS_POSIX */
