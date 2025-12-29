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

- ncurses
- OpenSSL
- SQLite3, libpq (PostgreSQL), libmariadb (MySQL)

### Build

```bash
# Initialize submodules (required for first build)
git submodule update --init --recursive

# Build
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
