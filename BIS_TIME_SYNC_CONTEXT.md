# nRF54L15 八节点 BIS 时间同步上下文

## 目标

本工程配置为 **1 个 BIS broadcaster + 7 个 BIS receiver**，共 8 个 nRF54L15。广播端建立 8 条 BIS（BIS index 1..8），每个接收设备选择并同步到其中一条 BIS。所有设备使用控制器 ISO 时间戳和 GRTC/DPPI 定时呈现，避免使用 Zephyr 线程调度作为最终同步时基。

## 实现

- `CONFIG_BT_ISO_MAX_CHAN=8`：广播端最多建立 8 条 BIS，并为每条 BIS 准备发送缓存。
- `bis_transmitter.c`：BIG 的 `num_bis` 固定为 8。
- `main.c`：接收端 BIS index 限制为 1..8。
- SDU interval 设为 10000 us；presentation delay 设为 8000 us。较大的 presentation delay 给 host/controller 数据路径留下余量，避免错过 controller-timed deadline；它不会改变各接收端的最终同步参考。
- `CONFIG_BIS_TIME_SYNC_MEASURE_GPIO=y` 强制每个有效 SDU 都重新装载 controller-timed presentation 事件，即使相邻 SDU 的 payload 值相同；这样测量不受业务数据变化影响。
- 精度采集时关闭 host GPIO 即时翻转，避免 printk/线程 GPIO 路径干扰。
- `led1` 的 GPIOTE + DPPI controller-timed 事件是评估跨设备同步误差的首选信号；host 回调边沿不应作为最终精度指标。

## 硬件测量

每块 nRF54L15DK 的 `led1` GPIO 接入同一台逻辑分析仪/示波器的独立通道，并共地。不要把 GPIO 直接互连。使用相同 SDU counter 对齐波形，统计 8 个设备的 presentation edge：

- 最大值 - 最小值：peak-to-peak 同步误差。
- 相对于 broadcaster：每个 receiver 的偏差。
- 连续至少采集 1 分钟，分别记录平均值、标准差、最大绝对值和丢包/重同步次数。

原有 `led1` 是 controller-timed GPIOTE/DPPI 输出，包含最少的软件路径。当前板级 DTS 已提供 `led1`、`led2` aliases；如使用自定义硬件，请在 board overlay 将 `led1` alias 映射到独立测量 GPIO。

## 启动顺序

1. 烧录 1 台 broadcaster，终端选择 `b`；建议先使用 RTN=0 或 1，并设置足够但不过大的 transport latency。
2. 烧录 7 台 receiver，终端选择 `r`，分别选择 BIS index 1..7（或按实验需要分配；一个 BIS 可有多个 receiver）。
3. 等待所有设备输出 `ISO Channel connected`。
4. 逻辑分析仪触发并记录 `led1`（同步呈现）和可选 `led2`（接收/发送参考）信号。

## 精度注意事项

BLE BIS 能保证接收端共享广播事件的同步呈现参考，但不能保证外部 GPIO 的无线空口到 host 接收边沿相同。RTN、PTO、射频干扰、扫描同步建立时间和晶振误差都会影响测量结果。为了获得较高精度：

- 使用无连接、无扫描负载的专用测试环境。
- 保持所有设备同一 PHY、同一 SDU interval 和 BIG 参数。
- 精度采集时不启用 `CONFIG_LED_TOGGLE_IMMEDIATELY_ON_SEND_OR_RECEIVE`，避免 host GPIO 操作污染测量。
- 优先使用 controller-timed `led1` 作为最终误差指标，不要用 printk 时间戳或线程 GPIO 操作作为精度依据。
- 接收端请求确定性的单一 MSE（当前使用 `mse=1`）；如果目标 SDC 版本对该值有不同约束，应按该版本 API 定义调整，不要退回 `BT_ISO_SYNC_MSE_ANY` 做精度基准。
- 将 presentation delay 设置为明显大于最坏的 host 处理时间；不能设置为过小，否则会出现过期 SDU 或错过定时事件。
- 8 台设备同时运行时确认 controller ISO buffer 数量和 RAM 余量；如构建因资源不足失败，应先增大 interval、降低 MTU/RTN 或减少并发 ISO 资源，而不是在接收回调中增加工作量。

## 当前限制

BIS broadcaster 本身只发送 8 条 BIS；Bluetooth controller 不会把 8 个独立 nRF54L15 的本地 GRTC 时钟变成同一物理时钟。这里的“同步”是 ISO 规范定义的同步数据呈现，测得的残余误差必须通过示波器确认。若需求是长期保持绝对 UTC/网络时间一致，还需要额外的时间源或校准协议；本改动不声称提供 UTC 同步。



在现有 BIS 同步呈现基础上，为 1 发 2 收实现长期运行的共享时间戳：SDU 内嵌发送端时间戳，接收端用时钟伺服（偏移+漂移估计）把本地 GRTC 换算到发送端时间基准，三方对同一事件打戳一致（µs 级），并做长期稳定性验证。
