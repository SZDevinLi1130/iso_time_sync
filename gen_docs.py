# -*- coding: utf-8 -*-
"""生成《BIS 时间同步技术总结》Word 文档与《客户演示》PPT。"""

from docx import Document
from docx.shared import Pt, RGBColor, Inches
from docx.enum.text import WD_ALIGN_PARAGRAPH
from docx.oxml.ns import qn
from pptx import Presentation
from pptx.util import Inches as PInches, Pt as PPt
from pptx.dml.color import RGBColor as PRGB
from pptx.enum.text import PP_ALIGN

OUT_DIR = "/home/devin/ncs/lim_work/iso_time_sync"

# 中文字体设置
def set_cn_font(run, name="微软雅黑", size=None, bold=None, color=None):
    run.font.name = name
    r = run._element.rPr
    rFonts = r.find(qn('w:rFonts'))
    if rFonts is None:
        rFonts = r.makeelement(qn('w:rFonts'), {})
        r.append(rFonts)
    rFonts.set(qn('w:eastAsia'), name)
    if size:
        run.font.size = Pt(size)
    if bold is not None:
        run.font.bold = bold
    if color:
        run.font.color.rgb = RGBColor(*color)


# ============================ Word ============================
doc = Document()

doc.add_heading("nRF54L15 蓝牙 BIS 时间同步 Demo —— 技术总结", level=0)

# 1 概述
doc.add_heading("1. 概述", level=1)
doc.add_paragraph(
    "本项目在 nRF54L15（NCS v3.4.0 / Zephyr 4.4 / SoftDevice Controller）上，"
    "基于蓝牙 LE 广播等时流（BIS，Broadcast Isochronous Stream），实现 1 个发送端 + 多个接收端之间"
    "的长期时间同步。接收端各自维护一个软件时钟，把本地 GRTC 时间换算到发送端的时间基准上，"
    "使得三方对同一物理事件能打出相同的时间戳，长期精度达到微秒（µs）级。"
)

doc.add_heading("1.1 关键指标", level=2)
tbl = doc.add_table(rows=7, cols=2)
tbl.style = "Light Grid Accent 1"
rows = [
    ("同步精度（时间戳一致）", "约 1 µs（实测 shared_ts 与发送端 timestamp 完全相等）"),
    ("脉冲边沿对齐（逻辑分析仪）", "µs 级（controller-timed 呈现）"),
    ("SDU 间隔 / 更新率", "5 ms（规范最小值）/ 200 Hz"),
    ("BIS 数量 / 拓扑", "1 条 BIS / 1 发 + 2 收（可扩展）"),
    ("长期漂移处理", "晶振漂移（ppm）在线估计 + 外推 + holdover"),
    ("长期稳定性验证", "每 60s 汇总 offset 极差/drift/丢包，支持 ≥1 小时长跑"),
]
for i, (k, v) in enumerate(rows):
    tbl.rows[i].cells[0].text = k
    tbl.rows[i].cells[1].text = v

# 2 原理
doc.add_heading("2. 时间同步原理", level=1)

doc.add_heading("2.1 BLE 广播等时流（BIS）", level=2)
doc.add_paragraph(
    "BLE 等时流（ISO）以固定时间表发送 SDU（Service Data Unit）。SDC 官方文档明确："
    "“SDUs 以固定时间表发送，可让多个接收设备以微秒级精度时间同步”。BIS 属于广播等时流，"
    "由一个 broadcaster 向任意多个 synchronized receiver 单向广播，接收端无需连接、无需回传。"
)

doc.add_heading("2.2 SDU 同步参考时间戳", level=2)
doc.add_paragraph(
    "每个 SDU 都有一个“同步参考时间戳”（SDU synchronization reference），即 BIG 事件锚点时刻。"
    "当 ISO_Interval == SDU_Interval（本项目均为 5ms，每间隔发 1 个 SDU）时，锚点与 SDU 同步参考相同。"
    "发送端用时间戳模式（timestamp mode）把该时刻写入 payload，接收端从 HCI 拿到同一时刻在本地时钟域的读数 "
    "info->ts，从而得到一对同一物理瞬间的两个时钟读数 (tx_ts, info->ts)。"
)

doc.add_heading("2.3 控制器定时呈现（GPIO 对齐）", level=2)
doc.add_paragraph(
    "为避免软件线程调度抖动，脉冲/电平的 GPIO 翻转不走线程，而是用硬件链路：GRTC 比较通道 → DPPI → "
    "GPIOTE SET/CLR 任务，在精确的 presentation 时刻直接驱动 P1.10。发送端与接收端在同一 sync reference + "
    "PD（presentation delay）时刻呈现，边沿天然对齐（µs 级）。"
)

doc.add_heading("2.4 单向时间传递 + 时钟伺服", level=2)
doc.add_paragraph(
    "方案本质是“一次性单向时间传递 + 每接收端独立时钟伺服”："
)
for s in [
    "发送端把本 SDU 的预定发送时间戳 tx_ts 嵌入 payload，作为时间基准载体；",
    "接收端对每个有效 SDU 得到参考对 (tx_ts, info->ts)，用三状态 Kalman 估计 offset / drift / drift-rate；",
    "维护映射 shared(local) = local + offset + drift×age + ½·drift-rate×age²，把本地 GRTC 换算到发送端时间基准。",
]:
    doc.add_paragraph(s, style="List Bullet")

# 3 实现
doc.add_heading("3. 软件实现方法", level=1)

doc.add_heading("3.1 系统架构", level=2)
doc.add_paragraph(
    "发送端：每个 SDU 生成一次触发事件 + 递增 counter，把 (trigger, counter, tx_ts) 组成 9 字节 SDU 经 BIS 广播；"
    "同时在本地 presentation 时刻在 P1.10 上产生 1µs 脉冲。"
    "接收端：解析 SDU，将 (tx_ts, info->ts) 送入时钟伺服，输出 shared_ts，并在 P1.10 上同步呈现脉冲。"
)

doc.add_heading("3.2 SDU 格式", level=2)
p = doc.add_paragraph()
r = p.add_run("[trigger(1 byte)][counter(4 bytes, LE)][tx_ts(4 bytes, LE)]  = 9 字节")
set_cn_font(r, name="Consolas", size=10)
doc.add_paragraph(
    "其中 tx_ts 为发送端控制器时钟的 SDU 同步参考时间戳；counter==0 的首包无有效 tx_ts（置 0），接收端跳过。"
    "CONFIG_BT_ISO_TX_MTU 提升到 16，RX MTU 23 不变。"
)

doc.add_heading("3.3 时钟伺服核心：三状态 Kalman（time_sync.c）", level=2)
doc.add_paragraph(
    "时钟伺服是纯软件模块，每 SDU 一次 O(1) 更新，无动态内存，估计状态 "
    "x = [offset(µs), drift(µs/s), drift-rate(µs/s²)]："
)
for s in [
    "测量：meas = (int32_t)(tx_ts - local_ts)。两时钟同以 1MHz 回绕，32 位有符号差天然 wrap-safe，"
    "状态约束在 [-2^31, 2^31)，新息按 2^32 取模；",
    "预测/更新：标准 3×3 协方差 Kalman，过程噪声用离散 white-noise-jerk 模型（KF_JERK_PSD）；",
    "温度自适应：q = KF_JERK_PSD·(1 + KF_TEMP_Q_GAIN·|dT/dt|)，每秒读片上 TEMP，"
    "温度稳定时安静、变化时敏捷；",
    "抗离群：Huber 软门限，归一化新息超过 3σ 后按 nu²/C² 膨胀 R（平滑降权），"
    "仅 > 100ms 的粗差直接丢弃；",
    "映射：shared_ts = local + offset + drift×age + ½·drift-rate×age²，"
    "holdover 时按最后的 offset/drift/drift-rate 滑行。",
]:
    doc.add_paragraph(s, style="List Bullet")
doc.add_paragraph(
    "相比旧的“128 样本 trimmed mean + 最小二乘”定窗方案：噪声/跟踪滞后由显式噪声模型自动折中，"
    "drift-rate 状态可跟踪温漂斜坡并改善 holdover，离群门限更严格。"
)

doc.add_heading("3.4 关键参数配置", level=2)
tbl2 = doc.add_table(rows=6, cols=2)
tbl2.style = "Light Grid Accent 1"
rows2 = [
    ("CONFIG_SDU_INTERVAL_US", "5000（5ms，规范最小值）"),
    ("CONFIG_TIMED_LED_PRESENTATION_DELAY_US", "1500 µs"),
    ("CONFIG_BT_ISO_TX_MTU", "16（SDU 9 字节 + 余量）"),
    ("num_bis", "1（1 发 2 收共用 BIS index 1）"),
    ("RTN / 触发周期", "1（NSE=2）/ 每个 SDU 一个 1µs 脉冲（5ms）"),
]
for i, (k, v) in enumerate(rows2):
    tbl2.rows[i].cells[0].text = k
    tbl2.rows[i].cells[1].text = v

doc.add_heading("3.5 源码文件", level=2)
for s in [
    "src/iso_tx.c：SDU 组包 + 时间戳嵌入 + 发送端定时呈现；",
    "src/iso_rx.c：SDU 解析 + 时钟伺服接入 + 链路统计；",
    "src/time_sync.c / include/time_sync.h：三状态 Kalman 时钟伺服（Huber + 温度自适应）；",
    "src/sync_log.c / include/sync_log.h：延迟日志（消息队列 + 低优先级线程），"
    "避免串口阻塞收发回调；",
    "src/temp_sensor.c：周期读取片上 TEMP，喂给时钟伺服；",
    "src/timed_led_toggle.c：GRTC + DPPI + GPIOTE 的 controller-timed 呈现；",
    "src/controller_time_nrf54.c：双 GRTC 比较通道；",
    "src/main.c：P1.09 上电选择收发角色。",
]:
    doc.add_paragraph(s, style="List Bullet")

# 4 精度因素
doc.add_heading("4. 影响同步精度的因素", level=1)
tbl3 = doc.add_table(rows=8, cols=3)
tbl3.style = "Light Grid Accent 1"
rows3 = [
    ("因素", "影响", "应对"),
    ("晶振漂移（±20ppm≈70ms/h）", "offset 随时间缓慢变化", "在线估计 drift 并外推"),
    ("SDU 间隔（5ms）", "采样/呈现粒度", "取规范最小值；更细受限于 ISO 间隔下限"),
    ("时间戳量化", "±1µs 噪声底", "GRTC 1MHz 分辨率，属硬性极限"),
    ("presentation delay", "过小错过 deadline，过大被下一 SDU 抢断", "PD+处理提前量 < SDU 间隔"),
    ("丢包 / 重传", "offset 样本离群", "Kalman 新息门限（6σ）剔除"),
    ("printk/线程阻塞", "个别 SDU 处理延迟", "伺服按离群点剔除；低频打印"),
    ("温度变化", "晶振 ppm 漂移", "持续跟踪 drift；必要时温度补偿"),
]
for i, r in enumerate(rows3):
    for j, c in enumerate(r):
        tbl3.rows[i].cells[j].text = c

# 5 产品化
doc.add_heading("5. 产品化注意事项", level=1)
for s in [
    "射频/天线：接收端需可靠同步到 BIG；天线匹配不良会导致收不到广播（本项目曾遇到一块板无法同步，定位为硬件问题）。",
    "统一固件与参数：所有节点必须跑同一固件、同一 PD 与 SDU 间隔，否则 presentation 时刻错位（例如 PD 不一致会产生固定偏置）。",
    "长期运行：32 位时间戳约 71.6 分钟回绕；需要漂移跟踪 + holdover（失同步时按最后速率滑行）与恢复后快速重收敛。",
    "绝对 UTC 对齐：BIS 只能保证节点间相对同步；若需绝对 UTC/网络时间，需要额外时源（GNSS/NTP）或校准协议。",
    "功耗：5ms 间隔意味着 200Hz 的射频/处理活动；低功耗场景需权衡间隔与电流。",
    "验证方法：逻辑分析仪多通道抓 P1.10 脉冲边沿，测最大/最小/标准差；长跑 ≥1 小时看串口 "
    "time_sync_lt / iso_rx_lt 行统计 offset 极差/ppm/最大残差/丢包。",
]:
    doc.add_paragraph(s, style="List Bullet")

# 6 踩坑
doc.add_heading("6. 关键技术细节 / 踩坑记录", level=1)
for s in [
    "net_buf_remove_*() 从缓冲区【末尾】取数（pull 系列才从头部），SDU 解析顺序不可调换。",
    "时钟伺服在 BT RX workqueue 运行，栈很小：大数组必须放静态区，否则栈溢出导致复位。",
    "shared_ts 换算不能混入本地 GRTC 的 64 位高位（GRTC 属 always-on 域，复位不清零），否则凭空多 2^32（约 71.6 分钟）。",
    "5ms 最小间隔下 SDC 为 periodic adv 保留 2.5ms，8 BIS×NSE=2 塞不下，需降 num_bis（本项目降为 1）。",
    "PD + 接收端处理提前量（~3ms）必须小于 SDU 间隔，否则触发事件被下一 SDU 抢断、永不触发。",
]:
    doc.add_paragraph(s, style="List Bullet")

doc.add_heading("7. 实测验证结果", level=1)
doc.add_paragraph(
    "同一 counter 下，接收端 shared_ts 与发送端 timestamp 完全相等（误差 0µs）；"
    "脉冲边沿三通道（TX/RX1/RX2）对齐在 µs 级。日志示例："
)
p = doc.add_paragraph()
r = p.add_run("TX: Sent SDU counter 205600 timestamp 1028163407 us\n"
              "RX: Recv SDU counter 205600 ... shared_ts 1028163407 us")
set_cn_font(r, name="Consolas", size=9)

doc.add_paragraph(
    "长期稳定性：接收端每 CONFIG_TIME_SYNC_LT_STATS_PERIOD_S（默认 60s）输出一行汇总，"
    "用于 ≥1 小时长跑统计。示例："
)
p = doc.add_paragraph()
r = p.add_run("time_sync_lt: t=3600s samples=128 locked=1 offset=123.45 us drift=12 ppm "
              "off_min=123 us off_max=126 us off_span=3 us max_resid=1 us win_updates=12000\n"
              "iso_rx_lt: t=3600s received=720000 lost=0 resync=0")
set_cn_font(r, name="Consolas", size=9)

doc.save(f"{OUT_DIR}/BIS时间同步技术总结.docx")
print("Word 文档已生成")


# ============================ PPT ============================
prs = Presentation()
prs.slide_width = PInches(13.333)
prs.slide_height = PInches(7.5)

def add_slide(title, bullets, title_color=(0x1F, 0x4E, 0x79)):
    slide = prs.slides.add_slide(prs.slide_layouts[6])  # blank
    # title
    tb = slide.shapes.add_textbox(PInches(0.6), PInches(0.4), PInches(12), PInches(1.0))
    tf = tb.text_frame
    tf.text = title
    p = tf.paragraphs[0]
    p.font.size = PPt(34)
    p.font.bold = True
    p.font.color.rgb = PRGB(*title_color)
    p.font.name = "微软雅黑"
    # body
    tb2 = slide.shapes.add_textbox(PInches(0.8), PInches(1.6), PInches(11.7), PInches(5.4))
    tf2 = tb2.text_frame
    tf2.word_wrap = True
    first = True
    for b in bullets:
        para = tf2.paragraphs[0] if first else tf2.add_paragraph()
        first = False
        para.text = b
        para.font.size = PPt(20)
        para.font.name = "微软雅黑"
        para.level = 0
    return slide

# 封面
s = prs.slides.add_slide(prs.slide_layouts[6])
tb = s.shapes.add_textbox(PInches(1), PInches(2.2), PInches(11), PInches(2.6))
tf = tb.text_frame
tf.text = "nRF54L15 蓝牙 BIS 时间同步 Demo"
tf.paragraphs[0].font.size = PPt(44)
tf.paragraphs[0].font.bold = True
tf.paragraphs[0].font.color.rgb = PRGB(0x1F, 0x4E, 0x79)
tf.paragraphs[0].font.name = "微软雅黑"
tb2 = s.shapes.add_textbox(PInches(1), PInches(4.6), PInches(11), PInches(1.0))
tb2.text_frame.text = "微秒级多节点时间同步 · 技术方案与演示"
tb2.text_frame.paragraphs[0].font.size = PPt(24)
tb2.text_frame.paragraphs[0].font.name = "微软雅黑"

add_slide("演示目标", [
    "实现 1 个发送端 + 2 个接收端之间的长期时间同步",
    "三方对同一事件打出相同时间戳，精度达到微秒级（实测约 1µs）",
    "通过 P1.10 的 1µs 窄脉冲，用逻辑分析仪直观展示边沿对齐",
    "为多设备协同、精密测量等场景提供统一时间基准",
])

add_slide("什么是 BLE BIS", [
    "BIS = Broadcast Isochronous Stream（广播等时流），属于 BLE 5.2 ISO 特性",
    "以固定时间表发送 SDU，广播端一对多、无需连接",
    "官方确认：可让多个接收设备实现微秒级精度的时间同步",
    "本 Demo：ISO 间隔 5ms（规范最小值）、1 条 BIS、2M PHY",
])

add_slide("同步原理（一）：时间戳载体", [
    "每个 SDU 有一个“同步参考时间戳”（BIG 锚点时刻）",
    "发送端把该时刻写入 payload：trigger + counter + tx_ts",
    "接收端拿到同一瞬间在本地时钟域的读数 info->ts",
    "于是得到一对 (tx_ts, info->ts)：同一物理瞬间、两个时钟的读数",
])

add_slide("同步原理（二）：时钟伺服", [
    "offset = 发送端时钟 − 接收端时钟（约数十秒，来自开机时间差）",
    "三状态 Kalman 同时估计 offset / drift / drift-rate",
    "新息 6σ 门限剔除丢包/重传离群点",
    "shared = local + offset + drift×age + ½·drift-rate×age²",
])

add_slide("同步原理（三）：控制器定时呈现", [
    "GPIO 翻转不走线程，避免软件调度抖动",
    "硬件链路：GRTC 比较通道 → DPPI → GPIOTE SET/CLR",
    "在精确的 presentation 时刻直接驱动 P1.10",
    "发送端与接收端边沿天然对齐（µs 级）",
])

add_slide("系统架构", [
    "发送端：生成触发事件 → 组包（嵌入时间戳）→ BIS 广播 → 本地定时呈现",
    "接收端：解析 SDU → 时钟伺服（offset/drift）→ shared_ts → 本地定时呈现",
    "两个接收端各自独立运行同一算法，参考同一发送端时钟",
    "三方对同一事件打戳一致，残差 = 呈现误差（µs 级）",
])

add_slide("演示效果", [
    "P1.10 输出 1µs 窄脉冲，每个 SDU 一次（5ms）",
    "逻辑分析仪 3 通道抓 TX / RX1 / RX2 的脉冲边沿",
    "三通道上升沿基本重合，时间差即为同步误差",
    "串口日志：同一 counter 下 shared_ts 与发送端 timestamp 完全相等",
])

add_slide("实测数据", [
    "TX: Sent SDU counter 205600 timestamp 1028163407 us",
    "RX: Recv SDU counter 205600 ... shared_ts 1028163407 us",
    "→ 误差 0µs（时间戳完全一致）",
    "脉冲边沿三通道对齐：µs 级",
])

add_slide("技术优势", [
    "微秒级精度：controller-timed 呈现 + 时钟伺服",
    "长期稳定：在线漂移（ppm）估计 + 外推 + holdover",
    "无需连接/回传：一对多广播，接收端数量可扩展",
    "硬件级确定性：GRTC/DPPI/GPIOTE 免软件抖动",
    "纯软件伺服：O(1) 更新、无动态内存、开销可忽略",
])

add_slide("应用场景", [
    "多传感器/多设备精密时间戳对齐",
    "分布式数据采集、工业控制、状态监测",
    "多通道音频/振动/声学同步采集",
    "需要统一时间基准的协同测量系统",
])

add_slide("总结", [
    "基于 BLE BIS + 控制器时间戳 + 时钟伺服，实现微秒级多节点时间同步",
    "实测时间戳一致（0µs）、脉冲边沿 µs 级对齐",
    "方案轻量、可扩展、长期稳定",
    "适用于对时间精度有要求的协同/测量类产品",
])

prs.save(f"{OUT_DIR}/BIS时间同步Demo演示.pptx")
print("PPT 已生成")
