# CH592F 3.1.41 候选版本变更报告

## 发布定位

- 整理日期：2026-08-21
- 目标仓库：`MeowKJ/BinaryKeyboard`
- 上游基线：`binarykeyboard-20260818`（`b5ca94e`）
- 基线 CH592F 版本：`3.1.40`
- 建议发布版本：`3.1.41`
- 适用设备：Wireless 5KEY / Wireless KNOB
- 关联问题：#75 KNOB RGB 顺序错误、#76 FN 长按逻辑优化

本报告只描述相对于当前上游 `main` 的候选改动。ADC、电池与充电状态修复已经通过 PR #91 进入 `binarykeyboard-20260818`，不应在本次 Release 中重复描述为全新功能；本次仅补充 ADC 分压的最终低功耗策略。

## 用户可见变化

### 1. 重构 LIGHT / DEEP 休眠流程

- LIGHT 期间由 WCH BLE 调度器在连接事件之间安全低功耗运行，不再由应用层直接进入 CPU Idle。
- LIGHT 保持 BLE 连接；`DEEP=0` 表示禁用 DEEP，不会让设备在 LIGHT 后立即断开。
- DEEP 时间定义为进入 LIGHT 后的追加等待时间。例如 `LIGHT=1`、`DEEP=1` 表示最后一次有效操作约 1 分钟进入 LIGHT，再约 1 分钟进入 DEEP。
- 修复 BLE 调度 RTC 与 DEEP 计时共用造成的断线、循环退出 LIGHT 和无法进入 DEEP。
- 修复无 USB 连接时悬空 USB 中断、无效 GPIO 标志和旋钮无效边沿错误唤醒 LIGHT。
- DEEP 入口增加持久唤醒标记，并在外设初始化清除 GPIO 标志前保存真实唤醒源。

### 2. 增加“LIGHT 无感唤醒”设置并保护 DEEP 重连

- Studio 电源设置新增 LIGHT 首键开关，默认开启。
- 开启时，LIGHT 唤醒普通键继续执行原操作；关闭时，LIGHT 首键只负责唤醒。
- LIGHT 下按键沿继续走正常输入路径，不合并多次操作。
- DEEP 首键和 BLE 重连期间输入全部丢弃，不在连接恢复后回放。
- DEEP 等待链路加密和 Keyboard Input Report 通知稳定 200 ms 后，先可靠发送全零键盘报告，再开放新的输入。
- BLE 报告暂时失败时保留最新完整键盘状态并重试，避免 PRESS 已发送而 RELEASE 丢失导致主机持续连发。
- 消抖解锁时重新采样 GPIO，补齐发生在 10/15 ms 锁定窗口内的真实边沿；队列满时优先保留 RELEASE。

### 3. 优化 FN 长按（#76）

- FN 达到配置的长按阈值时立即执行，不再等待用户松开。
- 每个 FN 使用自身配置的 `long_press_ms`，不再固定使用底层 800 ms 常量。
- 阈值前松开发送 CLICK；阈值后只发送一次 LONG，松开不重复触发。
- FN 长按动作若为休眠，会抑制同一次按键松开造成的立即唤醒；进入 DEEP 前等待该 FN 松开。

### 4. 修复 KNOB RGB 和 EC11 映射（#75）

- KNOB 板的 K1/K2、K3/K4 RGB 物理顺序统一在板级映射层修正。
- 静态灯效、彩虹、按键响应和层提示共用同一物理 LED 顺序；5KEY 不受影响。
- Studio EC11 虚拟键索引改为与固件一致：`4=顺时针`、`5=逆时针`、`6=按下`。
- 增加布局回归测试，防止旋钮按下、顺时针和逆时针再次错位。

### 5. 配置加载与兼容性

- 没有有效 DataFlash 槽时，默认配置会在首次启动后落盘；已有用户配置不会被覆盖。
- LIGHT、DEEP 和 FN 长按阈值均从设备实际配置加载，并在 Studio 写入后即时更新。
- DataFlash 配置结构仍为 64 字节，不提升布局版本，不清空现有键位、FN、RGB 或休眠设置。
- LIGHT 无感唤醒使用系统配置保留字节和显式标记值；旧配置中的非标记值迁移为默认开启。
- RGB/电源读取响应从 13 字节扩展为 14 字节。
- 新固件兼容旧 Studio 的 12 字节设置包，也接受带无感唤醒开关的 13 字节设置包。
- 新 Studio 根据设备响应能力选择 12 或 13 字节设置包，仍可配置旧固件。

### 6. ADC 与维修诊断最终策略

- 外部 VBAT 分压改为按需开启：采样前打开 PA15，完成后关闭，降低持续功耗。
- CH592F ADC 模拟域保持开启，并未完全断电。
- ADC 原始值、电池电压和 CHRG 原始状态诊断保持可用，维修时仍可判断主控采样链路是否运行。

### 7. 构建与刷写可靠性

- 修复完整固件构建指定版本号时只改文件名、未同步设备内版本头的缓存问题。
- 增加 5KEY / KNOB 独立休眠测试预设；每 10 秒状态闪灯只存在于测试构建，正式固件不包含周期闪灯。
- 完整 HEX 包含 `0x00000` JumpIAP、`0x01000` App 和 `0x6D000` IAP，可用于 WCHISP 首刷或恢复。

## 兼容性说明

| 项目 | 结果 |
| :--- | :--- |
| 旧 DataFlash 配置 | 保留并迁移，无需恢复出厂设置 |
| 新固件 + 旧 Studio | 可用；旧 Studio 不显示无感唤醒开关，发送旧长度配置 |
| 旧固件 + 新 Studio | 可用；Studio 根据响应长度不发送新增字段 |
| 5KEY / KNOB | 共用电源与唤醒逻辑；RGB 映射修复仅对 KNOB 生效 |
| USB / BLE | 普通输入只在 USB CONFIGURED 或 BLE HID 真正可输入后开放 |

## 已完成验证

- `release-5key` 编译通过。
- `release-knob` 编译通过。
- `debug-5key-sleep-test` 编译通过。
- `debug-knob-sleep-test` 编译通过。
- Studio 类型检查通过，生产构建通过。
- Studio 协议与布局测试共 74 项通过。
- 四份 full HEX 均已生成并检查关键地址段。
- `git diff --check` 无空白错误；仅有 Windows 工作区的 LF/CRLF 提示。

## 发布前必须完成的实机门槛

当前状态应标记为 Release Candidate，而不是直接标记稳定版。主仓库 Release 前至少完成以下矩阵：

| 场景 | 5KEY | KNOB |
| :--- | :---: | :---: |
| LIGHT 保持 BLE 连接 30 分钟 | 待测 | 待测 |
| `DEEP=0` 两小时内不进入 DEEP、不循环退出 LIGHT | 待测 | 已初步验证，建议复测 |
| `LIGHT=1, DEEP=1` 按预期进入 DEEP | 待测 | 已初步验证，建议复测 |
| Android DEEP 首键和重连期间输入不回放 | 待测 | 待测 |
| iOS DEEP 首键和重连期间输入不回放 | 待测 | 待测 |
| LIGHT 无感开关的开启/关闭行为 | 待测 | 待测 |
| 快速短按和 BLE 压力下无持续卡键 | 待测 | 待测 |
| FN 短按、长按阈值触发、长按休眠 | 待测 | 待测 |
| EC11 顺时针、逆时针、按下映射 | 不适用 | 待测 |
| KNOB 四颗 RGB 物理顺序及全部灯效 | 不适用 | 待测 |
| 电池供电静置功耗与 ADC 周期采样 | 待测 | 待测 |

手机应用可能在蓝牙重连时自行移走输入焦点或调整输入法界面，固件无法恢复应用焦点。因此 DEEP 明确采用首键仅启动策略，不承诺蓝牙断链后的界面级无感恢复。

## 建议提交拆分

不要把当前工作区作为一个大型提交直接合入。建议按以下顺序拆分，便于审查和回退：

1. `fix(ch592): stabilize light and deep sleep transitions`
2. `feat(ch592): add configurable seamless wake replay`
3. `fix(ch592): trigger fn long press at configured threshold`
4. `fix(ch592-knob): correct rgb and encoder mappings`
5. `fix(ch592): switch battery divider on demand`
6. `fix(ch592-build): keep full image and embedded version in sync`
7. `test(ch592): add isolated sleep diagnostics presets`
8. `docs(ch592): document power behavior and release validation`

## 版本与发布建议

- 上游 `3.1.40` 已对应 `binarykeyboard-20260818`，本次功能不应继续用相同版本号对外发布。
- 建议合并后由主仓库版本脚本统一生成 CH592F `3.1.41`，并创建新的日期 Release tag。
- 正式附件只发布 `release-5key` 和 `release-knob` 的 full HEX/在线升级产物。
- `sleep-test` 固件应作为诊断附件单独标注，不能替代正式固件，也不应进入默认在线更新清单。
- Release 页面应明确说明：旧配置保留、LIGHT 无感默认开启、DEEP 首键不回放，以及 `DEEP=0` 的新语义。
