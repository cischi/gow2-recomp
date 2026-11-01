import sys
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import package_release  # noqa: E402

DECOYS = ["gow2.log", "OptiScaler.ini", "nvngx_dlss.dll", "nvngx_dlssg.dll", "nvngx_dlssnr.dll", "dxgi.dll",
          "gow2.toml", "game_data/x", "user_data/x", "save.sav"]


def _fake_repo(tmp_path):
    root = tmp_path / "repo"
    (root / "games" / "gow2").mkdir(parents=True)
    for name in ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md", "README.md"):
        (root / name).write_text(name)
    (root / "games" / "gow2" / "gow2.toml").write_text("xenonrecomp")
    build = tmp_path / "build"
    for name in package_release.BUILD_FILES + ["licenses/a.txt", "licenses/b.md"] + DECOYS:
        p = build / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("personal" if name == "gow2.toml" else name)
    return root, build


def test_zip_has_exactly_the_allowlist(tmp_path):
    root, build = _fake_repo(tmp_path)
    z = package_release.package(build, "v1.0.0", tmp_path / "out", root)
    assert z.name == "gow2-recomp-v1.0.0-win64.zip"
    with zipfile.ZipFile(z) as zf:
        names = set(zf.namelist())
    expected = {"gow2-recomp/" + n for n in package_release.BUILD_FILES} | {
        "gow2-recomp/licenses/a.txt", "gow2-recomp/licenses/b.md", "gow2-recomp/LICENSE", "gow2-recomp/NOTICE",
        "gow2-recomp/THIRD_PARTY_NOTICES.md", "gow2-recomp/README.md"}
    assert names == expected


def test_missing_file_names_it(tmp_path):
    root, build = _fake_repo(tmp_path)
    (build / "gow2_dlss.dll").unlink()
    with pytest.raises(SystemExit, match="gow2_dlss.dll"):
        package_release.package(build, "v1.0.0", tmp_path / "out", root)


@pytest.mark.parametrize("version", ["../x", "a/b", "v1", "v1.0.0/../x", ""])
def test_bad_version_rejected(tmp_path, version):
    root, build = _fake_repo(tmp_path)
    with pytest.raises(SystemExit, match="invalid version"):
        package_release.package(build, version, tmp_path / "out", root)
    assert not (tmp_path / "out").exists()


def test_missing_root_file_names_it(tmp_path):
    root, build = _fake_repo(tmp_path)
    (root / "NOTICE").unlink()
    with pytest.raises(SystemExit, match="NOTICE"):
        package_release.package(build, "v1.0.0", tmp_path / "out", root)


@pytest.mark.parametrize("empty", [False, True])
def test_missing_or_empty_licenses(tmp_path, empty):
    import shutil
    root, build = _fake_repo(tmp_path)
    shutil.rmtree(build / "licenses")
    if empty:
        (build / "licenses").mkdir()
    with pytest.raises(SystemExit, match="licenses"):
        package_release.package(build, "v1.0.0", tmp_path / "out", root)
