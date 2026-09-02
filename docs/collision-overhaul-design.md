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

自动化命令有两条等价通道，命令集相同（`ping`/`status`/`physview`/`physobjs`/`level N`/`key`/`record`/`quit` 等），都在游戏线程的 OnProcess 里派发：

- **命名管道**（`BMMO_COMMAND_PIPE`）：交互式使用，一问一答。
- **命令文件**（`BMMO_COMMAND_FILE`，缺省 `Bin/bmmo_command.txt`）：更稳的回退方式。每帧若文件存在则读取整份、逐行派发、把结果写入同目录 `<file>.out` 和 BML 日志（`CommandFile: <cmd> -> <resp>`），随后删除该文件。派发在 OnProcess 最前面执行，即使后续逻辑抛异常也不受影响，因此既能驱动游戏又能用日志观察是否按预期工作。


客户端 Mod 通过环境变量 `BMMO_COMMAND_PIPE=<name>` 开启命名管道 `\\.\pipe\<name>`，按行接收命令：`mmo <子命令>`（等价于游戏内 `/mmo ...`）、`bml <命令>`（`IBML::ExecuteCommand`）、`level <n>`、`key <名称> <down|up>`、`screenshot <path>`、`quit`、`status`。所有命令在游戏线程执行。测试脚本 `scripts/bmmo_ctl.py` 负责发送。

## 4. 里程碑

1. M0：分支、构建、设计文档、命令通道。
2. M1（已完成）：确定性校验台（客户端固定 tick + 录制；无头世界回放；逐 tick 比对）。结论：四个平台对同一段 Level 1 录制（2345 帧，含开场碎块、键盘操控、死亡重置）全部位级一致，见第 6 节。
3. M2（已完成）：房间系统与影子球会话。房间协议（`room_request`/`room_state`/`room_event`）落地，服务端 `BallanceMMOServer/room/room_manager.hpp` + `server.cpp`，客户端 `/mmo room ...` 命令与 `session/room_client.cpp`；球状态按房间过滤。详见第 7 节。
4. M3：物理会话（多球、tick 协议、预测与修正、机关镜像、分节、死亡、迟到加入、重开）。
5. M4：打磨（host 迁移、重同步、清理、打包）。

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
