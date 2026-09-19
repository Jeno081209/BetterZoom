[English](README.en.md) | **中文**

# BetterZoom v2 — 独立的 Flarial 风格缩放模组

一个为 **LeviLauncher**（Minecraft Bedrock **arm64-v8a**）实现的
**独立原生 Zoom 模组**，功能对齐 **Flarial Client** 的 Zoom 模块（含设置面板参数项），
完全基于 Levi 官方 native mod SDK（`pl::` API，来自 preloader-android）与
[BedrockTools](https://github.com/QYCottage/BedrockTools) 风格的字节签名钩子实现，
不依赖 Flarial 本体、不依赖 LeviLauncher 内置 Zoom。

> **兼容性（v2.4.1）**：Minecraft Bedrock **26.40 / 26.44.3 / 26.45.1 / 26.50**
> 四个版本上，全部必需签名均已验证**唯一命中**；触摸桥 `GameActivityMotionEvent_fromJava`
> 导出符号在 26.50 上依然存在（同尺寸 888 字节）。
> 钩子在后台线程安装，不会阻塞启动器主线程。

> 之前的 `FlarialZoom v1.3.0` / `ZoomPlus` 为失败品（按钮被注销、Hold/Toggle 缺失、
> 或缺少 `NEEDED libpreloader.so` 导致无法加载），本版本为全新实现。

## 相比 LeviLauncher 内置 Zoom 的改进

| 问题（原版） | BetterZoom v2 |
| --- | --- |
| 缩放动画曲线生硬（线性过渡） | **Flarial 同款指数缓出曲线**（帧率无关，可调速度） |
| 不能滑动调整倍率 | **按住按钮后上下滑动**（或双指）实时调整，松开即恢复 |
| 只有点击切换一种方式 | **Hold（按住）/ Toggle（点击）** 两种模式，设置面板即时切换 |
| 无参数面板 | 完整设置面板（= Flarial Zoom 设置项） |
| 无倍率指示 | 缩放时按钮上 / 屏幕下方显示当前倍率 `x2.3` |

## 安装

1. 打开 LeviLauncher → 模组管理 → 删除旧的 `BetterZoom` / `ZoomPlus` 导入。
2. 导入 `BetterZoom.levipack` 并启用。
3. **关闭 LeviLauncher 自带的 Zoom 模组**（“模组”列表里的 Zoom 开关），
   否则内置缩放与本模组会同时改写 FOV，出现双重缩放。
4. 从 LeviLauncher 启动 Minecraft（1.26.40.x）。进入游戏后右下角出现放大镜按钮。

## 使用

| 操作 | 效果 |
| --- | --- |
| **Hold 模式**：按住放大镜按钮 | 开始缩放（平滑缓入） |
| 按住按钮的同时，在按钮上**上下滑动**（同一根手指） | 上滑放大、下滑缩小，实时调整倍率 |
| **松开按钮** | 关闭缩放，视角平滑恢复 |
| **Toggle 模式**：轻点按钮 | 开启缩放（常亮），再次轻点关闭 |
| 缩放开启时，按钮上**拖动** | 调整倍率（不改变开关状态） |
| **屏幕上的任意手指**滑动 | 转视角（与原生一致，**任意手指数量/顺序、任意时刻**都正常） |
| 绑定 **Keybind** 后按键盘 | Hold：按住缩放 / Toggle：按下切换 |
| 缩放时 | 隐藏第一人称手、降低转向灵敏度（可关） |

> **调倍率与转视角可同时进行**：转视角完全交给游戏原生触摸（所有屏幕手指原样透传，
> 游戏怎么转就怎么转）；调倍率只走缩放按钮上的滑动。按钮手指的滑动数据在**游戏触摸
> 桥钩子**里跟踪（它能同时看到所有手指，无论按钮是第一根还是第二根按下都有效），
> 并被从游戏触摸流中剔除——因此"按住按钮滑动调倍率"与"另一根手指转视角"互不抢占、
> 可同时进行。

> **转视角为什么任何顺序都正常**：本模组挂钩了游戏自己的触摸桥
> `GameActivityMotionEvent_fromJava`（libminecraftpe.so 的导出符号），在游戏输入系统
> 看到触摸之前把"按钮手指"从触摸事件里剔除（按 pointerId 匹配、指针数减一并压缩数组），
> 游戏只认得到转视角的手指——这正是 Flarial Client 的做法。
> 因此先按按钮再转视角、或先转视角再按按钮，都完全正常。

> 触屏说明：Launcher 悬浮按钮是独立窗口，拿不到按钮上的滑动数据；若用自绘按钮却
> 不处理游戏侧触摸，按钮手指会以“幽灵指针”形式漏进游戏、把视角绑定在静止的按钮
> 手指上导致无法转向。因此本模组自绘按钮（同指滑动调倍率）+ 挂钩游戏触摸桥
> `GameActivityMotionEvent_fromJava` 按 pointerId 剔除按钮手指（任意顺序转视角），
> 两者缺一不可。按钮位置通过设置面板的 “Button X / Button Y” 调整（0–1000 的比例坐标，全屏任意位置）。

## 模组菜单参数（= Flarial Zoom 设置面板）

| 参数 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| Zoom Mode | Radio | Hold | `Hold` 按住缩放 / `Toggle` 点击切换 |
| Default Zoom Level | Slider | 10 | 缩放倍率（**1x–80x，设定值 = 放大倍数**，如 5 = 5 倍） |
| Use Scroll / Drag | Toggle | on | 允许滑动/滚轮调整倍率；**关闭后缩放倍率固定为 Default Zoom Level**（不可调整） |

> **鼠标滚轮**：缩放中滚轮调整倍率（上滚放大/下滚缩小）；非缩放时滚轮正常切换物品栏。

> **自定义按钮外观（背景图标 + 按钮图标，支持按下态）**：把任意图片
> （PNG/JPEG/WebP/**SVG**，推荐正方形、透明背景）按下面的文件名放进
> `mods/BetterZoom/config/`，重启游戏后按钮即显示你的外观；删掉对应文件即恢复该层的
> 默认外观。**四个文件都是可选的**，可以只换其中一个；SVG 直接改用 `.svg` 后缀即可
> （如 `button_icon.svg`），会按 512×512 栅格化后再随按钮大小缩放：
>
> | 文件名 | 作用 | 缺省回退 |
> | --- | --- | --- |
> | `button_bg.png` / `.svg` | 按钮背景图（未按下） | 默认深色圆角矩形 |
> | `button_bg_pressed.png` / `.svg` | 按钮背景图（按下/缩放中） | 默认绿色圆角矩形 |
> | `button_icon.png` / `.svg` | 按钮图标（未按下） | 默认放大镜 |
> | `button_icon_pressed.png` / `.svg` | 按钮图标（按下/缩放中） | 默认放大镜（深色） |
>
> “按下”= 缩放开启：Hold 模式为按住按钮时，Toggle 模式为缩放切换到开启时。
> 安装包里已自带一套 **LeviLauncher 风格占位图**：绿色圆形按钮背景
> （`bg_overlay_button` 配色）+ 放大镜图标（`ic_zoom` 配色），
> 直接把上面的文件替换成你自己的图即可。
| Zoom Sensitivity | Slider | 10 | 滑动每步调整的 FOV 量 |
| Disable Animation | Toggle | off | 关闭缩放动画（瞬间切换） |
| Animation Speed | Slider | 0.30 | 动画速度（越高越快） |
| Save Zoom Level | Toggle | on | 关闭后每次激活回到默认倍率 |
| Always Animate | Toggle | off | 未缩放时也平滑 FOV 变化（疾跑等） |
| Hide Hand | Toggle | on | 缩放时隐藏第一人称手 |
| Low Sensitivity | Toggle | on | 缩放时降低转向灵敏度 |
| Low Sensitivity Strength | Slider | 0.75 | 灵敏度降低程度 |
| Cinematic Camera | Toggle | off | 电影镜头模式 |
| Smoothing | Toggle | on | 转向平滑（依赖 Cinematic Camera） |
| Smoothness | Slider | 7.0 | 平滑强度 |
| Cinematic Bars | Toggle | off | 上下黑边（依赖 Cinematic Camera） |
| Cinematic Bar Height | Slider | 0.20 | 黑边高度占比 |
| Cinematic Bar Color | Color | #000000 | 黑边颜色 |
| Zoom Indicator | Toggle | on | 倍率指示器（屏幕下方 `x2.3`） |
| Hide HUD While Zoomed | Toggle | off | 缩放时隐藏游戏 HUD（快捷栏、准星等） |
| Keybind | Keybind | 空 | 可选键盘键位 |
| Show Zoom Button | Toggle | on | 显示/隐藏放大镜按钮 |
| Button Background Size | Slider | 200 | 按钮背景大小（px，48–480） |
| Button Icon Size | Slider | 150 | 按钮图标大小（px，24–480，独立于背景） |
| Button X | Slider | 850 | 按钮横坐标（0=最左，1000=最右，覆盖全屏，背景与图标共用） |
| Button Y | Slider | 600 | 按钮纵坐标（0=最上，1000=最下，覆盖全屏，背景与图标共用） |
| Button Icon Opacity | Slider | 0.5 | 按钮图标不透明度（0–1.0，独立，只影响图标） |
| Button Background Opacity | Slider | 0.5 | 按钮背景不透明度（0–1.0，独立，只影响背景） |

> **界面语言**：模组菜单会自动跟随 LeviLauncher 的系统语言（中文环境显示中文，
> 英文环境显示英文），无需手动切换。

> 背景大小、图标大小、两者不透明度均可独立调节；位置（Button X / Y）背景与图标
> 仍共用同一个锚点，所以调整位置时两者一起移动、相对位置保持不变。

参数修改即写回模组 `config/config.json`，下次启动自动恢复。

## 实现要点

- 生命周期：`PL_REGISTER_MOD` + `load/enable/disable/unload`（Levi 官方模版）。
- 菜单：`pl::modmenu::ModuleBuilder`（不写死 `modId`，由 preloader 归属当前模组）。
- 输入：`pl::input::registerTouchCallback` 拿到游戏视图的原始触摸流
  （返回 `true` 会消费事件、游戏与悬浮按钮都收不到，用于按钮点击与调倍率手势）。
- 屏幕尺寸：JNI 读取 `Resources.getSystem().getDisplayMetrics()`（全公开 API），
  用于按钮命中测试；失败时退化为按触摸坐标估算。
- 钩子（签名在 `libminecraftpe.so` 26.40.5 上逐一验证、各恰好命中一处）：
  - `LevelRendererPlayer::getFov` —— 缩放本体（指数缓出动画）
  - `LocalPlayer::applyTurnDelta` —— 低灵敏度 / 电影镜头平滑
  - `BaseOptionRegistry::getHideItemInHand` —— 隐藏手持物
  - `GameActivityMotionEvent_fromJava` —— 游戏触摸桥（导出符号，按名解析）；
    剔除缩放按钮手指，保证任意触摸顺序下转视角都正常（Flarial 同款机制）
- `libminecraftpe.so` 尚未加载时先挂 `dlopen` 钩子，游戏库加载后再解析签名并装钩。
- 覆盖层：`submitDrawCommands` 绘制按钮 / 倍率指示 / 黑边（负坐标锚点：
  `<= -9000` 右/下边缘、`<= -19000` 屏幕中心）。
- 依赖：`NEEDED libpreloader.so`（链接期使用 `lib/libpreloader-1.5.16.so` 解析符号，
  运行期解析到 launcher 自己的 preloader）；`--exclude-libs` 保证只导出
  `PLGetModRegistration`，与正常工作的参考模组一致。

## 版本适配（游戏更新后怎么做）

每次游戏更新只需三步，全部有工具支撑：

```bash
# 1) 拿新的 libminecraftpe.so，跑校验：哪些签名失效一目了然
python3 tools/verify_signatures.py /path/to/new/libminecraftpe.so

# 2) 对失效的签名，从旧版已知地址自动派生跨版本容错模式，再到新版里定位
python3 tools/derive_pattern.py --lib OLD.so --addr 0xae31ec0 \
        --insns 10 --find-in NEW.so --show

# 3) 把新模式写回 src/Signatures.hpp，再跑一遍第 1 步，四个版本都要 UNIQUE
```

### 26.50 适配实录（踩过的坑，供下次参考）

| 签名 | 26.50 上的情况 | 处理 |
| --- | --- | --- |
| `applyTurnDelta` | 模式未变，仍唯一 | 无需改动 |
| `getHideItemInHand` | 模式未变、仍唯一 | **选项号通配了（`? ? ? 52`），所以选项 id 从 49 漂到 46 也不受影响** |
| `getFov` | 原模式**变成二义**（2 处命中，另一处是恰好共享 prologue 的无关函数） | 用 `derive_pattern.py` 加钉两条非 PC 相对指令（`mov w20,w1` / `mov x19,x0`），四版本全唯一 |
| `getHideHud` | 原模式**完全失配** | 见下 |

**关键坑：选项 id 会整体漂移。** 26.45.1 → 26.50 之间，`BaseOptionRegistry` 的选项号整体 **−3**：

- `getFov` 内部用的选项号：50 → 47
- hide-item-in-hand：49 → 46
- **hide HUD：48 → 45**（这才是 hideHUD 模式失配的真正原因）

由于选项号变了，`mov w1,#0x30` 变成 `mov w1,#0x2d`，单条静态模式无法同时覆盖新旧版本。
本模组用**结构校验**而不是顺序/猜测来选：

> hide-HUD 的 stub 与 hide-item-in-hand 的 stub 相距固定 **0x70**（在 26.40 / 26.44.3 / 26.45.1 / 26.50 上实测一致）。
> 于是把两个候选都解析出来，**只接受满足"锚点 − 0x70"关系的那一个**——旧版本上只有 opt48 成立，26.50 上只有 opt45 成立。
> 若两个都不成立，则**不装该钩子**（而不是挂到错误的选项上），并打日志；
> 此时"缩放时隐藏 HUD"仍由 F1 键机制生效（与版本无关）。

这条规则的价值在于**失败方向是安全的**：关系一旦变化，得到的是"钩子缺失 + 明确日志"，而不是"悄悄挂错函数"。

## 构建

```bash
# 需要 clang-17/lld-17 与 Android NDK r28c sysroot（arm64）
NDK_SYSROOT=/path/to/ndk/sysroot ./build.sh        # 产出 build/libbetterzoom.so
python3 scripts/package_levipack.py --root .       # 产出 BetterZoom.levipack
```

### 签名工具（tools/）

| 工具 | 用途 |
| --- | --- |
| `verify_signatures.py` | 直接从 `src/Signatures.hpp` 读模式并校验；同时**预测运行时对 hideHUD 候选的选择**，无需上机即可确认适配结果 |
| `derive_pattern.py` | 从某版本的一个已知函数地址自动生成跨版本容错模式（自动通配 PC 相对指令），并可在另一版本中定位 |

> 注意：**不要手工编码 ARM64 指令**。本次适配中手工推算 `mov w20,w1` 得出了错误字节
> （应为 `F4 03 01 2A`，误写为 `E0 03 01 2A`），导致模式在四个版本上全部失配——是四版本批量校验
> 抓出来的。`derive_pattern.py` 从反汇编器解码结果生成字节，从根本上消除这类错误。

### 依赖说明

- `include/pl/`、`third_party/`：SDK 头文件与 fmt / nlohmann-json，已随仓库提供。
- `lib/libpreloader-1.5.16.so`：**链接期**需要的 preloader 桩库，用于解析
  `pl::` 符号（运行期会解析到 launcher 自带的 `libpreloader.so`）。
  它来自开源项目 [LeviLaunchroid](https://github.com/LiteLDev/LeviLaunchroid)
  的 preloader 构建产物；若缺失，请从其 release/构建产物中取得同名文件放入 `lib/`。
- 构建脚本末尾会用 `llvm-strip --strip-all` 精简 `.so`（约 7 MB → 0.6 MB）。

## 故障排查

- **模组菜单里没有 “Zoom” 模块**：logcat 看 `adb logcat -s Preloader BetterZoom`，
  应有 `Mod menu module registered: true`；若为 false，看 “Rejected …” 原因。
- **钩子未生效（缩放无反应）**：`adb logcat -s BetterZoom` 应显示
  `GetFov=0x... ApplyTurnDelta=0x... HideItemInHand=0x...` 与
  `Game hooks installed: fov=.. turn=.. hand=.. touchBridge=..`；若 fov=false，
  检查 MC 版本是否确为 26.40.5（签名只验证过该版本）。
- **同时转向+调倍率排查**（关键日志，前若干次触发各打印几行）：
  - `TOUCH action=.. pointerId=.. x=.. y=..` —— 模组收到的触摸事件（重点看
    action=2 即 MOVE 时 pointerId 是否恒为 0）
  - `LATCHED adjust finger pointerId=..` —— 快速竖向滑动是否被识别为调倍率
  - `BRIDGE arg4=.. arg5=.. count=..` —— 游戏触摸桥是否被调到
  - `Stripping pointer id=.. (count .. -> ..)` —— 按钮手指是否被从游戏触摸流剔除
- **加载失败**：`Failed to load mod ...` / `dlopen failed: cannot locate symbol`
  时，确认 levipack 是本次新打包的（旧 v1.3.0 / ZoomPlus 需先删除）。
- **双重缩放**：确认已关闭 launcher 内置 Zoom。

## 已知取舍（相对 Flarial）

- Flarial 的 “Hide Modules”（隐藏其它 HUD）无法跨模组实现，省略。
- 打开 GUI（背包等）时缩放状态保留但 GUI 画面不缩放（与 OptiFine 一致）。
- 黑边高度按 1080p 折算为像素；手持物通道通过 `getFov` 的
  `enableVariableFOV` 位标志识别（固定 70° 通道），GUI 通道按 60° 值识别：
  **只有世界画面缩放**，手持物与 GUI 始终保持各自固定 FOV（手不会被拉伸）。

## 许可证

本项目采用 **MIT License**，详见 [LICENSE](LICENSE)。

第三方组件（preloader 桩库、`pl::` SDK 头文件、fmt、nlohmann-json、Minecraft 字体等）
各自的许可证与来源见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
