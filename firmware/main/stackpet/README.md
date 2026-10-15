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
- 遥控器（官方的 StickC-Plus + Mini JoyC，刷 StackPet 的遥控器固件）：机器人开机就在听；主屏上摇杆左右换应用、回中按住打开，M5 键回桌面；
  遥控模式里摇杆转头，还有摇头、点头、转圈、换表情的手势（见「遥控器」一节）。

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
| **usage**（主屏屏保：时钟 + Claude 用量） | `stackpet/usage/*`（`style_*.cpp` 四种风格、`clock_style.h` / `clock_widgets.cpp`、`fonts/font_usage_*.c`）、`apps/app_launcher/app_launcher.{h,cpp}` 的 `screensaver_update()` / `close_screensaver()`、`_screensaver` 的类型和亮度、插电那几个成员、`open_app()` 开头的 `UsageFeed::stop()`、`stackpet_remote_update()` 里 M5 键叫醒屏保和屏保上换风格那两段、`hal/hal.{h,cpp}` 的 `hasExternalPower()`、`hal/board/hal_bridge.h` + `hal/board/stackchan.cc` 的 `board_has_external_power()`、`hal/hal.h` + `hal/hal_espnow.cpp` 的 `joinRouter()` / `leaveRouter()`、`s_router_*` 和 `_wifi_init()` 里的 `esp_netif_init()`、`stackpet/remote/remote_link.{h,cpp}` 的 `pause()` / `resume()` / `paused()`、`tools/gen_usage_fonts.sh`、`tools/stackpet_usage_setup.py`、`tools/usage_preview/*`、`tests/stackpet_usage.cmake` + `tests/stackpet_usage_test.cpp`、`tests/CMakeLists.txt` 里 foreach 加的 `stackpet_usage.cmake` |
| **remote**（遥控器导航/手势/开机监听） | `stackpet/remote/*`、`apps/app_espnow_ctrl/*` 里标了 StackPet 的部分、`apps/app_launcher/app_launcher.{h,cpp}` 的 `stackpet_remote_start()` / `stackpet_remote_update()` / `open_app()` / `open_touched()` / `open_from_remote()` / `keeps_listening()` 和相关成员、`create_launcher_view()` 里改走 `open_touched()` 的那行和 AUTOOPEN 钩子里 `STACKPET_AUTOOPEN_REMOTE` 那段、`apps/app_launcher/view/view.{h,cpp}` 的 StackPet 段（`ready()` / `step()` / `focusedApp()` / `setOpenProgress()`、`_icon_app_ids`、长按进度条和提示）、`apps/common/home_indicator/home_indicator.cpp` 的 `update_home_indicator()` 里问 `takeHomePress()` 那段、`apps/app_radar/*` 的 `remote_session` / `remote_since_ms`、`kRemoteHintMs` 和 `update_status()` 里「按 M5 键回桌面」那一支、`hal/hal.h` + `hal/hal_espnow.cpp` 的 `startEspNow()` 重入、`stopEspNow()`、`attachEspNow()`、`setEspNowChannel()` / `getEspNowChannel()` / `getEspNowRxChannel()`、`hal/hal_network.cpp` 里调 `attach()` 的一处、`hal/hal.cpp` 的 `remote_attach_update()` / `remote_notice_update()` 和小智循环里调它们的两行、`hal/hal_ws_avatar.cpp` 的 `stop_streaming()` 里跳过省电的判断、`patches/esp-now.patch`（本地改的是 `components/esp-now` 的 `espnow.c`、`espnow.h`）和 `repos.json` 里登记它的一行、`stackpet/ui/ui.{h,cpp}` 的 `fontCn20Current()` 和 `s_cn20_data`、`hal/board/hal_bridge.h` 的 `espnow_keeps_radio_awake()`、`hal/board/stackchan.cc` 的 `SetPowerSaveLevel()` 那三行、`main/CMakeLists.txt` 的 `STACKPET_AUTOOPEN_REMOTE`、`remote/code/main/joystick/joystick_{basic.h,handle.c}`、`remote/code/main/esp_now/esp_now_init.{c,h}` 的 `espnow_send_everywhere()`、`remote/code/main/ui/ui.c` 的 `ui_init()`、`remote/code/main/ui/ui_running_screen.c` 的标题和 `B:` 显示、`remote/code/main/StackChan-RemoteControl-ESPNow.cpp` 的起始页和按键（M5 键、电源键换页）、`tests/stackpet_remote.cmake` + `tests/stackpet_remote_test.cpp` + `tests/stackpet_remote_link_test.cpp` + `tests/stubs_remote/*`、`tests/CMakeLists.txt` 里 foreach 加的 `stackpet_remote.cmake` |

`xiaozhi-esp32/` 被 git 忽略，改动必须体现在 `patches/xiaozhi-esp32.patch` 里（`fetch_repos.py` 会在干净的 v2.2.4 上应用它）：
改完后执行 `git -C xiaozhi-esp32 diff > patches/xiaozhi-esp32.patch`（包含官方原补丁和新改动），再在一份干净 checkout 上验证 `git apply --check`。
`components/esp-now/` 也一样（remote 的补丁，见 RemoteLink 的 `attach()`）：改完后执行 `git -C components/esp-now diff > patches/esp-now.patch`，
再在一份干净的 `c33383de` 上验证 `git apply --check`。`fetch_repos.py` 打不上补丁时只打印一行 `... skipped`（已经打过的再拉一次也是这样），
固件照样编得过；所以补丁在 `espnow.h` 里定义了 `ESPNOW_STACKPET_TASK_STACK_IN_PSRAM`，没有它时编译 `hal_espnow.cpp` 会出一条 `#warning`。

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

### 遥控器：开机监听、主屏左右选应用、长按摇杆打开、M5 键回主屏、摇杆手势（remote）
遥控器是 StickC-Plus + Hat Mini JoyC（固件在 `remote/code`），走 ESP-NOW 广播：默认信道 1，目标 id 0 表示发给所有机器人。
摇杆模式下摇杆移动 ≥5（IMU 模式 ≥10）或者按键状态变化时才发包；乐鑫 espnow 组件会把每个广播包重发 10 次，接收端按 magic 去重。
遥控器大约每 60–75ms 读一次手柄的按键，比这更短的点按可能漏掉（摇杆现在要长按，影响不大）；M5 键的点按会锁存到下一包，不会漏。

**怎么用**
1. 给遥控器刷 StackPet 的遥控器固件（刷一次就行，命令见下）。刷之前先整片备份。
   **刷完（备份也一样）拔掉 USB，按住电源键约 4 秒（屏幕灭了就松手）关机，再开机。** 备份和刷机都会复位遥控器，正好赶上它在读手柄时，手柄就会卡住（见已知限制）。
   卡住时遥控器屏幕顶上显示 `JoyC stuck`，再断一次电就好。
2. 遥控器开机直接进摇杆页（信道 1、ID 0），马上开始发包。
   - 侧面的电源键短按（1 秒以内）：依次切到 IMU 页、设置页，再按一次回到摇杆页。设置页里按正面的 M5 键（屏幕上的「Press to Start」）也回摇杆页。
     原来换页用的是 M5 键，现在 M5 键归机器人（回桌面）。
   - B 键切换激光。
3. 机器人在主屏时：
   - 摇杆往右推一下是下一个应用，往左是上一个。推住不放，0.6s 后每 0.45s 再换一个，最多再连换 10 个（推一次最多走 11 个）。
   - 摇杆回中、按住约 0.8s：打开当前的应用。按住时图标下面有一条白色进度条，中途松开就不打开。
     停在遥控器管不了的应用（SETUP、DANCE）上时，进度条下面同时提示「打开后用触摸回桌面」。
   - 屏保亮着时，遥控器的第一下只是叫醒屏幕。
4. M5 键回主屏：
   - ESPNOW.REMOTE、RADAR、FACE、SPIN、FREEZE 里按 M5 键就回主屏，和触摸上滑、点主页键一样。
     - 用遥控器打开时一定可以。ESPNOW.REMOTE 直接当接收端，说「遥控器连上啦～」。
     - 用触摸打开时，只要机器人这次通电以后收到过 StackPet 遥控器的包（动过摇杆、按过键都算；官方遥控器不算），也一样可以，断电才清零。
       还没用过遥控器时，触摸打开会先关掉遥控链路，回到主屏后才重新监听。
     - ESPNOW.REMOTE 例外：触摸打开先关掉链路，过了角色页它自己再开，之后照样能按 M5 回来。
     - 能按 M5 回去时，RADAR 前 15s 在状态栏轮流提示「按 M5 键回桌面」。
   - AI.AGENT、AVATAR、APP.CENTER、EZDATA、WEB.REMOTE 要自己连家里的 Wi-Fi，打开前先关掉遥控链路。
     只要这次通电用过遥控器，等它们连上 Wi-Fi、底部的主页键能用了以后，按 M5 也能回主屏；在那之前按的不算，要再按一次。
     - 要等多久（以前的日志）：AVATAR、EZDATA 底部主页键一出现就行（约 10 秒）；APP.CENTER 要等应用列表出来；WEB.REMOTE 要等连接页消失；
       AI.AGENT 在底部主页键出现以后还要再等约 10 秒（打开以后一般 20–40 秒，慢的时候一分多钟），能按时屏幕上方会提示 3 秒「现在可以按 M5 回桌面」。
       没出提示就还不能按，先用触摸回；在小智控制台换过资源包以后的第一次，提示是英文 `Press M5 to go home`。
     - 代价：这时 Wi-Fi 一直不省电（不然收不到遥控器），比平时耗电；这次通电用过遥控器以后，手点打开的也一样，到断电为止。
     - 这些应用里只有 M5 键有用，摇杆和别的按键都不起作用。
   - SETUP 和 DANCE（只用蓝牙）里遥控器基本不起作用，要用触摸回主屏。SETUP 大部分页面不联网：进过 Account → Unbind & Reset、看到账号信息以后，
     或者 Firmware → Check for Updates 检查完以后，这次 SETUP 里都能按 M5 回（账号页的红色按钮是解绑并恢复出厂，别点）。
5. 遥控模式（ESPNOW.REMOTE）里的手势：
   - 摇杆照常控制头；
   - 快速左右左右扳四下（换边 3 次）：摇头，「不要～」；
   - 快速上下上下扳四下：点头，「好的！」；
   - 沿边缘转一圈：原地转一圈，然后晕；
   - B 键快速按两下：换表情。

刷遥控器的命令（StickC-Plus 的串口是 `/dev/cu.usbserial-*`；实测 750000 波特率可以，921600 读不到 flash）：
```bash
esptool.py --chip esp32 -p <端口> -b 750000 read_flash 0 0x400000 ~/stackpet-remote-backup.bin  # 先整片备份（4MB，约 65s）
cd remote/code
idf.py reconfigure            # 第一次：先把 managed_components 拉下来，再改 m5gfx 的一行（IDF 5.5 才要）
sed -i '' 's/__has_include(<driver\/i2c_master.h>)/0/' managed_components/m5stack__m5gfx/src/lgfx/v1/platforms/esp32/common.cpp
idf.py build && cd build
esptool.py --chip esp32 -p <端口> -b 750000 write_flash 0x1000 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin 0x10000 StackChan-RemoteControl-ESPNow.bin
esptool.py --chip esp32 -p <端口> -b 750000 write_flash 0x0 ~/stackpet-remote-backup.bin       # 还原
```
（`sed -i ''` 是 macOS 的写法，Linux 上是 `sed -i`。）
也可以把仓库里的官方镜像 `remote/code/k151-R-StackChan-RemoteControl-ESPNow-v1.2-jyy-20260617_0x0.bin` 从 0x0 写回去。
实测它和出厂的应用部分逐字节相同，只有 NVS 不一样。

- **包格式**（`remote/remote_packet.h`，小端）：
  - 官方遥控器 8 字节：`[0]` 目标 id、`[1-2]` yaw -1280..1280、`[3-4]` pitch 0..900、`[5-6]` speed 0..1000、`[7]` 激光开关（B 键切换）。
  - StackPet 的遥控器固件多发两个字节：`[8]` 按键、`[9]` 遥控器自己的发送信道 1..14。官方机器人固件只读前 8 字节，照样能用。
    - 按键字节是「此刻按着」的状态：bit 0 = 摇杆按下，bit 1 = M5 键（`kButtonHome`）。按下、松开各发一包，按着的时候摇杆一动也照发。
      bit 2（`kButtonEverywhere`）：这一包是 M5 键按下时在所有信道上补发的副本（见下），它的帧头不说明遥控器在哪个信道。
    - 第 11 字节 `[10]`：M5 键到现在按了几次（0..255 循环）。同一次按下不管收到多少份副本都只算一次，松开那一包没收到也不影响下一次。
    - 第 10 字节其实和帧头里的信道重复（见 RemoteLink）：以帧头为准，两者不一致时打一条警告；遥控器已经按 10 字节刷好了，就没再改回去。
  - `parsePacket()` 只接受 8/9/10/11 字节，数值还得在遥控器会发的范围内：yaw ±1800、pitch -100..1100、speed 0..1000、激光 0/1、按键只有 bit 0–2、信道 1..14。
    其他 ESP-NOW 流量一律丢掉。
  - yaw / pitch 放宽到这么大，是因为遥控器把摇杆 0..4095 的原始值直接线性映射，不限幅。
    摇杆推得比标定值还远时，会发出 yaw -1581..1800、pitch -88..1081。解析时限幅回 ±1280 / 0..900，相当于推到底。
  - 链路在摇杆按下、松开时各打一条日志：`[Remote] stick pressed` / `released`；M5 键被取走时打
    `[Remote] M5 button: home (its copy on every channel)` 或 `(sent on N)`。
- **遥控器固件的改动**（`remote/code/main/joystick/joystick_{basic.h,handle.c}`、`main/esp_now/esp_now_init.{c,h}`、`main/ui/ui.c`、`main/ui/ui_running_screen.c`、`main/StackChan-RemoteControl-ESPNow.cpp`）：
  - 摇杆按键读 Mini JoyC（I2C 0x54）的寄存器 0x30：读到 0 就是按下，和 M5Stack 自己的 MiniJoyHat 驱动一致；读失败时保持上一次的状态。
    - 按住时 JoyC 的 RGB 灯（寄存器 0x40）亮蓝色，串口日志打 `StackPet: stick pressed` / `released`，寄存器原始值一变也打一行。
    - 第一版用的是「和开机时读到的值不一样就算按下」，上机实测一次按下都没认出来，所以改成了现在这样。
  - M5 键（BtnA）：主循环（20ms 一次）把 `M5.BtnA.isPressed()` 记进 `home_held`，摇杆页和 IMU 页发包时放进按键字节的 bit 1。
    这一位从 0 变 1 时（`joystick_update_buttons()`），`home_count` 加一，这一包照常发完以后，再用 `ESPNOW_CHANNEL_ALL` 在所有信道上补发一份
    （带 bit 2，`joystick_send_everywhere()`）：乐鑫 espnow 组件自己逐个信道切过去发，这里只重发 3 轮（平时是 10 轮）。
    默认国家码 "01" 只有 1–11 信道。组件发完不切回原信道（遥控器的 sdkconfig 没开 `CONFIG_ESPNOW_AUTO_RESTORE_CHANNEL`），
    所以 `espnow_send_everywhere()` 先记下当前信道、发完自己设回去（第一版漏了，按一次 M5 遥控器就停在 11 上，gate 发现的）。
    机器人在联网应用里待在路由器的信道上，只有这份副本收得到。补发要切 11×3 次信道，这期间发包循环停一会儿（没有实测多久）。
    发包的循环一圈要 60–75ms，比这更短的点按在 `wasPressed()` 时锁存进 `home_click`（32 位原子变量），发包时取走，这一包一定带 bit 1，下一包再松开。
  - 换页从 BtnA 改到电源键：`M5.BtnPWR.wasClicked()`，就是 AXP192 的短按中断（M5Unified 默认开着 `pmic_button`，REG36H=0x0C：长按门限 1s、4s 关机）。
    按超过 1 秒算长按，不换页；按住约 4 秒照旧关机。
    开机那一下可能在松手时被 AXP192 记成一次短按（M5Unified 不先清 REG46H；`begin()` 里那次 `update()` 只取走它之前的）。
    所以进主循环之前再 `M5.update()` 一次，把它读掉、不理；进主循环以后才松手的，已经按了整个开机过程（约 2 秒），只会算长按。
    第一版改成「开机 1.5s 内不认」，expert 指出主循环第一次读按键已经过了 1.6s 以上，那个守卫根本碰不到。
    设置页里 BtnA 还是「Press to Start」，回摇杆页；这一下一直按着的时候不算 M5 键（`a_left_setup`），免得一进摇杆页就让机器人回主屏。
  - 开机时读一次按键寄存器，打进日志；不再扫描全部 126 个地址（卡住时那要 25s）。卡住以后每 2s 才再试一次手柄，界面不会被每次 200ms 的超时拖慢。
  - IMU 页不读手柄（和原来一样），所以那里没有摇杆按下，只有 M5 键。
  - 摇杆页电量那一行是 `Bat:xx% B:n`，`n` 是按键寄存器最近一次读到的原始值（1 松开、0 按下、-1 从没读到过）；
    从没读到过、最后一次读又失败时显示 `B!<错误码>`（`esp_err_t`，比如 263 是超时）。这些都在 `ui/ui_running_screen.c`。
    手柄连续 20 次没应答时，标题从 `StackChan :)` 变成 `JoyC stuck`。不插电脑也能看出手柄的状态（电脑一打开串口就会复位遥控器）。
  - 初始化 I2C 之前只读一下两根线的电平（不驱动），打进日志。
  - 开机直接进摇杆页。`ui_init()` 先把设置页和摇杆页都建好，免得以后切到设置页时边建边用；显示的是摇杆页。
    电源键依次是 IMU 页、设置页、摇杆页。设置不存 NVS，每次开机都是信道 1、ID 0（和原来一样）。
  - 摇杆模式和 IMU 模式都发 11 字节，按键变化时也单独发一包。每一包都按遥控器当前的 id、位置和速度填（`fill_packet()`）。
    原来只发按键的那一包沿用上一次移动时的字节：设置页改过 id 之后 id 是旧的，IMU 模式下还没动过时 speed 是 0。
  - 摇杆读数从中位开始。手柄读不到时，原来的 0 会映射成 yaw 1800、pitch -88，头会被甩到一边。
  - 编译用 ESP-IDF 5.5.4。m5gfx 在 5.5 上要把 `managed_components/m5stack__m5gfx/src/lgfx/v1/platforms/esp32/common.cpp`
    里的 `__has_include(<driver/i2c_master.h>)` 改成 `0`。managed_components 不进仓库，只在本地副本里改。
  - 产物是 `build/StackChan-RemoteControl-ESPNow.bin`，芯片 esp32，烧录地址：bootloader 0x1000、分区表 0x8000、app 0x10000。
- **RemoteLink**（`remote/remote_link.*`）：单例。除了 ESP-NOW 的接收回调（乐鑫 espnow 组件的任务里）和 `attach()`（见下），都只在主循环里用。
  回调做两件事，都在 `_mutex` 里：把解析成功的包放进队列（最多 32 个），记下 M5 键的按下。它读的接收 id 是原子变量，其余状态只有主循环碰。
  - 启动器开机就开始监听，直到听到遥控器：
    - 几乎一直停在 NVS（`stackpet_remote/channel`）存的信道上，没存过就是 1；
    - 每 3s 去另一个信道看 150ms，轮流看，一圈约 38s。
  - 主屏上的第一下往往只有很少几包（长按时算数的只有按下那一包），所以要几乎一直待在遥控器的信道上才听得到。
    最早的版本是每 150ms 换一个信道、13 个轮流转；PM 评审按模型估算，那样单按一次只有 8–39% 能听到。
  - 听到以后锁定在**遥控器的**信道上。乐鑫 espnow 组件发包时把发送方当前的信道写进帧头，收包时又用它覆盖 `rx_ctrl->channel`
    （`components/esp-now/src/espnow/src/espnow.c` 的收发两处），HAL 把这个值交给链路。所以官方遥控器也一样，拿到的就是它自己的信道。
  - 实测遥控器离得近（同信道 -42dBm）时，隔 4 个信道也能收到它的包（那一包是 -76dBm）。最早的版本取的是「机器人当时所在的信道」，
    就这样在信道 5 上锁住、还存了下来，而遥控器其实在信道 1。现在即使是串过来的包，报的也是信道 1。
  - 锁定以后，遥控器的信道和当前不一样就跟过去（比如之前存错了，或者用户在高级页选错了），收到一个串过来的包就能自己纠正。
    跟随最多每 3s 一次：两个都发给所有机器人的遥控器在不同信道上同时被听到时，不会每批包都来回切、写 NVS。
  - 信道 14 来的包（帧头或者包里说的）直接丢掉：机器人只用 1..13，这种遥控器只有离得近、串到 13 上才听得到。
  - 只看发给本机的包。接收 id 存在 NVS（`stackpet_remote/id`，默认 1）：ESPNOW.REMOTE 触摸打开、选成接收端时存下所选的 id，
    启动器和从遥控器打开的应用都按它过滤。
  - 听到的信道有变化时写进 NVS，下次先在这里听。
  - **M5 键**：接收回调里就把它记下来（只看发给本机的包），这样不读链路的应用（FACE、SPIN、FREEZE、RADAR、联网应用）也收得到。
    - 包里有按下次数（第 11 字节）时按次数算：bit 1 按着、次数和上一包不同，就是一次新的按下。没有次数的包（旧固件）取 bit 1 的上升沿。
    - 副本（bit 2）只用来认 M5 键，不放进队列：它的帧头是补发时那个信道的，交给 `update()` 会把找信道、跟信道带偏。
    - `takeHomePress(now)`：有一次按下就返回 true 并清掉，谁先问归谁；按下超过 1s 还没人问就作废，不会留到以后突然回主屏。
      比较用有符号差：按下可能晚于调用方读时钟。
    - `start()` 和 `release()` 都清掉；没在监听时总是 false。`start()` 先把接收 id 从 NVS 读好（回调里用原子变量读它）。
    - 同一次按着期间摇杆再动、多发几包，不会再算一次。按次数算以后，松开那一包丢了也不影响下一次。
  - `release()` 停掉 ESP-NOW 和 Wi-Fi（`Hal::stopEspNow()`），把 Wi-Fi 还给要自己连网的应用。
  - `attach()`：应用自己的 Wi-Fi 连上路由器以后（`Hal::startNetwork()` 的末尾；AI.AGENT 是第一次待命 10s 以后，`hal.cpp` 的小智循环里），
    这次通电用过遥控器的话，就在这个 Wi-Fi 上开 ESP-NOW 接收（`Hal::attachEspNow()`），信道是路由器的，不找、不跟、不存 NVS。
    这里主要收到 M5 键的副本（离得近时也有串过来的正常包），主页指示器照常问 `takeHomePress()`；小智模式的主页指示器也是 warm reboot 回主屏，焦点停在 AI.AGENT 上。
    - AI.AGENT 不是一进入待命就接：待命那一刻唤醒词的模型正在加载，是内部 RAM 最紧的时候（以前的日志最低 12KB）。
      `hal.cpp` 的 `remote_attach_update()` 等第一次待命 10s 以后、内部 RAM 还有 30KB 以上（最大块 4KB 以上）时才接，在 LVGL 锁外面做
      （`espnow_init()` 要拿事件循环的锁）。总量够了才去量最大块（它要关着中断走一遍堆）；不够时打一次 `remote: waiting for internal RAM`，
      之后每 1s 再看；没接上（Wi-Fi 正在重连）隔 5s 再试；这次通电没用过遥控器就不接。
      接上以后打一行内部 RAM（当前、开机以来最低、最大块）和这个任务（stackchan，4KB 栈）剩下的栈。
    - 接上以后在屏幕最上方正中（每种表情的眉眼都在它下面）提示 3 秒「现在可以按 M5 回桌面」（`remote_notice_update()`，在 `lv_layer_top()` 上，持 LVGL 锁），
      删掉时再打一次剩下的栈（`remote notice shown: stack N`，算上了建提示用的栈）。
      字用 `fontCn20()`，它的字形留在映射的资源分区里；小智激活时可能下载新资源、重新映射分区（`Assets::Download()`），启动器建的字库就指着旧地址了。
      所以先用 `ui::fontCn20Current()` 再查一次 `Assets::GetAssetData()`，地址或大小变了就改用内置的 Montserrat 20，提示英文 `Press M5 to go home`。
      重新映射一般还在原来的虚拟地址（IDF 从头找第一个够大的空隙），所以大小也要比；地址、大小都一样而内容不同的分不出来。
    - 内部 RAM 还有一处低谷：第一次对话时小智才建音频处理器和一个 4KB 内部栈的任务（以前没开 ESP-NOW 的日志里那时最低 9KB）。
      所以 espnow 组件的接收任务打了补丁（`patches/esp-now.patch`，`repos.json` 里登记，`fetch_repos.py` 拉下来时打上）：
      `xTaskCreateWithCaps()` 建，4KB 栈放 PSRAM、TCB 在内部。没开加密时这个任务只把包交给接收处理器、回 ack，不碰 flash，和 SoundPlayer、人脸检测的任务一样；
      开了加密它要从 NVS 读密钥（`espnow_get_dec_key()`），栈就不能放 PSRAM，所以 `sec_enable` 时照旧用内部栈（现在没开）。
      PSRAM 不够时退回原来的 `xTaskCreate()`，打一条警告 `No PSRAM for the main task's stack`；任务参数记着是哪一种。
      任务退出时 `vTaskDeleteWithCaps(NULL)` 把栈和 TCB 都释放：启动器那次 init 的也是，不会带进之后打开的应用（第一版不释放，gate 指出它会一直带进 AI.AGENT）。
      IDF 5.5 删自己时要临时建一个清理任务（内部约 1.9KB），建不出来就 abort。任务在两种时候退出：
      - `espnow_deinit()`：只在启动器打开应用之前调（`release()`），那时内部 RAM 有 90KB 以上；
      - 任务一开始建队列就失败（内部 RAM 连两三百字节都没有）：原版这时只是静默收不到包，现在会因为建不出清理任务而重启。实际碰不到。
      上游的限制照旧：`espnow_deinit()` 以后旧任务过一会儿才自己退出，退出时按全局变量删队列，紧接着再 `espnow_init()` 的话新任务的队列会被它删掉。
      固件里没有这样紧挨着的调用（最快的是触摸打开 ESPNOW.REMOTE 以后的角色页，要人去点），以后也别加（`stopEspNow()` 上面写了）。
      这样接上以后常驻内部 RAM 约 1.1–1.5KB（TCB、两个队列、互斥量、事件组这些约 0.86KB，加上 Wi-Fi 库里 `esp_now_init()`、广播 peer 的分配，没测），原来约 5KB。
      上机：主屏的内部 RAM 从 93071 涨到 97163，多出的 4092 字节就是挪走的栈。
    - `attach()` 跑在应用的 Wi-Fi 连上的那个任务里（WEB.REMOTE 是它自己的联网任务，AI.AGENT 是 stackchan 任务），主循环同时可能在问
      `listening()` / `takeHomePress()`：`_listening` 是原子变量，最后才置位；其余状态在接收回调那边由 `_mutex` 管。
    - 认出 M5 按下时记下它是副本还是正常包（`sent on` 几），日志 `[Remote] M5 button: home (its copy on every channel)` 或 `(sent on N)`。
      路由器在 2、遥控器在 1 时，离得近也能直接收到正常包，要确认副本有效，得把遥控器换到离路由器远的信道再试。
  - `settle()`：保持监听的应用从不调 `update()`，找信道的状态机就停在那一刻（可能正在别的信道上看）。打开它们之前先停止寻找，回到存下的信道；
    已经锁在遥控器的信道（或者手动选的）上时不动。
  - `usedSincePowerOn()`：接收回调每收到一个带按键字节、发给本机的包（StackPet 的遥控器；官方遥控器没有 M5 键，不算），
    就在 RTC 内存（`RTC_NOINIT_ATTR`）里写一个 32 位记号。warm reboot（每个应用关闭时都有）不清它，断电以后 RTC 内存是乱的，记号自然不对。
    谁也不清它：这次通电以后用过一次遥控器，之后一直算在用。主机测试里它是普通静态变量，那个测试排在最前面。
    第一版只看「这次开机听到过、或者按 M5 回来的」，中间用触摸回过一次主屏，手点打开的应用里 M5 就不灵了（pm 的探针 F3 / F6）。
  - `appKeepsListening(name)`：哪些应用保持监听（按启动器里的名字）。纯函数，有主机测试。
- **主页指示器**（`apps/common/home_indicator/home_indicator.cpp` 的 `update_home_indicator()`）：每个应用本来每帧都调它。
  StackPet 在这里问 `takeHomePress()`，有就调用应用自己的 `onGoHome`（全都是 `close()`），和触摸点主页键走同一条路；关应用照旧 warm reboot，
  回到主屏时停在刚才那个应用上。没有指示器的那一帧也照样问，按下就作废。
- **启动器**（`apps/app_launcher/app_launcher.cpp` 的 StackPet 段）：
  - `stackpet_remote_start()`（第一帧或第一次触摸打开时，只一次）：按名字记下哪些应用保持监听、哪个是 ESPNOW.REMOTE，
    开始找遥控器，再 `reset(now, true)` 导航（开机前没有在进行的操作，第一包多快到都算）。
  - 听到的包交给 `StickNavigator`：左右调 `LauncherView::step(±1)`，长按调 `open_from_remote(focusedApp())`。
  - `open_from_remote()`：应用保持监听（`appKeepsListening()`）就不断开链路，否则先 `release()`；ESPNOW.REMOTE 还会设 `setDirectReceiver(true)`（跳过角色页）。
  - `open_touched()`（触摸打开）：`usedSincePowerOn()`、而且应用保持监听，就不断开链路；ESPNOW.REMOTE 除外，它过了角色页自己开（角色页期间不该攒包）。
    这次通电还没用过遥控器时照旧 `release()`，没有遥控器的人行为不变。代价是用过遥控器以后，手点打开的这四个应用也一直开着 ESP-NOW，和遥控器打开时一样。
  - `open_app()`：保持监听就 `settle()`，否则 `release()`；然后置 `_app_opened`，这一帧后面和以后都不再处理遥控器
    （触摸打开发生在 `_view->update()` 里，同一帧后面还会跑 `stackpet_remote_update()`）。
  - 按名字查应用都在 `getAppProps()` 返回的临时 vector 还活着的循环里完成。第一版把名字存成 `std::string_view`，循环结束后读的是已经释放的内存（gate / expert 用 ASan 复现）。
  - 主屏上的 M5 键每帧取掉：屏保亮着时叫醒它（屏保取用量的那几秒只收得到 M5 的副本，见「屏保」一节），否则什么也不做。冷启动的启动动画期间不处理摇杆、也不叫醒屏保，包直接丢掉；
    导航保持开机时的 `reset(now, true)`，动画一结束第一包就算（第一版每帧 `reset(now)`，动画结束后 1s 内的长按会被吞掉）。
  - 有包时调 `lv_display_trigger_activity()`：遥控器在用就不进屏保。屏保亮着时这一批包只用来叫醒（再 `reset(now)`），不翻页、不打开。
  - 长按进度条的提示：焦点所在的应用既不保持监听、也不是联网应用（就是 SETUP 和 DANCE，`_touch_home_apps`）时，进度条下面显示「打开后用触摸回桌面」。
  - `open_app()` 先关掉屏保。屏保有自己的屏幕，触摸会先把它关掉，遥控器不会；不关的话，应用会建在屏保的屏幕上。
  - 全仓库只有启动器的 `open_app()` 调用 `openApp()`。
  - AI.AGENT 在同一次启动里接着起小智（不重启）。小智的 `WifiManager` 允许默认事件循环已经存在，和上机验证过的 WEB.REMOTE 是同一套代码。
- **启动器界面**（`apps/app_launcher/view/view.{h,cpp}` 末尾的 StackPet 段）：
  - `step(±1)` 用 LVGL 的滚动动画移一个图标（和触摸点两边箭头一样，LVGL 每次滚动最多 400ms）。
    起点是正在滚的动画的终点（`lv_obj_get_scroll_end()`：这一步之前那一步的、甩完后吸附的、点箭头的），没有动画就是当前位置，取最近的图标。
    所以动画还没走完时再来一步，从那个终点接着算，不会丢步；手指拖着的时候没有动画，就从手指下面居中的图标算。
    动画用 `lv_obj_scroll_to_x()` 起：它先删掉正在跑的那个。上一步还没走出 1px 时反向的一步，差值是 0，
    要是用 `scrollBy()` 就什么都不做、让上一步接着跑完（gate 和 expert 都用模型复现了「差一格」）；`scroll_to_x()` 删掉以后停在原处，正好。
  - 一直很快地往同一个方向翻，面板一直在滚，官方回绕要等它停下才执行，会滚出 5 套副本（expert 的模型：每步不到 400ms，三十来步）。
    所以 `step()` 发现终点出了中间那几套（1.5–3.5 套），先不带动画地跳一整套（图标一样，看不出来；顺带结束正在滚的动画），再从那里滚过去。
  - `focusedApp()`：同样取滚动终点上的图标；动画中是终点，否则是居中的那个。
  - 长按进度条：图标（到 y=75）和页码点（从 y=91）之间一条 120x6 的条，深色底（黑色、不透明度 90）上填白色，在每个应用的底色上都看得清。
    按住时从左往右填满，松开或打开时隐藏。
  - 提示：页码点上面盖一块深色圆角底的 `fontCn20()` 字「打开后用触摸回桌面」，只在长按、而且启动器说这个应用遥控器管不了时显示。
- **导航**（`remote/stick_navigator.*`，纯 C++，有主机测试）：
  - 只看 yaw：遥控器把摇杆往右映射成负的 yaw（`map(x, X_MIN, X_MAX, 1280, -1280)`），它自己屏幕上的点却随 x 往右走，所以往右是下一个（`kNextYaw = -1`）。
    这是从代码推出来的；2026-10-13 第一次上机时用户正常翻页、打开了 FREEZE / SPIN / RADAR，没说方向反了，但也还没专门确认。
    反了就把头文件里的 `kNextYaw` 改成 `+1`，主机测试也用这个常数，不用跟着改。
  - 越过 ±0.5 走一步；要回到 0.3 以内才认下一步。按着摇杆时不走。在外面松开按键算不算那一次推：
    - 推出去走了一步、在外面误按一下再在外面松开：不再走（第一版会多走一步，gate 用探针复现）；
    - 在中间开始长按、按着推出去（作废）再在外面松开：不走；
    - `reset()` 之后 1s 内第一包就是推在外面、按着（比如用遥控器叫醒屏保），在外面松开：不走，也不连走（要先回中）；
    - 快速甩出去、到外面的第一包就带着误按，再在外面松开：这才是那一次推，走一步；在外面按着 0.3s 再松也一样；
    - 在外面开始按、按住 0.4s 以上（`openPressMs / 2`）再在外面松开：当作偏离中间的长按没成功，不走。
    前两种和第三种按下记作 `_press_steady`（在中间开始的、或者之前就按着的）；后两种看按下持续了多久（`long_off`）。
  - 推住不放：0.6s 后、之后每 0.45s 再走一步，最多再走 10 步（推一次最多 11 格，12 个应用差一格转满一圈），免得回中那包丢了就一直转下去。
    0.45s 比一步的滚动（400ms）长，每一格都会停稳，松手时不容易多走一格。中途松到 0.3–0.5 之间、或者按一下摇杆，就停，要回中再推。
  - 长按：按下那一包里摇杆偏离中间不超过 0.5，按住 0.8s 打开，只打开一次；途中推离中间或者松开都作废，松开再按才有下一次。
  - 连走和长按的计时只在 `tick()` 里做，启动器每帧先把这一批包都 `update()` 完再 `tick()`：主循环卡了一下、按着和松开的包一起到时，先算松开，不会误开。
  - `reset(now)` 之后 1s 内收到的第一包，不管是按着还是推着，都当作之前就那样，要先松开、回中。超过 1s 才收到的第一包照常算
    （遥控器只在变化时发包，安静一段时间以后，第一包就是这一下）。`reset(now, true)` 用在开机：开机前不会有操作在进行，第一包多快到都算
    （第一版开机也套 1s，回到主屏马上长按会被吞掉）。从没调用过 `reset()` 时一律当作之前就那样。
- **ESPNOW.REMOTE**（`apps/app_espnow_ctrl`）：数据改走 RemoteLink，原来自己挂的 `onEspNowData` 处理器去掉了。
  - 从遥控器打开时直接当接收端，信道用链路当前的，说「遥控器连上啦～」并放 Success。
    触摸打开照旧走角色页、高级页，然后锁定到选的信道。
  - 摇杆照旧控制头和激光。手势动作进行期间头归动作管，激光照常跟着包走。
  - 气泡用 `ui::fontCn20()`。Montserrat 没有汉字，第一版的气泡是空的。
  - M5 键回主屏走主页指示器。`onClose`：先 `_actions.stop()`（正在转圈的话，把 yaw 舵机切回位置模式），再 `SoundPlayer::shutdown()`。
- **RADAR**：
  - 打开时链路还开着（`remote_session`：从遥控器打开，或者这次通电用过遥控器以后触摸打开）：前 15s 状态栏每隔 2s 换成「按 M5 键回桌面」，错误提示优先显示。
    锁定、生气这些阶段雷达屏藏着，这条提示看不到。
  - M5 键和触摸回主屏一样 `close()`；`onClose` 第一件事就是停转，警笛也一起停。
  - 这次通电还没用过遥控器时，触摸打开的 RADAR 不碰 Wi-Fi，也不认遥控器。
- **手势**（`remote/stick_gestures.*`，纯 C++，有主机测试）：输入是发给本机的每个包，坐标 x = yaw/1280、y = (pitch-450)/450。
  - 摇头 Shake：x 越过 ±0.5，1.5s 内换边 3 次；只看越线的那几包，它们的 |y| 要小于 0.5（沿边转圈时不会被当成摇头）。点头 Nod 一样，只是换成 y 方向。
  - 转圈 Circle：沿边缘转（半径 ≥0.6 开始，小于 0.4 作废重来），2.5s 内累计 330°，方向 ±1。
  - 双击 DoubleClick：B 键 600ms 内按两次（激光开关翻转两次）。第一包里的激光状态只是起点。
  - 认出一个摇杆手势后，800ms 内不认新的摇杆手势；双击不受影响。
  - 摇杆按下和 M5 键不是手势（主屏和主页指示器的事）。
- **动作**（`remote/remote_actions.*`，在主循环里执行，持有 LVGL 锁）：
  - 摇头：yaw ±18°，每 160ms 一帧，速度 900，配「不要～」、Doubt 表情和 Boop 音效。
  - 点头：pitch 往中间方向点两次，每次 15°，配「好的！」、Happy 和 Success。
  - 摇头和点头都以摇杆**当时**的位置为中心，扳完一松手就是正前方。
  - 转圈：先把 yaw 用最硬的弹簧移到它此刻的位置（`moveWithSpeed(当前角度, 1000)`，gate 的模型里停稳快约 40%），等头停稳，然后用轮式模式以 600 转一整圈。
    认出转圈之前头一直跟着摇杆，弹簧要 0.9–1.4s 才停稳（主循环 50–33Hz）；还在动时写一次位置就会退出轮式模式，
    所以 `isMoving()` 为真时绝不 `rotate()`，最多等 3s，等不到就放弃这一圈，说「转不动～」（不晕）。第一版只等 600ms，gate 模拟出几乎转不起来。
  - 转圈结束、被打断都照 RADAR 的 `leave_spin()` 退出轮式模式：先 `rotate(0)` 滑行到 60°/s 以下（最多 0.3s），停在电位器盲区里就以 300
    朝起点方向转出来（最多 1.5s），再切回位置模式。高速时直接切位置模式等于反接制动。关应用时 `stop()` 阻塞着做完这几步。
    角度用 RADAR 的 `SpinTracker` 跟踪，转到 3600 减去滑行量（约 15°）就刹车，超过 3s 也刹；停下后显示 Dizzy 装饰 1.6s，配「晕了……」。
  - 双击：依次换 10 种表情。之后其他动作结束时，也都回到这个表情。动作进行中双击也算：表情等这个动作的台词结束后再换。
  - 每个动作结束后，头回到摇杆当时的位置（动作期间松手的话就是正中），台词 1.6s 后消失。
  - `announce()`：说一句话、换一个表情，1.6s 后恢复原来的表情。
- **HAL**（`hal/hal_espnow.cpp`）：
  - `startEspNow()` 可以重复调用，已经启动时只换信道。
  - `attachEspNow()`：Wi-Fi 已经由应用连上路由器（`esp_wifi_sta_get_ap_info()` 成功）时，只做 `espnow_init()` 和注册接收处理器，不碰 Wi-Fi 本身。
    事件队列只要 8 项（默认 32：这里主要来 M5 的副本，离得近时也有串过来的正常包），`send_max_timeout = 0`：接收回调跑在 Wi-Fi 任务里，
    默认会等队列有空位最多 3s，那会卡住应用自己的联网；现在队列满了就丢这一包（M5 的副本有三轮）。
    Wi-Fi 驱动的日志级别不动（不切信道，不会刷屏，断线原因之类的日志要留着）。
    乐鑫 espnow 组件在 STA 已连接时把 `g_set_channel_flag` 置成 false，之后任何 Wi-Fi 事件又会把它改回 true；机器人这边从不发包，所以无所谓。
    接着 `esp_wifi_set_ps(WIFI_PS_NONE)`：省电时射频只在路由器的信标附近醒着，
    遥控器的广播收不到（IDF 的文档说 ESP-NOW 默认的接收窗口会让射频一直开着，这里不依赖它）。
    小智空闲时会要 LOW_POWER，`hal/board/stackchan.cc` 的 `SetPowerSaveLevel()` 在 `hal_bridge::espnow_keeps_radio_awake()`（原子变量）为真时改成 PERFORMANCE；
    AVATAR 推流停下时直接要 BALANCED（`hal_ws_avatar.cpp`），这时也跳过。
    `stopEspNow()` 对这种情况只 `espnow_deinit()`，不停 Wi-Fi，并恢复 "espnow" 的日志级别；`setEspNowChannel()` 不做事。
    espnow 组件会多一个任务（打了补丁，4KB 栈在 PSRAM，内部常驻约 1.1–1.5KB，见 RemoteLink 的 `attach()`）。
    队列满了丢包时 espnow 组件会在 Wi-Fi 任务里打一条警告（串口每条要占几 ms），所以接上时把 "espnow" 的日志级别降到 ERROR。
    `espnow_init()` 真正会返回错误的只有建事件组、互斥量失败（内部 RAM 用完了），其余失败都在组件里 `ESP_ERROR_CHECK`，直接重启。
    返回错误时组件留着它的配置，下次调用什么都不建就说成功（gate 用仿真复现了「假接上」）。所以失败一次以后这次开机不再试（`s_espnow_init_failed`）：
    - `attachEspNow()` 打一条警告，以后都返回 false（AI.AGENT 的 `remote_attach_update()` 照样每 5s 问一次，不再打日志）；
    - `startEspNow()` 打一条警告，马上把刚起的 Wi-Fi 拆掉（`_wifi_deinit()`，和 `stopEspNow()` 拆的一样），应用自己的 Wi-Fi 才起得来；链路这次开机什么也收不到。
  - `stopEspNow()` 依次执行：`espnow_deinit`、`esp_wifi_stop` / `esp_wifi_deinit`、注销自己注册的两个事件处理器、`esp_netif_destroy_default_wifi`。
    默认事件循环保留不删；`_wifi_init` 遇到它已经存在（`ESP_ERR_INVALID_STATE`）时不报错。
  - 新增 `setEspNowChannel` / `getEspNowChannel` / `getEspNowRxChannel`，换信道不打日志。
  - 换信道失败（比如以后设了只到 11 的国家码）只打一条警告、留在原信道，不再 `ESP_ERROR_CHECK` 把每次开机都变成重启。
  - 每收到一个包打一行的 `espnow_recv`（上游就有）降成 debug：遥控器在用时约 20 包/秒，现在主屏、遥控器打开的应用、
    还有用过遥控器以后手点打开的那四个应用都在听。
  - `startEspNow()` 开着 ESP-NOW 时，把 Wi-Fi 驱动的日志级别降到 WARN（每换一次信道它会打两行 `ic_enable_sniffer` / `ic_disable_sniffer`），`stopEspNow()` 时恢复。
    `attachEspNow()` 不降 Wi-Fi 驱动的（只降 "espnow" 的，见上）。
  - 上机验证过：拆掉之后，REMOTE 应用能正常连上家里的 Wi-Fi。
- **实测**（2026-10-12，链路开着、锁定信道，也就是真实路径下的 RADAR）：
  - 特征图那 27KB 拿不到内部块，按设计退回 PSRAM。加载后 D/IRAM 剩 28KB（最大块 14KB），内部 RAM 剩 92KB。
  - 没人时检测 9.3–11 次/秒（MSR 86–93ms），锁定人脸时 7.7 次/秒（MSR 88ms + MNP 18ms）。不开 Wi-Fi 时分别是 10.5 和 7.4 次/秒。
  - 一直换信道的话（最早的版本），每次推理多花约 10ms（约 100ms，8.4–9.3 次/秒）。
- **测试钩子**：`-DSTACKPET_AUTOOPEN=<启动器序号> -DSTACKPET_AUTOOPEN_REMOTE=1` 模拟从遥控器打开那个应用：
  链路停在存下的信道上（`settle()`），按 `appKeepsListening()` 决定开着还是关掉，ESPNOW.REMOTE 跳过角色页。用完记得改回 `=0`。
- **已知限制**：
  - 官方遥控器（8 字节）没有摇杆按下和 M5 键：主屏能左右翻，但打不开应用，也回不了主屏；要遥控就用触摸打开 ESPNOW.REMOTE、选接收端。
  - SETUP 和 DANCE 里遥控器基本不起作用，要用触摸回主屏；联网应用要等连上 Wi-Fi 才能按 M5 回，而且只有 M5 键（见「怎么用」第 4 条）。
  - 联网应用里为了收遥控器，Wi-Fi 一直不省电，比原来耗电（没有实测）。AI.AGENT 的内部 RAM 本来就紧，多了 espnow 的任务以后的余量还没上机量过。
  - 路由器的信道要在 1–11 里：遥控器用默认国家码 "01"，补发只到 11 信道。路由器在 12、13 信道时，联网应用里按 M5 不起作用。
  - 遥控器的设置页能选信道 12–14，但遥控器用默认国家码 "01"，只有 1–11：选 12–14 时 `wifi_espnow_reinit()` 里的 `ESP_ERROR_CHECK(esp_wifi_set_channel())`
    会让遥控器重启（上游代码就这样），别选。
  - 遥控器在设置页换了信道以后，还在找的机器人要等轮到那个信道（最多约 38s）、而且遥控器正好在发包时才找得到；也可以在 ESPNOW.REMOTE 的高级页手动选。
    主屏已经锁定在遥控器原来的信道上时不再找：只有离得近、串过来的包能让它跟过去，否则要等下一次回主屏（warm reboot 后重新找）。
  - 回主屏要 warm reboot，约 3–4 秒，期间画面不动；回来以后链路要重新找遥控器（先在存下的信道上听）。
  - 刷机、或者电脑打开 StickC-Plus 的串口（macOS 一打开就会复位它，在 pyserial 里先把 DTR / RTS 设成 False 也一样），都可能让手柄的 STM32 卡住：
    手柄的 SDA 就是 ESP32 的 GPIO0，也是启动用的 strapping 脚，USB 芯片下载 / 复位时会拉它，总线上就是一串乱码。
    卡住时手柄一直拉住 SCL（日志 `I2C lines: SDA 1 SCL 0`），摇杆和按键都不动，屏幕标题显示 `JoyC stuck`。
    手柄吃的是 StickC-Plus 的 3V3，ESP32 复位不断电，打时钟也救不回来，必须整机断电：拔掉 USB，按住电源键约 4 秒关机，再开机。
    平时用电源键开关机不会这样，所以开发时刷完机、看完日志都要整机断电一次再测。
    开机时用 GPIO 打 9 个时钟加 STOP 来救的办法试过：反而让刚上电的手柄一开机就卡住，已经去掉，现在开机只读一下线电平。
  - SETUP 里开了「开机启动 AI」时，冷启动直接进小智、不经过主屏，所以不监听遥控器。
  - 主屏上 Wi-Fi 一直开着收包，比原来耗电（没有实测）。这次通电用过遥控器以后，FACE、SPIN、FREEZE、RADAR 不管怎么打开都开着；
    FREEZE 还要和摄像头抢内部 RAM：2026-10-13 从遥控器打开过一次（只开了 1.5s），摄像头正常启动、没报错，还没完整玩一局。
  - 手势阈值是按主机模拟定的，真人拿遥控器的验证还没做完。
  - 两个都发给所有机器人（id 0）的遥控器同时在用时，机器人最多每 3s 跟着换一次信道。
  - IMU 页里没有摇杆按下（遥控器在 IMU 页不读手柄），所以在 IMU 页打不开应用；M5 键照常。
    IMU 页的 yaw 来自左右倾斜：在主屏上，遥控器倾斜超过约 43° 就会翻页，倾着不动还会连翻。误按电源键进了 IMU 页，会看到应用自己在转。
  - 遥控器上看不出电源键现在管换页：屏幕上只有 Montserrat 14 一种字体，摇杆页也排满了，没加提示。
  - 开机后先用手点开 FACE、AI.AGENT 这些应用、还没碰过遥控器时，里面按 M5 没反应（链路是关的）。回主屏动一下摇杆就好了，一直到断电。
    机器人上看不出这次通电有没有用过遥控器。
  - 2026-10-13 改成现在的按键逻辑（主屏左右选、长按打开、M5 键回主屏），上机情况：
    - 第一版（12:05–12:11）：翻页、长按打开 FREEZE / SPIN / RADAR，在三个应用里按 M5 回主屏，都正常。
    - 第三轮修改版（12:31–12:42）：遥控器打开 AVATAR（链路关掉）、触摸回主屏；遥控器打开 RADAR 两次，都按 M5 回来；遥控器打开 AI.AGENT。
    - 第四、五轮的修改上机时只用了正常操作（13:40 前后遥控器打开 RADAR、AI.AGENT 又回来），专门的场景（误按、屏保、连翻）还没逐个试。
    - 第六轮（联网应用里按 M5）：14:19 遥控器打开 AI.AGENT，待命后按 M5 回了主屏，日志看不出是副本还是直接收到的；在 AI.AGENT 里久待、对话以后的内部 RAM 还没量。
      同一时间遥控器信道恢复的修复见效：主屏上按过 M5 以后收到的包仍是 `sent on 1`。
    - 第七到九轮（AI.AGENT 待命后延后接、espnow 任务栈放 PSRAM、提示、诊断日志）：15:56 刷了第九轮的版本（主屏内部 RAM 97163，打补丁前 93071）。
      16:18 用户自己试了一遍：遥控器打开一个保持监听的应用、按 M5 回主屏；遥控器打开 AI.AGENT，打开后约 20s 接上
      （`remote attached: internal RAM free 29939 (lowest since boot 8019, largest block 7680), stack 1120`），提示出来；
      对话两次，SystemInfo 报 `minimal sram: 2623`（对话中内部 RAM 最低约 2.6KB，以前没开 ESP-NOW 的老版本是约 9KB，还没分清多少是 ESP-NOW 的）；
      按 M5 回主屏成功（`sent on 1`：路由器在 2、遥控器在 1，串过来的正常包，不是副本）。
      之后那次 warm reboot 停在引导程序交给应用的地方（`Disabling RNG early entropy source` 之后没有输出，esptool 也连不上），断电重开后正常，原因没查到，以前十几次 warm reboot 都正常。
    - 第十轮（第九轮的 NIT：init 失败只试一次、补丁在加密时不用 PSRAM 栈、字库也比大小、日志）19:51 刷了，开机正常（主屏 97147），没有交互。

### 屏保：时钟 + Claude 用量（usage）
用户原话（2026-10-13 晚）：「屏保改成时钟+claude用量吧,然后颜色看情况切换」；看了第一版（浅色纸面）以后：「这配色也太丑了,而且达不到屏保的效果吧,好好设计几种风格」，
又说「插着电源的时候不要进入省电模式」。主屏 30s 没操作出来的屏保（原来是官方的 DVD 弹跳小脸）换成大时钟和 Claude 的两个额度，有四种风格。
用量来自用户 Mac 上自己写的菜单栏 App ClaudeUsageBar 的局域网服务，机器人隔一会儿去取一次。

**四种风格**（`usage/style_*.cpp`，`usage/clock_screen.{h,cpp}` 一次放一种；`tools/usage_preview/render.sh` 在 Mac 上按设备的 LVGL 渲染出每种风格的每个场景）
- 底色四种都是纯黑 `0x000000`：用户说「黑底感觉不够黑 泛白光」，原来暖黑是深灰 `0x141312`、睡觉 `0x0E0D10`、终端 `0x020803`，
  液晶屏上整屏的深灰被背光照着就是一层泛白的光（`0x141312` 比纯黑亮好几倍）。风格靠底上的颜色区分，主机测试要求底色是纯黑。
  纯黑剩下的那点亮是液晶本身漏的背光，背光越亮越明显（插着电时屏保不调暗，见下面「背光」）。
- **暖黑**（`claude`，默认）：纯黑底、奶油色 96px 大时钟（Space Grotesk Bold）、Claude 的陶土色。左上角八道光芒的小图标：Claude 工作时慢慢转，等你确认时变琥珀色并闪，做完变绿。
  时钟下面一条细线每分钟从左走到右，颜色是最接近上限的那个额度的颜色；底下两栏「5 小时」「本周」：百分比、进度条、重置时间（「4:12 后」「周四 15:00」）。
- **表盘**（`rings`）：两圈圆环围着时钟，外圈每周、内圈 5 小时（变化时有动画），一个小光点每分钟沿外圈转一圈（等你确认时变琥珀色）；右边一栏是两个额度的百分比和重置时间。
- **终端**（`terminal`）：绿色磷光终端：扫描线盖满全屏（1x3 的图案平铺），一条亮带每 7 秒从上往下扫一遍，状态后面一个闪烁的光标；
  `5-HOUR [######----]   9% IN 4:00` 这样的等宽字行（Share Tech Mono），额度高了那一行变琥珀或红。
- **睡觉**（`sleepy`）：StackChan 闭着眼在时钟下面睡觉，呼吸一样上下起伏，「Z」从右眼旁边往上飘；Claude 等你确认时它睁开眼、头上冒出闪烁的「!」，做完了闭眼微笑。
- **换风格**：屏保亮着时推遥控器摇杆左右（左上一个、右下一个），屏保不退；按一下摇杆或 M5 键回主屏，摸屏幕也回主屏。选的风格存在 NVS（`stackpet_usage/style`）。
  也可以 `tools/stackpet_usage_setup.py --style rings`。屏保上摇杆推一下不再叫醒屏幕（以前遥控器的第一下只是叫醒）。

**按情况变色**（`usage/usage_look.{h,cpp}`，纯函数，有主机测试）
- 额度：70% 以下是风格自己的颜色（暖黑陶土色、表盘天蓝、终端绿、睡觉淡紫），70–89% 琥珀色，90% 以上红色；过了重置时间算 0%、「已重置」。
- Claude 的状态：工作中 / 等你确认（琥珀色、闪）/ 已完成（绿）/ 空闲。几个会话在等：「2 个等你确认」。
- 数据旧了（最后一次取到超过 5 分钟）：额度都变暗，状态不显示，换成原因和多久以前：「连不上 Mac · 12 分钟前」（终端：`LINK MAC OFFLINE 12M AGO`）；
  一次都没取到时额度的位置只写原因：「用量未配置」「正在取用量...」。偶尔一次没取到、上一次还不到 5 分钟时什么都不说。
- 夜里（22:00–6:59）什么都不闪（光标、边框、感叹号都停在亮的那一下），睡觉风格的呼吸慢一些。
- 整个画面一直在缓慢漂移（两个周期 173s、131s 的正弦，每种风格按排版定幅度 ±3–14px，每 0.25s 最多挪 1px）：不会一整天点着同一批像素，看起来也是活的。

**背光**：插着电时用用户设的亮度，不省电（白天夜里都不调暗）；用电池时屏保期间最多 30%，夜里最多 8%（用户设得更低就不动），回主屏恢复。
是否插着电每 2s 问一次 PMIC（`Hal::hasExternalPower()`：不在放电就算插着，读不到也算插着）。

**数据从哪来**：ClaudeUsageBar 的 `GET http://<Mac>:8787/api/v1/snapshot`，`Authorization: Bearer <令牌>`（App「配对手机」窗口里的那个）。
回的 JSON 约 1.2KB，只读 `claude.session` / `claude.weekly` 的 `pct`、`resetsAt`，`claude.dataAgeSec`、`claude.model`，`agent.state`、`agent.waiting` 和 `ts`
（`usage/usage_snapshot.{h,cpp}`：ArduinoJson 带过滤；HTTP 响应按 Content-Length 取正文；都是纯函数，有主机测试）。
Mac 的 `ts` 和机器人的时钟差 2s 以上时，机器人按它对时并写回 RTC（主屏不跑 SNTP）。

**怎么配置**（一次就行，令牌换了再做一次）：机器人插在 Mac 上、停在主屏时
```bash
cd firmware && uv run --with pyserial python3 tools/stackpet_usage_setup.py   # --show 看机器人记的，--clear 清掉，--style <风格>
```
脚本从 ClaudeUsageBar 的设置里读令牌（`defaults read local.claude-usage-bar http.token`），地址默认用 en0 的 IP（`--host` 可改），
经 USB 发一行 `stackpet usage <地址> <端口> <令牌>` 给机器人：令牌不打印、不写文件。机器人存进 NVS（`stackpet_usage` 的 host / port / token），
日志只回 `set up: the Mac at 192.168.31.90:8787 (a token of 24 characters)`。
- 接着发 `stackpet usage tz <Mac 的时区>`（`/etc/localtime` 末尾的 POSIX 串，比如 `CST-8`）：机器人的时区还是默认的 `GMT0`（SETUP 里从没选过）时就用它
  （`Hal::setTimezone()`，所有应用都按它）；SETUP 里选过的不动，日志说一声。第一版屏保在用户家里显示的是 UTC：机器人从没设过时区。
- USB 口在这块板子上是副控制台（主控制台是 UART0），没有输入。所以主屏每帧直接读 USB-Serial/JTAG 外设的接收 FIFO（`usb_serial_jtag_ll_read_rxfifo`，不装驱动）；
  FIFO 满了主机会等，不丢字节。只有主屏读：应用里发的行会等回到主屏才读到。脚本打开串口时不改线路状态，机器人不会复位。
- 行的格式和检查在 `usage/usage_config.{h,cpp}`（纯函数，有主机测试）：地址只许字母数字点横线，端口 1–65535，令牌 8–128 个 bearer 里能用的字符，
  风格是那四个名字之一，时区是 POSIX TZ 里的字符、3–63 个；错的行只回一句用法，200 个字符以上的行整行丢掉。
- Mac 的 IP 换了要重新跑一次（机器人没有 mDNS；也可以用路由器能解析的主机名）。

**怎么取**（`usage/usage_feed.{h,cpp}`，主循环里）：屏保一出来就取一次（上一次不到 30s 就不取），之后每 60s 一次；屏保没了就不取。
1. 遥控器链路 `pause()`：不再去别的信道看、不跟信道（正在别的信道上看的话，回到存下的信道）；包照样收。
2. 建一个任务（6KB 内部栈，每次取完就删：主屏打开应用不重启，不能留东西给应用），里面：
   - `Hal::joinRouter()`：用机器人存的 Wi-Fi（SETUP 配的，`SsidManager` 的第一个），在主屏 ESP-NOW 的那个 STA 接口上连路由器，最多等 8s。
     连着的这段时间 `setEspNowChannel()` 不做事。配置只放 RAM（`WIFI_STORAGE_RAM`：默认会写 NVS，这里一分钟一次）；省电关掉（`WIFI_PS_NONE`）；20MHz。
     上次路由器的信道先看（`FAST_SCAN`），不锁某一个 AP（家里是 mesh，锁着子节点时常连不上）。
     连上以后把这个网口的 MTU 设成 576：Mac 回的段最多 536 字节（见下面「踩过的坑」）。
   - lwIP socket 发 GET：一个连接最多等 10s（SYN 由 lwIP 自己在 1.5s、再 3s 后重发；不再每秒换一个新连接，原因见「踩过的坑」），
     收 5s 超时，每 0.3s 看一次要不要停；读到 Mac 关连接为止，最多 16KB；请求串用完就清零。
   - `Hal::leaveRouter()`：断开，等断开事件（最多 1s），回到链路的信道。
3. 主循环看到任务做完：链路 `resume()`（从现在起再去别的信道看），收下结果。
- 屏保被叫醒时，正在取的这一次让它尽快停（不要结果，早点回到遥控器）；打开应用之前 `UsageFeed::stop()` 让它停下并等它结束（最多约 1.5s），Wi-Fi 才交给应用。
- 主屏的 `_wifi_init()` 先 `esp_netif_init()`（TCP/IP），A-MPDU 接收关掉（`ampdu_rx_enable = 0`）。

**踩过的坑**（2026-10-13 晚上机）
- 主屏只用 ESP-NOW，原来没初始化 TCP/IP：第一次连上路由器，第一个 IP 包就断言失败重启（`tcpip_inpkt ... Invalid mbox`）。
  现在 `_wifi_init()` 里初始化；TCP/IP 的任务一旦起来就一直在，主屏开机后内部 RAM 少约 4.4KB（101011 → 96607），之后打开的应用也带着它（联网的应用本来就要起它）。
- 取的成功率很低（20:36–21:08 约三成）：Mac 那边能看到连接收到了请求、也回了，但 1.4KB 的回答一直没被确认（`netstat` 里 `FIN_WAIT_1`、发送队列 1390 字节）；
  握手这些小包两边都通。依次试了关省电、20MHz、关 A-MPDU 接收，都没有根本改善；成功的那几次 HTTP 要 2–4s，像是靠 Mac 的重传才进来。
  当时就让 Mac 只发 536 字节的段，收的超时也放宽到 5s。那时以为是大帧收不进来；按下一条查到的，更可能是机器人回的确认被自己扣住了，
  536 字节和关 A-MPDU 接收先留着，要不要还没验证。
- 后来查清（22:00–23:00，临时诊断：UDP 回显探测、混杂模式抓空口、驱动计数）：问题在机器人自己这边，连上路由器以后它把要发的包扣住 0.2–3s 再成批发出，
  有的在队列里过期丢掉（Mac 上 7 个 UDP 包同一毫秒到）。空口是干净的：噪底 -96dBm、没有长时间占信道的帧、路由器的信标一直收得到（射频没离开信道）；
  只用 802.11b、关掉 A-MPDU 发送都一样，所以不是速率、也不是聚合。主节点比子节点更常见，子节点也有。根因还没定。
- 原来连 Mac 是「每秒一个新连接、关掉上一个」，被扣住的 SYN 晚几秒才发出去，Mac 回的 SYN-ACK 到的时候那个连接已经关了：
  抓包里看到 Mac 的 6 个 SYN-ACK 一起回来，8 次全算失败。改成一个连接等 10s，让 lwIP 自己重发。
- 每次取的时候遥控器收不到摇杆的包（射频在路由器的信道上），见下。

**对遥控器的影响**：取的这段时间（通常 3–8s，最长约 25s：连路由器 8s + 连接 10s + 收 5s）射频在路由器的信道上，遥控器在别的信道时摇杆的包收不到（离得近串过来的除外），M5 键的副本照样收得到。
屏保上推摇杆换风格时正好碰上就没反应，再推一下；主屏上按 M5 键也会叫醒屏保（以前主屏上的 M5 什么都不做）。

**已知限制**
- Mac 要开着、ClaudeUsageBar 要在跑，机器人和 Mac 在同一个局域网；Mac 睡了就是「连不上 Mac」。
- 只有主屏的屏保显示用量，应用里没有。
- 令牌存在机器人的 NVS 里（flash 没加密），能拿到机器人的人能读出来；它只能读 ClaudeUsageBar 的数据，换令牌在 App 的「配对手机」窗口。
- 夜里的时间段（22:00–6:59）、阈值（70% / 90%）、取的间隔（60s）、电池时的亮度都写死在代码里。
- 每分钟连一次路由器，比只开 ESP-NOW 耗电（没有实测）。
- 字体：数字是 Space Grotesk Bold 和 Share Tech Mono（都是 SIL OFL 1.1，`tools/gen_usage_fonts.sh` 生成，文件头带各自的版权声明；生成的字体不用原名：Share Tech Mono 保留了名字）。
  中文都是 `ui::fontCn20()`（资源分区的普惠 20px）；睡觉风格的「Z」是内置的 Montserrat。

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
- **usage**（`stackpet_usage_test`，`usage_snapshot.cpp` / `usage_look.cpp` / `usage_config.cpp`）：
  - 快照：ClaudeUsageBar 真实的形状读对；null、缺字段、类型不对读成「不知道」；超范围的限幅、小数四舍五入；状态四种和不认识的；
    型号只留可打印 ASCII、最多 24 个字符；不是 JSON、没有 `claude`、截断的都是 false 且不动原来的值。
  - HTTP：按 Content-Length 取正文（大小写都认、比收到的短就截）、没有就取剩下的；正文不够、长度不是数字、没有头结尾、不是 HTTP/1.x、状态码不是数字都是 false。
  - 等级 70 / 90 的边界；过了重置时间算 0%；两个额度取最高；夜里 22:00–6:59；白天夜里两套颜色不同、每个颜色都不和底色一样；
    时钟只在琥珀 / 红的时候变色；夜里背光只往下调。
  - 文字：时钟、日期（周日到周六）、没对时；四种状态和几个会话在等；重置时间每一档（含取整、刚过的、六天以上）；
    旧数据的原因和多久以前（分钟、小时），还新的时候什么都不说，未配置 / 没 Wi-Fi 一直说。
  - 风格：四个名字、解析（大小写、不认识的不动原值）、左右轮换回到原处；每种风格的调色板都是暗底（底色亮度 < 24、其余都更亮、
    文字 > 次要 > 暗淡）、三个等级颜色不同、风格之间底色或主色不同。
  - 屏保亮度：插着电时白天夜里都是用户设的；电池时白天 30、夜里 8，用户设得更低就不动。
  - 漂移：几种幅度下都在 ±幅度里、每 0.25s 最多 1px、几分钟内走到四边，同一时刻同一位置，X / Y 周期不同。
  - 终端的文字：英文日期 / 状态 / 重置时间、`[######----]` 的取整、百分比列宽 4；只有原因的说明、英文说明。
  - 配置行：设置、多余的空格和 `\r`、清除、查看、风格（四个名字）、时区（`CST-8`、带夏令时规则的、`<+0545>-5:45`）；各种坏行（端口 0 / 65536 / 非数字 / 太长、令牌太短 / 有引号、多一个词、地址有下划线或开头是点横线）都是 Bad 且不动原值；
    坏的风格名、时区里有别的字符或空格也是 Bad；别的行是 None。
  - 没有主机测试的：`clock_screen.cpp` 和四种风格（LVGL：`tools/usage_preview` 渲染出图来看）、`usage_feed.cpp` 和 HAL 的 `joinRouter()` / `leaveRouter()`（Wi-Fi、lwIP、FreeRTOS：上机日志
    `[Usage] snapshot in N ms (WiFi M ms): ...` / `fetch failed after ...` / `set up: ...`）、`tools/stackpet_usage_setup.py`。
- **remote**（`stackpet_remote_test`）：
  - 包解析：官方 8 字节、加按键的 9 字节、再加信道的 10 字节、再加 M5 次数的 11 字节、目标 id；按键里的摇杆、M5 键、副本三位；
    超出标定的 yaw / pitch 限幅回范围；不合法的长度（7、12）/ 数值 / 按键位（0x08、0x80）/ 信道都拒绝；摇杆偏移量。
  - 摇头 / 点头：快速扳 4 下各认一次，有冷却；慢慢扳、幅度不够都不算。正常操控（扳到底停住、换边、斜着回中）不会误认。
  - 转圈：两个方向都认；转 3/4 圈、半路回中心、转得太慢都不算。
  - 按键：B 双击，两次间隔 1s 不算；冷却期里的双击照常；第一个包里的激光状态不算双击，`reset()` 之后也一样；
    摇杆按下和 M5 键不产生手势。时钟回绕、30 天没包之后照常；转一圈半只算一个转圈；沿对角线来回扳既不是摇头也不是点头。
  - 导航（`StickNavigator`，方向都按 `kNextYaw` 算）：
    - 往右推一次是一个 Next，推住只算一次；松到 0.4 再推出去不算，回到 0.1 再推算；直接扳到左边是 Previous；0.45、上下推都不算；官方遥控器也能翻。
    - 在外面误按一下再在外面松开：只走一步；松到 0.4 按一下再推出去：不走；中间开始长按、按着推出去再松开：不走也不开；
      快速甩出去、第一包就带着误按再在外面松开：走一步，按着 0.3s 再松也走一步；偏着 0.6 按住 1s 再偏着松开：不走、不连走、也不出进度条；
      `reset(5000)` 后 5500 推着按着、在外面松开：不走也不连走，回中再推走一步。
    - 推住：0.6s、1.05s 各再走一步，之后每 0.45s，最多再走 10 步；松到 0.4 就停，再推出去也不接着走；连走时按一下摇杆就停；只有包没有 `tick()` 时不连走。
    - 长按：0.4s 时进度 0.5，0.8s 打开、只开一次，之后进度归零；松开再按再开一次；0.5s 就松开不算；推偏了按下去不算，回中还按着也不算；
      按着时推离中间作废；M5 键不算；官方遥控器永远打不开。
    - 一批包：按着和松开在同一批里时不开（松开先算），一批里还按着就在 `tick()` 时开。
    - 第一包：`reset(5000)` 后 5500 的按下不算（松开再按算），6000 的算；5500 推着的不走、也不连走，回中再推算；6500 推着的算；
      `reset(5000, true)`（开机）后 5100 的按下、推出都算；从没 `reset()` 时按着不算；`reset()` 会作废正在进行的长按。
    - 时钟在长按和推住期间回绕：照常打开、照常连走。
  - `stackpet_remote_link_test`（真实的 `remote_link.cpp`，HAL / NVS / 日志换成 `tests/stubs_remote/`）：
    - 找遥控器：40s 里 ≥93% 的时间在存下的信道，其余 12 个都看过，每次离开不超过 200ms；先在 NVS 存的信道上；NVS 里的乱值用默认。
    - 听到以后：锁定在帧头 / 包里报的信道（串过来的也一样）、存进 NVS、不再找；同一个信道不重复写。
    - 接收 id：按 id 过滤，过滤掉的包不影响信道；id 存进 NVS、「重启」后从 NVS 读回，非法 id 不收。
    - 信道来源：帧头和包里说的不一致时按帧头；信道 14 的包丢掉、不锁定、继续找。
    - 跟随：最多每 3s 一次；两个遥控器来回拉，10s 内最多换 4 次；手动锁定的信道被遥控器自己的信道纠正。
    - M5 键：没在监听时收不到；按下取一次就没了，没人读包也收得到；按着再动摇杆不算新的一次，松开再按算；和摇杆按下同时也算，摇杆按下单独不算；
      超过 1s 才问作废；调用方的时钟比按下早 10ms 也算；别的机器人的、信道 14 的不算，改成那个 id 以后算；包照样交出去、按键位还在；
      `start()` 先从 NVS 读接收 id；`release()` 丢掉还没取的按下，重新 `start()` 后按着的那一包算一次；官方遥控器永远没有。
    - 保持监听的应用：五个名字是，另外七个、空串、小写的都不是；比较的是名字的内容。
    - `settle()`：没在监听时什么都不做；正在别的信道上看的时候回到存下的信道、不再找；已经在遥控器的信道或手动选的信道上时不动。
    - M5 按下次数：正常那一包和它的副本只算一次；松开没收到，下一次照样算；次数从 255 绕回 0 照样算；副本不交出去，
      从别的信道补发来的副本不会让链路跟过去；别的机器人的副本不算；重新开始监听以后，第一次听到的按下算。
    - M5 的日志（日志替身记下最后一条 info 的格式串和整数参数）：副本认出的打 `(its copy on every channel)`，正常包认出的打 `(sent on {})`，
      `{}` 是它的发送信道（从信道 3 串过来的就是 3）；正常包先到、副本后到时算正常包，副本先到时算副本；没取到按下时不打。
    - `attach()`：应用的 Wi-Fi 没连上时什么都不做；连上以后在路由器的信道（6）上听，10s 里不找、不跟、不存 NVS，
      `lockChannel()` / `settle()` 不动它，收到 M5 的副本照样回主屏；已经在听时再 `attach()`、`start()` 都不做事；`release()` 让 HAL 停掉。
    - `usedSincePowerOn()`（排在最前面跑）：一开始是假，这时 `attach()` 也不听；官方遥控器的 8 字节包、别的机器人的包都不算；
      StackPet 遥控器发给本机的包一到就是真（不用等 `update()`），`release()`、重新 `start()` 以后还是真。
    - 队列最多 32 包、丢最老的；时钟回绕时照常去别的信道看；`release()` 之后什么都不收，重新 `start()` 也不交出期间收到的包。
    - `pause()`（屏保取用量）：正在别的信道上看时暂停，`channel()` 回到存下的信道；暂停期间 20s 不换信道，别的信道来的包照样交出、不跟过去，
      M5 的副本照样认；`resume()` 以后 1s 内不去别的信道看，之后照常，听到了照常锁定；没在听时暂停不了，`release()`、`start()` 结束暂停。
  - 这两个测试也用 ASan + UBSan 跑过（`-fsanitize=address,undefined`）。
  - 没有主机测试的：`RemoteActions`（依赖 `GetStackChan()`、LVGL 和声音，要整套替身），启动器和启动器界面（LVGL）；
    `hal.cpp` 的 `remote_attach_update()` / `remote_notice_update()`（小智循环、堆统计、LVGL），HAL 的 `_espnow_init()` / `attachEspNow()`（Wi-Fi 驱动），
    `patches/esp-now.patch`（FreeRTOS 和 espnow 组件；只验证过能在干净的 `c33383de` 上打上、打完和本地的组件一样）。
    这些靠上机日志：`remote attached: ...`、`remote notice shown: stack N`、`M5 button: home (...)`。
    转圈「停稳才 `rotate()`」由 gate 用真实的 `servo.cpp` 和弹簧模拟验证过（64 种组合，起转后 0 次位置写入）。
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
