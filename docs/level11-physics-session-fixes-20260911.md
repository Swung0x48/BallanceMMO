# 第 11 关物理会话的穿透、机关互穿与沙袋抽搐：根因与修复（2026-09-11）

用户报告的现象——玩家球穿进道具石球后才被弹回、机关之间同样穿透后回弹、玩家球被沙袋撞击时
沙袋持续抽搐——来自三个互相独立的原因。修好之后的复测又暴露出第四个：死光所有命重开之后绳子
和沙袋脱开，并把客户端拖到卡死。四个都已修复并复测。

**结论：三个现象分别由 9.17 起的机关"服务端行死推"（Option A）、会话起始的 tick 相位错位与
一个会跑行为帧的"暂停"、以及第 11 关沙袋 `Sequencer` 计数跨关卡重开保留造成。** 前两个的表现
是穿透与回弹，第三个的表现是沙袋反复被纠正。修复后：无头双客户端空闲 30 s 沙袋 0 次回滚、
`mech_max_err=0.0000`；本机两台零售客户端场景跑穿透样本 0、沙袋 0 次回滚；远程服务器两台零售
客户端重同步 0/0。

四项修复对应设计文档的 9.25、9.26、9.27、9.28 小节，本文是面向问题本身的汇总；推导过程、被否掉的
方案和逐条实现细节见 `collision-overhaul-design.md` 对应小节。

## 版本与环境

- 仓库：分支 `collision-overhaul`。9.25–9.27 已提交为 `5870a1a53339`；9.28 在其之上。
- Ballanced：`79850832d846`，分支 `bmmo-collision-overhaul`（引擎改动 #15 已提交）。
- physics_RT（`Source/BuildingBlocks`）：`f62897876421`，分支 `bmmo-collision-overhaul`。
- 物理桥 API：`BMMO_PHYSICS_API_VERSION 10`（保留休眠状态、写入后重算接触的刚体写）。
- 远程服务器：SSH `mc@lm.okbc.st:46981`，游戏 UDP `lm.okbc.st:46995`，运行目录 `/home/mc/bmmo`。
- 零售客户端：Windows x86 `Player.exe` + BMLPlus，两台同机运行，只装 BMMO 与运行时依赖。

## 现象一与现象二：机关被死推，写入时不重算碰撞

### 根因

9.17 的 "Option A" 把机关从"客户端本地模拟"改成"客户端按服务端快照行线性死推"。第 11 关的沙袋
（`P_Modul_26`）是一个受迫摆：两个方向相反的 `SetPhysicsForce` 执行器沿 Halter 的 Z 轴，由一个
1.5 s 的 `Delayer` 来回翻转。对这样的轨迹做线性外推，83% 的时间偏差在 1–3 m。

三个缺陷叠加放大了它：

1. **跳帧快照漏掉睡着的机关。** `physics_world.cpp` 的 `collect_bodies` 里 `if (!full && !body.simulated) continue`，
   一个睡着的机关在增量快照里完全不出现，客户端要等 32–98 tick 的盲窗之后才在一次全量里看到真值，
   于是发生一次大跳。
2. **施加位置时把刚体连同速度一起瞬移，且不重算接触。** IVP 的 `set_transformation` 之后没有
   `recheck_ov_element`，被瞬移的机关和本地玩家球之间不产生接触点；等到穿透被 IVP 的救护脉冲发现时，
   球以 15–33 m/s 被弹出。**这就是"先穿进去再被弹回来"的直接来源**，机关对机关同理。
3. **回滚历史被反复清空。** 任何一个 peer 的 Physicalize/Unphysicalize 都会作废整段历史，全场约 6%
   的时间（50 个窗口，每个一个传输延迟）没有任何纠正在工作，其中一次正好落在球与道具石球接触中间的 35 tick。

此外，9.16 之前的本地预测之所以也"抖"，是另一个原因：零售版的死亡链
（`Gameplay_Ingame / BallManager / Deactivate Ball` → `Execute Script Gameplay_SectorManager`）会重跑本节的
`*_MF Script`；在引擎改动 #6 的 body guard 下刚体活了下来，但 `Set Physics Ball Joint.Create` 会用
IC 复原后的参考系重建关节（`P_Modul_26_Balljoint_oben/unten`），锚点被永久移位。服务端从不跑这条死亡
重置（它只激活各节的并集），所以客户端也不能跑。

### 修复（9.25）

机关回到本地模拟，由回滚引擎按服务端行做**分组**纠正：

- 状态恢复升级到桥 API v10 的 `set_body_state_ex`：补 leapfrog 的 `delta_world_f_core_psis`（每次恢复原本
  丢掉一个 PSI 的位移，约 `|v|/66` m）、恢复后调用 `recheck_ov_element` 重算接触、不再重新武装冻结计时器、
  用 `wake_mode {keep, wake, freeze}` 显式表达休眠意图；客户端读到的位姿比最后一个 PSI 晚一个 PSI（f=1），
  恢复时按 `t_env - time_of_last_psi` 补偿。
- 用每实体的 lifetime generation 取代历史清空。
- 出生 tick 之后按出生记录重新摆位；新增 `mechanism_tracking.hpp` 注册表，零售端与无头端共用。
- 零售端 N1：会话中的死亡链不再执行 `Gameplay_SectorManager`；N2：存档点激活自己这一节时不反激活上一节。
  自动化的 `pausechain` 会同时报告两者的状态。

## 现象三的一半：起始相位错位，以及一个会跑行为帧的"暂停"

### 根因

服务端在全员 Ready 时从 tick 0 开始步进世界，而起始成员把锚点之后的第 k 帧标成基数 B。脚本驱动的机关
因此差了 `B-k` 个 tick 的相位（无头端 8 tick，零售端 11 tick），空闲时每次 `Delayer` 翻转就产生 2 次沙袋纠正。

更要命的是等待 `SessionAssign` 时的"暂停"实现：把时间缩放设成 `1e-6`。它确实停住了脚本计时器和 IVP 的
PSI 推进，**但行为图里按帧计数的 link 照跑不误**——零售端 hold 了 62 帧，本节脚本就比服务端多走了 62 步。

在真实网络上还有第三层：服务端把 tick B 标成"立即到期"，而客户端要等半个往返才收到分配，于是每一个输入
都是迟到的，2 秒后输入饥饿检测器把所有人重同步一遍。

### 修复（9.26 + start lead）

- 服务端世界的第一个 tick 就编号为 B（`physics_world::set_tick_index`、`create_session(start_base)`、
  journal 头 `first_tick = B`）。
- 零售端与无头端在锚点等待 `SessionAssign` 时**一帧都不跑**：零售端在锚点帧的 `OnProcess` 里阻塞轮询
  会话自己的 `assign_queue`（`SessionAssign` 不再走 BML 的 `AddTimer` 队列——那个只在帧之间触发，
  hold 在帧内部，永远等不到），每 1 ms 一次，10 s 超时，掉线也能解除。状态行里的 `held=` 报告轮询次数。
- 起始提前量：调度器从 `now + B × tick_period` 起算，而不是 `now`。B 个 tick 周期正是一个从 0 数起的世界
  走到 B 所需的时间，成员因此保住了基数给他们的墙钟余量。
- 零售端的 `beam` 动作会重新上报 Unphysicalize + Physicalize，服务端的副本也跟着动。

## 现象三的另一半：沙袋首摆方向由一个跨重开保留的计数决定

### 根因

上面两项都修好之后，无头端沙袋已经 0 回滚，**零售端却仍有约一半的会话里两个沙袋每 6 tick 被纠正一次**；
同一对客户端中可能一台干净、另一台一直循环，而打开 `session trace` 又会让它"消失"（纯属时序扰动）。

第 11 关沙袋的 Swing 组在 `Swing.On` 时用一个 `Sequencer` 决定先创建两个方向相反的 `SetPhysicsForce` 中的哪一个。
`Sequencer` 把自己的位置存在输出参数 0（`Current`）里，而 `CKBehavior::Reset()`（零售版与重实现都一样）
只清激活标志和 link 延迟，**从不碰参数**。会话的关卡重开因此保留了进入会话之前那次游玩留下的值，它的奇偶
取决于玩家在本关待了多久（每 1.5 s 翻一次）。

文件里的初值是 −1（"从未触发" → 首次走 Out 1）；服务端的引导过程恰好把它推到了 0（Out 2）。所以"干净"的
客户端只是计数碰巧也等于 0 的那些。**位置和速度的恢复永远补不回这个差异，因为不同的是力的方向，不是状态。**

### 修复（9.27）

新增 `BallanceMMOCommon/include/game/script_state.hpp` 的 `sequencer_state`：

- 关卡加载时记录每一个 `P_*` 所属 `Sequencer` 的计数（服务端与无头端在每个加载 tick 首次见到时记；
  零售端在地图 `OnLoadObject` 之后的头 30 帧里记——hook 自身触发得太早，那时还枚举不到行为块）。
- 锚点时写回。语义是"会话像首次进入关卡那样起动机关"，三端一致。

本关卡其它带状态的块不受影响：`Delayer` 在 `Swing.On` 时重新起算，`TT Scaleable Proximity` 在 Create 时
重新武装。别的关卡若有其它跨重开保留的计数块，按同样办法处理。

顺带确认：沙袋脚本（`P_Modul_26_MF Script`）里**没有** Proximity 块，它无条件摆动；板子（`P_Modul_25`）、
推杆（`P_Modul_01`）和桥（`P_Modul_37`）才有 `TT Scaleable Proximity`（距离 50，ObjectA = `Ball_Pos_Frame`）。

## 现象四：关卡重置让绳子脱开，并把客户端拖到卡死（9.28）

9.25–9.27 上线后的五人复测里出现了两个新现象：死光所有命重开之后有概率顿住卡死（关掉 F3 面板有时能缓解），
之后沙袋的绳子就和沙袋不连接、以奇怪的姿态运动。

### 根因

`Gameplay_Ingame` 有**两处**调用 `Gameplay_SectorManager`，9.25 的 N1 只中和了死亡那一处：

| 调用点 | 触发 | 9.25 状态 |
| --- | --- | --- |
| `BallManager / Deactivate Ball`（脚本第 582 行） | 死亡 | 已中和 |
| `Init Ingame / activate Scripts`（第 249 行） | **关卡重置** | **漏掉了** |

死光命自动重开、以及 ESC 菜单里手动重开，走的都是后者：发 `Reset Level` 消息 → `Event_handler :: reset Level`
把模块的参考系 `P_Modul_26_Balljoint_oben/unten` 用 `TT Restore IC` 放回初始位姿 → 重新激活 `Gameplay_Ingame`
→ `Init Ingame` 调用扇区管理器 → 模块脚本重跑 `Set Physics Ball Joint.Create`。body guard 保住了刚体，新的
关节锚点却建在刚被复位的参考系上，绳子从此永久挂歪。机制与 9.25 修的那个完全相同，只是入口不同。

Journal 精确对上了：重置在 tick 25838，`P_Modul_26_Rope001` 从 tick 25844、`P_Modul_26_Rope` 从 tick 25862
开始被修正，之后每个快照都修正一次，误差从 3.02 m 涨到 4.95 m 且从不收敛。之前四次普通死亡都没有坏。

### 卡死是它的下游

绳子永久发散意味着每个快照都要回滚重演。那个客户端 48 秒内多跑了 32184 个物理步，约 670 步/秒，而实时只有
66 步/秒——十倍的物理负载，最后连自己的输入都停供了 2 秒并触发重同步。

F3 是另一条独立的固定开销：面板文字里带 tick 号，每帧都在变，于是 `BGui::Text::SetText` 每帧把整块文字重新
光栅化一次，整个会话期间一直如此。物理负载翻倍之后这部分正好把帧预算压垮，关掉面板就把它拿回来了。
没有发现死锁：锚点的阻塞等待只存在于会话启动阶段，关卡重置不会再触发它。

### 修复

- N1 从"按组路径找一个块"改成"递归遍历 `Gameplay_Ingame`，清空每一个指向 `Gameplay_SectorManager` 的
  `Execute Script`"，找到少于 2 个就整体拒绝并报错。`Gameplay_Events`（存档点，N2 的地盘）和 `Event_handler`
  的调用点不碰——后者共用 `Level_Init` 发布的共享参数，清掉会波及别处。
- 跳过重新激活不会把扇区放死：重置的 `deactivate Scripts` 迭代的是 `Logic_Scripts`，实测里面只有 20 个
  `Gameplay_*` / `Ball*` / `AnimTrafo_*` 脚本，没有任何模块 MF Script，模块脚本从来就没被停过。
- F3 面板更新限流到 10 Hz。
- 新增自动化动词 `restart`，让回归测试能直接走玩家那条重置路径。

### 一次修坏了的中间版本

第一版把 `activate Scripts` 的调用点在锚点处就清空，结果连**会话自己那次扇区激活也跳过了**——锚点发生在
`Gameplay_Ingame` 刚被激活的那一刻，`Init Ingame` 的链条还没走到 `activate Scripts`。客户端因此完全不模拟
机关：状态行是 `mechanisms=15/0`，journal 里沙袋**没有任何 LOCAL 行**。当时"机关修正 0 次"被误读成完全同步，
其实是本地根本没有可比的东西。

改法：`Deactivate Ball` 那处照旧在锚点立即清空；其余调用点要等 `mechanism_tracking.resolved() > 0`
（扇区已经起来了）才清空，会话启动时的那次激活因此照常发生。

**判断机关是否真在工作，看这两个信号，不要看修正数**：状态行的 `mechanisms=<known>/<resolved>` 第二个数必须是
15；客户端 journal 里沙袋的 LOCAL 行必须存在且在摆动。

### 验证

| 检查 | 结果 |
| --- | --- |
| `mechanisms` | 全程 `15/15`，四次重置前后不变 |
| 客户端 LOCAL 沙袋行 | 存在且摆动，x 范围 12.63 m（服务端 12.29 m）；同 tick mean 0.046 m、max 0.165 m |
| `pausechain` | `death_reset=2/resolved`，两处都是 `applied/now=0` |
| 重置后的沙袋误差 | 稳定 mean 0.048 m / max 0.053 m，**不增长** |
| 对照（修复前） | 重置后每 1000 tick 约 1030 次修正，`resim=143412`，绳子误差 3.02→4.95 m 且从不收敛 |

**残留**：重置过的客户端沙袋会稳定偏离服务端约 5 cm，正好卡在修正阈值上，于是几乎每个快照都被拉回一次
（`mism=3071/4174`、`resim=6590`）。误差不增长说明关节没坏，更像是重置后本地机关相位落后了一点，尚未定位。

**尚未复测的部分**：本轮是在本机零售双客户端（约 0 延迟）上验证的。远端 300 ms 服务器当时没有监听，且其部署
的引擎 build id 比本次构建旧，要复测需要先重新部署服务端。这个缺陷本身与延迟无关，但"300 ms 下的手感没有
变差"尚未在远端确认。存档点那条路的 activate 半边是否也会重建关节同样仍未被测到——两次手动测试里所有玩家
都停在 sector 1。

## 验证

| 测试 | 结果 |
| --- | --- |
| `BallanceMMOMessageTests`（Windows x64 与 Linux） | 162/162 |
| 无头 `probe.py`，第 11 关双客户端空闲 30 s | 沙袋 0 次回滚，`mech_max_err=0.0000`，不符 5/644 与 22/655（余下全是斜面上自己球的 5 cm 容差漂移） |
| 无头，边沿输入第 11 关四人 / 第 13 关四人 | 147–161/930 / 58–71/660 |
| 无头，beam 到道具球上、沙袋撞球 | 修正 2–5 cm，穿透样本 0 |
| 零售双客户端（本机），空闲 30 s | `mism` 1/1 |
| 零售双客户端（本机），场景跑 | 穿透样本 0，沙袋 0 次回滚，道具球撞击 1 次 1.4 cm 修正，被撞期间 0 次修正，`held=0/11..36` |
| 零售双客户端（远程 `lm.okbc.st`） | 重同步 0/0，`held=0/170` 与 `0/185`，穿透样本 0，unmatched 0；`Delayer` 翻转点有少量沙袋纠正（约 8 次 Sack、6 次 Sack001），是真实网络延迟下客户端晚几 tick 看到翻转的预期结果 |
| 9.27 交错进关（0.75 s / 1.1 s） | 两台都记到 −1、锚点写回 −1，出生力 +0.25、tick 119 翻转 −0.25，与服务端一致；tick 19 位姿哈希三端同为 `9eee2492e417b822` |
| 服务端 journal 回放 | 9.26 之后新录的 `matched=1314/1314`；SimTool `--restore-at` A/B 残差约 1e-7 m |
| 自动化 `pausechain` | `death_reset=2/resolved sector_keep=…/applied/now=0`，两处扇区重置都是 `applied/now=0` |

服务端日志会打印一行确认：
`2 of 2 mechanism Sequencer counter(s) restored to the level file's (P_Modul_26_MF=-1, P_Modul_26_MF001=-1)`。

## 玩家能察觉的行为变化

- **会话开始时有一次短暂停顿**：锚点帧阻塞等待分配到达。本机约 11–36 次 1 ms 轮询，远程服务器约 170–185 次。
- **沙袋总是从关卡文件定义的方向起摆**（首次游玩的语义），不再取决于进入会话之前玩了多久。
- **会话中的死亡不再重置机关**：不跑 `Gameplay_SectorManager`，不重建沙袋关节；存档点也不再反激活上一节。

## 代价与已知限制

- 服务端沙袋的首摆方向从 Out 2 变成 Out 1，**9.24 录的旧 journal 从 tick 7（沙袋出生）起不再能回放**；
  9.26 之后新录的 journal 回放正常。
- **迟入者**拿到的是文件原始值的 `Sequencer` 计数，而服务端的已经翻过若干次，方向可能相反，回滚纠正补不回来。
  和迟入者的脚本相位一起待做——需要把机关脚本状态放进全量快照。
- **无头客户端没有 N1/N2**，它自己的死亡仍会重置沙袋关节。
- 服务端 `BodyRevived` 每次唤醒会按成员数重复发送（5 人时 5 次），待去重。
- 多节共享的占位体（设计 A2 §6.3）未处理。
- 重生到起点时若另一名玩家的球一直闲置在那里，会产生一次 0.29 m 的一次性修正（服务端把新球弹起 9 m/s，
  本地没有）。

## 改动清单

| 文件 | 变更 |
| --- | --- |
| `BallanceMMOCommon/include/game/script_state.hpp` | 新增：`Sequencer` 计数的记录与写回 |
| `BallanceMMOClient/BallanceMMOClient.h` | 9.28：N1 改成一张表；F3 面板更新限流到 10 Hz |
| `BallanceMMOClient/automation/client_automation.cpp` | 9.28：新增 `restart` 动词 |
| `BallanceMMOCommon/include/session/mechanism_tracking.hpp` | 新增：机关注册表，零售端与无头端共用 |
| `BallanceMMOCommon/include/physics/physics_rt_api.h` | 桥 API v10（`set_body_state_ex`、`wake_mode`） |
| `BallanceMMOCommon/src/physics/physics_state.cpp` | leapfrog 补偿、接触重算、冻结计时器、出生/约束/力/PSI 诊断记录 |
| `BallanceMMOCommon/include/session/rollback.hpp` | lifetime generation 取代历史清空 |
| `BallanceMMOClient/session/physics_session_client.cpp` | 阻塞 hold、`assign_queue`、锚点写回 `Sequencer`、桥诊断 |
| `BallanceMMOClient/session/physics_session.hpp` | `assign_notice` / `assign_queue`、`hold_polls` |
| `BallanceMMOClient/session/fixed_tick.{hpp,cpp}` | hold 不再操作时间缩放，只置标志并在解除时校正原点 |
| `BallanceMMOClient/BallanceMMOClient.{h,cpp}` | 地图加载后延迟 30 帧枚举行为块，记录 `Sequencer` |
| `BallanceMMOServer/sim/physics_world.{hpp,cpp}` | `set_tick_index`、每加载 tick 记录 `Sequencer`、锚点写回、桥诊断 |
| `BallanceMMOServer/sim/session_runner.{hpp,cpp}` | `start_base` 字段与起始提前量调度 |
| `BallanceMMOServer/sim/session_client.cpp` | 无头端的 `Sequencer` 记录与写回 |
| `docs/collision-overhaul-design.md` | 9.25 / 9.26 / 9.27 小节 |
| `docs/engine-changes.md` | 引擎改动 #15：创建观察点（仅诊断） |
| physics_RT（`CKIpionManager`、`PhysicsCallback`、`PhysicsBallJoint`、`PhysicsForce`） | 引擎改动 #15 的观察点 |
| `scripts/` | `retail_two_client_test.py`、`probe.py`、`analyze_contacts.py`、`phase_check.py`、`dump_window.py`、`run_server_local.py` |

## 留下的诊断手段

桥接事件日志新增四种条目：`birth`（出生时的核心状态、凸包哈希、FPU 字）、`constraint`（球关节创建：两刚体、
立即/延后）、`force`（`SetPhysicsForce` 创建：方向、位置）、`psi`（设 `BMMO_PSI_PROBE=<名字前缀>` 时，出生后
前 6 个 PSI 的核心状态与 sim unit 里核心/控制器的顺序）。零售端在 `session trace on` 时逐条打日志，服务端在
`debug_trace` 时打 `bridge at tick N: ...`；exact 转储窗口为 tick 4..12。

当一个机关"状态完全相同却从出生起就不一样"时，该看的是脚本**建了什么**（`force` / `constraint` 两种条目），
以及哪些脚本块参数能活过一次关卡重开——而不是继续对状态做更精细的恢复。

## 排查中确认"没有差异"的项（不必重查）

physicalize 时的实体矩阵、刚体出生状态（含凸包哈希与惯量）、球关节的创建顺序与立即性、x87 控制字
（客户端 `000a001f` 是 D3D 单精度，服务端 `0008001f`）与 MXCSR 控制位、sim unit 里核心与控制器的顺序、
PSI 相位。`BMMO_SIM_ALLOC_PERTURB` 能改变结果只是因为它改变了时序。针对 IVP 出生路径的独立引擎审计
（Opus 级，报告 `findings/Q-ivp-birth-audit.md`）把 `revive_cores_PSI` 的链表变更和
`announce_controller_to_environment` 里 `mtype` 的锁存列为引擎隐患，但均未验证，且都不是本次的原因。
