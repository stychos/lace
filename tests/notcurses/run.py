#!/usr/bin/env python3
"""Run the notcurses TUI tests (tests/notcurses/test_*.py) with the tmux
harness of tests/tui: run.py [filter...]"""
import importlib.util, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
TUI = os.path.join(os.path.dirname(HERE), "tui")
sys.path.insert(0, TUI)
sys.path.insert(0, HERE)
# This file is also called run.py, so load the TUI runner by path
_spec = importlib.util.spec_from_file_location("tui_run", os.path.join(TUI, "run.py"))
tui_run = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tui_run)
if __name__ == "__main__":
    sys.exit(tui_run.main(HERE, sys.argv[1:]))
