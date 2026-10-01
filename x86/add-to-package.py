"""Add x86 D3D10/11 and system Vulkan/OpenGL registration to a staged package.

The base package must already contain the x86 D3D12/Turnip/loader chain.
Sign the entire resulting package and install with pnputil afterward.
"""
import json
import pathlib
import re
import shutil
import sys

win3d = pathlib.Path(__file__).resolve().parents[2]
package = pathlib.Path(sys.argv[1]).resolve()
if package.parent != win3d / "dist/packages":
    raise SystemExit("Expected a staging directory under win3d/dist/packages")
files = {
    "dx11um_virtio_x86.dll": win3d / "virtio-d3d11/build-x86/dist/dx11um_virtio.dll",
    "droidvm_opengl_x86.dll": win3d / "dist/opengl-x86/droidvm_opengl.dll",
}
for name in ("dx12um_virtio_x86.dll", "droidvm_vulkan_x86.dll", "vulkan_freedreno_x86.dll"):
    if not (package / name).is_file():
        raise SystemExit(f"Missing base dependency: {name}")
for name, source in files.items():
    if not source.is_file():
        raise SystemExit(f"Missing build output: {source}")
inf = package / "virtio_wddm_rs.inf"
text = inf.read_text()
sections = ("[SourceDisksFiles]\n", "[VirtioGpu_Files.Usermode]\n", "[VirtioGpu_DeviceSettings]\n")
if any(section not in text for section in sections):
    raise SystemExit("Missing INF package sections")
for name, source in files.items():
    shutil.copy2(source, package / name)
manifest = json.loads((package / "freedreno_icd.arm64x.json").read_text())
manifest["ICD"].update(library_arch="32", library_path=".\\vulkan_freedreno_x86.dll")
manifest_name = "freedreno_icd.x86.json"
(package / manifest_name).write_text(json.dumps(manifest, indent=2) + "\n")
for name in [*files, manifest_name]:
    if f"{name} = 1,," not in text:
        text = text.replace(sections[0], sections[0] + f"{name} = 1,,\n")
    if f"{name},,,0" not in text:
        text = text.replace(sections[1], sections[1] + f"{name},,,0\n")
settings = {
    "OpenGLVersionWow": "%REG_DWORD%, 2",
    "OpenGLFlagsWow": "%REG_DWORD%, 1",
    "UserModeDriverNameWow": r'%REG_MULTI_SZ%, <>,%13%\dx11um_virtio_x86.dll,%13%\dx11um_virtio_x86.dll,%13%\dx12um_virtio_x86.dll',
    "VulkanDriverNameWow": r'%REG_SZ%, "%13%\freedreno_icd.x86.json"',
    "OpenGLDriverNameWow": r'%REG_SZ%, "%13%\droidvm_opengl_x86.dll"',
}
for key, value in settings.items():
    text = re.sub(r"(?m)^HKR,, " + key + r",.*\n", "", text)
    text = text.replace(sections[2], sections[2] + f"HKR,, {key}, {value}\n")
inf.write_bytes(text.replace("\n", "\r\n").encode("ascii"))
(package / "virtio_wddm_rs.cat").unlink(missing_ok=True)
