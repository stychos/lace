# Integration tests

Tests that run lace against real databases: SQLite, PostgreSQL, MariaDB,
MySQL, and PostgreSQL through an SSH tunnel.

```bash
make test-integration                       # everything (needs podman or docker)
make test-integration IT_ARGS="--slow"      # also the 1M-row limit tests
make test-integration IT_ARGS="--only postgres,sshd -- tunnel"
```

`run.py` starts the servers from `compose.yaml` with `podman compose` (or
`docker compose` when podman is not installed; `LACE_TEST_DOCKER` names the
CLI to use), using only the command line: it works with whatever engine or
context the CLI is set up for. podman needs a compose provider (podman-compose
or docker-compose) and, for docker-compose, its user socket
(`systemctl --user enable --now podman.socket`). Ports are published on
127.0.0.1 with ephemeral host ports. Without a container engine only the
SQLite tests run.

The test binary itself knows nothing about containers. It takes one URL
per database from the environment and skips the others, so it also runs
against servers you already have:

```bash
make && make -C tui/ncurses integration
LACE_IT_POSTGRES=postgres://user:pass@host/db \
LACE_IT_MYSQL=mysql://user:pass@host/db \
  python3 tests/integration/run.py --no-docker
```

| Variable | Database |
|---|---|
| `LACE_IT_SQLITE` | set by run.py (a temporary file) |
| `LACE_IT_POSTGRES` | PostgreSQL |
| `LACE_IT_MARIADB` | MariaDB |
| `LACE_IT_MYSQL` | MySQL |
| `LACE_IT_SSH_POSTGRES` | PostgreSQL through `postgres+ssh://` |
| `LACE_IT_SSH_MARIADB` | MariaDB through `mariadb+ssh://` |
| `LACE_IT_SLOW=1` | also run slow tests (`--slow`) |

Each run covers both daemon transports (a private socket daemon and a
private stdio daemon); `--transport socket|stdio` picks one. When a test
fails, the daemon log and temporary files are kept and their path printed.
`--keep` leaves the containers running.

Tests live in `it_*.c`: a table of `{name, function, databases, slow}` per
file, with helpers in `it.h`. Each test makes its own `it_*` tables and drops
them.
