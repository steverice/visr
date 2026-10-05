"""tools/embed_texture_policy.py: port/assets/texture-policy.json as a C table for the host build."""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def emit(tmp_path, raw):
    source = tmp_path / "policy.json"
    source.write_text(json.dumps(raw))
    out = tmp_path / "table.c"
    subprocess.run([sys.executable, str(ROOT / "tools/embed_texture_policy.py"), str(source), str(out)], check=True)
    return out.read_text()


def test_the_shipped_policy_becomes_a_table(tmp_path):
    raw = json.loads((ROOT / "port/assets/texture-policy.json").read_text())
    text = emit(tmp_path, raw)
    assert "const struct texture_policy_table texture_policy_embedded" in text
    assert '"levels\\\\a10\\\\bitmaps\\\\tech rack mount", -1, NULL, TEXTURE_POLICY_S4G' in text
    assert '"effects\\\\decals\\\\"' in text and '"diplays"' in text
    assert "12.0f, 30.0f, 1.0f, 3" in text


def test_an_empty_override_list_and_a_pin_with_index_and_hash(tmp_path):
    raw = json.loads((ROOT / "port/assets/texture-policy.json").read_text())
    assert "NULL, 0 }" in emit(tmp_path, dict(raw, overrides=[]))
    pinned = dict(raw, overrides=[{"tag": "a\\b", "index": 2, "hash": "00000000000000aa", "result": "original",
                                   "reason": "r"}])
    assert '"a\\\\b", 2, "00000000000000aa", TEXTURE_POLICY_ORIGINAL' in emit(tmp_path, pinned)
