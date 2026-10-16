#!/usr/bin/env python3
"""Run the TUI tests: the real lace binary in a real terminal (tmux).

    run.py [filter...]   run tests whose "module.name" contains a filter

Each test gets a private HOME and a private tmux server; on failure the
screen is printed and the HOME (with the daemon log) is kept.
Needs tmux; without it the tests are skipped.
"""

import importlib.util
import os
import shutil
import sys
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import harness  # noqa: E402


def collect(test_dir, filters):
    if test_dir not in sys.path:
        sys.path.insert(0, test_dir)
    tests = []
    for fname in sorted(os.listdir(test_dir)):
        if not (fname.startswith("test_") and fname.endswith(".py")):
            continue
        # By path: another suite's directory may hold a module of the same name
        spec = importlib.util.spec_from_file_location(
            f"{os.path.basename(test_dir)}_{fname[:-3]}", os.path.join(test_dir, fname))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        for name in sorted(dir(mod)):
            fn = getattr(mod, name)
            if not (name.startswith("test_") and callable(fn)):
                continue
            if getattr(fn, "each_db", False):
                # Once per configured database: fn(home, db)
                for kind, url in harness.configured_dbs():
                    if kind not in fn.each_db:
                        continue
                    label = f"{fname[5:-3]}.{name[5:]}[{kind}]"
                    run = (lambda f, k, u: lambda home: f(home, harness.Db(home, k, u)))(fn, kind, url)
                    if not filters or any(f in label for f in filters):
                        tests.append((label, run))
            else:
                label = f"{fname[5:-3]}.{name[5:]}"
                if not filters or any(f in label for f in filters):
                    tests.append((label, fn))
    return tests


def main(test_dir, filters):
    if not shutil.which("tmux"):
        print("tui: tmux not found, TUI tests skipped")
        return 0
    binary = harness.LACE_NC if test_dir != HERE else harness.LACE
    for path in (binary, os.path.join(harness.LACED_DIR, "laced")):
        if not os.path.exists(path):
            print(f"tui: {path} is missing: run `make` first")
            return 1
    tests = collect(test_dir, filters)
    failed = 0
    for label, fn in tests:
        home = harness.Home()
        t0 = time.time()
        try:
            fn(home)
            ok = True
        except Exception as e:  # noqa: BLE001
            ok = False
            print(f"  FAIL {label}: {e}")
            if not isinstance(e, harness.TuiError):
                traceback.print_exc()
        ms = int((time.time() - t0) * 1000)
        if ok:
            print(f"  ok   {label} ({ms} ms)")
            home.cleanup()
        else:
            failed += 1
            print(f"       kept {home.path}")
        sys.stdout.flush()
    print(f"tui: {len(tests)} tests, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(HERE, sys.argv[1:]))
