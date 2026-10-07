"""tools/shader_lists.py: merging recorded shader lists into port/shader-lists"""
from tools import shader_lists

VS = "vs 00000000000000aa 0 0"
PS = "ps 00000000000000bb " + "00" * 8
PIPELINE = "pipeline 00000000000000aa.0.0 00000000000000bb 0 0 0 0 15 0 1 " + "00" * 16


def entries(path):
    return [line for line in path.read_text().splitlines() if line and not line.startswith("#")]


def test_a_run_folder_adds_its_lines_in_kind_order(tmp_path):
    lists = tmp_path / "lists"
    recorded = tmp_path / "run/runner/shader-lists"
    recorded.mkdir(parents=True)
    (recorded / "a10.txt").write_text(f"{PIPELINE}\n{PS}\n{VS}\n")
    shader_lists.merge([tmp_path / "run"], lists)
    assert entries(lists / "a10.txt") == [VS, PS, PIPELINE]
    assert (lists / "a10.txt").read_text().startswith("# a10: ")


def test_a_folder_of_map_files_merges_as_it_is(tmp_path):
    """the app's shader-lists-missed folder, pulled from a device or a runner"""
    lists = tmp_path / "lists"
    missed = tmp_path / "shader-lists-missed"
    missed.mkdir()
    (missed / "b30.txt").write_text(f"{VS}\n")
    shader_lists.merge([missed], lists)
    assert entries(lists / "b30.txt") == [VS]


def test_lines_already_listed_or_repeated_are_not_added_again(tmp_path):
    lists = tmp_path / "lists"
    lists.mkdir()
    (lists / "a10.txt").write_text(f"# header\n{VS}\n")
    first, second = tmp_path / "one", tmp_path / "two"
    for folder in (first, second):
        folder.mkdir()
        (folder / "a10.txt").write_text(f"{VS}\n{PS}\n{PS}\n")
    added = shader_lists.merge([first, second], lists)
    assert entries(lists / "a10.txt") == [VS, PS]
    assert added == {"a10": 1}


def test_comments_blanks_and_unknown_kinds_are_dropped(tmp_path):
    lists = tmp_path / "lists"
    missed = tmp_path / "missed"
    missed.mkdir()
    (missed / "c40.txt").write_text(f"# comment\n\n  \nbogus 1 2 3\n{PS}  \n")
    shader_lists.merge([missed], lists)
    assert entries(lists / "c40.txt") == [PS]


def test_a_folder_with_no_lists_is_an_error(tmp_path):
    try:
        shader_lists.merge([tmp_path], tmp_path / "lists")
    except SystemExit as error:
        assert "no recorded lists" in str(error)
    else:
        raise AssertionError("merge accepted a folder with no lists")
