# 休眠可视化测试

## 测试固件

测试预设：

```text
debug-5key-sleep-test
debug-knob-sleep-test
```

也可通过统一构建脚本生成完整镜像：

```text
python tools/scripts/targets/ch592/build.py build-full -k 5KEY --profile sleep-test
python tools/scripts/targets/ch592/build.py build-full -k KNOB --profile sleep-test
```

使用 WCHISPStudio / ISP 烧录时必须选择完整镜像：

```text
firmware/CH592F/build/debug-5key-sleep-test/CH592F-5KEY-sleep-test-full.hex
firmware/CH592F/build/debug-knob-sleep-test/CH592F-KNOB-sleep-test-full.hex
```

`CH592F.bin` 是链接到 `0x1000` 的应用镜像，不包含地址 `0x0000` 的
JumpIAP。它只能用于 Studio 在线更新，不能作为 WCHISP 首刷或救砖文件。
如果误用 ISP 从地址 0 写入该文件，设备将无法启动；重新进入芯片 ROM
下载模式并刷对应的 `-sleep-test-full.hex` 即可恢复。

## 指示灯含义

- 未达到设备实际配置的 `LIGHT` 时间时，第一颗指示灯每累计 10 秒蓝青色短闪一次。
- 10 秒闪烁只读空闲计时，不会刷新或延长休眠计时。
- 到达 `LIGHT` 时间后，执行蓝青色三次闪烁，然后关闭 RGB 并进入 LIGHT_SLEEP。
- 到达 `LIGHT` 时间但 USB 已完成枚举时，改为每 10 秒红色短闪，表示 USB 阻止休眠。
- 到达 `LIGHT` 时间但检测到正在充电时，改为每 10 秒黄色短闪，表示充电阻止休眠。
- 如果 `LIGHT=0`（自动休眠关闭），每 10 秒紫色短闪。
- 到达 `DEEP` 总时间后，BLE 模式主动断开并执行深度休眠。

测试时应断开 USB、停止充电，并确认实际配置不是 `0/0`。默认配置为 `LIGHT=1`、`DEEP=1`，所以通常约 1 分钟进入 LIGHT，约 2 分钟进入 DEEP。

## LIGHT 保持 BLE 连接测试

将 `LIGHT` 设为 `1`、`DEEP` 设为 `0`。蓝青色三闪表示已进入 LIGHT，之后主机应始终显示 BLE 已连接，不应转为重新广播或要求重新连接。按键必须立即唤醒设备，并按“无感唤醒”设置决定首键是否发送给主机。

`DEEP=0` 表示完全禁用 DEEP，不表示进入 LIGHT 后立即进入 DEEP。如果蓝青色三闪后 BLE 断开，则 LIGHT 保活仍然异常。

使用 `LIGHT=1`、`DEEP=1` 时，DEEP 是进入 LIGHT 后追加 1 分钟，正常情况下应在最后一次真实操作约 2 分钟后进入。BLE 定时唤醒和无效 GPIO 标志都不应退出 LIGHT，也不应让 10 秒测试计时重新开始。

退出 LIGHT 后恢复的绿色是 BLE 已连接状态指示，不是额外的休眠阶段。如果完全无操作时出现绿色并重新开始每 10 秒蓝青色计时，表示 LIGHT 被意外输入唤醒。

BLE 模式会保留 USB Device 以便插线使用 Studio，但没有完成枚举的 USB 标志不能退出 LIGHT。测试期间未连接 USB，却在每轮 LIGHT 后固定出现绿色，优先检查所刷镜像是否包含“未枚举 USB 伪唤醒过滤”修复。

“LIGHT 无感唤醒”只控制 LIGHT 首键。开启时 LIGHT 唤醒键正常执行，关闭时首键只唤醒。DEEP 首键和 BLE 重连期间的全部输入始终只用于恢复设备，不会稍后补发。

手机端的 GAP“蓝牙已连接”早于 HID 真正可输入。DEEP 恢复会等待加密和 Keyboard Input Report 通知均已恢复并稳定 200 ms，然后可靠发送全零报告；报告成功前不开放新按键，避免 PRESS 成功而 RELEASE 丢失造成持续连发。

## 正式固件

`release-5key` 和 `release-knob` 不定义 `KBD_SLEEP_TEST_MODE`，不会产生每 10 秒的测试闪烁，也不会因为测试模式增加持续功耗。

## 配置加载

设备没有有效 DataFlash 配置时，固件加载默认 FN、休眠和 RGB 配置后立即保存一份默认配置。已有有效配置不会被首次启动逻辑覆盖，网页读取到的值应以设备 `RGB_GET` / `FNKEY_GET` 响应为准。
