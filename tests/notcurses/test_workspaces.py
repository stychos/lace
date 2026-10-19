"""The workspaces manager (w / F2)"""
from nc import Nc
from test_misc import save_connection
from test_table import make_db


def first_line(s):
    return s.split("\n")[0]


def test_manager_lists_and_switches(home):
    with Nc(home, ["-s"]) as t:
        t.wait("URI")
        t.keys("C-p"); t.type("new workspace"); t.keys("Enter")
        t.wait(lambda s: first_line(s).startswith(" Workspace 2 ▸"))
        t.keys("F2")
        t.wait("Workspaces")
        t.wait("OPEN")
        t.wait("1  Workspace")
        t.wait("2  Workspace 2")
        t.keys("Up", "Enter")
        t.wait(lambda s: first_line(s).startswith(" Workspace ▸"))
        t.keys("q")
        assert t.quit() == 0


def test_manager_rename_save_close(home):
    db = make_db(home, rows=3)
    save_connection(home, db)
    with Nc(home, ["-s"]) as t:
        t.wait("testdb"); t.keys("Down", "Enter"); t.wait("big")
        t.keys("F2"); t.wait("OPEN")
        t.keys("r"); t.wait("Rename workspace"); t.keys("C-u"); t.type("main"); t.keys("Enter")
        t.wait("1  main")
        t.keys("s"); t.wait("Save workspace"); t.keys("Enter")
        t.wait("SAVED")
        assert [w["name"] for w in home.read("workspaces.json")["workspaces"]] == ["main"]
        t.keys("Down")                 # the saved one
        t.keys("r"); t.wait("Rename saved workspace"); t.keys("C-u"); t.type("other"); t.keys("Enter")
        t.wait("other")
        assert [w["name"] for w in home.read("workspaces.json")["workspaces"]] == ["other"]
        t.keys("x"); t.wait("Delete the saved workspace"); t.keys("y")
        t.wait(lambda s: "SAVED" not in s)
        assert home.read("workspaces.json")["workspaces"] == []
        t.keys("Up", "x")              # the only open workspace: closing it quits
        assert t.quit() == 0


def test_manager_new_workspace(home):
    with Nc(home, ["-s"]) as t:
        t.wait("URI")
        t.keys("F2"); t.wait("OPEN")
        t.keys("n")
        t.wait(lambda s: first_line(s).startswith(" Workspace 2 ▸ New connection"))
        t.keys("q")
        assert t.quit() == 0
