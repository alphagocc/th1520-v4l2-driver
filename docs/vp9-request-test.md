# VP9 Request API 负向测试工具

`tools/vp9-request-test.c` 通过 Linux 6.6 标准 V4L2 UAPI 检查控件拒绝、
Request 控件完整性、软件参数校验和队列生命周期。测试数据是 64 字节占位内容，
每个提交执行的请求均故意违反后端的参数限制。

本工具的成功结果只证明日志列出的检查通过。实际 VP9 解码、图像像素、概率更新、
参考帧、硬件 DMA、IRQ 和 watchdog 恢复需要另外执行真实码流测试。

## 编译和运行

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror \
    -o build/vp9-request-test tools/vp9-request-test.c
./build/vp9-request-test --device /dev/video0 --media /dev/media0 \
    --iterations 20
```

`--device` 和 `--media` 必须指定相互关联的节点。默认超时为 3000 毫秒，
可以通过 `--timeout-ms` 调整。程序只使用选定节点，结束时关闭自己的队列和文件描述符。

## 检查内容与预期

| 检查 | 预期结果 |
| --- | --- |
| 64×64、Profile 0、8 bit、4:2:0 的 FRAME 控件 | TRY_EXT_CTRLS、S_EXT_CTRLS 成功 |
| Profile 1 的 4:2:2、Profile 2 的 10 bit、奇数宽高 | TRY_EXT_CTRLS、S_EXT_CTRLS 返回 EINVAL |
| Profile 0 缺少横向或纵向采样标志 | TRY_EXT_CTRLS、S_EXT_CTRLS 返回 EINVAL |
| Request 缺少 FRAME、缺少 COMPRESSED_HDR、二者皆缺 | 绑定一个 OUTPUT 后，QUEUE 返回 ENOENT，REINIT 成功 |
| 两类 header 长度合计等于或大于有效 payload | Request 完成，OUTPUT、CAPTURE 均带 ERROR，REINIT 成功 |
| 任一 header 长度为零 | Request 完成，双队列 ERROR，REINIT 成功 |
| FRAME 宽度或高度超过协商尺寸 | Request 完成，双队列 ERROR，REINIT 成功 |
| OUTPUT 请求等待 CAPTURE 时执行 STREAMOFF | STREAMOFF 前请求未完成，随后完成并可 REINIT |
| 再次 STREAMON | 软件参数拒绝和请求完成仍正常 |

Profile 1 和 Profile 2 用例使用符合 Linux 通用 VP9 控件验证规则的 profile、
bit depth、subsampling 组合，以便验证驱动支持范围。Profile 0 的采样标志用例
同时受 Linux 通用控件检查约束。程序记录 UAPI 返回结果，不单独宣称每个拒绝
都由驱动专用回调触发。

后台参数用例使用 `data_offset = 13`，有效 payload 为 64 字节。header 长度检查
因此也覆盖对有效 payload 扣除 `data_offset` 的计算。程序核查完成缓冲的时间戳，
并在完整请求完成及 STREAMOFF 后重试缺少控件的请求，检查前一次控件值没有
代替当前请求必需的控件。

默认一轮包含 21 项检查，其中控件 TRY、S 两次操作合计为一项。每增加一轮，
增加 11 项检查；20 轮为 230 项。程序以退出码 0 表示全部预期通过。

STREAMOFF 检查仅覆盖等待 CAPTURE 的请求和再次启动队列，活动硬件作业的中止
及随后真实关键帧恢复属于单独的板端测试内容。
