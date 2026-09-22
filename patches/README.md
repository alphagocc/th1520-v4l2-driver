# GStreamer HEVC 裁剪性能补丁

`gstreamer-1.22-hevc-crop-fastpath.patch` 仅将 HEVC 裁剪转换的
`GST_VIDEO_CONVERTER_OPT_DITHER_QUANTIZATION` 从 `0` 调整为默认值 `1`。
`GstVideoConverter` 在该值为 `1` 时才检查快速转换实现；NV12 裁剪因此可以
使用现有逐平面复制。裁剪区域、输出尺寸及 `GST_VIDEO_DITHER_NONE` 保持原样。

补丁基于 GStreamer 官方 tag `1.22.0`，对应 commit
`f13c65d977b740b5955343d320dcf2061bbdf62d`。目标文件版权属于
Nicolas Dufresne 和 Safran Passenger Innovations LLC，许可为
`LGPL-2.0-or-later`；应用补丁时保留上游文件的版权和许可文本。

源码依据为
[HEVC 裁剪转换参数](https://github.com/GStreamer/gstreamer/blob/1.22.0/subprojects/gst-plugins-bad/sys/v4l2codecs/gstv4l2codech265dec.c#L1096)、
[快速转换入口条件](https://github.com/GStreamer/gstreamer/blob/1.22.0/subprojects/gst-plugins-base/gst-libs/gst/video/video-converter.c#L8174)
及 [NV12 转换实现选择](https://github.com/GStreamer/gstreamer/blob/1.22.0/subprojects/gst-plugins-base/gst-libs/gst/video/video-converter.c#L7973)。

官方 1.22.12、1.24.0、1.26.0 仍包含该参数值。后续 commit
[`e7be87b3de56e8c9922359c410b5c17864cbbf29`](https://github.com/GStreamer/gstreamer/commit/e7be87b3de56e8c9922359c410b5c17864cbbf29)
针对左侧和顶部裁剪量为零的情况改用 `GstVideoMeta` 或图像复制；
[1.26.4 的实现](https://github.com/GStreamer/gstreamer/blob/1.26.4/subprojects/gst-plugins-bad/sys/v4l2codecs/gstv4l2codech265dec.c)
包含这一优化。此次补丁属于本地最小修复，与该上游提交的修改方法有所区别。

在 GStreamer 单一仓库根目录使用 `patch -p1`，在单独的
`gst-plugins-bad-1.22.0` 发行源码目录使用 `patch -p3`。
构建产物放在项目构建目录，通过进程环境中的 `GST_PLUGIN_PATH_1_0`
选择临时插件；系统安装目录保持原样。性能与像素验证结果由板端测试记录提供。
