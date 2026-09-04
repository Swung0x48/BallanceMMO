# 房间与会话协议（collision-overhaul）

本文定义在旧协议之上追加的房间（Room）与会话（Session）消息。旧消息与 opcode 值完全不变；新 opcode 追加在 `bmmo::opcode` 枚举末尾（`RemoteCommand` 之后），顺序固定：

```
RoomRequest, RoomState, RoomEvent,
SessionStart, SessionEnd, SessionReady, SessionInput, SessionSnapshot, SessionResync, SessionEvent, SessionAssign, SessionRemoteInput
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
SessionStart, SessionEnd, SessionReady, SessionInput, SessionSnapshot, SessionResync, SessionEvent, SessionAssign, SessionRemoteInput
```

### 2.1 时间线

- `tick` 长度 1/66 s。每个客户端在收到 `SessionStart` 后先播放 3 秒 “3 - 2 - 1 - Go!” 倒计时（沿用 `countdown_msg` 的提示音与提示行，纯本地效果，不参与确定性），在 “Go!” 这一帧重开当前关卡，`Gameplay_Ingame` 首次激活的行为帧为锚点，编号为 `first_tick`（首次开始为 0，迟到加入者由服务端指定）。锚点执行会话重置（IVP 时钟归零、`ivp_srand(seed)`），此后每个行为帧一个 tick。
- 客户端每 tick 发送 `SessionInput`；服务端在收齐所有成员该 tick 的输入、或墙钟超过 `开始时刻 + (tick + input_delay)/66 s` 时模拟该 tick，缺失输入沿用该玩家上一 tick 的输入。
- 服务端每 `snapshot_interval` 个 tick 广播一次 `SessionSnapshot`（不可靠）；每 66 tick 或刚体集合变化时广播 full 快照（可靠，携带机关名字典）。
- 生命周期事件（`SessionEvent`）可靠传输，带发生的客户端 tick；服务端在不早于该 tick 的模拟步中应用，并转发给房间其他成员（`player` 字段为来源）。

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
| first_tick | u32（接收者锚点 tick 的编号） |
| seed | i32 |
| spawn_impulse | f32（每次出生 Physicalize 的踢出速度 m/s，0 = 无；写在 seed 之后） |
| players | u8 count，每项：id u32、join_order u8、ball_type u8、spawn_position f32×3、spawn_rotation f32×4（实体世界矩阵位姿，CK 侧为单精度；这是原版复活点本身，每个成员都相同——设计 9.10 去掉了出生环偏移） |

`session_ready_msg`（client → server，reliable）：session u32、first_tick u32、anchor_hash u64（锚点世界的可动 core 位姿哈希，即 `world_hash::pose`；不含物理时间因子等时钟派生量，因为客户端重开关卡会比新加载早一帧设置时间因子）、anchor_surfaces u64（碰撞表面签名）、physics_sha256 string ≤ 64、build_id string ≤ 64。服务端把哈希与自己锚点的值比较（迟到加入者除外）；不一致则 `SessionEnd` 并给出原因。同时比 `build_id` 的引擎半部（`ballanced-<rev>`）：不同则同样结束会话，任一边为 `unknown` 时不比。

`session_input_msg`（client → server，unreliable no-delay）：session u32、first_tick u32、count u8（≤ 8，从 first_tick 起连续 count 个 tick，最新的在最后），每项：keys u8（bit0 叶子 0 … bit3 叶子 3，叶子编号按 `Ball Navigation` 图内 `SetPhysicsForce` 的子块顺序；bit4 Shift、bit5 Space 仅作记录）、cam_right f32×3、cam_up f32×3、cam_dir f32×3（`Cam_OrientRef` 世界矩阵的三条基向量）、ball_type u8、flags u8（bit0 physicalized、bit1 paused、bit2 nav_active——客户端 BallNav activate/deactivate 的当前状态，服务端据此复现 Key Event 的 On/Off）。

`session_event_msg`（双向，reliable）：session u32、player u32（服务端转发时为来源，客户端发送时为 0）、tick u32、type u8、按 type 附加：

| type | 附加字段 |
| --- | --- |
| Physicalize = 0 | ball_type u8、flags u8（bit0 = spawn：在 `CurrentLevel[0,3]` 复活点 Physicalize，即出生或复活；紧跟 ball_type 之后，设计 9.10）、position f32×3、rotation f32×9（实体世界矩阵的三行旋转部分 right/up/dir，按位传输，服务端用它原样重建矩阵后 Physicalize，避免四元数往返的舍入）、recipe：fixed u8、friction f32、elasticity f32、mass f32、start_frozen u8、enable_collision u8、calc_mass_center u8、linear_damp f32、rot_damp f32、mass_center f32×3、collision_surface string ≤ 64、convex_count u8 + 每项网格名 string ≤ 64、ball_count u8 + 每项 center f32×3、radius f32、concave_count u8 + 每项网格名 string ≤ 64 |
| Unphysicalize = 1 | 无 |
| Sector = 2 | sector i32 |
| Finish = 3 | 无 |
| BodyRevived = 4 | name string ≤ 64 |

`session_snapshot_msg`（server → client，unreliable；full 版本 reliable）：session u32、tick u32、full u8、acked_input_tick u32、bodies u16 count，每项：kind u8（`Ball=0, Mechanism=1`）、owner u32（球：玩家 id；机关：字典索引）、name string ≤ 64（仅 full 且 kind=Mechanism，其余为空串）、position f64×3、rotation f64×4、linear f32×3、angular f32×3、flags u8（bit0 simulated、bit1 collision enabled）。位置与四元数用双精度，因为 IVP core 的位置/四元数本身是双精度（速度是单精度），镜像/修正要按位写回。

`session_assign_msg`（server → client，reliable；会话开始时在场的成员在全员 `session_ready_msg` 到齐后一起收到 `first_tick = 0`，迟到加入者与重同步者即时收到 `first_tick = 服务端当前 tick + input_delay + 2`，从而与其他成员一样领先服务端；`session_resync_msg{session, last_full_tick}`（client → server，reliable）由客户端在节拍原点重设、连续 3 次硬置或连续 30 个快照对不上历史时发出，服务端按迟到加入处理：重新分配编号、重发各成员最近的 Physicalize、强制一次全量快照；客户端收到新分配后清空历史，用下一个全量快照一次写入全部刚体）：session u32、first_tick u32——客户端锚点帧对应的服务端 tick 编号：会话开始时在场的成员为 0，迟到加入者为服务端收到 Ready 时的当前 tick。客户端收到前不发送输入（缓存），收到后按 `first_tick + 锚点后经过的帧数` 编号并补发缓存。

`session_remote_input_msg`（server → client，unreliable no-delay，每模拟一个 tick 发一条，M4 设计 9.1）：session u32、tick u32、count u8、count × {player u32、input_frame（与 `session_input_msg` 的帧布局相同：keys u8、cam_right/cam_up/cam_dir f32×3、ball_type u8、flags u8）}。内容是服务端在该 tick **实际采用**的每个其他成员的输入帧（新鲜的或沿用上一 tick 的），去掉了收件成员自己；客户端用它驱动远端球的本地导航复制（桥接 API v3 `navigation_*`），快照只做校正。晚到的帧不回放，客户端在没有新帧时沿用最近一帧预测。

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
  input_delay: 6                      # tick；服务端最多等待这么久再用上一 tick 的输入
  spawn_impulse: 3.0                  # m/s；每次出生 Physicalize 的踢出速度，0 = 关闭；单人会话强制 0
  maximum_physics_rooms: 1            # M3 只验证过单房间
  debug_trace: false                  # 每 tick 诊断日志（rng/清醒刚体变化、输入沿、精确核心转储）；客户端用自动化命令 session trace on 配对
  event_rate_limit: 20                # 每玩家每秒上报事件数上限，超出部分丢弃；0 = 不限
  allowed_mods:                       # 物理会话 Mod 白名单（id: 版本）；为空则不检查
    BallanceMMOClient: "3.6.8-beta18"
```

### 3.1 部署

- 服务端需要一份完整的游戏数据目录（`physics.game_root`，含 `base.cmo`、`3D Entities`、`Textures`、`Sounds` 等），Windows 与 Linux 都可以；一个物理房间的世界大约占一个核心，`maximum_physics_rooms` 默认 1。
- 客户端需要与服务端同一引擎提交构建的 physics_RT.dll 与 Mod。`SessionReady` 上报 DLL 的 sha256 与构建 id（`ballanced-<引擎提交>+bmmo-<仓库提交>`，每次构建前重新生成）；服务端只比引擎那一半，不同则结束会话并写明两边版本。无 git 可读的构建报 `unknown`，不参与比较（可用 `-DBMMO_BUILD_ID=` 指定）。
- 安装目标：`BallanceMMOServer`、`BallanceMMOSimTool`（离线回放/诊断）、`BallanceMMOSessionClient`（无头会话客户端，用于联调与压测）。
- 服务端校验客户端事件（设计 9.4）：每玩家每秒超过 20 条事件的部分直接丢弃；球型超范围或配方数值不合理的 Physicalize 直接拒绝；位姿远离复活点 2.5 m 且远离上次位置 5 m 的 Physicalize、非单调的 Sector 只记日志并计数（控制台 `sessions` 显示 flagged/rejected）。
