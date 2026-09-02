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
| 客户端引擎二进制 | 已用确定性校验台证明：装有开源 physics_RT（含 BMMO 桥接）的原版客户端与 Windows x86/x64、Linux x86_64、Android ARM64 的无头引擎逐帧位级一致（见第 6 节）。因此物理会话要求客户端使用开源 physics_RT（按哈希白名单），不再考虑原版 DLL + 修正掩盖的方案。 |
| 服务端平台 | Windows 与 Linux 都要，其它平台（含 ARM64）也必须能跑；无头引擎为 Ballanced 开源引擎静态链接，已在 Windows x86/x64、Linux x86_64（GCC）、Android ARM64（NDK clang）验证一致。 |
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
- 会话开始（以及重开）时两端执行同样的重置：IVP 环境 `base_time/current_time/time_of_last_psi/time_of_next_psi`、`CKIpionManager` 平滑 delta 与**物理时间因子**（重置为新建世界的 1.0，即 `m_PhysicsTimeFactor = 0.001`：已经跑过一次关卡的客户端会带着 Gameplay 脚本设过的 2.0 进入锚点，而刚启动的服务端世界是 1.0，锚点后第一个 tick 一边跑 1 个 PSI、一边跑 2 个，IVP 绝对时间从此相差 1/66 s；M3 联调中实测如此）、`ivp_srand(seed)`、`qh_srand(1)`（做法与 BallanceTAS `ResetPhysicsTime` 一致）。
- 服务端权威时间线落后客户端约一个单向延迟：tick T 在收齐所有成员的输入或超时后才模拟；超时的玩家沿用上一 tick 输入。
- **休眠判定与全局状态解耦（引擎改动 #5）**：原版 IVP 用环境里唯一的倒计数决定哪个 PSI 做"静止检查"（每个被模拟的 sim unit 每 PSI 都减一，归零时用 `ivp_rand()` 重置为 15..19），所以任何一个刚体何时冻结取决于此前有多少**别的**刚体在动、以及全局 RNG 游标。两端只要清醒刚体集合不同（别的玩家所在分节、客户端本地重置过的机关），同一个球就会差几个 PSI 冻结、停在略不同的位置——M3 联调里球每 ~50 tick 需要一次 ~1 cm 修正就是它。现在每个 sim unit 自带倒计数（首次 10 PSI，之后每 17 PSI，无随机），`ivp_rand()` 不再被模拟使用，`ivp_srand(seed)` 只是保留的接口。多房间时也不再需要按世界保存/恢复 RNG 游标。

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

自动化命令有两条等价通道，命令集相同（`ping`/`status`/`physview`/`physobjs`/`level N`/`key`/`record`/`quit` 等），都在游戏线程的 OnProcess 里派发：

- **命名管道**（`BMMO_COMMAND_PIPE`）：交互式使用，一问一答。
- **命令文件**（`BMMO_COMMAND_FILE`，缺省 `Bin/bmmo_command.txt`）：更稳的回退方式。每帧若文件存在则读取整份、逐行派发、把结果写入同目录 `<file>.out` 和 BML 日志（`CommandFile: <cmd> -> <resp>`），随后删除该文件。派发在 OnProcess 最前面执行，即使后续逻辑抛异常也不受影响，因此既能驱动游戏又能用日志观察是否按预期工作。


客户端 Mod 通过环境变量 `BMMO_COMMAND_PIPE=<name>` 开启命名管道 `\\.\pipe\<name>`，按行接收命令：`mmo <子命令>`（等价于游戏内 `/mmo ...`）、`bml <命令>`（`IBML::ExecuteCommand`）、`level <n>`、`key <名称> <down|up>`、`screenshot <path>`、`quit`、`status`。所有命令在游戏线程执行。测试脚本 `scripts/bmmo_ctl.py` 负责发送。

## 4. 里程碑

1. M0：分支、构建、设计文档、命令通道。
2. M1（已完成）：确定性校验台（客户端固定 tick + 录制；无头世界回放；逐 tick 比对）。结论：四个平台对同一段 Level 1 录制（2345 帧，含开场碎块、键盘操控、死亡重置）全部位级一致，见第 6 节。
3. M2（已完成）：房间系统与影子球会话。房间协议（`room_request`/`room_state`/`room_event`）落地，服务端 `BallanceMMOServer/room/room_manager.hpp` + `server.cpp`，客户端 `/mmo room ...` 命令与 `session/room_client.cpp`；球状态按房间过滤。详见第 7 节。
4. M3（已完成，提交 f7e1060）：物理会话（多球、tick 协议、预测与修正、机关镜像、分节、死亡）。实施设计与联调结果见第 8 节。
5. M4（进行中）：远端球本地预测、重同步与暂停、迟到加入/断线/host 迁移的路径验证、服务端校验客户端事件、清理与打包。实施设计见第 9 节。

## 5. 目录约定

- `BallanceMMOCommon/include/message/room_*.hpp`、`session_*.hpp`：新协议。
- `BallanceMMOServer/room/`：房间管理。
- `BallanceMMOServer/sim/`：无头世界、tick 调度、录制回放工具。
- `BallanceMMOClient/session/`：客户端会话控制、预测修正。
- `BallanceMMOClient/physics/`：通过 physics_RT 导出的 `bmmo_physics_api` 桥接表读取物理世界（哈希、刚体、事件日志）；原版 DLL 私有布局适配已删除。
- `BallanceMMOCommon/include/physics/`、`src/physics/`：桥接 C API、世界哈希、录制格式、可移植数学（OpenLibm 子集）与确定性排序垫片。
- `cmake/PhysicsFloatingPoint.cmake`、`PortableMath.cmake`、`PhysicsRTPlugin.cmake`、`BallancedHeadless.cmake`：客户端插件与无头引擎共用的确定性构建规则。
- `BallanceMMOClient/automation/`：命令通道。
- `docs/engine-changes.md`：引擎 fork 改动记录。


## 7. 房间与影子球会话（M2）

房间协议见 `docs/rooms-and-sessions-protocol.md`，本节记录落地情况与已验证行为。

实现：
- 协议：`BallanceMMOCommon/include/entity/room.hpp`（共享枚举与 POD），`message/room_request_msg.hpp`、`room_state_msg.hpp`、`room_event_msg.hpp`（显式小端、逐字段边界检查）。新 opcode 追加在 `bmmo::opcode` 末尾，旧消息不变。
- 服务端：`BallanceMMOServer/room/room_manager.hpp`（纯房间状态，无网络：create/join/ready/start/kick/close/leave 与房主迁移），`server.cpp` 的 `handle_room_request` 收发消息、`state_mutex_` 下串行；`tick()` 的球广播按房间分组，房内成员只收本房间的球，房外玩家（room 0）之间仍互见；断连自动退房并通知。config 增加 `rooms_enabled`/`maximum_rooms`/`maximum_members`。
- 客户端：`/mmo room list|create|join|leave|ready|start|kick|close|status` 子命令（`BallanceMMOClient.cpp`），`RoomState`/`RoomEvent` 处理与渲染在 `session/room_client.cpp`；Tab 补全已加。

已验证（本机，服务端 + 原版客户端 + MockClient）：
- 房间生命周期：create → list → status → ready → start(shadow) → leave，单成员离开自动关房；房主标记、ready 状态、phase（lobby/running）转换均正确。
- 球状态按房间过滤：真客户端在关卡内、建房后移动，房外的 MockClient 收到该玩家 0 条球更新；离房后重新收到（动态生效）。基线（两者都在房外）互见正常。
- `room_manager` 纯逻辑单元测试（`BallanceMMOServer/tests/room_manager_test.cpp`，gtest）：容量上限、ready 门禁、房主迁移、踢人、关房等断言全过。

范围说明：M2 只把球状态按房间隔离；聊天保持全局（符合协议 1.2）。倒计时/分节等旧广播仍全局，留待后续。物理会话（`mode=Physics`）此阶段返回 `PhysicsUnavailable`，由 M3 实现。
## 6. 确定性结论与要求（M1）

同一份客户端录制（`BallanceMMOClient` 命令通道 `record start`，BMRC v2）在下列引擎上回放，2345 帧的物理哈希、刚体姿态、探针位置/速度全部一致，输出录制文件逐字节相同：

| 引擎 | 编译器 | 结果 |
| --- | --- | --- |
| 原版客户端 + 开源 physics_RT.dll（x86） | MSVC | 参考 |
| 无头引擎 Windows x86 / x64 | MSVC `/fp:precise` | 2345/2345 |
| 无头引擎 Linux x86_64（WSL Arch） | GCC 16 | 2345/2345 |
| 无头引擎 Android ARM64（手机，adb） | NDK 28 clang | 2345/2345 |

要做到这一点必须同时满足：

1. 客户端与服务端使用同一份开源 physics_RT 源码；引擎 fork 的两处改动见 `docs/engine-changes.md`（向量 FPU 块大小固定为 1；`XArray/XSArray::Sort` 与 qhull 改用与 MSVC 运行库同序的确定性快速排序）。
2. 浮点：MSVC `/fp:precise`（x86 加 `/arch:SSE2`），GCC/Clang `-ffp-contract=off -fexcess-precision=standard`；不能用 `/fp:strict`（x86 碰撞代码会崩）。
3. 超越函数不走各平台 libm，而是构建进物理模块的 OpenLibm 子集（`BallanceMMOCommon/third_party/openlibm`）；`sqrt`/`fabs`/`floor` 等精确运算保持硬件实现。
4. 无头引擎的空管理器必须严格遵守 CK2 的调用约定（例如 `CKWaveSound::PlayMinion` 传给 `Play` 的是 `SoundMinion` 包装而非源句柄）；此前所有“随内存布局变化”的假性不确定现象都来自这一处堆越界。

验证工具：`BallanceMMOSimTool --replay`（`--write-record`、`--exact-frames`、`--bodies-frames`、`--debug-ticks`）、`scripts/bmrc_diff.py`、`scripts/test_det_qsort.cpp`；定位平台差异的方法是对两个引擎逐 tick 的行为块执行轨迹做 diff。

## 8. 物理会话（M3）

本节是 M3 的实施设计，依据第 3 节的架构与第 6 节的确定性结论，并按 2026-09-02 对原版脚本图（`BallanceMMOSimTool --dump-script`）的实际读取结果修正了若干细节。协议见 `docs/rooms-and-sessions-protocol.md` 第 2 节。

### 8.1 原版脚本事实（Level 1 实测）

| 事实 | 出处 |
| --- | --- |
| 球导航 = `Gameplay_Ingame/Ball Navigation` 内四个 `SetPhysicsForce` 叶子（Position 0,0,0，Pos Referential = 当前球，Direction 为 (1,0,0)/(-1,0,0)/(0,-1,0)/(0,1,0) 之一，Direction Ref = `Cam_OrientRef`，Force Value = `Physicalize_GameBall` 表该球型的 Force 列），每个叶子由一个 `Key Event` 驱动：Pressed → Create，Released → Shutdown；Create/Shutdown 的 Out 都接 `Physics WakeUp`。第五个叶子（Direction 0,0,1）只在 Gameplay_Init 的调试标志为 TRUE 时启用，原版恒为 FALSE。 | `Gameplay_Ingame` 图 |
| `SetPhysicsForce.Create` 在脚本执行的当场创建 `PhysicsControllerForce`（`PhysicsCallbackContainer::Process(cb)` 立即执行回调，只有球尚无刚体时才排队到 PreSimulate 重试）：力向量 = Cam_OrientRef.TransformVector(Direction) 归一化 × Force，每个 PSI 通过 `async_push_core` 施加；Shutdown 立即删除控制器。控制器加入 core 的顺序 = 创建顺序。同一帧内 `Ball Navigation` 先于相机脚本执行，因此 Create 读到的是**上一帧末**的 Cam_OrientRef 矩阵（离线回放实测：用本帧末矩阵会在按键帧偏差一个方向分量）。 | `physics_RT/Behaviors/PhysicsForce.cpp`、`PhysicsCallback.cpp`、离线回放 |
| 球的物理配方：Wood/Stone 为 Ball Count=1、半径 2、Collision Surface=`Ball_X_Mesh`；Paper 为 Convex Count=1、Convex=`Ball_Paper_Mesh`。Friction/Elasticity/Mass/Linear Damp/Rot Damp 来自 `Physicalize_GameBall`（Paper 0.5/0.4/0.2/1.5/0.1，Stone 0.5/0.1/10/0.3/0.1，Wood 0.8/0.2/1.9/0.9/0.1），Collision Group=`Ball`，Automatic Calculate Mass Center=FALSE 且 Shift Mass Center=0,0,0（即显式的零质心偏移，走 `mass_center_override` 路径）。 | `physicalize new Ball` 子图、`Physicalize_GameBall` 数组 |
| 关卡开始：`Gameplay_Ingame` 激活 → `Init Ingame`（Set Physics Globals：重力 0,0,-20、时间因子 2；Execute Script `Gameplay_SectorManager` 激活分节 1）→ `BallManager/New Ball`：Set World Matrix(CurrentLevel[0,3] 复活点) → Delayer 3 s → `physicalize new Ball` → Physics WakeUp。 | `Gameplay_Ingame` 图 |
| 死亡：`BallManager` 每帧用 `Box Box Intersection` 检测球与 `DepthTestCubes`；命中后 BallNav deactivate → 1 s 后 Unphysicalize 并隐藏 → `Set Cell IngameParameter[0,1]=[0,2]=当前分节` 并 Execute `Gameplay_SectorManager`（即重置当前分节机关）→ New Ball（3 s 后在复活点重新 Physicalize）。 | `Deactivate Ball` 子图 |
| 变球：`Trafo Manager` 用 `Get Nearest In Group(Trafos)` 距离 < 4.3 触发：Unphysicalize → 动画 1.35 s → 换球实体 → Physicalize 新球型。 | `Trafo Manager` 子图 |
| 分节：`Gameplay_SectorManager` 被 Execute Script 同步执行：读 `IngameParameter[0,2]`（Deactivate Sector）与 `[0,1]`（Activate Sector），先按 PH 表逐行重置 Sector==Deactivate 的对象（Type 1：Activate Script(reset)，Type 2：Unphysicalize+Hide），再激活 Sector==Activate 的对象（Type 1：Show+Set World Matrix+Activate Script；Type 2：Physicalize 箱子；Type 3：Physicalize 球型对象）；`CurrentLevel[0,4]`（Activation Phase?）在激活阶段为 TRUE。到达检查点由 `Gameplay_Events/activate Sektor` 触发同一脚本。 | `Gameplay_SectorManager`、`Gameplay_Events` 图 |
| 所有机关/检查点/加分/唤醒的触发都以 **`Ball_Pos_Frame`** 为参照（`TT Scaleable Proximity`，ObjectA=Ball_Pos_Frame）。`Ball_Pos_Frame` 是当前球的**刚性子节点**（`Set Init-Positions` → `Set Parent(Ball_Wood)`，变球时 `set new Ball` 重新挂接），并非由 `TT Set Dynamic Position` 驱动——那个块的 target 是相机架，`Object=Ball_Pos_Frame` 是它跟随的对象。`TT Scaleable Proximity` 的帧延迟由距离线性插值得到，不用 `rand()`。 | `gameplay_ingame.txt:259`、`SetDynamicPosition.cpp`、`ScaleableProximity.cpp` |
| 碰撞组语义：`IVP_Collision_Filter_Coll_Group_Ident` 让 nocoll 组名相同的物体互不碰撞（球组 `Ball` 与关卡里同组的 `P_Modul_01_Rinne` 等互不碰撞）；`IVP_Meta_Collision_Filter` 对所有子过滤器取与，可在运行时 `add_collision_filter`。 | `ivp_collision_filter.hxx/.cxx` |
| BMLPlus 提供 `OnPhysicalize(target, 全部配方参数)` / `OnUnphysicalize(target)`、`OnBallOff`、`OnPostCheckpointReached`、`OnLevelFinish`、`OnPreResetLevel` 等钩子。 | BMLPlus 0.3.12 `IMod.h` |
| IVP 的 nocoll 组名最多 7 个字符（`IVP_NO_COLL_GROUP_STRING_LEN` = 8，超长直接 `CORE` 崩溃）；`IVP_Environment::must_perform_movement_check()` 对每个被模拟的 core、每个 PSI 递减一次全局计数器并在归零时消耗 `ivp_rand()`，因此**冻结**（`disable_simulation`）的球在其 sim unit 完成 calm 检查前仍被计数，会让服务端与客户端的随机游标错位；只有删除刚体才不影响。 | `ivp_real_object.hxx`、`ivp_environment.hxx`、离线回放实测 |
| `SetPhysicsForce`/`Physics WakeUp` 等物理回调都通过 `PhysicsCallbackContainer::Process(cb)`：`Execute()` 返回 1 立即完成，返回 0 则排入队列，在每次 `Simulate()` 的 PreSimulate 阶段（脚本之后、PSI 之前）逆序重试。这个阶段是"脚本已跑完、物理未开始"的唯一钩子。 | `PhysicsCallback.cpp`、`CKIpionManager::Simulate` |

### 8.2 总体模型

- **服务端**：每个物理房间一个 `headless_engine`（独立 CKContext），由唯一的模拟线程顺序推进。世界通过菜单加载关卡，等到 `Gameplay_Ingame` 首次激活（锚点）后执行会话重置（第 3.1 节），记录锚点世界哈希。原版球（`CurrentLevel[0,1]`）**停放** = 在原版脚本 Physicalize 它的那个 tick 的 PreSimulate 阶段把它的刚体**删除**（`delete_silently`，见 8.1 关于 calm 检查计数的事实；冻结不够），实体留在出生点，脚本继续运行但不再影响世界（空键盘、DepthTest 不命中、Trafo 距离不满足）。每个玩家一个**克隆球**：复制原版球实体（`CKContext::CopyObject`，只复制名字，共享网格、不带脚本，名字 `Ball_<type>_BMMO_<id>`），按客户端上报的配方 Physicalize，nocoll 组名 `P#<槽位>`（IVP 限 7 字符）；再加一个 BMMO 碰撞过滤器：玩家球与组名为 `Ball` 的非玩家物体不碰撞（保持原版语义），玩家球之间碰撞。客户端在自己的球 Physicalize 后把它的组名也改为 `P#<join_order>`，否则本地球（原版 `Ball` 组）永远碰不到远端镜像球。
- **机关触发的并集**：原版所有 `TT Scaleable Proximity` 的 ObjectA 都是 `Ball_Pos_Frame`（原版球的子节点，停放后静止）。服务端在锚点扫描全部此类块，把每个块的 ObjectA 参数改接到一个私有 frame（`BMMO_Prox_<k>`），每 tick 在脚本执行前把该 frame 放到离该块 ObjectB（机关本体）最近的玩家球位置——原版的"任一球进入范围"语义由此成立，PE_Balloon 之类靠邻近创建力控制器的机关在服务端会真的启动。Level 1 有 18 个这样的块。
- **客户端**：本地球仍是原版球、由原版脚本驱动（预测）；远端球是精灵球实体按对方配方 Physicalize 的镜像刚体（组名 `BMMO_<id>`，同一过滤器）；共享机关的可动刚体每 tick 镜像服务端状态。
- **权威划分**：物理（位姿/速度/碰撞）服务端权威；球的生命周期（Physicalize/Unphysicalize、变球、复活位姿）、分节、完成由客户端原版脚本决定并以可靠事件上报，服务端照做并转发给其他成员。这是 M3 的取舍：不在服务端重写检查点/死亡/变球逻辑（它们全部依赖单一 `Ball_Pos_Frame`），M4 再评估是否要服务端校验。
- **时间线**：客户端锚点 = 会话开始后重开关卡并首次看到 `Gameplay_Ingame` 激活的那个 tick，记为本地 tick `first_tick`（首次开始为 0）。此后每个行为帧一个 tick（固定 1/66 s，`fixed_tick_driver` 节拍）。服务端在所有成员 `SessionReady` 后开始推进；tick T 在收齐所有成员 T 的输入、或服务端墙钟到达 `tick0_wall + (T + input_delay)/66 s` 时模拟，缺失输入沿用该玩家上一 tick 的输入。各客户端锚点的墙钟时刻可能相差 1–3 s（重开耗时），只影响修正延迟，不影响正确性。

### 8.3 服务端

目录 `BallanceMMOServer/sim/`（新增）：

- `physics_world.hpp/.cpp`：`physics_world`，包装一个 `headless_engine`：`boot(level)`（加载 base.cmo → 菜单进关 → 等锚点 → 停放原版球 → 会话重置 → 记录锚点哈希/表面签名）、`add_player / remove_player`、`apply_input(player, tick, input)`、`apply_event(player, event)`、`tick()`、`snapshot(full)`。
- `player_navigation.hpp/.cpp`（实际文件名）：每玩家一个相机参照实体（`CamRef_BMMO_<id>`，每 tick 用输入中的三条基向量写世界矩阵的旋转部分）+ 导航状态机。导航按 8.1 语义：四个叶子各自维护 Key Event 的电平状态；本 tick 键按下沿 → 创建与 `PhysicsControllerForce` 逐行等价的控制器（`TransformVector` → `IVP_U_Point.normize()` → `mult(force)`）并 `ensure_in_simulation()`；抬起沿 → 删除控制器并 `ensure_in_simulation()`；`nav_active` 由假变真时重置电平（Key Event.On 语义），由真变假时全部 Shutdown。整个过程在物理管理器的 PreSimulate 阶段执行（`physics_world::pre_simulate`，通过一个自定义 `PhysicsCallback`），即本 tick 的脚本之后、PSI 之前。叶子按其 **Key Event 在图中的子块序号**排序（这是引擎执行 Key Event 的顺序，也就是多键同按时控制器加入 core 的顺序），方向从图读取，力值取 `Physicalize_GameBall` 行；键码在 `Gameplay_Refresh` 跑过后才可读（锚点后第 3 tick），两端都要延迟读取。
- `sector_union`：服务端不跑原版检查点逻辑；收到玩家分节上报后，对每个尚未激活的分节 S 执行“只激活”：`IngameParameter[0,2]=0`、`[0,1]=S`，`scene->Activate(Gameplay_SectorManager, reset)`（下一 tick 执行）。已激活分节永不反激活（并集，只增不减）。
- `mechanism_wake`：客户端上报 `BodyRevived{tick, name}`（其原版脚本对机关做了 `Physics WakeUp`），服务端对同名刚体 `ensure_in_simulation()`。
- `session_runner.hpp/.cpp`：模拟线程。命令队列（创建/销毁世界、加入/离开、输入、事件、重同步请求）；每个世界一个 tick 调度器（8.2 的规则），每 tick 后调用 `on_snapshot` 回调；快照每 `snapshot_interval` tick 一次（不可靠），每 66 tick 或刚体集合变化时发 full（可靠，含机关名字典）。**生命周期屏障**：某玩家 tick T 的输入标记 physicalized 而世界里还没有它的球、也没收到 tick ≤ T 的 Physicalize 事件时，最多等 1 s 让可靠事件追上不可靠输入，再继续。多房间时在世界间切换前后保存/恢复 `ivp_srand` 游标；M3 配置上限 `maximum_physics_rooms: 1`，多房间留 M4 验证。
- `server.cpp` 集成：`Start(mode=Physics)` 校验（`physics.enabled`、`game_root`、全员同一原版关卡、Mod 白名单——`ModList` 登录时已收到，存入 `client_data.mods`）→ 房间进入 `Starting` → 模拟线程 boot 世界 → 就绪后发 `SessionStart` → 收 `SessionReady`：比对锚点哈希与表面签名，不一致则 `SessionEnd(reason)`；全员就绪 → 开始推进 → `SessionInput/SessionEvent` 转入队列 → 快照回调直接用 `interface_->SendMessageToConnection`（GNS 线程安全）发给房间成员 → 离开/断线 → `remove_player` 并广播 `PlayerLeft`；房主 `Close`/`End` 或全员离开 → 销毁世界 → `SessionEnd`。迟到加入：房间 Running 时 `Join` 允许，服务端发 `SessionStart`，客户端重开关卡并锚点后发 `SessionReady`，服务端以 `SessionAssign{first_tick = 收到 Ready 时的当前 tick}` 回复（在场成员为 0；重开耗时 1–3 s，因此不能在 SessionStart 里预先给号），客户端此后按 `first_tick + 锚点后帧数` 编号并补发缓存的输入；服务端再补发所有在场玩家最后一次 Physicalize 事件与 full 快照；不做哈希校验。重开：房主再次 `Start` 相当于 `End` + `Start`。
- 工作目录：`headless_engine::create` 会把进程 CWD 切到 `<game_root>/Bin`；`config_manager` 改为在首次加载时记录启动目录并用绝对路径读写 `config.yml`、登录记录与玩家状态文件。

### 8.4 物理桥接（`physics_rt_api.h` v2，`physics_state.cpp` 共享实现）

追加（保持 v1 表前缀不变，`api_version` 升到 2）：

- `list_bodies(out[], max)`：每个已 Physicalize 的实体：名字、是否可动（core 非 unmovable）、movement_state、位姿/速度。
- `get_body_state(entity_name, out)` / `set_body_state(entity_name, pose, linear, angular, wake)`：`beam_object_to_new_position` + 直接写 core 的 `speed/rot_speed`（并清零 `speed_change/rot_speed_change`），`wake` 时 `ensure_in_simulation()`，否则若目标为休眠则 `disable_simulation()`。
- `physicalize_ball(entity_name, recipe, group)` / `unphysicalize(entity_name)`：按 8.1 配方直接调用 `CKIpionManager::CreatePhysicsObjectOnParameters`（不经 CK 行为块）；`recipe` 为 `OnPhysicalize` 的全部参数（fixed/friction/elasticity/mass/startFrozen/enableColl/calcMassCenter/linear/rot damp/collSurface/massCenter/convex 网格名/ball 中心与半径）。
- `install_player_filter(prefix)`：向环境的 `IVP_Meta_Collision_Filter` 追加 8.2 的 BMMO 过滤器（幂等；环境重建后需重装，客户端在每次 `OnPhysicalize` 自己球时检查）。
- `world_hash` 不变；`capture_world_hash` 仍以可动 core 集合为准，因此停放的原版球不计入。

### 8.5 客户端（`BallanceMMOClient/session/physics_session_client.hpp/.cpp`）

状态机 `Idle → Restarting → Anchoring → Running → Ended`：

1. `SessionStart`（网络线程）→ 游戏线程：记下参数，**先启用固定节拍驱动器**（否则锚点帧——`Gameplay_Ingame` 的第一帧、原版开场计时器起算的那一帧——的行为 delta 是重开关卡实际耗时（实测 4.5 ms 到上百 ms），两端开场结束、物理恢复的 tick 会差一帧），再调用 `restart_current_level()`；进入 Restarting。
2. `OnProcess`：先看到 `Gameplay_Ingame` 失活、再看到它激活（边沿检测：收到 SessionStart 时它本来就是激活的）→ 锚点：`fixed_tick_` 重新启用（重设节拍原点；驱动器落后超过 33 tick 时也自动重设而不是快进），`physics_view_.reset_session_clock(seed)`、捕获世界哈希、安装玩家碰撞过滤器 → 发 `SessionReady`；进入 Running。锚点帧为第 0 帧，第 f 帧代表 tick `tick_base + f − 1`，`tick_base` 由 `SessionAssign` 给出，收到前输入只缓存。
3. 每 tick（OnProcess 起始）：从输入钩子的 `frame_keys_` 取四个导航键（键码从 `Ball Navigation` 图的 `Key Event` 参数读取，映射到叶子编号），相机基向量用**上一帧末**记录的 `Cam_OrientRef` 矩阵（见 8.1），`ball_type`、`physicalized`/`paused`/`nav_active` 标志，写入环形历史并发 `SessionInput`（携带最近 ≤8 tick，UnreliableNoDelay）；把自己球的 core 状态（bridge `get_body_state`）存入历史。球一旦有刚体就把它的 nocoll 组改成 `P#<join_order>`；球没有刚体而位置正好等于 `CurrentLevel[0,3]` 复活点时，把它挪到复活点 + 出生环偏移（首次出生与每次复活都适用，单人会话偏移为 0）。
4. 事件：`OnPhysicalize(target==自己球)` → `Physicalize{tick, type, recipe, pose}`；`OnUnphysicalize` → `Unphysicalize{tick}`；`OnPostCheckpointReached` → `Sector{tick, sector}`；`OnLevelFinish` → `Finish`；桥接事件日志里非球刚体的 revived → `BodyRevived{tick, name}`。事件走可靠通道。
5. 收到 `SessionSnapshot`（网络线程）→ 队列 → 游戏线程 tick 开头处理：远端球：按玩家找到镜像刚体 `set_body_state(…, wake=simulated)`；自己的球和机关（按字典名找到本地同名刚体，本地没有的跳过）走同一套修正（`body_corrector`）：每 tick 把本地刚体状态存入该刚体的历史，快照里 tick T 的状态只与历史中 T 的状态比较，**绝不与当前状态比**——服务端权威时间线落后客户端 `input_delay` 加网络延迟（本机实测约 8 tick），拿快照直接覆盖当前刚体会把运动中的机关每次倒回 8 tick（M3 联调时机关正是这样被反复倒带、冻结时机错开、RNG 随之分叉，球的静止位置也偏了几毫米）。位置误差 < ε₁（0.01）且速度误差 < ε₂（0.05）忽略；< ε₃（1.0）则把差值按 K=8 tick 逐步加到刚体上（每 tick 位置 +Δp/K、速度 +Δv/K，通过 `set_body_state` 写回），渐变进行中的新快照跳过不比（否则同一误差会被计两次）；更大误差直接硬置到快照状态并清空历史。每次 blend/hard 记一行日志，计数进 `/mmo room session`（自动化命令 `session`）。
6. 远端玩家的 `Physicalize/Unphysicalize` 事件（服务端转发）→ 创建/销毁镜像刚体（`game_objects` 的精灵球实体，`physicalize_ball` 配方与对方一致，组名 `BMMO_<id>`）。远端球的显示位置由镜像刚体决定（`PlayerObjects.physicalized = true` 时跳过旧的外推）。
7. `SessionEnd` 或离房 → 销毁镜像刚体、停止发送、`fixed_tick_.disable`；本地球不动。
8. 会话中 ESC 暂停：本地物理停摆，服务端不停；恢复后由修正拉回。M3 记为已知限制。
9. 死亡：客户端原版脚本在 `Deactivate Ball` 后重置当前分节（机关回到初始位姿并重新 Physicalize，桥接事件日志里出现 revived → 客户端上报 `BodyRevived`），服务端只唤醒同名刚体、不重置（个人死亡不重置共享机关，见第 2 节）。客户端下一次快照就会把这些机关硬置回服务端状态（误差 1.5 m 左右，Level 1 的纸球/木箱实测），肉眼可见一次跳变；球本身不受影响（引擎改动 #5 之后球的历史不再依赖机关的清醒状态）。M3 已知限制，M4 考虑在客户端拦截原版分节重置。

### 8.6 验证计划

1. **单元**：新消息序列化往返/截断（gtest，`tests/session_messages_test.cpp`）；`session_timeline`（纯逻辑：输入缓冲、缺失沿用、快照节奏）；`navigation_graph`（从图读取键→叶子映射的解析）。
2. **离线导航复现**（最关键）：`BallanceMMOSimTool --replay <bmrc> --nav clone`：原版键盘照常喂给空输入管理器（教程等脚本行为不变），原版球在 Physicalize 的那个 tick 被删除刚体并由克隆球（原版配方、原版位姿、`P#0` 组）取代，克隆球由 C++ 导航从录制键驱动，`nav_active` 取原版 Key Event 的激活状态；为了覆盖死亡/复活，原版球实体每 tick 镜像克隆球位姿（`mirror_clone_to_retail`），原版脚本 Hide 它时克隆球去刚体、原版球再次 Physicalize 时克隆球重建。**结果（2026-09-02）**：2345 帧全部位级一致（哈希与 pose），含第一次按键、下落死亡、复活与第二次操作。`--nav retail-cxx`（原版球本身由 C++ 导航驱动、原版叶子力值清零）是诊断模式，目前在第一次按键帧出现方向分量偏差，尚未查明，不作为验收依据。
3. **无头会话客户端** `BallanceMMOSessionClient`（`BallanceMMOServer/sim/session_client.cpp`，服务端构建的一部分，跨平台，单线程：引擎帧之间轮询网络）：无头引擎 + GNS 客户端，走真实房间/会话协议（登录 → 上报关卡 → 建房/加入第一个大厅房 → 准备 →（房主模式下人齐即开）），`SessionStart` 后经菜单加载关卡、在 `Gameplay_Ingame` 首次激活处锚点并发 `SessionReady`，键来自 bmrc（按锚点后帧号注入原版键盘缓冲，同时算出 `SessionInput` 的键位掩码），Physicalize/Unphysicalize/BodyRevived 从桥接事件日志推导（Physicalize 位姿取复活点矩阵 + 出生环偏移，创建后一 tick 的刚体位置离它超过 5 cm 才退回实体矩阵并告警），分节轮询 `IngameParameter[0,1]`，远端球用 `CopyObject` 的克隆按配方 Physicalize 并逐快照 `set_body_state`，自己球与机关走同一 `body_corrector`。用法：`BallanceMMOSessionClient --root <game> --server ip:port --join-first --record x.bmrc [--trace] [--no-correct] [--seconds S]`，每 5 s 打印一行 `status:`，退出码 0 表示自己球从未被修正。
4. **原版客户端端到端**：原版客户端做房主，无头会话客户端加入，`/mmo session status` 观察修正统计；球碰撞、机关镜像目视验证。

**联调结果（2026-09-02，原版客户端 × 无头服务端，单人，Level 1）**：锚点后两端逐 tick 世界哈希（pose）与 IVP 状态位级一致：开场、机关物理化与暂停/恢复、球下落、按键驾驶、两次掉落死亡与复活，全程自己球的修正统计 `compared=658 ignored=658 blended=0 hard=0 max_err=0.0000`；`rng t=` 变化日志（seed/mc/清醒刚体集合）两端完全相同。之前三个各造成毫米到厘米级偏差的原因都已定位并修掉：(a) 机关快照直接覆盖当前刚体（服务端落后 ~8 tick，运动中的机关每次被倒回，冻结时机错开）→ 改为历史比较 + 渐变（8.5 第 5 条）；(b) 锚点物理时间因子 0.001/0.002 不一致（客户端重开关卡前脚本已设 2.0）→ `reset_session_clock` 统一为 1.0；锚点帧行为 delta 不固定（重开耗时 4.5 ms..上百 ms）→ `SessionStart` 时即启用固定节拍；(c) 死亡后机关清醒集合不同使全局休眠倒计数/RNG 分叉 → 引擎改动 #5。另外 `build-retail` 曾未开 `BMMO_PHYSICS_PORTABLE_MATH`（服务端用 UCRT sin/cos，四元数差 2 ulp），该选项现在默认开启。逐 tick 比对两端 `exact t=` 转储用 `scripts/compare_exact.py <server.log> <client.log> [max] [own player id]`；联调脚本 `scripts/run_server.py`（stdin 命令文件）、`scripts/launch_client.ps1`、`scripts/client_ctl.py`（文件命令通道）；客户端崩溃转储用 `scripts/minidump_info.py <dmp> <map...>` 配合 `/MAP` 链接符号化。离线复现（第 2 条）在改动 #5 之后重录重放：4169/4169 帧一致（录制时须 `fixedtick on`）。

**双人联调结果（2026-09-02，原版客户端做房主 + 无头会话客户端加入，Level 1）**：房间/会话协议全程走通（两端锚点 pose 哈希一致、服务端 2 人同 tick 0 开跑、事件互相转发、双方都镜像出对方的球）；两端各自的球在 Physicalize 后 12 tick 的精确转储与服务端逐位相同；约 66 tick 后两球沿出生台的碟形斜面滚向中心并**互相顶住**（半径 6 的出生环放在 Level 1 的起点碟里会汇聚），从这一刻起两端各出现 4–11 cm 的持续误差并由修正器拉回——因为客户端里对方的球只是按快照回写的镜像（落后 8 到 38 tick），球-球接触的结果必然与服务端不同。这是设计 3.3 第一阶段镜像的预期行为，不是确定性缺陷；无接触时双人仍位级一致。无头客户端锚点早于原版客户端约 0.5 s，因而领先服务端 ~38 tick，大滞后下的渐变修正会互相叠加（误差 0.1 → 0.8 m 再收敛），第二阶段（远端球本地预测）解决。

### 8.7 M3 明确不做

- 服务端校验客户端上报的生命周期/分节事件（M4）。
- 多个物理房间同时运行（配置上限 1，M4 验证全局状态隔离）。
- 分节反激活；远端球的本地预测（设计 3.3 第二阶段）；暂停语义；积分/生命同步。
- 客户端死亡时本地分节重置带来的机关跳变（8.5 第 9 条）。
- 球-球接触时的一致性：远端球是快照镜像，两球顶住/相撞后各端都要靠修正（8.6 双人结果）；远端球本地预测（3.3 第二阶段）与出生环/起点碟的几何问题留给 M4。

## 9. M4 实施设计

M3 留下的清单（8.7）按"对玩家可感知的收益 / 风险"排序，M4 依次做：

1. **远端球本地预测**（3.3 第二阶段）——双人联调里唯一的非一致来源是球-球接触时对方的球只是滞后镜像；做完之后接触也走本地物理。
2. **重同步与暂停语义**——客户端暂停、长时间掉帧、硬修正反复出现时能回到一致状态，而不是永远靠渐变追。
3. **路径验证**：迟到加入、断线、host 迁移、房主重开，用无头会话客户端做成可重复的脚本。
4. **服务端校验客户端事件**：Physicalize 位姿/配方、分节单调、事件频率。
5. **清理与打包**：诊断模式收口、原版球停放后的控制台刷屏、安装目标、配置与部署说明。
6. 视时间：死亡时本地分节重置的机关跳变（已在 9.6 随引擎改动 #6 解决）；多房间（全局状态隔离）；积分/生命同步。

### 9.1 远端球本地预测

**现状**：客户端里对方的球是"镜像刚体"，每个快照 `set_body_state` 直接写服务端状态；快照落后客户端 8 到 38 tick，所以自己球与对方球接触时本地算出来的结果必然与服务端不同，随后靠修正拉回（8.6 双人结果）。

**目标**：客户端对每个远端球运行与服务端**完全相同**的导航复制（同一段代码、同一顺序的 IVP 调用），输入来自服务端转发的"该 tick 实际采用的输入"；快照只用来校正，不再逐快照覆盖。对方按住方向键匀速滚动时远端球位级一致，接触结果一致；对方按键沿变化时，本地在收到转发前只能沿用上一帧输入，误差由 `body_corrector` 按球的阶梯（忽略 <1 cm、8 tick 渐变、≥1 m 硬置）拉回。

**共享导航模块**：把 `BallanceMMOServer/sim/player_navigation` 移到 `BallanceMMOCommon/src/physics/ball_navigation.cpp`（头文件 `include/physics/ball_navigation.hpp`），像 `physics_state.cpp` 一样同时编进客户端的 physics_RT.dll 与服务端的 BallanceMMOSim，服务端 `physics_world` 直接改用它（离线 `--nav clone` 回放必须仍然 4169/4169，作为搬迁没有改变任何一次 IVP 调用的证据）。方向参照仍是一个 CK3dEntity（`CamRef_BMMO_<id>`，每 tick 由三条基向量写旋转部分，再 `TransformVector`），两端走同一条 CK 路径，不自己实现矩阵乘法。

**桥接 API v3**（`physics_rt_api.h`，追加在 v2 表末尾，版本号 3）：
- `nav_create(manager, ball_entity, direction_ref_entity, leaf_directions[4][3], leaf_count, force_value)`：为一个远端球建立导航状态机（按球实体名索引）；`direction_ref_entity` 不存在时由桥接创建。
- `nav_input(manager, ball_entity, keys, cam_right, cam_up, cam_dir, active)`：登记下一次物理步之前要施加的输入；桥接把它排进 `CKIpionManager::m_PreSimulateCallbacks`（与服务端 `tick_callback` 相同的时机：在该 tick 的 `simulate_dtime` 之前、原版脚本之后），回调里先写相机参照矩阵再 `apply(keys, active)`。
- `nav_set_ball(manager, ball_entity, new_entity)`（变球）、`nav_destroy(manager, ball_entity)`。

**输入转发**：新增 opcode `SessionRemoteInput`（追加在 `SessionAssign` 之后）与消息 `session_remote_input_msg{session, tick, entries[]: {player, input_frame}}`。服务端每模拟一个 tick，把这一 tick 各玩家**实际采用**的输入帧（新鲜的或沿用的）打包成一条消息发给每个成员（去掉成员自己的那项），不可靠、不延迟。带宽为每 tick 每成员 (N−1)×~50 B。客户端为每个远端玩家保存最近的输入帧；模拟 tick T 时若已有 T 的转发就用它，否则沿用最近一帧（预测）；转发晚到不回放。

**客户端**（`physics_session_client.cpp`）：远端 `Physicalize` 事件仍按配方物理化镜像实体，随后 `nav_create`（叶子方向来自本地 `navigation_graph`，力值来自 `Physicalize_GameBall` 行，球型来自事件）；每帧末为每个远端球 `nav_input`（下一 tick 的预测输入）；每帧记录远端球状态到各自的 `body_corrector` 历史；快照里对方的球改为 `compare`，只在阶梯判定时写刚体。`Unphysicalize` → `nav_destroy` + 去刚体。诊断：`/mmo room session` 增加 `remote_compared/ignored/blended/hard`。

**验收**：双人（原版 + 无头，或两个无头）在起点碟里顶住之后，双方球的修正统计保持 `blended=0 hard=0`（对方按键沿变化的 tick 附近允许 ≤1 cm 的忽略级误差）；单人回归不变。

**开始对齐**：`SessionAssign` 对会话开始时在场的成员改为**全员准备好后一起发**（迟到者仍即时发）；无头会话客户端在收到分配前不推进帧。原因：锚点早的客户端会在等待别人期间领先服务端几十 tick，转发输入对它就滞后同样多（联调里无头客户端领先 38 tick，远端预测几乎失效）。原版客户端仍按锚点起跑（BML 的 OnProcess 不能停帧），两台原版客户端同时重开关卡时锚点相差不到一秒。

**结果（2026-09-02，原版房主 + 无头客户端，Level 1，两球在起点碟里顶住并互相驱动）**：共享导航搬迁后离线 `--nav clone` 回放仍 4169/4169；原版客户端自己球 `compared=1594 ignored=1216 blended=85 hard=0 max_err=0.045`，对无头球的预测 `1599/1112/105/0`；无头客户端自己球 `1551/1350/46/0 max_err=0.023`，对原版球的预测 `1546/1392/36/0`。改动前同一场景是每 4-11 cm 持续误差、无头侧最大 24 m。剩下的 1-2 cm 渐变集中在对方按键沿之后的约 10 tick（转发滞后 = input_delay + 网络）以及远端球刚 Physicalize 的头几个快照（事件比快照晚 2-3 tick 到达，镜像从事件位姿起步）。

### 9.2 重同步与暂停

- **暂停**（ESC、失焦）：客户端本地物理停摆，服务端不停。恢复后固定节拍驱动器最多快进 33 tick，超过则重设节拍原点——此时客户端的 tick 编号与服务端脱节。M4 规定：驱动器重设原点时客户端向服务端发 `SessionResync{session, last_full_tick}`，服务端回 `SessionAssign{first_tick = 当前 tick}` 并强制一次全量快照；客户端收到后把 `tick_base` 改为新值、`frames_since_anchor` 归零重排（后续帧号从新基数计）、清空所有修正历史，并在下一次全量快照时对所有刚体（自己的球、远端球、机关）硬置一次。输入历史里旧编号的帧丢弃。
- **反复硬置**：自己球连续 3 次快照落入硬置档，或 `unmatched` 连续超过 30 个快照（历史里找不到快照 tick，即编号已错位），同样触发 `SessionResync`。
- 服务端对 `SessionResync` 的处理与迟到加入相同（`late` 集合 + 当前 tick 编号 + 全量快照），因此迟到加入路径与重同步路径共用一套代码和测试。
- **编号要领先**：迟到加入与重同步分配的不是服务端"当前 tick"，而是 `当前 tick + input_delay + 2`。客户端从收到分配那一刻起按这个编号推进，才能像会话开始时的成员一样领先服务端；按当前 tick 分配时客户端反而落后半个 RTT，每个快照到达时本地还没有那个 tick 的历史，全部 `unmatched`（首次联调正是如此，1380/1380）。
- 客户端触发重同步后继续按旧编号推进（原版客户端停不了帧），旧编号的输入被服务端的输入缓冲丢弃；收到新的 `SessionAssign` 才归零重排。

**结果（2026-09-02，无头客户端 `--pause-at 4200 --pause-ms 5000`）**：暂停后节拍原点重设 → `SessionResync` → 服务端 `resynced at tick 4532` → 客户端 `resynced: tick base 4532` → 全量快照 10 个刚体一次写入；之后自己球 `compared=1679 ignored=705 blended=185 hard=0 max_err=0.14`（另一方在它暂停期间一直在推它，误差来自接触）。

### 9.3 路径验证（无头会话客户端脚本）

| 场景 | 步骤 | 通过标准 |
|---|---|---|
| 迟到加入 | 会话跑到 ≥1000 tick 后无头客户端加入 | 收到 `SessionAssign(first_tick=当前)` 与全量快照；锚点后 60 tick 内自己球修正 `hard≤1`、之后 `blended=0`；对方球出现在本地 |
| 断线 | 无头客户端进程被杀 | 服务端 `member_left`，克隆球去刚体，其余成员收到 `Unphysicalize`（服务端代发）；会话继续 |
| host 迁移 | 房主（原版客户端）离开 | 房间 host 变更，会话不中断，新 host 能 `/mmo room close` 结束会话 |
| 房主重开 | 房主再次 `start physics` | 旧会话 `SessionEnd("restarted by the host")`，新会话全员重锚 |
| 暂停恢复 | 原版客户端 ESC 5 s 后恢复 | 触发 9.2 的重同步，之后 `blended=0` |

服务端在成员离开时代发该玩家的 `Unphysicalize`（目前只在世界里去刚体，其他客户端的镜像会留下），这是 M3 遗漏的一条。已补：`physics_session_member_left` 用离开者的 id 向其余成员发 `SessionEvent{Unphysicalize}`。

**结果（2026-09-02，原版房主 + 无头客户端）**：
- 迟到加入：会话跑到 tick ~2300 时无头客户端加入房间（Running 状态的房间可加入），锚点后收到 `SessionAssign(first_tick=2329)`、房主球的 Physicalize 与全量快照；房主球先按镜像方式出现，导航图在锚点后第 3 tick 可读时自动升级为预测（`remote ... now predicted`）。
- 断线/离开：房主 `mmo room leave` 后无头客户端收到代发的 `Unphysicalize`（`remote player ... unphysicalized`），会话为剩下的成员继续；最后一人离开时 `everyone left`。
- host 迁移：由房间系统处理，会话不中断（同上）。
- 房主重开：无头客户端在 `SessionStart` 时若关卡仍加载着则重启引擎回到主菜单再加载（约 5 s），原版客户端走 `restart_current_level`。

### 9.4 服务端校验客户端事件

只做拒绝明显不合理的事件，不做物理层面的重演：Physicalize 位姿必须在复活点环 2 m 内或距该玩家上一快照位置 5 m 内（变球）；配方的球型必须在 `Physicalize_GameBall` 行内且数值与行一致；`Sector` 只能等于当前或 +1（并集里已有的直接忽略）；每玩家每秒事件数上限 20。被拒绝的事件记日志并向该客户端回 `SessionEvent`（type 不变，`player = 0`，`name = "rejected"`）——M4 先记日志不回包。

**实现（2026-09-02）**：`handle_session_event` 里，每玩家每秒第 21 条起丢弃并记一次日志；Physicalize 的球型 > 2 或配方数值出界（质量 (0,100]、摩擦/弹性 [0,10]、阻尼 [0,1]、至少一个球/凸包）直接拒绝；位姿检查用会话开始时发给各成员的出生环槽位（任一槽位 2.5 m 内）或该玩家最近快照位置 5 m 内，不满足只记 `suspicious event`（复活点随检查点移动而服务端只知道并集，不能据此拒绝）；Sector 只允许当前或 +1，其余记日志。计数在控制台 `sessions` 里显示。配置 `physics.require_physics_sha` 非空时 `SessionReady` 上报的 DLL sha 不匹配即结束会话（无头客户端豁免）。

### 9.5 清理与打包

- 原版球停放后每次 Physicalize 有 21 行 `[CK] You must Physicalize Ball_Wood ...`：在服务端的 CK 控制台接收器里过滤以 `You must Physicalize ` 开头且实体名为停放球的行。
- `--nav retail-cxx` 诊断模式与 `mirror_clone_to_retail` 保留但在 `--help` 里标注为诊断。
- 安装目标加入 `BallanceMMOSessionClient`；`docs/rooms-and-sessions-protocol.md` §3 增加服务端部署一节：需要完整游戏数据目录（`game_root`）、Windows/Linux 都可，一个物理房间约占一个核心；客户端侧需要 physics_RT.dll 与 Mod 同版本（`physics_sha256` 仅记录不校验，M4 增加 `physics.require_physics_sha` 配置：非空时只接受该 sha）。——已做：控制台过滤 `You must Physicalize Ball_*`、`install(TARGETS BallanceMMOSimTool BallanceMMOSessionClient)`、SimTool `--help` 标注诊断模式、协议文档 §3.1 部署、`require_physics_sha`。

### 9.6 客户端回滚重模拟（服务端权威的 prediction & reconciliation）

**目标**：客户端始终领先服务端已确认进度若干 tick（现在就是这样：领先 `input_delay` + 网络），对"服务端稍后才会确认的东西"——其他玩家的输入、物理世界的演化——做预测；服务端 tick T 的权威结果回来后，若与本地在 T 的预测不符，就**回到 T 的权威状态、用记录下来的输入重新模拟 T+1 到当前**，而不是把差值渐变地加到刚体上。这是 GGPO 的"预测 + 回滚"，但回滚只发生在客户端、权威在服务端（3.3 原来排除它的理由是 IVP 没有可靠的世界快照恢复；这里不做完整快照，而是用"逐体 beam + 只跑物理和导航的重模拟"逼近，差别见下）。

**为什么可行**：
- 两端的物理和导航复制是位级一致的同一段代码（M1、9.1），重模拟就是把服务端会做的事在本地再做一遍。
- 服务端每 2 tick 的快照已经带全部球和清醒机关的完整核心状态（f64 位姿、速度），恢复用 `set_body_state`（beam + 速度 + 清醒/冻结）。
- 只重跑物理与导航，不重跑 CK 脚本（脚本每帧只在真实帧里跑一次）：窗口只有约 10 tick，脚本驱动的东西（检查点、机关唤醒）在真实帧里照样发生。
- IVP 的 PSI 时刻由环境里的双精度时钟推进，每 tick 恰好 2 个 PSI 且有整整一个 PSI 的裕度，绝对时间平移 K 个 tick 不改变分组；回滚**不倒拨绝对时钟**，重模拟让本地 IVP 时间比服务端多走 K 个 tick（只影响 mindist 事件表里的 float 相对时刻，产生的罕见差异由下一个快照再次纠正）。

**与 GGPO 的差别**：没有完整存档，恢复后的接触/摩擦内部状态是 IVP 在下一个 PSI 重建的，所以重模拟结果与"从未分叉"的连续模拟不保证位级相同，但每 2 tick 的权威快照会再次拉齐；没有对等方，回滚只在客户端；不做输入延迟协商（服务端的 `input_delay` 固定）。

**需要的新东西**：
1. **自己的球改由导航复制驱动**（原版 `SetPhysicsForce` 叶子的 Force Value 每帧清零，原版脚本照常跑，只是推力为零）：复制以"轮询模式"工作——在该 tick 的 PreSimulate 里直接读输入管理器的键盘缓冲（与原版脚本在同一帧看到的一样）、Key Event 块的激活状态作为 nav_active、相机基向量用 Mod 传入的上一帧 `Cam_OrientRef` 三行（与发给服务端的一致，这也是 M3 `--nav retail-cxx` 诊断模式偏差的原因：它用了当帧的相机）。只有这样窗口内自己的按键沿才能在重模拟时按记录重放。
2. **每 tick 世界历史**（最近 64 tick）：每个被跟踪刚体（自己的球、远端球、机关字典里的机关）的核心状态 + 每个导航复制的内部状态（激活、各叶子键状态、各叶子是否有力控制器及其力向量——力向量在 Create 时按当时相机算出并固定，回滚必须恢复而不是重建）。
3. **桥接 API v4**：`step_physics(delta_ms)`（`CKIpionManager::Simulate`：PreSimulate 回调 → `simulate_dtime` → 接触管理 → PostSimulate → 实体矩阵刷新）、`navigation_poll`（轮询模式）、`navigation_get_state/set_state`。
4. **回滚引擎**（`BallanceMMOCommon/include/session/rollback.hpp`，纯逻辑 + 世界适配器接口，原版 Mod 与无头客户端共用）：收到 tick T 的快照 → 与历史 T 比较（位置 1 mm、速度 1 cm/s、清醒标志）→ 不符则：写入快照里的刚体、其余被跟踪刚体写回历史 T、恢复导航状态 T → 对 t = T+1..now：喂入 t 的输入（自己：记录的键/相机/激活；远端：转发中 t 的帧，没有则 ≤ t 的最近一帧）→ `step_physics` → 记录历史 t。相符则什么都不做。
5. 远端输入按 tick 保存（不只是最近一帧），窗口 64。

**验收**：单人（无远端）回滚永不触发（`rollbacks=0`，与现在一样位级一致）；双人在起点碟里顶住并互相驱动时，回滚只发生在对方按键沿之后，回滚后的下一个快照与本地一致（`rollbacks` 计数 ≈ 对方按键沿数，`mismatch after rollback` 接近 0）；原版客户端画面上不再有渐变，只有对方按键沿处一次小跳。

**实施（2026-09-02）**：

- `BallanceMMOCommon/include/session/rollback.hpp`：`rollback_engine`（`record` / `on_snapshot`）+ `rollback_world` 适配器（get/set body、get/set nav、nav_input、nav_poll、step、simulating、log）。Mod 在 `physics_session_frame` 末尾记录每 tick（自己的球、有导航复制的远端球、机关字典里的机关；应用的输入 = 自己的 `input_frame`、远端上次驱动喂入的帧），`physics_session_apply_snapshot` 在 `rollback_enabled`（默认开，自动化 `session rollback on|off` 可切回渐变路径）时走 `physics_session_rollback`；无头客户端同样（`--no-rollback` 切回）。回滚后重新排队下一帧的相机行（重模拟消耗了它）。
- 桥接 API v5：v4 之外加 `set_body_guard`（引擎改动 #6）和 `get_clock`（时间因子 / 下一步物理 delta）。
- 引擎改动 #6（`docs/engine-changes.md`）：会话期间原版 Unphysicalize 块只放行当前球，其它刚体保留；Physicalize 块对已有刚体把刚体位姿写回实体。原因：原版死亡分节重置会删掉并重建机关刚体，新刚体从初始位姿落下、接触状态全新，此后每个快照都不符（先 1.5 m，随后 1–10 mm 持续约 1 s）。Mod 每帧对当前球名启用守卫，会话结束关闭。
- **本地物理时钟停止时的路径**：原版脚本会把物理时间因子设为 0（Level 1 的 `Gameplay_Tutorial` 在关卡开始后停约 26 s；暂停菜单），这段时间本地既不能预测也不能重模拟。引擎通过 `world.simulating()`（`get_clock` 因子 > 0）识别，不符时只写入权威状态、不重模拟、计入 `frozen`。同步开始的会话两端一起进教程、快照一致；迟到加入者在自己的教程期间靠这条路径贴住服务端。
- 诊断：`session` 状态行里的 `rollback: snaps/ok/mism/rb/resim/unmatched/far/frozen/max_err/last`；前 40 次不符打印逐刚体本地/服务端位姿；`session trace on` 打开重模拟逐步轨迹；`BMMO_TRACE_TIMEFACTOR=1` 让 physics_RT 打印哪个脚本改了时间因子。
- 单元测试 `BallanceMMOServer/tests/rollback_engine_test.cpp`（假世界：匹配不回滚、不符恢复并按记录输入重模拟、时钟停止只贴齐、超出重模拟窗口只写入、历史有界、未命中计数）。

**联调结果（2026-09-02，原版客户端 × 无头服务端，Level 1）**：

| 场景 | 快照 | 一致 | 不符 / 回滚 | 重模拟 tick | 说明 |
|---|---|---|---|---|---|
| 单人，含 3 次死亡（守卫前） | 2040 | 2000 | 40 / 40 | 69 | 全部在死亡分节重置之后：先 1.54 m，随后 1–10 mm |
| 单人，含 3 次死亡（守卫后） | 2040 | 2040 | 0 / 0 | 0 | 位级一致，死亡不再产生任何修正 |
| 双人同步开始（原版房主 × 无头回放），房主侧 | 2510 | 2489 | 21 / 21 | 63 | 19 次在对方球（按键沿），2 次自己的球（1 mm 级残差） |
| 同上，无头侧 | 2510 | 2484 | 26 / 26 | 5 | 无头端与服务端几乎零滞后，恢复后无需重模拟 |
| 迟到加入（无头），教程期间 | 3135 | 3111 | 16 / 11 | 23 | `frozen=5`（教程里其他人没动就一致），教程结束后按键沿回滚，最大误差 5.48 m 是加入瞬间 |

- 教程期间的发现：迟到加入者本地 IVP 时间不走（因子 0），旧的"回滚"每个快照都恢复 + 重模拟却什么也模拟不了（核心的 `t_env` 不变），表现为远端球总停在服务端上一个快照的位置；`frozen` 路径解决。
- 自己的球的 1 mm 级残差出现在两球在出生点接触时：恢复只还原位姿/速度，IVP 接触状态在下一 PSI 重建，与"从未分叉"的连续模拟差 1 mm 左右，下一个快照拉齐（设计里预期的 GGPO 差别）。
- 原版渐变路径（`body_corrector`）保留为 `session rollback off` 的回退。
