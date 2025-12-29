# Lace

A fast, lightweight TUI database viewer and manager for SQLite, PostgreSQL, and MySQL.

[![asciicast](https://asciinema.org/a/ZBXGyPBcaUoAYaIVRtmPheudu.svg)](https://asciinema.org/a/ZBXGyPBcaUoAYaIVRtmPheudu)

## Features

- Memory efficient, as fast as possible
- Classic & vim-like, spatial & configurable navigation with mouse support
- Multi-tabs & workspaces
- SQL editor tabs that remember content between restarts
- Multi-column data filtering and sorting, RAW SQL filters
- Inline data editing with live updates
- Linux & macOS compatible
- Works over SSH

## Installation

### Dependencies

**macOS (Homebrew):**
```bash
brew install ncurses sqlite libpq mariadb cjson
```

**Linux (Debian/Ubuntu):**
```bash
apt install libncurses-dev libsqlite3-dev libpq-dev libmariadb-dev libcjson-dev
```

**Linux (Arch):**
```bash
pacman -S ncurses sqlite libpq mariadb-libs cjson
```

**Linux (Fedora/RHEL):**
```bash
dnf install ncurses-devel sqlite-devel libpq-devel mariadb-devel cjson-devel
```

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

# Or start without arguments for connection dialog or to restore previous session (optionally)
./build/lace
```

## TODO

- [x] Configuration management
- [x] Connection management (saved connections)
- [x] Session persistence (save/restore tabs on restart)
- [ ] Data adding (insert rows)
- [ ] Export operations
- [ ] Batch operations (bulk delete, partial export)
- [ ] Database management
- [ ] Tables management
- [ ] Schema management
- [ ] Foreign keys management
- [ ] Database users management

## License

MIT
