from nc import Nc
from test_table import make_db


def test_sidebar_toggle_search_open(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("TABLES")
        t.keys("t")                            # hide
        t.wait(lambda s: "TABLES" not in s)
        t.keys("t"); t.wait("TABLES")
        t.wait(lambda s: s.split("\n")[2].startswith(" TABLES"))   # no search field until it is called
        t.wait("/ search")                     # the focused sidebar's hints
        t.keys("/")
        t.wait(lambda s: s.split("\n")[2].startswith(" ") and not s.split("\n")[2].startswith(" TABLES"))
        t.type("bi")                           # the search field has the focus
        t.wait(lambda s: s.split("\n")[2].startswith(" bi") and "TABLES" in s.split("\n")[3])
        t.wait(lambda s: "small" not in s.split("\n")[4])          # the list is filtered
        t.keys("Enter")
        t.wait("1 of 5")
        t.wait(lambda s: s.split("\n")[2].startswith(" bi"))      # the field stays while it has text
        t.keys("Tab")                          # focus back to the sidebar
        t.keys("Escape")                       # clears the search and hides the field
        t.wait(lambda s: s.split("\n")[2].startswith(" TABLES"))
        t.keys("e")                            # a table key does nothing from the sidebar (no editor opens)
        t.keys("Escape")                       # leaves the sidebar
        t.wait("f filter")
        t.keys("C-p"); t.type("filter tables"); t.keys("Enter")   # the palette calls the field too
        t.wait("/ search")
        t.wait(lambda s: not s.split("\n")[2].startswith(" TABLES"))
        t.keys("Escape", "Escape")             # hides it, leaves the sidebar
        t.wait("f filter")
        t.keys("q"); assert t.quit() == 0


def test_view_switcher_and_close(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("Enter"); t.wait("1 of 5")
        t.keys("p"); t.wait("New query")
        t.keys("C-e")
        t.wait("Views in")
        t.wait("big")
        t.keys("Up", "Enter")                  # back to the table view
        t.wait("1 of 5")
        t.keys("C-w")                          # closes the table view, the query remains
        t.wait("New query")
        t.keys("q"); assert t.quit() == 0


def test_log_and_history(home):
    db = make_db(home, rows=5)
    with Nc(home, ["-s", "sqlite://" + db]) as t:
        t.wait("big"); t.keys("Enter"); t.wait("1 of 5")
        t.keys("F8")
        t.wait("LIMIT 1000")                   # the page SQL in the log
        t.keys("Escape")
        t.keys("p"); t.type("SELECT 42"); t.keys("C-r"); t.wait("1 of 1")
        t.keys("Escape")                       # leave the editor for the results
        t.keys("i")
        t.wait("History")
        t.wait("SELECT 42")                    # history of the connection
        t.keys("Escape")
        t.keys("q"); assert t.quit() == 0


def test_prefs_dialog_saves_to_config(home):
    with Nc(home, ["-s"]) as t:
        t.wait("palette")                      # input before the first draw is discarded
        t.keys(",")
        t.wait("Preferences")
        t.wait("Confirm deletes")
        t.keys("Space")                        # the first item, Confirm deletes, on by default: now off
        t.wait("Confirm deletes          off")
        t.keys("Escape")
        t.keys("q"); assert t.quit() == 0
    cfg = home.read("config.json")
    assert cfg["general"]["delete_confirmation"] is False
    assert "notcurses" in cfg
