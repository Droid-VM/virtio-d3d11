"""Generate a stable UMD shader-cache version from actual compiler inputs."""
import argparse
import hashlib
from pathlib import Path
import tempfile


SOURCE_DIRS = (
    "src", "include",
    "subprojects/dxbc-spirv/dxbc", "subprojects/dxbc-spirv/ir",
    "subprojects/dxbc-spirv/sm3", "subprojects/dxbc-spirv/spirv",
    "subprojects/dxbc-spirv/util",
    "subprojects/dxbc-spirv/submodules/spirv_headers/include",
)
SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".in", ".inc", ".def",
                   ".comp", ".vert", ".frag", ".geom", ".tesc", ".tese", ".glsl"}


def version(root, target, compiler, flags):
    digest = hashlib.sha256()
    for value in ("droidvm-dxvk-cache-v1", target, compiler, flags):
        digest.update(value.encode("utf-8") + b"\0")
    paths = {root / "version.h.in"}
    for directory in SOURCE_DIRS:
        paths.update(path for path in (root / directory).rglob("*")
                     if path.is_file() and path.suffix in SOURCE_SUFFIXES)
    for path in sorted(paths):
        data = path.read_bytes()
        digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        digest.update(len(data).to_bytes(8, "little"))
        digest.update(data)
    return "umd-" + digest.hexdigest()


def generate(root, output, target, compiler, flags):
    template = (root / "version.h.in").read_text(encoding="utf-8")
    if "@VCS_TAG@" not in template:
        raise ValueError("DXVK version template is missing @VCS_TAG@")
    data = template.replace("@VCS_TAG@", version(root, target, compiler, flags)).encode("utf-8")
    if output.exists() and output.read_bytes() == data:
        return False
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as temporary:
        temporary.write(data)
        temporary_path = Path(temporary.name)
    temporary_path.replace(output)
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--target", required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--flags", required=True)
    args = parser.parse_args()
    generate(args.root, args.output, args.target, args.compiler, args.flags)
