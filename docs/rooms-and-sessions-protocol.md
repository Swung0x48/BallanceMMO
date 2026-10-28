# 房间与会话协议（collision-overhaul）

本文定义在旧协议之上追加的房间（Room）与会话（Session）消息。旧消息与 opcode 值完全不变；新 opcode 追加在 `bmmo::opcode` 枚举末尾（`RemoteCommand` 之后），顺序固定：

```
RoomRequest, RoomState, RoomEvent,
SessionStart, SessionEnd, SessionReady, SessionInput, SessionSnapshot, SessionResync, SessionEvent, SessionAssign, SessionRemoteInput, SessionLead, SessionLeadAck
```

所有新消息都是 `serializable_message`（显式小端编码，`std::stringstream raw`），读取端必须检查长度、数量上限和枚举范围；字符串编码为 `u16 长度 + UTF-8 字节`，上限见各字段。

## 1. 房间

### 1.1 生命周期

1. 房主 `Create`（可带名字，≤ 32 字节；空则服务端生成）。创建者自动成为成员与房主。
2. 其他玩家 `List` 查看，`Join <id>` 加入。房间人数上限由服务端配置（默认 8）。
3. 成员 `Ready` / `Unready`。房主本人也需要 `Ready`。
4. 房主 `Start`，可带 `mode`：`Shadow`（影子球，旧逻辑）或 `Physics`（物理会话）。服务端校验：全员 Ready、全员在同一张地图（`current_map` 一致）、物理会话时 Mod 白名单通过且服务端物理模拟可用。校验失败返回 `RequestDenied` 与错误码。
5. 会话进行中房间 phase 为 `Running`；会话结束（房主 `Close`、房主发起 `End`、所有成员离开或服务端失败）回到 `Lobby` 或销毁。
6. 房主断线：按加入顺序把房主转给下一名成员（`HostChanged`）。最后一名成员离开时销毁房间（`RoomClosed`）。
7. `Kick <player>` 仅房主可用。被踢者收到 `Kicked`。
8. 断线即视为 `Leave`。

### 1.2 可见性

- 房间内的成员之间按旧协议互相可见（球状态、聊天、倒计时等）。
- 处于房间中的玩家与房间外玩家之间不转发球状态（服务端在转发 `OwnedBallState*` 时按房间过滤）；聊天保持全局。
- 房间外的玩家彼此之间行为与旧版完全相同。

### 1.3 消息

`room_request_msg`（client → server，reliable）

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| action | u8 | `List=0, Create=1, Join=2, Leave=3, Ready=4, Unready=5, Start=6, Kick=7, Close=8` |
| room | u32 | 目标房间；`Create`/`List` 时为 0 |
| name | string ≤ 32 | 仅 `Create` |
| target | u32 | 仅 `Kick`：被踢玩家 id |
| mode | u8 | 仅 `Start`：`Shadow=0, Physics=1` |

`room_state_msg`（server → client，reliable；房间列表或本房间变化时推送给相关玩家）

| 字段 | 类型 |
| --- | --- |
| own_room | u32（0 = 不在房间） |
| rooms | u16 count，每项：id u32、name string、host u32、member_count u16、capacity u16、phase u8（`Lobby=0, Running=1`）、mode u8 |
| members | u16 count（仅本房间），每项：id u32、name string ≤ 64、ready u8、is_host u8、map（type u8、level i32、md5[16]） |

`room_event_msg`（server → client，reliable）

| 字段 | 类型 |
| --- | --- |
| type | u8：`RequestAccepted=0, RequestDenied=1, PlayerJoined=2, PlayerLeft=3, HostChanged=4, Kicked=5, RoomClosed=6, ReadyChanged=7, SessionStarting=8, SessionEnded=9` |
| error | u8：`None=0, NotFound, Full, AlreadyInRoom, NotInRoom, NotHost, NotReady, MapMismatch, ModMismatch, PhysicsUnavailable, InvalidName, ServerBusy, Unsupported` |
| room | u32 |
| actor | u32（发起者） |
| subject | u32（被操作者，可为 0） |
| reason | string ≤ 256 |

每一条 `room_request` 都恰好收到一条 `RequestAccepted` 或 `RequestDenied`（含 `List`、`Ready`/`Unready`、`Close`）。连接可靠且有序，客户端据此把结果对应回发出的子命令并回显；`RequestDenied` 的 `error`（必要时加 `reason`）说明失败原因。客户端等结果最多 5 秒：超时就报 `Error: the server did not answer "/mmo room <sub>".` 并把这一条丢掉，免得不回结果的服务端（早于本节的版本）让命令悄无声息，又把结果错配给后面的命令。

一次操作可能同时产生广播事件：服务端先发给发起者结果，再把 `PlayerJoined` / `PlayerLeft` / `HostChanged` / `RoomClosed` / `SessionStarting` 发给其他成员，最后推送 `room_state`。`Ready`/`Unready` 相反——先推 `room_state`（其中已带新的 ready 标记），再发 `ReadyChanged`，最后回结果，这样收到事件的客户端可以直接从名单里读出新状态与人数。客户端对自己发起的操作只显示结果那一条，不重复显示随之而来的广播事件。

客户端命令：`/mmo room list|create [name]|join <id>|leave|ready [on|off]|start [physics|shadow]|kick <player>|close|status|session`。

## 2. 会话（物理模式）

影子球会话不需要下面的消息：`SessionStarting` 后各客户端继续走旧逻辑。物理会话的实施设计见 `docs/collision-overhaul-design.md` 第 8 节；本节只定义线上格式。新 opcode 顺序（追加在 `RoomEvent` 之后）：

```
SessionStart, SessionEnd, SessionReady, SessionInput, SessionSnapshot, SessionResync, SessionEvent, SessionAssign, SessionRemoteInput, SessionLead, SessionLeadAck
```

### 2.1 时间线

- `tick` 长度 1/66 s。每个客户端在收到 `SessionStart` 后先播放 3 秒 “3 - 2 - 1 - Go!” 倒计时（沿用 `countdown_msg` 的提示音与提示行，纯本地效果，不参与确定性），在 “Go!” 这一帧重开当前关卡，`Gameplay_Ingame` 首次激活的行为帧为锚点，锚点的编号由服务端在 `SessionAssign` 里给出：会话开始时在场的成员用**会话起始基数**（见 2.2 的公式），迟到加入者与重同步者用服务端当前 tick 加上同一个前导量。锚点执行会话重置（IVP 时钟归零、`ivp_srand(seed)`），此后每个行为帧一个 tick。
- 客户端每 tick 发送 `SessionInput`；服务端在收齐所有成员该 tick 的输入、或墙钟超过 `开始时刻 + (tick + input_delay)/66 s` 时模拟该 tick，缺失输入沿用该玩家上一 tick 的输入。
- `input_delay` 是**每个会话**自己的，在会话开始时定下并写进该会话的 `SessionStart`：取成员里最差的一条链路（GNS 的 `m_nPing`，且用该连接上见过的峰值而不是当下值），按 `单程 × 1.5 + 16 ms` 折成 tick，再夹在 `physics.input_delay`（下限）与 40 tick（上限）之间。窗口装的是**单程**——客户端按自己的时钟跑在服务端调度之前，输入只需单向抵达；乘 1.5 是因为决定输入迟不迟到的是抖动而不是均值，而 RTT 本身已经是平滑过的数。定下之后整场不变：调度的截止时刻是从会话开始时刻带着这个值算出来的，迟到加入者不能把已经在跑的成员的时间线挪走。
- 服务端每 `snapshot_interval` 个 tick 广播一次 `SessionSnapshot`（不可靠）；每 66 tick 或刚体集合变化时广播 full 快照（可靠，携带机关名字典）。
- 生命周期事件（`SessionEvent`）可靠传输，带发生的客户端 tick（**origin**）；**协议 9 起**服务端在模拟线程上**接受**它时定下应用 tick `applied = max(origin, N)`（N = 该会话下一个要模拟的 tick），恰好在 `applied` 那一步应用，并从接受处转发给**全部**成员（含发送者本人，`player` 字段为来源），`tick = applied`、`origin_tick = origin`。按时到达的事件 `applied == origin`，与以前一样；迟到的事件不再 “late by N” 地只在服务端晚应用，而是各端都在 `applied` 应用（迟到玩家设计 3.1）。
- **分节是整场会话的输入**（协议 3，`session/sector_union.hpp`）：每一端——服务端世界、每个客户端——都运行**所有成员到过的分节的并集**，且每个分节在每一端都于同一 tick 启动。成员跨过检查点时把 `Sector` 事件盖上 `当前步 + 1 + sector_lead` 的 tick 立即发出（原版链里跑 `Gameplay_SectorManager` 的那一步被扣住，见中和 N3），服务端在该 tick 应用并**不经 Nagle** 转发，其他成员收到后同样在该 tick 应用；随后启动该分节的那一遍管理器运行（`IngameParameter[0,2]=0`、`[0,1]=分节`、`scene->Activate`）在各端按同一节拍规则跑（`session/sector_pacer.hpp`：管理器空闲 3 tick 后一遍，最小的待启动分节先）。**任何一端都不再反激活任何分节**，服务端也不：并集只增不减。迟到加入者与重同步者在各成员最近的 Physicalize 之后补收本场会话的全部 `Sector` 事件。
- `sector_lead` 同样是每个会话自己的，与 `input_delay` 出自同一次 RTT 测量并写进 `SessionStart`（`session::sector_lead_for_ping`）：`input_delay`（到服务端那一程的单程预算）+ 1 tick（客户端只能在它的**下一步**之前跑管理器）+ 再一份按同一 RTT 折算的单程预算（服务端转发到其他成员那一程）。局域网 9 tick，100 ms RTT 约 15，300 ms 约 33。太短则转发晚于该 tick 到达，那名成员的这个分节从此错相且回滚修不回来（回滚无法重放分节的初始化）；太长只是机关在过线后晚一点启动。

### 2.2 消息

所有多字节整数小端，浮点为 IEEE-754 单/双精度按字节原样，向量为 `x,y,z`，四元数为 `x,y,z,w`。

`session_start_msg`（server → client，reliable）

| 字段 | 类型 |
| --- | --- |
| room | u32 |
| session | u32（每次 Start 递增） |
| mode | u8 |
| map | type u8、level i32、md5[16] |
| tick_rate | u8（66） |
| snapshot_interval | u8 |
| input_delay | u8 |
| sector_lead | u8（成员给自己 `Sector` 事件盖的提前量，tick；见 2.1） |
| first_tick | u32（接收者锚点 tick 的编号；实际编号随 `SessionAssign` 到达，两端客户端都不读这里，服务端写 0） |
| seed | i32 |
| spawn_impulse | f32（每次出生 Physicalize 的踢出速度 m/s，0 = 无；写在 seed 之后） |
| world_options | u32（协议 11：会话的世界选项 `session::WORLD_*`，写在 protocol 之后、players 之前，整个会话固定、迟到加入者相同；bit 1 `WORLD_TRAFO_INPUTS` = 变球作为输入，由服务端配置 `physics.trafo_inputs` 决定，默认关；协议 12 起 bit 0 `WORLD_PLACEMENT_CLONES` = 小节摆放：PH 表中每一行都在锚点得到自己的对象（docs/sector-placement-design.md），由服务端配置 `physics.placement_clones` 决定；步骤 S5 到 S10 默认关（零售核心宿主还不能建副本），S10 起默认开；协议 16 起 bit 2 `WORLD_PROXIMITY_UNION` = 邻近并集：每个世界按同一条规则放置机关的邻近探针与球身份读取，由服务端配置 `physics.proximity_union` 决定，G3 起默认开，见下文协议 16） |
| players | u8 count，每项：id u32、join_order u8、ball_type u8、spawn_position f32×3、spawn_rotation f32×4（实体世界矩阵位姿，CK 侧为单精度；这是原版复活点本身，每个成员都相同——设计 9.10 去掉了出生环偏移） |

`session_ready_msg`（client → server，reliable）：session u32、first_tick u32、anchor_hash u64（锚点世界的可动 core 位姿哈希，即 `world_hash::pose`，每个 core 各自哈希后无序求和，与刚体列表顺序无关；不含物理时间因子等时钟派生量，因为客户端重开关卡会比新加载早一帧设置时间因子）、anchor_surfaces u64（碰撞表面签名）、physics_sha256 string ≤ 64、build_id string ≤ 64；protocol u32（客户端的 `session::PHYSICS_PROTOCOL_VERSION`，当前为 16）紧跟 first_tick 之后，是必填字段，缺了整条消息不解析；caps u32（协议 8 起，放在 build_id 之后，`session::CAP_*`：bit0 `CAP_LEAD` = 客户端会执行 `SessionLead` 并回 `SessionLeadAck`；bit 3 `CAP_PLACEMENT` 见协议 12；协议 12 起 caps 之后还有 anchor_placement u64）——protocol < 8 的消息不带它，照样解析（caps 0），好让服务端说出版本不符。服务端先比 protocol：不等于自己的就 `SessionEnd` 并写明两边版本——新旧构建不得同场。再把哈希与自己锚点的值比较（迟到加入者除外）；不一致则 `SessionEnd` 并给出原因。同时比 `build_id` 的引擎半部（`ballanced-<rev>`）：不同则同样结束会话，任一边为 `unknown` 时不比。`session_start_msg` 在 spawn_impulse 之后同样带服务端的 protocol u32（必填）：客户端不等则拒绝开始会话并提示更新，解析不出来（旧服务端）也提示。

`session_input_msg`（client → server，unreliable no-delay）：session u32、first_tick u32、count u8（≤ 8，从 first_tick 起连续 count 个 tick，最新的在最后），每项：keys u8（bit0 叶子 0 … bit3 叶子 3，叶子编号按 `Ball Navigation` 图内 `SetPhysicsForce` 的子块顺序；bit4 Shift、bit5 Space 仅作记录）、cam_right f32×3、cam_up f32×3、cam_dir f32×3（`Cam_OrientRef` 世界矩阵的三条基向量）、ball_type u8、flags u8（bit0 physicalized、bit1 paused、bit2 nav_active——客户端 BallNav activate/deactivate 的当前状态，服务端据此复现 Key Event 的 On/Off；bit1 仅作记录，会话期间暂停不再停摆世界，见设计 9.2）。

`session_event_msg`（双向，reliable）：session u32、player u32（服务端转发时为来源，客户端发送时为 0）、tick u32（协议 9：服务端转发时为应用 tick，客户端发送时为戳记）、type u8、按 type 附加（见下表），**之后**（协议 9）origin_tick u32（发送者的戳记；客户端发送时等于 tick）、state u8（`session::EVENT_STATE_*`，只有服务端置位：bit0 MOVED = tick > origin_tick，服务端晚于戳记应用；bit1 SUPERSEDED = 被同一玩家更新的 P/U 取代的出生，从不应用，只发给发送者本人；bit2 REJECTED = 服务端拒收，只发给发送者本人，plan step 18 起发送）。这两个字段放在末尾，所以协议 8 的报文对协议 9 的读方就是截短的报文，任何 type 都解析不过；读方还拒收 origin_tick 晚于 tick 的报文（重新盖戳只会往后挪）和未定义的 state 位。 **协议 6 起**：两个方向都仍先 reliable（NoNagle）发一份，随后由发送方不可靠重发 6 份、每隔一个 tick 一份（跨 12 tick）（服务端在 `on_session_inputs` 里对仍在场的成员发，客户端在每帧发输入的地方发；`session/lifecycle.hpp` `event_repeats`）；接收方以 (tick, player, type, ball_type, sector, name) 为键只取首达的一份（`event_seen`：服务端按连接在限速之前去重，无头客户端在入队前，零售 mod 在排空队列时），其余忽略。这样一次丢包不再把事件拖到 ~400 ms 的重传之后（超出回滚 48 tick 的可达范围）；事件仍然按戳记应用：服务端按 tick + 入场顺序，无头客户端按戳记排队、迟到则回滚，零售 mod 把一批按 (tick, 入场顺序) 排序后应用，并丢弃比该玩家已应用的生命周期事件更旧的 Physicalize/Unphysicalize（它只能到达即应用、不能回滚）。

| type | 附加字段 |
| --- | --- |
| Physicalize = 0 | ball_type u8、flags u8（bit0 = spawn：在 `CurrentLevel[0,3]` 复活点 Physicalize，即出生或复活；紧跟 ball_type 之后，设计 9.10）、position f32×3、rotation f32×9（实体世界矩阵的三行旋转部分 right/up/dir，按位传输，服务端用它原样重建矩阵后 Physicalize，避免四元数往返的舍入）、recipe：fixed u8、friction f32、elasticity f32、mass f32、start_frozen u8、enable_collision u8、calc_mass_center u8、linear_damp f32、rot_damp f32、mass_center f32×3、collision_surface string ≤ 64、convex_count u8 + 每项网格名 string ≤ 64、ball_count u8 + 每项 center f32×3、radius f32、concave_count u8 + 每项网格名 string ≤ 64 |
| Unphysicalize = 1 | 无 |
| Sector = 2 | sector i32 |
| Finish = 3 | 无 |
| BodyRevived = 4 | name string ≤ 64（协议 16 起在 `WORLD_PROXIMITY_UNION` 下退役：客户端不再发送，服务端收到即丢弃，见下文协议 16） |
| Trafo = 5 | 协议 11：ball_type u8（碎片类型 0 木 1 石 2 纸，即变球前的球型）、flags u8（`TRAFO_RESET` bit0、`TRAFO_SERVER` bit1、`TRAFO_FINAL` bit2，后两位只由服务端设置）、position f32×3（爆炸的参照系平移；重置为 0） |

**协议 12（小节摆放设计 2.4、5，步骤 S5）：摆放签名。** `session_ready_msg` 在 caps 之后加 anchor_placement u64（协议 ≥ 12 才读；更旧的消息照样解析为 0）：客户端世界在锚点的 PH 表摆放签名（`game/sector_placement.hpp` signature，摆放开或关都算）。宿主能算签名时 caps 带 `CAP_PLACEMENT`（bit 3），服务端在引擎比较之后把它与自己世界的签名比较——迟到加入者也比（签名与时间无关）——不一致即结束会话（"placement mismatch for X (client …, server …)"）。不带该 cap 的成员发 0、不比较；会话的 world_options 有 `WORLD_PLACEMENT_CLONES` 时该成员被移出会话（不移出房间），与缺 `CAP_TRAFO_INPUTS` 相同。journal 升到 v6：HEADER 末尾（world_options 之后）加 anchor_placement u64（服务端总是写；客户端写它在 SessionReady 里发的值；更旧的文件读作 0 = 未知、不比较），客户端 HEADER 从此也写 world_options；SimTool `--replay-session` 按 HEADER 的 world_options 建世界，签名非 0 且与重放世界不同则拒绝重放。

协议 12 的各端（步骤 S6、S10 之后）：服务端世界、无头客户端与零售 Mod 的核心宿主（session core on）都在锚点建副本（选项开时）并声明 `CAP_PLACEMENT`、上报签名；是否建副本由客户端内核决定（能力 + 会话的 `WORLD_PLACEMENT_CLONES`），宿主不自己读位。选项关而关卡里还留着副本时，宿主先释放，释放不掉就不锚定、在本地结束会话；建副本失败（全有或全无）同样在发 SessionReady 之前本地结束。旧的零售路径（session core off）不声明该能力：会话带 `WORLD_PLACEMENT_CLONES` 时，它照常先发一条 caps 为 0 的 SessionReady，再在开始倒计时之前本地结束会话（"the placement needs the session core (session core on)"），服务端因此在同一秒就把它移出会话，其他成员不必等 8 s 的起始屏障。各关卡的签名、副本数与实测见 `docs/sector-placement-design.md` §12。

**协议 14、15（步骤 BALA、SPGH2，用户 2026-10-28 的飞船规则与出生规则）**：本文尚未展开，以 `entity/session.hpp` 中 `PHYSICS_PROTOCOL_VERSION` 的注释为准——14 加了飞船区的配对规则与`SessionGhostPose`（opcode 追加在 `SessionResyncState` 之后，`message/session_ghost_pose_msg.hpp`），15 加了出生区的配对规则（`PHYSICALIZE_FLAG_SPAWN`）与 savestate 格式 3，消息布局不变。

**协议 16（世界一致设计 2.2-2.6、5，步骤 G3 与 G5，同一版本）：邻近并集与 BodyRevived 退役。** 世界选项 bit 2 `WORLD_PROXIMITY_UNION`（服务端配置 `physics.proximity_union`，G3 起默认开）打开时，每个世界——服务端世界与每个客户端——都在锚点把机关脚本里 ObjectA 为 `Ball_Pos_Frame` 的每个 `TT Scaleable Proximity` 改接到私有 frame `BMMO_Prox_<owner>_<n>`，把同一脚本里读 `CurrentLevel[0,ActiveBall]` 的 `Get Cell` 改接到私有副本 `CurrentLevel#BMMOId_<owner>_<n>`（`game/proximity_union.hpp`，n 是块在根脚本先序遍历中的位置，名字在每个世界相同；服务端改接全部类，客户端只改机关类）；每个 tick 在生命周期变化之前（服务端：离开者与事件之前；客户端：内核的第 7b 步，实时 tick 与每个回放 tick 都一样）按块自己的度量把 frame 放到最近的有刚体的球上（本人的球或镜像球，`session/proximity_rules.hpp`，距离相同取玩家 id 小的），没有候选时 frame 原地保持；身份副本跟随它的门控探针的选择。客户端的脚本记录升到 v6（hold 段，bit 2），重同步的可移植记录带 hold 段（版本 3），服务端的 hold 按名字随 `SessionResyncState` 到达。`session_ready_msg` 在 anchor_placement 之后加 anchor_probes u64（协议 ≥ 16 才读，总是写）：客户端世界在锚点的机关类探针签名（`proximity_union::signature`，选项关时为 0）；服务端（选项开时）在摆放签名之后把它与自己世界的机关类签名比较，迟到加入者也比，不一致即把该成员移出会话（不移出房间，其他成员继续）："probe mismatch (client …, server …)"。同一选项下客户端不再发 BodyRevived（每个世界自己唤醒自己的机关）；服务端收到的 BodyRevived 在去重之后直接丢弃——不提交、不转发，按成员计数，第一条记日志并写一条 journal NOTE——服务端世界与 SimTool `--replay-session` 也不应用它。journal 布局不变（HEADER 的 world_options 带这一位）。选项关时一切照旧（反向对照与旧 journal 的重放）。各关卡的签名与实测见 `docs/world-agreement-gaps-design.md` §12。

**协议 11（变球复制设计 2.2、2.14，步骤 T2）：变球是锁步输入。** 世界选项 `WORLD_TRAFO_INPUTS` 打开时，每个成员在每个世界里都有自己的一套碎片（`BMMO_TrafoOwner_<id>` 拥有的 8 个脚本），变球的爆炸与重置作为 `Trafo` 事件经服务端按 (玩家, 碎片类型) 的台账（`session/trafo_input.hpp` `trafo_acceptance`）接受：同一类型的爆炸与重置不在同一 tick，下一次爆炸至少在上次重置后 140 tick；仍有碎片的类型再次爆炸时服务端先插入自己的重置；比最新爆炸的 origin 更旧的重置被取代（只回 SUPERSEDED 给发送者）；爆炸后 1320 tick（20 s）内没有重置的，服务端在下一个 tick + sector_lead 够到期限时排入自己的重置（`TRAFO_SERVER`）；离开者每种类型都得到一个 `TRAFO_RESET|TRAFO_SERVER|TRAFO_FINAL`。校验只用静态关卡数据：爆炸位置距关卡 PH 表中每个 `P_Trafo_*` 行（以及当前的 `P_Trafo_*` 实体）都超过 `kTrafoReach`（4.5 m）即拒绝——不应用、只给发送者回 REJECTED、日志按“可能作弊或数据损坏”记下玩家、tick、位置、最近的变球器及距离、球型与会话。`SessionReady` 的 caps 必须带 `CAP_TRAFO_INPUTS`（bit 2），否则该成员被移出会话（不移出房间）。`SessionResyncState` 末尾带台账列表 `trafo_ledger`：服务端世界在 S 时持有的每一套碎片的三种类型各一项（从未爆炸过的成员的套、离开者在最终重置之后到退役之前的套都在内）（u16 个数，每项 player u32、type u8、位 u8（有爆炸/有碎片/有重置/已最终重置）、explode_applied u32、position f32×3、reset_applied u32、final_applied u32）。加入者与重同步成员随状态还会重收应用 tick ≥ 基准的 `Trafo` 事件（台账只覆盖 ≤ S）。超过速率上限的 `Trafo` 事件也回 REJECTED；重置的 position 在接收时清零后再转发。服务端 journal 升到 v5：HEADER 末尾加 world_options u32，`Trafo` 事件用现有的 ball_type/flags/position 字段。

**协议 9（迟到玩家设计 3.1-3.2）：迟到的生命周期事件在接受处重新盖戳。** 服务端收到事件（按 (origin, player, type, ball_type, sector, name) 去重、校验）后不再在网络线程上立即转发，而是把它交给模拟线程（`session_runner::submit_event`）。模拟线程上会话的下一个 tick N 是确定的，在那里定下 `applied = max(origin, N)` 并按 `applied` 入队；同一 tick 的事件按 (入场顺序, origin, 类型序) 应用，Unphysicalize 的守卫（比该玩家最近一次已应用的 Physicalize 的 origin 不新则丢弃）也比较 origin；传给世界的事件 tick 是 `applied`，所以出生的踢出方向序号取自 `applied`（各端一致）。接受时还有一条 **superseded 规则**：每个玩家记住最近接受的 16 个 Physicalize/Unphysicalize 的 (origin, applied)（按个数而不是按 128 tick 的年龄限界：重传晚到几秒的旧出生也照样被取代）；一个 Physicalize 的 origin 比其中某个更旧、而它的 applied 又会落在那个事件的 applied 之后时，它被取代——不入队、不转给别人，只给发送者回一份带 SUPERSEDED 的回显（服务端 journal 里只记一条 NOTE）；仍能落在更新事件之前（或同一 tick，按 origin 排在前面）的旧出生照常应用。接受后由回调 `on_event_accepted` 在模拟线程上转发：给**每个**成员（含发送者：它收到的是回显，`applied == origin` 时就是一次确认）先 reliable（NoNagle）一份，再按协议 6 不可靠重发 6 份、每隔一个 tick 一份；BodyRevived 不回显给发送者。迟到加入者补发用的各成员最近的 Physicalize 与 `Sector` 历史也在接受处更新（按 origin；一个 origin 比该成员已接受的最新 Unphysicalize 更旧的出生不再记为“当前的球”）。离开者的 Unphysicalize 由 `remove_player` 在模拟线程上按会话自己的下一个 tick 产生（若该玩家还有已接受未应用的出生，则排在它之后，origin 比最新出生新），不再从网络线程估计；离开者若还有已接受、已转发但未应用的事件（提前盖戳的出生、`Sector` 等），服务端不再丢弃它们，而是把该玩家留在世界里（用最后一次应用、也就是最后一次转发的输入帧驱动，不转发），在这些事件与它的 Unphysicalize（此时作为普通 EVENT 入队）都应用之后的下一个 tick 才移除（journal 的 PLAYER 记录在那个 tick），与各接收方一致。服务端每个转发的事件打一行 `S5 event player=… type=… origin=… applied=… late=… delay_us=… delay_ticks=… to=…`（从收到到发出的延迟，plan step 17 的门槛是其 p99 ≤ 2 tick）。

各接收方都按 origin 去重、排序和守卫：去重键 `lifecycle_event_key` 为 (origin, applied, player, type, ball_type, sector, name)——一次断线里两次同类型的变形（木 → 石 → 木）被重新盖到同一个 tick 也不会撞键；无头客户端一个 tick 内按 (入场顺序, applied, origin, 类型序) 应用，并且一个 origin 比该远端已应用的 Unphysicalize 更旧的出生晚到时：applied 不早于那个死亡的 applied 就丢弃（两者同 tick 不留球），更早则照常在回滚重放里创建、随后把那个死亡再应用一次并为它回滚（状态行 `overtaken=`）；零售 mod 一批按 (applied, 入场顺序, origin, 类型序) 应用，丢弃 origin 比该玩家已应用的生命周期事件更旧的 P/U；两端的 “Unphysicalize 取了已被替换的球” 守卫都比较 origin。自己事件的回显：`applied == origin` 时忽略；MOVED/SUPERSEDED/REJECTED 目前只计数（无头客户端状态行 `echo=acked/moved/superseded/rejected`，零售 mod 的 `session` 状态行 `events=sent/received/dups/superseded echo=acked/moved/superseded/rejected`，并记日志），自己的事件仍按自己的戳记应用——把自己的出生挪到服务端的 tick 是 plan step 19（无头）与 24（零售）。会话日志（journal）升到版本 3：EVENT 记录在 event_tick 之后追加 origin_tick u32 与 state u8；服务端 journal 里 tick 与 event_tick 都是 applied，origin_tick 是戳记；版本 2 的文件仍可读（origin 取其戳记）。SimTool 重放客户端 journal 时一个 tick 内按 (入场顺序, origin, 类型序) 排序，重放服务端 journal 按文件顺序（即服务端的应用顺序）在 applied 应用。

生命周期屏障（输入帧声称有球而世界里没有、也没有排队的 Physicalize 时，整个会话最多等 1 秒）已在 plan step 20 删除，配置项 `physics.lifecycle_barrier` 一并去掉：迟到的事件在接受处重新盖戳、各端在同一个 applied tick 应用，世界本身从不读 `INPUT_FLAG_PHYSICALIZED`，所以这样的 tick 直接按“没有那个球”步进，谁也不等。剩下的只是诊断（S4）：某成员的帧连续若干 tick 声称有球而世界里没有，这一段在结束时打一行 `S4 bodiless player=… ticks=A..B n=…`，仍未结束到第 66 tick 时再打一行带 ` open` 的（丢失的出生永远不会结束），每会话最多 50 行，同时在服务端 journal 里各记一条 NOTE；`describe` 行里是 `bodiless=段数/未结束段数/短段数`。出生的戳记比第一次声称有球的帧晚至多 `input_delay + 1` tick，所以几 tick 的短段是正常的，长度才说明事件迟到或丢失：不超过 `input_delay + 1` tick 的段只计数、不打日志也不记 NOTE，50 行的上限只留给长段；` open` 行在段长达到或超过 66 tick 时打一次（某些 tick 里该成员不在已应用输入中、段长跳过 66 也不会漏）。

自己的出生等回显的期限（plan step 20）：无头客户端的一个出生在服务端的 tick 超过其戳记 192 tick 仍无回显时视为丢失（没到服务端，或者晚到会超出回放范围），先把这个服务端没有的球从世界和记录里拿掉，再只为自己请求一次重同步。服务端的 tick 取自中继报头（`session_remote_input_msg::tick`，每个 tick 每个数据报都有，与快照开不开无关），快照开着时取两者较新的；第 19 步只看快照，快照关掉时这个球就永远留在拥有者那里。期限只在报头的 `acked`（服务端最新新鲜采用的本方帧）到达或越过该出生的戳记之后才开始算，并且从第一次看到这一点时的服务端 tick 起再给 66 tick（`session/lifecycle.hpp` 的 `own_echo_deadline`）：服务端的 tick 随下行走，上行断开超过期限时可靠的 Physicalize 还排在本地，若此时放弃，这个副本稍后到达、被回显为已创建，会再触发一次重同步。零售 mod 还没有这个期限（plan step 24）。

`session_snapshot_msg`（server → client，unreliable；full 版本 reliable）：session u32、tick u32、full u8、acked_input_tick u32、bodies u16 count，每项：kind u8（`Ball=0, Mechanism=1`）、owner u32（球：玩家 id；机关：字典索引）、name string ≤ 64（仅 full 且 kind=Mechanism，其余为空串）、position f64×3、rotation f64×4、linear f32×3、angular f32×3、flags u8（bit0 simulated、bit1 collision enabled）。位置与四元数用双精度，因为 IVP core 的位置/四元数本身是双精度（速度是单精度），镜像/修正要按位写回。

`session_assign_msg`（server → client，reliable；会话开始时在场的成员在全员 `session_ready_msg` 到齐后**一起**收到 `first_tick = 会话起始基数 = input_delay + min(ceil(最差成员 RTT_ms × 66 / 1000), 12) + 2`，再夹到 `44 − input_delay`（44 = `rollback.hpp` 的 `max_resim_ticks` 48 减 4 tick 余量，下限 1，`sim/late_tick.hpp` 的 `session_start_tick_base`；起始滞后是基数加 `input_delay`，所以基数必须让这个和留在回滚窗口内），迟到加入者即时收到 `first_tick = 服务端当前 tick + 同一前导量`（未夹紧），重同步者收到 `服务端当前 tick + 会话起始基数的公式`（同一前导量再夹到 `44 − input_delay`，plan step 20：让重同步者的中继滞后回到开局时的水平——lp19b_a6_1500 里未夹紧的 31 让它稳定在 27-28，开局成员是 20；`physics.lead_adjust` 开且客户端声明 CAP_LEAD 时仍按迟到玩家设计 2.8 的 `L_eff`）。重同步或迟到加入把该成员的输入游标挪到基数时，跳过的那几个 tick（服务端当前 tick 到基数 − 1）不再等截止时刻：不开 patience 时步进判断把它们和 lead reset 的冻结区间一样当作“已定”（`input_buffer::reanchored`，仍要求至少有一个成员的真实帧在场）。以前整个会话要在这 31 个 tick 上逐个等到截止时刻，lp19b_a6_1500 里所有成员因此落后 22 tick（337 ms 的 TICK 间隙），其余成员的中继滞后 20 → 42，重同步者 48-49，正好顶到回放范围，导致第二次重同步；`session_resync_msg{session, last_full_tick}`（client → server，reliable）由客户端在节拍原点重设、连续 3 次硬置或连续 30 个快照对不上历史时发出，服务端按迟到加入处理：重新分配编号、重发各成员最近的 Physicalize、强制一次全量快照；客户端收到新分配后清空历史，用下一个全量快照一次写入全部刚体）：session u32、first_tick u32——客户端锚点帧对应的服务端 tick 编号（服务端世界自己的编号仍从 0 开始，这个基数只是客户端看到的号）：会话开始时在场的成员为会话起始基数（世界此时还没 tick 过，“当前 tick”项因此为 0），迟到加入者为服务端收到 Ready 时的当前 tick 加同一个前导量。给的是前导量而不是服务端当前 tick，是因为这条可靠消息要过半个 RTT 才到：按当前 tick 分配会让客户端落后半个 RTT，输入赶不上调度截止时刻（首次联调 1380/1380 全 unmatched 正是如此）。客户端收到前不发送输入（缓存），收到后按 `first_tick + 锚点后经过的帧数` 编号，旧编号的缓存帧不再补发、直接丢弃。会话开始也用同一个前导量，是因为锚点是各自关卡加载完成的时刻，成员之间可以相差数秒；不重新编号的成员会一直按自己的锚点编号跑在服务端前面，快照与本地历史的差值（编号偏移 + input_delay + 单程）随加载差增长，超过 `rollback.hpp` 的 `max_resim_ticks = 48` 就变成 too_far → 重同步抖动。

`session_remote_input_msg`（server → client，unreliable no-delay，每模拟一个 tick 发一条，M4 设计 9.1；协议 5 起带冗余窗口，`session/input_relay.hpp`；协议 7 起带收件人自己的被替换帧，见下表）：session u32、tick u32（这条里最新的 tick）、count u8、acked u32、held_total u32、epoch u32（协议 7）、wait u8（协议 13，见下）、count × {player u32、nframes u8（1..`MAX_RELAY_FRAMES` = 32，对应 tick−nframes+1 .. tick，旧的在前）、nframes × 帧}；每帧：header u8（bit0 `FRAME_CAMERA`：后面跟 cam_right/cam_up/cam_dir f32×3）、keys u8、ball_type u8、flags u8、[相机 36 B]。每个成员的第一帧必带相机（读到不带的即拒收），之后的帧与前一帧的三个向量逐位相同时省掉，读方从前一帧复制——每条报文自成一体，不依赖别的报文到达；相机不动时一帧 4 B。一条报文装不超过 1100 B（一个包）的整项，装不下就分成几条同 tick 的报文（GNS 对跨包的 unreliable 报文丢一片即整条丢，超过 16.5 KB 的会悄悄改成 reliable 发）；一项单独放不进一个包时丢掉最旧的帧。内容是服务端在最近 `relay_window`（服务端配置，默认 8，钳到 1..16：input_delay + 一个单程 + 窗口要留在回滚可达的 48 tick 以内）个 tick **实际采用**的每个其他成员的输入帧（新鲜的或沿用上一 tick 的；只带最新 tick 在场的成员，取它连续在场的那一段），去掉了收件成员自己。客户端按 tick 重组（`relay_seen` 保证同一 (player, tick) 只记一次账、只校正一次；`relay_store` 进镜像的输入环；镜像还没建的玩家进 `pending_relay`，Physicalize 时并入），用它驱动远端球的本地导航复制（桥接 API v3 `navigation_*`），快照只做校正；一帧只有 `relay_window` 条报文连续丢失才真正丢；这样的洞（前后都有帧）重放时用最近的前一帧，还没到的尾部沿用记录的预测；到得太晚（超过 48 tick 或超出历史）的帧不再回滚，计入状态行的 `relay=.../late`，`lag=max/p99` 是首达帧的滞后。零售 mod 做同样的重组、去重、暂存、记账和预测推进，但校正仍只来自快照（无头客户端的输入校正回滚尚未移植）。2026-10-19 到 10-21 这条流曾改成 reliable 有序：一次丢包会把后面所有帧压住，10% 丢包下校正到达时已超出回滚可达范围（48 tick），只能写位姿，世界散到 120-150 m，故改回。

`session_lead_msg`（server → client，reliable，之后每 40 ms 一条 unreliable 副本共 3 条，收到 ack 即停；协议 8，迟到玩家设计 2.2-2.3，服务端开关 `physics.lead_adjust`，只发给 SessionReady 声明了 `CAP_LEAD` 的客户端）：session u32、player u32（收件人）、seq u32（服务端的决策 id，每个玩家整个会话内唯一，ack 认它，客户端也用它去重）、delta i16（tick，正 = 跑得更靠前）、held_from u32（N：服务端从这个 tick 起重置了该玩家的输入缓冲，0 = 无区间）、effective u32（E：从这个 tick 起服务端按新节奏收它的帧）、held 帧（keys u8、cam_right/cam_up/cam_dir f32×3、ball_type u8、flags u8：[N, E) 每个 tick 都用它，决策时冻结）、held_source u32（held 原本是哪个 tick 的帧，`INPUT_SOURCE_DEFAULT` = 默认帧）、epoch u32（决策时该玩家分配的基数；客户端基数更新则丢弃）。held_from 非 0 时必须小于 effective，delta 与 seq 不得为 0。降低领先量不动区间（held_from 0，effective = N）。`session_lead_ack_msg`（client → server，reliable）：session u32、seq u32、applied i16（客户端实际执行的 tick 数）；服务端只认尚未确认的那一条，applied 先截到 0 与该条 delta 之间（同号，不超过发出的量），再 `lead_offset += applied`（不低于 0）；声明了 `CAP_LEAD` 的客户端重同步时按 L_eff = 首次分配的前导量 + lead_offset（≤ 80）重新编号。

**协议 7（late-player 设计 1.1-1.2）：被替换的帧回给它的主人。** 服务端在某 tick 没有某成员的新鲜帧时，用该成员**最新到达**的帧（tick 不超过已模拟到的位置，新鲜或迟到的都算；什么都没到过时为默认帧）代替（`input_buffer::take` 同时给出这帧的来源 tick），照常应用、记入 journal（flag 0）、转发给其他成员。以前只有其他成员知道这件事，迟到的那个客户端自己永远不知道；现在每条报文都告诉收件人它自己哪些 tick 被替换了：

| 字段 | 位置 | 含义 |
| --- | --- | --- |
| acked u32 | 头部，count 之后 | 服务端对收件人最后一次以新鲜帧应用的 tick；每次 `SessionAssign` 置为 first_tick − 1（基数之前的 tick 不属于新编号），之后只增不减 |
| held_total u32 | 头部 | 收件人在 `epoch` 及之后被替换的 tick 总数 |
| wait u8 | 头部 | 协议 13（late-player 设计 2.5“As built (step R12)”）：收件人在服务端的等待，整 tick，见下 |
| epoch u32 | 头部 | 收件人最近一次 `SessionAssign` 的 first_tick（它当前编号的基数） |
| held 段 | 各项之后，**总是存在** | nheld u8（0..32），之后每个收件人在最近 `kHeldNoticeTicks` = 32 个 tick 内被替换的 tick T 一条记录 |

记录：back u8（bit0-4 = tick − T，0..31；bit7 = 后面跟内联帧而不是 source；bit5-6 必须为 0），然后要么 source u32（服务端在 T 应用的是客户端自己为 source 这个 tick 发出的那帧原样字节；`0xFFFFFFFF` = 默认帧），要么一帧（上面的帧编码，相机总是带）。只有 T − source ≥ 96 或 source 早于 epoch（换了编号，客户端的输入历史已清空）时才内联；客户端解析引用的已发帧环为 `kSentInputRing` = 256 tick：记录在其锚点 T−1 仍在回滚可达范围（最深 120）内时才会被用，引用的 source 最远在 reach + 94 tick 之前。引用一条 5 B，32 条加计数 161 B；整段放进该收件人第一条装得下的报文，否则单独一条（25 + 161 B）；全是内联时 32 × 41 B 超过一个包，按记录拆到几条里，不丢记录。头部从 13 B 变为 25 B（`kRelayDatagramHeader`），每条再加 1 B 的 nheld。读方拒收 nheld > 32、back 的 bit5-6 非 0、记录早于 tick 0，以及 held 段之后还有字节的报文——协议 6 的报文（没有这三个头部字段，也没有 held 段）因此解析不过。

服务端每个 tick 给**每个**成员发至少一条，没有别的可说时只有头部（协议 13 起 27 B），单人会话也发（`session_runner` 的 `on_inputs` 每一步都回调，不再要求至少两名玩家）：头部里的 acked 和 held_total 不依赖快照。held 段只含 tick ≥ epoch 的记录，held_total 也只数这些：重同步或迟到加入把游标重设到基数，基数之前的 tick 都按替换应用，但它们在客户端自己的编号之前，从不计数也不通知；收到 `SessionAssign` 前不发记录。`send_session_assign` 重置 epoch、held_total 和 acked。一个替换在收件人自己的报文里跟随 T .. T+31 共 32 个 tick（比转发窗口长，因为迟到玩家自己的下行最可能是坏的），在其他成员的报文里跟随 `relay_window` 个 tick。服务端日志每段连续替换一行 `S2 held player=… ticks=A..B n=… sent=… total=… copies=… epoch=… src=…..… delay=d*k,…`（n = 这段替换的 tick 数；sent = 其中首发报文（back 0）被 socket 接受的条数；copies = 该成员本 epoch 被 socket 接受的全部记录副本数，含重复；delay = T − source，`d` = 默认帧；超过 66 tick 的段按 66 切行）。客户端按 epoch 只认自己当前编号的记录（别的基数的报文只取 acked；其记录按 (epoch, tick) 去重后计入 stale_epoch），按 tick 去重后计数：无头客户端状态行 `subst=recv/diff/same/bodiless/rew/late`（目前只填 recv 与 late——late 是超出回滚可达范围的记录；diff/same/bodiless/rew 属于自身校正，plan step 4）、`subst_skip=dup/stale_epoch/base`（base = 任何回滚都够不到的记录：基数那个 tick 本身、重同步的全量快照已覆盖的 tick、等快照期间收到的，或没开 `--rollback`；这些不应触发重同步）以及 `acked=… held_total=…`（本 epoch 最新报文头），每个 epoch 的 recv + late + base 等于服务端 S2 行 n 之和；零售 mod 状态行 `subst=recv/dup/stale_epoch acked=… held_total=…`；两端都还不据此校正、也不写 journal。

协议 13（步骤 R12，用户 2026-10-27 的决定：回滚可达深度的余量 = 8 + 服务端等待，等待写进协议）：头部的 `wait` 是收件人在服务端的**等待**——它的帧缺席时，`session_runner` 的推进判定会比实际推进这一 tick 多等多久（`session/lateness.hpp` `server_wait_ticks`）：没开 patience 时等到截止时刻，即这一步比截止时刻早了多少 tick；开了 patience 时等到该成员的 patience（再减去它的 lead），它被豁免且有别的成员的帧在场时不等。不会为负（在截止时刻或之后推进的一步——被替换的 tick、卡顿后的追赶——什么也没等）。报头里带的是该成员最近 4 个 60 tick 窗口里每一步等待的最大值，向上取整，封顶 255（`server_wait_meter`）：一个被替换 tick 的通知随服务端在截止时刻那一步发出，比客户端用来采样报头滞后的那些报文晚了正好这么多，所以通知的深度是 h + w + 1（h 为报头滞后）。会话核心客户端把它加进回滚可达深度：reach = clamp(p99 + 8 + w, 48, cap)，cap 由客户端自己的回滚开销估算（`rollback.hpp` `rewind_cost_meter`：恢复耗时和每 tick 重放耗时的滑动平均，cap = clamp(floor((B − 恢复) / 每 tick 重放), 48, 120)，B = 3 tick ≈ 45 ms；第一次回滚之前取 48）。journal v7 记录它（tag 9 WAIT：u32 tick、u32 player、u8 wait；服务端在某成员报头里的值变化时写，客户端在收到的最新报头值变化时写）。零售旧路径照样解析这个字段，不使用。

`session_resync_msg`（client → server，reliable）：session u32、last_full_tick u32。服务端回复一个 full snapshot。

`session_end_msg`（server → client，reliable）：session u32、reason string ≤ 256。

## 3. 服务端配置

```yaml
rooms:
  maximum_rooms: 64
  maximum_members: 8
physics:
  enabled: true
  game_root: "C:/Ballance"            # base.cmo 所在目录（含 Bin/、3D Entities/ 等）
  snapshot_interval: 2                # tick
  input_delay: 6                      # 输入宽限的**下限**（tick）；每个会话开始时按成员里最差的 RTT 自行决定实际值
  spawn_impulse: 3.0                  # m/s；每次出生 Physicalize 的踢出速度，0 = 关闭；单人会话强制 0
  maximum_physics_rooms: 1            # M3 只验证过单房间
  debug_trace: false                  # 每 tick 诊断日志（rng/清醒刚体变化、输入沿、精确核心转储）；客户端用自动化命令 session trace on 配对
  event_rate_limit: 20                # 每玩家每秒上报事件数上限，超出部分丢弃；0 = 不限
  trafo_inputs: false                 # 协议 11：世界选项 WORLD_TRAFO_INPUTS（变球作为输入）；会话拒绝没有 CAP_TRAFO_INPUTS 的成员
  placement_clones: true              # 协议 12：世界选项 WORLD_PLACEMENT_CLONES（小节摆放）；会话拒绝没有 CAP_PLACEMENT 的成员；写过的 config.yml 保留原值，要开需手改
  proximity_union: true               # 协议 16：世界选项 WORLD_PROXIMITY_UNION（邻近并集，BodyRevived 退役）；关 = 反向对照与旧 journal 的重放
  allowed_mods:                       # 物理会话 Mod 白名单（id: 版本）；为空则不检查
    BallanceMMOClient: "3.6.8-beta18"
```

### 3.1 部署

- 服务端需要一份完整的游戏数据目录（`physics.game_root`，含 `base.cmo`、`3D Entities`、`Textures`、`Sounds` 等），Windows 与 Linux 都可以；一个物理房间的世界大约占一个核心，`maximum_physics_rooms` 默认 1。
- 客户端需要与服务端同一引擎提交构建的 physics_RT.dll 与 Mod。`SessionReady` 上报 DLL 的 sha256 与构建 id（`ballanced-<引擎提交>+bmmo-<仓库提交>`，每次构建前重新生成）；服务端只比引擎那一半，不同则结束会话并写明两边版本。无 git 可读的构建报 `unknown`，不参与比较（可用 `-DBMMO_BUILD_ID=` 指定）。
- 安装目标：`BallanceMMOServer`、`BallanceMMOSimTool`（离线回放/诊断）、`BallanceMMOSessionClient`（无头会话客户端，用于联调与压测）。
- 服务端校验客户端事件（设计 9.4）：每玩家每秒超过 20 条事件的部分直接丢弃；球型超范围或配方数值不合理的 Physicalize 直接拒绝；位姿远离关卡每个复活点（`ResetPoints` 的每一行）2.5 m 且远离上次位置 5 m 的 Physicalize、非单调的 Sector 只记日志并计数（控制台 `sessions` 显示 flagged/rejected）。
