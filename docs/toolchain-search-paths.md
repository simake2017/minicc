# Linux 工具链搜索路径完全指南（Clang / ld / ld.so）

本文记录 minicc 项目遇到的 `fatal error: 'format' file not found` 问题的完整始末，
并以此为切入点，讲清 Linux 下一个 C++ 程序从**预处理 → 编译 → 链接 → 运行**四个阶段
各自"去哪里找文件"，所有数据均为本机（Debian, Clang 18.1.8, x86-64）实测。

- 读者：被 `file not found`、`undefined symbol`、`error while loading shared libraries` 拦过、
  想弄清"编译器到底去哪儿找文件"的 minicc 开发者
- 审法：每条"去哪里找"的结论都要能指回一条实测命令的输出；每条排查命令都要说明"看到什么算正常"
- 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 按 tech-doc 完整规格骨架补齐 |

---

## 目录

- [第一部分：概要设计（这一块对外承诺什么）](#第一部分概要设计这一块对外承诺什么)
- [第二部分：详细设计（怎么做）](#第二部分详细设计怎么做)
- [一、问题始末：`<format>` 为什么找不到](#一问题始末format-为什么找不到)
- [二、阶段 1：预处理/编译 —— 头文件搜索路径](#二阶段-1预处理编译--头文件搜索路径)
- [三、阶段 2：编译 —— Clang 如何挑选 GCC 工具链](#三阶段-2编译--clang-如何挑选-gcc-工具链)
- [四、阶段 3：链接 —— 库文件搜索路径](#四阶段-3链接--库文件搜索路径)
- [五、阶段 4：运行 —— 动态库解析（ld.so）](#五阶段-4运行--动态库解析ldso)
- [六、CMake 中的修复](#六cmake-中的修复)
- [七、常用排查命令速查](#七常用排查命令速查)

---

## 第一部分：概要设计（这一块对外承诺什么）

### §1 概念

#### 1.1 实体

| 实体 | 是什么 | 在哪些场景起作用 |
|---|---|---|
| **头文件搜索路径** | 一串目录，`#include <xxx>` 按顺序在里面找 | 预处理与编译阶段 |
| **标准库实现** | C++ 标准库的两家主流实现：`libstdc++`（GCC 自带）与 `libc++`（LLVM 自带）；**clang++ 自己不带标准库** | 决定头文件从哪来、链接 `-lstdc++` 还是 `-lc++` |
| **GCC 工具链** | clang 在 Linux 上借用的那一套 GCC 运行时（`libstdc++` 头文件 + CRT 启动文件 + `libgcc`）；clang 启动时探测一次 | 即使切到 libc++，**探测仍然照做**（CRT 与 libgcc 还要从那里取） |
| **库搜索路径**（`-L` 目录列表） | 链接器找 `libxxx.so` / `libxxx.a` 的目录列表 | 链接阶段 |
| **动态链接器**（`ld.so`） | 运行时按 `DT_NEEDED` 重新解析共享库的 `/lib64/ld-linux-x86-64.so.2` | 程序启动时 |
| **运行时缓存**（`/etc/ld.so.cache`） | `ldconfig` 从 `/etc/ld.so.conf` + `/etc/ld.so.conf.d/*.conf` 生成的 soname → 路径映射表 | 动态链接器第 3 优先级 |

#### 1.2 名词

| 名词 | 一句话（大白话） | 在哪出现 |
|---|---|---|
| multiarch | Debian 把 64 位库放在带架构后缀的目录里的布局（`/lib/x86_64-linux-gnu`） | 头文件与库路径 |
| soname | 共享库的"身份证名"（`libc++.so.1`），ELF 里只记这个名字，不记路径 | `DT_NEEDED` |
| `DT_NEEDED` | 可执行文件里"我需要哪些共享库"的清单 | `readelf -d` 输出 |
| `DT_RUNPATH` / `DT_RPATH` | 可执行文件自带的库搜索路径 | 本项目**没有**（实测确认） |
| GCC 探测 | clang 启动时在 `/usr/lib/gcc/$triple/` 下找 GCC 安装目录，取最高版本 | `clang++ -v` 输出开头 |
| CRT 启动文件 | `crti.o` / `crtbeginS.o` / `crtn.o` / `crtendS.o` 等，负责全局构造与进程入口 | 链接阶段 |

### §2 正常流程

**整体流程图**（方框图；每一阶段的"去哪里找"是本主题的主角）：

```
┌────────────────────────────────────────────┐
│ 阶段 1 预处理 / 编译                       │
│ 找什么：头文件（#include <...>）            │
│ 在哪找：-I → -isystem → 标准库 C++ 头 →     │
│         clang 内置头 → 系统默认目录          │
└──────────────────┬─────────────────────────┘
                   │ 顺带做一次 GCC 探测
                   ▼
┌────────────────────────────────────────────┐
│ 阶段 1' GCC 探测                           │
│ 找什么：GCC 安装目录（取最高版本）           │
│ 用来取：libstdc++ 头 / CRT 启动文件 / libgcc │
└──────────────────┬─────────────────────────┘
                   ▼
┌────────────────────────────────────────────┐
│ 阶段 3 链接                                │
│ 找什么：库文件（-lxxx）                     │
│ 在哪找：命令行上的 -L（按顺序）→ ld 内置      │
│         SEARCH_DIR                          │
└──────────────────┬─────────────────────────┘
                   │ 只记 soname 进 DT_NEEDED
                   ▼
┌────────────────────────────────────────────┐
│ 阶段 4 运行                                │
│ 找什么：共享库的实际路径                    │
│ 在哪找：LD_LIBRARY_PATH → DT_RUNPATH →      │
│         /etc/ld.so.cache → /lib、/usr/lib   │
└────────────────────────────────────────────┘
```

**步骤**（每步一个动作）：

1. **预处理/编译找头文件**：按 `-I` → `-isystem` → 标准库 C++ 头 → clang 内置头 → 系统默认目录的顺序找，**先命中谁就用谁**。【为什么】标准库头排在系统 C 头之前，否则 `<string>` 会被系统 C 头抢走。
2. **clang 启动时做一次 GCC 探测**：扫描 `/usr/lib/gcc/$triple/` 等候选目录，取**最高版本**；本机只有一个候选（GCC 10），所以是"没得选"而不是"版本匹配"。
3. **探测结果有三处用途**：libstdc++ 头文件目录、CRT 启动文件目录、`libgcc` 所在目录（进 `-L`）。
4. **链接器按 `-L` 顺序找 `-l` 指定的库**：`-lc++` 先找 `libc++.so` 再找 `libc++.a`；`-L` 只对**排在它后面**的 `-l` 生效；**库顺序敏感**，被依赖者排后面。
5. **`-L` 全落空还有兜底**：ld 编译进二进制的 `SEARCH_DIR`（`ld --verbose` 可查）。
6. **链接只把库名记进 ELF**：写 `DT_NEEDED`（**只记 soname，不记路径**）。
7. **运行时由 `ld.so` 重新解析**：`LD_LIBRARY_PATH` → `DT_RUNPATH`/`DT_RPATH` → `/etc/ld.so.cache` → `/lib`、`/usr/lib`。
8. **`ld.so.cache` 是 `ldconfig` 生成的**：来源是 `/etc/ld.so.conf` 与 `/etc/ld.so.conf.d/*.conf`；apt 装包会自动跑 `ldconfig`，**手动拷 `.so` 进 `/usr/local/lib` 则必须自己跑一次**。

### §2.5 更新对照

本主题**有写契约**：改 `CMakeLists.txt` 里的开关会同时改掉"编译期去哪儿找头文件"与"链接期去哪儿找库"。

改前（只有语言标准开关，标准库仍是系统 libstdc++）：

```jsonc
{
  "编译期搜索路径": [
    "/usr/include/c++/10",                              // GCC 10 的 libstdc++ 头（没有 <format>）
    "/usr/include/x86_64-linux-gnu/c++/10",
    "/usr/include/c++/10/backward",
    "/usr/lib/llvm-18/lib/clang/18/include",
    "/usr/local/include",
    "/usr/include/x86_64-linux-gnu",
    "/usr/include"
  ],
  "链接期库": ["-lstdc++"],   // ← 改：切到 libc++ 后变成 -lc++
  "来源": "set(CMAKE_CXX_STANDARD 20)（无 -stdlib 开关）"
}
```

改后（加上 `-stdlib=libc++`，编译与链接两边都加）：

```jsonc
{
  "编译期搜索路径": [
    "/usr/lib/llvm-18/include/c++/v1",                 // ← 改：libstdc++ 那三条整组被一条 c++/v1 替换
    "/usr/lib/llvm-18/lib/clang/18/include",
    "/usr/local/include",
    "/usr/include/x86_64-linux-gnu",
    "/usr/include"
  ],
  "链接期库": ["-lc++"],     // ← 改
  "来源": "set(CMAKE_CXX_STANDARD 20) + add_compile_options(-stdlib=libc++) + add_link_options(-stdlib=libc++)"
}
```

★ 判据：**`-stdlib=libc++` 必须编译、链接两边都加**——只加编译侧，链接期仍会去找 `libstdc++.so`。

### §3 异常处理

| # | 异常（现象） | 如何处理 | 可重试 |
|---|---|---|---|
| 1 | `src/type.cpp:6:10: fatal error: 'format' file not found`（多个源文件同时报同一错） | 根因是**标准库版本太老**：本机 libstdc++ 来自 GCC 10，没有 `<format>`（该头文件要 GCC 13+）。装 `libc++-18-dev` + `libc++abi-18-dev`，再按 §2.5 加上 `-stdlib=libc++`（两边都加） | 否——缺的是软件包，改完要重新配置并编译 |
| 2 | 头文件找到了，但报 `error: no member named 'format' in namespace 'std'` | **语言标准**开关没开：clang 18 默认是 gnu++17，而 `std::format` 是 C++20 特性。补 `-std=c++20` | 否 |
| 3 | 链接期报找不到库（`cannot find -lxxx` 一类） | `-L` 路径不对，或 `-l` 的顺序不对（被依赖者要排在后面）；`-L` 只对排在它后面的 `-l` 生效 | 否 |
| 4 | 运行时报 `error while loading shared libraries: ... cannot open shared object file` | `ld.so` 没解析到：查 `ldd` 看解析到哪一步；若是手动把 `.so` 拷进 `/usr/local/lib` 造成的，跑一次 `sudo ldconfig` 重建 `/etc/ld.so.cache` | 否——缓存没重建 |
| 5 | `ldconfig` 跑过了还是找不到 | 该库不在 `/etc/ld.so.conf` 与 `/etc/ld.so.conf.d/*.conf` 覆盖的目录里；改用 `LD_LIBRARY_PATH` 指到绝对路径 | 否 |

★ 本主题**没有自己的日志键**：错误全部来自 clang / ld / ld.so 三个外部工具，
排查入口就是 §5 的命令表。

### §4 Out of Scope

- **静态链接**（`-static`、`-static-libstdc++`）：本文只讲动态链接路径；静态链接不经过 `ld.so` 这一段。
- **交叉编译与 sysroot**：`--sysroot` 会改写 `SEARCH_DIR` 里那个 `=` 前缀的含义，但本文的实测数据都在默认 sysroot（`/`）下取得。
- **包管理生态**（pkg-config、`find_package`、vcpkg、conan）：头文件与库的"发现方式"不在本文范围。
- **macOS 的 `dyld` 与 `@rpath`**：另一份文档 `cpp-compilation-lifecycle/tutorial.md` 讲 Mach-O 侧。
- **运行时插桩工具对库搜索的影响**（ASan、覆盖率插桩）：会额外拉进运行库，本文未覆盖。
- **Clang 以外的编译器**（GCC 直接编译、MSVC）：本文的探测结论是 clang++-18 在 Linux 上的行为。

### §5 观测点

下表是排查时真正会敲的命令，以及**看到什么算正常**。

| 时机 | 命令 | 看到什么算正常 |
|---|---|---|
| 头文件找不到时 | `clang++ -E -x c++ -v /dev/null` | 打印 `#include <...> search starts here:` 到 `End of search list.`；标准库那几条是不是 `c++/v1`（libc++）还是 `c++/10`（libstdc++）一眼可辨 |
| 想看完整编译与链接命令 | `clang++ -v a.cpp -o a` | 打印 GCC 探测结果 + 完整 `ld` 命令行 |
| 想看 GCC 探测结果 | 同上，看输出开头 | `Found candidate GCC installation:` 与 `Selected GCC installation:` 两行 |
| 想看驱动级搜索目录 | `clang++ -print-search-dirs` | `programs:` 与 `libraries:` 两段 |
| 想看 ld 内置兜底目录 | `ld --verbose \| grep SEARCH_DIR` | 一串 `SEARCH_DIR("=...")` |
| 想看可执行文件记了哪些库 | `readelf -d minicc \| grep -E "NEEDED\|RUNPATH"` | 一串 `(NEEDED) Shared library: [...]`；本项目实测**无 RUNPATH** |
| 想看运行时到底解析到哪 | `ldd minicc` | 每行 `libxxx.so.1 => /绝对路径`；出现 `not found` 就是没解析到 |
| 想看运行时搜索配置 | `cat /etc/ld.so.conf.d/*.conf` | 一串目录（`/usr/local/lib`、`/lib/x86_64-linux-gnu` …） |
| 手动装了库之后 | `sudo ldconfig` | 无输出即成功；之后 `ldd` 应能解析到 |
| 想知道某个头属于哪个包 | `dpkg -S /usr/include/c++/v1/format` | 打印包名（Debian） |

---

## 第二部分：详细设计（怎么做）

### §6 整体流程图

分解视角：把"去哪里找文件"拆成四个查找子流程，每个子流程有**自己的查找列表**和**自己的失败出口**。

```
┌────────────────────────────┐
│ 头文件查找子流程             │
│ 输入：#include <xxx>        │
│ 列表：-I → -isystem → 标准库 │
│       → clang 内置 → 系统默认│
│ 失败出口：file not found     │
└─────────────┬──────────────┘
              │ 同时产出：GCC 探测结果
              ▼
┌────────────────────────────┐
│ GCC 工具链探测子流程         │
│ 列表：--gcc-toolchain →      │
│       --gcc-install-dir →    │
│       自动扫描候选（取最高版）│
│ 产出：libstdc++ 头目录 /     │
│       CRT 启动文件 / libgcc  │
└─────────────┬──────────────┘
              ▼
┌────────────────────────────┐
│ 库文件查找子流程             │
│ 输入：-lxxx                 │
│ 列表：命令行 -L（按顺序）→   │
│       ld 内置 SEARCH_DIR     │
│ 失败出口：cannot find -lxxx  │
└─────────────┬──────────────┘
              │ 产出：ELF 里的 DT_NEEDED（只有 soname）
              ▼
┌────────────────────────────┐
│ 运行时解析子流程             │
│ 输入：DT_NEEDED 里的 soname  │
│ 列表：LD_LIBRARY_PATH →      │
│       DT_RUNPATH/RPATH →    │
│       /etc/ld.so.cache →     │
│       /lib、/usr/lib         │
│ 失败出口：error while loading│
│       shared libraries       │
└────────────────────────────┘
```

★ 与 §2 的差别：§2 是**阶段视角**（四个阶段各找什么）；
这里是**查找列表视角**（每个列表的顺序、来源与失败出口）。

### §7 子流程 + 说人话步骤

#### 7.1 头文件查找子流程

```
→ 遇到 #include <xxx>
   → 按顺序走列表
      ├─ -I 目录（命令行顺序）
      ├─ -isystem 目录
      ├─ 标准库 C++ 头目录（libstdc++ 或 libc++）
      ├─ clang 内置头目录（stddef.h / stdarg.h …）
      └─ 系统默认目录（/usr/local/include → multiarch → /usr/include）
         → 命中即停
```

1. **双引号形式先特殊对待**：`#include "x"` 会**先找当前文件所在目录**，这是它与尖括号形式的唯一差别。
2. **`-I` 目录按命令行先后排**：`clang++ -I a -I b` 里 `a` 优先于 `b`。
3. **`-isystem` 目录排在 `-I` 之后**：CMake 的 `include_directories(SYSTEM ...)` 翻译成 `-isystem`；系统目录用 `-isystem` 是为了让它的警告被压掉。
4. **标准库 C++ 头排在系统 C 头之前**。【为什么】否则 `<string>` 之类会被 `/usr/include` 里的 C 头抢走。
5. **命中即停**：第一个找到的就是它，**不会**继续往后找同名文件。

#### 7.2 GCC 工具链探测子流程

```
→ clang++ 启动
   → 决定用哪套 GCC
      ├─ --gcc-toolchain=<dir>          （显式，最高优先）
      ├─ --gcc-install-dir=<dir>         （Clang 16+，直接指到版本目录）
      └─ 自动扫描候选目录 → 取最高版本
         → 记下三处用途：libstdc++ 头 / CRT 启动文件 / libgcc
```

1. **clang 自身不带 libstdc++ 也不带 CRT**，必须借一套 GCC 的，所以启动时先探测。
2. **探测在 `-v` 输出最开头**，能看到 `Found candidate` 与 `Selected` 两行。
3. **候选目录取最高版本**：本机 `/usr/lib/gcc/x86_64-linux-gnu/` 下只有 `10`，所以"没得选"——Clang 18 配 GCC 10 是唯一候选的结果，不是版本匹配。
4. **探测结果只被用在三处**：libstdc++ 头文件目录、CRT 启动文件（`crtbeginS.o` / `crtendS.o`）、`libgcc` 所在目录。
5. **切到 libc++ 之后探测照做**。【为什么】CRT 与 libgcc 仍然要从 GCC 那儿取。

#### 7.3 库文件查找子流程

```
→ 链接器收到 -lxxx
   → 先扫命令行上的 -L（按先后）
      → 找 libxxx.so，再找 libxxx.a
   → 全落空 → ld 内置 SEARCH_DIR
      → 仍落空 → 报 cannot find -lxxx
```

1. **`-lc++` 的解析**：在 `-L` 目录里**按顺序**找 `libc++.so`，找不到再找 `libc++.a`。
2. **`-L` 只对它后面出现的 `-l` 生效**。【为什么】`-L` 是位置相关的命令行参数，不是全局设置。
3. **库顺序敏感**：被依赖者排后面（`-lc` 几乎总是最后一个）。
4. **`-L` 全落空还有 ld 的内置兜底**：`ld --verbose | grep SEARCH_DIR` 能看到（开头那个 `=` 表示相对于 `--sysroot`，默认即 `/`）。

#### 7.4 运行时解析子流程

```
→ 进程启动，内核把动态链接器 ld.so 拉起来
   → ld.so 读可执行文件的 DT_NEEDED（只有 soname）
      → 按顺序解析
         ├─ LD_LIBRARY_PATH（setuid 程序忽略）
         ├─ DT_RUNPATH / DT_RPATH
         ├─ /etc/ld.so.cache
         └─ /lib、/usr/lib
```

1. **链接期只记名字**：ELF 的 `DT_NEEDED` 里写的是 `libc++.so.1` 这种 **soname**，不写路径。【为什么】绝对路径会把库钉死在某一台机器上，soname 才能让运行时按环境解析。
2. **`ld.so` 按四级顺序解析**：`LD_LIBRARY_PATH` → `DT_RUNPATH`/`DT_RPATH` → `/etc/ld.so.cache` → 默认目录。
3. **`/etc/ld.so.cache` 是 `ldconfig` 生成的**：来源是 `/etc/ld.so.conf` 与 `/etc/ld.so.conf.d/*.conf` 里的目录列表。
4. **apt 装包会自动跑 `ldconfig`**，所以装 `libc++-18-dev` 之后不用管；**手动把 `.so` 拷进 `/usr/local/lib` 则必须自己跑一次 `sudo ldconfig`**。
5. **本项目实测**：所有库都命中 multiarch 目录 `/lib/x86_64-linux-gnu/`，且**没有 RUNPATH**。

### §8 异常流转表

| 异常 | 触发点 | 状态/字段变化 | 恢复路径 |
|---|---|---|---|
| `fatal error: 'format' file not found` | 头文件查找子流程第 5 步，标准库 C++ 头目录里没有 `format` | 编译中止，`.o` 不生成；构建目录里留下上一次的成功产物 | 装 `libc++-18-dev` + `libc++abi-18-dev`；改 `CMakeLists.txt` 加 `-stdlib=libc++`（编译、链接两边）；重跑配置与构建 |
| `no member named 'format' in namespace 'std'` | 头文件找到了，但语言标准版本把该特性关掉了 | 同上，编译中止 | 补 `-std=c++20`（`set(CMAKE_CXX_STANDARD 20)`） |
| 链接期找不到库 | 库文件查找子流程第 4 步 | `.o` 都齐了，但可执行文件**不生成** | 修 `-L` 路径；调整 `-l` 顺序（被依赖者排后面）；必要时查 `ld --verbose \| grep SEARCH_DIR` 看兜底目录 |
| 运行时 `error while loading shared libraries` | 运行时解析子流程第 2 步，某一级都没解析到 | 进程根本没起来；磁盘上的可执行文件是好的 | 若是手动装库，跑 `sudo ldconfig`；否则查 `ldd` 看断在哪一级 |
| 手动装 `.so` 后 `ldd` 仍 `not found` | 运行时解析子流程第 3 步，缓存里没有新库 | `/etc/ld.so.cache` 内容过时 | 重跑 `sudo ldconfig`；或临时用 `LD_LIBRARY_PATH` 指到绝对路径 |

### §9 持久状态 / 空值语义

本主题**没有状态机**（四个阶段是一次次独立跑的过程），只有三处持久状态。

```jsonc
{
  "/etc/ld.so.conf": {
    "//": "ldconfig 的主配置文件，内容是 include 指令与目录列表",
    "包含": "/etc/ld.so.conf.d/*.conf"
  },
  "/etc/ld.so.conf.d/*.conf": {
    "//": "本机实测的目录列表（ldconfig 据此生成缓存）",
    "目录": [
      "/usr/lib/x86_64-linux-gnu/libfakeroot",
      "/usr/local/lib",
      "/usr/local/lib/x86_64-linux-gnu",
      "/lib/x86_64-linux-gnu",
      "/usr/lib/x86_64-linux-gnu",
      "/lib32",
      "/usr/lib32"
    ]
  },
  "/etc/ld.so.cache": {
    "//": "ldconfig 生成的二进制映射表：soname → 绝对路径",
    "生成者": "ldconfig（apt 装包时自动跑；手动装 .so 需手动跑）",
    "过期后果": "新装的共享库在运行时 not found"
  },
  "可执行文件的 ELF 动态段": {
    "DT_NEEDED": ["libc++.so.1", "libc++abi.so.1", "libunwind.so.1", "libm.so.6", "libgcc_s.so.1", "libc.so.6"],
    "DT_RUNPATH": null,   // null = 本项目没有这一项（实测），不是"有但为空"
    "DT_RPATH": null      // null = 同上
  }
}
```

**空值语义**：

| 空值 | 含义 |
|---|---|
| `DT_RUNPATH` 为 null | 可执行文件**没有**自带库搜索路径，运行时完全依赖 `LD_LIBRARY_PATH` 与 `ld.so.cache` |
| `LD_LIBRARY_PATH` 未设置 | 跳过第 1 优先级，直接进 `DT_RUNPATH`/`DT_RPATH` |
| `ld.so.cache` 里查不到某 soname | 该库不在缓存覆盖的目录里；不是"缓存为空"，是"这条记录没有" |
| `/etc/ld.so.conf.d/*.conf` 目录列表为空 | `ldconfig` 只会写默认目录进去；此时装到 `/usr/local/lib` 的库必然漏 |

### §10 数据形状

三张列表的数据形状（本主题的全部"状态"就是这三张表）：

```jsonc
{
  "编译期头文件搜索列表": {
    "//": "顺序即优先级，先命中谁用谁",
    "条目": [
      { "来源": "-I", "目录": "/usr/include/c++/10", "备注": "GCC 10 的 libstdc++" },
      { "来源": "-isystem", "目录": "/usr/lib/llvm-18/include/c++/v1", "备注": "libc++（加了 -stdlib=libc++ 才有）" },
      { "来源": "clang 内置", "目录": "/usr/lib/llvm-18/lib/clang/18/include", "备注": "stddef.h / stdarg.h 等" },
      { "来源": "系统默认", "目录": "/usr/local/include", "备注": "" },
      { "来源": "系统默认", "目录": "/usr/include/x86_64-linux-gnu", "备注": "multiarch" },
      { "来源": "系统默认", "目录": "/usr/include", "备注": "兜底" }
    ]
  },
  "链接期 -L 列表": {
    "//": "-L 只对它后面出现的 -l 生效；库顺序敏感",
    "条目": [
      { "目录": "/usr/bin/../lib/gcc/x86_64-linux-gnu/10", "来源": "GCC 探测结果" },
      { "目录": "/lib/x86_64-linux-gnu", "来源": "系统默认" },
      { "目录": "/usr/lib/llvm-18/bin/../lib", "来源": "clang 驱动自带" }
    ],
    "要链接的库": ["-lc++", "-lm", "-lgcc_s", "-lgcc", "-lc", "-lgcc_s", "-lgcc"],
    "CRT 位置": {
      "开头": ["/lib/x86_64-linux-gnu/Scrt1.o", "/lib/x86_64-linux-gnu/crti.o", "/usr/lib/gcc/x86_64-linux-gnu/10/crtbeginS.o"],
      "结尾": ["/usr/lib/gcc/x86_64-linux-gnu/10/crtendS.o", "/lib/x86_64-linux-gnu/crtn.o"]
    }
  },
  "运行时解析列表": {
    "//": "四级顺序",
    "条目": [
      { "级别": 1, "来源": "LD_LIBRARY_PATH", "备注": "setuid 程序忽略" },
      { "级别": 2, "来源": "DT_RUNPATH / DT_RPATH", "备注": "本项目为 null" },
      { "级别": 3, "来源": "/etc/ld.so.cache", "备注": "ldconfig 生成" },
      { "级别": 4, "来源": "/lib、/usr/lib", "备注": "ld 内置默认" }
    ]
  }
}
```

### §11 接口契约

本主题的外部接口是**编译器/链接器/运行时的命令行开关与环境变量**。

**编译与链接开关**

| 开关 | 影响哪一步 | 契约 |
|---|---|---|
| `-I<dir>` | 头文件查找（第 1 优先级） | 追加到列表**前面**，按命令行顺序 |
| `-isystem <dir>` | 头文件查找（第 2 优先级） | 追加到 `-I` 之后；该目录的警告被压制 |
| `-stdlib=libc++` | 头文件查找 + 库查找 | **编译与链接两边都要加**；整组替换 libstdc++ 路径 |
| `-std=c++20` | 语言特性开关 | 与 `-stdlib` 是**两个独立开关**，都要开 |
| `--gcc-toolchain=<dir>` | GCC 探测 | 最高优先级地指定借哪套 GCC |
| `--gcc-install-dir=<dir>` | GCC 探测 | Clang 16+ 支持，直接指到版本目录 |
| `-L<dir>` | 库查找 | **只对它后面出现的 `-l` 生效** |
| `-l<name>` | 库查找 | 先找 `lib<name>.so` 再找 `lib<name>.a`；顺序敏感 |

**运行时环境变量与命令**

| 名字 | 影响哪一步 | 契约 |
|---|---|---|
| `LD_LIBRARY_PATH` | 运行时解析（第 1 优先级） | setuid 程序忽略；临时验证用，勿长期依赖 |
| `/etc/ld.so.conf.d/*.conf` | 运行时解析（第 3 级的数据源） | 改完必须 `sudo ldconfig` 才生效 |
| `sudo ldconfig` | 重建 `/etc/ld.so.cache` | 无输出即成功 |

**边界（谁说了算）**：

- **头文件路径由编译命令说了算**（`-I` / `-isystem` / 标准库开关），不来自 `CMakeCache.txt` 之外的地方。
- **库路径由链接命令说了算**（`-L` / `-l` / `-stdlib`），运行时的解析由**运行环境**说了算（`LD_LIBRARY_PATH` / `ld.so.cache`）——同一份可执行文件，在两台机器上可能解析到不同的库。

### §12 否决记录

| 被否方案 | 否决原因 | 谁否的 / 什么时候 |
|---|---|---|
| 升级系统 GCC 到 13+ 以获得 `<format>` | 会牵动整条工具链；而且 Debian 11 的包源里没有现成的新版 GCC。改用 clang 自带的 libc++ 只是换一个头文件与库的来源，不动系统编译器 | 本项目实际选型（正文「修复」一节） |
| 只在编译期加 `-stdlib=libc++` | 链接期仍会去找 `libstdc++.so`，头文件与库不是同一套标准库实现 | 正文「要点」一节 |
| 条件里只写 `if(UNIX)` | macOS 分支已经单独处理（Homebrew llvm 自带 `c++/v1`），重复加会打架，所以必须带 `NOT APPLE` | 正文「要点」一节 |
| 把头文件路径硬编码进 CMakeLists | 换机器/换发行版就失效；`include_directories(SYSTEM ...)` 至少还带 `if(EXISTS)` 保护 | 正文 `CMakeLists.txt` 片段 |

---

## 一、问题始末：`<format>` 为什么找不到

### 现象

```text
src/type.cpp:6:10: fatal error: 'format' file not found
    6 | #include <format>
```

`lexer.cpp`、`parser.cpp`、`codegen.cpp` 等全部源文件同时报同一错误。

### 根因链条

| # | 事实 | 说明 |
|---|------|------|
| 1 | 编译器是 Clang 18.1.8 | 本身完全支持 C++20 |
| 2 | Clang **不自带** C++ 标准库 | Linux 上默认借用 GCC 的 libstdc++ |
| 3 | 系统唯一的 libstdc++ 来自 GCC 10.2.1 | Debian 11 自带 |
| 4 | `<format>` 需要 **GCC 13+** 的 libstdc++ | GCC 10 的标准库里根本没有这个头文件 |
| 5 | Debian 的 clang 包**不依赖** libc++ | `libc++-18-dev` 是独立包，默认不装 |

结论：不是代码问题，是**标准库版本太老 + 备选标准库未安装**。

### 修复

1. 安装 LLVM 自家标准库（头文件 + 静态库）：

   ```bash
   sudo apt install libc++-18-dev libc++abi-18-dev
   ```

   安装后头文件落在 `/usr/lib/llvm-18/include/c++/v1/`（`format`、`__format/` 都在其中）。

2. CMakeLists.txt 增加 Linux + Clang 分支（见[第六节](#六cmake-中的修复)）。

3. 重新 `cmake .. && make`，构建通过。

### 另一个容易踩的坑：语言标准

独立命令行直接编译时，Clang 18 默认标准是 **gnu++17**，而 `std::format` 是 C++20 特性：

```bash
$ clang++ -stdlib=libc++ fmt_probe.cpp
error: no member named 'format' in namespace 'std'   # 头文件找到了，但特性被标准版本关掉

$ clang++ -std=c++20 -stdlib=libc++ fmt_probe.cpp    # OK
```

所以完整开关是两个：`-std=c++20`（开特性）+ `-stdlib=libc++`（换标准库）。
项目里前者由 `set(CMAKE_CXX_STANDARD 20)` 提供。

---

## 二、阶段 1：预处理/编译 —— 头文件搜索路径

### 搜索顺序（`#include <...>`）

| 优先级 | 来源 |
|--------|------|
| 1 | `#include "x"`：当前文件所在目录（仅双引号形式） |
| 2 | `-I` 目录，按命令行顺序 |
| 3 | `-isystem` 目录（CMake 的 `include_directories(SYSTEM ...)`） |
| 4 | **标准库 C++ 头**（libstdc++ 或 libc++，见下） |
| 5 | Clang 内置头（`stddef.h`、`stdarg.h`、`__clang_hip_libdevice_declares.h` 等） |
| 6 | 系统默认：`/usr/local/include` → multiarch → `/usr/include` |

先命中谁就用谁，因此标准库头排在系统 C 头之前。

### 实测 A：默认（libstdc++）

`clang++ -E -x c++ -v /dev/null`：

```text
#include <...> search starts here:
 /usr/bin/../lib/gcc/x86_64-linux-gnu/10/../../../../include/c++/10
 /usr/bin/../lib/gcc/x86_64-linux-gnu/10/../../../../include/x86_64-linux-gnu/c++/10
 /usr/bin/../lib/gcc/x86_64-linux-gnu/10/../../../../include/c++/10/backward
 /usr/lib/llvm-18/lib/clang/18/include
 /usr/local/include
 /usr/include/x86_64-linux-gnu
 /usr/include
End of search list.
```

归一化后前三条就是 `/usr/include/c++/10{,/<triple>,/backward}` —— GCC 10 的 libstdc++，
**没有 `<format>`，报错就从这里来**。

### 实测 B：`-stdlib=libc++`

```text
#include <...> search starts here:
 /usr/lib/llvm-18/bin/../include/c++/v1
 /usr/lib/llvm-18/lib/clang/18/include
 /usr/local/include
 /usr/include/x86_64-linux-gnu
 /usr/include
End of search list.
```

三条 libstdc++ 路径被整组替换成一条 `c++/v1`（`libc++-18-dev` 提供）。

---

## 三、阶段 2：编译 —— Clang 如何挑选 GCC 工具链

Clang 自身不带 libstdc++ 和 CRT 启动文件，启动时会做一次 **GCC 探测**。
`clang++ -v` 开头就会打印探测结果（本机实测）：

```text
Found candidate GCC installation: /usr/bin/../lib/gcc/x86_64-linux-gnu/10
Selected GCC installation: /usr/bin/../lib/gcc/x86_64-linux-gnu/10
Candidate multilib: .;@m64
Selected multilib: .;@m64
```

### 挑选规则（按优先级）

1. `--gcc-toolchain=<dir>` 显式指定
2. `--gcc-install-dir=<dir>`（Clang 16+，直接指到版本目录）
3. 自动扫描候选目录：`/usr/lib/gcc/$triple/`、`/usr/lib/gcc/`、PATH 中 gcc 的位置
4. 候选里取**最高版本**

本机 `/usr/lib/gcc/x86_64-linux-gnu/` 下只有 `10`，所以"没得选"——
这就是 Clang 18 配 GCC 10 的原因：不是版本匹配，是唯一候选。

### 探测结果的三个用途

| 用途 | 落点 |
|------|------|
| libstdc++ 头文件 | `$GCC/include/c++/10`（第二节的第 4 优先级） |
| CRT 启动文件 | `$GCC/crtbeginS.o`、`$GCC/crtendS.o`（第四节链接用） |
| GCC 运行时库 | `libgcc.a` / `libgcc_s.so` 所在目录（进 `-L`） |

> 即使加了 `-stdlib=libc++`，GCC 探测**仍然进行**——CRT 文件和 libgcc 还要从那里取。

---

## 四、阶段 3：链接 —— 库文件搜索路径

`clang++` 是驱动，编译出 `.o` 后拼一条 `ld` 命令交给 GNU ld。
实测（`clang++ -std=c++20 -stdlib=libc++ -v fmt_probe.cpp -o fmt_probe`）提取的链接要素：

### 链接输入（按 ld 命令行顺序）

```text
/lib64/ld-linux-x86-64.so.2              ← 指定的动态链接器（写进 ELF 的 PT_INTERP）
/lib/x86_64-linux-gnu/Scrt1.o            ← libc 的 _start 入口（PIE 版）
/lib/x86_64-linux-gnu/crti.o             ← 全局构造/析构 prologue
/usr/lib/gcc/x86_64-linux-gnu/10/crtbeginS.o   ← 来自 GCC 探测结果
-L/usr/bin/../lib/gcc/x86_64-linux-gnu/10      ← ┐
-L.../lib64                                    │
-L/lib/x86_64-linux-gnu                        │
-L/lib/../lib64                                ├ -L 库搜索目录（按此顺序）
-L/usr/lib/x86_64-linux-gnu                    │
-L/usr/lib/../lib64                            │
-L/usr/lib/llvm-18/bin/../lib                  │
-L/lib                                         │
-L/usr/lib                                     ← ┘
-lc++ -lm -lgcc_s -lgcc -lc -lgcc_s -lgcc      ← 要链接的库（顺序敏感）
/usr/lib/gcc/x86_64-linux-gnu/10/crtendS.o     ← 来自 GCC 探测结果
/lib/x86_64-linux-gnu/crtn.o                   ← epilogue
```

### `-L` 与 `-l` 的解析规则

- `-lc++` → 在 `-L` 目录里**按顺序**找 `libc++.so`，找不到再找 `libc++.a`
- `-L` 只对命令行中**排在它后面**的 `-l` 生效
- 库顺序敏感：被依赖者排后面（`-lc` 几乎总是最后）

### ld 的内置兜底目录

`-L` 全部落空后，ld 还有编译进二进制的默认路径（`ld --verbose | grep SEARCH_DIR`，实测）：

```text
SEARCH_DIR("=/usr/local/lib/x86_64-linux-gnu")
SEARCH_DIR("=/lib/x86_64-linux-gnu")
SEARCH_DIR("=/usr/lib/x86_64-linux-gnu")
SEARCH_DIR("=/usr/lib/x86_64-linux-gnu64")
SEARCH_DIR("=/usr/local/lib64")
SEARCH_DIR("=/lib64")
SEARCH_DIR("=/usr/lib64")
SEARCH_DIR("=/usr/local/lib")
SEARCH_DIR("=/lib")
SEARCH_DIR("=/usr/lib")
SEARCH_DIR("=/usr/x86_64-linux-gnu/lib64")
SEARCH_DIR("=/usr/x86_64-linux-gnu/lib")
```

（开头的 `=` 表示相对于 `--sysroot`，默认即 `/`。）

### Clang 驱动自身的搜索目录

`clang++ -print-search-dirs`（实测）：

```text
programs: =/usr/bin:/usr/lib/llvm-18/bin:/usr/bin/../lib/gcc/x86_64-linux-gnu/10/../../../../x86_64-linux-gnu/bin
libraries: =/usr/lib/llvm-18/lib/clang/18:/usr/bin/../lib/gcc/x86_64-linux-gnu/10:
           =/usr/bin/../lib/gcc/x86_64-linux-gnu/10/../../../../lib64:/lib/x86_64-linux-gnu:
           /lib/../lib64:/usr/lib/x86_64-linux-gnu:/usr/lib/../lib64:
           /usr/lib/llvm-18/bin/../lib:/lib:/usr/lib
```

---

## 五、阶段 4：运行 —— 动态库解析（ld.so）

链接期只把 `-lc++` 记成 ELF 的 `DT_NEEDED`（**只记 soname，不记路径**），
运行时由动态链接器 `/lib64/ld-linux-x86-64.so.2` 重新解析。

### 实测：minicc 的 NEEDED 项

`readelf -d cmake-build-debug/minicc`：

```text
 (NEEDED)  Shared library: [libc++.so.1]
 (NEEDED)  Shared library: [libc++abi.so.1]
 (NEEDED)  Shared library: [libunwind.so.1]
 (NEEDED)  Shared library: [libm.so.6]
 (NEEDED)  Shared library: [libgcc_s.so.1]
 (NEEDED)  Shared library: [libc.so.6]
```

### 解析顺序

| 优先级 | 来源 |
|--------|------|
| 1 | `LD_LIBRARY_PATH`（setuid 程序忽略） |
| 2 | ELF 的 `DT_RUNPATH` / `DT_RPATH`（本项目无，见下） |
| 3 | `/etc/ld.so.cache`（由 `ldconfig` 从下面的配置生成） |
| 4 | 默认目录 `/lib`、`/usr/lib` |

本机的 cache 来源（`/etc/ld.so.conf` + `/etc/ld.so.conf.d/*.conf`，实测）：

```text
/usr/lib/x86_64-linux-gnu/libfakeroot
/usr/local/lib
/usr/local/lib/x86_64-linux-gnu
/lib/x86_64-linux-gnu
/usr/lib/x86_64-linux-gnu
/lib32
/usr/lib32
```

### 实测：minicc 的最终解析结果

`ldd cmake-build-debug/minicc`：

```text
linux-vdso.so.1 (0x00007ffdf8d21000)
libc++.so.1     => /lib/x86_64-linux-gnu/libc++.so.1
libc++abi.so.1  => /lib/x86_64-linux-gnu/libc++abi.so.1
libunwind.so.1  => /lib/x86_64-linux-gnu/libunwind.so.1
libm.so.6       => /lib/x86_64-linux-gnu/libm.so.6
libgcc_s.so.1   => /lib/x86_64-linux-gnu/libgcc_s.so.1
libc.so.6       => /lib/x86_64-linux-gnu/libc.so.6
libpthread.so.0 => /lib/x86_64-linux-gnu/libpthread.so.0
libdl.so.2      => /lib/x86_64-linux-gnu/libdl.so.2
/lib64/ld-linux-x86-64.so.2
```

全部命中 multiarch 目录，无 RUNPATH——所以**换了标准库记得 `ldconfig` 不用管
（apt 装包会自动跑），但手动装 .so 到 `/usr/local/lib` 后必须跑一次**。

---

## 六、CMake 中的修复

本项目 CMakeLists.txt 现状（与原有 macOS 分支并列）：

```cmake
set(CMAKE_CXX_STANDARD 20)          # 开关 1：语言标准（std::format 需要）
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Linux + Clang: 系统 libstdc++ (GCC 10) 无 <format>，改用 libc++
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-stdlib=libc++)   # 开关 2：编译期换头文件
    add_link_options(-stdlib=libc++)      # 开关 3：链接期换 -lc++
endif()
```

要点：

- `-stdlib=libc++` 必须编译、链接**两边都加**，否则链接期仍去找 `libstdc++.so`
- 条件里带 `NOT APPLE` 是因为 macOS 分支已单独处理（Homebrew llvm 自带 `c++/v1`）
- 用 GCC 编译 Linux 版则不受影响（前提是 GCC ≥ 13）

---

## 七、常用排查命令速查

| 目的 | 命令 |
|------|------|
| 看头文件搜索路径 | `clang++ -E -x c++ -v /dev/null` |
| 看完整编译/链接命令 | `clang++ -v a.cpp -o a`（编译与 ld 命令行全打印） |
| 看 GCC 探测结果 | 同上，输出开头的 `Selected GCC installation` |
| 看驱动级搜索目录 | `clang++ -print-search-dirs` |
| 看 ld 内置 SEARCH_DIR | `ld --verbose \| grep SEARCH_DIR` |
| 看 ELF 依赖（静态视角） | `readelf -d minicc \| grep -E "NEEDED\|RUNPATH"` |
| 看运行时解析（动态视角） | `ldd minicc` |
| 看运行时配置 | `cat /etc/ld.so.conf.d/*.conf` |
| 重建运行时缓存 | `sudo ldconfig` |
| 查某个头属于哪个包 | `dpkg -S /usr/include/c++/v1/format` |

配套自检脚本见 [`scripts/check-toolchain.sh`](../scripts/check-toolchain.sh)。
