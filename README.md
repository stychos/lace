# Lace

A fast, lightweight TUI database viewer and manager for SQLite, PostgreSQL, MySQL, and MariaDB.

[![asciicast](https://asciinema.org/a/ZBXGyPBcaUoAYaIVRtmPheudu.svg)](https://asciinema.org/a/ZBXGyPBcaUoAYaIVRtmPheudu)

## Features

- Memory-efficient and optimized for speed
- Classic and Vim-style navigation with configurable hotkeys and mouse support
- Multiple tabs and workspaces
- SQL editor tabs with persistent content across restarts
- Multi-column filtering and sorting with raw SQL filter support
- Inline data editing with live updates
- Quick row insertion
- Query history tracking
- System clipboard integration with internal clipboard fallback
- Compatible with Linux and macOS
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

# Or start without arguments to open the connection dialog or restore a previous session
./build/lace
```

## TODO

- [x] Configuration management
- [x] Connection management (saved connections)
- [x] Session persistence (save/restore tabs on restart)
- [x] Bulk row deletion
- [x] Row insertion
- [ ] Data export (including partial exports for selected rows)
- [ ] Database management
- [ ] Table management
- [ ] Schema management
- [ ] Foreign key management
- [ ] User management

## License

MIT
