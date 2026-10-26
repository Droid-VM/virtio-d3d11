# Windows on ARM: 32-bit D3D10/11 and OpenGL

Build with `bash win3d/build.sh d3d11 opengl --arch x86`. This uses llvm-mingw's
`i686-w64-mingw32` target, separate build directories, and `win3d/mesa` with the
Mesa x86 cross-file shared with `virtio-d3d12/x86`. Build the accompanying
D3D12/Turnip/loader chain with `bash win3d/build.sh d3d12 turnip loader --arch x86`.
The old x86 build entry points forward to this unified builder.

For a base package that already declares the x86 files, append `--package` to
the unified build command to replace only the selected architectures and sign
the complete package. The manual helper below is for introducing x86 entries
into an older base package.

Create a new uninstalled package with `new-gpu-driver.sh --no-sign --no-zip
--allow-unchanged`, then run:

```
python3 win3d/virtio-d3d11/x86/add-to-package.py win3d/dist/packages/gpu-driver-N
bash win3d/signing/sign-gpu-driver.sh win3d/dist/packages/gpu-driver-N
bash win3d/deploy-gpu-driver.sh gpu-driver-N -- -NoRestart
```

The package helper adds the x86 DLLs and ICD manifest, preserves the native
ARM64/ARM64EC entries and x86 D3D12 slot, and invalidates the old catalog.
OpenGL requires all three WoW values: `OpenGLDriverNameWow`,
`OpenGLVersionWow`, and `OpenGLFlagsWow`. Registering only the filename lets
Windows load the DLL but reports version/flags zero and fails context creation.
The D3D9 slot stays empty intentionally: Windows uses the existing D3D9On12
hardware path. No app-local D3D9 replacement is required by these probes.

Validate in fresh x86 processes using the system runtimes: D3D9/10/11 draw and
readback, D3D12 device/descriptor operations, Vulkan device creation without ICD
environment overrides, and OpenGL rendering through both Freedreno and Zink.
Check the actual loaded DriverStore paths. Never overwrite loaded DriverStore
files; a changed KMD or a pending PnP restart still requires a reboot.
