#!/usr/bin/env python3
"""Run the lace integration tests against real database servers.

The servers come from compose.yaml through the `docker` command line only
(`docker compose up/port/down`): nothing here knows where or how the engine
runs, so it works wherever the CLI does (Docker Desktop, colima, a Linux
daemon, a remote context with published ports reachable on 127.0.0.1).
LACE_TEST_DOCKER names another compatible CLI (e.g. podman).

The test binary never talks to the engine: it gets one connection URL per
database in LACE_IT_* environment variables and skips the databases that
have none. So the same binary runs against servers started any other way:

    LACE_IT_POSTGRES=postgres://u:p@host/db run.py --no-docker

Each run goes through both transports the client has: a private socket
daemon (started here, in a temporary directory) and a private stdio
daemon per database. --transport picks one.

Usage:
    run.py [--no-docker] [--keep] [--only postgres,mysql,...] [--slow]
           [--transport both|socket|stdio] [-- <test binary arguments>]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BINARY = os.path.join(ROOT, "tui", "ncurses", "build", "test_integration")
LACED = os.path.join(ROOT, "laced", "build", "laced")
COMPOSE = os.path.join(HERE, "compose.yaml")
TUI_RUNNER = os.path.join(ROOT, "tests", "tui", "run.py")

# service -> (container port, URL template; {host}/{port} filled in)
SERVICES = {
    "postgres": (5432, "postgres://lt:ltpw@{host}:{port}/lt"),
    "mariadb": (3306, "mariadb://lt:ltpw@{host}:{port}/lt"),
    "mysql": (3306, "mysql://lt:ltpw@{host}:{port}/lt"),
    "sshd": (22, None),
}


def log(msg):
    print(f"[run.py] {msg}", flush=True)


def docker_cmd():
    return os.environ.get("LACE_TEST_DOCKER", "docker")


def compose(project, *args, capture=False):
    cmd = [docker_cmd(), "compose", "-p", project, "-f", COMPOSE, *args]
    if capture:
        return subprocess.run(cmd, check=True, text=True,
                              stdout=subprocess.PIPE).stdout
    subprocess.run(cmd, check=True)
    return None


def engine_available():
    if not shutil.which(docker_cmd()):
        return False
    try:
        subprocess.run([docker_cmd(), "info"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=30)
        return True
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return False


def published(project, service, port):
    """host, port where the engine published service's port"""
    out = compose(project, "port", service, str(port), capture=True).strip()
    host, _, hport = out.splitlines()[0].rpartition(":")
    host = host.strip("[]")
    if host in ("0.0.0.0", "::", ""):
        host = "127.0.0.1"
    return os.environ.get("LACE_TEST_DB_HOST", host), int(hport)


def start_socket_daemon(tmp, env):
    """A private socket daemon (not the user's): returns (process, path)"""
    path = os.path.join(tmp, "laced.sock")
    proc = subprocess.Popen([LACED, "--unix", path, "--quiet"], env=env)
    for _ in range(100):
        if os.path.exists(path):
            return proc, path
        if proc.poll() is not None:
            sys.exit(f"laced exited ({proc.returncode}); see {tmp}/laced.log")
        time.sleep(0.05)
    proc.kill()
    sys.exit("laced did not create its socket")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-docker", action="store_true",
                    help="use the LACE_IT_* URLs already in the environment")
    ap.add_argument("--keep", action="store_true",
                    help="leave the containers running afterwards")
    ap.add_argument("--only", default="",
                    help="comma-separated services to start (default: all)")
    ap.add_argument("--slow", action="store_true",
                    help="also run slow tests (LACE_IT_SLOW=1)")
    ap.add_argument("--suite", default="all", choices=["all", "c", "tui"],
                    help="the C integration tests, the TUI tests (tests/tui, "
                         "needs tmux) against the same databases, or both")
    ap.add_argument("--transport", default="both",
                    choices=["both", "socket", "stdio"],
                    help="which daemon transports to test (default: both)")
    ap.add_argument("rest", nargs="*", help="arguments for the test binary")
    args = ap.parse_args()

    for path in ([LACED] if args.suite == "tui" else [BINARY, LACED]):
        if not os.path.exists(path):
            sys.exit(f"{path} is missing: run `make test-integration` "
                     "(or `make` and `make -C tui/ncurses integration`)")

    env = dict(os.environ)
    env["LACE_IT_LACED"] = LACED
    if args.slow:
        env["LACE_IT_SLOW"] = "1"
    # Short: a Unix socket path must fit in sun_path (~104 bytes)
    base = tempfile.gettempdir()
    if len(base) > 60 and os.path.isdir("/tmp"):
        base = "/tmp"
    tmp = tempfile.mkdtemp(prefix="lace-it-", dir=base)
    env["LACE_IT_SQLITE"] = "sqlite://" + os.path.join(tmp, "it.sqlite") + "?create=1"

    # ssh for tunnels: a private config, so host keys of throwaway
    # containers never reach the user's known_hosts
    ssh_config = os.path.join(tmp, "ssh_config")
    with open(ssh_config, "w") as f:
        f.write(f"Host *\n"
                f"  UserKnownHostsFile {os.path.join(tmp, 'known_hosts')}\n"
                f"  GlobalKnownHostsFile /dev/null\n"
                f"  StrictHostKeyChecking accept-new\n"
                f"  LogLevel ERROR\n")
    env["LACED_SSH_CONFIG"] = ssh_config
    # Every daemon logs here (stdio ones too): kept when a test fails
    env["LACED_LOG"] = os.path.join(tmp, "laced.log")
    # A private HOME for the tests and their daemons: tunnel directories,
    # token files and default sockets land there, never in the user's cache
    # directory (docker itself keeps the real HOME: compose() uses os.environ)
    # Short, because socket paths under it must fit sun_path (104 bytes on
    # macOS, where the temporary directory alone takes ~50)
    short_root = "/tmp" if os.path.isdir("/tmp") else None
    home = tempfile.mkdtemp(prefix="lit-", dir=short_root)
    env["HOME"] = home
    env["XDG_RUNTIME_DIR"] = ""

    project = f"lace-it-{os.getpid()}"
    started = False
    try:
        if not args.no_docker:
            if not engine_available():
                log(f"`{docker_cmd()}` is not available: SQLite only")
            else:
                wanted = [s for s in args.only.split(",") if s] or list(SERVICES)
                unknown = set(wanted) - set(SERVICES)
                if unknown:
                    sys.exit(f"unknown services: {', '.join(sorted(unknown))}")
                log(f"starting {', '.join(wanted)} (project {project})")
                started = True
                compose(project, "up", "-d", "--build", "--wait",
                        "--wait-timeout", "300", *wanted)
                for svc in wanted:
                    port, url = SERVICES[svc]
                    host, hport = published(project, svc, port)
                    if url:
                        env[f"LACE_IT_{svc.upper()}"] = url.format(host=host, port=hport)
                    log(f"{svc}: {host}:{hport}")
                if "sshd" in wanted:
                    host, hport = published(project, "sshd", 22)
                    jump = f"tunnel:tunnelpw@{host}:{hport}"
                    if "postgres" in wanted:
                        env["LACE_IT_SSH_POSTGRES"] = (
                            f"postgres+ssh://{jump}/lt:ltpw@postgres:5432/lt")
                    if "mariadb" in wanted:
                        env["LACE_IT_SSH_MARIADB"] = (
                            f"mariadb+ssh://{jump}/lt:ltpw@mariadb:3306/lt")
                    # Key authentication too: a throwaway key, its public
                    # half installed for the tunnel user
                    if "postgres" in wanted and shutil.which("ssh-keygen"):
                        key = os.path.join(tmp, "id_it")
                        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "",
                                        "-f", key], check=True)
                        with open(key + ".pub") as f:
                            pub = f.read().strip()
                        compose(project, "exec", "-T", "sshd", "sh", "-c",
                                "mkdir -p /home/tunnel/.ssh && "
                                f"echo '{pub}' > /home/tunnel/.ssh/authorized_keys && "
                                "chown -R tunnel /home/tunnel/.ssh && "
                                "chmod 700 /home/tunnel/.ssh && "
                                "chmod 600 /home/tunnel/.ssh/authorized_keys")
                        env["LACE_IT_SSH_KEY_POSTGRES"] = (
                            f"postgres+ssh://tunnel@{host}:{hport}/lt:ltpw@postgres:5432/lt"
                            f"?ssh_key={urllib.parse.quote(key, safe='')}")
        rc = 0
        transports = (["socket", "stdio"] if args.transport == "both"
                      else [args.transport])
        if args.suite == "tui":
            transports = []
        for transport in transports:
            log(f"transport: {transport}")
            env["LACE_IT_TRANSPORT"] = transport
            if transport == "socket":
                daemon = start_socket_daemon(tmp, env)
                env["LACE_IT_SOCKET"] = daemon[1]
                try:
                    rc |= subprocess.run([BINARY, *args.rest], env=env).returncode
                finally:
                    daemon[0].terminate()
                    try:
                        daemon[0].wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        daemon[0].kill()
            else:
                rc |= subprocess.run([BINARY, *args.rest], env=env).returncode
        if args.suite in ("all", "tui"):
            # The TUI starts its own daemon: once, not per transport
            log("TUI tests")
            tui_env = {k: v for k, v in env.items()
                       if k not in ("LACE_IT_TRANSPORT", "LACE_IT_SOCKET")}
            rc |= subprocess.run([sys.executable, TUI_RUNNER, *args.rest],
                                 env=tui_env).returncode
    finally:
        if started and not args.keep:
            log("removing containers")
            compose(project, "down", "-v", "--remove-orphans")
        elif started:
            log(f"containers kept: {docker_cmd()} compose -p {project} -f {COMPOSE} down -v")
        if rc == 0:
            shutil.rmtree(tmp, ignore_errors=True)
            shutil.rmtree(home, ignore_errors=True)
        else:
            log(f"test HOME kept: {home}")
            log(f"failed: daemon log and files kept in {tmp}")
    sys.exit(rc)


if __name__ == "__main__":
    main()
