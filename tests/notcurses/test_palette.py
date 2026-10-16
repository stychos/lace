from nc import Nc


def test_palette_lists_actions_and_runs_one(home):
    with Nc(home, ["-s"]) as t:
        t.wait("palette")
        t.keys("C-p")
        t.wait("Quit")            # an action row with its key
        t.wait("Ctrl+X")
        t.type("qui")
        t.wait("Quit")
        t.keys("Enter")           # runs Quit
        assert t.quit() == 0


def test_help_lists_bindings(home):
    with Nc(home, ["-s"]) as t:
        t.wait("palette")         # input before the first draw is discarded
        t.keys("?")
        t.wait("Command palette")
        t.wait("Ctrl+P")
        t.keys("Escape")
        t.wait("palette")         # the status line again, overlay gone
        t.keys("q")
        assert t.quit() == 0


def test_palette_toggles_a_preference(home):
    with Nc(home, ["-s"]) as t:
        t.wait("palette")
        t.keys("C-p")
        t.wait("PREFERENCES")                 # the header of the toggle rows
        t.type("confirm deletes")
        t.wait("Confirm deletes: on")
        t.keys("Enter")                       # toggles and saves, as the dialog does
        t.wait_status("Confirm deletes: off")
        t.keys("C-p")
        t.type("confirm deletes")
        t.wait("Confirm deletes: off")        # the row follows the setting
        t.keys("Escape")
        t.keys("q"); assert t.quit() == 0
    assert home.read("config.json")["general"]["delete_confirmation"] is False


def test_palette_lists_only_runnable_actions(home):
    with Nc(home, ["-s"]) as t:
        t.wait("palette")
        t.keys("C-p")
        t.wait("Quit")
        t.type("cut line")                    # an editor key: not a palette row
        t.wait("> cut line")
        assert "Cut line" not in t.text()
        t.keys("Escape")
        t.keys("q"); assert t.quit() == 0
