# 回滚核心统一方案（v2，2026-10-21，按三路对源码的审查修订）

目标一句话：**锚点恢复 + 重放的逻辑只存在一份**，无头客户端、SimTool、零售 mod 三方
都用同一个 hook 绑定驱动同一个 `rollback_engine`；SimTool 的 `--savestate-check`
测的是客户端真正走的那条路，而不是自己复刻的一条。

> v1 写的时候，工作树里已经在做无头侧的一半（主线会话）。v2 以工作树为准：
> 已经落地的不再重复计划，只写剩下的和要修正的。审查发现的事实都带 `file:line`。

## 0. 已经在工作树里的（以此为 S1 的起点，不再另起一套）

- `BallanceMMOCommon/include/session/physics_rollback_binding.hpp`（新，header-only）：
  `physics_rollback_options` + `make_physics_rollback_world(options)`，无头侧（直调
  `bmmo::physics::*`）的唯一绑定。选项已含：`physics`、`context`、`delta_ms`、
  `restore_clock`（默认 false）、`savestate_flags`、脚本回放（`scripts`、`script_hooks`、
  `std::vector<CK_ID>* scripts_replayed`、`scripts_kept_live(anchor, kept)` 回调、
  `script_diff_detail`）、轮询键表、`log`、`before_replayed_tick`、`on_contacts_restored`、
  `on_savestate_capture_failed`、`on_savestate_restored(ok, error, report)`、
  `on_script_restore_failed`、`on_correction`。
- `session_client.cpp` 的 `rollback_world()` 已委托给它（约 :2695-2815），`--replay-scripts`
  与 `--trace` 通过事后替换 / 包装 `world.step` 实现。
- `sim_tool.cpp --savestate-check` 已改为驱动引擎：`engine.thresholds().history_ticks =
  window+2`、`max_resim_ticks = window+2`、`set_exact_restore(!legacy)`（:1870-1872）；直通段
  在 tick 边界、步进之前 `engine.record(t)` 并用 `engine.recorded_savestate(t)` 校验与
  harness 自存 blob 相同（:1937-1939）；每回合 `world->restore_step_state(anchor.counters)`
  后 `engine.rewind_to`（:2004-2013）；`binding.step` 被包成
  `[world->pre_simulate(); inner()]`（:1858-1859），这正是脚本 → PreSimulate → PSI 的 live
  顺序。`rollback.hpp` 为此加了 `recorded_savestate()`（:1019）和可写的 `thresholds()`（:1023）。
  旧的 `capture_legacy_anchor` / `restore_legacy_anchor` / `step_replay` 已删。

共用情况于是变成：物理（一份源码：`BallanceMMOSim` 与 `physics_RT.dll` 同源）、脚本回放
（header-only，三方）、回滚引擎（两个客户端）、**hook 绑定（无头客户端 + SimTool 一份；
零售 mod 仍是单独一份**，`physics_session_client.cpp:2778-2960`，经 `physics_view_`）。

## 1. 审查指出的、要在现有绑定里修的（S1，小改，先做）

| # | 问题 | 改法 |
|---|---|---|
| 1 | `nav_poll` 在没有键码时直接返回 true、不调桥接（`physics_rollback_binding.hpp:236-237`）；原无头绑定总是调 `navigation_poll`，`enable=false, count=0` 会成功并清掉 poll 标志（`ball_navigation.cpp:485-509`） | 总是调用；键码为空只影响 enable=true 的那次 |
| 2 | 脚本 hook 的安装条件写成 `!scripts->scripts.empty()`（:146），而 `selection::empty()` 还算卫星块（`script_replay.hpp:98`）；零售端条件是 `!script_ids.empty()`，无头端是 `args_.script_replay && !scripts_.empty()` | 统一为：`scripts && context && !scripts->empty()`；调用方决定传不传指针 |
| 3 | `pre_step` 只在有脚本时安装（沿用零售端的门），但无头端的 `pre_step` 还负责 `drain_pending_births`（原 `session_client.cpp:2784-2785`）；`--no-script-replay` 下丢掉它会回到 c97d02c 修过的"重合出生"分叉 | `before_replayed_tick` 非空就装 `pre_step`，脚本部分再按 #2 的门 |
| 4 | 重放后导航"晚一拍"的修复 `rearm_own_navigation` 在调用方（`session_client.cpp:2180-2195`，两处 rewind 之后调），绑定与引擎都不含它，SimTool 驱动引擎测不到 | 绑定加 `after_replay(uint32_t current)` 回调（或引擎在 `rewind_to` 末尾调一个 world hook），把重臂放进去；两个客户端都走这一处 |
| 5 | 零售端绑定在失败时写 `physics_session_.last_error`（`physics_session_client.cpp:2799, 2881, 2903, 2912`），选项里没有错误槽 | 加 `std::function<void(const std::string&)> on_error`（或 `std::string* last_error`） |
| 6 | 日志上限计数器：无头 `contact_traces_`（上限 48）被 `pre_step` 和 `maybe_force_rollback` 共用；零售的 savestate 计数是函数 static，跨会话不清零；`contact_restore_logs` / `script_restore_logs` 不在 `reset_runtime` 里（`physics_session.hpp:256-257, 325-387`） | 计数器留在调用方回调里（共享预算不变）；零售端 static 改成会话成员，并在 anchor 清零 |
| 7 | 零售端 `script_reset_ticks` 从不清（只写于 `:1912`，`reset_runtime` 漏掉），旧会话的 reset tick 会把新会话的脚本标成 kept-live；无头端每会话清（`session_client.cpp:903`） | 单独一个小改：零售 anchor 时清空；与统一无关，但要列出 |

## 2. 零售 mod 共用同一份绑定（S2）

插件边界的真实限制是**链接**，不是 include：`bmmo::physics::*` 只编进 `physics_RT.dll` 和
`BallanceMMOSim`，mod 只编 `physics_view.cpp` 并经 C 表（精确版本校验，`physics_view.cpp:108`）
访问。`rollback.hpp` 只 include std 头、entity/session、message 头和 `physics_rt_api.h`；
`script_replay.hpp` include `CKAll.h`，mod 本来就用。所以一份 header-only 绑定可以被 mod
include，条件是**它一个 `bmmo::physics::` 自由函数都不直接调**——现在的
`make_physics_rollback_world` 直接调（例如 `set_event_log_muted`），这就是它不能给 mod 用的
全部原因。

改法：把 hook 体改成模板，物理原语经 `Backend` 传入。

```
template <class Backend>
rollback_world bind_rollback_world(Backend physics, physics_rollback_options options);
        // 两个参数都按值捕进 lambda：两个客户端每次调引擎都重建 rollback_world 并按值返回
        // （session_client.cpp:1655,1742,2011,2223,2281,2480；physics_session_client.cpp:1913,2044,3001），
        // 引用会悬空。持久状态只能经指针进来：scripts_replayed、计数器、键表、reset-tick map。

无头 Backend:  physics_direct { CKIpionManager* m; }     // 薄包装，同名方法转 bmmo::physics::*
               include physics_state.hpp / physics_savestate.hpp / ball_navigation.hpp（它们只前置声明
               CKIpionManager），不 include CKIpionManager.h；只在无头侧编译
零售 Backend:  physics_view_ref { const physics_view* v; }  // 指针句柄。不能按值拷 physics_view：
               它可拷贝但带 mutable savestate_size_hint_（physics_view.hpp:168），每次重建都从 0 容量起步，
               一次 savestate_get 变成两次完整 capture（physics_view.cpp:539-543, physics_rt_bridge.cpp:566-570）
```

Backend 方法名以 `physics_view` 为准（mod 侧改名代价高）：`get_body_state`、
`set_body_state(..., bmmo::physics::wake_mode, recheck, error)`（模板里显式
`static_cast<wake_mode>`，否则会落到 `bool wake` 的旧 beam 重载，`physics_state.hpp:162` /
`physics_view.hpp:48`）、`get/set_core_extra`、`get/set_clock_state`、`get_clock`、
`contacts_get/contacts_set`（16 项缓冲、-1 约定两边一致）、`body_recheck`、
`navigation_get_state/set_state/input/poll`、`savestate_get`、`savestate_set`、`step_physics`、
`set_event_log_muted`。审查逐 hook 核过：**无需新增 C API**。

两处两侧不等价的地方要进契约：
- `savestate_set`：无头拿到完整 `savestate_report`（计数 + warnings），零售只拿到两个名字表
  （C 入口不传别的，`physics_rt_api.h:562-563`；桥接丢掉 warnings，`physics_rt_bridge.cpp:576-597`）。
  Backend 签名定为 `savestate_set(data, size, flags, error, savestate_outcome&, savestate_report* /*可空*/)`，
  `on_savestate_restored` 的 report 参数零售端为空。
- 零售端把 dropped / unknown 名字表 snprintf 进 2048 / 4096 字节的定长缓冲
  （`physics_view.cpp:585-599`，`physics_rt_bridge.cpp:591`），截断会让引擎漏掉对 unknown 刚体
  的 recheck（`rollback.hpp:731-732`）。改成先查长度再分配，或截断时返回失败。

两侧**行为**差异（不只是日志），逐条进选项或明确保留：
`set_clock` 只在无头 `--restore-clock` 下安装、零售从不安装——而引擎的 `restore_clock_`
默认为 true（`rollback.hpp:1293`），零售端"不装 hook"是它唯一的时钟开关，所以
`restore_clock` 必须默认 false 且零售永不置真；`pre_step` 的 `drain_pending_births` 只有无头
端有（mod 没有这条路径，统一不会给 mod 加上，S2 要写明）；`--replay-scripts` 整帧步进只
有无头端；`savestate_flags` 无头可 STRICT、零售恒 0；零售 `nav_poll` 是
`enable && !input_muted`（`physics_session_client.cpp:2927`），要按调用时读（回调或
`const bool*`）；`on_correction` 无头端对 kind 1/5/6 要 checkpoint、零售只对 1
（`session_client.cpp:3079` vs `physics_session_client.cpp:2964`），保留为各自回调，不统一。

落地判据：`grep "world\.set_body = "` 只命中 `bind_rollback_world`；mod 工程用
`physics_view_ref` 实例化模板并能链接（任何直接调 `bmmo::physics::` 的残留都会链接失败）；
`BallanceMMOMessageTests` 加一个假 Backend 的测试，逐 hook 断言调用了哪个方法、
`wake_mode` / `recheck` / flags / 缓冲大小（现有 197 个测试不碰 CK 头，证明不了这些；模板
核心因此要**不含 CK 类型**——脚本部分放到选项里的类型擦除回调或第二个头文件）。

## 3. SimTool 驱动引擎还差的缝（S3）

已核实引擎 API 足够：`record` / `amend_record` / `rewind_to` 签名与用法一致；重放时按
anchor+1..current 的顺序每 tick 恰好一次 `step`，紧前 `pre_step(t)`；record 在 tick 边界、
步进之前调用，于是 `record(t)` 对应 `straight[t-t0]`，anchor 0 也有记录。恢复顺序是
脚本 → `set_nav` → `set_savestate` → 停放（`rollback.hpp:677-709`）。还差：

1. **链式回合会污染引擎历史**：重放中每个 tick 都 capture 并 `*recorded = state`
   （`rollback.hpp:965-987`），后一回合若锚在前一回合窗口内，恢复的是上一回合重放出来的
   记录而不是直通段的；一格失败会以 restore identity 的形式连锁失败。改法：直通段结束后
   `const rollback_engine pristine = engine;`（成员都是 std 容器 / POD），每回合前
   `engine = pristine`。内存约 50 MB（每条记录一份 blob，默认 `--savestate-every 1`
   harness 也存一份）加一份副本。
2. **`rewind_to` 返回 true 不等于发生了重放**：锚点无记录才返回 false；窗口超
   `max_resim_ticks` 走硬设并丢历史、`simulating()` 为假走 frozen、blob 被拒静默退回逐体、
   `step` 失败只 break。每回合后用 `stats()` 的增量判定（`savestates_restored`、
   `savestate_failures`、`too_far`、`frozen`、`resim_ticks == d`）加 step 包装里自己的失败
   标志，blob 被拒后停止后续回合。工作树已设阈值并检查 `savestates_restored` 增量
   （:1948-1957），照此写死。
3. **每步观测看到的是出生刚体复位之前的世界**：step 包装在 `set_body(..., true)` 复位之前
   触发（`rollback.hpp:935` vs `:944-951`）。没有出生刚体的场景两者相同；涉及出生的格改读
   `rewind_to` 返回后的 `engine.recorded_savestate(t)`。restore identity 在
   `before_replayed_tick(anchor+1)` 里抓（`execute_frame` 之前），`d == 0` 时在 `rewind_to`
   返回后抓。
4. **负控制换了含义**：`set_exact_restore(false)` 是客户端 `--no-savestate` 的逐刚体路径，
   与旧 `restore_legacy_anchor` 至少十处不同（不回拨时钟而叠 calm_shift；不写管理器三个
   浮点；不写 RNG 游标；按记录 wake/freeze 而非 keep，freeze 丢排队冲量；移动超 1 mm 就
   recheck；重建接触；UNIT 单独一遍）。计划明确采用这个新定义：负控制 = 客户端逐体路径，
   并让 `savestate_check.py` 检查它预期的**首个失败字段**，不只检查 FAIL。
5. **导航**：`physics_world` 自己持有 `player_navigation`（`physics_world.cpp:568, 698`，
   `players_` 私有），不在桥接 registry 里，绑定的 `navigation_*` 对 SimTool 克隆球静默返回
   false，引擎不检查返回值（`rollback.hpp:916-919`）。
   - (A) `physics_world` 改用 registry：registry 只 apply 本 tick 有 pending 输入的条目、
     按实体名顺序、在自己的回调里；`physics_world` 每 tick 按玩家 id 顺序、在
     `park_retail_ball` 之后 apply。两个玩家同 tick 建控制器时创建顺序会变——这是**权威
     服务器的行为变化**，需要单独一次服务器 journal 逐位验证。高风险后续项。
   - (B) 事后覆盖 `get_nav/set_nav/nav_input`（绑定返回值，可覆盖，和 `step` 一样），需要
     `physics_world` 新增公开 API：实体名 → 玩家 id、导航 `get_state/set_state`；
     `nav_input` → `set_input` 只存值，靠 step 包装里的 `pre_simulate` 生效；每 tick 要把
     全部玩家输入记进 `inputs_applied`。
   - 先 (B)；现有三格场景（spawn / drop / sector）的球没有输入，(B) 之前它们已经正确。

## 4. 验收标准（修正）

- 行为等价的标准不是 journal 逐字节相同——`.bmjr` 头带 utc_ms、服务器分配的 session /
  player id、build_id、tick base，每条 TICK 记录带墙钟毫秒，correction 记录带 local_tick
  （`journal.hpp:101, 151`；`session_client.cpp:2957-2969, 3067`）。标准是：
  `worst_bodies.py` 全部 0.0 / never，`exact_difference` 为 0.0，且归一化 id 与 tick base 后
  逐 tick 的 TICK 哈希 / 姿态序列和 correction kind 序列相同。臂用 `--pairs 20:12,7:5,50:48`
  （脚本默认是 50:12,97:24,131:47，要写清）。
- 零售端：竞技场一局带回滚的会话，journal 经 `--replay-session` 与服务器 replay 一致。
- `savestate_check.py --quick` 结果不变（physics 三格 PASS、legacy 格 FAIL 且首个失败字段
  符合预期）。

## 5. 新场景（S4，按可达成的判据重写）

- **窗口内出生**：blob 恢复不会销毁锚点之后出生的刚体，它被停放成惰性、只在出生 tick 用
  beam + recheck 复位（`rollback.hpp:796, 944-951`），所以出生之前的重放 blob 比直通段多一个
  停放刚体，复位靠 recheck 重建碰撞状态而不是还原 live 状态——**按构造做不到逐字节一致**。
  这一格的判据改为：出生 tick 之后的世界哈希 / exact cores，排除该刚体或设容差；同时写明
  测的是"本地记录的停放 / 复位"这条路径（迟到的远端出生走的是 `drain_pending_births`，
  是另一条）。真正逐字节的做法是"出生 tick 的部分 savestate 恢复"，单列为后续项。
- **重放后第一个 live tick 的按键沿**：引擎只对 `own_entity` 非空且 `own_polls` 的 tracked
  调 `nav_poll`，SimTool 场景 own 为空；`physics_world` 没有轮询；重臂在调用方。可达的做法：
  先做 §1 的 #4（重臂进绑定），再在 (A) 下给 SimTool 建一个轮询模式的 registry 条目、往
  null input manager 写键状态，并在重放后跑 live tick 之前把 `world` 的 step_state 同步到
  重放后的 RNG 游标（否则 `world->tick()` 用过期的 `rng_cursor_` 覆盖，`physics_world.cpp:899`）。
  (A) 之前这一格不做；该 bug 的回归由竞技场 7:5 臂守住。

## 6. 顺序与规模

S1（§1 七条，含 `after_replay`）→ S2（模板化 + `physics_direct` + `physics_view_ref` + 假
Backend 测试 + mod 编译）→ S3（§3 的 1-4，(B) 的 API）→ S4（§5 第一格）→ 文档
（`ivp-savestate-design.md` 的 Tests / plumbing 两节、`engine-changes.md` 桥接笔记）与记忆。
S1-S2 净新增约 250 行，S2 删零售端约 180 行绑定。

## 7. 实施状态（2026-10-21 晚起，S1-S3 收口于 10-22）

- S1 **done**（§1 七条）：`nav_poll` 总是调后端，键码表原样传（关闭不需要键码）；重放后的重臂
  进了绑定——`rollback_binding_options::on_poll_resumed(entity)` 在轮询被重新打开时回调，无头端
  挂 `rearm_own_navigation()`，零售端挂相机行重臂，两端的手工重臂都删了；脚本门统一为
  `scripts && context && !scripts->empty()`；`before_replayed_tick` 非空就装 `pre_step`（无头的
  `drain_pending_births` 不再依赖脚本开关）；`on_write_failed` 错误槽（零售写 `last_error`）；零售的
  savestate 日志计数改成 `physics_session` 成员并在 `reset()` 清零，`script_reset_ticks` /
  `script_reset_skips` / `scripts_replayed` 一并清。
- S2 **done**：模板核心 `session/rollback_binding.hpp` 不含任何 CK 类型；脚本部分拆成
  `session/rollback_binding_scripts.hpp`（`script_binding_options` + `bind_script_replay(world,
  backend, o)`，把脚本帧包在 `pre_step` 外面）；三个调用方（`session_client.cpp`、`sim_tool.cpp`、
  `physics_session_client.cpp`）都是先 `bind_rollback_world` 再按条件 `bind_script_replay`。
  `BallanceMMOServer/tests/rollback_binding_test.cpp` 用假 Backend 逐 hook 断言（wake_mode /
  recheck、clock 只在 `restore_clock` 下装、savestate 开关与 outcome / flags、nav_poll 总到后端且
  只在打开时 resume、`input_muted`、nav active 位、step 的 delta 与 `whole_frame`、`simulating`、
  接触 16 项上限与 `on_contacts_restored`、`pre_step`、选项按值拷贝）：12 个用例，全部 211 个
  测试通过。零售 dropped / unknown 缓冲改 16 KB / 32 KB，溢出时桥接返回失败而不是截断。
- S3 **done（§3 的 1-4）**：直通段后 `pristine = engine`，每回合 `engine = pristine`；回合用
  `stats()` 增量判定（`resim_ticks == d`、`savestates_restored` 等）；重放身份读
  `engine.recorded_savestate(t)`（legacy 读包装 `step` 的 blob）；`savestate_check.py` 的 legacy 格
  要求首个失败字段恰为 `restore identity`，否则该格不算通过。
  §3.5 的导航 (B) **todo**：`physics_world` 要新增 `player_of_entity(name)`、
  `navigation_state(id, out)` / `set_navigation_state(id, in)`（转 `player_navigation::get_state /
  set_state`），`nav_input` 走 `set_input(id, frame)` 只存值、靠 step 包装里的 `pre_simulate` 生效，
  SimTool 在 `bind_rollback_world` 之后覆盖 `get_nav / set_nav / nav_input` 三个 hook，并把全部
  玩家输入记进每 tick 的 `inputs_applied`。现有三格场景没有输入，(B) 之前已经正确。
- S4/S5 **todo**：见 §8 的验收实跑。
- 验收 1：`grep "world\.set_body = "` 只命中 `rollback_binding.hpp`。
- 顺带的工具：`BallanceMMOSessionClient --dump-savestate A B DIR` 现在同时写 `<t>.scr`（该 tick
  的脚本状态），`--compare-scripts DIR` 把本次运行每 tick 的脚本状态与 DIR 里另一次运行的比较，
  首个差异（脚本 / 块 / 字段）每次变化记一行——两次运行的脚本状态在哪里分叉，journal 和 blob
  diff 都看不到的那一半；`BallanceMMOSimTool --savestate-diff / --savestate-describe` 仍管 IVP 那一半
  （`--savestate-diff A B --ignore-ov` 跳过 overlap tree 看下一个差异；`--describe-script NAME` 打印
  脚本图：块、原型、id、flags、链接与延迟、全部参数值）。
- 用这套工具找到并修了一个脚本回放保真 bug：`restore_parameter` 用 `SetValue` 写输出参数，
  `CKParameterOut::SetValue` 末尾的 `DataChanged()` 把值复制进它的全部 destination（模块脚本图的
  local 就是子块输出的 destination），于是 Identity 块停放的输出把刚恢复的 `TurnON_Permission`
  又改回 0，风扇再也不吹纸球——20:12 臂纸球 0.32 m、Platte 3 cm 的全部来源。改成
  `GetWriteDataPtr()` + memcpy（无通知）。详见 lockstep-without-snapshots.md 对应小节。

## 8. 验收实跑（2026-10-22，`scripts/savestate_check.py --journals --server 127.0.0.1:26700 --pairs 7:5,50:48` / `20:12`）

脚本恢复的参数传播 bug 修掉之后（§7 末条）：

| 臂 | forced | 与 control 不同的刚体 | 备注 |
|---|---|---|---|
| 7:5 | 348 | **1 / 53**：只剩 `P_Modul_08_Schaukel`（窗口内出生，1814 激活，1848 起 2.1 m） | 球、纸球、Platte、全部机关 0.0 / never |
| 20:12 | 122 | 11 / 53：Schaukel + 纸球 0.18 m + Platte01-09 ≤ 2.5 cm，首次 >1e-6 m 在 tick 924 | 修前纸球 0.32 m、首次 910 |
| 50:48 | 48 | 11 / 53：同 20:12 的集合与数值 | |

- 引擎自己的脚本回放检查（"the scripts re-executed to tick N differ from the live record"）整局静默
  （修前每局 3-4 次：TurnON_Permission、UFO Referential）；`--compare-scripts` 显示臂的 live
  记录与 control 从 tick 8 到结束逐 tick 相同（958 起只有一个 float：Platte 近邻传感器的距离
  跟着下面的物理差异走）。
- 剩余差异的来源（`--savestate-diff --ignore-ov --max-diff`）：臂在 tick 900（重放记录）与
  control 的 blob 除三个簿记字段外完全相同——overlap tree 布局（自第一次跨出生的回滚起多 2 个
  节点）、sim unit 顺序、cache reuse index——而 901-908 的 live tick 在 `A_Floor_009` 的 hull
  条目（Platte 对地面的 mindist）上出现 1-2 ulp 的差异，920 回滚后 924 起放大。**IVP 的
  链表/树顺序在堆叠里是有数值意义的**；来源是窗口内出生的刚体：恢复时停放、出生 tick 步进后
  `recheck` 回树，拿到的树节点、unit 槽位、mindist 创建时刻都和 live 的 Physicalize 不同。
  7:5 的窗口只有 5 tick，跨 831/832 出生的那次回滚没把差异带进 924 的那场碰撞；深窗口会。
- 下一步（不在本批）：窗口内出生的刚体在重放里**按 live 的方式重建**——恢复时 unphysicalize，
  出生 tick 的 pre_step 用 Physicalize 自己的参数与姿态（引擎事件日志记下的）`physicalize()`，
  出生记录的 re-pose 只做校验；同时给无头客户端加上"重放身份"在线检查（最后一个重放 tick 的
  blob 与 live 记录逐字节比较，不同则记字段）。
- `--quick` 5/5 格如预期；x86 零售客户端 CLIENT-BUILD-OK；211 个单元测试通过。

## 9. 下一阶段设计：窗口内出生的刚体在重放里重建（草案，2026-10-22）

目标：跨出生的回滚也逐字节；§8 的残差（树布局 / unit 顺序 / cache 游标，以及由此在堆叠里
放大的 ulp）全部来自"停放 + 出生 tick 步进后 recheck 回树"和 live 的 Physicalize 走的不是
同一条插入路径。

1. **出生记录**：引擎事件日志（`event_log_listener`）现在只记 `created <name>`；让 physics_RT 的
   Physicalize 块在创建时把完整参数（`bmmo_physics_ball_recipe` 的全部字段：fixed / frozen /
   collision / mass center / friction / elasticity / mass / damp / collision_surface / convex /
   ball / concave 名）和实体当时的世界矩阵一并写进日志行（physics_RT 侧改动，不碰 IVP）。零售端
   同一信息来自 BML 的 `OnPhysicalize`。客户端按 tick 存 `local_births_[tick] = {name, recipe,
   matrix, group}`，随历史裁剪。
2. **引擎**：`rollback_world` 新增两个可选 hook：`destroy_body(entity)` 与
   `recreate_body(entity, birth_tick)`（后者由调用方用出生记录 `physicalize()`）。两者都装上时，
   planning 的 birth 分支从"停放惰性 + 出生 tick 步进后 re-pose"改为：恢复时 `destroy_body`
   （IVP 对象消失，CK 实体留着，脚本的力控制器由脚本恢复的 force hook 按锚点记录处理），
   `pre_step(birth_tick)` 里、在 `drain_pending_births` 与 `execute_frame` 之前 `recreate_body`；
   出生 tick 步进之后仍与出生记录比较——相同则不写（预期），不同则按旧路径 re-pose 并计数
   （`birth_recreate_mismatches`）。没装 hook 的调用方（当前零售端）保持停放路径。
3. **为什么能逐字节**：live 的 Physicalize 发生在该帧脚本遍历里、Simulate 之前；重放的
   `recreate_body` 也在该 tick 的脚本帧之前、步进之前，走同一个 `CreatePhysicsObject` 路径——
   同样的 OV tree 插入、同样的 sim unit 追加、同样的 cache 槽位、同样的 mindist 创建时刻。剩余的
   帧内顺序差（同帧里别的脚本在 Physicalize 之前/之后移动了刚体）是二阶的，用 §8 的工具可量。
4. **SimTool 场景（§5 第一格）**：`--savestate-check` 加 `--birth-at TICK ENTITY`（在直通段某 tick
   `physicalize` 一个 prop），`physics_direct` 后端提供 destroy/recreate，判据回到逐字节。
5. **顺序**：先用新加的在线"重放身份"检查（`savestate_replay_mismatches`）确认当前客户端的重放
   在无出生窗口里是逐字节的，再做 1-4。**已确认（2026-10-22）**：20:12 臂 123 次回滚，
   `sv=123/0/0`，"savestate replay:" 一行都没有——live 客户端里每次重放的最后一个 tick 的 blob 与
   被它覆盖的 live 记录逐字节相同（blob 里不再记 bmmo 自己的 PreSimulate 回调：键盘轮询回调在
   重放期间按设计是关着的，带着它的 blob 永远和自己的重放对不上）。所以堆叠的漂移不是重放失真，
   只能是 §8 说的顺序字段。

