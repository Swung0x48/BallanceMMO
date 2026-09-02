# 房间与会话协议（collision-overhaul）

本文定义在旧协议之上追加的房间（Room）与会话（Session）消息。旧消息与 opcode 值完全不变；新 opcode 追加在 `bmmo::opcode` 枚举末尾（`RemoteCommand` 之后），顺序固定：

```
RoomRequest, RoomState, RoomEvent,
SessionStart, SessionEnd, SessionReady, SessionInput, SessionSnapshot, SessionResync
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

客户端命令：`/mmo room list|create [name]|join <id>|leave|ready [on|off]|start [physics|shadow]|kick <player>|close`。

## 2. 会话（物理模式）

影子球会话不需要下面的消息：`SessionStarting` 后各客户端继续走旧逻辑。

### 2.1 时间线

- `tick` 长度 1/66 s。服务端在 `SessionStart` 中给出 `tick0_server_time`（服务端单调时钟 µs），客户端用旧协议已有的时钟偏移估计把它换算成本地时间，从 tick 0 开始按固定 tick 推进。
- 客户端每 tick 发送 `SessionInput`；服务端在 `tick + input_delay` 之前未收到该 tick 输入则沿用上一 tick。
- 服务端每 `snapshot_interval` 个 tick 广播一次 `SessionSnapshot`。

### 2.2 消息

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
| tick0_server_time | i64 |
| seed | i32 |
| players | u8 count，每项：id u32、join_order u8、ball_type u8 |

`session_ready_msg`（client → server，reliable）：session u32、load_order_hash u64、mechanism_count u16、physics_sha256 string ≤ 64。服务端把它与无头世界的对应值比较；不一致则 `SessionEnded` 并给出原因。

`session_input_msg`（client → server，unreliable no-delay）：session u32、first_tick u32、count u8（≤ 8，最近 count 个 tick 的输入，冗余抗丢包），每项：keys u8（bit0 上 bit1 下 bit2 左 bit3 右 bit4 Shift bit5 Space）、camera_right f32×3、camera_forward f32×3。

`session_snapshot_msg`（server → client，unreliable；full 版本 reliable）：session u32、tick u32、full u8、acked_input_tick u32、bodies u16 count，每项：kind u8（`Ball=0, Mechanism=1`）、owner u32（球）或 index u16（机关，按会话开始时两端一致的加载顺序表）、position f32×3、rotation f32×4（x,y,z,w）、linear f32×3、angular f32×3、flags u8（bit0 active、bit1 sleeping、bit2 collision enabled）。

`session_resync_msg`（client → server，reliable）：session u32、last_full_tick u32。服务端回复一个 full snapshot。

`session_end_msg`（server → client，reliable）：session u32、reason string ≤ 256。

## 3. 服务端配置

```yaml
rooms:
  maximum_rooms: 64
  maximum_members: 8
physics:
  enabled: true
  game_root: "C:/Ballance"            # base.cmo 所在目录
  snapshot_interval: 2                # tick
  input_delay: 2                      # tick
  allowed_mods:                       # 物理会话 Mod 白名单（id: 版本）
    BallanceMMOClient: "3.7.0"
```
