# virtio-d3d11 当前基线

- 当前组件：ARM64 DX11 UMD，包含 AV1 tag/lifetime/flush 修复及 VP input/output view 身份刷新修复。
- 本地仓库：`01-源码/virtio-d3d11`；分支以 `git branch --show-current` 为准；HEAD 以 `git rev-parse HEAD` 为准。
- 对应基线 tag：`server-vp-view-fix-20261101`。
- 服务器源码：`root@192.168.2.244:/srv/virtio-d3d11-edge-vp-view-fix-work/source`。
- 服务器构建目录：上述源码下 `build-edge-vp-view-fix`。
- 本次导入来自与服务器冻结构建对应的本地 source 副本；逐文件 SHA256 与改动清单见 `04-测试记录/仓库整理-20261101/umd-latest-import-manifest.json`。
- 未签名 DLL SHA256：`ACF4E5C684F87E7F0BB6B1E2BA5AE393D2A7E9B27B431B1FC5CEA811E18F9EC2`。
- 已部署签名 DLL SHA256：`F01BB964F102AAB425B4987399D4D3DFBDD7A639A67FEA49C5A7982BC6339C2D`。
- 远程地址：使用本仓库现有 `git remote -v`，本次不更改远程、不推送。

## 构建输入与边界

DXVK 静态库复用服务器 `/srv/virtio-d3d11-umd-tag-unified-candidate/archive-inputs/`；本次未重新编译这些库：

- dxvk.a：`1d5025c619aed63be4cb1967172e276d0e8ba70d677e7e587ca9d63cd55d237c`
- dxvk-d3d11.a：`514905a685250cc611db88600a3485cd3fe3310b5c2839b5510206f94e8d41fe`
- dxvk-dxgi.a：`ebf8585dd35026651cf96996e7484d24dacc0eeef4f9c3080123201cb1da195e`

DXVK 子模块已有未提交修改，本次不重置、不纳入父仓库提交，也不宣称其工作树干净。外部 SDK 头文件仍遵循现有忽略规则。本次保存生产源代码及源码测试，不纳入构建缓存。
