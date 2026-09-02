# collision-overhaul 设计文档

分支：`collision-overhaul`（基于 `refactor@2176899`）。旧的 `collision` 分支原型已作为 WIP 提交保留（`533c53f`），仅供参考，不再继续。

## 1. 目标

1. 原汁原味保留游戏手感：服务端运行一个无头版本的原版客户端（Ballanced 开源引擎静态链接，NullRasterizer，原版 Gameplay/Levelinit/Balls/关卡与 PH 脚本原样运行），不写任何近似物理。
2. 服务端权威模拟，客户端 lockstep 时间线；客户端本地也运行原版物理来掩盖延迟，并用服务端权威状态做修正。
3. 尽可能真正确定性的物理模拟（固定 tick、固定 delta、IVP 时钟与随机种子重置、位级一致的输入）。
4. 房间系统：房主开房 → 其他玩家加入 → 全员准备 → 房主启动会话。启动时选择"物理会话"或"影子球会话"。影子球会话即现有旧逻辑；物理会话中同房间玩家共享同一物理时间，球可互相碰撞，机关状态与运动同步。
5. 全平台：客户端 Windows（BMLPlus）与 Ballanced 的其它平台；服务端 Windows/Linux。

## 2. 已确认的决策

| 主题 | 决策 |
| --- | --- |
| 客户端引擎二进制 | 先用确定性校验台验证开源 physics_RT 的跨平台一致性；可证明一致则物理会话要求使用开源 physics_RT（按哈希白名单），否则保持原版 DLL，依赖修正掩盖差异。 |
| 服务端平台 | Windows 与 Linux 都要；无头引擎为 Ballanced 开源引擎静态链接（x64）。 |
| 引擎 fork 改动 | 尽量不改；确有必要时允许，必须在 `docs/engine-changes.md` 逐条记录内容与理由。 |
| tick 频率 | 按原版：CK 行为帧 1/66 s，Gameplay 脚本设置的物理 time factor 为 2，因此每 tick 恰好 2 个 IVP PSI（IVP `delta_PSI_time` = 1/66 s）。会话开始时从脚本读取 factor 并校验。 |
| 机关 | 第一阶段镜像：机关脚本只在服务端运行；客户端挂起共享机关的本地脚本根，每 tick 把服务端机关刚体状态写入本地刚体。 |
| 死亡 | 个人死亡不重置共享机关。 |
| BML 版本 | 只支持最新 BMLPlus（当前 0.3.12）；不再构建 BML 0.3.43 目标。 |
| 旧协议 | 保留。房间外的玩家继续走全局影子球模式，旧客户端仍可连接。 |
| Mod | 物理会话要求成员 Mod 一致：服务端配置白名单（mod id + 版本）。测试时两个游戏实例只保留 BallanceMMOClient。 |

## 3. 架构

```
Client Mod (BMLPlus, Win32)        Server (x64, GNS)                  Sim thread (in-process)
  session controller  <-- GNS -->  lobby / rooms / relay  <-- queue -->  HeadlessWorld per room
  local prediction                 legacy shadow-ball path             Ballanced static engine
  input capture / recorder         tick scheduler                      NullRasterizer, null sound
  correction + smoothing           snapshot fan-out                    original scripts + N balls
  automation pipe
```

### 3.1 时间模型

- tick = 1/66 s 真实时间；每 tick 调用一次 `CKContext::Process()`；`CKTimeManager` 的最小/最大 delta 都设为 `1000/66` ms，使行为帧 delta 与墙钟无关。
- 客户端在 `OnProcess` 中做节拍：落后则 `SkipRenderForNextTick()` 连续多跑 tick，超前则等待到下一 tick 时刻。
- 会话开始（以及重开）时两端执行同样的重置：IVP 环境 `base_time/current_time/time_of_last_psi/time_of_next_psi`、`CKIpionManager` 平滑 delta、`ivp_srand(1)`、`qh_srand(1)`（做法与 BallanceTAS `ResetPhysicsTime` 一致）。
- 服务端权威时间线落后客户端约一个单向延迟：tick T 在收齐所有成员的输入或超时后才模拟；超时的玩家沿用上一 tick 输入。

### 3.2 输入

每 tick 一条 `SessionInput{tick, keys(上下左右等), camera_right, camera_forward}`。相机基向量由客户端从 `Cam_OrientRef` 取得并按 Ball Navigation 的方式归一化，服务端直接用它更新该玩家的相机参照实体，不重新计算，保证物理输入两端位级一致。

### 3.3 预测与修正（客户端）

- 本地球：由原版 Ball Navigation 驱动，即本地预测。
- 远端球：第一阶段镜像（每 tick 写入服务端状态，本地 IVP 用服务端速度积分）；第二阶段可选：本地用对方最近输入施力预测。
- 修正：客户端保存每 tick 的刚体状态历史；收到 tick T 的权威状态后与历史比对。误差小于 ε 忽略；中等误差在 K 个 tick 内以位置/速度偏移逐步施加到刚体；大误差直接硬置。不做渲染代理，不做 GGPO 式回滚（IVP 无可靠世界快照恢复）。

### 3.4 无头世界（服务端）

- 加载顺序与客户端一致（base.cmo → 关卡 → 关卡脚本的 Object Load），保证自动命名一致；会话开始时两端交换"按加载顺序的对象名序列哈希"做握手。
- 每玩家：克隆球实体与原版 Physicalize 配方、四个 `SetPhysicsForce` 叶子与 `Physics WakeUp`、独立相机参照、独立无碰撞组；出生点围绕原版 Resetpoint 小半径环形错开。
- 机关激活集合 = 所有玩家所在分节的并集；个人死亡不重置机关。
- 一个模拟线程顺序推进所有房间；每房间独立 CKContext。

### 3.5 房间与协议

- 新增 opcode 追加在旧枚举末尾；旧消息完全不变。
- 房间：`RoomCreate/Join/Leave/Ready/Start/Kick/Close/List`、`RoomState`（广播）、`RoomEvent`。
- 会话：`SessionStart{mode, map, tick0, snapshot_interval}`、`SessionInput`、`SessionSnapshot`（球全量 + 机关增量）、`SessionResync`、`SessionEnd`。
- 物理会话准入：地图与依赖文件哈希核对（服务端下发它实际加载的文件清单，客户端后台线程哈希本地文件；绝不在游戏线程创建第二个 CKContext）、Mod 白名单、physics_RT 哈希（若启用开源 DLL 模式）。

### 3.6 自动化命令通道

客户端 Mod 通过环境变量 `BMMO_COMMAND_PIPE=<name>` 开启命名管道 `\\.\pipe\<name>`，按行接收命令：`mmo <子命令>`（等价于游戏内 `/mmo ...`）、`bml <命令>`（`IBML::ExecuteCommand`）、`level <n>`、`key <名称> <down|up>`、`screenshot <path>`、`quit`、`status`。所有命令在游戏线程执行。测试脚本 `scripts/bmmo_ctl.py` 负责发送。

## 4. 里程碑

1. M0：分支、构建、设计文档、命令通道。
2. M1：确定性校验台（客户端固定 tick + 录制；无头世界回放；逐 tick 比对；跨平台一致性结论）。
3. M2：房间系统与影子球会话。
4. M3：物理会话（多球、tick 协议、预测与修正、机关镜像、分节、死亡、迟到加入、重开）。
5. M4：打磨（host 迁移、重同步、清理、打包）。

## 5. 目录约定

- `BallanceMMOCommon/include/message/room_*.hpp`、`session_*.hpp`：新协议。
- `BallanceMMOServer/room/`：房间管理。
- `BallanceMMOServer/sim/`：无头世界、tick 调度、录制回放工具。
- `BallanceMMOClient/session/`：客户端会话控制、预测修正。
- `BallanceMMOClient/physics/`：原版 physics_RT 私有布局适配（参考 BallanceTAS `physics_RT.h`）。
- `BallanceMMOClient/automation/`：命令通道。
- `docs/engine-changes.md`：引擎 fork 改动记录。
