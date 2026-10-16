from nc import Nc

def test_starts_and_quits(home):
    with Nc(home, ["-s"]) as t:
        t.wait("Workspace")
        t.wait("palette")
        t.keys("q")
        assert t.quit() == 0
