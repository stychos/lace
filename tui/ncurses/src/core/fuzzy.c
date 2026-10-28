/*
 * Lace
 * The command palettes' fuzzy match (both TUIs): see fuzzy.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "fuzzy.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

/* Length counts for a little, but never enough to cross a class (310/210/...) */
#define LEN_PENALTY_MAX 9

static bool word_start(const char *hay, size_t i) {
  if (i == 0) return true;
  char c = hay[i - 1];
  return c == ' ' || c == '_' || c == '-' || c == '.';
}

static bool match_at(const char *hay, size_t i, const char *w, size_t wn) {
  for (size_t k = 0; k < wn; k++)
    if (!hay[i + k] || tolower((unsigned char)hay[i + k]) != tolower((unsigned char)w[k])) return false;
  return true;
}

/* Best class of one word in hay, or -1 */
static int word_score(const char *hay, size_t hn, const char *w, size_t wn) {
  int best = -1;
  for (size_t i = 0; i + wn <= hn; i++) {
    if (!match_at(hay, i, w, wn)) continue;
    int s = i == 0 ? 310 : word_start(hay, i) ? 210 : 110;
    if (s > best) best = s;
    if (best == 310) return best;
  }
  if (best >= 0) return best;
  size_t k = 0;
  for (size_t i = 0; i < hn && k < wn; i++)
    if (tolower((unsigned char)hay[i]) == tolower((unsigned char)w[k])) k++;
  return k == wn ? 60 : -1;
}

int fuzzy_score(const char *needle, const char *hay) {
  size_t hn = strlen(hay);
  int sum = 0;
  const char *p = needle;
  for (;;) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char *e = p;
    while (*e && *e != ' ') e++;
    int s = word_score(hay, hn, p, (size_t)(e - p));
    if (s < 0) return -1;
    sum += s;
    p = e;
  }
  size_t pen = hn / 8;
  sum -= (int)(pen > LEN_PENALTY_MAX ? LEN_PENALTY_MAX : pen);
  return sum < 0 ? 0 : sum;
}

