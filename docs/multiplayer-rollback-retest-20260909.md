# 9.16 多人物理联机复测：仍存在持续回滚和输入停供

测试日期：2026-09-09，操作时间 10:08–10:27 UTC（本机日志为 UTC−4，即 06:08–06:27）。

**结论：在用户指定的五份客户端安装环境中，修复后仍存在多人动态场景的持续回滚。** 五个真实零售客户端连接已部署的 Linux 服务端，在第 8、11 关第一小节复现。第 8 关移动阶段 53.25% 的快照触发回滚；第 11 关退出关卡、重新加载后，关闭详细 trace 的移动阶段仍为 89.04%。另录到输入长期停供、重同步未恢复输入，以及重开时等待某位玩家 Ready 而整房不推进的问题。

**环境限制：客户端 4、5 还安装了其他 mod；没有完成全员仅装 BMMO 的对照，不能把所有异常直接归因于 BMMO 单独运行。** 用户要求停止补测后，已立即停止：仅为补测重新启动过客户端 1–3、连接并加载关卡，没有创建新的物理会话，没有产生新的测试结论或 journal。下文只使用已完成的 session 1–5。

本次只部署和测试，没有修改运行逻辑，也没有重启、更新或改变生产服务器配置。

## 版本、环境和证据目录

- 仓库：`e72266bddca7893870a935884a793cb6e471192b`，collision-overhaul 9.16。
- Ballanced：`9cc27f74941ac453839be09d150bbb2407f206e9`；CKBuildingBlocks：`0bb5e8ca6caf49567ad423bc378263cd59fc3d53`；物理桥 API 7。
- 服务器：SSH `mc@lm.okbc.st:46981`，游戏 UDP `lm.okbc.st:46995`，运行目录 `/home/mc/bmmo`，进程 PID 76224。
- 服务端 build id：`ballanced-9cc27f74941a+bmmo-e72266bddca7`。
- 客户端实测 build id：`ballanced-9cc27f74941a+bmmo-e72266bddca7-dirty`。构建前后没有已跟踪源码差异；已有未跟踪的 `scripts/__pycache__/`、`vc140.pdb` 被保留。引擎版本通过服务端准入。
- BMLPlus 0.3.12，Windows x86 零售 `Player.exe`。客户端 1–3 只有 BMMO 和运行时依赖；客户端 4–5 另装有 `BallSticky.zip`、`BMLModuls.zip`、BLinguist、CameraUtilities、DebugUtilities、DeformedWB、DualBallControl、DynamicFov、MapScripts、Physics、SpiritTrail、TravelMode。完整文件及哈希见 `installed-mods.json`。这些附加 mod 未被移除或禁用，是现有结果的重要混杂因素。
- 服务器配置：`snapshot_interval=2`、`input_delay=10`（下限）、`spawn_impulse=3`、`event_rate_limit=100`、`journal_max_mb=256`、`journal_checkpoint_ticks=660`、`debug_trace=false`。
- 本轮会话根据约 278–280 ms RTT 使用 `input_delay=15 tick`（约 227 ms），没有另外注入延迟或丢包。
- 五客户端同机运行，使用游戏本身的命令文件接口注入正常方向键。没有用 `beam`、改坐标或 `sector` 强制切节来制造本次复现。

客户端编号对应：

| 编号 | Downloads 下目录 | 玩家 ID | 显示名 |
| --- | --- | ---: | --- |
| 1 | `Ballance-MMOTest` | 269501247 | Swung0x48 |
| 2 | `Ballance-MMOTest - Copy` | 956292139 | Player673_0 |
| 3 | `Ballance-MMOTest - Copy (2)` | 315249806 | Player6 |
| 4 | `Ballance-MMOTest - Copy2` | 3257029693 | BMMOTest4 |
| 5 | `Ballance-MMOTestCopy3` | 393696277 | BMMOTest5 |

五份安装均已替换并核对相同的两个文件：

| 文件 | SHA-256 |
| --- | --- |
| `ModLoader/Mods/BallanceMMOClient.bmodp` | `10177b8f7ebf5bf7e77d30f76463cf8f1199d651d6dad854eaafc44a700c6440` |
| `BuildingBlocks/physics_RT.dll` | `5c36c69fa2ceebf6a2b315c5d3bd87051387cf89504b02230611a7134f321880` |
| Linux `BallanceMMOServer` | `ad7c2b9270b523638e503870a313595ca13c889b047056775f172bc5d7229923` |

构建沿用可用的 VS18 x86 工具链，通过已有 `build-main-vs18.py` 构建 `build-client-stock` 的 `BallanceMMOClient physics_RT` 两个目标，退出码 0。原二进制、配置、旧日志、旧 journal 备份在各 `client-N/before/` 中，未混入本次证据压缩包。

完整证据目录：`D:/repos/BallanceMMO/build/live-rollback-retest-20260909/`。

- `BallanceMMO-rollback-retest-20260909.zip`：报告、**25 份 journal（20 客户端 + 5 服务端）**、五份最终 ModLoader.log、服务端日志、命令时间线、截图、统计与回放输出。
- `client-N/capture/`：该客户端本次 `.bmjr` 和最终 `ModLoader.log`；`transcript.jsonl` 保存每条命令的 UTC、耗时及未截断答复。
- `server/`：五份权威 journal、服务端日志、四份完整回放日志、远端 SHA-256。
- `phase-metrics.json`：以每个阶段开始/结束时的计数器差值统计，避免长时间空闲稀释回滚率。
- `metrics.json`、`journal-validation.txt`：直接解析 journal 的事件、回滚主体、误差、深度、输入和完整性结果。
- `trace-L8-t8958.txt`、`trace-L11-t6900.txt`、`trace-L11-t8500.txt`：客户端和权威记录按 tick 合并的重点片段。
- `SHA256SUMS.txt`：压缩包内证据的校验清单。

## 复现方式和阶段结果

1. 启动上述客户端，以各自独立的 `BMMO_COMMAND_FILE` 控制；连接 `lm.okbc.st:46995`，`journal on`。
2. 从菜单加载目标关卡，确认每位玩家 `ingame=1 playing=1 sector=1`。
3. 房主 `mmo room create <name>`，其余 `mmo room join <id>`；全部 `mmo room ready`，房主 `mmo room start physics`。
4. 确认所有人 `assigned=1`、已收到快照、journal 正在录制，再测量。
5. 静止阶段执行 `keys clear`；移动阶段循环：**按上约 2 秒 → 松上按下约 2 秒 → 全部松开约 2 秒**。五人同步执行，每 10 秒采样 `status/session/panel`；阶段边界写 `journal mark`。
6. 第 11 关重载复测在关闭上个房间后，先执行 `message Exit Level`，再通过 `level 11` 从菜单重新加载。这是同一组进程中的重新加载，不能视为重启进程后的冷启动测试。

脚本：证据目录中的 `ctl.py`、`exercise.py`；例如 `python exercise.py L8_5p_move --seconds 65 --mode forward`。连接、开房、关卡加载等前置操作的精确顺序在各客户端 `transcript.jsonl` 中。

下表多人数值均为五客户端相加。“触发率”是新增 `mism/snaps`；`rb` 计数在实现里也包含超过最大深度而只恢复状态的情况，`resim` 才是实际执行的回放 tick。journal 的 `kind=rollback` 则只统计实际进入回放路径的记录。

| 阶段 / session | 测量时长 | 触发修正 / 快照 | 触发率 | 实际重模拟 tick | 新增 resync |
| --- | ---: | ---: | ---: | ---: | ---: |
| 第 8 关单人 / 1 | 约 49 秒 | 0 / 1,635 | 0% | 0 | 0 |
| 第 8 关五人静止 / 2 | 35 秒 | 4 / 5,789 | 0.069% | 87 | 0 |
| 第 8 关五人移动 / 2 | 65 秒 | 5,874 / 11,030 | **53.25%** | 123,208 | 0 |
| 第 11 关五人静止 / 3 | 30 秒 | 0 / 4,973 | 0% | 0 | 0 |
| 第 11 关五人移动 / 3 | 65 秒 | 7,354 / 10,744 | **68.45%** | 44,495 | **5** |
| 第 11 关同场关闭 trace 后继续移动 / 3 | 40 秒 | 6,635 / 6,789 | **97.73%** | 120,529 | 0 |
| 第 11 关重新加载、trace 关闭 / 5 | 80 秒 | 12,074 / 13,560 | **89.04%** | 252,711 | 0 |

单人第 8 关包括约 11 秒向前移动、一次死亡重生，结束采样有 48 个 unmatched，不能写成“所有快照完全一致”。多人阶段也包含自然死亡重生；第 11 关部分客户端随后进入非游戏状态，这部分有完整 status 记录。所有测量都发生在第一小节；截图确认第 8 关已行至起点后的窄桥，第 11 关已行至起点后的道路/斜坡。**没有完成整个第一小节或整关，也没有把沙袋的回滚记录说成已目视确认玩家碰到了沙袋。**

## 问题 1：运动过程中持续回滚，绳索/沙袋也参与分歧

优先级建议：P1。期望是在短时输入预测分歧后收敛；实际反复恢复并回放，且并非只有初始化或死亡时出现。

第 8 关 session 2：

- 静止时基本收敛，一旦开始移动，65 秒内平均每客户端约 1,175 次修正。
- 客户端 1 的该阶段实际 rollback 深度中位数 24、P95 25、最大 39 tick；其余客户端典型约 20–21 tick。
- 主体为本地及远端木球。多数修正较小，但存在移动期间的米级异常：**服务端 tick 8958 / 客户端 local tick 8984，`Ball_Wood_Peer_393696278` 位置误差 13.0596 m、速度误差 2.5052 m/s**。本地历史位置约 `(64.0701,35.2685,-0.04345)`，权威位置约 `(77.1289,35.1256,-0.04345)`。查看 `trace-L8-t8958.txt`。
- 该阶段还有 724 个 unmatched、3 次 too_far。整场 `max_err=59.3707 m` 出现在早期，不能拿它代替稳定移动阶段的典型误差。
- 服务端 tick 240 后五人合计 62,535 条输入，只有 84 条未获得新输入（0.134%），最大连续 12 tick。不能用持续输入饥饿解释整个回滚阶段。

第 11 关 session 5（重载、trace 关闭）：

- 80 秒内 89.04% 快照触发修正；没有 resync，也没有 too_far，说明高回滚率可以独立于重同步问题存在。
- 客户端 1 的该阶段实际 rollback 深度中位数 21、P95 23、最大 40 tick。
- 主要分歧主体为 `P_Modul_26_Rope001`、`P_Modul_26_Rope`，同时有沙袋与玩家球。客户端 1 的 2,402 次实际回放中，1,282 次主体为 `P_Modul_26_Rope001`，196 次为 `P_Modul_26_Rope`。
- 该客户端阶段内位置误差中位数约 **3.3661 m**、P95 **3.7648 m**，不是单纯超过 1 mm 球容差的小修正。`trace-L11-t8500.txt` 可见多次绳索位置相差数米、速度差超过 200 m/s 的记录。
- 服务端 tick 240 后五人合计 51,560 条输入，仅 47 条没有新输入（0.091%），各人连续缺失最多 5–6 tick。全部 10,552 个权威 tick 可逐位离线重放。

这两组证据表明当前安装环境中客户端预测/恢复后仍无法持续贴合权威世界。**还没有定位到唯一根因，也未排除附加 mod 的影响**。现有回滚恢复逐刚体及导航历史，却不等于完整恢复 IVP 的接触、摩擦、约束与时钟内部状态；这值得跟进，但本次没有把这一推测当作已证实根因，也没有据此改代码。

## 问题 2：第 11 关输入先停供，随后重同步仍未恢复

优先级建议：P1。证据来自 session 3。客户端仍持续发送输入、保持连接，服务端对四个玩家长期使用旧输入；五个客户端后来各请求一次 `tick driver rebased` resync。

重要的先后关系：**权威输入的新鲜度已经在 tick 5719–5978 附近停止，不能说是稍后的 resync 导致了最初停供。** resync 完成后仍未恢复，才是本轮确认的恢复失败。

| 玩家 ID / 客户端 | 最后 fresh tick（四人未恢复） | 最长连续 nonfresh 范围 | 长度 |
| --- | ---: | --- | ---: |
| 269501247 / 1 | 5846 | 5847–16808 | 10,962 tick，166.1 秒 |
| 956292139 / 2 | 5719 | 5720–16808 | 11,089 tick，168.0 秒 |
| 393696277 / 5 | 5838 | 5839–16808 | 10,970 tick，166.2 秒 |
| 3257029693 / 4 | 5978 | 5979–16808 | 10,830 tick，164.1 秒 |
| 315249806 / 3 | 后来间歇恢复，最后 fresh=16765 | 5723–10177 | 4,455 tick，67.5 秒 |

客户端 1 的 journal 和日志链条：

- `tick 6900`：请求 resync，原因 `tick driver rebased`；日志本地时间 `06:19:10.255`。
- `tick 6937`：收到重新分配的 tick base；`06:19:10.568`。
- `local tick 7004`：从权威 full snapshot `tick 6986` 应用恢复；`06:19:11.568`。
- 随后的 `inputs_sent` 继续增加，到结束采样为 16,764；服务端 journal 仍显示 fresh 输入停在 5846。
- 服务端 `10:20:36 UTC` 的 `sessions` 输出显示 world 正在 tick 12567、5/5 ready、pending_events=0；多个输入队列的读取位置停在 resync 分配点，而接收计数持续增加。原始行保存在 `server/server.log`。

`panel` 的 Input late 百分比计数会跨本次连续会话累计；本报告采用每个权威 journal 的 fresh 标记、每场 `acked` 与命令采样确认停供，不把后续房间里遗留的百分比当作后续房间的丢包率。服务端队列 `late` 也包含重复/过期重发，不可直接换算为网络丢包。

## 问题 3：某成员未进入关卡，重开房间后无快照且不推进

优先级建议：P2。session 4，`Retest0916-L11-clean`，10:22:10–10:23:03 UTC；这次未成功开局，不纳入前面的回滚率。

- 上场结束后，客户端 3 已是 `ingame=0 playing=0`，仍保留 `map=Level_11`。五人 room ready 被接受并启动房间。
- 服务端只收到客户端 1/2/4/5 的物理 Ready；客户端 3 `phase=idle assigned=0`，没有建立客户端 journal。
- 其他四人显示 `phase=running`，但 `assigned=0 inputs_sent=0 snapshots=0`；本地 tick 已推进到 3252，服务端始终没有一个 TICK。
- 权威 journal 长度 **472 字节**，记录房间建立/结束而无 TICK。它是完整的失败会话记录，**不是坏文件或截断文件**。
- 关闭房间、让所有客户端退出关卡并重新从菜单加载，session 5 才恢复正常 5/5 ready 并收到快照。

需要跟进 Ready 的游戏状态检查、等待成员进入物理世界的超时/失败反馈，以及非游戏状态客户端接收 SessionStart 时的加载路径。这里只有一次失败记录，尚未验证所有触发条件。

## 本次没有复现的旧症状与验证边界

- 第 8 关 session 2 全场仅 21 个 BodyRevived，按连续 66 tick 分桶峰值 5；没有复现旧生产记录中每秒数百次的洪泛。
- 第 11 关 session 3 有 892 个 BodyRevived，主要来自两个沙袋各 426 次；session 5 有 578 个，峰值均为每 66 tick 50 次。新版保留显式 script_wakeup 的协议事件名仍为 BodyRevived，不能仅凭名字认定旧的普通 revived 回传缺陷回来了。
- 排除最初 240 tick 后，服务端最大单 tick 墙钟间隔：单人第 8 关 46 ms、五人第 8 关 188 ms、session 3 第 11 关 134 ms、session 5 第 11 关 165 ms。仍有短暂抖动，但未复现每次生命周期变更等待满一秒的旧症状。
- 五进程同机、约 280 ms RTT、顺序执行多个会话，其中两份安装还带有附加 mod；不是不同人数的严格性能 A/B，也不是全员仅装 BMMO 的隔离测试。测试覆盖起点、第一小节部分道路、自然死亡与重开；没有覆盖整个第一小节、整关或超过五人的情况。
- 关闭 trace 后在相同故障会话里继续出错只证明故障持续；另外的 session 5 在重载、trace 关闭、没有 resync 的情况下复现，提供了独立于 resync 的证据。没有据此声称已排除一切历史状态或同机资源因素。

## Journal 对应关系与完整性

| session | 内容 | 服务端 journal | 客户端数量 |
| --- | --- | --- | ---: |
| 1 | 第 8 关单人 | `session_1_level8_20260909101034.bmjr` | 1，时间戳 `101037` |
| 2 | 第 8 关五人 | `session_2_level8_20260909101158.bmjr` | 5，时间戳 `101201` |
| 3 | 第 11 关五人，包含 resync/输入停供 | `session_3_level11_20260909101722.bmjr` | 5，时间戳 `101725` |
| 4 | 第 11 关重开等待失败 | `session_4_level11_20260909102210.bmjr` | 4，时间戳 `102213`，客户端 3 无文件 |
| 5 | 第 11 关重新加载，trace 关闭 | `session_5_level11_20260909102336.bmjr` | 5，时间戳 `102339` |

客户端文件完整名称为 `session_<id>_level<N>_20260909<时间戳>_p<玩家ID>.bmjr`。服务端原件仍在 `/home/mc/bmmo/journals/`，本地副本另存；全部 25 份解析成功，无 truncated、dropped bytes 或 reader warning。

直接使用生产部署的同版 Linux SimTool，按原始权威 journal 完整离线回放：

| session | matched / ticks | first_divergence | 检查点 / 不匹配 | 退出码 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 3,251 / 3,251 | −1 | 5 / 0 | 0 |
| 2 | 12,747 / 12,747 | −1 | 20 / 0 | 0 |
| 3 | 16,809 / 16,809 | −1 | 26 / 0 | 0 |
| 5 | 10,552 / 10,552 | −1 | 16 / 0 | 0 |

session 4 没有模拟 tick，仅做格式和内容检查。服务端原始 journal 的完整回放一致性证明记录足以重复权威世界，**不证明世界物理行为正确，也不证明客户端回滚正确**。本次未做客户端 journal 的确定性回放结论。

示例回放命令（服务器上）：

```sh
LD_LIBRARY_PATH=/home/mc/bmmo/lib /home/mc/bmmo/BallanceMMOSimTool \
  --root /home/mc/bmmo4/ballance \
  --replay-session /home/mc/bmmo/journals/session_5_level11_20260909102336.bmjr \
  --report-every 1000
```

示例逐 tick 对照（仓库根目录）：

```text
python scripts/journal_trace.py build/live-rollback-retest-20260909/server/session_5_level11_20260909102336.bmjr build/live-rollback-retest-20260909/client-1/capture/session_5_level11_20260909102339_p269501247.bmjr --around 8500 --window 6
```

测试结束后全部测试客户端正常断开并退出、所有测试房间关闭，生产服务器仍保持原 PID 运行。新的 mod 和 physics_RT.dll 留在上述五个游戏目录，journal 录制保持开启。
