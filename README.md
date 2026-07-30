# Windows 音频设备控制工具

`audioctl` 是一个 C++17 命令行程序，使用 Windows Core Audio API 控制扬声器和麦克风。

当前版本：`v1.0.0`。程序图标由 `Six6.ico` 嵌入可执行文件。

## 构建

需要 Visual Studio 2022（安装“使用 C++ 的桌面开发”）和 CMake：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

生成的程序位于 `build\Release\audioctl.exe`。

MSVC 的 C/C++ 运行库采用静态链接：Release 使用 `/MT`，Debug 使用 `/MTd`，目标机器无需另行安装 Visual C++ Redistributable。Windows Core Audio、COM 等系统 DLL 仍由操作系统提供。

## 使用

先列出活动设备。每一类设备的序号单独从 `0` 开始，`*` 表示当前默认设备：

```powershell
audioctl list
audioctl list speakers
audioctl list microphones
```

五种操作对应题目中的数字参数：

```powershell
# 1：切换扬声器（可用序号、完整设备 ID 或唯一的名称片段）
audioctl 1 1
audioctl 1 "USB Audio"

# 2：切换麦克风
audioctl 2 0

# 3：设置扬声器音量为 65%；省略设备时操作当前默认扬声器
audioctl 3 65
audioctl 3 65 1

# 4：设置麦克风音量为 80%
audioctl 4 80

# 5：设置麦克风硬件增益（Microphone Boost）为 20 dB
audioctl 5 20
audioctl 5 20 0
```

设备切换会同时修改 Windows 的控制台、多媒体和通信三种默认角色。名称匹配不区分大小写；名称匹配到多个设备时，程序会要求改用序号或完整设备 ID。

## 麦克风增益说明

麦克风“音量”和硬件“增益/加强”是两个不同控件。操作 `4` 修改端点音量，操作 `5` 查找设备拓扑中的 Boost/Gain 控件并以 dB 设置。可以先查看硬件暴露的控件和范围：

```powershell
audioctl gain-controls
audioctl gain-controls 0
```

并非所有麦克风或 USB 声卡都提供硬件增益控件；这种情况下操作 `5` 会返回明确错误，无法通过通用 Core Audio API 强行增加该功能。
