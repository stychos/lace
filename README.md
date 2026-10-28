# Lace

A fast, lightweight database viewer and manager for SQLite, PostgreSQL, MySQL and MariaDB, with terminal and desktop frontends.

[![asciicast](https://asciinema.org/a/ZBXGyPBcaUoAYaIVRtmPheudu.svg)](https://asciinema.org/a/ZBXGyPBcaUoAYaIVRtmPheudu)

## Features

- Memory-efficient and fast: tables and results are read a page at a time, also with millions of rows
- Classic and Vim-style navigation, configurable hotkeys, mouse support
- Workspaces of connection tabs, each with its own table and SQL editor tabs
- SQL editor tabs whose text survives restarts; run a statement, a script, or a script in one transaction
- Multi-column filters and sorts, raw SQL filters included
- Inline editing, checked against the database before every write; row insertion and bulk deletion
- Undo of edits and deletes (terminal frontends)
- Table sub-views: data, structure, indexes, foreign keys and table information
- Views, functions and triggers listed beside the tables, with their definitions
- Structure editing: columns, primary keys, indexes, check and foreign key constraints; every statement is shown before it runs
- Follow a foreign key to the row it references, or list the rows that reference it
- Open transactions marked on their tabs, and asked about before they would be rolled back
- Saved connections in folders, recent connections, query history
- SSH tunnels to database servers; on macOS, passwords can stay in the Keychain
- System clipboard integration with an internal clipboard as fallback
- Runs on Linux and macOS, also over SSH

## Frontends

- **lace**: the ncurses terminal frontend (`tui/ncurses/`)
- **lace-nc**: the notcurses terminal frontend (`tui/notcurses/`); both terminal frontends look and work alike, with a command palette (Ctrl+P or `:`)
- **lace-gtk**: a GTK4/libadwaita desktop frontend (`gui/gtk/`)
- **Lace.app**: a native macOS frontend (`gui/macos/`)

The frontends never connect to a database themselves. **laced**, a small daemon started on demand, owns the database drivers and serves every frontend. They all share saved connections, settings, sessions and query history.

## Installation

### Dependencies

**macOS (Homebrew):**
```bash
brew install ncurses sqlite libpq mariadb cjson
brew install notcurses                       # lace-nc
brew install gtk4 libadwaita gtksourceview5  # lace-gtk
xcode-select --install                       # Lace.app: the Command Line Tools (Swift); Xcode is not needed
```

**Linux (Debian/Ubuntu):**
```bash
apt install libncurses-dev libsqlite3-dev libpq-dev libmariadb-dev libcjson-dev
apt install libnotcurses-dev                                         # lace-nc
apt install libgtk-4-dev libadwaita-1-dev libgtksourceview-5-dev     # lace-gtk
```

**Linux (Arch):**
```bash
pacman -S ncurses sqlite libpq mariadb-libs cjson
pacman -S notcurses                       # lace-nc
pacman -S gtk4 libadwaita gtksourceview5  # lace-gtk
```

**Linux (Fedora/RHEL):**
```bash
dnf install ncurses-devel sqlite-devel libpq-devel mariadb-devel cjson-devel
dnf install notcurses-devel                               # lace-nc
dnf install gtk4-devel libadwaita-devel gtksourceview5-devel  # lace-gtk
```

### Build

```bash
make            # everything whose dependencies are installed; a missing toolkit only skips that frontend
make install    # laced, lace and lace-nc into PREFIX (default /usr/local); make install-gtk adds lace-gtk
```

The programs are left at:

| Program | Path |
|---|---|
| lace | `tui/ncurses/build/lace` |
| lace-nc | `tui/notcurses/build/lace-nc` |
| lace-gtk | `gui/gtk/build/lace-gtk` |
| Lace.app | `gui/macos/build/Lace.app` |
| laced | `laced/build/laced` |

## Usage

```bash
# SQLite: a connection string or just the file
./tui/ncurses/build/lace sqlite:///path/to/database.db
./tui/ncurses/build/lace ./database.db

# PostgreSQL
./tui/ncurses/build/lace postgres://user:pass@host/database

# MySQL/MariaDB
./tui/ncurses/build/lace mysql://user:pass@host/database

# Through an SSH tunnel
./tui/ncurses/build/lace postgres+ssh://sshuser@bastion:22/user:pass@dbhost:5432/database

# Run one query, print the result as a table and exit (lace-nc -q too)
./tui/ncurses/build/lace -q 'SELECT * FROM users' sqlite:///path/to/database.db

# Other output formats: vertical (like MySQL's \G; also a query ending in \G), csv, json, tsv
./tui/ncurses/build/lace -q 'SELECT * FROM users' -f json sqlite:///path/to/database.db
./tui/ncurses/build/lace -q 'SELECT * FROM users\G' sqlite:///path/to/database.db
```

Without arguments, lace restores the previous session, or opens a connection tab. There you pick a saved connection or type a URL. `-s` (`--no-session`) skips the session. `lace-nc` and `lace-gtk` take URLs, files and `-s` too. Press `?` in the terminal frontends for the keys.

## TODO

- [x] Configuration management
- [x] Connection management (saved connections)
- [x] Session persistence (tabs, filters and sorts restored on restart)
- [x] Bulk row deletion
- [x] Row insertion
- [x] Table management (create, rename, truncate, drop)
- [x] Schema management (columns, primary keys, indexes, check constraints)
- [x] Foreign key management
- [ ] Data export (including partial exports for selected rows)
- [ ] Database management
- [ ] User management

## License

MIT
