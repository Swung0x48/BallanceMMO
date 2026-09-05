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
- 每玩家：克隆球实体与原版 Physicalize 配方、四个 `SetPhysicsForce` 叶子与 `Physics WakeUp`、独立相机参照、独立无碰撞组；出生点是原版 Resetpoint 本身，出生时额外施加一个确定性冲量把球们踢开（9.10）。
- 机关激活集合 = 所有玩家所在分节的并集；个人死亡不重置机关。
- 一个模拟线程顺序推进所有房间；每房间独立 CKContext。

### 3.5 房间与协议

- 新增 opcode 追加在旧枚举末尾；旧消息完全不变。
- 房间：`RoomCreate/Join/Leave/Ready/Start/Kick/Close/List`、`RoomState`（广播）、`RoomEvent`。
- 会话：`SessionStart{mode, map, tick0, snapshot_interval}`、`SessionInput`、`SessionSnapshot`（球全量 + 机关增量）、`SessionResync`、`SessionEnd`。
- 物理会话准入：地图与依赖文件哈希核对（服务端下发它实际加载的文件清单，客户端后台线程哈希本地文件；绝不在游戏线程创建第二个 CKContext）、Mod 白名单、physics_RT 哈希（若启用开源 DLL 模式）。

### 3.6 自动化命令通道

自动化命令有三条等价通道，命令集相同（`ping`/`status`/`physview`/`physobjs`/`level N`/`key`/`record`/`quit` 等），都在游戏线程派发：

- **命名管道**（`BMMO_COMMAND_PIPE`）：交互式使用，一问一答。
- **游戏内命令栏**：`/mmo auto <自动化命令>`（如 `/mmo auto record start D:/rec/a.bmrc 2`），走同一个 `dispatch_automation_command`，回答直接发到聊天/日志。自己手动跑一段再标注问题时不用再开第二个窗口；`OnFullCommand` 本来就保证在游戏线程上执行。
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
- **机关触发的并集**：原版所有 `TT Scaleable Proximity` 的 ObjectA 都是 `Ball_Pos_Frame`（原版球的子节点，停放后静止）。服务端在锚点扫描全部此类块，把每个块的 ObjectA 参数改接到一个私有 frame（`BMMO_Prox_<k>`），每 tick 在脚本执行前把该 frame 放到离该块 ObjectB（机关本体）最近的玩家球位置——原版的"任一球进入范围"语义由此成立，PE_Balloon 之类靠邻近创建力控制器的机关在服务端会真的启动。Level 1 有 18 个这样的块。机关脚本里按球型分支的**身份**判定（`CurrentLevel[0,ActiveBall]` 的名字）另有一套并集，见 9.8。
- **客户端**：本地球仍是原版球、由原版脚本驱动（预测）；远端球是精灵球实体按对方配方 Physicalize 的镜像刚体（组名 `BMMO_<id>`，同一过滤器）；共享机关的可动刚体每 tick 镜像服务端状态。
- **权威划分**：物理（位姿/速度/碰撞）服务端权威；球的生命周期（Physicalize/Unphysicalize、变球、复活位姿）、分节、完成由客户端原版脚本决定并以可靠事件上报，服务端照做并转发给其他成员。这是 M3 的取舍：不在服务端重写检查点/死亡/变球逻辑（它们全部依赖单一 `Ball_Pos_Frame`），M4 再评估是否要服务端校验。
- **时间线**：客户端锚点 = 会话开始后重开关卡并首次看到 `Gameplay_Ingame` 激活的那个 tick，记为本地 tick `first_tick`（首次开始为 0）。此后每个行为帧一个 tick（固定 1/66 s，`fixed_tick_driver` 节拍）。服务端在所有成员 `SessionReady` 后开始推进；tick T 在收齐所有成员 T 的输入、或服务端墙钟到达 `tick0_wall + (T + input_delay)/66 s` 时模拟，缺失输入沿用该玩家上一 tick 的输入。各客户端锚点的墙钟时刻可能相差 1–3 s（重开耗时），只影响修正延迟，不影响正确性。

### 8.3 服务端

目录 `BallanceMMOServer/sim/`（新增）：

- `physics_world.hpp/.cpp`：`physics_world`，包装一个 `headless_engine`：`boot(level)`（加载 base.cmo → 菜单进关 → 等锚点 → 停放原版球 → 会话重置 → 记录锚点哈希/表面签名）、`add_player / remove_player`、`apply_input(player, tick, input)`、`apply_event(player, event)`、`tick()`、`snapshot(full)`。
- `player_navigation.hpp/.cpp`（实际文件名）：每玩家一个相机参照实体（`CamRef_BMMO_<id>`，每 tick 用输入中的三条基向量写世界矩阵的旋转部分）+ 导航状态机。导航按 8.1 语义：四个叶子各自维护 Key Event 的电平状态；本 tick 键按下沿 → 创建与 `PhysicsControllerForce` 逐行等价的控制器（`TransformVector` → `IVP_U_Point.normize()` → `mult(force)`）并 `ensure_in_simulation()`；抬起沿 → 删除控制器并 `ensure_in_simulation()`；`nav_active` 由假变真时重置电平（Key Event.On 语义），由真变假时全部 Shutdown。整个过程在物理管理器的 PreSimulate 阶段执行（`physics_world::pre_simulate`，通过一个自定义 `PhysicsCallback`），即本 tick 的脚本之后、PSI 之前。叶子按其 **Key Event 在图中的子块序号**排序（这是引擎执行 Key Event 的顺序，也就是多键同按时控制器加入 core 的顺序），方向从图读取，力值取 `Physicalize_GameBall` 行；键码在 `Gameplay_Refresh` 跑过后才可读（锚点后第 3 tick），两端都要延迟读取。
- `update_sectors`（原 `sector_union`，2026-09-03 重写，见 9.9）：服务端不跑原版检查点逻辑，**正在运行的分节 = 所有玩家所在分节的集合**——有人到的分节启动，没人的分节反激活。每 tick 比较该集合与已激活集合，差异逐个交给原版 `Gameplay_SectorManager`（`IngameParameter[0,2]=反激活`、`[0,1]=激活`，`scene->Activate(..., reset)`），一次一个：管理器走完 PH 表要约 7 帧，中途重启会既打断遍历又在它刚启动的机关脚本读 `CurrentLevel[Activation Phase?]` 之前把该标志翻掉。
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
3. 每 tick（OnProcess 起始）：从输入钩子的 `frame_keys_` 取四个导航键（键码从 `Ball Navigation` 图的 `Key Event` 参数读取，映射到叶子编号），相机基向量用**上一帧末**记录的 `Cam_OrientRef` 矩阵（见 8.1），`ball_type`、`physicalized`/`paused`/`nav_active` 标志，写入环形历史并发 `SessionInput`（携带最近 ≤8 tick，UnreliableNoDelay）；把自己球的 core 状态（bridge `get_body_state`）存入历史。球一旦有刚体就把它的 nocoll 组改成 `P#<join_order>`；客户端不再移动球——`OnPhysicalize` 里若原始位姿等于 `CurrentLevel[0,3]` 复活点，上报的 `Physicalize` 事件带出生标志（`flags` bit0），随后把冲量排进这一帧 PreSimulate 阶段（设计 9.10；首次出生与每次复活都适用，单人会话冲量为 0）。
4. 事件：`OnPhysicalize(target==自己球)` → `Physicalize{tick, type, recipe, pose}`；`OnUnphysicalize` → `Unphysicalize{tick}`；`OnPostCheckpointReached` → `Sector{tick, sector}`；`OnLevelFinish` → `Finish`；桥接事件日志里非球刚体的 revived → `BodyRevived{tick, name}`。事件走可靠通道。
5. 收到 `SessionSnapshot`（网络线程）→ 队列 → 游戏线程 tick 开头处理：远端球：按玩家找到镜像刚体 `set_body_state(…, wake=simulated)`；自己的球和机关（按字典名找到本地同名刚体，本地没有的跳过）走同一套修正（`body_corrector`）：每 tick 把本地刚体状态存入该刚体的历史，快照里 tick T 的状态只与历史中 T 的状态比较，**绝不与当前状态比**——服务端权威时间线落后客户端 `input_delay` 加网络延迟（本机实测约 8 tick），拿快照直接覆盖当前刚体会把运动中的机关每次倒回 8 tick（M3 联调时机关正是这样被反复倒带、冻结时机错开、RNG 随之分叉，球的静止位置也偏了几毫米）。位置误差 < ε₁（0.01）且速度误差 < ε₂（0.05）忽略；< ε₃（1.0）则把差值按 K=8 tick 逐步加到刚体上（每 tick 位置 +Δp/K、速度 +Δv/K，通过 `set_body_state` 写回），渐变进行中的新快照跳过不比（否则同一误差会被计两次）；更大误差直接硬置到快照状态并清空历史。每次 blend/hard 记一行日志，计数进 `/mmo room session`（自动化命令 `session`）。
6. 远端玩家的 `Physicalize/Unphysicalize` 事件（服务端转发）→ 创建/销毁镜像刚体（`game_objects` 的精灵球实体，`physicalize_ball` 配方与对方一致，组名 `BMMO_<id>`）。远端球的显示位置由镜像刚体决定（`PlayerObjects.physicalized = true` 时跳过旧的外推）。
7. `SessionEnd` 或离房 → 销毁镜像刚体、停止发送、`fixed_tick_.disable`；本地球不动。
8. 会话中 ESC 暂停：本地物理停摆，服务端不停；恢复后由修正拉回。M3 记为已知限制。
9. 死亡：客户端原版脚本在 `Deactivate Ball` 后重置当前分节（机关回到初始位姿并重新 Physicalize，桥接事件日志里出现 revived → 客户端上报 `BodyRevived`），服务端只唤醒同名刚体、不重置（个人死亡不重置共享机关，见第 2 节）。客户端下一次快照就会把这些机关硬置回服务端状态（误差 1.5 m 左右，Level 1 的纸球/木箱实测），肉眼可见一次跳变；球本身不受影响（引擎改动 #5 之后球的历史不再依赖机关的清醒状态）。M3 已知限制，M4 考虑在客户端拦截原版分节重置。

### 8.6 验证计划

1. **单元**：新消息序列化往返/截断（gtest，`tests/session_messages_test.cpp`）；`session_timeline`（纯逻辑：输入缓冲、缺失沿用、快照节奏）；`navigation_graph`（从图读取键→叶子映射的解析）。
2. **离线导航复现**（最关键）：`BallanceMMOSimTool --replay <bmrc> --nav clone`：原版键盘照常喂给空输入管理器（教程等脚本行为不变），原版球在 Physicalize 的那个 tick 被删除刚体并由克隆球（原版配方、原版位姿、`P#0` 组）取代，克隆球由 C++ 导航从录制键驱动，`nav_active` 取原版 Key Event 的激活状态；为了覆盖死亡/复活，原版球实体每 tick 镜像克隆球位姿（`mirror_clone_to_retail`），原版脚本 Hide 它时克隆球去刚体、原版球再次 Physicalize 时克隆球重建。**结果（2026-09-02）**：2345 帧全部位级一致（哈希与 pose），含第一次按键、下落死亡、复活与第二次操作。`--nav retail-cxx`（原版球本身由 C++ 导航驱动、原版叶子力值清零）是诊断模式，目前在第一次按键帧出现方向分量偏差，尚未查明，不作为验收依据。
3. **无头会话客户端** `BallanceMMOSessionClient`（`BallanceMMOServer/sim/session_client.cpp`，服务端构建的一部分，跨平台，单线程：引擎帧之间轮询网络）：无头引擎 + GNS 客户端，走真实房间/会话协议（登录 → 上报关卡 → 建房/加入第一个大厅房 → 准备 →（房主模式下人齐即开）），`SessionStart` 后经菜单加载关卡、在 `Gameplay_Ingame` 首次激活处锚点并发 `SessionReady`，键来自 bmrc（按锚点后帧号注入原版键盘缓冲，同时算出 `SessionInput` 的键位掩码），Physicalize/Unphysicalize/BodyRevived 从桥接事件日志推导（Physicalize 位姿取复活点矩阵本身，不再加偏移，创建后一 tick 的刚体位置离它超过 5 cm 才退回实体矩阵并告警），分节轮询 `IngameParameter[0,1]`，远端球用 `CopyObject` 的克隆按配方 Physicalize 并逐快照 `set_body_state`，自己球与机关走同一 `body_corrector`。用法：`BallanceMMOSessionClient --root <game> --server ip:port --join-first --record x.bmrc [--trace] [--no-correct] [--seconds S]`，每 5 s 打印一行 `status:`，退出码 0 表示自己球从未被修正。
4. **原版客户端端到端**：原版客户端做房主，无头会话客户端加入，`/mmo session status` 观察修正统计；球碰撞、机关镜像目视验证。

**联调结果（2026-09-02，原版客户端 × 无头服务端，单人，Level 1）**：锚点后两端逐 tick 世界哈希（pose）与 IVP 状态位级一致：开场、机关物理化与暂停/恢复、球下落、按键驾驶、两次掉落死亡与复活，全程自己球的修正统计 `compared=658 ignored=658 blended=0 hard=0 max_err=0.0000`；`rng t=` 变化日志（seed/mc/清醒刚体集合）两端完全相同。之前三个各造成毫米到厘米级偏差的原因都已定位并修掉：(a) 机关快照直接覆盖当前刚体（服务端落后 ~8 tick，运动中的机关每次被倒回，冻结时机错开）→ 改为历史比较 + 渐变（8.5 第 5 条）；(b) 锚点物理时间因子 0.001/0.002 不一致（客户端重开关卡前脚本已设 2.0）→ `reset_session_clock` 统一为 1.0；锚点帧行为 delta 不固定（重开耗时 4.5 ms..上百 ms）→ `SessionStart` 时即启用固定节拍；(c) 死亡后机关清醒集合不同使全局休眠倒计数/RNG 分叉 → 引擎改动 #5。另外 `build-retail` 曾未开 `BMMO_PHYSICS_PORTABLE_MATH`（服务端用 UCRT sin/cos，四元数差 2 ulp），该选项现在默认开启。逐 tick 比对两端 `exact t=` 转储用 `scripts/compare_exact.py <server.log> <client.log> [max] [own player id]`；联调脚本 `scripts/run_server.py`（stdin 命令文件）、`scripts/launch_client.ps1`、`scripts/client_ctl.py`（文件命令通道）；客户端崩溃转储用 `scripts/minidump_info.py <dmp> <map...>` 配合 `/MAP` 链接符号化。离线复现（第 2 条）在改动 #5 之后重录重放：4169/4169 帧一致（录制时须 `fixedtick on`）。

**双人联调结果（2026-09-02，原版客户端做房主 + 无头会话客户端加入，Level 1）**：房间/会话协议全程走通（两端锚点 pose 哈希一致、服务端 2 人同 tick 0 开跑、事件互相转发、双方都镜像出对方的球）；两端各自的球在 Physicalize 后 12 tick 的精确转储与服务端逐位相同；约 66 tick 后两球沿出生台的碟形斜面滚向中心并**互相顶住**（半径 6 的出生环放在 Level 1 的起点碟里会汇聚），从这一刻起两端各出现 4–11 cm 的持续误差并由修正器拉回——因为客户端里对方的球只是按快照回写的镜像（落后 8 到 38 tick），球-球接触的结果必然与服务端不同。这是设计 3.3 第一阶段镜像的预期行为，不是确定性缺陷；无接触时双人仍位级一致。无头客户端锚点早于原版客户端约 0.5 s，因而领先服务端 ~38 tick，大滞后下的渐变修正会互相叠加（误差 0.1 → 0.8 m 再收敛），第二阶段（远端球本地预测）解决。（出生环已在 9.10 被出生冲量取代，此处的联调数据按当时的实现保留，不代表现状。）

### 8.7 M3 明确不做

- 服务端校验客户端上报的生命周期/分节事件（M4）。
- 多个物理房间同时运行（配置上限 1，M4 验证全局状态隔离）。
- ~~分节反激活~~（9.9 已做）；远端球的本地预测（设计 3.3 第二阶段）；暂停语义；积分/生命同步。
- 客户端死亡时本地分节重置带来的机关跳变（8.5 第 9 条）。
- 球-球接触时的一致性：远端球是快照镜像，两球顶住/相撞后各端都要靠修正（8.6 双人结果）；远端球本地预测（3.3 第二阶段）与出生环/起点碟的几何问题留给 M4（出生环后来被 9.10 的出生冲量取代）。

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

只做拒绝明显不合理的事件，不做物理层面的重演：Physicalize 位姿必须在复活点 2.5 m 内或距该玩家上一快照位置 5 m 内（变球）；配方的球型必须在 `Physicalize_GameBall` 行内且数值与行一致；`Sector` 只能等于当前或 +1（并集里已有的直接忽略）；每玩家每秒事件数上限由 `physics.event_rate_limit` 配置（默认 20，0 = 不限）。被拒绝的事件记日志并向该客户端回 `SessionEvent`（type 不变，`player = 0`，`name = "rejected"`）——M4 先记日志不回包。

**实现（2026-09-02）**：`handle_session_event` 里，每玩家每秒超出 `physics.event_rate_limit` 的事件丢弃并记一次日志（固定窗口，`reload` 后立即生效；置 0 关闭。一次分节重置会为该分节的每个机关各发一条 `BodyRevived`，机关多的关卡应调高或关闭）；位姿检查用会话开始时发给各成员的复活点（2.5 m 内）或该玩家最近快照位置（5 m 内），不满足只记 `suspicious event`（复活点随检查点移动而服务端只知道并集，不能据此拒绝）；Sector 只允许当前或 +1，其余记日志。计数在控制台 `sessions` 里显示。配置 `physics.require_physics_sha` 非空时 `SessionReady` 上报的 DLL sha 不匹配即结束会话（无头客户端豁免）。

**Physicalize 配方的校验以关卡的 `Physicalize_GameBall` 行为准，不用固定区间（2026-09-02 修）**：世界启动时把该表的每一行随 `world_ready_info` 交给网络线程（服务端日志里也逐行打印），事件到达时只拒绝**世界根本无法执行**的配方——球型不在表内、数值非有限、质量 ≤ 0；其余数值与该行不符的只记 `suspicious event`，**配方本身一个字节都不改**（会话的立身之本是两端执行客户端上报的同一次调用；改写会让一个诚实但被别的 Mod 改过物理的客户端与服务端各跑各的，永远互相修正）。几何（凸包/球/凹面）同样原样透传：原版确实存在凸包与球都为 0、只给 collision surface 的 Physicalize（实测出现在开了 BML cheat 后的复活；两端都走同一条 `CreatePhysicsObjectOnParameters`，默认半径 1.0，结果一致）。要真的拒绝数值，得先有"拒绝 → 通知客户端 → 客户端重报"的闭环。
> 原来的固定区间把阻尼卡在 [0,1]，而 `Ball_Paper` 的 Linear Damp 是 **1.5**（实测三行：Paper 0.5/0.4/0.2/1.5/0.1 force 0.065，Stone 0.5/0.1/10/0.3/0.1 force 0.92，Wood 0.8/0.2/1.9/0.9/0.1 force 0.43），于是**每一次变成纸球的 Physicalize 都被拒绝**：服务端此后没有该玩家的刚体，球不再参与模拟、不再推动机关，而原版脚本一辈子只上报这一次。木球/石球在区间内，所以只有变纸球会犯。
>
> 丢事件必须可恢复，所以还有两道保险：`session_runner::waiting_for_lifecycle` 的一秒栅栏改为**每玩家只等一次**（`lifecycle_missing`），否则一个永远等不到的 Physicalize 会把整个会话按住在 1 tick/s；客户端记住自己最后一次 Physicalize 的配方，连续 30 个快照里没有自己的球（快照对每个已物理化的玩家恒有一行，睡着也有）就带当前位姿重报一次（2 s 冷却，`session` 状态里的 `phys_resends`）。

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
- 引擎改动 #6（`docs/engine-changes.md`）：会话期间原版 Unphysicalize 块只放行当前球，其它刚体保留；Physicalize 块对已有刚体把刚体位姿写回实体。原因：原版死亡分节重置会删掉并重建机关刚体，新刚体从初始位姿落下、接触状态全新，此后每个快照都不符（先 1.5 m，随后 1–10 mm 持续约 1 s）。Mod 每帧对当前球名启用守卫，会话结束关闭。（守卫的豁免名单见 9.14 / 引擎改动 #13：变球碎片是会话期间原版脚本自建自毁的刚体，不归守卫管。）
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

**跨平台验证（2026-09-02，Arch WSL，GCC 16.1.1）**：回滚路径此前只在 Windows/MSVC 上验证过，这次把服务端和两个无头客户端全部放到 Linux 上跑了一遍。

- 单元测试 `BallanceMMOMessageTests` **75/75 通过**，含 6 个回滚引擎用例——回滚引擎第一次在 MSVC 以外的编译器上验证。注意 `enable_testing()` 在 `BallanceMMOServer/` 子目录里调用，在上层跑 `ctest` 会报 "No tests were found" 并且**退出码仍是 0**（原有 CI 的测试步骤因此一直空转），要 `ctest --test-dir <build>/BallanceMMOServer`。
- 端到端：服务端 + `--host --expect 2` + `--join-first` 三个进程全在 WSL 内，`physics.game_root` 指向 `/mnt/c/...` 下的 Windows 游戏数据目录，两个客户端回放同一份录制输入。

| 场景 | 快照 | 一致 | 不符 / 回滚 | max_err | frozen | unmatched |
|---|---|---|---|---|---|---|
| Linux 双人,HostA | 3832 | 3821 | 11 / 11 | 0.049 m | 0 | 0 |
| Linux 双人,JoinB | 3968 | 3961 | 7 / 7 | 0.049 m | 0 | 0 |

- 锚点位姿哈希 `cbf29ce484222325`，**与同日 Windows 侧的取值相同**——两端引擎在 Linux 上仍然位级一致，回滚只在对方按键沿附近触发，数量级与 Windows 结果一致。
- `game_root` 只被当作数据读取（不加载任何 Windows DLL），所以 Linux 服务端可以直接复用一份 Windows 游戏目录。

### 9.7 变球（trafo）

原版 `Trafo Manager`（`Gameplay_Ingame`）：`Get Nearest In Group(Trafos)` 距离 < 4.3 → `dephysic Ball`（Unphysicalize 当前球）→ 动画约 1.35 s → `set new Ball`（`Set Cell` 把 `CurrentLevel[0,1]` 换成新球实体、`Set Parent` 把 `Ball_Pos_Frame` 挂到新球、`Activate Script(Reset=TRUE, Gameplay_Refresh)`，出口链路带 `delay=2`）→ `physicalize new Ball`（按 `Get Key Row` 读到的 `Physicalize_GameBall` 行 Switch：Paper = Convex Count 1，Stone/Wood = Ball Count 1 半径 2）。因此换球实体在 Physicalize 之前，Mod 的 `target == get_current_ball()` 判定对两端事件都成立，上报本身是对的。

2026-09-02 修掉的变球缺陷（症状：变球后球不再参与服务端模拟、不再推动机关）：

- **服务端拒绝纸球配方**（根因，见 9.4）。
- **回滚窗口跨变球**（`rollback.hpp`）：重模拟原来整段用锚点 tick 的 `tracked`，跨过变球后驱动的是**已经不存在的旧球**（新球一次输入也拿不到），并且 `nav_poll` 关的也是旧球，重放的每个 tick 都读了实时键盘。改为逐 tick 用该 tick 记录的 `tracked`，窗口内出现过的每个自球都先关轮询、结束后再开。另外快照里映射到"当前球"的行如果在该 tick 的历史里没有对应刚体，不再写回——否则变球后的新球会被拉回旧球在 tick T 的位姿。回归用例 `RollbackEngine.TrafoWindowDrivesTheBallOfEachTick`。
- **对方变球**（`physics_session_apply_event`）：旧球型的幽灵球没有隐藏（`on_trafo` 的 from 传的是 `UINT32_MAX`），转发输入环 `inputs`/`have_input`/`applied` 也没清——重放里旧球的输入会喂给新球。
- **变球那一帧原版力叶子没归零**：`physics_session_zero_retail_forces` 只在帧首（`own_navigation` 已挂上）跑，而新球的导航复制下一帧才挂过去；球还没有刚体时 `SetPhysicsForce.Create` 会排队到 PreSimulate 重试，读到的就是原版力值。现在 `OnPhysicalize` 里立刻再归零一次。

变球到石球时服务端崩在 `IVP_ASSERT(worst_case_speed > max_coll_speed)`（`ivp_mindist_event.cxx:1143`）不是物理问题，是构建问题：`build-retail` 的 `CMAKE_CXX_FLAGS_RELEASE` 是空的，IVP 于是带着断言和每秒一行的统计输出（`IVP_IF(1 || ...)`）编进服务端，而这条界只在 IVP 自己的 `sqrt(1.001 - c²)` 余量内成立，一个自转快又直冲表面的球（石球质量 10）就能踩到。原版客户端的 physics_RT.dll 带 `NDEBUG`，所以只有服务端会死。见 `docs/building-and-deployment.md`；根 `CMakeLists.txt` 现在会在非 Debug 构建缺 `NDEBUG` 时告警。加回 `/O2 /Ob2 /DNDEBUG` 不改变模拟：`BallanceMMOSimTool --level 1 --ticks 2500 --report-every 250` 的每个世界/位姿哈希前后完全一致（速度快 1.7 倍）。


### 9.8 机关的球身份并集（软木桥 P_Modul_29）

症状：物理会话里玩家变成石球后压不断软木桥（绳索踏板桥），桥只是被压得下垂。

原版脚本（`P_Modul_29_MF Script`，SimTool `--dump-script` 实测）：
`TT Scaleable Proximity(ObjectA=Ball_Pos_Frame, ObjectB=P_Modul_29_Platte06, 距离 4)` → `Get Cell(CurrentLevel[0,ActiveBall])` → `Get Name` → `Test(Equal, "Ball_Stone")` → `Wave Player(Misc_RopeTears)` + `10 Hinges` 的 Shutdown（九块踏板脱开铰链落下）。Test 只有 True 出口。

根因：邻近判定早就做了并集（8.2 的 `BMMO_Prox_<k>` 私有 frame），**身份判定没有**。服务端的 `CurrentLevel[0,1]`（ActiveBall）指的是**停放的原版球**：锚点时是 `Ball_Wood`，而且永远不会变——原版 `Trafo Manager` 靠原版球自己滚到变球器附近才触发，停放的球不动。于是 `Get Name` 永远返回 `Ball_Wood`，不管哪个玩家、什么球型压上去，桥都不断。客户端本地脚本读的是自己的球（正确），会在本地断桥，但服务端没断，机关刚体的修正又把踏板拉回去——玩家看到的就是"压不断"。

修法（`physics_world::rewire_ball_identity_reads` / `update_ball_identity_reads`）：锚点扫描所有 `Get Cell` 块，目标数组是 `CurrentLevel`、列是 `ActiveBall`、**且所在根脚本里有被改接过的邻近块**（即机关脚本）的，把该块的目标参数改接到一份 `CurrentLevel` 的私有副本（`CopyObject` + `CK_DEPENDENCIES_COPY_DATAARRAY_DATA`，共享单元里引用的对象）。每 tick 脚本执行前，副本的 ActiveBall 单元写成"离该脚本任一机关（邻近块的 ObjectB）最近的玩家"的球型对应的**原版球实体**（`Ball_Stone`/`Ball_Wood`/`Ball_Paper`，名字正是脚本要比的那个）；没有玩家时写停放的原版球，即原版会读到的值。原版逻辑的读取（`Event_handler`、`Sound_Manager`、`Ball_Shadow`、Gameplay 各脚本）不改接——它们作用在停放球上，必须保持。Level 1–13 的实测：只有 `P_Modul_18_MF Script` 与 `P_Modul_29_MF Script` 被改接（Level 2 是 9 + 1）。

验证工具：`BallanceMMOSimTool --level N --drop <名字片段> <球型> --drop-at X Y Z [--drop-height F] [--drop-sector N] [--drop-second-sector N] [--drop-move 分节 tick] [--drop-prop 实体] [--drop-player-at X Y Z] [--ticks N]`——在真正的会话世界里（停放原版球、克隆球、并集改接）逐个激活分节，把一名玩家的球丢到机关上，逐 tick 打印名字含该片段的刚体位置与 simulated 标志。Level 2 的 P_Modul_29（`PH` 表：分节 3，(960.709,46.4925,-346.743)）跑 300 tick，最低踏板的下落量：

| | 石球 | 木球 | 纸球 |
| --- | --- | --- | --- |
| 修复前 | 2.51 m（只是下垂） | 2.31 m | — |
| 修复后 | **8.51 m（绳断落桥）** | 2.31 m | 0.85 m |

已知边界：

- 远端玩家压断桥时，观察者客户端的本地脚本不会断（它只看自己的球），本地铰链仍拉着踏板，只能靠机关刚体的修正跟随服务端。
- 名字判定的机关一次只认一个球：多个玩家同时在同一机关上时，取离机关最近的那个。

（初版把所有改接的读取都指向原版球实体，于是 `P_Modul_18`（风扇）照旧吹不动玩家球；分节并集当时也是每 tick 只 pop 一个待激活分节。两者都在 9.9 修掉。）

### 9.9 机关按用途取球 + 分节并集重写

**风扇（`P_Modul_18`）吹不动玩家球.** 它读的也是 `CurrentLevel[0,ActiveBall]`，但读到的球是拿去 `Box Box Intersection`（球在不在风口箱子里）和 `SetPhysicsForce`（持续力控制器）的。9.8 把这类读取也指向了**没有刚体**的原版球实体，所以服务端照旧不吹任何人；客户端本地脚本却在吹自己的球，再被服务端快照拉回。

修法：按**消费者**区分同一个 Get Cell 的用途。原版桥的名字判定是一个**参数运算**（`Get Name`）喂给 `Test`，而作用在球上的机关把球交给**行为块**。`rewire_ball_identity_reads` 现在扫 `CKCID_PARAMETEROPERATION`，看有没有运算读这个块的输出（注意参数连线存在读方 `CKParameterIn::m_OutSource`，producer 的 destination 列表在运行时是空的，只能反向找）：

- 有运算读它（Level 1–13 里只有 `Get Name`）→ 写**原版球实体**（`Ball_Stone`……），名字判定成立；
- 没有 → 写**离机关最近的玩家的克隆球**，机关真的作用在那个球上。

日志里能看到分类：`P_Modul_29_MF Script (Get Name) x1, P_Modul_18_MF Script (body) x9`。

实测（Level 2 分节 3 的风扇，PH 矩阵 (1016.67,45.6233,-366.743)，纸球从上方 3 m 丢下）：修复前球落到 y≈47.6 就停住不动；修复后被吹到 y≈66（160 tick 内）。软木桥不受影响（石球仍 8.5 m、木球 2.3 m）。

**分节并集重写.** 原来"只激活、永不反激活、每 tick pop 一个"有两个问题：没人的分节的机关一直在跑；同一 tick 有多个分节变化时后一个会打断前一个。实测原因：`Gameplay_SectorManager` 不是同步执行完的——它走 PH 表要**约 7 帧**（每行的 `Activate Script` 都等一帧），期间 `CurrentLevel[Activation Phase?]` 为 FALSE，走完才置 TRUE；机关脚本在被 `Activate Script` 启动后的第一帧读这个标志，读到 FALSE 就走"关闭"分支。所以在管理器忙的时候再 `scene->Activate(..., reset)`（或直接 `CKBehavior::Execute`）等于把前一个分节的机关全部掐死——1 tick 间隔连开 12 个分节时一个机关都起不来，同 tick 连开 2 个则只有最后一个活。

现在 `physics_world::update_sectors()` 每 tick：

1. `desired` = 所有玩家 `p.sector` 的集合（`apply_event(Sector)` 只更新 `p.sector`，不再直接激活）；
2. 与 `active_sectors_` 求差，取一个待激活和一个待反激活配成一次管理器运行（和原版过检查点时"反激活旧、激活新"完全一样）；
3. 仅在管理器空闲且已空闲 ≥3 tick 时启动下一次（那两 tick 让上一批机关脚本读完标志）；
4. 剩下的差异在随后的帧里继续，日志带 `(more to come)`。

变化在出现的那一 tick 就被接受，不会丢；应用则按管理器自己的节奏排队。实测（Level 2，玩家 1 在分节 3、玩家 2 在分节 5）：`sector +3 -1 at tick 2` → `sector +5 at tick 9`，此时 `P_Modul_29_Platte01..09`（分节 3）与 `PE_Balloon_Platte01..08`（分节 5）同时是刚体；玩家 1 也走到分节 5 后 `sector -3`，桥的刚体消失。

**道具球（关卡自带的非玩家球）不受影响.** 两个并集都只遍历 `players_`：道具球既不会移动邻近探针（`BMMO_Prox_<k>` 只放到玩家球上），也永远不会被写进 ActiveBall 单元，所以机关只认玩家的球——和原版一样。`--drop-prop <实体>`（把关卡里已被分节激活的道具球瞬移到落点，`--drop-player-at` 单独指定玩家球位置）实测：

| 场景 | 结果 |
| --- | --- |
| Level 9 分节 4：玩家**木球**在桥上 + 道具**石球**瞬移到 Platte06 上方 | 踏板只下垂 2.733 m，与没有道具球时**逐位相同**（桥不断） |
| 同上，玩家换成**石球** | 8.492 m，绳断（对照） |
| Level 10 分节 2：道具**纸球**瞬移进风口 + 玩家纸球在旁边 2.5 m | 道具球一路掉下去（-17.6 → -52.3），玩家球被吹上去（-17.0 → -4.1） |

已知边界：一个玩家在分节 N、另一个在 N+2 时，中间没人的分节 N+1 的机关不运行——玩家看到的就是原版"还没激活"的样子（对象隐藏），走到那里时才启动。

### 9.10 出生冲量与确定性 Random 块

**出生环的问题.** 9.9 之前每玩家的出生点是原版 Resetpoint 半径 6 的环形错位（3.4、8.5 第 3 条）；双人联调（8.6）已经看到 Level 1 的起点是个碟形斜面，环上的两个点会一起滚向中心互相顶住。更根本的问题是这个偏移量对大多数关卡不安全——半径 6 很容易把球错位到平台边缘之外、卡进墙里，或者落在原版脚本没设计过要处理的地方；而且它悄悄改写了原版 Resetpoint 的语义：脚本、机关、`CurrentLevel[0,3]` 的读者都认为复活点只有一个,球出现在别处等于给它们喂了假数据。改法是把偏移去掉，所有玩家都在同一个复活点 Physicalize，靠一次性的冲量把它们踢开，冲量方向按加入顺序取表里第几个方向，互不相同。

**冲量的机制.** `session_event_msg` 的 Physicalize 载荷新增 `flags`（`ball_type` 之后），bit0 = `PHYSICALIZE_FLAG_SPAWN`：这次 Physicalize 发生在关卡当前复活点（`CurrentLevel[0,3]`），即出生或复活，不是变球。方向来自一张 64 个黄金角 XZ 平面单位向量的表，下标 = `(join_order + hash(seed, tick)) mod 64`，整个计算是整数运算（`BallanceMMOCommon/include/session/spawn_impulse.hpp` 的 `spawn_direction_index`），保证跨平台位级一致。冲量大小 = 方向 × 速度 × 质量，通过桥接的 `push_impulse` 在球的质心处施加，路径与原版 "Physics Impulse" 块（Referential = 实体、Position (0,0,0)）完全一样（无自旋）。

三处施加时机不同，因为出生这一 tick 里球体的创建时刻不同：

- **服务端克隆球**：`physics_world::apply_event` 处理 Physicalize 时刚体已经创建（8.1 的配方直接建刚体），冲量在这次调用里紧跟着施加，早于这个 tick 剩下的 PreSimulate 与 PSI。
- **客户端自己的球**：BMLPlus 在原版 Physicalize 块函数返回、创建刚体之前先广播 `OnPhysicalize`，所以桥接此刻还找不到刚体；`push_impulse` 于是把冲量排进 `CKIpionManager` 这一帧的 PreSimulate 队列（锚定在 Ball Navigation 的行为块上），块函数创建完刚体之后、这一帧的物理步真正跑之前取出来施加，效果与服务端一次到位等价。
- **远端球镜像**：服务端转发的 Physicalize 事件到达、镜像刚体按配方建好之后立即施加；此后快照按 8.5 第 5 条的历史比较/渐变机制照常纠正剩余误差。

**配置与确定性.** `physics.spawn_impulse`（米/秒，默认 3，0 = 关闭）随 `session_start_msg` 下发（写在 `seed` 之后）；这个值在 `start_physics_session` 里算一次、存进 `physics_session_state`，既随 `create_session` 进入世界也随每一份 `SessionStart` 上线（`runner_config` 里没有它的副本，世界除了调用方无处可取）；单人会话服务端强制发 0，使单人游玩仍与旧录像位级一致（8.6 第 2 条的 `--nav clone` 回放不受影响）。加入顺序需要跨重连稳定：服务端按世界槽位给每个成员分配 `join_order`（进入会话时取最小空闲值，离开时释放），送进 `player_entry` 供两端计算方向下标。9.4 的位姿校验相应改为"复活点 2.5 m 内或该玩家上一快照位置 5 m 内"——单一复活点取代了原来对环上任一槽位的检查。

**确定性 Random 块（Part B）.** `3D Entities\Balls.nmo` 里 `Ball_Explosion_Wood/Stone/Paper`（变球爆炸的碎片脚本，`All_Balls`）会 Physicalize 16-18 块碎片，随后用 Virtools 自带的 "Random" 块给每块碎片的质量/摩擦力/冲量取随机值。这个块的实现直接调用宿主 C 运行时的 `rand()`：原版客户端经 `Logics.dll` 链接到 MSVCRT，无头服务端经 UCRT/glibc 链接——算法不同、调用历史也不同，碎片（可动 core，计入世界哈希约 2 秒）因此无法在两端重放。修法与出生冲量共用同一块桥接代码：`physics_state.cpp` 实现一个与平台无关的确定性生成器（Microsoft 运行时的 LCG，`state = state * 214013 + 2531011`，取值 `(state >> 16) & 0x7fff`，`RAND_MAX` 32767），`install_random_block` 只把这三个爆炸脚本内部的 "Random" 块实例（Balls.nmo 里共 5 个：木 1、石 1、纸 3）的函数换成用这个生成器改写的同一份算术（`random_next()` 代替 `rand()`，`CKBehavior::SetFunction`），**不动原型**：游戏里其余的 Random 块（`Sound.nmo` 2 个、`Gameplay.nmo`/`MenuLevel.nmo` 各 1 个）继续用 C 运行时，这样任何只在一端发生的抽样——比如客户端有声音管理器、无头引擎只有空实现，声音脚本的分支可能不同——都不可能挪动碎片所用的序列。安装点是 `reset_session_clock`（每次会话锚点、每次录制/回放锚点都经过它，且 Balls.nmo 早已加载），客户端 Mod 的 `OnLoad`/`OnLoadObject` 与无头引擎的加载完成处额外再调一次（幂等）。`reset_session_clock` 同时重播种这个生成器；服务端按世界保存/恢复生成器状态，和 `ivp_srand` 游标的做法一样。爆炸碎片在 nocoll 组 "Ball" 里，从不与玩家球碰撞（同组在 IVP 里不产生碰撞，BMMO 的玩家过滤器也排除这个组），所以这处改动只影响碎片自身与世界哈希，不涉及引擎改动。

**同点出生时 IVP 的行为（源码结论 + SimTool 实测）.** 两个球体刚体球心重合时 `minimize_BB` 给零法线（`inv_len = 1`，无除零、无 NaN），`max_coll_speed` 为 0，不排任何碰撞事件；正在分离的一对球 IVP 完全不管；一对球在仍然重叠时又靠近，`p_calc_friction_s_PP` 把负间隙钳到 0，冲击求解器只以约 1.6 m/s 的"救援速度"把它们推开，不会爆开。实测（`--spawn-test`，Level 1/2，木球，踢出 3–5 m/s，3–4 个球）：球在 0.5 s 内分开 4–6 m，滚回碟底后正常相撞，150 tick 内全部静止在相互接触的位置（球心距 4.02），无 NaN；Windows x64 与 Linux 逐 tick 哈希一致。曾试过让球心距小于 3.5 的两个玩家球互不碰撞（"先穿过再相撞"）——IVP 只在 OV 树重新插入对象时才重新询问碰撞过滤器，被拒绝过的一对球之后一直互不碰撞、最终重叠着静止，因此撤掉了，玩家球之间始终碰撞。

**碎片位级重放要过的三道坎（客户端录像 vs 无头回放，`explode <type>` + `--explode`）.** 把 Random 换成确定性生成器之后碎片的速度已经逐位一致，但位置仍差 0.14 m：(1) 游戏启动时 `Balls_Init` 的 `Init Ballpieces` 把碎片物理化了几帧再取消，碎片下落的距离取决于启动阶段的帧间隔（原版客户端是实时帧、每次启动都不同：实测 -1.165 / -1.207；无头固定 15.15 ms：-1.0085），所以每个进程里第一次爆炸的碎片起点都不一样——`reset_session_clock` 现在按原版 `Ball_ResetPieces_*` 的 `TT Restore IC` 做法把三个 `Ball_*Pieces_Frame` 层级恢复到文件里的初始条件（`restore_explosion_pieces`）。(2) 恢复之后仍差一个 float ulp：原版 CK2（x87 老代码）与重实现的 CK2 对层级矩阵的舍入不同，而 `Balls_MF`、碎片父框架、`Ball_Pos_Frame` 都带着关卡文件里 1e-6 的倾斜/缩放，任何一次 local↔world 换算都不精确。修法是让整条链只剩精确运算：碎片父框架脱离 `Balls_MF`、旋转部分写成精确单位阵、再恢复子物体的初始条件（子物体 local = world × I 精确）；`Ball_Pos_Frame` 的局部平移（文件里是 5e-6）清零，它的世界矩阵就等于球的世界矩阵。爆炸脚本开头的 `Set Position` 块被包了一层（和 Random 块一样按实例 `SetFunction`），每次爆炸前先做这一步，因为原版的 Restore IC 会把父框架重新挂回 `Balls_MF`。(3) 剩下一两块带一般旋转的碎片四元数仍差 ulp：`FillTemplateInfo` 用宿主 VxMath 的 `VxQuaternion::FromMatrix`，原版 `VxMath.dll` 与重实现对一般旋转的结果差最后一位——引擎改动 #8 把这个换算搬进 physics_RT 自己（重实现的算法，无头结果不变）。(4) 最后还剩木球的 `piece08`/`piece16`（以及惯量差得更小的 `piece13`）：差的不是物理，是喂给凸包的**缩放**。`CreatePhysicsObjectOnParameters` 开头 `target->GetScale(&scale)` 默认取**局部**矩阵，而局部矩阵是 CK2 用父物体的逆矩阵乘出来的；碎片挂在 `Ball_WoodPieces_Frame` 下、后者挂在带 0.99999 缩放的 `Balls_MF` 下，这步乘法不精确，原版 CK2 与重实现的舍入不同。51 块碎片里有 3 块的行长度差一个 ulp（例如 `piece08` 的 z：客户端 `0x1.0000620p+0`，无头 `0x1.0000640p+0`）。这个缩放会乘到每个网格顶点上再编译凸包，于是转动惯量不同（`piece08`：`1.e210f6/1.fd609e/1.555e4a` 对 `1.e211000/1.fd60a6/1.555e46`），碎片从第一个模拟帧起就转得不一样——`piece08` 位置完全一致、只有四元数不同，正是纯惯量差异的样子。凸包在**游戏启动时**（`Balls_Init` 把每块碎片物理化一次再取消）就按碰撞面名字编译并缓存，几分钟后的爆炸直接命中缓存，所以后做的层级归正救不回来。修法是引擎改动 #9：给缓存加一个按名字删除的接口，`restore_explosion_pieces` 在碎片没有刚体时把它的凸包从缓存里丢掉，下一次 Physicalize 就用归正后的层级重新编译，两端输入一致。排除法：我在 physics_RT 里用 `sqrtf(x²+y²+z²)` 自己算过一遍，两端各自都与本地 `GetScale` 完全吻合，所以不是算法不同而是输入不同；其余 48 块碎片的行接近轴对齐，那步推导本来就精确。

实测（原版客户端录像 vs 无头回放，Level 1，`explode` 后约 6 s，每次用干净的客户端进程）：木球 3457/3457、石球 3457/3457、纸球 3456/3456，全部逐位一致。`--explode` 的 200 tick 轨迹在 x64、x86、Linux 三个无头引擎之间也全等。单人回放 `rec_m3b.bmrc` 仍是 4169/4169。注意录制前要用干净的客户端：上一次爆炸的碎片若还在场，锚点帧的可动刚体集合与无头回放对不上（实测 `cores=5/0`，第 0 帧即分叉）。

**引擎改动 #7（`ivp_mindist.cxx`）.** 球-球 mindist 原来按 `client_data` 指针（即 `CK3dEntity*` 的堆地址）决定两个突触的顺序，从而决定接触法线的符号和两个 core 在冲击/摩擦求解里的角色；客户端与服务端的堆地址不同，第一次球球接触就会分叉。现在按 nocoll 组名（两端同一玩家的球都是 `P#<join_order>`）再按名字排序。原版游戏里只有一个球体刚体，这条路径从未执行过，单人回放不受影响（`rec_m3b.bmrc` 仍 4169/4169）。

**验证工具.** `BallanceMMOSimTool --spawn-test N [--spawn-impulse S] [--ticks T]` 起 N 个玩家在同一个复活点各领一个方向的冲量，跑若干 tick 检查球之间不再重叠、无 NaN、两端哈希一致；`--explode <wood|paper|stone> AT_TICK` 触发对应的爆炸脚本并逐 tick 打印世界哈希，用来对比 Windows/Linux 在碎片飞散阶段是否仍然位级一致；客户端自动化命令新增 `explode <type>` 触发同一段脚本用于目视/录制对照。以上都建立在桥接 API v6 之上（`push_impulse`、`random_reset/get_state/set_state/next`、`install_random_block`）。

### 9.11 可动机关的位级回放

9.10 之后把同一套办法用到机关上：客户端录制时逐个激活分节（自动化命令 `sector <n>`，做法与服务端 `update_sectors` 相同），机关被物理化并在重力与接触下自行落位；无头侧用 `--sector N FRAME` 在同一帧做同样的事回放，逐帧比世界哈希。13 个关卡各跑一遍，覆盖 PH 表里出现过的全部机关类型（`P_Modul_01/03/08/17/18/19/25/26/29/30/34/37/41`、`P_Box`、`P_Ball_*`、`P_Dome`、`P_Trafo_*` 等）。

**查出并修好的：铰链机关（引擎改动 #10）.** Level 8 的倒板 `P_Modul_30_Wippe` 从关卡开始就在模拟，第 64 帧两端的速度差一个 ulp，位置还完全相同；Level 9 同样的问题出现在第 534 帧。根因是 `Set Physics Hinge` 的转轴取自 `CK3dEntity::GetOrientation`，那个函数把世界矩阵的一行归一化——一个平方根加一次除法；关卡矩阵的行长度从来不是精确的 1，原版 `VxMath.dll`（x87，客户端还跑在 24 位精度下）与重实现的最后一位不同，转轴一歪，铰链驱动的刚体从第一帧起就不一样。同一类问题还出现在 `GetScale`（凸包缩放）。改动 #10 把这三处换算搬进 physics_RT 自己算，无头结果不变。

**仍然存在的：脚本摆放的活动部件.** 剩下的分叉全部集中在两个机关：摇篮的转臂 `Modul17_Dreharme`（Level 3/7/9/13）与秋千 `P_Modul_08_Schaukel`（Level 8/10/11）。用逐帧实体矩阵跟踪（`BMMO_TRACE_ENTITY`）定位到：它们的**实体世界矩阵在刚体出现之前就已经不同**（秋千是提前 2 帧，摇篮提前 1 帧），而它们的父框架 `P_Modul_08_MF` 全程 4231 帧逐位相同，PH 表给父框架的矩阵也是轴对齐的。也就是说差异出在 CK 侧从世界矩阵推导子物体局部矩阵那一步（分节激活时的 Restore IC 会走这条路，父矩阵求逆再相乘），原版 CK2.dll 与重实现的舍入不同。这在 physics_RT 里无法修复——要修得让客户端也用 BMMO 编译的 CK2/VxMath 与其余 Building Blocks，而不是只替换 physics_RT.dll，属于另一个量级的改动。

对会话本身没有影响：机关由服务端权威，客户端的副本按快照修正，而这里的差异是 1e-7 量级，比修正阶梯的忽略阈值（1 cm）小五个数量级。它只影响离线回放工具对这两个机关的位级验证。

**逐关结果（原版客户端录像 vs 无头回放，全部分节激活）：**

| 关卡 | 结果 | 首个分叉的刚体 |
| --- | --- | --- |
| 1、2、4、5、6、12 | 逐位一致 | — |
| 3、7、9、13 | 分叉 | `Modul17_Dreharme`（摇篮转臂） |
| 8、10、11 | 分叉 | `P_Modul_08_Schaukel`（秋千） |

Level 10 另有一次第 7 帧的哈希差异，那时可动 core 的精确状态两端完全相同，差的是环境量（固定节拍刚启用时的平滑 delta），属于录制起点的时序噪声，不是机关问题。

**工具.** 客户端自动化命令 `sector <n>`（激活分节）、`beam <x> <y> <z>`（把当前球瞬移到机关上方砸下去）、`entity <name>`（实体矩阵）；SimTool 对应 `--sector N FRAME`、`--beam X Y Z FRAME`、`--dump-entity NAME`。脚本 `<scratchpad>/mech_check.py` 把"录制—激活分节—回放—比对"串成一条命令。

> 上面"需要客户端也换用 BMMO 编译的 CK2/VxMath"的判断，在 9.12 里被推翻了：方向对（确实是 CK 侧的矩阵推导），落点错（不在 CK2），而且一个 DLL 都不用换。

### 9.12 定位并消除重实现与原版 VxMath 的求和顺序差异

**落点错在哪.** `CK3dEntity` 根本不在 `CK2.dll` 里。原版整套 DLL 没有任何一个导出 `CK3dEntity::GetScale` / `SetWorldMatrix`——它们在 SDK 头里是 `CK_PURE` 纯虚，实现随类注册一起放在 `RenderEngines/CK2_3D.dll`；fork 里对应的也正是 `Source/RenderEngine/src/CK3dEntity.cpp`。而 CK2_3D 自己不算矩阵：推导子物体局部矩阵那一步是 `Vx3DMultiplyMatrix(child->m_LocalMatrix, parentInverse, child->m_WorldMatrix)`，一个 **VxMath.dll 的导出函数**。真正握着分叉的库是 VxMath，不是 CK2。

**差在哪.** 浮点加法不满足结合律。四个乘积 `p_k = A[k][j]*B[i][k]`，重实现从左往右累加 `((p0+p1)+p2)+p3`，原版两两相加 `(p0+p1)+(p2+p3)`。每个元素差 1–2 ulp，正好是机关漂移的量级。

**怎么确定的：量出来的，不是读出来的.** `scripts/vxmath_diff` 用 `LoadLibrary` 按修饰名加载原版 `VxMath.dll`（不能链接导入库，否则它的符号会和 fork 同名的 inline 版本在同一映像里撞车），把 x87 控制字设成客户端实际运行的 24 位精度（`000a001f`），先用单位阵／平移阵／缩放阵确认两边对存储顺序和操作数顺序的理解一致，再穷举四个乘积的全部求和顺序去对原版。6 万对随机矩阵下只剩一种分组存活。两个向量变换用同样办法测出原版是从左往右——重实现的标量路径本来就对，是它的 SSE 内核折反了方向。

**改动（引擎改动 #11）.** `Vx3DMultiplyMatrix` / `Vx3DMultiplyMatrix4` 的标量与 SSE 路径改成两两相加，`VxSIMDMatrixMultiplyVector3` / `VxSIMDMatrixRotateVector3` 改成从左往右。SSE 一律写成显式 `_mm_mul_ps` / `_mm_add_ps`，不再用 `VX_FMADD_PS`，免得开了 FMA 的构建把一次舍入融掉。改完这四个函数在 20 万组输入（随机的和 Ballance 形状的各一半）上与原版逐位相同。

**方向.** 客户端一个字节都没动，仍然用原版 `VxMath.dll`（physics_RT 链接的是 Virtools SDK 导入库）；是重实现向已经发行的二进制靠拢。

**结果.** 13 关机关扫描：3、7、9、13（摇篮转臂）和 8（秋千）全程逐位一致，原先一致的关卡一个都没退化。10 和 11 仍报哈希不符，但那根本不是物理差异（见下）。四个平台（Windows x64 / Windows x86 / Linux x86_64 / Android arm64）在改动前后都是 17 个用例逐帧完全相同。

**10 和 11 是哈希的顺序敏感，不是物理分叉.** 世界哈希按 `CKIpionManager::m_MovableObjects` 的存放顺序逐个折叠 core，而两端有两个刚体的先后是反的，**每个刚体的状态逐位相同**。Level 11：把录像自己存的精确状态按 `P_Modul_26_Rope`、`_Sack`、`_Rope001`、`_Sack001`、`Ball_Wood`、`P_Modul_08_Schaukel`、`Schaukel001` 的顺序折叠，正好还原出录像里的哈希，而重实现把最后两个反了过来；809 帧之后（第 4500 帧）七个刚体依然逐位相同。Level 10：只有第 7–27 帧不符，正是 `P_Modul_01_Pusher/_Rinne/_Filler` 醒着的那段窗口，期间九个刚体全部相同，其余 4204 帧一致。证明方式是离线的——录像里存的是 `%a` 十六进制浮点，可以精确重建 FNV-1a 折叠再穷举排列（`scripts/order_solve.py`），不需要跑客户端。

这个列表由 IVP 的 `event_object_revived` 追加，只被用来把刚体位姿写回实体矩阵，不进入求解器——和"刚体自始至终没分开"是一致的。**所以 13 关的物理全部逐位可回放**，剩下的只是诊断哈希问得比"状态是否相同"更严一点。要修有两条路：折叠前按名字排成规范顺序（会作废现有全部录像），或者让回放工具认出这种情况；都还没做。

**为什么最终没有换库.** "换 VxMath.dll"这条路也量了一遍，比预期贵得多。fork 把 `Vx3D*` 系列做成了头文件 inline，`VxMath.dll` 一个都不导出，而游戏其余模块实际 import 了其中 168 个符号，全得补成导出转发；`CK2.dll` 同样缺 219 个（135 个只是 `char*` 变 `const char*`、返回类型 `CKERROR` 变 `int` 这类签名漂移，另外 84 个另有原因）。更要紧的是 fork 的 VxMath 与原版还有**语义**差异，不只是舍入：`Vx3DMatrixFromRotation` 的结果与原版互为转置，且原版的正余弦只有约四位有效数字；`Vx3DInterpolateMatrix` 在 step=0 时两边返回的都不是同一个端点。把这样一个 VxMath 塞进原版游戏，会改掉物理之外的行为。

**仍然不同的函数.** `Vx3DInverseMatrix`（`GetInverseWorldMatrix` 会用）、`VxVector::Normalize`、`VxQuaternion::FromMatrix`、`Vx3DDecomposeMatrix`、`Vx3DInterpolateMatrix`、`Vx3DMatrixFromRotation`。求逆试了 36 种写法（余子式用 float 还是 double、行列式三项的求和顺序、倒数取 float 还是 double、最后一次乘法在哪个精度）都没对上，说明原版用的是另一套算法，而不只是另一种舍入顺序。Level 11 剩下的分叉仍在秋千上；Level 10 是第 7 帧 `P_Modul_01` 物理化时的老问题（匹配帧数从 2837 涨到 4210，首帧分叉位置未变）。

**四平台基线.** `scripts/determinism_baseline.py <tag>` 在一个引擎构建上跑固定的 17 个用例（13 关机关扫描 + `rec_m3b` 游玩回放 + 三种爆炸），把每帧的引擎自身状态（世界哈希、活动 core 数、IVP 时钟与种子、movement-check 计数、下次 PSI 时刻、位姿是否与客户端一致）压成一行；`scripts/compare_platforms.py` 跨平台比对。这套基线是改动前后判断"有没有退化"的依据，也是以后任何引擎改动的第一道关。

### 9.13 Linux 服务端上的"world mismatch"：碰撞面签名不跨平台

**症状.** 原版客户端连 Linux 服务端，开物理会话必被拒：`Physics session 1
(room 1) ended: world mismatch for <player> (client <A>/<B>, server <A>/<C>)`。
`session_ready_msg` 的两个值里，可动 core 位姿哈希 `anchor_hash` 两端相同，
只有碰撞面签名 `anchor_surfaces` 不同——也就是说物理状态一致，被拒的是几何。
Windows 服务端不复现。

**根因（引擎改动 #12）.** `surface_signature` 直接哈希每个刚体
`IVP_Compact_Surface` 的原始字节，而这块 blob 的排布是构建选项决定的：
`IVP_Template_Surbuild_LedgeSoup` 的 `merge_points` 默认值在 `WIN32` 下是
`IVP_SLMP_MERGE_AND_REALLOCATE`（把重复顶点合并进一个共享点数组），其它平台是
`IVP_SLMP_NO_MERGE`（每个 ledge 各存一份）。同一个凸包，两种存法：
`mass_center`、`rotation_inertia`、`upper_limit_radius` 逐位相同，节点数、ledge
数、三角形数也相同，但 blob 大小和内容不同（Level 2 的 `A01_Floor_00`：579 节点 /
290 ledge / 580 三角形，合并后 32916 字节，不合并 44100 字节）。签名于是必然不等。
把默认值统一成游戏在跑的那一种即可；客户端不受影响（`WIN32` 分支本来就是它）。

**复现与验证用的手段.** `physics_state.cpp` 的 `describe_surfaces_exact` 逐刚体
打印 `blob`（签名哈希的那些字节）、`head`（三个头部浮点的哈希）和节点/ledge/三角形
计数，SimTool 的 `--dump-surfaces-at N` 把它导出来，两端文本 diff 就能把"几何真的
不同"和"只是存法不同"分开；`report` 也一并打印 `surfaces=`。定位过程就是这样做的：
42 个刚体的 `head` 和计数全等，`blob` 全不等。

**验证.** Linux 上改动前后：三个关卡各自由跑 1200–1500 tick、三次 trafo 爆炸、
出生冲量测试、机关落体，输出逐字节相同，Level 1 录像回放的帧数与哈希也不变。改动后，
同一组用例（连同 `surfaces=`）在 Linux x64 与 Windows x86 无头构建之间逐字节相同；
原版客户端连 Linux 服务端（本机 WSL）开 Level 2 物理会话，锚点两端都是
`0ce8aeb84f5f017c`，会话正常开始。

**顺带修的：房间命令没有回显.** 客户端按可靠有序连接把 `RequestAccepted` /
`RequestDenied` 依次对应回自己发出的子命令（协议 1.3）。比 632bf1c 早的服务端对
`Ready`/`Unready`、`List`、`Close` 根本不回结果，客户端于是既不回显，也会把队首那条
错配给下一条命令的结果。现在超过 5 秒没等到结果就报
`Error: the server did not answer "/mmo room ready".` 并丢弃该条，队列不再错位。

**握手改成比引擎版本，`require_physics_sha` 删除.** 锚点的两个值里，`anchor_hash`
在没有可动刚体的锚点上恒等于 FNV 的初值，实际起作用的只有 `anchor_surfaces`；而它只
回答"世界一不一样"，回答不了"物理代码一不一样"——积分器被改过、关卡相同的客户端照样
握手通过。原来补这一块的 `physics.require_physics_sha` 并不称职：那串 sha 由客户端自
己算自己报（不是完整性保证），默认空所以没人开，每次重编都要改配置，`headless-` 前缀
还直接绕过。

现在两端都编进同一个构建 id（`ballanced-<引擎提交>+bmmo-<仓库提交>`，
`cmake/BuildId.cmake`），`SessionReady` 上报，服务端只比引擎那一半——仓库那一半会因为
改文档而变——不同就结束会话并写明两边版本，任一边解析不出（从没有 git 的导出目录构建）
则不比，只记一行日志。构建 id 改成**每次构建前**生成而不是 configure 时定死：旧的做法
让一棵长期不重新 configure 的构建树一直自称当初那个提交（`build-client-stock` 报
`bmmo-72f61dc9b55c` 而二进制是当前源码），拿这种标识去卡准入只会既放过该拦的又拦下不该
拦的。工作区有未提交改动时 id 带 `-dirty`：同一提交、不同工作区，本来就不是同一个构建。

三条路径都在 WSL Linux 服务端 × 本机 Win32 原版客户端上实测：引擎版本相同 → 会话正常
开始；服务端用 `-DBMMO_BUILD_ID` 换成另一个引擎提交 → 两端都收到
`Swung0x48 runs engine ballanced-40ce91e307ff, this server runs ballanced-000000badbad`；
服务端 id 为 `unknown` → 日志记 `not comparable, letting it in`，会话照常开始。

### 9.14 变球碎片总是落在第一个变球器那边（引擎改动 #13）

**症状.** 物理会话里第二次以后的每一次变球，碎片都不在当前变球器炸开，而是出现在
**本次会话第一次变球**的那个变球器旁边——正是第一批碎片当时停下来的地方。

**根因.** 不在 9.10 的碎片归正链上，而在引擎改动 #6 的 body guard。原版一次变球的
碎片生命周期是：`Ball_Explosion_<type>` 把 51 块碎片 Physicalize，两秒后
`Ball_ResetPieces_<type>` 淡出、**De Physicalize**、`Restore IC`。会话期间 guard 是
"除了自己的球，谁也不许 Unphysicalize"，于是这条 De Physicalize 被吞掉，碎片刚体从第
一次变球起就再也没消失过；下一次爆炸走到 Physicalize 块的"已经物理化"提前返回，而
guard 在那里会把**刚体的**位姿写回实体（#6 用来抵消死亡时分节重置的矩阵复位），碎片就
被拉回上一次停下的位置。除了看起来不对，那堆看不见的刚体还一直躺在第一个变球器旁边挡
球，而服务端世界里根本没有它们。

碎片是原版脚本在会话期间**自己创建又自己销毁**的唯一一批刚体，服务端不持有它们，本来
就不该由 guard 保护。引擎改动 #13 于是给 guard 加了一张豁免表：
`restore_explosion_pieces` 顺手把三个 `Ball_*Pieces_Frame` 层级（54 个实体）写进
`m_KeepLevelBodiesFree`——它本来就要走这些层级，而且在每次锚点和每次爆炸开头都会跑，
豁免表永远早于碎片被物理化就位；`set_body_guard(false)` 清空它。引擎仍然不认识任何
Ballance 的对象名。

**无头复现（SimTool 新增 `--activate SCRIPT TICK`、`--body-guard ENTITY TICK`，
`--explode` 可重复，`--beam` 也能在自由跑里用）.**

```
BallanceMMOSimTool --root <game> --level 2 --level-at 30 --ticks 615 \
    --explode wood 300 --activate Ball_ResetPieces_Wood 400 \
    --beam 150 10 -141 560 --explode wood 600 --list-bodies-at 610 \
    [--body-guard Ball_Wood 250]
```

`--activate` 是必需的：`--explode` 只激活爆炸脚本，而原版是变球流程稍后再激活
`Ball_ResetPieces_<type>`。改动前，带 `--body-guard` 的一跑第二次爆炸的碎片全部留在
第一次的落点（~(20, 8, -153)），不带的一跑则出现在被 beam 过去的球那里（~(150, …)）；
改动后两跑的 20 个球体刚体位置与两次爆炸的 215 个 `pose` 哈希逐位相同。单次
`--explode` 的输出、`rec_m3b` 回放（4169/4169）、`--spawn-test 3` 与 82 个单元测试均
不变。

**线上实测（2026-09-04，okbc.st 服务端 + 本机原版客户端，Level 2 物理房间）.** 客户端
自动化也补了对应的 `activate <root script>`（`explode` 只放爆炸，缺的就是变球流程稍后
激活的 `Ball_ResetPieces_<type>`）。会话中依次：`explode wood`（球在 (19.2, 8.7,
-152.9)）→ 碎片落在球周围；`activate Ball_ResetPieces_Wood` → 碎片实体回到初始条件
(0.360, -1.002, -0.290)、`physobjs` 里只剩 `Ball_Wood`，**证明 De Physicalize 这次真的
过去了**；把球开下去摔到 (90.8, -22.4, -136.0) 再 `explode wood` → 碎片出现在
x≈85–115、z≈-122…-148，跟着球走，不再留在第一次的落点。整段会话 7142 个快照里 7096 个
一致，46 次不符全部来自开局的装饰纸球（最大 2 cm），三次死亡复活都正常。

### 9.15 会话黑匣子（服务端日志 + 离线回放 + 客户端录制）

**动机.** 只在真人多人房间里才出现的物理 bug，此前没有任何复现手段。`.bmrc` 录像（客户端
`record start`，`BallanceMMOSimTool --replay`）录的是一个人的键盘，而且 `record start` 会重置
会话时钟，天然是单人的；除此之外只剩客户端日志（`session trace on`：`mismatch at tick N`、
`rollback to tick N`）和服务端 `physics.debug_trace`，两边都只给症状，给不出一个能重跑的世界。
但服务端本来就是权威且确定的：给定锚点世界、每 tick 每个玩家的输入帧、客户端上报的生命周期事件，
模拟逐位可复现（第 8 节的前提，Windows/Linux 已经对齐，见 9.13）。把这三样原样记下来，
会话就能离线一比一重放——这就是黑匣子。

**确定性契约.** `bmmo::sim::physics_world` 的全部输入面只有六个调用：`create()`、`add_player()`、
`remove_player()`、`set_input()`、`apply_event()`、`tick()`。journal 记的正是它们，
`physics_world.hpp` 的类声明前把这条契约写成了注释：以后谁再给世界加一路输入，必须在同一个动作里
补进 journal，否则用到它的那些会话，每一次回放都会静默分叉。契约里还有一条不是"记什么"而是"按什么顺序"：
**同一 tick 到期的多条事件按成员的 join order 应用**，不是按到达顺序（下面单开一段说为什么必须是它），
服务端与任何一次离线回放共用这同一条规则。

格式放在 `BallanceMMOCommon/include/session/journal.hpp`（纯头文件，写与读都在里面），因为写它的是
两边：服务端写它的世界吃进去的东西（kind 0），客户端——原版 mod 或无头会话客户端——写同一批记录，
外加它收到的快照和它自己做的每一次修正（kind 1）。读它的有三处：SimTool 的 `--replay-session`、
`scripts/journal_trace.py`、单元测试 `BallanceMMOServer/tests/session_journal_test.cpp`（14 个用例，
含 event_tick 的往返与旧格式兼容）。

**格式（version 1，扩展名 `.bmjr`）.** 一律小端、逐字段写，绝不 dump 结构体：写的可能是 Linux x64
服务端，读的是 Windows x64/x86。

```
file   = magic "BMMOJRNL" (8 字节) , u32 version , record*
record = u8 tag , u32 payload_size , payload[payload_size]
str    = u16 长度 , 该长度的字节（无结尾 0，上限 4096）
```

| tag | 名称 | payload |
| --- | --- | --- |
| 0 | HEADER | `u8 kind`（0 服务端 / 1 客户端）, `u32 session`, `i32 level`, `i32 seed`, `f32 spawn_impulse`, `u32 input_delay`, `u32 checkpoint_ticks`, `u32 first_tick`, `u64 anchor_hash`, `u64 anchor_surfaces`, `str build_id`, `u64 utc_ms`, `u32 own_player`, `u8 own_join_order` |
| 1 | PLAYER | `u32 tick`, `u32 id`, `u8 join_order`, `u8 added`（1 加入 / 0 离开）, `str name` |
| 2 | INPUT | `u32 tick`, `u32 id`, `u8 repeat`, `u8 flags`；`repeat == 0` 时再跟 `u8 keys`, `f32 cam_right[3]`, `f32 cam_up[3]`, `f32 cam_dir[3]`, `u8 ball_type`, `u8 input_flags` |
| 3 | EVENT | `u32 tick`（世界**应用**它的 tick）, `u32 id`, `u8 type`, `u8 ball_type`, `u8 flags`, `f32 position[3]`, `f32 rotation[9]`, `i32 sector`, `str name`, recipe, `u32 event_tick`（事件**自带**的 tick） |
| 4 | TICK | `u32 tick`, `u64 hash`, `u64 pose`, `i32 cores`, `u32 ms`, `str probe_name`, `f64 probe_position[3]`, `f32 probe_speed[3]` |
| 5 | CHECKPOINT | `u32 tick`, `u8 flags`, `u32 count`, count × body |
| 6 | NOTE | `u32 tick`, `str text` |
| 7 | CORRECTION | `u32 tick`（快照的 tick）, `u32 local_tick`, `u8 kind`, `str entity`, `f32 error_m`, `f32 velocity_error`, `f64 local_position[3]`, `f64 server_position[3]` |

```
body   = u8 kind (0 球 / 1 机关) , u32 owner , str name , f64 position[3] ,
         f64 rotation[4] (x,y,z,w) , f32 linear[3] , f32 angular[3] , u8 flags
recipe = bmmo_physics_ball_recipe 的字段按声明顺序，定长字符数组写成 str；
         convex / ball / concave 三组计数写时钳到 BMMO_PHYSICS_MAX_*，读时校验
```

除 HEADER 外每条记录的第一个字段都是 tick，所以读者不认识任何 tag 也能按 tick 分组；长度前缀让格式
双向兼容：老读者跳过不认识的 tag（并计数），也会忽略新写者往已知 payload 尾部追加的字段。

`INPUT.repeat == 1` 表示"与本文件中该玩家上一条 INPUT 逐位相同"（没人动相机时的常态，一条 10 字节）；
`flags` bit0 = FRESH（写者当时确实拿到了这个玩家自己的帧），bit1 = RELAYED（客户端：这帧是服务端
中转来的，也就是服务端真正应用的那一帧）。`CHECKPOINT.flags` bit0 = FULL（全部可动刚体，带名字）、
bit1 = LOCAL（客户端自己的世界）、bit2 = RECEIVED（收到的快照原样）。`CORRECTION.kind` 依次是
0 mismatch、1 rollback、2 hard、3 blend、4 resync、5 too_far、6 frozen、7 unmatched
（`session/rollback.hpp` 的 `rollback_correction`，回滚引擎的 `on_correction` 回调把它们送出来）。
NOTE 除自由文本外还承担结构化里程碑，用前缀区分：`start:`、`boot failed:`、`anchor:`、`assigned:`、
`resync:`、`late join:`、`event failed:`、`mark:`、`cap:`、`end:`。

**EVENT 为什么有两个 tick.** 服务端 `step()` 把所有 `e.tick <= tick` 的事件出队并在**当前** tick 应用，
而事件自带的 tick 是客户端盖的章：会话故意落后客户端 `input_delay` 个 tick，抖动一超就会有事件迟到，
落到服务端已经跑过的 tick 上。这两个 tick 缺一不可——分组的 tick 决定回放**何时**应用它，而
`lifecycle_event::tick` 决定出生冲量的方向（`spawn_direction_index(seed, slot, event.tick)`，9.10）。
只记标记 tick 会把事件提前应用，只记应用 tick 会把球踢向错误的方向。所以 `tick` 是应用 tick、
`event_tick` 是标记 tick（原样写，0 也是合法的标记：客户端在锚点帧就是 0），追加在 recipe 之后，
早于该字段的旧文件读出来 `event_tick = tick`。两者不同时，SimTool 的 `--list` 打
`stamped S applied T`、回放打 `tick T: <type> event from pN was stamped for tick S`，
`journal_trace.py` 打 `event@S`——这个差本身就是值得看见的症状。下面实测的第二次会话里，16 条
BodyRevived 是客户端盖 tick 6、服务端 tick 7 才应用的，还有三条 Unphysicalize 盖 2959/3752/3760、
同样晚一个 tick 才应用。

**同一 tick 的两条事件谁先应用.** `step()` 把这个 tick 到期的事件**全部取出**，按事件主人的
**join order** 稳定排序，再逐条应用；未知玩家（他的移除与事件擦身而过）排在所有已知玩家之后，
同一名次内保持到达顺序。顺序是有后果的：两个玩家在同一 tick 各 physicalize 一个球，世界按应用顺序给它们
建刚体，换个顺序整段模拟就是另一个世界。之所以定成 join order，是因为它是**唯一一个客户端也能复现的
顺序**——谁的包先到只有服务端知道，而每个成员都知道每个人的 join order。于是同一条规则写在两处：
服务端的 `session_runner.cpp::step()`，以及 SimTool 回放**客户端** journal 时对每个 tick 组的重排
（`sim_tool.cpp`）。服务端 journal 不动一个字：它按实际应用顺序落盘，文件顺序**就是**应用顺序；
客户端 journal 的组内顺序只是它自己的收信顺序，不再拿来当真，回放开头会横幅说明
`client journal: same-tick events applied in join-order order, the server's rule`，
`--write-journal` 写出的也是重排之后的顺序。
这是 9.15 最后堵上的一个致命缺口：在这条规则之前，joiner 那份 journal 从两人同 tick physicalize 的那一刻
起就整段不可用（下面的实测有两轮对照数字）。

**服务端捕获点.** 全部在 `BallanceMMOServer/sim/session_runner.cpp`，模拟线程上，不加锁：

- `create_session()`：`physics_world::create` 成功后开文件，写 header（level、seed、
  **本会话的** spawn_impulse 而不是配置里的、本会话的 input delay、checkpoint 周期、
  `first_tick`、锚点 hash/surfaces、`bmmo::physics::build_id()`、UTC 毫秒），一条
  `start: room <id> "<房名>": <名字>(<id>, join <序号>), ...` NOTE，然后每个初始成员一条 PLAYER。
  启动**失败**也照样开文件：header（锚点写 0）+ `boot failed: <error>` NOTE + 关闭——黑匣子在最需要它
  的时候不能是空的。
- `add_player()` / `remove_player()`：各一条 PLAYER。
- `step()`：`buffer.take(tick, fresh)` 真正交给世界的那批输入逐条 INPUT（`fresh` 进 FRESH 位）；
  每条出队事件（已按 join order 排好，见上）在 `e.event.tick = e.tick` 之后一条 EVENT，写入顺序就是
  应用顺序（`apply_event` 失败也写，再补一条 `event failed:` NOTE）；`world->tick()` 成功后 `capture_world_hash` 一条 TICK（`ms` 是距 header 的
  稳定时钟毫秒）；每 `journal_checkpoint_ticks`（默认 660 = 10 s）一条 FULL CHECKPOINT。
- `destroy_session()`：`end: <reason>` NOTE，关闭。

录制不能改动模拟，也不能改动**服务端发出的字节**。checkpoint 因此走新增的只读通道
`physics_world::snapshot_for_journal()`：它与 `snapshot(true, ...)` 共用 `collect_bodies()`，但不写
`body_index_`、`last_body_set_`、`body_set_changed_`。否则 journal 的那次全量快照会替服务端提前给
非模拟机关编号（编号就是 delta 快照里上线的 `owner`），也会在关卡脚本于本 tick 内新建刚体时把
`body_set_changed_` 顶起来，让下一 tick 多发一份 FULL 快照。默认配置下两者都够不着
（`snapshot_interval` 2 与 `full_interval` 66 都整除 660），但这是个必须堵死的洞，不是可以留着的巧合。

**服务端配置.** `physics.journal_dir`（默认 `journals`，空 = 不录）、`physics.journal_max_mb`
（默认 256）、`physics.journal_checkpoint_ticks`（默认 660）。`journal_dir` 用
`config_manager::resolve_path` 相对**服务端自身所在目录**解析，不是进程 cwd：无头引擎启动时会把
工作目录切到游戏的 `Bin`。文件名 `session_<id>_level<N>_<UTC yyyymmddhhmmss>.bmjr`。启动横幅里带
`journal <目录>`（关掉时是 `off`），控制台 `sessions` 命令的每行末尾带该会话的 journal 路径。

**客户端录制.** 原版 mod 侧是 `BallanceMMOClient/session/session_journal_client.hpp/.cpp` 的
`bmmo::session::client_journal`，实例是 .cpp 里的文件级单例而不是 mod 类的成员（mod 类的布局不能动，
与输入新鲜度计数器同理）。开关是 BML 属性 `Gameplay`/`SessionJournal`（bool，默认开），**会话开始时读一次**，
所以在菜单里改它是从下一次会话生效；目录 `<游戏>/ModLoader/BMMOJournals`（cwd 是 `Bin`，取其父目录），
文件名再加 `_p<自己的 id>`，只保留最新 10 份，每份上限 256 MB。挂点：

| 时机 | 记录 |
| --- | --- |
| `physics_session_anchor` | header + 每个成员一条 PLAYER + `anchor:` / `start:` NOTE |
| `handle_session_assign` | `assigned: tick base N` / `resync: reassigned, tick base N` NOTE |
| `physics_session_frame` | 自己的 INPUT（发出去之前）、帧末的 TICK |
| `physics_session_apply_queues` | 中转来的 INPUT（RELAYED）、收到的快照（RECEIVED CHECKPOINT） |
| `physics_session_send_event` / `_apply_event` | 自己发的 / 收到的 EVENT |
| 回滚引擎 `on_correction`、hard/blend、resync | CORRECTION（kind 0/1/5/6/7、2/3、4） |
| 每 660 tick、以及每次回滚或 hard 修正后（最密 33 tick 一次） | LOCAL CHECKPOINT |
| `physics_session_request_resync` | `resync: requested (<原因>)` NOTE |
| `physics_session_end_local` | `end: <原因>` NOTE + 关闭 |

客户端 journal 的 EVENT 两个 tick 都是标记 tick：客户端永远不知道服务端在哪个 tick 出队。
房间里后来加入的人，客户端只会从中转输入或收到的事件里第一次看见他，于是补一条 PLAYER 记录 +
一条 `late join: player <id> (<名字>) first seen here; join order <n> is a guess, ...` NOTE
（协议只把加入通知给加入者本人，既有成员拿不到他的 join order；写死一个错的比写明是猜的更糟）；
离开则由 `PlayerLeft` 写一条移除记录，因为服务端会把这个玩家连同刚体和输入状态一起从世界里去掉。

命令：`/mmo journal on|off|status|mark <text>`（自动化通道里是 `journal ...`，`/mmo auto journal ...`
也走同一个分发器，tab 补全已收录）。`on`/`off` 写的就是上面那个 BML 属性，两者是同一个开关，不是两个；
唯一的即时效果是 `off` 会把当下正开着的文件收尾关闭（打这条命令的人就是要它现在停）。
`mark <text>` 写一条 `mark:` NOTE 外加一份 LOCAL CHECKPOINT，不受频率限制——玩家指着的这一刻，
比省下的几 KB 重要。

无头会话客户端 `BallanceMMOSessionClient --journal <file.bmjr>` 写的是同一批记录——包括 `PlayerLeft`
的移除记录：`RoomEvent` 分支上挂 `journal_leave_player()`，写一条 `added = 0` 的 PLAYER 并把这个 id 从
"已 announce" 集合里删掉（他再进来时要重新 announce 一次）。所以端到端测试不需要开原版游戏就能覆盖
客户端格式，包括"这一 tick 之后他不在这个世界里了"这句话。

**离线回放（SimTool）.**

```
BallanceMMOSimTool [--root <game dir>] --replay-session <file.bmjr> [--list] [--ticks N]
    [--from A --to B] [--dump-entity NAME] [--stop-on-divergence] [--journal-trace]
    [--write-journal <out.bmjr>] [--report-every N] [--continue-after-jump]
```

`--list` 是分诊通道：不启动引擎、不需要 `--root`，用 `scan_journal` 流式扫一遍，只累计计数器和
PLAYER/EVENT/NOTE/CORRECTION/CHECKPOINT 行（INPUT 只计数不展开）。实测一份 64 MB / 116 万条记录的
文件，流式 `--list` 峰值 11.8 MB，先 `read_journal` 全量展开则是 157 MB（13 倍），折算到 256 MB
上限约合 45 MB 对 600 MB——分诊本来就是拿到线上文件后的第一步，不该先要一台大内存机器。
回放则：从 header 建 `world_options`，构建 id 只比引擎那一半（不同只警告），
锚点 hash/surfaces 与本机游戏数据比对（不同基本就是游戏数据不对，这是第一个要查的），按文件顺序加入
初始成员，然后每个 tick 组依次 PLAYER 增删 → 每条 INPUT `set_input` → 每条 EVENT `apply_event`
（`lifecycle_event.tick` 取 `event_tick`）→ `world->tick()` → 比 `hash`/`pose`；遇到 FULL checkpoint
逐刚体比位置（double）、旋转、线/角速度。收尾一行
`summary: ticks=N matched=M first_divergence=T checkpoints=C checkpoint_mismatches=D`，
退出码 0 = 没有分叉、3 = 有分叉（含 checkpoint 不符与在 tick 跳变处停下）、1 = 出错、2 = 用法不对。

几条为了不骗人而加的规矩：checkpoint 里的无名机关行（delta 快照不带名字）先用全量快照学到的
索引→名字字典解析，解析不出就记为 `unresolved` 而不参与比较，绝不拿回放自己的编号去撞；
"最差刚体"只在真的有差时才报，缺一个或多一个刚体则直接报 `missing:` / `extra:` 加身份；
tick 号出现空洞（晚加入或 resync 会让客户端重新编号）时停下并说明，`--continue-after-jump` 才继续。
客户端 journal 走同一条路但只是尽力而为：它的 TICK 哈希是它自己预测的世界，不参与比较，
真正比的是 RECEIVED 快照——那是服务端的话；每个 tick 组里的多条 EVENT 先按 join order 重排回服务端的
规则（横幅会说明这一点，服务端 journal 则原样照放）。`--write-journal` 让这次回放写出自己的服务端 kind
journal（事件按重排后的顺序写），于是两个平台的回放可以用 `journal_trace.py --diff` 逐 tick 对，
一份客户端 journal 也可以用它来量"它到底复现了服务端世界的多少 tick"。

**合并时间线（`scripts/journal_trace.py`）.** 纯 Python 3.8+、只用标准库，同一份格式的第二个实现
（两个读者互为校验，实测在截断文件上计数逐字节一致）。

```
journal_trace.py <a.bmjr> [b.bmjr ...] [--log ModLoader.log ...] [--around TICK] [--window 40] [--nonempty]
journal_trace.py <a.bmjr> [...] --list
journal_trace.py <a.bmjr> <b.bmjr> --diff
journal_trace.py --selftest
```

默认输出是一条合并时间线：每个 tick 一块，先是服务端 journal 的输入、事件、NOTE 和
hash/pose/probe，然后每份客户端 journal 自己的 tick 哈希、修正、NOTE、收到的快照，
最后是带该 tick 的日志行（识别 `mismatch at tick N (local M)`、`rollback to tick N`、
`resim tick N`、`physicalized at tick N`、`resync requested (...)`、`tick base N assigned` 等，
其它带 `tick N` 的行也挂到 N 上）。同时打印墙钟映射（header 的 UTC + 每条 TICK 的 `ms`），
玩家说"21:37 左右不对劲"就能落到具体 tick 上。`--log` 认三种时间戳：BMLPlus 的
`[09/04/2026 22:47:06.611]`、ISO 的 `[2026-09-05 04:09:19]`，以及 **BMMO 自己的控制台格式**
`[09-05 04:09:19]`——本地时间、没有年份，服务端和无头会话客户端的日志用的都是它，所以现在服务端自己的
日志也能进这条时间线。年份取自第一份 journal header 的 UTC 时间（按本地时区换算，因为控制台打的是本地
时间），跨年时按月份回退自动 +1；行首还要跳过控制台打印前擦除输入行留下的 `\x1b[0K` 和 `\r`，
否则服务端日志的第一段时间就全丢了。剩下的空白不在解析器这边：无头会话客户端的 `mismatch` /
`rollback` 这些诊断行本身不带时间戳（它的 printer 就不打），所以那份日志只有带戳的行能上钟。
`--diff` 比前两份 journal，规则按这一对的类型走：
两份服务端 journal 的哈希才是同一个世界（`--write-journal` 就是为它准备的），哈希计入退出码；
服务端 + 客户端只共享服务端的快照，那里哈希只作参考、退出码看 checkpoint 比对，
delta 快照缺的刚体只作信息、不算分歧；两份客户端 journal 没有权威的一半，哈希就是全部，
于是又计入退出码——两个玩家 desync 就是这么分诊的。
退出码 3 = 有分歧，1 = 什么都没比上（崩溃或启动失败的文件不该在 CI 里绿着过）。
它的 `--list` 与 SimTool 一样是流式的（同一份 64 MB 文件峰值 10.4 MB，全量展开是 1.07 GB），
输出在 GBK 控制台上也不会被中文房名或 emoji 打断（stdout/stderr 一律 reconfigure 成 UTF-8）。
`--selftest` 用脚本自己的写者造一份含全部 tag 的合成 journal 再读回来。

**实测（2026-09-05，本机 Windows x64 服务端 + 两个无头会话客户端，Level 1）.** 同一天跑了两次真实两人
会话（都是 host 中途退出，服务端与两个客户端各录一份），第二次是加上 join order 排序规则之后重跑的。
下面的数字来自第二次，只有"体积与速率""每 tick 代价""崩溃尾巴"三项是第一次量的（录制路径没变，
第二次的 609,243 B / 5,089 tick = 119.7 B/tick 与第一次的 119.4 对得上）：

- **服务端 journal 离线逐位重放**：`summary: ticks=5088 matched=5088 first_divergence=-1
  checkpoints=8 checkpoint_mismatches=0`，退出码 0（第一次会话同样是 5123/5123、0 处不符）。
  中途退人、两轮 physicalize（两轮都是两个玩家落在同一 tick）、死亡复活、以及三条标记/应用 tick
  分离的 Unphysicalize 全部原样重现。
- **`--write-journal` 往返**：回放写出的 journal 与源文件 `journal_trace.py --diff`
  `hash: matched=5123/5123`、`pose: matched=5123/5123`，`first_divergence=-1`（第一次会话）。
- **客户端 journal（排序规则的成绩单）**：host 那份 1,988 份 RECEIVED 快照 3 处不符，joiner 那份
  2,343 份也是 3 处——两边不符的是**同样的三处**，正是服务端晚一个 tick 才应用的那三条 Unphysicalize
  （tick 2959 / 3752 / 3760，报的都是 `missing: ball of p...`）。把两份的 `--write-journal` 输出与
  服务端 journal 逐 tick 对：host `hash/pose matched=3888/3891`、joiner `matched=4596/4599`，
  两边差的 tick 集合都恰好是 `{2959, 3752, 3760}`。**规则之前**：joiner 那份 376/2,336 处不符，
  世界从 tick 1862（两人同 tick physicalize 的那一刻）起就分叉，之后整段不可用。
- **顺序本身的证据**：joiner 文件里 tick 1862 和 3163 两组都是它自己（p201606466）在前，
  它的回放写出的 journal 里是 p2779083744 在前——与服务端 journal 的顺序一致；服务端 journal 的
  tick 7 那组也是 host 的 8 条在前、joiner 的 8 条在后（按 join order）。
- **`PlayerLeft` 移除记录**：joiner 那份里有 `tick 4405 player -p2779083744`，服务端那份里是
  `tick 4400 player -p2779083744`——两边都记下了"他从这个世界里被去掉了"。
- **体积与速率**：服务端 611,684 B / 14,209 条记录 / 5,123 tick / 77.6 s = 119.4 B/tick、7.9 KB/s、
  两人时 3.9 KB/s/人；客户端 host 355.0 B/tick、23.4 KB/s，joiner 310.5 B/tick、20.5 KB/s
  ——客户端约为服务端的 3 倍，因为它还存每 2 tick 一份 RECEIVED 快照和全部中转输入。
  按这个速率 256 MB 上限约合服务端 9.5 小时、客户端 3.2 小时。
- **每 tick 代价**：回放本身 2.14 ms/tick；同一次回放加上 `--write-journal` 只多 127 ms / 5123 tick
  = **0.025 ms/tick（约 1.1%）**，写的是整份 611 KB、14,208 条记录、8 个全量 checkpoint。
- **崩溃尾巴**：把服务端 journal 截掉最后 2000 字节，`--list` 报
  `read=609655 dropped=29` + `warning: truncated: 29 bytes dropped` 并照常列出头部、成员与
  checkpoint，回放前 200 tick 仍 `matched=200`，Python 读者给出逐字节相同的计数。
- **回归**：98/98 单元测试；`rec_m3b.bmrc` 回放 4169/4169；9.14 的两次爆炸 body-guard 用例开关
  guard 两跑的 224 行 pose 与 tick 610 刚体表完全相同；`--level 2 --spawn-test 3 --spawn-ball 2`
  `summary: ok`。

**原版客户端实测（2026-09-05，本机服务端 + 本机原版客户端，Level 1 与 Level 2 各一场）.** 通过文件命令
通道驱动真机：连接、`level N`、建房、`ready`、`start physics`、按键、`journal mark looks wrong here`、
`journal status`、`/mmo journal status`。Level 1 一场（死亡一次、复活一次）：客户端写出
`ModLoader/BMMOJournals/session_1_level1_…_p2070170140.bmjr`（1.04 MB，3194 tick，48 份 FULL、6 份
LOCAL、1525 份 RECEIVED checkpoint），`--list` 里 mark 落在 tick 1939；服务端那份 `--replay-session`
2963/2963 逐位一致；客户端那份回放 1525 份收到的快照全部一致；`journal_trace.py <服务端> <客户端>
--log ModLoader.log --around 1939` 把 mark、`unphysicalized at tick 2047` 的日志行和两边的 hash 排在同一
屏上。Level 2 一场：服务端 1963/1963 逐位一致；客户端那份回放 986 份快照里 72 份不符，第一处在 tick 272
——服务端回放打印的 `BodyRevived event … was stamped for tick 272`（273 才应用）就是它：一条晚应用的机关
复活事件让回放世界早一个 tick 醒来一个机关，此后一直差着，正是上面第 4 条说的"事件只有标记 tick"。

**黑匣子第一场就抓到的 bug.** Level 1 那场合并时间线里，服务端每条 INPUT 都是 `keys=00`，服务端的球
一直停在出生点，客户端的球却在 `key up down` 之后掉下了平台：客户端 `session` 状态行 `keys_known=0`。
新加的自动化动词 `navgraph`（打印 `read_navigation_graph` 看到的东西）给出原因：这份安装里的其他 mod
（NewBallType/BallSticky 等）往 Ball Navigation 里接了自己的 SetPhysicsForce 叶子，本机看到 6 片叶子，
其中两片（order 39/40，方向 (0,±1,0)，force 1.9）由没有绑键的 Key Event 驱动，`key=0`；而客户端等
"每片叶子都有键"的条件永远不成立，于是整场会话不发任何按键，回滚也从不跟踪自己的球（`rb=0 mism=0`
看起来一切正常）。修法在 `physics_session_client.cpp`：丢掉 `key==0` 的叶子，按原版顺序重新编号，
等到原版四片都拿到键为止（服务端跑的是无 mod 的脚本，本来就只有这四片）。修完同一流程
`keys_known=1`、`4 navigation leaves, keys 200/208/205/203`、`own ball … driven by the navigation
replica`，服务端 journal 里有 215 条 `keys=01` 的输入，回滚跟踪着自己的球（978 份快照 0 处不符）。

**注意事项.**

1. 上限到了就写一条 `cap:` NOTE 然后停止录制（不是截断，也不是无限长）。默认 256 MB，按上面的实测
   速率是服务端约 9.5 小时、客户端约 3.2 小时一份。
2. `journal_dir` 相对服务端自身所在目录解析，不是 cwd（无头引擎会 chdir 进游戏的 `Bin`）；
   部署机上就是 `~/bmmo/journals/`。客户端固定写到 `<游戏>/ModLoader/BMMOJournals`，只留最新 10 份。
3. 跨平台：文件逐字段小端写，Linux x64 服务端录的直接在 Windows 上 `--replay-session`；
   两个平台各自 `--write-journal` 再 `--diff`，就是一次跨平台逐 tick 的一致性检查（9.13 的常备闸门）。
4. **客户端 journal 的回放是尽力而为**，判定权威结论永远看服务端那份。同 tick 事件的顺序不再是其中
   一项：join order 规则两边共用，回放会把客户端写下的组内顺序重排回服务端的规则（曾经的 376 处不符
   就只是这一件事）。剩下的尽力而为有三样：输入是它发出的和服务端中转给它的，不是服务端 `take()`
   真正交给世界的那一帧；事件只有标记 tick，服务端晚应用的那些会差一个 tick（上面实测里剩下的 3 处
   checkpoint 不符全部是它）；晚加入者的 join order 是猜的，NOTE 里写明。客户端 journal 的价值是解释
   "这个客户端看见了什么、为什么回滚"，不是裁判。
5. 录制不改模拟：checkpoint 走只读的 `snapshot_for_journal()`，实测整份 journal 只花 0.025 ms/tick。
   `--list` 是流式的，只有回放才建内存中的分组结构。
6. 真机原版客户端上跑过的：整条录制链路、`journal status|mark`、`/mmo journal status`、单人会话的
   回放与合并时间线（见上面的原版客户端实测）。还只经过编译的：BML 菜单里的 `SessionJournal` 开关、
   10 份轮转、以及原版 mod 那条 `PlayerLeft` 挂点（无头客户端上同名的那条已经在实测会话里写出记录了，
   见上）；`cap:` 与 `resync:` 两条路径也还没在实测会话里触发过。
