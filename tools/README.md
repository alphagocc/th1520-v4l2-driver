<!-- SPDX-License-Identifier: GPL-2.0-only -->

# 开发与验证工具

以下命令以独立驱动仓库为基础。构建脚本从自身位置确定仓库根目录，
生成源码、模块和测试结果默认分别保存到 `build/`、仓库根目录和 `test-results/`。
内核头文件、编译器、GStreamer、FFmpeg 等由运行环境提供。

| 工具 | 用途与输入 |
| --- | --- |
| `board-build.sh` | 使用匹配当前内核的头文件构建模块；缺少的 media helper 从 Linux stable 官方仓库下载到 `build/` |
| `board-load.sh` | 临时替换并加载本仓库构建的模块 |
| `make-fixtures.sh` | 生成基础 H.264、HEVC 码流和软件解码基准 |
| `make-h264-matrix.sh` | 生成并检查 H.264 profile、参考帧、slice 等组合，详见 [测试矩阵](h264-matrix.md) |
| `make-hevc-matrix.sh` | 生成不同尺寸与编码特征的 HEVC 样本 |
| `fetch-hevc-conformance.sh` | 从 FFmpeg FATE 官方服务下载选定样本与固定版本的预期结果 |
| `decode-matrix.sh` | 对指定目录的码流执行 V4L2 解码和像素比较 |
| `test-containers.sh` | 检查 MP4、Matroska 和三段 CVS 分辨率变化 |
| `request-test.c` | 使用标准 V4L2 Request API 的独立用户态测试，包含自行生成的 I_PCM 图案 |
| `board-test.sh` | 执行基础码流、并发及 Request API 验证 |
| `check-watchdog.sh` | 在独立构建副本中检查看门狗恢复，结束后恢复常规模块 |

`KDIR` 可以指定内核头文件目录。构建工具首先使用系统的
`/lib/modules/$(uname -r)/build`，同时兼容 RevyOS 的头文件安装位置。

在仓库根目录生成容器测试输入：

```sh
sh tools/make-fixtures.sh
sh tools/make-h264-matrix.sh
sh tools/make-hevc-matrix.sh
sh tools/test-containers.sh
```

`test-containers.sh` 的第一个可选参数为独立驱动仓库根目录，第二个为结果目录。
输入位于该仓库的 `test-results/fixtures`、`test-results/h264-matrix` 和
`test-results/hevc-matrix`。显式调用方式如下：

```sh
sh tools/test-containers.sh "$PWD" test-results/containers
```

这些工具及 I_PCM 生成器属于本仓库编写的测试实现。接口依据为公开的
[Linux media 用户态 API](https://www.kernel.org/doc/html/v6.6/userspace-api/media/index.html)、
[FFmpeg 命令文档](https://ffmpeg.org/ffmpeg.html)和脚本中标注的官方源码版本。
自定义测试图像与缩放矩阵由代码生成。

下载的 Linux helper 保留原文件许可标识，并记录来源 URL。
FFmpeg FATE 样本保存在 `test-results/hevc-conformance`，来源记录随下载文件保存；
外部下载内容的授权以其发布者的条款为准。
