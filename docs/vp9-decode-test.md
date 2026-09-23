# VP9 关键帧、截断与恢复测试

`tools/vp9-decode-test.c` 重放从标准 V4L2 Request API 捕获的 VP9 控件和一个
真实关键帧，在同一 context 中检查图像、截断错误和关键帧恢复。

输入文件分别为一个原始 `v4l2_ctrl_vp9_frame`、一个原始
`v4l2_ctrl_vp9_compressed_hdr`、去除 IVF 容器头的首帧 VP9 payload，以及这帧
紧密排列的 NV12 软件解码参考。控制结构按目标平台的原生字节序保存，文件长度
必须等于工具编译所使用的 Linux UAPI 结构尺寸。NV12 文件仅包含可见宽高。

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror \
    -o build/vp9-decode-test tools/vp9-decode-test.c
./build/vp9-decode-test \
    --device /dev/video0 --media /dev/media0 \
    --frame-controls frame.bin --compressed-controls compressed.bin \
    --bitstream first-frame.vp9 --reference first-frame.nv12 \
    --data-offset 13
```

默认检查次序为完整关键帧、截断 tile 数据、完整关键帧恢复。截断请求保留两类
header 和一个 tile 数据字节，所以 header 长度小于有效 payload，满足驱动的
header 范围检查。工具要求截断请求的 OUTPUT、CAPTURE 均带 ERROR；真正的错误
来源需要结合内核 IRQ 或 watchdog 日志判断。

每个正常请求均逐字节比较可见 NV12 图像。比较根据协商后的 CAPTURE 行距提取
各行，以协商后的存储高度计算 UV 起始位置，排除行尾及图像底部补齐字节。
每帧提交前使用固定字节填充 CAPTURE，避免前一帧内容掩盖本帧没有输出的问题。

`--fault-first` 将检查次序改为完整关键帧的预期 watchdog ERROR，随后完整关键帧
像素恢复。此模式要求另行准备只丢弃一次完成处理的测试模块；工具本身保持标准
V4L2 UAPI，不配置驱动故障注入。首帧双队列须带 ERROR，完成耗时须处于
1500 毫秒至 10000 毫秒之间，之后关键帧必须逐字节匹配参考。

工具记录每次 request poll 的返回值、事件位、完成耗时、双队列 buffer 标志，
并核查 CAPTURE 时间戳和 request REINIT。每个请求 poll 最多等待 10000 毫秒。
图像与 ERROR 预期失败后仍尝试下一帧恢复；传输错误或等待超时则结束测试并释放
队列。默认完整执行三帧，`--fault-first` 完整执行两帧，全部检查成功时退出码为 0。
