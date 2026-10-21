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

## 7. 实施状态（2026-10-21 晚）

- S1 **done**：`session/rollback_binding.hpp`（`rollback_binding_options` +
  `bind_rollback_world<Backend>`）、`physics/physics_direct.hpp`（无头侧 Backend，方法名
  与 `physics_view` 一致，另带 `movable_body_names`）。
- S2 **done**：`session_client.cpp` 的 `rollback_world()` 只填选项 + `bind_rollback_world(physics_direct_, o)`；
  trace 经 `before_replayed_tick` / `on_savestate_restored`（读 `physics_direct_.last_report`）保留，
  `--replay-scripts` 经 `whole_frame`，`--trace` 的 step 诊断包在返回的 `step` 外面。
- S3 **done（编译验证）**：`physics_session_rollback_world()` 填选项（`input_muted`、键码表、
  logger、journal 的 `on_correction`）+ `bind_rollback_world(physics_view_, o)`；x86 零售
  客户端 `CLIENT-BUILD-OK`。竞技场实跑待做。
- S4 **done**：`--savestate-check` 的直通段 `engine.record`，回合 `engine.rewind_to`，恢复身份
  在 `before_replayed_tick` 抓 blob，重放身份在包住的 `step` 里抓；`--legacy-restore` =
  `set_exact_restore(false)`；`capture_legacy_anchor` / `restore_legacy_anchor` / `step_replay`
  已删。`--quick` 5/5 格如预期（含 frames 格 PASS，之前自复刻配方的 frames 格失败在
  P_Modul_18_Particle 的 DEACTIVATENEXTFRAME 标志，引擎配方下不再出现）。
- 验收 1：`grep "world\.set_body = "` 只命中 `rollback_binding.hpp`。
- S5 **todo**：窗口内出生、重放后首个 live tick 按键沿两个场景（需要 §3.3 的导航 (B)）。
- S6 部分：`ivp-savestate-design.md` / `engine-changes.md` 的桥接笔记已指向新头文件。
- 顺带的工具：`BallanceMMOSessionClient --dump-savestate A B DIR` + `BallanceMMOSimTool
  --savestate-diff / --savestate-describe`，把两端 journal 的位置漂移追到具体引擎字段。
