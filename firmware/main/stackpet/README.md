# StackPet 扩展：设计与分工契约

在 M5Stack 官方 StackChan 固件（v1.5.1）上做的优化和新玩法。所有新增代码放在 `main/stackpet/` 和
`main/apps/app_{remote,freeze,spin,face,radar}/`，官方文件只做最小改动，方便以后合并上游。

## 目标

**优化**
1. 摄像头链路：采集、编码、发送不再阻塞主循环；去掉官方 350/700ms 硬限速；复用帧缓冲；DVP 双缓冲（实测稳定才保留）。
2. 局域网直连：手机/电脑浏览器在同一 Wi-Fi 下直接看实时画面、遥控（不经过云端）。
3. 唤醒词「小二小二」：MultiNet 自定义唤醒词 + AFE 前处理（回声消除/降噪），原版这条路径直接取原始音频。
4. 禁止 xiaozhi 自动 OTA 把自编固件覆盖掉。
5. 语言改 zh-CN（已在 sdkconfig.defaults 完成）。

**新玩法（启动器新增 5 个应用，排在官方应用之后，序号 7–11）**
- `WEB.REMOTE` 网页遥控台（名字避免和官方 `ESPNOW.REMOTE` 混淆）：扫屏幕上的二维码打开网页 → 实时画面 + 摇杆转头 + 10 种表情 / 表情风格切换 / 装饰 + 灯光 + 动作 + 让它说话 + 拍照 + 音量。
- `FREEZE` 一二三木头人：它背过身数「一、二、三、木头人」，猛回头用摄像头抓动的人；摸到它的头就算你赢。
- `SPIN` 幸运转盘：360° 连续转圈，减速停下指向一个人，或者从「吃什么/真心话/大冒险/做决定」里抽一个结果。
- `FACE` 表情风格（序号 10）：轮播 10 种情绪；点屏幕左右三分之一或前后滑头，在官方原版 / 经典加强 / 大眼萌系之间切换。选中的风格存进 NVS，所有应用和 AI 对话都跟着用（规格见 `stackpet/face/README.md`）。
- `RADAR` 雷达（序号 11）：头来回扫描，屏幕变成雷达，绿灯跑马，响声呐 ping。ESP-DL 认出人脸后转头锁定，屏幕显示摄像头画面和红色锁定框，同时红灯快闪、警笛响 3 秒；之后换成生气的脸瞪着对方，直到人走开。摸摸头它就消气。

## 硬约束（所有开发线都必须遵守）

- **只改自己负责的文件**（见「文件归属」）。确实需要改别人的文件，写进最终报告由主控处理，不要自己改。
- **不要改**：`main/CMakeLists.txt`、`main/main.cpp`、`main/apps/apps.h`、`dependencies.lock`、`main/idf_component.yml`
  （主控已经接好：`stackpet/*.c|cc|cpp` 已加入 GLOB，`stackpet/web/index.html` 已经 EMBED_TXTFILES，已加 `esp_http_server` 依赖）。
  新增源文件放进已有目录即可，但 GLOB 不会自动发现新文件，新增 .cpp 后要先 `idf.py ... reconfigure`。
- **编译用自己的 build 目录和 sdkconfig**，防止并行编译互相踩：
  ```bash
  cd firmware   # 在 ESP-IDF v5.5.4 的环境里（作者本机用一个进入环境的包装脚本 idf-env.sh，不在仓库里）
  idf.py -B build_<track> -D SDKCONFIG=build_<track>/sdkconfig build
  ```
  `<track>` = `web` / `games` / `audio` / `faceA` / `faceB` / `radar`。首次编译约 5 分钟。**不要用默认的 `build/`，不要刷机、不要打开串口**（设备只有一台，由主控统一刷机测试）。
- **线程模型**：mooncake 主循环（app_main 任务）调用 `App::onRunning()`；LVGL 有自己的线程，任何 `lv_*` / avatar / `view::`
  调用都要持有 `LvglLockGuard`。舵机（`GetStackChan().motion()`）只在主循环里用（`GetStackChan().update()` 驱动动画）。
  HTTP handler、采集任务、HAL 信号回调（头顶触摸、IMU 在别的任务里触发）**不得直接碰 LVGL / Motion**，只能置原子标志或进线程安全队列，交给主循环处理。
- **应用模式下 xiaozhi `Application` 没有初始化**：禁止调用 `Application::GetInstance().PlaySound()` /
  `hal_bridge::app_play_sound()`。声音一律走 `stackpet::SoundPlayer`。
- **应用退出**：`onClose()` 里清理（`GetStackChan().resetAvatar()`、`clearModifiers()`，`GetHAL().onXxx.clear()` 你连过的信号，
  停止自己的后台任务，`SoundPlayer::shutdown()`），然后 `GetHAL().requestWarmReboot(stackpet::kAppIndexXxx)`
  （官方应用都这样做：重启回到启动器对应图标，所有资源随重启释放）。
- **内存**：内部 SRAM 只剩约 130KB。帧、JPEG、PCM 等大缓冲放 PSRAM（`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`，
  或者大于 512 字节的 `std::vector` 自动走 PSRAM，见 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512`）。任务栈按需给，别浪费。
- **日志**：`mclog::tagInfo/tagWarn/tagError`（fmt 风格 `{}`）。每帧级别的日志用 debug 或节流到 ≤1 次/秒。
- **代码风格**：跟周围代码一致（4 空格缩进，私有成员 `_snake_case`，方法 `camelCase`，注释以英文为主），文件头用 SPDX（StackPet, MIT）。
- **中文 UI 文本**：大字用 `stackpet::ui::fontTitle44()` / `fontTitle28()`（子集字体，最后由主控统一重新生成以覆盖所有用到的汉字，
  所以字符串里的汉字随便用）；小字或任意文本用 `fontCn20()`（资源分区里的 PuHui 常用字库，6656 个码点，约 5258 个汉字；缺「蛳」等少数生僻字）。

## 设备事实（已核实）

- **摄像头 GC0308**：esp_video 配置为 YUV422（YUYV）320x240 @20fps（`CONFIG_CAMERA_GC0308_DVP_YUV422_320X240_20FPS`）。
  `hal_bridge::board_get_camera()` 返回 `StackChanCamera*`（可能为 nullptr）。`StreamCaptures()` 取一帧到内部缓冲，
  再用 `GetFrameData/Size/Width/Height/Format()` 读取。JPEG 编码用 xiaozhi 的 `image_to_jpeg()`（`jpg/image_to_jpeg.h`）。
  板子初始化里 `SetHMirror(false)`；画面左右方向和机器人视角的对应关系 2026-10-12 用人脸实测过：画面往右看要 +yaw（REMOTE 的 `kLookYawSign = 1`，RADAR 的 `kImageXToYaw = 1`）。
- **舵机**：角度单位 0.1°。yaw 位置模式 ±1280，pitch 0..900（建议 50..850）。`GetStackChan().motion()`：
  `moveWithSpeed()` / `lookAtNormalized()` / `goHome()` / `isMoving()`。连续旋转用 `motion().yawServo().rotate(v)`（v ∈ -1000..1000，
  PWM 模式）。**结束旋转必须调 `motion().yawServo().stopRotation()`**（同步：PWM 归零、切回位置模式、原地保持）；任何退出路径
  （onClose、warm reboot 之前）都要先调它——轮式模式写在舵机寄存器里，ESP32 重启时舵机不断电，新一次启动无法恢复。
  HAL 另有两道兜底：esp_restart 的 shutdown handler 会停转；开机时发现限位寄存器是 0/0（轮式模式残留）会写回 NVS 里记下的本机限位
  （这台机器 yaw 实测 20/1003）。舵机上电受控时不要设计“要人用手掰头”的玩法。
- **头顶触摸**：`GetHAL().onHeadPetGesture`（`Press / Release / SwipeForward / SwipeBackward`）。
- **灯**：12 颗，0-5 左排、6-11 右排，用 `stackpet::fx`（`fx/led_fx.h`）。每次刷新是一次 I2C 事务，≤30Hz。
- **屏幕** 320x240 触摸屏。`avatar::DefaultAvatar`；`Emotion`：Neutral/Happy/Angry/Sad/Doubt/Sleepy；
  Decorator：Heart/Angry/Sweat/Shy/Dizzy（`stackchan/avatar/decorators/decorators.h`）；modifiers 见
  `stackchan/modifiers/modifiers.h`（Blink/Breath/IdleMotion/HeadPet/TimedEmotion/TimedSpeech/Speaking/Dance…）。
  通用控件：`apps/common`（home indicator、status bar、toast、loading page）。
- **网络**：`GetHAL().startNetwork(onLog)` 会阻塞到连上已保存的 Wi-Fi；IP 用 `WifiManager::GetInstance().GetIpAddress()`。
- **音频**：`Board::GetInstance().GetAudioCodec()` → `EnableOutput(true)` → `OutputData(std::vector<int16_t>&)`
  （单声道 24kHz，阻塞写 I2S，参考 `hal/audio.cpp` 的麦克风测试）。
- **中文数字语音**：`<assets/lang_config.h>` 的 `Lang::Sounds::OGG_1/OGG_2/OGG_3`（zh-CN，Ogg/Opus，即「一」「二」「三」）。

## 文件归属

| 开发线 | 负责的文件 |
|---|---|
| **web**（摄像头/网页/遥控） | `stackpet/camera/*`、`stackpet/web/*`、`apps/app_remote/*`、`hal/board/stackchan_camera.{h,cc}`、`hal/hal_ws_avatar.cpp`、`tests/stackpet_web.cmake` + `tests/stackpet_web_*.cpp` |
| **games**（游戏/视觉/灯效） | `stackpet/vision/*`、`stackpet/games/*`、`stackpet/fx/*`、`apps/app_freeze/*`、`apps/app_spin/*`、`tests/stackpet_games.cmake` + `tests/stackpet_games_*.cpp` |
| **audio**（声音/字体/图标/唤醒词/OTA） | `stackpet/sound/*`、`stackpet/ui/*`、`main/assets/assets_bin/icon_{remote,freeze,spin}.bin`、`firmware/tools/*`、`xiaozhi-esp32/main/audio/wake_words/custom_wake_word.{h,cc}`、`xiaozhi-esp32/main/application.cc`（只改自动升级那段）、`patches/xiaozhi-esp32.patch`（重新生成）、`sdkconfig.defaults`（确有必要时）、`tests/stackpet_audio.cmake` + `tests/stackpet_audio_*.cpp` |
| **radar**（雷达/人脸检测） | `apps/app_radar/*`、`stackpet/vision/face_{detector,model,bench,scoring}.*` + `stackpet/vision/testdata/*`、`main/idf_component.yml` 里 esp-dl 和依赖固定那两段、`stackpet/camera/frame_grabber.*` 的 RGB565 输出（只做加法）、`stackpet/web/web_server.cpp` 里测试版的 `/debug/rgb565`、`/debug/faces`、`/debug/spin` 和 `apps/app_remote/*` 里 `STACKPET_CAL` 的部分、`stackchan/motion/servo.h` + `hal/hal_servo.cpp` 的 `readRawPosition()` / `readTemperature()`、`rotate()` 丢弃待写的对齐（`stackchan/motion/servo.*` 的 `begin_rotation()`）、`getZeroRawPosition()`、`stopRotation()` 的连读和关扭矩、`hal/drivers/FTServo_Arduino/src/SCSCL.*` 的 `CacheAngleLimit()` 与 `SwitchMode(1)` 不缓存 0/0、`firmware/tools/espdl_unclash.py` + 顶层 `CMakeLists.txt` 末尾的 PRE_LINK 段、`stackpet/sound/sound_player.h` + `tone_synth.cpp` 里的 `Sfx::RadarPing` / `Sfx::Alarm`、`firmware/tools/gen_icons.py` 里的 `draw_radar`、`main/assets/assets_bin/icon_radar.bin`、`tests/stackpet_radar.cmake` + `tests/stackpet_radar_*.cpp` + `tests/stubs/hal/hal.h` |

`xiaozhi-esp32/` 被 git 忽略，改动必须体现在 `patches/xiaozhi-esp32.patch` 里（`fetch_repos.py` 会在干净的 v2.2.4 上应用它）：
改完后执行 `git -C xiaozhi-esp32 diff > patches/xiaozhi-esp32.patch`（包含官方原补丁和新改动），再在一份干净 checkout 上验证 `git apply --check`。

## 模块规格

### FrameGrabber（web）
见 `camera/frame_grabber.h`。一个任务独占摄像头：采集 → 可选的下采样亮度图（给视觉用）→ 可选的 JPEG 编码（只有注册了 JPEG 消费者才编码）。
消费者只读“最新帧”，旧帧直接丢弃，慢客户端永远拖不住采集。目标：1 个观看者、2.4G Wi-Fi、320x240、质量≈50 时，局域网 MJPEG ≥12fps。

**RGB565 输出（radar 加的，只做了加法）**：给 RADAR 的人脸检测和锁定画面用。
- `GrabberConfig::rgbDownscale`：1 = 传感器分辨率 320x240（默认），2 = 160x120（2x2 盒式滤波）。YUYV 用和 esp-dl 相同的全范围 BT.601 系数转换（`pixel::yuyvToRgb565`，头文件里的纯函数，有主机测试）；传感器出 RGB565 时直接拷贝或抽点。
- 按需转换：`requestRgb()` 只置一个原子标志，采集任务转换完**下一个**传感器帧后清掉它。
  `waitRgb(afterSeq, out, timeoutMs)`：有比 afterSeq 新的帧就直接返回，没有就先请求再等；和 `waitJpeg` 一样，停流不会让它提前返回。
  `latestRgb()`：只取最新帧，不请求。想预取的消费者在处理当前帧之前调一次 `requestRgb()`。
- 零开销：没人请求时采集循环每帧只多一次原子读，REMOTE / FREEZE 不受影响。`GrabberStats::rgbFps/rgbMs` 和每 10 秒一条的 `rgb565 … fps` 日志只在有转换时出现。
- `RgbFrame` 的生命周期：每次转换都 `heap_caps_malloc` 一块新的 PSRAM（320x240 是 150KB），用 `shared_ptr<const uint16_t>` 共享。帧内容之后不再改动，最后一个持有者释放时才 free。
  拿到帧的人想用多久就持有多久：锁定画面直接让 `lv_image` 指向它，换帧和渲染都在 LVGL 锁里。所以同时可能有 4–5 帧在内存里（采集端最新一帧、检测任务手上和刚发布的、应用和锁定画面各一帧），约 0.6–0.75MB。
  `stop()` 后采集端放掉自己那份。分配失败时这一帧不转，下一帧重试。
- 线程：三个方法都线程安全，任何任务都能调。转换在采集任务里做（core 1，优先级 2），320x240 估计约 10ms，以日志为准。
  字节序是 LVGL 原生的 RGB565（等同于 esp-dl 的 `DL_IMAGE_PIX_TYPE_RGB565LE`）。

### WebServer + index.html（web）
HTTP 接口约定：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 控制页（`text/html; charset=utf-8`） |
| GET | `/api/status` | `{"battery","charging","yaw","pitch","volume","face_style","camera","fps","captureFps","encodeMs","jpegBytes","clients","maxClients","streamFps","ip","uptime","heapInternal","heapPsram"}` |
| POST | `/api/look?x=&y=&speed=` | x、y ∈ -1..1，按画面方向线性映射到 yaw/pitch（`moveWithSpeed`） |
| POST | `/api/home` | 回正 |
| POST | `/api/emotion?e=` | `neutral/happy/angry/sad/doubt/sleepy/laughing/surprised/shy/love`（顺序即 `avatar::Emotion` 的值，共 `kEmotionCount` = 10 个） |
| POST | `/api/face?style=` | 表情风格 `0` 官方原版 / `1` 经典加强 / `2` 大眼萌系（`stackpet::face::Style`），只接受这三个字符串，否则 400 |
| POST | `/api/decorator?d=` | `heart/angry/sweat/shy/dizzy` |
| POST | `/api/rgb?r=&g=&b=` | 全部灯 |
| POST | `/api/action?a=` | `nod/shake/spin/dance` |
| POST | `/api/speak` | 正文是 UTF-8 文本（≤60 字），显示在对话气泡 |
| POST | `/api/sfx?s=` | 音效名（对应 `stackpet::Sfx`，小写） |
| POST | `/api/volume?v=` | 音量 0-100（主循环里写 NVS） |
| POST | `/api/photo` | 机器人端 3-2-1 倒计时 + 白灯补光拍照，返回 `{"id":N}` |
| GET | `/photo.jpg?id=` | 取回上面那张照片 |
| GET | `/snapshot.jpg` | 单张（不倒计时） |
| GET | `:81/stream` | MJPEG（`multipart/x-mixed-replace`），单独的 server 实例，最多 2 个观看者，满了返回 503 |

除 `/`、`/index.html`、`/favicon.ico` 外，上表所有接口都要访问令牌：请求头 `X-StackPet-Key: <令牌>`，或 query `k=<令牌>`（给 `<img>` 拉流用），
不对一律 403（详见下面「访问控制」）。

页面：移动端优先、中文界面、深色；实时画面（断线自动重连）+ 帧率；摇杆（按住拖动，节流 ≤10Hz）；10 个表情按钮 + 「表情风格」三选一
（选中态跟随 `/api/status` 的 `face_style`，点了之后 4 秒内以点击为准）；装饰按钮；灯光色块；动作；输入框“让它说”；拍照下载；音量。

**访问控制（第一轮评审后加入，不要去掉）**：每次进入就绪状态用 `esp_random` 生成 8 位令牌（字母表去掉易混的 0/o/1/l/i）。
除 `/`、`/index.html`、`/favicon.ico` 外，所有接口和 `:81/stream` 都要令牌（请求头 `X-StackPet-Key`，或 query `k`，给 `<img>` 用），
否则 403；不发 `Access-Control-Allow-Origin`。二维码是 `http://IP/?k=令牌`；页面没有令牌时显示扫码提示（可手动输入令牌）。
另外有 `POST /api/volume?v=0-100`（主循环里写 NVS）和 `POST /api/photo` + `GET /photo.jpg?id=`（机器人端 3-2-1 倒计时 + 白灯补光）。

### AppRemote（web）
- `onOpen`：loading 页（Wi-Fi 连接中…）→ `startNetwork` → `FrameGrabber::start` → `WebServer::start`；头像 + Blink/Breath；
  覆盖层卡片：带令牌 URL 的二维码（`lv_qrcode`，已开启 `CONFIG_LV_USE_QRCODE`）+ 可手动输入的完整地址 + 隐私提醒「扫码的人都能看画面、遥控」。
  有客户端连上后自动隐藏卡片（气泡提示「点屏幕可以再显示二维码」），点屏幕再显示。
  有人在看画面时屏幕角落常驻「● 直播中 · N人」（卡片显示时改在卡片里显示，不能盖住二维码）。
  Wi-Fi 20 秒连不上：中文提示「没连上 Wi-Fi，请先在 SETUP 里配网」，可随时上滑退出。
- `onRunning`：取出命令执行（动作/表情/装饰/灯/说话/音效/音量/拍照/表情风格），更新状态，`GetStackChan().update()`。
- 头像一律走 `stackpet::face::create(lv_screen_active(), ui::fontCn20(), &panel)`（说话气泡字体即 `fontCn20`），情绪 clamp 到 `kEmotionCount - 1`。
- 表情风格（`CommandType::Face`）：主循环里 `face::setStyle()`（写 NVS）后在 REMOTE 内重建头像：保留当前情绪，重新挂面板点击回调，
  新头像 `moveBackground()` 到卡片 / 角标 / 状态栏下面；Blink / Breath / HeadPet 这几个基础修饰器重新创建（它们带着上一张脸的状态），
  装饰器、对话气泡、限时表情这类临时效果随旧头像一起消失。HTTP 任务只校验参数、投递命令，不碰 LVGL 和 NVS。

### 摄像头优化（web）
- `StackChanCamera::StreamCaptures()`：复用帧缓冲（不再每帧 malloc/free 150KB），去掉每帧一条的 WARN 日志。
- DVP 缓冲数改 2（编译期常量；无法上机验证的话保留开关并在报告里说明，由主控上机实测）。
- `hal_ws_avatar.cpp`：推流不再在主循环里同步采集编码；改用 FrameGrabber（JPEG 消费者）+ 独立发送任务，去掉 350/700ms 限速，
  上限约 12fps（视频通话模式约 8fps，因为设备还要解码对面发来的画面）。协议不变。（自编固件连不上官方服务器，这部分只能保证编译和逻辑正确。）

### MotionDetector / FreezeGame / SpinGame（games）
接口与阶段定义见头文件，纯 C++（不得包含 ESP-IDF / LVGL 头文件），时间由外部注入，可在主机上测试。

**表情编排（`games/game_mood.*`，纯 C++）**：每个阶段对应一小段情绪节拍（`MoodBeat`，最后一拍保持）。`freezeMood(phase)`、`spinMood(phase)`、
`spinRevealMood(SpinTone)` 给出脚本，`MoodScript` 按时间推进，只在情绪真正变化时上报，应用再调 `setEmotion()`（皮肤每次切换都会眨一下眼，所以每拍至少 `kMinBeatMs`=400ms）。
`WatchMood` 负责盯人时的「察觉」：危险度（运动 / 阈值）≥0.6 时瞪大眼 Surprised，<0.35 才恢复 Doubt，两次变化至少间隔 500ms。装饰跟着节拍出现。

**木头人流程与表现（AppFreeze）**
- Idle：头像 Neutral + Blink/Breath，标题「一二三木头人」；副标题在规则（「数数时靠近，回头时别动」「背景别有人走动哦」）和纪录（NVS 里的最高轮数 / 最快获胜）之间轮流显示；摸头或点屏幕 → `start()`。太暗时提示「太暗啦，开灯再玩」，没有摄像头时只比谁先摸到头。
- Ready（5 秒）：转向玩家、Happy，标题「快退到起点！」、副标题「摸到我的头就算你赢」；最后 3 秒每秒一声 Tick，12 颗灯逐个熄灭。
- Chanting：转头背对玩家（yaw 约 ±1100，随机方向，中速），Laughing（> < 眯眼坏笑：「我没偷看」），副标题「快来摸我的头！」；每个 ChantWord 播放「一」「二」「三」中文语音（已裁掉首尾静音），
  第 4 个词「木头人」播 `Sfx::MuTouRen` + 灯闪红。**下一个词的间隔从上一段声音真正播完（`SoundPlayer::isBusy()` 为假）才开始算**，
  节奏随机、越来越快，下限 350ms；「木头人」播到最后一个音「人」才回头，整段旋律放完才进入 Watching。首轮数数前 1.5 秒摸头不算。
- TurningBack：最快速度回正，标题「木头人！」、副标题「不许动！」，Doubt 眼神，灯红色常亮（灯光变化会被摄像头当成运动）。Settling：等舵机稳定、曝光稳定。
- Watching：盯人，Doubt；有人差点动过头时瞪大眼 Surprised（`WatchMood`）。屏幕显示「不许动！」+ 动作强度条（≥30px 高，玩家能看到自己有多危险），心跳式滴声。
- Caught：先 `stopAll()`；转头看向**最大连通变化块**的中心（不是全部变化的质心，两人同时动时不冤枉中间的人）、`Sfx::Caught`、「抓到你了！」+「坚持了 N 轮」（0 轮时「一上来就被抓啦，再来！」）。
  表情：Surprised 0.6s（吓一跳）→ Angry 1.4s（这时才出现 AngryDecorator）→ Laughing（嘲讽出局的人，装饰收掉）。
- CaughtWait（淘汰赛，`continueAfterCaught` 默认开）：被抓画面持续 `caughtMs` 后进入，标题「抓到你了！」、副标题「点屏幕继续 · 摸头重开」、
  气泡「被抓的人点屏幕，其他人别动」，头回正、灯呼吸、Doubt（盯着其他人不许动）。点屏幕 `continueGame()`：被抓的人出局，其他人进入下一轮（轮数 +1，第一段数数同样有
  1.5 秒摸头保护）；用过「继续」的这一局不更新最高纪录。摸头重开（回 Ready、轮数清零）；`caughtWaitTimeoutMs`（90 秒）无操作回 Idle。
- Survived：灯绿、Love + 小爱心、「很好～」，下一轮更快、更严。
- PlayerWin：先 `stopAll()`；Surprised 0.5s（「被你摸到了！」）→ Love + HeartDecorator + ShyDecorator、彩虹灯、`Sfx::Win`、「你赢啦！」+「第 N 轮摸到我啦！」。
- 运动检测：在 Watching 开始时拿第一帧新亮度图当参考，之后每帧与参考比较，连续 `confirmFrames` 帧超阈值才判定（抗噪声）。

**转盘流程与表现（AppSpin）**
- Idle：标题「‹ 类别 ›」（大字，箭头对应左右点击热区），气泡「摸摸头，开始转！」；选人/真心话/大冒险提示「别坐我背后哦，摸头开始转！」；头顶前/后滑或点屏幕左右切换类别。
- SpinUp/Cruise/SlowDown：`yawServo().rotate(velocity)`，灯跑马（`fx::chase(wheelPosition)`），每个 Tick 播放 `Sfx::Tick`，越转越慢。
  表情：SpinUp 时 Surprised 0.7s（「哇！」）→ Laughing（转得开心，带 DizzyDecorator）；SlowDown / Settle 时 Doubt（「会是谁呢…」）。
- Settle：先 `stopRotation()` 再位置模式移动到 `targetYaw()`（±1250 内按 5 个扇区洗牌抽取 + 抖动，连续两次不指同一方向）。
- Reveal：`Sfx::Reveal`；表情先 Surprised 0.45s（「当当！」），再按结果性质（`SpinTone`，由 `spinResultTone()` 给出）反应：
  选人 Laughing、吃什么 Love + 爱心、真心话 Shy + 害羞、大冒险 Laughing + 流汗、做决定的「同意」Love + 爱心 /「拒绝」Doubt + 流汗 /「打趣」Laughing。
  结果卡片会盖住脸，所以 1.2 秒后才出现，让玩家先看到表情；选人模式没有卡片，标题直接显示「就是你！」。
  **结果一直停留**（`revealMs = 0`），卡片底部「摸头再转 · 点两边换类别」。
- 类别：选人 / 吃什么 / 真心话 / 大冒险 / 做决定（内容要有趣、适合朋友聚会，避免冒犯；做决定的答案约同意 6 : 拒绝 5 : 打趣 3，
  每条答案在 `itemTones` 里标了性质）。每个类别用洗牌袋抽取，避免连续重复。

### FaceDetector / RADAR（radar）
**esp-sr 和 esp-dl 的内核撞名（2026-10-12 查出，必读）**
- esp-sr 自带一个预编译的旧版 esp-dl（`lib/esp32s3/libdl_lib.a`），里面 48 个汇编内核和 esp-dl 3.3 同名
  （`dl_tie728_s8_conv2d_33cn`、`dl_tie728_memcpy` 等），但参数结构不一样。
- 链接器遇到同名符号时，谁先被扫到就全局用谁，结果是 esp-sr 的。于是 esp-dl 的卷积全跑在旧内核上，算出来是垃圾：
  纯黑图上能「检测」出 6 张脸，画面左边缘一排假脸。第一版 RADAR 一直处于生气状态，就是被这排假脸触发的。
- 修法：顶层 `CMakeLists.txt` 在每次链接 ELF 之前（PRE_LINK）跑 `tools/espdl_unclash.py`。脚本找出 esp-dl 库里和 esp-sr
  预编译库同名的全局符号，用 objcopy 把 esp-dl 这一份改名成 `stackpet_espdl_*`，定义和 esp-dl 内部的调用一起改。
  esp-sr 不动，唤醒词和命令词照旧用它自己的旧内核。
  - 比对范围不按组件名写死：只要构建里有 esp-dl（按 `esp-dl` / `esp_dl` 结尾匹配），就和 managed_components 里本芯片的全部
    预编译库比（目前 21 个，不含 esp-dl 自带的），配置时会打印 `StackPet: esp-dl symbols are checked against N prebuilt archives`。
  - 脚本是幂等的：改过一次就不再撞名，以后链接时什么都不做。esp-dl 库重新打包后会自动再改一次。
    代价：改名那一次是在链接这一步里改了 esp-dl 库，所以紧接着的下一次构建会多做一次 ldgen 和链接（约 20s），结果不变。
  - 守卫（每次链接都跑，约 2s）：构建目录里任何其他组件库如果用原名调这些内核（它会绑到预编译的旧版上），构建直接失败并列出是谁。
    预编译库所属组件自己编出来的库（比如 esp-sr 的胶水代码）不算。nm / objcopy 出错、找不到任何预编译库，也都会让构建失败。
  - 编译日志里会出现 `espdl_unclash: renamed 48 esp-dl symbols ...`，在 map 文件里应该能看到 `stackpet_espdl_dl_tie728_s8_conv2d_33cn`。
  - 已核实：esp-sr 没有任何未定义符号是只靠 esp-dl 解析的，两边彻底分开；和 RADAR 之前的 map 比，esp-sr 相关符号的来源没有任何变化。
  - 升级 esp-sr 或 esp-dl 以后，先跑一遍基准（见下），所有探针都要 PASS。
- 加 esp-dl 时组件管理器重新求解了整棵依赖树，顺带升级了 6 个和它无关的组件（音频编解码驱动 `esp_codec_dev` 1.5.4→1.5.11、
  `dl_fft` 0.3.1→0.8.0 等）。小智模式没在新版本上验证过，所以 `main/idf_component.yml` 把它们全部固定回第一轮的版本，
  `dependencies.lock` 里除了新增的 esp-dl / human_face_detect，其余版本和 git 里一样。以后要升级，得连同小智模式整体回归。
  - 注意：降级换回的源码时间戳比原来的对象文件旧，ninja 不会重编，链接会报 `dl_fft_table_acquire` 未定义之类的错；
    依赖这些头文件的其他组件也可能留着按新头文件编的对象。换版本以后要把这些组件的构建目录删掉，并 `touch` 它们的源码目录（或者整个 fullclean）。

**人脸检测（`vision/face_model.*` + `vision/face_detector.*`）**
- 模型：`espressif/human_face_detect` 0.5.0 的 MSR+MNP（int8，两阶段）；esp-dl 锁在 3.3.0（原因见 `main/idf_component.yml`）。模型放在 flash rodata（192KB）。
  `sdkconfig.defaults` 只开了 `CONFIG_PIX_CVT_RGB565_TO_RGB888_SUPPORT`，其余 14 种像素转换都关掉，省了约 820KB flash。
  以后要给 esp-dl 喂别的像素格式，得先打开对应开关，否则 `preprocess` 里的 `ESP_ERROR_CHECK` 会直接 abort。
- `FaceModel` 用和 `human_face_detect::MSRMNP` 相同的 esp-dl 部件（同样的 anchor、预处理和阈值）自己拼两阶段，多了三样东西：
  - **MNP 的分数按「人脸」类算**。MNP 每个候选输出两类 logit（实测第 0 类是背景，第 1 类是人脸）。esp-dl 自带的
    `MNPPostprocessor`（3.3 和 master 都一样）取的是「最可能那一类」的概率，不管它是哪一类，所以背景小块也会以接近 1.0 的分数通过，
    第二阶段等于从不淘汰。`FaceMnpPostprocessor` 改成只看人脸类的 softmax 概率，框的解码不变（不解码五个关键点，没用到）。
    实测：把 MSR 阈值压到 0.01、强行把天花板小块送进 MNP，原版会出 7 张满分的「脸」，现在一张都没有，人脸概率都 ≤0.02。
  - **第一阶段特征图可以放进内部 RAM**（`FaceModelConfig::msrInternalBytes`）。esp-dl 申请时带 `MALLOC_CAP_SIMD`，在 S3 上只有
    D/IRAM 满足（64KB 的内部预留池、RTC RAM、IRAM 都不行），所以按 D/IRAM 的最大块算预算（规则是纯函数，见 `face_scoring.h`）：
    - 按加载**之前**的堆计算：整个内部 RAM 至少留 48KB，D/IRAM 至少留 32KB（小块 malloc 都从这里分，用完会退到 PSRAM），块里再留 4KB。
    - esp-dl 自己约 20KB 的小对象随后也从 D/IRAM 分，所以加载后 D/IRAM 实际只剩 20KB 左右（RADAR 实测 67KB → 约 20KB），加载日志里会打印。
    - DMA 缓冲不受影响：64KB 的内部预留池（`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`）能 DMA、但不支持 SIMD，esp-dl 不会从里面分。
      RADAR 运行时唯一新增的 DMA 分配（第一次放声音时的 I2S，约 12KB）从这个池子拿。
    - 块到 esp-dl 真正分配时已经不够了，就整个退回全放 PSRAM，不会因为预算导致加载失败。
  - **第二阶段候选数上限**（`maxCandidates`，候选按 MSR 分数从高到低），外加两阶段分别计时。
- FaceDetector 的配置：MSR 预算 32KB（RADAR 里实际拿到 27KB），MNP 全放 PSRAM，候选最多 3 个，阈值都是 0.5。
- 打分（`classProbability`、`aboveThreshold`）、预算裁剪（`fitInternalBudget`）和框转换（`boxesToFaces`）都是纯函数，有主机测试，
  其中用 CoreS3 实测的 logit 锁住了「看人脸类、不看最大类」。
- 实测（RADAR 模式，没有 Wi-Fi，四线 PSRAM 80MHz，D-cache 32KB）：

  | 项目 | 耗时 |
  |---|---|
  | MSR，特征图全在 PSRAM | 81ms |
  | MSR，27KB 特征图在内部 RAM | 71ms |
  | MSR，输入先缩到 160x120 | 78ms（只快 4%，所以不缩） |
  | 检测任务的栈放内部 RAM | 79ms（没区别，所以栈留在 PSRAM） |
  | MNP，每个候选 | 12.6ms（26KB 内部 RAM 时 10.1ms，但和 MSR 抢同一个块，放不下） |
  | esp-dl 内核放回 IRAM（去掉 `stackpet.lf`） | 没区别 |

  RADAR 实际运行：没人时检测约 10.5 次/秒，推理 80ms 左右，结果从抓帧到可用约 140ms；锁定时 core 1 要画锁定画面，降到约 7.4 次/秒。
  修复撞名之前是 3.7 次/秒，而且结果是错的。
- 输入输出：FrameGrabber 的 320x240 RGB565 帧原样送进 `FaceModel::run()`。MSR 内部会缩到 160x120，MNP 从原图裁候选框，所以比先降采样再检测更准。
  结果转成归一化坐标的 `Face`：中心 -1..1，宽高是占画面的比例，带分数；按分数排序，最多 4 个。
- 任务 `face_detect`：core 0，优先级 1，和 mooncake 主循环相同，两者轮流跑，不会压住 UI 和舵机。16KB 栈放在 PSRAM（这个任务不写 flash）。
  任务只创建一次，`stop()` 后挂起，等下一次 `start()`。分析当前帧之前先 `requestRgb()`，让下一帧在 core 1 上同时转换；每跑完一次 `vTaskDelay(1)`，让 IDLE0 能喂看门狗。
- 生命周期：`start()` 时在任务里加载模型（不懒加载，失败马上就能发现）；`stop()` 先等当前这次推理结束（最多 3s），再释放模型。
  加载前 PSRAM 不足 1MB、或者分配失败，就置位 `stats().failed`。
- 内存：
  - 静态只占 32 字节 .bss；esp-dl 的 `.iram1` 汇编核心已经被 `stackpet.lf` 放到 flash。
  - 没有任何全局构造，这一点在 ELF 的 `.init_array` 里核实过。模块注册表是函数内静态量，第一次加载模型时才创建，`Model::minimize()` 加载完马上清空。
  - RADAR 退出时一定 warm reboot，所以 AI 模式下不会有 esp-dl 的运行时内存。
  - 实测加载后占用：PSRAM 约 400KB，内部 RAM 约 63KB（其中 27KB 是 MSR 的特征图，其余是小对象），加载约 170ms。
- 日志（tag `FaceDetector`）：
  - 模型加载耗时，占用的 PSRAM / 内部 RAM，两阶段实际拿到的内部 RAM 预算。
  - 前 3 次推理各一条耗时。
  - 之后每 5 秒一条汇总：次数、fps、平均 / 最大推理时间（拆成 MSR + MNP 和平均候选数）、结果延迟、人脸数、最高分。
  - `stop()` 时打印释放后相对加载前的差值。应用还拿着的帧会算成没还的 PSRAM，每帧 150KB。
- `FaceTracker`（同一个文件里，纯 C++，有主机测试）：跟住一个目标。
  - 离上次位置 0.45 以内的检测算同一个人，位置做指数平均，新值权重 0.6。
  - 连续漏掉超过 2 次、或者 800ms 没看到，就把这个目标丢掉；分数低于 0.5 或宽度不到画面 3% 的框直接忽略。
  - `hits` 只数**连续**命中：中间漏一帧，就从 1 重新数。

**RADAR 应用（`apps/app_radar/*`）**
- 给用户的说明（能做什么、做不到什么）：
  - 头一直朝同一个方向 360° 转（USB 线插在底座上，头可以无限转）：先按 SPIN 的速度快转两圈，再停 8 个固定方向环顾一圈，循环。
    转动中的画面太糊，几乎认不出人脸，主要靠环顾时停下的那几眼发现人。正后方偏一侧约 15° 的窄条环顾时看不到（那里本来就够不到）。
  - 机身上的 USB-C 和 PORT.A/B/C 都跟着头转：转动扫描时机身上不要插线、不要接 Grove 单元，会绕线。
  - 头被手按住、或被线卡住转不动时：先停 3 秒再试；一分钟内卡住 3 次就改成来回扫描，状态在「头被卡住了？」和
    「摸摸头恢复转动」之间每 2 秒轮换；摸一下头顶（松手 1 秒后）或重新打开应用恢复转动。
  - 万一绕了线：先上滑退出 RADAR，再把线反方向解开，别在它转的时候硬拽。
  - 只认正脸：背对、低头、戴口罩、帽檐压低都可能认不出；照片、海报、电视里的人脸也算。它不认人，家里谁都会被当成「入侵者」。
  - 被发现后：转头对准，摄像头画面上红框收拢到脸上，红灯快闪、警笛 3 秒，然后换成生气脸一直瞪着，人走开约 1.5s 后冷却。
    舵机限位是 ±128°：128°–149° 的人能锁定（脸偏在画面边上）；正后方约 60°（149° 以外）只在雷达屏上显示光点，不确认、不报警。
  - 让它消气：摸一下头顶。之后进入友好模式，直到 30s 内都没再看到人为止；离开 30s 以上再回来，它会重新报警。
  - 人脸检测全在本机完成，不联网，不保存任何画面。
- 代码分三层：
  - `radar_logic.*`：纯 C++，包含扫描规划、状态机、角度换算和转动时的角度跟踪，有主机测试。
  - `radar_view.*`：LVGL 的雷达屏和锁定画面。
  - `app_radar.cpp`：把检测、舵机、灯、声音接起来。
  头像走 `stackpet::face::create()`，跟随用户选的风格。
- 转动扫描（`RadarConfig::spinScan`，用户 2026-10-12 选的「快转两圈再停着看一圈」）：
  - 偏航舵机切到轮式模式（`Servo::rotate()`），一直朝一个方向转。实测 `rotate(v)` 朝 -yaw 转，300–900 时约 0.46°/s 每单位
    （700 转一圈 1.08s，`cal_session.log` 的 SPINLOG）；快转用 800（约 370°/s，和 SPIN 游戏一样），每次转满 `spinTurns` = 2 圈（按转过的角度算）。
  - 环顾一圈：`lookStops` = 8 站，停在固定方位上（`lookFromYaw` … `lookToYaw` 均分，间隔约 42°；镜头水平视野 52°，有重叠）。
    这个范围是电位器读得到的那一段、两头各离盲区 10°（`kLookEdgeMargin`，由零点算，零点 460 时是 −133.8° … +158.8°），
    所以头不会停在盲区里，逃逸只在快转中途发现人脸时才可能用到（expert 评审的建议）；盲区那一侧靠两端两站的视野边缘覆盖，
    中间约 15° 看不到（超过 149°，本来就只出光点）。第一站是头前方至少 40° 的那一站（`lookLeadYaw`，要留出刹车距离）。
    - 站与站之间用 700 的速度移动。停转后 PWM 0 是自由滑行：真机从 700 滑约 17°（2026-10-12 测 6 站，等于 54ms 的行程；
      原先以为 6°，停站全都晚 10–18°），所以按「当前速度 × `lookCoastMs` = 55ms」提前停转。
    - RADAR 的主循环一个 tick 约 55ms、偶尔 150ms（和人脸检测共用 core 0、同优先级轮转），按 700 的速度一个 tick 就是 18°。所以：
      - 头的控制（读舵机 → 状态机 → 发指令）放在拿 LVGL 锁之前做，不被渲染耽误；
      - 刹车点在下一个 tick（按至少 40ms 再加 30ms 余量算）之前就会到时（每步一开始就检查，从静止起步也算），状态机把刹车点交给 app
        （`HeadCommand::brakeArmed` / `brakeAtYaw` / `brakeVelocity` / `brakeSerial`）：app 让头转起来、连续读舵机直到到点再停转，
        最多等 200ms。这段时间 app 的任务优先级临时 +1，否则每次等舵机应答都会把一整个 10ms 时间片让给检测任务，读一次要 9ms。
    - 真机（2026-10-12 第 4 轮）：大多数停站离方位 ±3° 以内；盲区后的第一站读数常是 0–8（离预期的 32 差 7–10°，
      可能是出口的「0 平台」）；偶尔一站短 20°（头那一下转慢了）。按真机 tick 分布仿真：99.9% 在 5° 以内、全部在 10° 以内、
      没有停进盲区（只按 tick 判断时是 81% / 97.5% / 0.4% 停进盲区）。
    停稳 150ms 后拍的帧算静止帧；每站看完 2 帧静止帧（最多停 0.7s）就去下一站，一圈约 4–5s。
  - 实测（2026-10-12，人坐在 1 米处）：连续转动时 45°/s 以上基本检测不到人脸（400°/s 连转 2 分钟 0 个候选），
    改成快转加环顾后，2 分钟里发现 3 次，锁定后人脸在画面中央（坐标 ±0.1 以内）。
  - 转动中的头部角度（`SpinTracker`）：主循环每个 tick 读一次舵机原始位置（`Servo::readRawPosition()`）。有效读数 0–1000 对应 312.5°，
    换算时取离预测值最近的那一圈（不跨圈跳变）；超过 1000 的读数是电位器盲区（约 48°），-1 是舵机没应答。
    - 预测按 `rotate()` 的指令来（`setCommand()`，app 每个 tick 在读数之前设好）：0.46°/s 每单位，一阶，加速时间常数 30ms、
      减速 55ms（自由滑行）。盲区按读数的单位算是 47.5°，按头的转角其实约 42°（转一圈比 1000 步按区内速度算的长 12%），
      所以盲区里估计值走快 1.15 倍。盲区里、没应答时、读数不可信时都按这个模型走，所以停在盲区里的头，估计值也会停下
      （之前按上次的角速度外推，停站会塌缩，gate 评审 M1）。
    - 读数和预测差超过 5° + 预测转角的 30%，就不采信：盲区边上会有单个离谱的读数（实测 221、801、403），
      头卡住、或者估计值漂了也会这样。连续 3 个彼此吻合、从上次采信的读数算起来得及转到的不可信读数，在说得通时才采信：
      被驱动时转速和指令差不到一半（至少跨 15ms）；静止时离估计值 30° 以内（至少跨 30ms；更远的一律不采信，停在盲区边上时
      同一个毛刺值可能连读几次）；或者持续 250ms（被按住的头）。app 盯刹车点时 1ms 读一次，毛刺也会连着重复，所以不能只看个数。
    - 速度：先按指令模型推进，再用间隔至少 15ms 的两次读数修正一半。
    - 标定都来自 `cal_session.log`（转一圈的周期、盲区穿越时间）和第 4 轮真机的停站日志；换一台舵机可能要重测
      `yawRatePerUnit`、`slowTauMs`、`bandRateScale`、`lookCoastMs`。
    - 开始转之前：零点直接用舵机自己的（NVS，`Servo::getZeroRawPosition()`，本机 462），起始角按读数换算
      （`getCurrentYawAngle()` 在 ±128° 限位处会被钳住，头可能转得更远）。
  - 进入轮式模式前要等偏航轴的弹簧动画停下（它下一次写位置会退出轮式模式），并关掉自动卸力（会让转动停下）；
    `rotate()` 进入轮式模式时还会先丢掉动画最后那次「到位对齐」（`Servo::begin_rotation()`），否则它晚一个 tick 写位置，
    又会退出轮式模式（gate 评审）。只能丢这一次写：把动画 teleport 到当前角度会让动画重新开始，下一步照样写位置，
    头根本转不起来（第 3 轮的回归，真机上 raw 一直停在 777；`stackpet_radar_servo_test` 防它再出现）。
  - 切回位置模式（`leave_spin()`）：
    1. 先 `rotate(0)` 滑行到 60°/s 以下（最多 0.3s）。高速时直接切位置模式等于反接制动，电流和齿轮冲击最大（expert 评审 S4）。
    2. 读数不在有效区里、或离盲区边缘不到 20 步（毛刺都出在那里，`clearOfDeadBand()`）：以 300 的速度沿到目标的较短方向转出去，最多 1.5s。
       超时（头被手或线卡住、传感器坏了）时，如果是在确认人脸，就放弃这次确认（`cancelCheck()`，那个方位照样静默），留在轮式模式不动；
       其他情况照样切位置模式。
    3. `stopRotation()` 连读两次位置，相差 8 步以内才算数，把目标写成当前位置再恢复限位，原地保持。
       读不到有效位置时先关扭矩再恢复限位，否则舵机会全速冲向轮式模式之前留下的旧目标（expert 评审 S2）。
    - 转出盲区期间，Checking 的「停稳 450ms」和确认时限都从回到位置模式那一刻才开始算。
  - SCSCL 的 `SwitchMode(1)` 读到 0/0（已经在轮式模式，上次切换的应答丢了）时不再把它当成原始限位缓存；
    开机时 HAL 把真实限位（或修复用的 NVS 值）先缓存进去（expert 评审 S3）。
  - 转不动（`SpinStallWatch`）：被驱动时，按采信的读数 1s 内沿驱动方向前进不到 10°，或者 1s 都没有可信读数，就算卡住。
    只看读数、只算驱动方向：卡住时估计值会在指令模型和读数之间来回摆 30–40°，按估计值算会被误当成在转。
    先 `rotate(0)` 停 3s 再试；一分钟内卡住 3 次，改成来回扫描并显示「头被卡住了？」/「摸摸头恢复转动」（每 2s 轮换），
    摸头（`tapped`，松手后 1s 再转，手不会还搭在头上）或重新打开应用恢复（expert 评审 M2，pm 的 P2 建议）。
  - 快转圈数按估计值沿转向的有符号转角累计：估计值往前跳又跳回来（毛刺、卡住）不算转了（gate 评审）。
  - 够不到的人脸（方位超过 `aimYawLimit` + 0.8 × 半视场，约 149°）只出灰色光点（`Blip::unreachable` → `BlipKind::Behind`），
    不进 Checking；雷达屏上的两条虚线就画在 ±149°（来回扫描时画在扫描限位 ±125°）：
    否则每次都确认超时，而快转两圈正好回到原处，环顾的第一站又对着他，前面的人就一直轮不到（pm 评审发现）。
    确认失败的方位 20s 内、±25° 以内不再急停（`checkMuteMs` / `checkMuteYaw`），状态区显示 1s「没看清」。
  - 舵机温度保护：转动时每 5s 读一次偏航舵机温度（`Servo::readTemperature()`），≥55°C 降到 500，≥65°C 停转，降回 55°C 以下再恢复；
    连续 3 次读不到（15s）也按过热停转。雷达屏显示「舵机降温中」。SCS0009 自己 70°C 卸力（13 号寄存器的出厂值），所以留出余量
    （expert 评审 S1）。实测连转几分钟 42°C。
  - 扫描时抬头 20°（坐在桌边的人脸；垂直视野 34°）。雷达屏的扫描线跟着实测的头部朝向转，灯上的绿点绕着两排灯循环跑。
  - 运动侦测退化方案需要镜头静止，切换时改回下面的来回扫描（`setSpinScan(false)`）。
- 来回扫描（`SweepPlanner`，只在退化方案里用）：
  - yaw 在 ±1250 之间匀速往返，最高 32°/s；两端以 60°/s² 刹车，速度正好在限位处降到 0 再反向。
  - 主循环每个 tick 把目标位置交给舵机；这时关掉 auto angle sync，弹簧能保持动量。
  - 扫描速度跟着检测快慢调整：视野 × fps ÷ 6，限制在 10–32°/s，保证一张脸在视野里至少被看到 6 次。
- 状态机（`RadarMachine`）：

  | 阶段 | 进入条件 | 表现 | 离开条件 |
  |---|---|---|---|
  | Scanning | 刚打开 / 冷却结束 / 确认超时 | 雷达屏：扫描线指向头的朝向（+yaw 画成顺时针），余辉长度随转速变化。光点按方位和估算距离显示（距离由人脸宽度推算，外圈是 3 米）：浅绿 = 看到，红 = 已确认，青 = 友好期。灯上的绿点跟着头转；每 1.8s 一声 ping；状态「快速扫描」/「环顾搜索」 | 任何一帧里看到够得着、没被静音的人脸（友好期除外） |
  | Checking | 扫描中看到人脸 | 急停，按抓帧时刻的头部角度（转动中扣掉 40ms 的帧延迟）加上画面偏移，转到人脸的方向；雷达屏照常，状态「发现目标？」，灯琥珀色呼吸 | 头停稳 450ms 后连续 2 帧静止帧都有人脸 → Locking（按静止帧再精确瞄准）；1.5s 内确认不了 → Scanning |
  | Locking | 确认目标 | 全屏显示检测用的那一帧，框和脸严格对齐；红框加四角标记在 350ms 内从屏幕边缘收拢，形成「聚焦」效果。「锁定目标」闪烁，红色屏框 4Hz 闪烁；红灯 8Hz 快闪，警笛响起，都持续 3s | `lockViewMs` = 3000，也就是整段警报都显示镜头画面 |
  | Angry | 锁定画面结束，或冷却中又看到人 | Angry 头像加 AngryDecorator，眼睛跟着残余偏差看。头部微调只用头停稳 450ms 以后拍的帧，误差超过 2.5° 才动，每次修正误差的 0.7 倍。锁定后 4.5s 弹出气泡「摸摸头，我就不生气」 | 至少瞪满 3s，并且 1.5s 内没再看到人 |
  | Cooldown | 人走了 | Doubt 表情加「人呢？」（从原谅状态过来时是 Happy）；pitch 回到扫描角度；灯是绿色慢呼吸 | 4s 后回到 Scanning；期间又确认到人，直接回 Angry，不再响第二次警报 |
  | Forgiven | Locking / Angry / Cooldown 时摸头 | 立刻停警报，播 `Sfx::Success`；Happy 加 HeartDecorator 和「好啦，不生气了～」；粉色灯 | 2.5s 后进入 Cooldown。之后是友好期：看到人只显示青色光点、不锁定，状态显示「友好模式」。友好期内每次看到人都续到 30s 以后（`friendGraceMs`），也就是**最后一次看到人 30s 之后**才结束，摸过头的人坐在桌前就不会被反复报警 |

- 退化方案（运行时自动切换）：
  - 触发条件：模型加载失败，或者平均推理超过 1.43s（不到 0.7fps）。
  - 切换后停掉检测，FrameGrabber 改成输出 40x30 亮度图，扫描改为来回扫、每走 40° 停 1s。
  - 头停稳 450ms 后取参考帧；`MotionDetector` 连续 2 帧看到运动（最大连通块 ≥4 格、分数 ≥0.008），就按同样的流程锁定，框画在运动块上。
  - 生气阶段没法跟踪，瞪满 3s 就进入冷却。雷达屏状态显示「动态侦测」。
  - 没有摄像头时只转，状态显示「摄像头不可用」。
- 雷达屏右上角的状态依次可能是：摄像头不可用、预热中…（模型加载中）、光线太暗（画面平均亮度低于 30）、发现目标？、没看清（确认失败后 1s）、头被卡住了？/ 摸摸头恢复转动（轮换）、舵机降温中、友好模式、动态侦测、快速扫描 / 环顾搜索（转动扫描）、扫描中（来回扫描）。
- 退出：`onClose()` 第一件事是退出轮式模式（`stop_spinning_now()`：同样先滑行、转出盲区，最多阻塞 0.6s，再 `stopRotation()`）；
  然后依次停检测任务、FrameGrabber、声音，恢复 auto angle sync 和自动卸力，删掉 UI，关灯，最后 warm reboot 到序号 11。
- 标定（`app_radar.cpp` 顶部，2026-10-12 用人脸实测，主机工具 `aim_cal.py`）：
  - 水平半视场 26.1°、垂直 17.1°（原先估的 33° / 25° 偏大）；画面右边的人脸在 +yaw 方向（`kImageXToYaw = 1`，原先是 -1，
    头会往反方向转，就是用户看到的「锁定后没正对着人」），画面下方的在 -pitch 方向（`kImageYToPitch = -1`）。
  - REMOTE 摇杆用的是同一个关系，`kLookYawSign` 也改成了 +1（原先往右推，镜头往左看）。
  - 方法：头在人脸附近按已知角度移动（偏航 ±12°、俯仰 ±8°），读检测器给出的人脸坐标，对头部角度做线性拟合，两轮 20 个点。
  - 转动中的帧延迟 `kSpinFrameLatencyMs` = 40ms 是估计值；转动中发现的人脸只决定 Checking 往哪转，最终瞄准用静止帧，所以它不敏感。
  时序参数在 `RadarConfig`，跟踪阈值在 `FaceTrackerConfig`。
- 日志：`spin: look-around turn / fast turns at yaw …`（环顾结束那条带 `looked from` 和每一站停的方位）、`spin: face seen, checking at yaw … pitch …`、
  `spin: the head does not turn, resting 3000 ms` / `spin: the head is stuck (3 stalls in a minute), sweeping instead`、
  `target locked at yaw … pitch …: face at (x, y) size w x h, score s, n face(s) in the frame`、每 30s 一条舵机温度。
- 测试版的注意事项：`STACKPET_AUTOOPEN=11` 的测试版**退不出 RADAR**。上滑退出会 warm reboot，launcher 的「只自动打开一次」是函数内静态量，
  重启后又会自动打开 RADAR；关机重开也一样。要测别的应用必须刷正式版。要让设备马上安静，长按底部 RST 约 3 秒进下载模式。
- 测试版的上机手段（正式版都没有）：
  - `-DSTACKPET_CAL=1`（配 `STACKPET_AUTOOPEN=7`）：REMOTE 同时跑人脸检测器，网页多两个接口：`GET /debug/faces`（最新检测结果，JSON，
    带抓帧时间）和 `POST /debug/spin?v=<速度>&ms=<时长>`（轮式模式转一段，舵机原始读数以 `SPINLOG` 行写进日志）。
    主机工具 `stackchan/tools/aim_cal.py <令牌> aim|spin` 用它们做上面的瞄准标定和转速测试（需要有人坐在 1 米处）。
    同目录还有不复位读串口的 `serial_capture.py`、复位后读串口的 `reset_capture.py`。
  - `-DSTACKPET_FD_BENCH=1`：RADAR 不启动检测器，改跑 `vision/face_bench.*` 的基准（头、喇叭、灯都不动）；REMOTE 也会同时跑，
    网页多一个 `GET /debug/rgb565`，返回检测器看到的原始 RGB565 帧（宽高在 `X-Width` / `X-Height` 头里），可以和 `/snapshot.jpg` 对照。
    基准每种方案在新任务里跑 15 帧，日志 tag `FaceBench`，最后一行是 `probes N/N passed: PASS`：
    - `probe face`：内嵌的正样本探针 `vision/testdata/face_probe.jpg`（Grace Hopper 的公有领域肖像，来源见同目录 README），
      每一帧的最佳人脸和预期框的 IoU 都要 ≥ 0.4（实测 15/15，IoU 0.64，MNP logit 是 [-3.88, 4.12]，人脸概率 1.000）。
      `probe face/d` 是同一张图用 FaceDetector 的配置跑。
    - `probe gray` / `probe black`：纯灰、纯黑图，必须 0 张脸。`probe mirror` 是左右翻转的摄像头画面，只记录不判定。
    - 其余是摄像头实拍和耗时对比，不判定。用完记得 `-DSTACKPET_FD_BENCH=0`。

### SoundPlayer（audio）
见 `sound/sound_player.h`。独立任务 + 队列；方波/正弦/三角/噪声合成带包络（避免爆音）和滑音；Ogg/Opus 用 xiaozhi 的 `OggDemuxer`
+ `esp_opus_dec` 解码到 24kHz（或解码后重采样）。每个 `Sfx` 设计成有辨识度的短音效（≤1.5s），`MuTouRen` 是童谣「木头人」的三音旋律。

radar 在 `Sfx` 枚举末尾追加了两个（原有的值不变，网页里的名字是 `radarping` / `alarm`）：
- `Sfx::RadarPing`：1400Hz 正弦 90ms（略往下滑到 1360Hz），后面跟两次渐弱回声（音量 85 / 32 / 12，间隔 120 / 150ms），共 0.49s。
  RADAR 扫描时每 1.8s 一声，播放器忙就跳过。合成器的包络没有指数衰减，所以用渐弱回声来模拟「快速衰减」。
- `Sfx::Alarm`：方波双音警笛，C6 1047Hz 和 G5 784Hz 各 250ms，交替 6 次，**正好 3s**。这是唯一超过 1.5s 的音效，对应用户要求的「警报声 3 秒」。
  锁定时用 `playSfxExclusive` 打断 ping；摸头原谅时 `stopAll()`。

### 字体与图标（audio）
- `fontCn20()`：从资源分区读 `font_puhui_common_20_4.bin`（先核实 index.json 里的实际文件名），用 `cbin_font_create()` 创建，懒加载一次。
- 标题字体：`firmware/tools/gen_stackpet_fonts.sh` 用 `npx lv_font_conv@1.5.2` 和 `managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf`
  生成 `main/stackpet/ui/fonts/font_stackpet_title_{28,44}.c`（4bpp）。字符集 = ASCII 可见字符 + 自动从 StackPet 源码里扫出来的全部汉字和全角标点。
- 图标：188x150、LVGL v9 bin、`RGB565A8`（cf=0x14，先 RGB565 平面再 A8 平面，参考 `assets_bin/icon_dance.bin`），
  风格对齐现有图标（粗黑描边贴纸风，粉/青点缀）。由 `firmware/tools/gen_icons.py`（`uv run --with pillow`）生成到 `main/assets/assets_bin/`。

### 唤醒词（audio → 主控上机调优）
`CustomWakeWord`：AFE（`AFE_TYPE_SR`、`AFE_MODE_HIGH_PERF`、有回采时输入格式 "MR"、关闭 WakeNet、开 AEC + VAD）→ fetch 任务 → 语音段队列 → MultiNet。
保留 index.json 里 `multinet_model` 的配置路径；检测到时日志打印 prob；阈值可配置。

**上机实测（2026-10-11）**：MultiNet7 在 CoreS3 上每 32ms 音频要算 41–47ms（mn5q8 更慢，约 68ms；乐鑫基准 11ms 是八线 PSRAM 的板子），
连续识别跟不上实时，直接喂会让 AFE 输入环形缓冲溢出并饿死 IDLE。因此改成 VAD 门控：
- fetch 任务（core 1，优先级 4，高于 LVGL 的 3）实时取走 AFE 输出，只把语音段（含 AFE 的 vad_cache 前置音频和 400ms 尾巴）切成 512 点的块排队；
- 识别任务（core 0，优先级 1，每 4 块让一次 CPU）在队列上跑 MultiNet，说完后追上进度；积压超过约 3s（94 块）就丢最旧的块，保证延迟有上限；
- AFE 任务在 core 1、优先级 5；VAD 用 WebRTC 的 mode 3（mode 1 时房间背景声约 60% 被判成语音）。
代价：周围持续有人说话时唤醒延迟会变大、可能漏掉；xiaozhi 模式下内部 SRAM 余量约 35KB（初始化瞬间最低约 10KB，首次对话实测能正常创建音频处理器）。

第二轮评审后的加固（都已上机验证）：积压超过 94 块时一次裁到最新 40 块、只清一次 MultiNet 状态（滞回，否则顶格时永远凑不齐一整句）；
Start/Stop 递增代数，排队时的代数和当前不一致的块和结果一律丢弃（防止按键开聊后旧音频又触发唤醒）；丢弃日志每秒最多一条；
`CONFIG_SEND_WAKE_WORD_DATA=n` 时不缓存唤醒音频、也不在唤醒瞬间创建编码任务；识别栈按实测（峰值约 1.5KB）缩到 4KB；
上游 `AfeAudioProcessor` 创建失败时打日志降级，不再空指针崩溃。

**还没做的优化（需要实测音质 / 吞吐再决定）**：上游 `opus_codec` 任务的 24KB 内部栈改放 PSRAM；`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`；
LVGL 绘制缓冲 20 行 → 10 行（省 6.4KB）；xiaozhi 空闲时让摄像头停流（实测 MultiNet 只快约 15%：47 → 40ms/块）。

### OTA（audio）与分区
`Application::CheckNewVersion()`：发现新版本时只打日志、不自动下载刷写（服务端下发 force 也不刷），激活和服务器配置照常处理。

分区表（`partitions.csv`）：
- `assets` 从 4MB 扩到 0x580000，因为 mn7_cn 有 2.68MB。
- RADAR 接入 ESP-DL 人脸检测后 app 约 6MB，两个 0x4f0000 的 OTA 槽放不下，所以去掉 ota_1，把 ota_0 扩到 0x9E0000。
- nvs、assets 的偏移都没变，Wi-Fi 配置和资源分区刷机后保留。
- 只剩一个 app 槽后，SETUP 里的「固件升级」会在 `esp_ota_begin` 安全失败（`ESP_ERR_OTA_PARTITION_CONFLICT`）。升级一律走 USB 刷机，恢复出厂用整片备份。

`main/stackpet/stackpet.lf`：esp-dl 的 TIE728 汇编核心自带 `.section .iram1`，约 5KB。它们只在 RADAR 的检测任务里跑，所以用 ldgen 放到 flash_text，把内部 RAM 留给 AI 模式。
实测放回 IRAM 推理速度没有差别（278 vs 274ms，撞名修复前测的；它也不是撞名问题的原因）。

## 测试

- **games**：MotionDetector（静止场景、整体曝光变化、局部运动的质心）、FreezeGame（假时钟下的阶段顺序、ChantWord 0..3、有运动被抓、无运动过关、摸头获胜、难度递增）、
  SpinGame（速度曲线方向和上限、Tick 单调、结果落在类别范围内、targetYaw 在限位内、类别非空）、
  表情编排（`MoodScript` 节拍时序与只在变化时上报、每个阶段 / 结果性质的脚本合法且拍长 ≥400ms、Caught 的 Surprised→Angry→Laughing、
  `WatchMood` 的迟滞与防抖、做决定 6:5:3 的性质标注）。
- **web**：抽出来的纯函数（例如 query 解析、MJPEG 分段头）写主机测试。
- **audio**：音色合成抽成纯函数后测试（采样数与时长、滑音、不削顶）。
- **radar**：
  - `stackpet_radar_logic_test`：
    - 扫描规划：不超出限位、加速度有上限、两端速度降到 0、中段严格匀速、卡顿时步长有上限、从限位外出发也会往回走、停顿扫描；
    - 角度换算：tan 映射、镜像开关、俯仰方向；自适应扫描速度和距离估算；
    - 状态机全流程：ping 节奏、连续 2 次才锁定、对准角度、锁定画面时长、警报 3s、人在就一直生气、人走 1.5s 后冷却、误报也要瞪满最短时间、冷却中回来不再报警、摸头原谅和友好期（30s 续期）、微调的稳定时间与死区、对准角度限幅、扫描历史的延迟补偿、退化方案的停顿扫描。
    - 转动扫描：
      - `SpinTracker`（带惯性的头模型 + 带盲区的电位器）：跨盲区不跳圈（头比模型快慢 10%）；盲区里停下，估计值也停下，再转起来接上读数；
        盲区出口的毛刺 221 和静止时反复读到的 801 都不采信；头被按住时估计值停在读数上；没应答按指令走；clearOfDeadBand 离边缘 20 步。
      - `SpinStallWatch`：tick 20–30 / 30–45 / 40–60 / 50–90ms，在读数区和盲区里按住头，1.5s 内都能发现、不会提前；故意停着不算。
      - 真机 tick 分布（中位 19ms、3.8% ≥40ms、最长 86ms）的闭环 144 次，app 在 tick 之间盯刹车点：≥99% 的停站在方位 5° 以内、
        全部在 10° 以内、不停进盲区、最差 ≤10°（实测 99.8% / 100% / 0 / 6.3°），无误报卡住。
      - 闭环（`RadarMachine` → 指令 → 头模型 → 传感器 → `SpinTracker`，168 次）：起始角每 15° 一个，七种工况（真机惯性和 20–30ms 的 tick；
        模型惯性、快慢 12%、时间常数 15/60ms、毛刺 3%/10%，tick 10–40ms 随机）。断言：快转两圈约 720°；每圈环顾 8 站，每站都停在固定方位附近
        （误差按滑行差和 tick 长度算）、按顺序一站接一站；真机工况不会停在盲区里；没有人脸时从不进 Checking；转动中 `SpinStallWatch` 从不误报；停站时估计误差 < 15°。
      - 快转两圈后环顾 8 站（固定方位、间隔 42°、同一方向、第一站留出刹车距离）、再回快转；环顾停站看到人脸按静止帧转过去确认；
        转动中看到人脸急停回转（按实测历史和帧延迟算方位）、两帧静止帧确认才锁定、确认超时继续转；还在轮式模式（转出盲区）时确认不计时、
        期间的帧不算；放弃确认后继续转、方位静音；友好期不急停；够不到的方位只出光点、边缘方位限幅；确认失败的方位静音 20s；过热暂停与降速。
  - `stackpet_radar_servo_test`：`stackchan/motion/servo.cpp` 加 smooth_ui_toolkit 的动画（主机时钟用 `tests/stubs/hal/hal.h`）。
    动画刚停、还欠一次到位对齐时 `rotate()`，之后 `update()` 不再写位置；teleport 会再写（第 3 轮回归的原因）；`stopRotation()` 后位置模式照常。
  - `stackpet_radar_vision_test`：
    - FaceTracker：连续命中、平滑、不被别的脸抢走、漏一帧就断连击、丢失目标、门限外另起新目标、过滤低分和过小的框；
    - YUYV → RGB565：灰 / 白 / 黑 / 红、2x2 降采样、不越界写；平均亮度；
    - 两个新音效：警笛正好 3s 且高低交替；ping 是同音高、逐次变弱的正弦，长度远短于 1.8s 周期；不削顶。
- 运行（用你自己的目录）：
  ```bash
  cd firmware/tests   # 只要 CMake 和支持 C++17 的编译器，不需要 ESP-IDF
  T=/tmp/stackpet-host-tests-<track>
  cmake -S . -B $T && cmake --build $T && ctest --test-dir $T --output-on-failure
  ```

## 每条开发线的完成标准

1. 用自己的 build 目录完整编译通过，自己负责的文件里没有新增警告；
2. 主机测试全部通过；
3. 最终报告：改了哪些文件、关键设计决定、已知限制、需要别的开发线或主控做的事（比如新增了要重新生成字体的汉字）、上机怎么测。
