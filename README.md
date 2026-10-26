# MarketDataService —— 独立 CTP 期货行情网关

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

MarketDataService 是一个**独立行情服务**。它连接一个或多个 CTP 行情前置，
订阅全市场期货合约，把每一笔 `OnRtnDepthMarketData` 行情：

- 通过 **TCP 单播** 和 **UDP 组播** 实时转发，报文采用固定的 JSON 格式；
- 按需写入 **ClickHouse**（HTTP 接口 + `JSONEachRow`）或 **本地 CSV 文件**，也可以只转发不落盘；
- 在配置的交易时段结束时**自动优雅退出**，适合交给计划任务 / 守护进程按交易日拉起。

> 本服务只做行情接收、转发与归档，**不包含交易、下单、风控或结算逻辑**。

---

## 目录

1. [功能特性](#1-功能特性)
2. [架构与数据流](#2-架构与数据流)
3. [源码模块](#3-源码模块)
4. [目录结构](#4-目录结构)
5. [构建](#5-构建)
6. [部署与运行](#6-部署与运行)
7. [配置参考](#7-配置参考)
8. [合约列表](#8-合约列表)
9. [存储后端](#9-存储后端)
10. [广播协议](#10-广播协议)
11. [运行期行为](#11-运行期行为)
12. [已知限制](#12-已知限制)
13. [故障排查](#13-故障排查)
14. [开发约定](#14-开发约定)
15. [许可证与第三方组件](#15-许可证与第三方组件)

---

## 1. 功能特性

| 能力 | 说明 |
| --- | --- |
| 多行情源 | `CTPSources[]` 中每个账号建立独立 `CThostFtdcMdApi`，可分别配置前置、流文件目录和交易所过滤 |
| 全市场订阅 | 启动时从 openctp dict HTTP 服务拉取全市场期货合约，本地缓存作为离线兜底 |
| 合约分片 | 同一合约只分配给**第一个**匹配的行情源，避免重复订阅、重复广播、重复入库 |
| 行情转发 | 单播 TCP 长连接 + 组播 UDP，报文格式固定，现有行情客户端可直接接入 |
| 快照恢复 | 每个合约保留最新报文；行情静默超过阈值后整体重发；慢客户端队列可裁剪 |
| 双存储后端 | ClickHouse（HTTP）或本地 CSV，均异步批量写入，不阻塞 CTP 回调线程 |
| 按需存储 | 两个后端都关闭时使用 `NullWriter`，仅转发不落盘 |
| 自动退出 | 可配置日盘 / 夜盘结束时间，到点优雅退出 |
| 优雅停机 | `Ctrl+C` / `SIGTERM` 先停行情、再刷盘、最后释放 CTP API |
| 零外部服务依赖 | 除 CTP 前置、可选的 ClickHouse / openctp 外，无 Redis、MQ、数据库客户端等依赖 |

---

## 2. 架构与数据流

```text
                  ┌──────────────────────────┐
 CTP 行情前置 1 ──▶│ CTPMarketDataSource #1   │──┐
                  │  CThostFtdcMdSpi         │  │
                  └──────────────────────────┘  │
                  ┌──────────────────────────┐  │   mdgw::Tick
 CTP 行情前置 N ──▶│ CTPMarketDataSource #N   │──┤
                  │  (每个源一个 CTP 实例)    │  │
                  └──────────────────────────┘  │
                                                │
                     ┌──────────────────────────┴──────────────────────────┐
                     │                                                     │
                     ▼                                                     ▼
   ┌─────────────────────────────────────┐             ┌──────────────────────────────────┐
   │ MarketDataDispatcher (1 个 io 线程)  │             │ TickWriter (1 个 worker 线程)     │
   │  · 全局 Seq                         │             │  优先级:                         │
   │  · 编码 T:300 JSON                  │             │  LocalFileWriter  (CSV)          │
   │  · 更新快照 Snapshot                │             │  > ClickHouseWriter (HTTP)       │
   │  · 单播 / 组播发送                  │             │  > NullWriter      (不落盘)      │
   └──────────────┬──────────────────────┘             └──────────────────────────────────┘
                  │
        ┌─────────┴──────────┐
        ▼                    ▼
 ┌─────────────┐      ┌────────────────┐
 │ UnicastServer│      │ MulticastSender│
 │ TCP 监听     │      │ UDP 组播发送   │
 └─────────────┘      └────────────────┘
```

### 2.1 一笔行情的处理路径

1. CTP 线程回调 `OnRtnDepthMarketData`，把 CTP 结构体逐字段转换为 `mdgw::Tick`：
   - 价格字段做 `doubleMaxTo0()`：CTP 用 `DBL_MAX` 表示“无值”，统一转 0；
   - `Tm` 拼成 `HH:MM:SS.mmm`。
2. **成交量过滤**：`Volume <= 0` 的行情在入口直接丢弃，不广播、不落盘、不计数。
3. 有效行情先交给 `MarketDataDispatcher`，由 io 线程分配 Seq、编码并立即广播。
4. **新鲜度过滤**：行情时间与本地时间的环形差超过 **60 秒**时，只广播、**不写入存储**、也不计入 tick 统计。
   这是为了避免早上启动时把 CTP 推送的历史快照写进当天的数据文件。
5. 通过新鲜度检查的行情交给当前 `TickWriter`，由独立 worker 线程批量落盘。

### 2.2 线程模型

| 线程 | 数量 | 职责 |
| --- | --- | --- |
| CTP 内部线程 | 每个源若干 | 触发 `OnRtn*` 回调；本服务只做轻量转换与入队 |
| Dispatcher io 线程 | 1 | 分配 Seq、编码 JSON、维护快照、单播 / 组播发送、队列裁剪、快照重发定时器 |
| Writer worker 线程 | 1 | 批量写 ClickHouse 或 CSV；失败退避重试 |
| 统计线程 | 1 | 每 60 秒打印 tick 与写入统计 |
| 主线程 | 1 | 每 200ms 轮询停止标志，每秒检查自动退出时间 |
| CTP `Join()` 线程 | 每个源 1 | 等待 `Release()`；由 watchdog 限时 10 秒 |

行情回调线程**不做磁盘 I/O、不等待网络**。唯一的例外是 CSV 后端在队列满时会对回调线程施加最长
120 秒的背压（见 [9.4](#94-本地-csv-存储localfile)）。

---

## 3. 源码模块

| 文件 | 职责 |
| --- | --- |
| `src/main.cpp` | 入口：解析 `-c/--config`、切换工作目录到可执行文件目录、安装信号处理、启动 `Service` |
| `src/Service.{h,cpp}` | 装配与生命周期：读配置 → 拉合约 → 分片 → 启动 dispatcher / writer / CTP 源；统计与自动退出 |
| `src/Config.{h,cpp}` | 解析 `MarketDataService.json`，填充带默认值的配置结构，做边界收敛 |
| `src/ContractList.{h,cpp}` | openctp dict HTTP 拉取、JSON 解析、缓存读写 |
| `src/CTPMarketDataSource.{h,cpp}` | `CThostFtdcMdSpi` 实现：连接、登录、分块订阅、断线重连、行情转换与过滤 |
| `src/Tick.{h,cpp}` | 行情结构体、交易所归一化、`doubleMaxTo0()` |
| `src/BroadcastEncoder.{h,cpp}` | 用 cJSON 编码旧 `T:300` 单行报文，追加 `\r\n` |
| `src/MarketDataDispatcher.{h,cpp}` | 全局序号、快照、单播 / 组播扇出、静默快照重发 |
| `src/UnicastServer.{h,cpp}` | TCP 监听、连接上限、每连接 FIFO、慢客户端队列裁剪 |
| `src/MulticastSender.{h,cpp}` | UDP 组播发送与队列裁剪 |
| `src/TickWriter.h` | 存储抽象基类 `TickWriter` 与空实现 `NullWriter` |
| `src/ClickHouseWriter.{h,cpp}` | ClickHouse HTTP 批量写入、启动探测、失败退避 |
| `src/LocalFileWriter.{h,cpp}` | 本地 CSV 异步写入、按自然日分文件、按大小滚动、失败重排 |
| `src/Logger.{h,cpp}` | 控制台 + 文件日志，级别 `DEBUG/INFO/WARN/ERROR` |
| `src/FanoutTypes.h` | `EncodedItem`、`Snapshot` 类型定义 |
| `sql/create_tick_table.sql` | ClickHouse 建表脚本（`market.tick`） |
| `config/*.json` | 三套开箱即用配置：ClickHouse / 本地文件 / 仅广播 |
| `third_party/` | cJSON 源码；Asio 与 CTP SDK 以压缩包形式内置 |

---

## 4. 目录结构

```text
MarketDataService/
├─ CMakeLists.txt              # 跨平台构建（MSVC / Linux）
├─ CMakePresets.json           # 可选的 CMake 预设（release / debug）
├─ README.md                   # 本文档
├─ .gitignore                  # 构建产物、运行数据、IDE 文件
├─ .gitattributes              # 换行符与二进制文件标记
├─ .editorconfig               # 编辑器基础风格
├─ src/                        # 服务源码
│  ├─ main.cpp
│  ├─ Service.{h,cpp}
│  ├─ Config.{h,cpp}
│  ├─ ContractList.{h,cpp}
│  ├─ CTPMarketDataSource.{h,cpp}
│  ├─ Tick.{h,cpp}
│  ├─ BroadcastEncoder.{h,cpp}
│  ├─ MarketDataDispatcher.{h,cpp}
│  ├─ UnicastServer.{h,cpp}
│  ├─ MulticastSender.{h,cpp}
│  ├─ TickWriter.h
│  ├─ ClickHouseWriter.{h,cpp}
│  ├─ LocalFileWriter.{h,cpp}
│  ├─ FanoutTypes.h
│  └─ Logger.{h,cpp}
├─ config/
│  ├─ MarketDataService.json              # ClickHouse 入库
│  ├─ MarketDataService.local-file.json   # 本地 CSV（默认配置）
│  └─ MarketDataService.broadcast-only.json # 仅转发
├─ sql/
│  └─ create_tick_table.sql
├─ third_party/
│  ├─ cJSON.h / cJSON.cpp                 # 源码内置
│  ├─ asio-asio-1-30-2.zip                # configure 时解包
│  └─ v6.7.10_20250422_traderapi.zip      # CTP SDK，configure 时解包
├─ logs/                       # 运行期日志（logs/.gitkeep 占位）
├─ build/                      # 构建目录（生成的，不入库）
└─ out/                        # Visual Studio CMake 集成输出（生成的，不入库）
```

---

## 5. 构建

### 5.1 依赖

| 依赖 | 提供方式 | 说明 |
| --- | --- | --- |
| CMake | 系统安装 | **≥ 3.20**（`file(ARCHIVE_EXTRACT)` 需要 3.18+） |
| C++ 编译器 | 系统安装 | C++11；Windows **推荐 MSVC** |
| standalone Asio 1.30.2 | `third_party/asio-asio-1-30-2.zip` | header-only，`ASIO_STANDALONE`，**不需要 Boost** |
| CTP 行情 API | `third_party/v6.7.10_20250422_traderapi.zip` | 使用其中的 `thostmduserapi_se`（头文件 + 导入库/动态库） |
| cJSON | `third_party/cJSON.{h,cpp}` | 直接编译进目标 |

两个压缩包在 **configure 阶段**解包到构建目录的 `deps/` 下（如 `build/deps/`，使用预设时为
`build/presets/<preset>/deps/`），源码目录保持干净；删除构建目录即可完全重置依赖。
整个构建过程**不需要联网**。

> Windows 下 CTP SDK 提供的是 MSVC 格式的 `.lib`，因此 MinGW 可以编译源码但**无法链接** CTP 库；
> 请使用 MSVC / Visual Studio 生成工具。
>
> CTP SDK 是第三方**专有组件**，不在本项目的 MIT 许可范围内。仓库内置压缩包仅为构建便利；
> 若你没有再分发权，请从仓库中移除，并通过 `-DCTP_API_DIR` 使用本机 SDK。详见
> [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

### 5.2 标准构建

Linux（单配置生成器）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Windows（Visual Studio / NMake；请在 **Developer PowerShell** 或 “x64 Native Tools” 命令行中执行，
或在 Visual Studio 中直接打开本目录使用其 CMake 集成）：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

也可以直接使用内置预设（预设文件使用 schema v3，需要 CMake ≥ 3.21）：

```bash
cmake --preset release
cmake --build --preset release
```

如果只有 CMake 3.20，请使用上面的直接命令，构建本身不依赖预设。

产物：

- Windows：`build/.../MarketDataService.exe`，构建后会把 `thostmduserapi_se.dll` 拷到 exe 同目录；
- Linux：`build/MarketDataService`，构建后会把 `libthostmduserapi_se.so` 拷到可执行文件旁，并把
  `$ORIGIN` 写入 RUNPATH，因此整个目录可以直接拷走运行，无需 `LD_LIBRARY_PATH`。

### 5.3 使用已解压的 CTP SDK

仓库内的压缩包只是兜底。若机器上已有 CTP SDK，可显式指定，优先级高于内置归档：

```bash
cmake -S . -B build -DCTP_API_DIR=/path/to/mduser_win64
```

`CTP_API_DIR` 必须包含：

- `ThostFtdcMdApi.h`
- Windows：`thostmduserapi_se.lib` + `thostmduserapi_se.dll`
- Linux：`thostmduserapi_se.so`

configure 阶段会立即校验头文件；缺失时给出明确的 `FATAL_ERROR`，而不是等到编译时报 `#include` 找不到。

### 5.4 IDE / clangd

`CMAKE_EXPORT_COMPILE_COMMANDS` 默认开启，在构建目录生成 `compile_commands.json`，可直接供 clangd /
clang-tidy 使用。新增 `src/*.cpp` 后无需手工改 CMake（`GLOB CONFIGURE_DEPENDS`），重新构建会自动重新 configure。

### 5.5 平台细节

- 源码为 UTF-8；MSVC 会加 `/utf-8`，并关闭因 CTP GBK 头文件产生的 `C4828` 噪声。
- Windows 链接 `ws2_32 mswsock`，并定义 `_WIN32_WINNT=0x0501`、`NOMINMAX`。
- Linux 链接 `pthread`；CTP 的 `thostmduserapi_se.so` 没有 `lib` 前缀，CMake 会在 `build/deps/lib`
  下建一个 `libthostmduserapi_se.so` 符号链接供链接器使用。

---

## 6. 部署与运行

### 6.1 关键规则：工作目录 = 可执行文件目录

`main.cpp` 启动后会 **`chdir` 到可执行文件所在目录**。因此：

- `-c` 的相对路径、配置中的 `LogFile`、`InstrumentCacheFile`、`LocalFile.DataDir`、`FlowPath`
  全部相对 **可执行文件目录** 解析，而不是调用者的当前目录；
- 用系统服务 / 计划任务拉起时，这个行为可以避免“双击能跑、服务里找不到配置”的问题；
- 需要绝对路径时直接写绝对路径即可。

推荐的部署目录：

```text
deploy/
├─ MarketDataService              # 或 MarketDataService.exe
├─ thostmduserapi_se.dll          # Windows 运行时（Linux 为 libthostmduserapi_se.so）
├─ config/
│  └─ MarketDataService.local-file.json
├─ logs/                          # 自动创建
├─ MarketDataFlow/                # CTP 流文件，自动创建
└─ marketdata_YYYYMMDDHHMMSS/     # 本地 CSV 输出，自动创建
```

### 6.2 启动

```bash
# 默认配置：config/MarketDataService.local-file.json
./MarketDataService

# 显式指定（相对可执行文件目录）
./MarketDataService -c config/MarketDataService.json

# 绝对路径
./MarketDataService -c /etc/marketdata/MarketDataService.json
```

Windows：

```powershell
.\MarketDataService.exe -c config\MarketDataService.local-file.json
```

参数：

| 参数 | 说明 |
| --- | --- |
| `-c <file>` / `--config <file>` | 配置文件路径 |
| `-h` / `--help` | 打印用法 |
| 其他参数 | 打印 `ignoring unknown option` 后忽略，便于守护脚本传入额外参数 |

> 默认配置是 `config/MarketDataService.local-file.json`。如果从仓库直接运行 `build/` 下的 exe，
> 请把 `config/` 复制到 exe 同目录，或传入**绝对路径**的 `-c`；配置里的其他相对路径仍然相对 exe 目录。

### 6.3 停止

| 平台 | 触发方式 |
| --- | --- |
| Windows | `Ctrl+C`、`Ctrl+Break`、关闭控制台、系统关机 |
| Linux | `SIGINT`、`SIGTERM`、`SIGHUP` |

信号处理函数只翻转一个原子标志，**不写日志、不分配内存**。真正的关闭顺序在 `Service::run()` 中：

1. 注销 CTP SPI（保证不再有新行情进入）；
2. 停止 dispatcher（不再广播）；
3. 停止 writer（把队列中的行情刷盘 / 写 ClickHouse）；
4. 释放 CTP API（`Release()` + `Join()`，watchdog 限时 10 秒）；
5. 等待统计线程退出，打印 `market data service stopped`。

### 6.4 退出码

| 退出码 | 含义 |
| --- | --- |
| `0` | 正常停止（信号或自动退出时间） |
| `1` | 初始化失败（配置、合约列表、CTP 源启动失败） |
| `1`（无正常日志） | `Release()` 超过 10 秒未返回，watchdog 调用 `_Exit(1)` |

初始化阶段（日志系统尚未配置）失败时，会在 exe 旁追加写入 `startup.log`，便于无控制台的守护进程排查。

### 6.5 ClickHouse 建表

首次部署、且启用 `ClickHouse.Enabled` 时：

```bash
clickhouse-client --query "CREATE DATABASE IF NOT EXISTS market"
clickhouse-client --multiquery < sql/create_tick_table.sql
```

表名 / 库名必须与配置中的 `ClickHouse.Database` / `ClickHouse.Table` 一致（脚本默认 `market.tick`）。
服务只写入、**不会自动建表**；启动时的 `SELECT 1` 探测失败只会告警，writer 仍会在后台持续重试。

---

## 7. 配置参考

三套示例配置的差别：

| 配置文件 | ClickHouse | 本地 CSV | 用途 |
| --- | --- | --- | --- |
| `config/MarketDataService.json` | ✅ | ❌ | 行情入库到 ClickHouse |
| `config/MarketDataService.local-file.json` | ❌ | ✅ | 本地 CSV 归档（**默认**） |
| `config/MarketDataService.broadcast-only.json` | ❌ | ❌ | 只转发，不落盘 |

> 示例中的 CTP 前置地址、ClickHouse 地址、端口仅为占位，部署前请替换成自己的环境；
> `UserID` / `Password` 默认为空，必须填写有行情权限的账号。
>
> 后端选择不是“同时写”：代码优先级为 **LocalFile > ClickHouse > NullWriter**。两个都启用时只有 CSV 生效。

### 7.1 顶层配置

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `InstrumentSourceUrl` | `http://dict.openctp.cn/instruments?types=futures` | 全市场合约来源；**仅支持 `http://`**，超时 10 秒，响应上限 8MB |
| `InstrumentCacheFile` | `config/instruments_cache.json` | 合约原始响应的本地缓存；成功后覆盖写入，失败时读取 |
| `DayExitTime` | `15:18` | 日盘自动退出时间，本地时间 `HH:MM` 或 `HHMM`；空串 / 非法值表示禁用 |
| `NightExitTime` | `02:35` | 夜盘自动退出时间，本地时间；空串 / 非法值表示禁用 |
| `LogFile` | 空 | 日志文件；空 = 仅控制台。目录不存在会自动创建，文件以追加方式写入 |
| `LogLevel` | `1` | `0=DEBUG`、`1=INFO`、`2=WARN`、`3=ERROR`；越界重置为 1 |

### 7.2 单播 `Unicast`

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `Enabled` | `false` | 是否启动 TCP 单播服务 |
| `Address` | 空 | 监听地址，通常 `0.0.0.0`（IPv4） |
| `Port` | `0` | 监听端口 |
| `MaxConnections` | `66` | 最大并发连接数；超出的连接会被拒绝并记录 WARN |

### 7.3 组播 `Multicast`

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `Enabled` | `false` | 是否发送 UDP 组播 |
| `Address` | 空 | 组播地址，例如 `239.0.0.3`（IPv4） |
| `Port` | `0` | 组播端口 |
| `Hops` | `5` | 组播 TTL（`asio::ip::multicast::hops`） |

### 7.4 广播 `Broadcast`

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `Levels` | `1` | 仅接受 `1` / `5`，但**当前无实际作用**：行情采集与编码只保留一档盘口 |
| `SnapshotIdleSeconds` | `30` | 行情静默超过该秒数后，重发全市场最新快照；小于 1 时按 1 处理 |

### 7.5 ClickHouse `ClickHouse`

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `Enabled` | `false` | 是否启用 |
| `Host` | `127.0.0.1` | ClickHouse HTTP 服务地址 |
| `Port` | `8123` | **HTTP 端口**，不是原生 TCP 端口 `9000` |
| `Database` / `Table` | `market` / `tick` | 目标库表 |
| `User` / `Password` | `default` / 空 | HTTP Basic Auth；`User` 为空时不发送认证头 |
| `BatchRows` | `1000` | 队列达到该行数时立即触发一次写入 |
| `FlushIntervalMs` | `1000` | 最长等待时间；最小 10 |
| `MaxQueuedRows` | `200000` | 内存队列上限；溢出时新行情被丢弃并计入 `dropped`；不小于 `BatchRows` |
| `ConnectTimeoutMs` | `5000` | 连接超时 |
| `ReadTimeoutMs` | `30000` | 读取响应超时 |
| `RetrySleepMs` | `3000` | 写入失败后的基础退避；连续失败会线性 / 封顶 30 秒退避 |

### 7.6 本地文件 `LocalFile`

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `Enabled` | `false` | 是否启用 |
| `DataDir` | `marketdata` | 输出根目录；**启动时会自动追加 `_YYYYMMDDHHMMSS`**，见 [9.4](#94-本地-csv-存储localfile) |
| `FilePrefix` | `tick` | 文件名前缀 |
| `FlushIntervalMs` | `1000` | 队列为空时 worker 的轮询 / 批量等待间隔；最小 10 |
| `MaxQueuedRows` | `200000` | 内存队列上限；满时回调线程等待（最长 120 秒）而不是立即丢弃 |
| `MaxFileSize` | `104857600`（100MB） | 单文件滚动阈值，最小 1MiB。内部按 `int` 解析，建议不超过 1GiB |

> 旧版文档中的 `LocalFile.BatchRows`、`MaxFiles`、`Compress` **已不存在**，写入这些键不会生效。

### 7.7 行情源 `CTPSources[]`

每个数组元素对应一个 CTP 行情账号。

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `BrokerID` | 空 | 期货公司 BrokerID |
| `UserID` / `Password` | 空 | 行情账号；必须具有行情权限 |
| `AppID` / `AuthCode` | 空 | **保留字段**：当前使用的 CTP 行情 API 没有 `ReqAuthenticate`，不会发送 |
| `FlowPath` | 空 | CTP API 流文件目录；空时使用 `MarketDataFlow/<BrokerID>_<UserID>`，并自动创建 |
| `MarketDataServerURIs` | 空 | 前置地址数组，例如 `["tcp://1.2.3.4:41213"]`；可配置多个 |
| `ExchangeFilter` | `[]` | 只接收这些交易所（规范 ID：`ZCE/DCE/SHFE/CFFE/INE/GFEX`）；空 = 全部 |
| `SubscribeChunkSize` | `300` | 每次 `SubscribeMarketData` 的合约数；小于 1 时按 300 |
| `LoginRetrySeconds` | `15` | 登录失败后的重试间隔；小于 1 时按 15 |

`CTPSources` 不能为空，否则配置解析直接失败。

---

## 8. 合约列表

### 8.1 主来源：openctp dict 服务

启动时同步请求：

```text
GET http://dict.openctp.cn/instruments?types=futures
```

处理规则：

- 只支持 `http://`，不支持 `https://`；连接 / 读取超时 10 秒，响应体上限 8MB；
- 响应为 `{ "rsp_code":0, "data":[ { "ExchangeID":"SHFE", "InstrumentID":"rb2609", "ProductClass":"1", ... } ] }`；
- `ProductClass` 存在且不等于 `"1"` 的记录（期权等）被忽略，缺失该字段的记录保留；
- 缺少 `ExchangeID` / `InstrumentID` 的记录被忽略；
- 按 `(ExchangeID, InstrumentID)` 去重，同一合约只保留第一条。

每次拉取成功后，**原始响应体**会以“临时文件 + 替换”的方式写入 `InstrumentCacheFile`。

### 8.2 缓存兜底

远端不可达、超时、HTTP 非 200、JSON 解析失败时：

- 读取 `InstrumentCacheFile` 中上一次保存的响应并解析；
- 缓存也不可用 → 启动失败，退出码 1，并打印具体原因。

合约列表**只在启动时刷新一次**，运行期间不会热更新。新增合约需要重启服务。

### 8.3 多账号分片

对每个合约按 `CTPSources` 的**数组顺序**做 first-match：

1. 若该源配置了 `ExchangeFilter`，合约交易所必须在列表中；
2. 合约分配给第一个匹配且尚未认领它的源；
3. 后续源不会再拿到同一合约。

因此：

- 源顺序有意义；不带 `ExchangeFilter` 的源会“吃掉”后面所有源能匹配的合约；
- 某个源最终没有分到任何合约时会被跳过并记录 WARN；
- 交易所过滤不匹配的合约最终会被忽略并汇总 WARN。

### 8.4 订阅与重连

- 登录成功后，按 `SubscribeChunkSize` 分块调用 `SubscribeMarketData`，每块之间间隔 50ms；
- 登录失败按 `LoginRetrySeconds` 重试；
- CTP 断线后由 CTP 库自动重连，`OnFrontConnected` 会重新登录并重新订阅；
- `SubscribeMarketData` 返回非 0 时记录 WARN，但不阻断其他分块。

---

## 9. 存储后端

存储是可选组件。早期版本只支持 ClickHouse，当 ClickHouse 连接不稳定或写入压力过大时，
行情链路容易受到影响；现在可以在 **ClickHouse / 本地 CSV / 不落盘** 之间切换，
且写入始终在独立 worker 线程完成，不阻塞 CTP 回调。

### 9.1 抽象接口

`src/TickWriter.h` 定义统一存储接口：

```cpp
class TickWriter
{
public:
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void push(const Tick & tick) = 0;
    virtual size_t queueSize() const = 0;
    virtual unsigned long long received() const = 0;
    virtual unsigned long long written() const = 0;
    virtual unsigned long long dropped() const = 0;
    virtual unsigned long long failedRows() const = 0;
};
```

实现：

| 实现 | 配置 | 行为 |
| --- | --- | --- |
| `ClickHouseWriter` | `ClickHouse.Enabled = true` | HTTP `INSERT ... FORMAT JSONEachRow` |
| `LocalFileWriter` | `LocalFile.Enabled = true` | 本地 CSV，按自然日分文件、按大小滚动 |
| `NullWriter` | 两者都关闭 | 只广播，不存储 |

选择优先级：**`LocalFile` > `ClickHouse` > `NullWriter`**。不要同时启用两个后端；当前版本不支持双写。

### 9.2 后端对比

| 维度 | ClickHouse | 本地 CSV | Null |
| --- | --- | --- | --- |
| 传输 / 格式 | HTTP POST，`JSONEachRow` | 追加写 CSV | 无 |
| 批量 | `BatchRows` 或 `FlushIntervalMs`，单请求最多 5000 行 | 最多 5000 行 / 次，`FlushIntervalMs` 轮询 | 无 |
| 队列上限 | `MaxQueuedRows` | `MaxQueuedRows` | 无 |
| 队列满 | 立即丢弃并计数 | 回调线程等待最多 120 秒 | 无 |
| 写入失败 | 该批**丢失**并计入 `failed`，退避重试下一批 | 整批**重排回队列**，持续重试，不丢行 | 无 |
| 停机刷新 | 尽量写完队列 | 刷新队列，最多重试 120 次 | 无 |
| 适用场景 | 集中查询 / 分析 / 长期存储 | ClickHouse 不可用、需要本地留存或离线导入 | 纯转发 |

> “不丢行”仅指正常磁盘可用的情况。CSV 后端在磁盘持续不可用超过 120 秒后仍会丢弃并计数；
> 停机阶段重试 120 次仍失败的行会计入 `failed`。

### 9.3 ClickHouse 存储

写入方式：

```text
POST http://<Host>:<Port>/?query=INSERT%20INTO%20<Database>.<Table>%20FORMAT%20JSONEachRow
Content-Type: text/plain; charset=UTF-8
Authorization: Basic <base64(user:password)>   # User 非空时
```

- worker 线程每 `FlushIntervalMs` 唤醒一次；队列达到 `BatchRows` 时也会被唤醒；
- 单次请求最多搬运 **5000 行**，避免请求体过大；
- 启动时执行一次 `SELECT 1` 探测，失败只告警，不阻止启动；
- 失败会退避（`RetrySleepMs` 起，连续失败线性增长，上限 30 秒），**当前批次不会重试**，
  行数计入 `failed`；队列继续接收新行情；
- `written` 只统计 HTTP 返回 `200` / `201` / `204` 的行数；
- `seq` 是进程内自增序号，仅作参考，不保证跨重启连续。

表结构见 `sql/create_tick_table.sql`：

| 列 | 类型 | 来源 |
| --- | --- | --- |
| `trading_day` | `Date` | CTP `TradingDay` |
| `exchange` | `LowCardinality(String)` | 归一化交易所 |
| `instrument` | `String` | CTP `InstrumentID` |
| `update_dt` | `DateTime64(3, 'Asia/Shanghai')` | `ActionDay + UpdateTime + UpdateMillisec` |
| `seq` | `UInt64` | 进程内序号 |
| `pre_settlement_price` … `lower_limit_price` | `Float64` / `UInt64` | 各行情字段，`DBL_MAX` 已转 0 |
| `bid_price_1` / `bid_volume_1` / `ask_price_1` / `ask_volume_1` | `Float64` / `UInt64` | 一档盘口 |

建表语句：

```bash
clickhouse-client --query "CREATE DATABASE IF NOT EXISTS market"
clickhouse-client --multiquery < sql/create_tick_table.sql
```

常用查询：

```sql
-- 当日每个合约最新一笔
SELECT *
FROM market.tick
WHERE trading_day = today()
ORDER BY instrument, update_dt DESC
LIMIT 1 BY instrument;

-- 某合约当日成交量和持仓量走势
SELECT update_dt, latest_price, volume, open_interest
FROM market.tick
WHERE trading_day = today() AND instrument = 'rb2609'
ORDER BY update_dt;
```

### 9.4 本地 CSV 存储（`LocalFile`）

#### 目录与文件命名

`Config` 解析时会把 `DataDir` 自动追加启动时间戳：

```text
<DataDir>_<YYYYMMDDHHMMSS>/
```

例如 `"DataDir": "marketdata"`，在 `2026-10-26 21:12:00` 启动，则实际目录为：

```text
marketdata_20261026211200/
```

目录内文件：

```text
<tick>_<ActionDay>_<NNN>.csv
```

- `tick`：`FilePrefix`；
- `ActionDay`：CTP `ActionDay`（自然日），缺失时依次退回 `TradingDay`、本地日期；
- `NNN`：三位序号，从 `001` 开始，达到 `MaxFileSize` 后递增。

示例：

```text
marketdata_20261026211200/tick_20261026_001.csv
marketdata_20261026211200/tick_20261026_002.csv
```

> 每个文件名对应**一个自然日**；夜盘跨越零点时，`update_dt` 和文件名跟随 `ActionDay`，
> 而 CSV 的 `trading_day` 列仍然保留 CTP 的交易日，两者可能不同，这是刻意设计。

#### CSV 格式

首行表头（每个文件只写一次）：

```csv
trading_day,exchange,instrument,update_dt,update_time,update_millisec,pre_settlement_price,pre_close_price,pre_open_interest,latest_price,open_price,highest_price,lowest_price,volume,turnover,open_interest,close_price,settlement_price,upper_limit_price,lower_limit_price,bid_price_1,bid_volume_1,ask_price_1,ask_volume_1
```

数据行示例：

```csv
20261026,SHFE,rb2609,2026-10-26 09:30:00.125,09:30:00.125,125,3200,3190,120000,3210,3205,3220,3195,15234,488765432,118000,3210,0,3300,3100,3209,12,3211,8
```

- 所有字段为纯 ASCII，数值不添加引号；
- `update_dt` 使用自然日 + `update_time` + 毫秒（三位补零）；
- `update_time` 保留 CTP 的 `HH:MM:SS.mmm` 字符串；
- 只输出一档盘口；
- 浮点数去掉无意义的尾零（`3210` 而不是 `3210.000000`），最多 6 位小数。

#### 滚动、恢复与可靠性

- 单个文件达到 `MaxFileSize` 后关闭并创建下一个序号；
- 写入前按当前文件大小判断是否需要滚动；
- 同一目录内重新创建 writer 时，会从 `seq=1` 开始扫描并续写第一个仍低于大小上限的分片；
  但由于 `DataDir` 每次启动都带新时间戳，**正常重启会进入新目录**，此续写逻辑主要用于进程内重建 writer；
- 追加失败（文件被 Excel / 杀毒软件占用等）会先重试 5 次；仍失败则滚动到新分片继续写；
- I/O 失败时会把文件截断回写入前的大小，避免下次重放产生重复行；
- 运行期间失败批次会整体重排回队列前端，保持顺序，**不丢行**；
- 队列满时 `push()` 最多等待 120 秒；超过后丢弃该行并记录 ERROR；
- 停机时刷新全部剩余行，最多重试 120 次，仍失败计入 `failedRows`；
- **不会自动删除旧文件**：磁盘容量与清理策略由外部负责（cron / 脚本 / 日志轮转）。

#### 容量估算

一行 CSV 大约 200–260 字节。按 250 字节估算：

| 行数 | 约占空间 |
| --- | --- |
| 100 万 | ~250 MB |
| 1000 万 | ~2.5 GB |
| 1 亿 | ~25 GB |

全市场期货一天的行数取决于行情活跃度，建议先用 `broadcast-only` 或小样本观察实际行数再定容量。

### 9.5 三种部署方案

#### 方案 A：本地 CSV（默认，推荐在 ClickHouse 不稳定时使用）

```bash
MarketDataService -c config/MarketDataService.local-file.json
```

- `ClickHouse.Enabled = false`
- `LocalFile.Enabled = true`

#### 方案 B：ClickHouse

```bash
MarketDataService -c config/MarketDataService.json
```

- `ClickHouse.Enabled = true`
- `LocalFile.Enabled = false`（或不写该段）

#### 方案 C：仅广播，不存储

```bash
MarketDataService -c config/MarketDataService.broadcast-only.json
```

- 两个后端都关闭，使用 `NullWriter`；适合只做行情分发、或压测广播链路。

### 9.6 参数调优

高频活跃市场（追求低延迟、少丢行）：

```json
"LocalFile": {
    "Enabled": true,
    "DataDir": "marketdata",
    "FilePrefix": "tick",
    "FlushIntervalMs": 500,
    "MaxQueuedRows": 500000,
    "MaxFileSize": 524288000
}
```

低频 / 夜盘品种（降低磁盘唤醒频率、缩小文件）：

```json
"LocalFile": {
    "Enabled": true,
    "DataDir": "marketdata",
    "FilePrefix": "tick",
    "FlushIntervalMs": 2000,
    "MaxQueuedRows": 100000,
    "MaxFileSize": 10485760
}
```

调优建议：

- `FlushIntervalMs` 越小，落盘越及时、磁盘唤醒越频繁；
- `MaxQueuedRows` 越大，抗抖动能力越强、内存占用越高（每行约数百字节）；
- `MaxFileSize` 影响单文件可读性与滚动频率；CSV 后端没有“最大保留文件数”，请另行清理；
- ClickHouse 侧优先调 `BatchRows` 与 `FlushIntervalMs`，避免单行 INSERT；
- 不要把 `ClickHouse.MaxQueuedRows` 设得小于 `BatchRows`，配置解析会自动抬高。

### 9.7 从旧版存储文档迁移

早期 `README-Storage-Options.md` 描述的方案与当前实现有差异，合并后以此处为准：

| 旧描述 | 当前实现 |
| --- | --- |
| 本地文件为 JSONL / JSONEachRow | **CSV**，带表头 |
| `LocalFile.BatchRows` | 已移除；worker 每次最多取 5000 行 |
| `LocalFile.MaxFiles` | 已移除；不会自动删除旧文件 |
| `LocalFile.Compress` | 已移除；不压缩 |
| 文件 `tick_YYYYMMDD_HHMMSS_MMM.jsonl` | `<prefix>_<ActionDay>_<NNN>.csv` |
| `DataDir` 固定 | 启动时追加 `_YYYYMMDDHHMMSS` |
| 队列满立即丢弃 | CSV 后端会背压等待最多 120 秒 |

迁移路径：

1. 当前建议先用 `config/MarketDataService.local-file.json` 保证行情不丢；
2. ClickHouse 恢复稳定后切回 `config/MarketDataService.json`；
3. 如需把历史 CSV 导入 ClickHouse，注意 **CSV 列与 `market.tick` 表并不一一对应**：
   - CSV 有 `update_time`、`update_millisec`，表中没有；
   - 表有 `seq`，CSV 中没有；
   - CSV 的 `trading_day` 是 `YYYYMMDD`，表要求 `Date`。

   因此不能直接整表导入。请用 `file()` 表函数或 `clickhouse-local` 显式选择并转换列，
   例如（`file()` 读取的是 **ClickHouse 服务端** 上的路径）：

   ```sql
   INSERT INTO market.tick
   SELECT
       toDate(parseDateTime(trading_day, 'YYYYMMDD')) AS trading_day,
       exchange,
       instrument,
       toDateTime64(update_dt, 3, 'Asia/Shanghai') AS update_dt,
       toUInt64(0) AS seq,
       pre_settlement_price, pre_close_price, pre_open_interest,
       latest_price, open_price, highest_price, lowest_price,
       toUInt64(volume) AS volume,
       turnover, open_interest, close_price, settlement_price,
       upper_limit_price, lower_limit_price,
       bid_price_1, toUInt64(bid_volume_1) AS bid_volume_1,
       ask_price_1, toUInt64(ask_volume_1) AS ask_volume_1
   FROM file('/path/on/server/tick_20261026_001.csv', 'CSVWithNames');
   ```

---

## 10. 广播协议

### 10.1 传输

| 方式 | 端口来源 | 分帧 |
| --- | --- | --- |
| TCP 单播 | `Unicast.Port` | 每条报文一行，以 `\r\n` 结尾；客户端按行读取 |
| UDP 组播 | `Multicast.Port` | 每个 tick 一个 UDP 数据报，报文末尾同样带 `\r\n` |

### 10.2 报文格式

单行、无格式化的 JSON，`T` 固定为 `300`：

```json
{"T":300,"Seq":1,"EX":"SHFE","InsID":"rb2609","PreStlPx":3200,"PreClsPx":3190,"PreOI":120000,"Px":3210,"OPx":3205,"HPx":3220,"LPx":3195,"V":15234,"Tov":488765432,"OI":118000,"CPx":0,"SPx":0,"ULPx":3300,"LLPx":3100,"Tm":"09:30:00.125","BPx1":3209,"BV1":12,"APx1":3211,"AV1":8}
```

字段：

| 键 | 含义 | 来源 |
| --- | --- | --- |
| `T` | 消息类型，固定 `300` | 常量 |
| `Seq` | 进程内全局序号，从 1 开始 | Dispatcher |
| `EX` | 归一化交易所 | `ZCE/DCE/SHFE/CFFE/INE/GFEX` |
| `InsID` | 合约代码 | CTP `InstrumentID` |
| `PreStlPx` | 昨结算 | `PreSettlementPrice` |
| `PreClsPx` | 昨收盘 | `PreClosePrice` |
| `PreOI` | 昨持仓 | `PreOpenInterest` |
| `Px` | 最新价 | `LastPrice` |
| `OPx` / `HPx` / `LPx` | 开 / 高 / 低 | `OpenPrice` / `HighestPrice` / `LowestPrice` |
| `V` | 成交量 | `Volume` |
| `Tov` | 成交额 | `Turnover` |
| `OI` | 持仓量 | `OpenInterest` |
| `CPx` / `SPx` | 收盘价 / 结算价 | `ClosePrice` / `SettlementPrice` |
| `ULPx` / `LLPx` | 涨停 / 跌停 | `UpperLimitPrice` / `LowerLimitPrice` |
| `Tm` | 行情时间 `HH:MM:SS.mmm` | `UpdateTime` + `UpdateMillisec` |
| `BPx1` / `BV1` | 买一价 / 买一量 | `BidPrice1` / `BidVolume1` |
| `APx1` / `AV1` | 卖一价 / 卖一量 | `AskPrice1` / `AskVolume1` |

所有价格字段已经过 `doubleMaxTo0()`，不会出现 `DBL_MAX` / `1.7976931348623157e308`。

> 当前版本**只采集和发送一档盘口**；`Broadcast.Levels = 5` 不会追加 `BPx2..APx5` 等键。
> 旧客户端可以继续按 `BPx1/APx1` 解析。

### 10.3 快照与慢客户端

- Dispatcher 为每个 `InsID` 保存最新一条编码后的报文（`Snapshot`）；
- 行情静默超过 `SnapshotIdleSeconds` 后，每秒检查一次并整体重发快照，便于新客户端拿到全市场最新价；
- 单连接发送队列超过 **1000** 条，且超过快照数量的 **3 倍**时触发裁剪：
  - 保留正在发送的一条；
  - 清空其余积压；
  - 用快照中 `Seq` 大于“已发送最大序号”的报文重新填充。
- 裁剪是**尽力而为**的恢复策略，不保证慢客户端不丢中间 tick，但保证其最终能看到每个合约的最新快照。

### 10.4 最小客户端示例

```python
import json
import socket

sock = socket.create_connection(("127.0.0.1", 12345))
buf = b""
while True:
    chunk = sock.recv(65536)
    if not chunk:
        break
    buf += chunk
    while b"\r\n" in buf:
        line, buf = buf.split(b"\r\n", 1)
        if line:
            tick = json.loads(line)
            print(tick["Seq"], tick["EX"], tick["InsID"], tick["Px"], tick["Tm"])
```

组播客户端需要先加入组播组（例如 `239.0.0.3:12345`），并按 UDP 数据报解析；每个数据报是一条完整报文。

---

## 11. 运行期行为

### 11.1 日志

控制台始终输出；`LogFile` 非空时同时追加写入文件。

```text
2026-10-26 21:12:00.125 [INFO ] configuration loaded from [config/MarketDataService.local-file.json]
2026-10-26 21:12:00.130 [INFO ] contract list loaded from openctp url [...]: contracts [870], ignored [0]
2026-10-26 21:12:00.131 [INFO ] source [9999:] owns [870] contracts
2026-10-26 21:12:00.140 [INFO ] unicast server started at [0.0.0.0/12345]
2026-10-26 21:12:00.141 [INFO ] multicast sender started at [239.0.0.3/12345]
2026-10-26 21:12:00.142 [INFO ] csv file writer started, directory [marketdata_20261026211200], ...
2026-10-26 21:12:01.200 [INFO ] source [9999:] market-data login succeeded, trading day [20261026]
2026-10-26 21:12:01.850 [INFO ] source [9999:] subscribed [870] contracts in [3] chunks
```

### 11.2 统计

每 60 秒输出一行 tick 统计和一行 writer 统计：

```text
stats: ticks total [152340], last minute [1523]
stats: local file received [152340] written [152300] dropped [0] failed [0], queue [40]
stats: clickhouse received [152340] written [150000] dropped [2340] failed [0], queue [0]
stats: null received [0] written [0] dropped [0] failed [0], queue [0]
```

- `ticks total`：通过新鲜度检查、真正进入存储链路的 tick 数；
- `last minute`：与上一次统计的差值；
- `received`：writer 接收的 tick 数；
- `written`：成功写入的行数；
- `dropped`：队列溢出 / 无法写入导致的丢弃；
- `failed`：写入失败的行数（ClickHouse 为整批失败；CSV 为停机刷新放弃的行数）；
- `queue`：当前积压行数。

### 11.3 自动退出

本地时间到达配置的时间点后触发优雅停止：

| 配置 | 生效窗口 | 条件 |
| --- | --- | --- |
| `NightExitTime` | `00:00 – 05:59` | 当前时间 ≥ 配置值 |
| `DayExitTime` | `06:00 – 19:59` | 当前时间 ≥ 配置值 |
| 其他时段 | `20:00 – 23:59` | 不触发，等待跨入夜盘窗口 |

这样同一个进程可以覆盖夜盘，不会被日盘退出时间误杀，反之亦然。检查每秒执行一次。

### 11.4 CTP 回调异常隔离

所有 `On*` 回调都在 C++/C ABI 边界内 `try/catch`，异常不会逃逸到 CTP 的 C 代码中
（否则会 `std::terminate`）。异常只记录 ERROR 日志，服务继续运行。

### 11.5 关闭 watchdog

`CThostFtdcMdApi::Release()` 在前置不可达时可能永久阻塞。服务会启动一个 10 秒 watchdog；
超时后记录 ERROR 并 `_Exit(1)`。此时 writer 已经刷盘完毕，因此不会丢失已入队的数据。

---

## 12. 已知限制

| 限制 | 影响 / 规避方式 |
| --- | --- |
| 只采集一档盘口 | `Broadcast.Levels` 实际无效；需要五档需改 CTP 转换与编码逻辑 |
| `AppID` / `AuthCode` 保留但未使用 | 当前 CTP 行情 API 无认证请求 |
| 合约列表不热更新 | 新增合约需重启服务 |
| ClickHouse 失败批次会丢失 | 失败计入 `failed`，不重试该批；对完整性要求高时改用 CSV |
| CSV `DataDir` 每次启动追加时间戳 | 每次运行一个新目录；旧目录不会自动清理，需外部归档 / 删除 |
| `MaxFileSize` 按 `int` 解析 | 超过约 2GiB 会溢出；建议 ≤ 1GiB |
| 仅支持 HTTP | openctp 拉取与 ClickHouse 写入都不支持 HTTPS/TLS |
| 默认按 IPv4 部署 | 示例地址与合约服务均为 IPv4；IPv6 未做专门验证 |
| 无鉴权 | 单播端口对网络开放，生产环境需防火墙 / 内网隔离 |
| 无自动化测试 | 仓库内没有单元测试；依赖构建 + 冒烟测试 |
| Windows 必须 MSVC | CTP 导入库为 MSVC 格式，MinGW 无法链接 |
| CSV 队列满会阻塞 CTP 回调 | 最长 120 秒；磁盘长期不可用会影响行情接收 |

---

## 13. 故障排查

### 13.1 构建失败

**`CTP market-data API headers not found`**

- 检查 `third_party/v6.7.10_20250422_traderapi.zip` 是否存在；
- 或显式传入 `-DCTP_API_DIR=<包含 ThostFtdcMdApi.h 的目录>`；
- 删除构建目录下的 `deps/` 后重新 configure。

**Windows 链接错误 `cannot open input file 'thostmduserapi_se.lib'`**

- 使用了 MinGW；改用 MSVC / Visual Studio 生成工具；
- 或 `CTP_API_DIR` 指向了错误平台（32 位 / Linux）的 SDK。

**Linux `cannot find -lthostmduserapi_se`**

- 确认 `CTP_API_DIR` 下有 `thostmduserapi_se.so`；
- 删除构建目录下的 `deps/lib` 让 CMake 重新建符号链接。

### 13.2 启动失败

**`can not open config file` / `can not parse config file`**

- 服务已 `chdir` 到 exe 目录；`-c` 的相对路径是相对 **exe 目录**，不是 shell 当前目录；
- 用绝对路径最稳妥。

**`no CTPSources defined`**

- `CTPSources` 必须是非空数组。

**`can not obtain the instrument list`**

- openctp URL 不可达且本地缓存不可用；
- 检查网络 / HTTP 代理（当前不支持代理配置）、URL 是否为 `http://`；
- 保留一份可用的 `config/instruments_cache.json` 作为兜底。

**`source [...] owns no instrument and is skipped`**

- 前面的源没有 `ExchangeFilter`，已经认领全部合约；调整源顺序或给前面的源加过滤。

**`no CTP source could be started`**

- 所有源都没有分到合约；检查 `ExchangeFilter` 与合约列表的交易所 ID 是否匹配。

### 13.3 收不到行情

1. 看日志是否出现 `market-data login succeeded`；
2. 看是否出现 `subscribed N contracts in M chunks`；
3. 核对账号是否有行情权限、前置 URI 是否正确（`tcp://host:port`）；
4. 当前代码会丢弃 `Volume <= 0` 的行情，并过滤超过 60 秒的旧行情（只广播不落盘）；
5. 检查本机时间是否准确；时间偏差过大会让所有行情被判定为“不新鲜”；
6. 检查防火墙是否放行单播 / 组播端口。

### 13.4 单播 / 组播收不到

- 单播：确认连接的是 `Unicast.Port`，客户端按 `\r\n` 分行；
- 超过 `MaxConnections` 的新连接会被直接拒绝，日志有 WARN；
- 组播：客户端必须加入同一组播地址；跨网段时检查 `Hops` 与网络设备的组播配置；
- 本机防火墙 / 安全组可能拦截 UDP。

### 13.5 ClickHouse 写入失败

日志：

```text
clickhouse HTTP ping failed [...]
```

依次检查：

1. `Port` 是否为 HTTP 端口 `8123`（不是 `9000`）；
2. 用户名 / 密码；
3. 库表是否存在（执行 `sql/create_tick_table.sql`）；
4. 服务器是否允许来自本机的 HTTP 访问；
5. 看 WARN 中的 ClickHouse 响应头，通常会带具体错误（类型不匹配、权限不足等）。

### 13.6 CSV 文件在哪里

- 先看启动日志 `csv file writer started, directory [...]`；
- 目录名是 `DataDir` + `_YYYYMMDDHHMMSS`；
- 路径相对 **exe 目录**；
- 文件名按 `ActionDay`，不是 `TradingDay`。

### 13.7 与其它行情服务同时运行

- 不要共用同一个 CTP 行情账号；
- `FlowPath` 必须分开，否则 CTP 流文件会冲突；
- 单播 / 组播端口不要重复绑定。

---

## 14. 开发约定

- **语言标准**：C++11；不使用 C++14/17 特性，保持与老工具链兼容。
- **代码风格**：Tab 缩进（显示宽度 4），Allman 大括号，UTF-8 源文件。
- **错误处理**：
  - CTP 回调边界必须 `try/catch`，异常不得穿越 C ABI；
  - 信号处理函数只做原子标志写入，不日志、不加锁、不分配；
  - 后台线程异常要在循环内捕获并记录，避免整个进程退出。
- **线程安全**：
  - `TickWriter::push()` 可从任意 CTP 线程调用；
  - Dispatcher 内部状态只在 io 线程读写；
  - 跨线程共享的计数器使用 `std::atomic`。
- **路径**：所有运行期相对路径以 exe 目录为基准；新增路径配置时保持这一约定。
- **提交前自检**：

  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j
  ```

  并用 `broadcast-only` 配置做一次冒烟测试：启动 → 连接单播 → 收到 `T:300` → `Ctrl+C` 正常退出。

- **依赖升级**：替换 `third_party/*.zip` 后删除构建目录下的 `deps/`，重新 configure；不要提交解包后的 SDK。

---

## 15. 许可证与第三方组件

本项目采用 [MIT License](LICENSE) 开源：

- 可以自由使用、修改、分发，包括商业用途；
- 分发时需保留版权声明和许可证文本；
- 软件按“现状”提供，不附带任何明示或暗示的担保。

第三方组件及其许可证：

| 组件 | 许可证 | 说明 |
| --- | --- | --- |
| cJSON | MIT | `third_party/cJSON.{h,cpp}` |
| standalone Asio | Boost Software License 1.0 | `third_party/asio-asio-1-30-2.zip` |
| CTP 行情 SDK | **专有** | `third_party/v6.7.10_20250422_traderapi.zip` |

> CTP SDK 的版权归上海期货信息技术有限公司所有，**不适用本项目的 MIT 许可证**。
> 仓库保留原始压缩包仅为构建方便；如果没有再分发权，请删除该压缩包并改用
> `-DCTP_API_DIR` 指向本机安装的 SDK。完整的第三方声明见
> [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
>
> 使用本项目、CTP SDK 及所接入行情数据所产生的授权、合规与数据使用责任，由使用者自行承担。

欢迎提交 Issue 和 Pull Request；贡献即表示同意以 MIT 许可证发布，详见
[CONTRIBUTING.md](CONTRIBUTING.md)。

---

## 附：快速检查清单

- [ ] `config/*.json` 中的 CTP 账号、前置、ClickHouse 地址已替换
- [ ] `CTPSources` 至少一项，`MarketDataServerURIs` 非空
- [ ] `config/` 与 exe 同目录，或以绝对路径传入 `-c`
- [ ] ClickHouse 已建库表（若启用）
- [ ] 单播 / 组播端口已放行
- [ ] `FlowPath` 各账号不冲突
- [ ] `LogFile` 目录可写，磁盘容量足够
- [ ] 自动退出时间符合当前交易时段
- [ ] 定期清理旧的 `marketdata_*` 目录和日志
