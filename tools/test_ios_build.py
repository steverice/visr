"""Tests for tools/ios_build.py's pure helpers."""

import pytest

from tools import ios_build

# the iPhoneOS SDK's OpenGLES.tbd as Xcode 27 ships it, cut down to two symbols
STUB = """--- !tapi-tbd
tbd-version:     4
targets:         [ arm64e-ios, arm64e.x1-ios ]
install-name:    '/System/Library/Frameworks/OpenGLES.framework/OpenGLES'
exports:
  - targets:         [ arm64e-ios, arm64e.x1-ios ]
    symbols:         [ _EAGLGetVersion, _glActiveTexture ]
...
"""


def test_the_stub_links_against_the_mac_s_maccatalyst_opengles():
    stub = ios_build.maccatalyst_stub(STUB)
    assert stub.count("[ arm64-maccatalyst, arm64e-maccatalyst ]") == 2
    assert "-ios" not in stub
    assert "install-name:    '/System/iOSSupport/System/Library/Frameworks/OpenGLES.framework/OpenGLES'" in stub
    assert "_EAGLGetVersion, _glActiveTexture" in stub


def test_a_stub_with_other_targets_is_refused():
    with pytest.raises(ValueError, match="does not list"):
        ios_build.maccatalyst_stub(STUB.replace("arm64e.x1-ios", "arm64e.x2-ios"))


def test_a_stub_with_another_install_name_is_refused():
    with pytest.raises(ValueError, match="install name"):
        ios_build.maccatalyst_stub(STUB.replace("/System/Library/Frameworks/OpenGLES", "/usr/lib/OpenGLES"))


def test_a_stub_naming_an_ios_target_the_rewrite_misses_is_refused():
    with pytest.raises(ValueError, match="iOS target"):
        ios_build.maccatalyst_stub(STUB + "uuids:\n  - target: arm64e-ios\n")


def test_the_catalyst_build_goes_in_the_checkout_by_default():
    assert ios_build.mac_build_folder({}) == ios_build.ROOT / "build/mac/app"


def test_a_host_s_designated_folder_takes_the_catalyst_build(tmp_path):
    assert ios_build.mac_build_folder({"HALO_MAC_BUILD": str(tmp_path)}) == tmp_path / ios_build.ROOT.name / "app"
