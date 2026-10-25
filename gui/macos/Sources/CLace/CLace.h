/*
 * Lace macOS frontend
 * The liblace headers Swift imports as the CLace module (module.modulemap):
 * the client API with the SQL builders and row keys, connection strings,
 * the saved-connection and history types shared with the other frontends,
 * and the TUI's saved-connections tree and history persistence (compiled
 * into build/shared/liblaceshared.a by the Makefile)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_MACOS_CLACE_H
#define LACE_MACOS_CLACE_H

#include "../../../../liblace/include/lace.h"
#include "../../../../liblace/include/util/connstr.h"
#include "../../../../liblace/include/util/str.h"
#include "../../../../liblace/include/config/saved_connection.h"
#include "../../../../liblace/include/config/history.h"
#include "../../../../tui/ncurses/src/config/connections.h"
#include "../../../../tui/ncurses/src/config/recent.h"
#include "../../../../tui/ncurses/src/core/history.h"
#include "../../../../tui/ncurses/src/core/catalog_rows.h"
#include "CLaceShared.h"

#endif /* LACE_MACOS_CLACE_H */
