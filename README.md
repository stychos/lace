# Lace

A fast, lightweight TUI database viewer and manager for SQLite, PostgreSQL, and MySQL.

[![asciicast](https://asciinema.org/a/thzoRMidllfDoAp85xFRdE1pu.svg)](https://asciinema.org/a/thzoRMidllfDoAp85xFRdE1pu)

## Features

- Memory efficient, browse millions of rows
- Classic & vim-like navigation
- Inline editing with live updates
- Multi-tabs
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

| Key | Action |
|-----|--------|
| `h/j/k/l` or arrows | Navigate |
| `Enter` | Edit cell inline |
| `e` | Open modal editor |
| `t` | Toggle tables list |
| `q` | Quit |

## License

MIT
