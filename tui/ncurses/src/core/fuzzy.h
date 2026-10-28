/*
 * Lace
 * The command palettes' fuzzy match, shared by both TUIs
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FUZZY_H
#define LACE_FUZZY_H

/* -1 no match; higher is better: prefix, word start, substring, subsequence
 * per space-separated word, less a little for a long text. Case-insensitive. */
int fuzzy_score(const char *needle, const char *hay);

#endif /* LACE_FUZZY_H */
