# StackPet 表情风格 + 雷达：设计与分工契约

用户反馈「几个表情看上去差距不大」，因为官方的眼睛只是 16px 的圆点，嘴从不随情绪变化。用户选了：**两套新风格都做，并且可以切换**。
另外要新增 **RADAR 雷达**应用。

效果图（Python 示意，不是 LVGL 实际渲染）：`A_classic.png`、`B_big_eyes.png`、`0_current.png`（在作者本机的 design 目录，没有收进仓库）。

## 已经由主控搭好的地基（不要改）
- `stackchan/avatar/avatar/elements/emotion.h`：在末尾追加了 `Laughing / Surprised / Shy / Love`，并定义 `kEmotionCount = 10`。
  原有的 6 个值保持不变，因为它们会通过 BLE / websocket JSON 传输。
- `stackpet/face/face.h`：
  - `Style { Official, Classic, BigEyes }`；
  - `style()` / `setStyle()` 读写 NVS（`stackpet_face` / `style`），默认值是 BigEyes；
  - `create(style, parent, font, &panel)` 工厂。
- 全部 10 个创建头像的地方都已改走工厂：AI 对话（`stackchan_display.cc`）、AVATAR、DANCE、ESP-NOW、SETUP、EZDATA、WEB.REMOTE、FREEZE、SPIN、FACE。
- AI 对话里小智的 21 种情绪名都已映射到这 10 种 Emotion（`stackchan_display.cc` 的 `SetEmotion`）。
- `apps/app_face`（启动器序号 10，FACE）和 `apps/app_radar`（序号 11，RADAR）的空壳已注册好。
- `Avatar` 基类加了 `virtual ~Avatar() = default;`。以前 `resetAvatar()` 只跑基类析构，DefaultAvatar 的 panel 会留在屏幕上，应用内切换风格时旧脸就残留在新脸上面。
- 依赖：`espressif/human_face_detect ==0.5.0` + `espressif/esp-dl ==3.3.0`，已锁定并拉取。esp-sr 锁了 esp-dsp 1.7.0，xiaozhi 锁了 esp_new_jpeg ^0.6.1，所以 esp-dl 不能再升级。

## 皮肤接口（两套风格都必须遵守）
皮肤类要提供 `init(lv_obj_t* parent, const lv_font_t* font)` 和 `getPanel()`。panel 是 320×240 的全屏容器，背景为黑色，点击事件挂在 panel 上。
在 `init` 里填好 `_key_elements`：左眼、右眼、嘴、对话气泡。**对话气泡请直接复用 `DefaultSpeechBubble`**，这样 AI 回复的文字显示保持一致。

每个 Feature 必须对下列调用给出正确的视觉反应（修饰器靠它们工作）：

| 调用 | 谁在调 | 要求 |
|---|---|---|
| `eye.setWeight(0..100)` | BlinkModifier 眨眼、HeadPet | 睁开程度：100 = 当前情绪的完整形状；BlinkModifier 闭眼时设的是 **25**，所以 ≤25 都要显示成闭合的横线。眨眼时要能看出「闭合再打开」。`setEmotion` 把 weight **复位为 100**（和官方皮肤一致，官方每次切情绪都会设 weight）：AI 路径在 `SetEmotion` 后立即 `resyncEyeWeights()`，如果不复位，落在眨眼闭合的 200ms 里就会把基准锁成 25，眼睛永远半闭 |
| `eye/mouth.setPosition({-100..100, -100..100})` | Breath 呼吸、IdleExpression 东张西望、JSON | 整体平移，幅度约 ±16px（参考官方），眼睛和嘴要一起动 |
| `mouth.setWeight(0..100)` | SpeakingModifier 说话 | 张嘴程度。每种情绪下说话都要好看，比如大笑时张得更大、难过时小嘴开合。BigEyes 没有常驻的嘴，说话时显示一张小嘴 |
| `eye.setSize(-100..100)` | IdleExpression | 眼睛缩放，0 是默认大小 |
| `mouth.setRotation(0.1°)` | IdleExpression | 嘴的轻微歪斜，做到不难看即可 |
| `setEmotion(e)` | 所有应用、AI | 切换到 10 种情绪之一。**切换时要有过渡**，例如约 120ms 的快速眨眼遮住形状切换，或者补间动画，不能硬切 |
| `setVisible(b)` | 视频通话等 | 显示 / 隐藏 |

Decorator（爱心、生气、流汗、害羞、晕）是叠加层，应用会自己加，皮肤不用管。

## 两套风格的规格
共同要求：10 种情绪**一眼可分**，在 2 米外的 2 寸屏上也要看得出；颜色要明快；不要用渐变；对象数量要克制（每只眼睛 ≤8 个 lv_obj）；
不要每帧都旋转大对象（LVGL 软件渲染的旋转很贵），静止时不要持续重绘。

**A 经典加强 `skin_classic.*`（ClassicAvatar）**：白色线条，背景黑色，保留 StackChan 点眼 + 线嘴的气质，并加上眉毛、随情绪变化的嘴形和少量彩色点缀。

| 情绪 | 眼睛 | 眉毛 | 嘴 | 点缀 |
|---|---|---|---|---|
| 平静 | 圆点 r≈14 | 无 | 横线 | — |
| 开心 | ^ 弧线 | 无 | 微笑弧 | — |
| 大笑 | > < 折线 | 无 | 张开的 D 形，带粉色舌头 | — |
| 生气 | 圆点下移 | 内低外高的斜线 | 撇嘴弧 ∩ | 左上角红色怒气符号（AngryDecorator 固定在右上，放左边不打架） |
| 难过 | 圆点 | 内高外低 | 下弯弧 | 一侧蓝色泪滴 |
| 惊讶 | 大圆点 | 上扬的弧 | 小 O 圆环 | — |
| 疑惑 | 一圆一扁 | 一侧挑眉 | 波浪线 | 「?」 |
| 困了 | ∪ 闭眼弧 | 无 | 小 o | Zzz |
| 害羞 | ^ 弧 | 无 | 小波浪 | 粉色腮红 |
| 喜欢 | 红色爱心 | 无 | 微笑弧 | — |

**B 大眼萌系 `skin_big_eyes.*`（BigEyesAvatar）**：大圆角矩形眼（约 76×92，r≈24），颜色随情绪变化，平时没有嘴。

| 情绪 | 形状 | 颜色 |
|---|---|---|
| 平静 | 圆角矩形 | 青 #3CE1FF |
| 开心 | ∩ 粗弧 | 青 |
| 大笑 | > < 粗折线 | 青 |
| 生气 | 内侧上角被斜切 | 橙红 #FF5A32 |
| 难过 | 外侧上角被斜切，带泪滴 | 蓝 #5A8CFF |
| 惊讶 | 正圆，偏大 | 青，带小 o 嘴 |
| 疑惑 | 一大一扁 | 青，带「?」 |
| 困了 | 细横条 | 暗青，带 Zzz |
| 害羞 | ∩ 弧加腮红 | 粉 |
| 喜欢 | 爱心 | 粉红 #FF3C82 |

颜色变化要有补间。眨眼时眼睛压扁成横条。看向某个方向时，眼睛整体平移，并带一点透视：看哪边，哪边的眼睛略大。

## 电脑端渲染器（必须用它自检）
`firmware/tools/face_preview/`：在 Mac 上编译固件里同一份 LVGL 9.4、smooth_ui_toolkit 和头像代码，把每种风格 × 10 种情绪，以及眨眼、说话、看左、看上、气泡这几个状态，渲染成 PNG，结果和设备像素级一致。
```bash
cd firmware/tools/face_preview
FACE_PREVIEW_BUILD=<你自己的构建目录> ./render.sh <输出目录>   # 输出目录默认是这里的 preview/
```
输出的 `sheet_<style>.png` 要用 Read 看图自检，迭代到满意为止。皮肤新增的 `.cpp` / `.c` 文件必须放在 `stackpet/face/` 目录下，renderer 会自动编译它们。
皮肤代码**不能**依赖 `hal/hal.h`、NVS 或 xiaozhi；需要时间就用 `smooth_ui_toolkit::ui_hal::get_tick()`。

## FACE 应用（`apps/app_face/*`，启动器序号 10）
- 进入后显示当前风格的头像，并自动轮播 10 种情绪，每种约 2 秒，屏幕底部用小字标出情绪名。
- 点屏幕左 / 右三分之一，或者前 / 后滑头，切换风格：官方原版 / 经典加强 / 大眼萌系。切换后立即 `setStyle()` 保存，重建头像，用标题字显示风格名约 1.5 秒。
- 点屏幕中间：暂停 / 继续轮播。上滑退出时 warm reboot 到序号 10。
- 图标：在 `firmware/tools/gen_icons.py` 里新增 `icon_face.bin`，风格对齐现有图标（黑色粗描边贴纸风，粉 / 青点缀）。

## RADAR 应用（`apps/app_radar/*`，启动器序号 11）
用户原话：「一直旋转扫描，并且亮着灯，播放着类似于雷达的那种扫描声音，然后一旦发现人，就屏幕聚焦到人上，闪着红光并发出警报声 3 秒，然后屏幕表情变成那种生气的表情」。

- **扫描**：
  - 头部在 yaw ±1250 之间用位置模式慢速往返扫描（约 25–35°/s），pitch 略微抬起。
  - 屏幕显示雷达界面：黑底、绿色同心圆和刻度，加一条持续旋转的扫描扇面。扫描线方向和头部 yaw 对应，扫过的地方留下余辉。
  - 灯：绿色跑马灯，和扫描方向同步。
  - 声音：每 1.5–2 秒一声「声呐 ping」，用正弦波加快速衰减和回声。
- **发现人**：用 human_face_detect 的 MSR+MNP 做人脸检测。帧从 FrameGrabber 取，可以给它增加 RGB888 或 RGB565 的降采样输出，但只能做加法，不能破坏 REMOTE；检测放在独立任务里。
  连续 2 帧检测到人脸才算确认。确认后依次执行：
  1. 立即停止扫描，转头对准人脸中心（按画面坐标换算出 yaw / pitch 的偏移）。
  2. 屏幕切到摄像头实时画面，人脸外画红色锁定框，带四角瞄准标记和「锁定目标」字样，持续整个 3 秒警报（用户原话是「聚焦到人上，闪着红光并发出警报声 3 秒，然后……生气」）。
  3. 同时红灯快闪，警报声（高低交替的警笛）持续 **3 秒**。
  4. 之后屏幕回到头像，表情切到 **Angry**，并加 AngryDecorator，瞪着目标约 3 秒（期间头跟随人脸微调）。
  5. 冷却约 4 秒后恢复扫描；如果目标还在画面里，就保持生气并继续盯着，等目标离开再恢复扫描。
- 人脸检测跑不动或编不过时，退化方案：每扫过约 40° 停 1 秒，用 MotionDetector 检测运动。退化原因要写进报告。
- 声音：在 `stackpet/sound/tone_synth.*` 和 `sound_player.h` 里只做加法，新增 `Sfx::RadarPing` 和 `Sfx::Alarm`。
- 退出：`onClose` 第一行调 `stopRotation()`（虽然这个应用只用位置模式，也照样调），然后停掉检测任务、FrameGrabber、声音、灯，warm reboot 到序号 11。
- 图标：`icon_radar.bin`，雷达扇面造型。

## 文件归属
| 开发线 | 负责的文件 |
|---|---|
| **faceA** | `stackpet/face/skin_classic.*`，以及 `stackpet/face/classic_*`（如有） |
| **faceB** | `stackpet/face/skin_big_eyes.*`、`stackpet/face/bigeyes_*`（如有）、`apps/app_face/*`、`tools/gen_icons.py` 里 icon_face 的部分、`main/assets/assets_bin/icon_face.bin` |
| **radar** | `apps/app_radar/*`、`stackpet/vision/face_detector.*`（新增）、`stackpet/camera/frame_grabber.*`（只做加法）、`stackpet/sound/tone_synth.*` + `sound_player.h`（只加 Sfx）、`tools/gen_icons.py` 里 icon_radar 的部分、`main/assets/assets_bin/icon_radar.bin`、radar 相关的主机测试 |
| **web**（之后） | 网页增加 4 个新表情按钮和风格切换（`POST /api/face?style=`）；`app_remote.cpp` 里情绪 clamp 的上限改为 `kEmotionCount-1` |
| **games**（之后） | FREEZE / SPIN 在合适的地方用上新情绪 |

`gen_icons.py` 由 faceB 和 radar 共用：各自只加自己图标的那个函数，改之前先重新读一遍文件，不要整体覆盖。
编译用各自的 build 目录：`idf.py -B build_<名字> -D SDKCONFIG=build_<名字>/sdkconfig build`（在 ESP-IDF 环境里），新增文件后先 reconfigure。
不要刷机、不要开串口、不要提交 commit。
