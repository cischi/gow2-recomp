"""Builds the release zip from an allowlist (never by copying the build folder).

Usage: python tools/package_release.py --build-dir build/clang-release/runtime --version v0.1.0 --out dist
Produces <out>/gow2-recomp-<version>-win64.zip with everything under gow2-recomp/. NVIDIA and OptiScaler DLLs are
deliberately absent: the launcher downloads them.
"""
import argparse
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD_FILES = ["gears.exe", "gow2.exe", "gow3.exe", "gow2_dlss.dll", "amd_fidelityfx_loader_dx12.dll",
               "amd_fidelityfx_upscaler_dx12.dll", "amd_fidelityfx_framegeneration_dx12.dll"]
ROOT_FILES = ["LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md", "README.md"]
# No gow2.toml/gow3.toml: games/<id>/<id>.toml is the XenonRecomp config (the exe rejects it); the launcher writes the settings file.
TOP = "gow2-recomp/"


def package(build_dir: Path, version: str, out_dir: Path, root: Path) -> Path:
    if not re.fullmatch(r"v\d+\.\d+\.\d+[\w.\-]*", version):
        raise SystemExit(f"invalid version {version!r}: expected like v0.1.0")
    entries = [(build_dir / n, TOP + n) for n in BUILD_FILES]
    lic = build_dir / "licenses"
    entries += [(p, TOP + "licenses/" + p.relative_to(lic).as_posix()) for p in sorted(lic.rglob("*")) if p.is_file()]
    entries += [(root / n, TOP + n) for n in ROOT_FILES]
    for src, _ in entries:
        if not src.is_file():
            raise SystemExit(f"missing release file: {src.name} ({src})")
    if not any(a.startswith(TOP + "licenses/") for _, a in entries):
        raise SystemExit(f"missing release file: licenses/ ({lic})")
    out_dir.mkdir(parents=True, exist_ok=True)
    zip_path = out_dir / f"gow2-recomp-{version}-win64.zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for src, arc in entries:
            zf.write(src, arc)
    return zip_path


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir", type=Path, required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", type=Path, default=Path("dist"))
    a = ap.parse_args()
    print(package(a.build_dir, a.version, a.out, ROOT))


if __name__ == "__main__":
    main()
