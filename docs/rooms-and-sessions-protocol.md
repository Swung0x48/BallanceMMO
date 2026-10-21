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

- `tick` 长度 1/66 s。每个客户端在收到 `SessionStart` 后先播放 3 秒 “3 - 2 - 1 - Go!” 倒计时（沿用 `countdown_msg` 的提示音与提示行，纯本地效果，不参与确定性），在 “Go!” 这一帧重开当前关卡，`Gameplay_Ingame` 首次激活的行为帧为锚点，锚点的编号由服务端在 `SessionAssign` 里给出：会话开始时在场的成员用**会话起始基数**（见 2.2 的公式），迟到加入者与重同步者用服务端当前 tick 加上同一个前导量。锚点执行会话重置（IVP 时钟归零、`ivp_srand(seed)`），此后每个行为帧一个 tick。
- 客户端每 tick 发送 `SessionInput`；服务端在收齐所有成员该 tick 的输入、或墙钟超过 `开始时刻 + (tick + input_delay)/66 s` 时模拟该 tick，缺失输入沿用该玩家上一 tick 的输入。
- `input_delay` 是**每个会话**自己的，在会话开始时定下并写进该会话的 `SessionStart`：取成员里最差的一条链路（GNS 的 `m_nPing`，且用该连接上见过的峰值而不是当下值），按 `单程 × 1.5 + 16 ms` 折成 tick，再夹在 `physics.input_delay`（下限）与 40 tick（上限）之间。窗口装的是**单程**——客户端按自己的时钟跑在服务端调度之前，输入只需单向抵达；乘 1.5 是因为决定输入迟不迟到的是抖动而不是均值，而 RTT 本身已经是平滑过的数。定下之后整场不变：调度的截止时刻是从会话开始时刻带着这个值算出来的，迟到加入者不能把已经在跑的成员的时间线挪走。
- 服务端每 `snapshot_interval` 个 tick 广播一次 `SessionSnapshot`（不可靠）；每 66 tick 或刚体集合变化时广播 full 快照（可靠，携带机关名字典）。
- 生命周期事件（`SessionEvent`）可靠传输，带发生的客户端 tick；服务端在不早于该 tick 的模拟步中应用，并转发给房间其他成员（`player` 字段为来源）。
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
| players | u8 count，每项：id u32、join_order u8、ball_type u8、spawn_position f32×3、spawn_rotation f32×4（实体世界矩阵位姿，CK 侧为单精度；这是原版复活点本身，每个成员都相同——设计 9.10 去掉了出生环偏移） |

`session_ready_msg`（client → server，reliable）：session u32、first_tick u32、anchor_hash u64（锚点世界的可动 core 位姿哈希，即 `world_hash::pose`，每个 core 各自哈希后无序求和，与刚体列表顺序无关；不含物理时间因子等时钟派生量，因为客户端重开关卡会比新加载早一帧设置时间因子）、anchor_surfaces u64（碰撞表面签名）、physics_sha256 string ≤ 64、build_id string ≤ 64；protocol u32（客户端的 `session::PHYSICS_PROTOCOL_VERSION`，当前为 7）紧跟 first_tick 之后，是必填字段，缺了整条消息不解析。服务端先比 protocol：不等于自己的就 `SessionEnd` 并写明两边版本——新旧构建不得同场。再把哈希与自己锚点的值比较（迟到加入者除外）；不一致则 `SessionEnd` 并给出原因。同时比 `build_id` 的引擎半部（`ballanced-<rev>`）：不同则同样结束会话，任一边为 `unknown` 时不比。`session_start_msg` 在 spawn_impulse 之后同样带服务端的 protocol u32（必填）：客户端不等则拒绝开始会话并提示更新，解析不出来（旧服务端）也提示。

`session_input_msg`（client → server，unreliable no-delay）：session u32、first_tick u32、count u8（≤ 8，从 first_tick 起连续 count 个 tick，最新的在最后），每项：keys u8（bit0 叶子 0 … bit3 叶子 3，叶子编号按 `Ball Navigation` 图内 `SetPhysicsForce` 的子块顺序；bit4 Shift、bit5 Space 仅作记录）、cam_right f32×3、cam_up f32×3、cam_dir f32×3（`Cam_OrientRef` 世界矩阵的三条基向量）、ball_type u8、flags u8（bit0 physicalized、bit1 paused、bit2 nav_active——客户端 BallNav activate/deactivate 的当前状态，服务端据此复现 Key Event 的 On/Off；bit1 仅作记录，会话期间暂停不再停摆世界，见设计 9.2）。

`session_event_msg`（双向，reliable）：session u32、player u32（服务端转发时为来源，客户端发送时为 0）、tick u32、type u8、按 type 附加： **协议 6 起**：两个方向都仍先 reliable（NoNagle）发一份，随后由发送方不可靠重发 6 份、每隔一个 tick 一份（跨 12 tick）（服务端在 `on_session_inputs` 里对仍在场的成员发，客户端在每帧发输入的地方发；`session/lifecycle.hpp` `event_repeats`）；接收方以 (tick, player, type, ball_type, sector, name) 为键只取首达的一份（`event_seen`：服务端按连接在限速之前去重，无头客户端在入队前，零售 mod 在排空队列时），其余忽略。这样一次丢包不再把事件拖到 ~400 ms 的重传之后（超出回滚 48 tick 的可达范围）；事件仍然按戳记应用：服务端按 tick + 入场顺序，无头客户端按戳记排队、迟到则回滚，零售 mod 把一批按 (tick, 入场顺序) 排序后应用，并丢弃比该玩家已应用的生命周期事件更旧的 Physicalize/Unphysicalize（它只能到达即应用、不能回滚）。

| type | 附加字段 |
| --- | --- |
| Physicalize = 0 | ball_type u8、flags u8（bit0 = spawn：在 `CurrentLevel[0,3]` 复活点 Physicalize，即出生或复活；紧跟 ball_type 之后，设计 9.10）、position f32×3、rotation f32×9（实体世界矩阵的三行旋转部分 right/up/dir，按位传输，服务端用它原样重建矩阵后 Physicalize，避免四元数往返的舍入）、recipe：fixed u8、friction f32、elasticity f32、mass f32、start_frozen u8、enable_collision u8、calc_mass_center u8、linear_damp f32、rot_damp f32、mass_center f32×3、collision_surface string ≤ 64、convex_count u8 + 每项网格名 string ≤ 64、ball_count u8 + 每项 center f32×3、radius f32、concave_count u8 + 每项网格名 string ≤ 64 |
| Unphysicalize = 1 | 无 |
| Sector = 2 | sector i32 |
| Finish = 3 | 无 |
| BodyRevived = 4 | name string ≤ 64 |

`session_snapshot_msg`（server → client，unreliable；full 版本 reliable）：session u32、tick u32、full u8、acked_input_tick u32、bodies u16 count，每项：kind u8（`Ball=0, Mechanism=1`）、owner u32（球：玩家 id；机关：字典索引）、name string ≤ 64（仅 full 且 kind=Mechanism，其余为空串）、position f64×3、rotation f64×4、linear f32×3、angular f32×3、flags u8（bit0 simulated、bit1 collision enabled）。位置与四元数用双精度，因为 IVP core 的位置/四元数本身是双精度（速度是单精度），镜像/修正要按位写回。

`session_assign_msg`（server → client，reliable；会话开始时在场的成员在全员 `session_ready_msg` 到齐后**一起**收到 `first_tick = 会话起始基数 = input_delay + min(ceil(最差成员 RTT_ms × 66 / 1000), 12) + 2`，再夹到 `44 − input_delay`（44 = `rollback.hpp` 的 `max_resim_ticks` 48 减 4 tick 余量，下限 1，`sim/late_tick.hpp` 的 `session_start_tick_base`；起始滞后是基数加 `input_delay`，所以基数必须让这个和留在回滚窗口内），迟到加入者与重同步者即时收到 `first_tick = 服务端当前 tick + 同一前导量`；`session_resync_msg{session, last_full_tick}`（client → server，reliable）由客户端在节拍原点重设、连续 3 次硬置或连续 30 个快照对不上历史时发出，服务端按迟到加入处理：重新分配编号、重发各成员最近的 Physicalize、强制一次全量快照；客户端收到新分配后清空历史，用下一个全量快照一次写入全部刚体）：session u32、first_tick u32——客户端锚点帧对应的服务端 tick 编号（服务端世界自己的编号仍从 0 开始，这个基数只是客户端看到的号）：会话开始时在场的成员为会话起始基数（世界此时还没 tick 过，“当前 tick”项因此为 0），迟到加入者为服务端收到 Ready 时的当前 tick 加同一个前导量。给的是前导量而不是服务端当前 tick，是因为这条可靠消息要过半个 RTT 才到：按当前 tick 分配会让客户端落后半个 RTT，输入赶不上调度截止时刻（首次联调 1380/1380 全 unmatched 正是如此）。客户端收到前不发送输入（缓存），收到后按 `first_tick + 锚点后经过的帧数` 编号，旧编号的缓存帧不再补发、直接丢弃。会话开始也用同一个前导量，是因为锚点是各自关卡加载完成的时刻，成员之间可以相差数秒；不重新编号的成员会一直按自己的锚点编号跑在服务端前面，快照与本地历史的差值（编号偏移 + input_delay + 单程）随加载差增长，超过 `rollback.hpp` 的 `max_resim_ticks = 48` 就变成 too_far → 重同步抖动。

`session_remote_input_msg`（server → client，unreliable no-delay，每模拟一个 tick 发一条，M4 设计 9.1；协议 5 起带冗余窗口，`session/input_relay.hpp`；协议 7 起带收件人自己的被替换帧，见下表）：session u32、tick u32（这条里最新的 tick）、count u8、acked u32、held_total u32、epoch u32（协议 7）、count × {player u32、nframes u8（1..`MAX_RELAY_FRAMES` = 32，对应 tick−nframes+1 .. tick，旧的在前）、nframes × 帧}；每帧：header u8（bit0 `FRAME_CAMERA`：后面跟 cam_right/cam_up/cam_dir f32×3）、keys u8、ball_type u8、flags u8、[相机 36 B]。每个成员的第一帧必带相机（读到不带的即拒收），之后的帧与前一帧的三个向量逐位相同时省掉，读方从前一帧复制——每条报文自成一体，不依赖别的报文到达；相机不动时一帧 4 B。一条报文装不超过 1100 B（一个包）的整项，装不下就分成几条同 tick 的报文（GNS 对跨包的 unreliable 报文丢一片即整条丢，超过 16.5 KB 的会悄悄改成 reliable 发）；一项单独放不进一个包时丢掉最旧的帧。内容是服务端在最近 `relay_window`（服务端配置，默认 8，钳到 1..16：input_delay + 一个单程 + 窗口要留在回滚可达的 48 tick 以内）个 tick **实际采用**的每个其他成员的输入帧（新鲜的或沿用上一 tick 的；只带最新 tick 在场的成员，取它连续在场的那一段），去掉了收件成员自己。客户端按 tick 重组（`relay_seen` 保证同一 (player, tick) 只记一次账、只校正一次；`relay_store` 进镜像的输入环；镜像还没建的玩家进 `pending_relay`，Physicalize 时并入），用它驱动远端球的本地导航复制（桥接 API v3 `navigation_*`），快照只做校正；一帧只有 `relay_window` 条报文连续丢失才真正丢；这样的洞（前后都有帧）重放时用最近的前一帧，还没到的尾部沿用记录的预测；到得太晚（超过 48 tick 或超出历史）的帧不再回滚，计入状态行的 `relay=.../late`，`lag=max/p99` 是首达帧的滞后。零售 mod 做同样的重组、去重、暂存、记账和预测推进，但校正仍只来自快照（无头客户端的输入校正回滚尚未移植）。2026-10-19 到 10-21 这条流曾改成 reliable 有序：一次丢包会把后面所有帧压住，10% 丢包下校正到达时已超出回滚可达范围（48 tick），只能写位姿，世界散到 120-150 m，故改回。

**协议 7（late-player 设计 1.1-1.2）：被替换的帧回给它的主人。** 服务端在某 tick 没有某成员的新鲜帧时，用该成员**最新到达**的帧（tick 不超过已模拟到的位置，新鲜或迟到的都算；什么都没到过时为默认帧）代替（`input_buffer::take` 同时给出这帧的来源 tick），照常应用、记入 journal（flag 0）、转发给其他成员。以前只有其他成员知道这件事，迟到的那个客户端自己永远不知道；现在每条报文都告诉收件人它自己哪些 tick 被替换了：

| 字段 | 位置 | 含义 |
| --- | --- | --- |
| acked u32 | 头部，count 之后 | 服务端对收件人最后一次以新鲜帧应用的 tick；每次 `SessionAssign` 置为 first_tick − 1（基数之前的 tick 不属于新编号），之后只增不减 |
| held_total u32 | 头部 | 收件人在 `epoch` 及之后被替换的 tick 总数 |
| epoch u32 | 头部 | 收件人最近一次 `SessionAssign` 的 first_tick（它当前编号的基数） |
| held 段 | 各项之后，**总是存在** | nheld u8（0..32），之后每个收件人在最近 `kHeldNoticeTicks` = 32 个 tick 内被替换的 tick T 一条记录 |

记录：back u8（bit0-4 = tick − T，0..31；bit7 = 后面跟内联帧而不是 source；bit5-6 必须为 0），然后要么 source u32（服务端在 T 应用的是客户端自己为 source 这个 tick 发出的那帧原样字节；`0xFFFFFFFF` = 默认帧），要么一帧（上面的帧编码，相机总是带）。只有 T − source ≥ 96 或 source 早于 epoch（换了编号，客户端的输入历史已清空）时才内联；客户端解析引用的已发帧环为 `kSentInputRing` = 256 tick：记录在其锚点 T−1 仍在回滚可达范围（最深 120）内时才会被用，引用的 source 最远在 reach + 94 tick 之前。引用一条 5 B，32 条加计数 161 B；整段放进该收件人第一条装得下的报文，否则单独一条（25 + 161 B）；全是内联时 32 × 41 B 超过一个包，按记录拆到几条里，不丢记录。头部从 13 B 变为 25 B（`kRelayDatagramHeader`），每条再加 1 B 的 nheld。读方拒收 nheld > 32、back 的 bit5-6 非 0、记录早于 tick 0，以及 held 段之后还有字节的报文——协议 6 的报文（没有这三个头部字段，也没有 held 段）因此解析不过。

服务端每个 tick 给**每个**成员发至少一条，没有别的可说时只有头部（26 B），单人会话也发（`session_runner` 的 `on_inputs` 每一步都回调，不再要求至少两名玩家）：头部里的 acked 和 held_total 不依赖快照。held 段只含 tick ≥ epoch 的记录，held_total 也只数这些：重同步或迟到加入把游标重设到基数，基数之前的 tick 都按替换应用，但它们在客户端自己的编号之前，从不计数也不通知；收到 `SessionAssign` 前不发记录。`send_session_assign` 重置 epoch、held_total 和 acked。一个替换在收件人自己的报文里跟随 T .. T+31 共 32 个 tick（比转发窗口长，因为迟到玩家自己的下行最可能是坏的），在其他成员的报文里跟随 `relay_window` 个 tick。服务端日志每段连续替换一行 `S2 held player=… ticks=A..B n=… sent=… total=… copies=… epoch=… src=…..… delay=d*k,…`（n = 这段替换的 tick 数；sent = 其中首发报文（back 0）被 socket 接受的条数；copies = 该成员本 epoch 被 socket 接受的全部记录副本数，含重复；delay = T − source，`d` = 默认帧；超过 66 tick 的段按 66 切行）。客户端按 epoch 只认自己当前编号的记录（别的基数的报文只取 acked；其记录按 (epoch, tick) 去重后计入 stale_epoch），按 tick 去重后计数：无头客户端状态行 `subst=recv/diff/same/bodiless/rew/late`（目前只填 recv 与 late——late 是超出回滚可达范围的记录；diff/same/bodiless/rew 属于自身校正，plan step 4）、`subst_skip=dup/stale_epoch/base`（base = 任何回滚都够不到的记录：基数那个 tick 本身、重同步的全量快照已覆盖的 tick、等快照期间收到的，或没开 `--rollback`；这些不应触发重同步）以及 `acked=… held_total=…`（本 epoch 最新报文头），每个 epoch 的 recv + late + base 等于服务端 S2 行 n 之和；零售 mod 状态行 `subst=recv/dup/stale_epoch acked=… held_total=…`；两端都还不据此校正、也不写 journal。

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
  allowed_mods:                       # 物理会话 Mod 白名单（id: 版本）；为空则不检查
    BallanceMMOClient: "3.6.8-beta18"
```

### 3.1 部署

- 服务端需要一份完整的游戏数据目录（`physics.game_root`，含 `base.cmo`、`3D Entities`、`Textures`、`Sounds` 等），Windows 与 Linux 都可以；一个物理房间的世界大约占一个核心，`maximum_physics_rooms` 默认 1。
- 客户端需要与服务端同一引擎提交构建的 physics_RT.dll 与 Mod。`SessionReady` 上报 DLL 的 sha256 与构建 id（`ballanced-<引擎提交>+bmmo-<仓库提交>`，每次构建前重新生成）；服务端只比引擎那一半，不同则结束会话并写明两边版本。无 git 可读的构建报 `unknown`，不参与比较（可用 `-DBMMO_BUILD_ID=` 指定）。
- 安装目标：`BallanceMMOServer`、`BallanceMMOSimTool`（离线回放/诊断）、`BallanceMMOSessionClient`（无头会话客户端，用于联调与压测）。
- 服务端校验客户端事件（设计 9.4）：每玩家每秒超过 20 条事件的部分直接丢弃；球型超范围或配方数值不合理的 Physicalize 直接拒绝；位姿远离关卡每个复活点（`ResetPoints` 的每一行）2.5 m 且远离上次位置 5 m 的 Physicalize、非单调的 Sector 只记日志并计数（控制台 `sessions` 显示 flagged/rejected）。
