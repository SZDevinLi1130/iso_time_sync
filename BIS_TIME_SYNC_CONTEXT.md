# nRF54L15 BIS 共享时间戳上下文（1 发 + 2 收）

## 目标

1 个 BIS broadcaster + 2 个 BIS sync receiver。广播端把每个 SDU 的预定发送
时间戳嵌入 payload，接收端用时钟伺服（offset + drift 估计）把本地 GRTC 换算
到广播端时间基准，使三方对同一物理事件打出相同时间戳（µs 级），并支持长时间
（≥ 1 小时）稳定运行与验证。

## 当前实现（以代码为准）

- 拓扑：1 条 BIS（`num_bis=1`），所有接收端同步到 BIS index 1；接收端数量可扩展。
- 参数：SDU interval 5000 µs（规范最小值），presentation delay 1500 µs，2M PHY，
  RTN=1（NSE=2）。
- SDU 9 字节：`[trigger(1)][counter(4, LE)][tx_ts(4, LE)]`；`counter==0` 的首包
  无有效 tx_ts（置 0），接收端跳过。
- 触发：每个 SDU 在 controller-timed presentation 时刻在 GPIO（nRF54L15DK 为
  `led1` / P1.10）上产生 1 µs 窄脉冲，供逻辑分析仪对齐。
- 角色选择：上电采样 `sw1`（P1.09），低电平 → 广播端，否则接收端；`sw0`（P1.13）
  在广播端触发一次立即同步事件（SDU 打 `SYNC_EVENT_TRIGGER_VAL=2`，三方打印同一
  counter/timestamp）。
- 发送端定时呈现由 GRTC 比较通道 → DPPI → GPIOTE SET/CLR 硬件链路完成，不走线程。

## 时钟伺服：三状态 Kalman（src/time_sync.c）

每个有效 SDU 提供一对同一物理瞬间的读数 `(tx_ts, info->ts)`。滤波器估计

```
x = [ offset (µs), drift (µs/s), drift-rate (µs/s²) ]
```

- 测量：`z = (int32_t)(tx_ts - local_ts)`。两时钟同以 ~1 MHz 回绕，32 位有符号差
  天然 wrap-safe；状态被约束在 `[-2^31, 2^31)`，新息按 2^32 取模。
- 预测/更新：标准 3×3 协方差 Kalman，`F = [[1,dt,dt²/2],[0,1,dt],[0,0,1]]`，
  过程噪声用离散 white-noise-jerk 模型（`KF_JERK_PSD`）。
- 抗离群：新息 `|innov| > KF_GATE_SIGMA·√S` 或 > 100 ms 直接丢弃，替代原来的
  trimmed mean 百分位带。
- 映射：`shared(local) = local + offset + drift·age + 0.5·drift-rate·age²`，
  `age` 为距上次更新的本地时间；holdover 时按最后的 offset/drift/drift-rate 滑行。
- `time_sync_to_shared()` 必须保持 32 位无符号 wrap-safe：不能混入本地 GRTC 64 位
  高位，也不能按有符号解释（raw controller time 在 ~35.8 min 后越过 2^31）。

相比旧的"128 样本 trimmed mean + 最小二乘"定窗方案：噪声/跟踪滞后由显式噪声模型
自动折中，drift-rate 状态可跟踪温漂斜坡并改善 holdover，离群门限更严格。调参常量
`KF_*` 在 `time_sync.c` 顶部，需用下面的 Allan/TDEV 实测结果进一步收敛。

## 日志：延迟输出（src/sync_log.c）

周期日志与同步事件日志不再在 BT RX/TX 回调里 `printk`，而是由回调把数值字段塞进
一个定长消息队列，由低优先级线程（`K_LOWEST_APPLICATION_THREAD_PRIO`）格式化输出。
队列满时丢弃、绝不阻塞调用者，从而保证 controller-timed 呈现路径不被串口延迟拖累。

## 长期稳定性验证

接收端周期性输出（`CONFIG_TIME_SYNC_LT_STATS_PERIOD_S`，默认 60 s）：

- `time_sync_lt: t=...s samples=... rejected=... locked=... offset=...us
  drift=...ppm off_min=...us off_max=...us off_span=...us max_resid=...us
  win_updates=...`
  其中 `off_span`（窗口内 offset 极差）随时间是漂移/温漂未补偿的直接指标，
  `rejected` 是被离群门限丢弃的样本数。
- `iso_rx_lt: t=...s received=... lost=... resync=...` 统计收到的 SDU、由 counter
  间隔估算的丢包数，以及大间隔（≥ 1 s）重同步次数。
- 另有 10 s 一次的 `time_sync:` 行给出即时 offset/drift/最大残差，便于观察收敛过程。

长跑步骤：三块板（1 TX + 2 RX）持续运行 ≥ 1 小时，串口记录上述行，统计 offset
范围、drift（ppm）、max_resid 与丢包/重同步；同时用逻辑分析仪多通道抓脉冲边沿，
测最大/最小/标准差。

## 硬件测量

- 每块 nRF54L15DK 的 `led1` GPIO 接同一台逻辑分析仪/示波器的独立通道，并共地。
  不要把 GPIO 直接互连。
- 用相同 SDU counter 对齐波形，统计各设备 presentation 边沿：最大值 − 最小值为
  peak-to-peak 同步误差；相对广播端的偏差为各接收端偏差。
- 连续采集 ≥ 1 分钟（稳定性验证建议 ≥ 1 小时），分别记录平均值、标准差、最大绝对
  值和丢包/重同步次数。

## 启动顺序

1. 烧录 1 台广播端（`sw1` 拉低进入广播端角色）。
2. 烧录 2 台接收端（`sw1` 保持高）。
3. 等待所有设备输出 `ISO Channel connected`。
4. 逻辑分析仪记录 `led1` 脉冲；串口观察 `shared_ts` 与广播端 timestamp 是否一致。

## 精度注意事项

- 使用无连接、无扫描负载的专用测试环境；所有设备同一 PHY、同一 SDU interval 和
  BIG 参数。
- 精度采集时不启用 `CONFIG_LED_TOGGLE_IMMEDIATELY_ON_SEND_OR_RECEIVE`，避免 host
  GPIO 操作污染测量。
- 以 controller-timed `led1` 边沿为最终误差指标，不要用 printk 时间戳或线程 GPIO
  操作。
- presentation delay 不能随意加大：接收端在 SDU 时间戳前 ~3 ms 处理，5 ms 间隔下
  上限约 2000 µs；超过后触发会被下一个 SDU 的重装载抢断、永不触发。当前 PD=1500 µs
  留 ~0.5 ms 余量。要保护这个余量应缩短回调（日志已延迟到低优先级线程），而不是
  加大 PD。
- 伺服运行在 BT RX workqueue，栈很小：大数组必须放静态区，否则栈溢出导致复位
  （Kalman 的 `kf_P`/`kf_x` 均为静态）。

## 当前限制

BIS 只保证接收端共享广播事件的同步呈现参考，不会把各设备本地 GRTC 变成同一物理
时钟；这里的“同步”指 ISO 规范定义的同步呈现与共享时间戳，残余误差须用示波器/
逻辑分析仪确认。若需长期保持绝对 UTC/网络时间一致，还需额外时源（GNSS/NTP）或
校准协议。
