# uvcview — 海康 4117 DIY 热成像仪实时预览

这是一个独立的 Cardputer Zero 热成像仪 App，硬件是 **DIY 组装的海康 4117
热成像仪**（不是 HIK 品牌整机），使用 **libuvc + libjpeg**，
绕过 Linux 的 `uvcvideo`/V4L2 直接读取 USB 摄像头数据，然后写
`/dev/fb_lcd`（320×170）显示。

## 为什么不用 V4L2

在 Cardputer Zero 的 Linux 上，这台 DIY 热成像仪（USB `2bdf:0101`，被识别
成普通 UVC 摄像头）虽然会被
`uvcvideo` 识别并创建 `/dev/video1`，但取流结果损坏：

```text
Dequeued v4l2 buffer contains corrupted data
```

240×320 YUYV 一帧本应是 153600 字节，实际每次只收到约 10KB 的 UVC payload，
uvcvideo 没有正确重组。Mac 上普通 UVC 软件可以正常使用，说明相机本身没问题。

`uvcview` 使用 libusb/libuvc 自己处理 UVC bulk 流，并从设备收到
**MJPEG/JPEG 帧**（虽然设备有时把格式标成 YUYV），所以能正常显示。

## 依赖

```bash
sudo apt-get install -y libuvc-dev libjpeg-dev libfreetype-dev
```

## 构建

```bash
cd uvcview
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

生成的二进制是：

```text
uvcview/build/uvcview
```

## 安装

```bash
sudo install -m 755 build/uvcview /usr/local/bin/uvcview
sudo install -d /usr/share/APPLaunch/bin
sudo install -m 755 run-uvcview /usr/share/APPLaunch/bin/run-uvcview
sudo install -m 644 uvcview.desktop \
    /usr/share/APPLaunch/applications/uvcview.desktop
```

图标使用系统已有的 `camera1.png`：

```text
/usr/share/APPLaunch/share/images/camera1.png
```

然后安装专用 sudo 规则，让 APPLaunch 里的 `pi` 用户可以免密以 root
运行查看器：

```bash
sudo tee /etc/sudoers.d/uvcview >/dev/null <<'EOF'
%sudo ALL=(root) NOPASSWD: /usr/local/bin/uvcview
EOF
sudo chmod 440 /etc/sudoers.d/uvcview
sudo visudo -cf /etc/sudoers.d/uvcview
```

如果没有这条规则，`run-uvcview` 里的 `sudo -n` 会失败，APPLaunch 看起来
就是“闪一下”。现在即使 sudo 失败，wrapper 也会退回普通用户运行，
由 App 自己显示可读的错误画面。

然后重启 APPLaunch：

```bash
export XDG_RUNTIME_DIR=/run/user/1000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
systemctl --user restart APPLaunch.service
```

APPLaunch 中应出现 **Thermal Camera**。

## 权限

libuvc 需要打开 USB 设备；在 Cardputer Zero 上，普通用户会得到：

```text
uvc_open: Access denied
```

`run-uvcview` 使用 `sudo -n` 以 root 运行，APPLaunch 的 `pi` 用户已配置
免密 sudo。也可以手动运行：

```bash
sudo /usr/local/bin/uvcview \
    -d /dev/fb_lcd \
    -k /dev/input/by-path/platform-3f804000.i2c-event
```

如果手动以普通用户运行时提示 USB 访问失败，请使用 `sudo`，或者给
USB 设备添加 udev 权限后以普通用户运行。

## 参数

```text
-d <fb>       framebuffer，默认 /dev/fb_lcd
-k <kbd>      evdev 键盘，默认 platform-3f804000.i2c-event
-r 0|90|180|270
              初始旋转角度，默认 90（顺时针）
-f            使用 fill 模式（等比放大并裁剪）
-t            使用 stretch 模式（默认：拉伸填满整个屏幕）
-n            关闭左右镜像（默认开启）
-b <value>    设置背光亮度（默认使用 max_brightness）
-h            帮助
```

## 按键

| 按键 | 功能 |
|---|---|
| `Esc` / `Q` | 退出 |
| `R` | 旋转 90° |
| `M` | 切换 fit / fill / stretch |
| `5` | 上下镜像（垂直翻转） |
| `6` | 左右镜像（水平翻转） |
| `7` | 拍照 |
| `8` | 进入 / 翻看拍照 |
| `4` | 切换画面比例（original / fill / stretch） |
| `←` / `↑` | 照片浏览：上一张 |
| `→` / `↓` | 照片浏览：下一张 |
| `Esc` | 照片浏览：返回实时画面；实时画面中退出 |

默认是 **顺时针 90° + 左右镜像 + stretch** 模式：240×320 的热成像画面
先按 170×320 的目标比例拉伸，再顺时针旋转 90°，最终填满 320×170 屏幕，
不再留左右黑边。按 `R` 继续旋转、按 `M` 在 fit / fill / stretch 之间切换。

### 屏幕按键

屏幕按键按**实体数字键 3 和 9 对应屏幕左右两个角**来定位，因此
`4`～`8` 的五个药丸按钮会落在屏幕偏左的位置，和实际键位对齐：

```text
4 AR    比例切换（original / fill / stretch）
5 VM    上下镜像
6 HM    左右镜像
7 PIC   拍照
8 VIEW  翻看拍照
```

`4` 按钮在照片浏览状态下也可以切换比例；当前的 `original`（等比完整）
状态会用白色药丸表示。

视觉风格：

- 药丸形（capsule）按钮，不是粗椭圆
- 1px 细边框 + 半透明深色填充，叠在热成像画面上
- 未选中：深色半透明填充 + 白色文字
- 选中：同一个细边框，但填充变白，文字改成深色
- 标签使用系统的 **Noto Sans CJK Light 12px** 抗锯齿字体
  （失败时回退 DejaVu Sans ExtraLight / DejaVu Sans）

### 拍照

按 `7` 会把当前热成像原始 JPEG 保存到：

```text
/home/pi/Pictures/Thermal/thermal_YYYYMMDD_HHMMSS_mmm.jpg
```

按 `8` 进入照片浏览，默认显示最新一张。

进入浏览后：

- `←` / `↑`：上一张（更旧）
- `→` / `↓`：下一张（更新）
- 到两端会循环
- `Esc`：回到实时画面
- `8`：继续往旧照片翻；翻到最旧后再按回到实时画面

照片浏览时仍可以用 `5`/`6` 镜像、`R` 旋转、`M` 切换缩放。

程序每秒会重新设置一次背光和 framebuffer blank 状态，防止桌面空闲逻辑
把屏幕关掉。默认会恢复到 `max_brightness`；可以用 `-b` 指定较低亮度。

## 画面说明

设备实际发送的是 **JPEG** 帧，程序会跳过不是 `FF D8` 开头的损坏帧。
屏幕上的 “Max xx.x°C” 等文字是热成像仪画面本身的一部分，本程序目前只做
图像显示，不解析模块内部的温度数据/扩展单元。

## 错误提示

启动失败时不会再直接退出闪回 APPLaunch。程序会在 framebuffer 上显示
一个可读的错误页，例如：

```text
THERMAL CAMERA
CAMERA BUSY OR ACCESS DENIED
Access denied
PRESS ESC OR Q TO CLOSE
```

支持的错误包括：

- `CAMERA NOT FOUND` / `CHECK USB CABLE`
- `CAMERA BUSY OR ACCESS DENIED`
- `CAMERA FORMAT ERROR`
- `STREAM START FAILED`
- `NO VIDEO FRAMES`

错误页会一直显示，直到按 `Esc`/`Q`，或 15 秒后自动退出。
即使 `sudo` 规则缺失、程序以普通用户运行，也会显示 USB 权限错误，
而不是一闪而过。

## 测试

```bash
sudo /usr/local/bin/uvcview -d /dev/fb_lcd
```

如果截图需要，可从另一个 SSH 会话执行：

```bash
sudo dd if=/dev/fb0 bs=640 count=170 2>/dev/null > /tmp/fb.raw
```

然后把 RGB565 原始数据转成 PNG 查看。
