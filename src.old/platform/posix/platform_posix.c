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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#ifdef LACE_OS_MACOS
#include <mach-o/dyld.h>
#include <sys/syslimits.h>
#endif

#ifdef LACE_OS_LINUX
#include <linux/limits.h>
#endif

#ifdef LACE_OS_FREEBSD
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

/* Static buffers for path functions */
static char s_home_dir[PATH_MAX];
static char s_config_dir[PATH_MAX];
static char s_data_dir[PATH_MAX];
static char s_temp_dir[PATH_MAX];
static char s_cwd[PATH_MAX];
static char s_exe_dir[PATH_MAX];

/* Original terminal settings for raw mode restoration */
static struct termios s_orig_termios;
static bool s_raw_mode_enabled = false;

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

const char *platform_get_temp_dir(void) {
  if (s_temp_dir[0] != '\0') {
    return s_temp_dir;
  }

  const char *tmp = getenv("TMPDIR");
  if (!tmp) {
    tmp = getenv("TMP");
  }
  if (!tmp) {
    tmp = getenv("TEMP");
  }
  if (!tmp) {
    tmp = "/tmp";
  }

  snprintf(s_temp_dir, sizeof(s_temp_dir), "%s", tmp);
  return s_temp_dir;
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

const char *platform_getenv(const char *name) { return getenv(name); }

bool platform_setenv(const char *name, const char *value) {
  if (!name) {
    return false;
  }
  if (value) {
    return setenv(name, value, 1) == 0;
  } else {
    return unsetenv(name) == 0;
  }
}

const char *platform_getcwd(void) {
  if (getcwd(s_cwd, sizeof(s_cwd)) != NULL) {
    return s_cwd;
  }
  return NULL;
}

const char *platform_get_exe_dir(void) {
  if (s_exe_dir[0] != '\0') {
    return s_exe_dir;
  }

  char path[PATH_MAX];

#ifdef LACE_OS_MACOS
  uint32_t size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) == 0) {
    char *resolved = realpath(path, NULL);
    if (resolved) {
      /* Get directory part */
      char *last_sep = strrchr(resolved, '/');
      if (last_sep) {
        size_t len = (size_t)(last_sep - resolved);
        if (len < sizeof(s_exe_dir)) {
          memcpy(s_exe_dir, resolved, len);
          s_exe_dir[len] = '\0';
        }
      }
      free(resolved);
    }
  }
#elif defined(LACE_OS_LINUX)
  ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (len > 0) {
    path[len] = '\0';
    char *last_sep = strrchr(path, '/');
    if (last_sep) {
      size_t dir_len = (size_t)(last_sep - path);
      if (dir_len < sizeof(s_exe_dir)) {
        memcpy(s_exe_dir, path, dir_len);
        s_exe_dir[dir_len] = '\0';
      }
    }
  }
#elif defined(LACE_OS_FREEBSD)
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
  size_t size = sizeof(path);
  if (sysctl(mib, 4, path, &size, NULL, 0) == 0) {
    char *last_sep = strrchr(path, '/');
    if (last_sep) {
      size_t len = (size_t)(last_sep - path);
      if (len < sizeof(s_exe_dir)) {
        memcpy(s_exe_dir, path, len);
        s_exe_dir[len] = '\0';
      }
    }
  }
#endif

  return s_exe_dir[0] ? s_exe_dir : NULL;
}

bool platform_is_tty(void) { return isatty(STDOUT_FILENO) != 0; }

bool platform_get_terminal_size(int *width, int *height) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    if (width) {
      *width = ws.ws_col;
    }
    if (height) {
      *height = ws.ws_row;
    }
    return true;
  }
  return false;
}

bool platform_set_raw_mode(bool enable) {
  if (enable && !s_raw_mode_enabled) {
    /* Save original settings */
    if (tcgetattr(STDIN_FILENO, &s_orig_termios) != 0) {
      return false;
    }

    struct termios raw = s_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= CS8;
    raw.c_oflag &= ~OPOST;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
      return false;
    }

    s_raw_mode_enabled = true;
    return true;
  } else if (!enable && s_raw_mode_enabled) {
    /* Restore original settings */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &s_orig_termios);
    s_raw_mode_enabled = false;
    return true;
  }

  return true;
}

#endif /* LACE_OS_POSIX */
