# Lace

A fast, lightweight TUI database viewer and manager for SQLite, PostgreSQL, and MySQL.

[![asciicast](https://asciinema.org/a/ZBXGyPBcaUoAYaIVRtmPheudu.svg)](https://asciinema.org/a/ZBXGyPBcaUoAYaIVRtmPheudu)

## Features

- Memory efficient, as fast as possible
- Classic & vim-like spatial navigation
- Full mouse support (click, double-click, scroll wheel)
- Multi-tab workspaces
- SQL query editor
- Column-based data filtering
- Inline data editing with live updates
- Linux & macOS compatible
- Works over SSH

## Installation

### Dependencies

- ncurses
- OpenSSL
- SQLite3, libpq (PostgreSQL), libmariadb (MySQL)

### Build

```bash
make
```

Binary will be at `build/lace`.

## Usage

```bash
# SQLite
./build/lace sqlite:///path/to/database.db

# PostgreSQL
./build/lace postgres://user:pass@host/database

# MySQL/MariaDB
./build/lace mysql://user:pass@host/database

# Or start without arguments for connection dialog
./build/lace
```

## Keyboard Shortcuts

### Navigation

| Key | Action |
|-----|--------|
| `h/j/k/l` or arrows | Navigate cells |
| `PgUp` / `PgDn` | Page up/down |
| `Home` / `End` | First/last column |
| `g` or `a` | Go to first row |
| `G` or `z` | Go to last row |
| `/` or `Ctrl+G` or `F5` | Go to specific row |

### Editing

| Key | Action |
|-----|--------|
| `Enter` | Edit cell inline |
| `e` or `F4` | Open modal editor |
| `n` or `Ctrl+N` | Set cell to NULL |
| `d` or `Ctrl+D` | Set cell to empty |
| `x` or `Delete` | Delete row |

### Tabs & Sidebar

| Key | Action |
|-----|--------|
| `t` or `F9` | Toggle sidebar |
| `[` or `F7` | Previous tab |
| `]` or `F6` | Next tab |
| `-` | Close current tab |
| `+` | Open table in new tab (in sidebar) |
| `f` or `/` | Filter tables (in sidebar) |

### Query Tab

| Key | Action |
|-----|--------|
| `p` | Open query tab |
| `Ctrl+R` | Execute query under cursor |
| `Ctrl+A` | Execute all queries |
| `Ctrl+T` | Execute all in transaction |
| `Ctrl+W` / `Esc` | Switch focus (editor/results) |
| Arrows / `PgUp` / `PgDn` | Navigate editor or results |

### Table Filters

| Key | Action |
|-----|--------|
| `f` or `/` | Toggle filters panel (in table view) |
| `+` / `=` | Add new filter |
| `-` / `x` / `Delete` | Remove filter |
| `c` | Clear all filters |
| `Ctrl+W` | Switch focus (filters/table) |
| `Esc` | Close filters panel |

### Other

| Key | Action |
|-----|--------|
| `r` | Refresh table data |
| `s` or `F3` | Show table schema |
| `c` or `F2` | Connection dialog |
| `m` | Toggle menu bar |
| `b` | Toggle status bar |
| `?` or `F1` | Help |
| `q` or `Ctrl+X` or `F10` | Quit |

## Mouse

| Action | Effect |
|--------|--------|
| Click on tab | Switch to tab |
| Click on table | Load in current tab |
| Double-click on table | Open in new tab |
| Click on cell | Select cell |
| Double-click on cell | Edit cell |
| Scroll wheel | Scroll rows |

## License

MIT
