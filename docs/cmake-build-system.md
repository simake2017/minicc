# CMake 构建系统详解

本文档解释 minicc 项目的构建过程：CMake、Make、Ninja 三者的关系，以及 `make` 如何自动追踪 `CMakeLists.txt` 的变更。

- 读者：想弄清「改了 `CMakeLists.txt` 为什么不用手动重跑 cmake」「为什么注释掉 `link_directories` 就链接失败」的 minicc 开发者
- 审法：本文出现的每一行 `CMakeLists.txt` 指令，都要能在正文里找到它变成了哪一条编译/链接命令；每一条"失败"都要能指回一条可执行的具体命令
- 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 按 tech-doc 完整规格骨架补齐 |

---

## 目录

- [第一部分：概要设计（这一块对外承诺什么）](#第一部分概要设计这一块对外承诺什么)
- [第二部分：详细设计（怎么做）](#第二部分详细设计怎么做)
- [核心问题：make 会读 CMakeLists.txt 吗？](#核心问题make-会读-cmakeliststxt-吗)
- [构建全流程](#构建全流程)
- [CMakeLists.txt 每一行的实际效果](#cmakeliststxt-每一行的实际效果)
- [为什么注释 link_directories 就编译失败](#为什么注释-link_directories-就编译失败)
- [CMake 生成的文件清单](#cmake-生成的文件清单)
- [make 如何自动检测 CMakeLists.txt 变更](#make-如何自动检测-cmakeliststxt-变更)
- [make vs ninja 对比](#make-vs-ninja-对比)
- [实际编译和链接命令](#实际编译和链接命令)

---

## 第一部分：概要设计（这一块对外承诺什么）

### §1 概念

#### 1.1 实体

| 实体 | 是什么 | 在哪些场景起作用 |
|---|---|---|
| **构建配置源**（`CMakeLists.txt`） | 人手写的构建配置，一行一条指令；它是本项目构建行为的唯一来源 | 加源文件、改 C++ 标准、换标准库路径 |
| **生成器**（make / ninja） | 把配置源翻译成"构建系统能吃的文件"的那一层；**CMake 自己不编译任何东西** | 决定产物是 `Makefile` 还是 `build.ninja` |
| **构建目录** | 生成物与最终产物的落脚处；本项目有 `build/`（手动，make）与 `cmake-build-debug/`（CLion 自动，ninja） | 换生成器 = 换一个构建目录 |
| **哨兵目标**（`cmake_check_build_system`） | CMake 写进 `Makefile` 的第一个目标，只干一件事：检查配置源有没有变 | 每次 `make` 开头都会被触发 |
| **编译期头文件路径**（`include_directories`） | 决定 `#include <xxx>` 去哪个目录找 | 只在编译阶段起作用 |
| **链接期库路径**（`link_directories`） | 决定 `-lxxx` 去哪个目录找 | 只在链接阶段起作用 |
| **ABI 配套关系** | 编译期用的头文件与链接期用的库必须来自**同一套标准库实现**，否则符号对不上 | 换标准库实现（libstdc++ ↔ libc++）时 |

#### 1.2 名词

| 名词 | 一句话（大白话） | 在哪出现 |
|---|---|---|
| 配置（configure） | 跑一次 `cmake`，把 `CMakeLists.txt` 翻译成构建文件 | 构建目录第一次出现时 |
| 构建（build） | 跑一次 `make` / `ninja`，真正编译与链接 | 每次改代码之后 |
| 增量构建 | 只重编被改动的源文件，其余产物沿用 | 第二次及以后的 `make` |
| 哨兵目标 | 一个名字像文件、其实是个动作的 make 目标，专门用来触发"重新配置" | `Makefile` 第一个目标 `all` 的依赖 |
| 目标文件（`.o`） | 编译单个 `.cpp` 的产物，含机器码与符号表，地址未定 | `CMakeFiles/minicc.dir/src/*.cpp.o` |
| 链接 | 把所有 `.o` 与库合成一个可执行文件，并把地址填实 | 产出 `build/minicc` |

### §2 正常流程

**整体流程图**（方框图；失败出口见 §3）：

```
┌───────────────────────────────────────┐
│ ① 配置：cmake ..                      │
│   读 CMakeLists.txt                   │
│   → Makefile / CMakeCache.txt         │
│   → CMakeFiles/（逐文件的命令与选项）  │
└──────────────────┬────────────────────┘
                   │ 生成 Makefile
                   ▼
┌───────────────────────────────────────┐
│ ② 变更检测：cmake_check_build_system  │
│   （哨兵目标）                         │
│   ◇ 配置源比 Makefile 新吗？            │
└────┬─────────────────────────┬────────┘
     │ 没变                    │ 变了
     │                         ▼
     │           ┌───────────────────────────────────┐
     │           │ 重新配置（cmake 再跑一遍）         │
     │           │ → 重写 Makefile / flags.make /    │
     │           │   link.txt                        │
     │           └────────────────┬──────────────────┘
     │                            │
     ▼                            ▼
┌──────────────────────────────────────────┐
│ ③ 编译：每个 .cpp → .o                  │
│   命令在 CMakeFiles/minicc.dir/build.make │
│   选项在 CMakeFiles/minicc.dir/flags.make │
└───────────────────┬──────────────────────┘
                    ▼
┌──────────────────────────────────────────┐
│ ④ 链接：所有 .o + 库 → build/minicc      │
│   命令在 CMakeFiles/minicc.dir/link.txt  │
│   （只有一行）                            │
└──────────────────────────────────────────┘
```

**步骤**（每步一个动作，决策处带【为什么】）：

1. **在构建目录里跑 `cmake ..`**——`..` 告诉 cmake 配置源在上一级。【为什么】配置源与构建目录必须分开，生成物不污染源码树。
2. **cmake 读 `CMakeLists.txt`，把每条指令翻译成一条编译/链接命令行**。【为什么】CMake 不自己编译，它只生成"给 make 看的说明书"。
3. **生成物落进构建目录**：`Makefile`（make 的入口）、`CMakeCache.txt`（配置变量缓存）、`CMakeFiles/`（逐文件的编译命令与选项）、`compile_commands.json`（IDE 用的完整编译命令）。
4. **跑 `make`**。【为什么】此刻 `make` 只认 `Makefile`，`CMakeLists.txt` 对它不可见；"配置有没有过期"由第 5 步的哨兵目标代劳。
5. **哨兵目标先跑**：比较配置源与 `Makefile` 的修改时间；发现配置源更新就自动重新配置一遍，**不需要手动再跑 `cmake ..`**。
6. **逐个 `.cpp` 编成 `.o`**：命令取自 `build.make`，选项取自 `flags.make`。
7. **所有 `.o` 链成 `minicc`**：命令取自 `link.txt`，整条命令就一行。

### §2.5 更新对照

本主题**有写契约**：改一行 `CMakeLists.txt` 会改写构建目录里生成的构建文件。
下面以"注释掉 `link_directories`"为例给出改前/改后对照（完整推导见正文「为什么注释 link_directories 就编译失败」一节）。

改前（`CMakeLists.txt` 保留 `link_directories`）——`build/CMakeFiles/minicc.dir/link.txt`：

```jsonc
{
  "命令行": "/opt/homebrew/opt/llvm/bin/clang++ -g -arch arm64 CMakeFiles/minicc.dir/src/lexer.cpp.o ... -o minicc -L/opt/homebrew/opt/llvm/lib/c++",
  //           ↑ 编译器 + 全部 .o                        ↑ 产物名            ↑ ← 改：注释掉 link_directories 后这一段整段消失
  "来源": "add_executable(minicc ${SOURCES}) + link_directories(\"/opt/homebrew/opt/llvm/lib/c++\")"
}
```

改后（把 `link_directories` 注释掉）：

```jsonc
{
  "命令行": "/opt/homebrew/opt/llvm/bin/clang++ -g -arch arm64 CMakeFiles/minicc.dir/src/lexer.cpp.o ... -o minicc",
  //           ↑ 编译器 + 全部 .o                        ↑ 产物名（末尾的 -L 整段不见了）
  "来源": "add_executable(minicc ${SOURCES})（link_directories 已被注释）",
  "后果": "链接器转去找系统自带的旧 libc++，与编译期用的 LLVM 21 头文件 ABI 不匹配 → Undefined symbols"
}
```

★ 判据一句话：**头文件用谁家的，库文件也得用谁家的**——`include_directories` 与 `link_directories` 必须配套。

### §3 异常处理

| # | 异常（现象） | 如何处理 | 可重试 |
|---|---|---|---|
| 1 | 链接期报 `Undefined symbols`（例如 `std::__1::__hash_memory` 这类新符号找不到），而编译期用的是 LLVM 21 的头文件 | 看 `build/CMakeFiles/minicc.dir/link.txt` 末尾有没有 `-L/opt/homebrew/opt/llvm/lib/c++`；没有就是 `link_directories` 被注释掉了，补回去重新 `cmake .. && make` | 否——是配置写错，必须改 `CMakeLists.txt` |
| 2 | 配置期报 `CMake Error: The source directory "..." does not appear to contain CMakeLists.txt.`（cmake 4.3.4 实测） | `cmake ..` 的 `..` 指错了层级：确认当前目录是构建目录、上一级才有 `CMakeLists.txt` | 否——路径写错 |
| 3 | 配置期报 `The CMAKE_CXX_COMPILER: ... is not a full path to an existing compiler tool.`（cmake 4.3.4 实测） | 用 `-DCMAKE_CXX_COMPILER=<编译器全路径>` 指定，或设环境变量 `CXX`；若 `build/CMakeCache.txt` 里已缓存了错的路径，先删掉那一行再重配 | 否——编译器路径写错 |
| 4 | 头文件能找到，但特性被语言标准挡掉（例：`std::format` 报 `no member named 'format' in namespace 'std'`） | 补 `-std=c++20`（本项目由 `set(CMAKE_CXX_STANDARD 20)` 提供）；Linux 上还要同时切标准库，见 `docs/toolchain-search-paths.md` | 否——开关写错 |

★ 表里每一条"现象"都是**配置期或链接期的原文报错**，本项目零日志可查——
CMake 与链接器的输出就是本主题的观测面（见 §5）。

### §4 Out of Scope

- **CMake 的模块化与打包命令**：`install` / `export` / `package` / `find_package` 生态不在本文范围（本文只讲"配置 → 构建"这一段）。
- **Visual Studio / Xcode 生成器**：只对比 `Unix Makefiles` 与 `Ninja` 两个生成器；Windows 与苹果的 IDE 生成器另说。
- **编译器缓存与分布式编译**（ccache / distcc / sccache）：本项目的构建不经过它们。
- **交叉编译**（toolchain file、Android/iOS 构建）：不在本文范围。
- **make 与 ninja 的语法本身**：另见 `docs/make-tutorial.md`；本文只讲"CMake 怎么把配置交给它们"。
- **Linux 分支的标准库切换**（`-stdlib=libc++`）：另见 `docs/toolchain-search-paths.md`，本文的 `CMakeLists.txt` 片段是 macOS/Homebrew 形态。

### §5 观测点

CMake 与 make 都是外部工具，本主题**没有自己的日志键**；下表是排查时真正会看的东西。

| 时机 | 看的键 / 命令 | 正常时看到什么 |
|---|---|---|
| 配置后：确认编译器与语言标准 | `grep -E 'CMAKE_CXX_COMPILER:\|CMAKE_CXX_STANDARD:' build/CMakeCache.txt` | 编译器是全路径；标准是 20 |
| 配置后：列出全部缓存变量 | `cmake -S . -B build -LAH` | 每条缓存变量一行，含类型与来源 |
| 配置过程想逐行看 | `cmake --trace-expand -S . -B build`（cmake 4.3.4 实测有此开关） | 逐条 `CMakeLists.txt` 指令的展开过程 |
| 构建前：确认哨兵目标还在 | `grep -A3 '^all:' build/Makefile` | `all` 的第一个依赖是 `cmake_check_build_system` |
| 构建前：确认配置源被追踪 | `grep CMakeLists.txt build/CMakeFiles/Makefile.cmake` | 出现 `"../CMakeLists.txt"` 这条依赖记录 |
| 编译期：确认最终选项 | `cat build/CMakeFiles/minicc.dir/flags.make` | 含 `-std=gnu++20`、`-I.../include`、`-isystem`、`-Wall -Wextra -pedantic` |
| 链接期：确认最终链接命令 | `cat build/CMakeFiles/minicc.dir/link.txt` | 整条命令一行，末尾有 `link_directories` 带来的 `-L` |
| IDE 侧：查完整编译命令 | `build/compile_commands.json` | 每个 `.cpp` 一条完整命令行 |

---

## 第二部分：详细设计（怎么做）

### §6 整体流程图

分解视角：把上一部分的宏观流程拆成四个可独立排查的子流程，框内只写子流程名字。

```
        ┌──────────────┐
        │ 配置子流程    │
        └──────┬───────┘
               │ 产出：Makefile、CMakeCache.txt、CMakeFiles/
               ▼
        ┌──────────────┐
        │ 变更检测子流程│◀── 每次 make 都从这里进
        └──────┬───────┘
               │ 配置源未变 → 直接进入编译
               │ 配置源已变 → 回到「配置子流程」
               ▼
        ┌──────────────┐
        │ 编译子流程    │
        └──────┬───────┘
               │ 产出：CMakeFiles/minicc.dir/src/*.cpp.o
               ▼
        ┌──────────────┐
        │ 链接子流程    │
        └──────┬───────┘
               │ 产出：build/minicc
               ▼
            可执行文件
```

★ 与 §2 的差别：§2 是**使用者视角**（我敲了什么命令、看到什么）；
这里是**分解视角**（哪一步失败会断掉后面、每个子流程读哪些文件、写哪些文件）。

### §7 子流程 + 说人话步骤

#### 7.1 配置子流程

```
→ 在构建目录里跑 cmake ..
   → cmake 读 CMakeLists.txt
      → 逐条指令翻译成命令行
         → 写 Makefile / CMakeCache.txt / CMakeFiles/
```

1. **进入构建目录，跑 `cmake ..`**：`..` 指向上一级的配置源。
2. **cmake 从 `CMakeLists.txt` 的第一条指令开始逐条翻译**：`project()` 定语言与工程名，`set()` 定变量，`add_executable()` 定"编译哪些源文件、产出什么"，`target_include_directories()` / `include_directories()` / `target_compile_options()` / `link_directories()` 定选项。
3. **把翻译结果分成三处落盘**：
   - `Makefile`——make 的入口，定义 `all` / `clean` / `minicc` 等目标；
   - `CMakeCache.txt`——缓存所有配置变量（编译器路径、构建类型、标准版本）；
   - `CMakeFiles/`——逐个 `.cpp` 的编译命令（`build.make`）、编译选项（`flags.make`）、头文件依赖（`depend.make`）、最终链接命令（`link.txt`）。
4. **顺带写 `compile_commands.json`**：给 IDE 用的完整编译命令清单，clangd / CLion 靠它做跳转与补全。

【为什么】把"配置"和"构建"拆成两步落盘，是因为配置结果可以被反复复用：
构建目录不删，`cmake ..` 就只跑一次，之后每次 `make` 只做增量。

#### 7.2 变更检测子流程

```
→ make 启动 → 第一个目标是 all
   → all 依赖 cmake_check_build_system
      → 跑 cmake --check-build-system CMakeFiles/Makefile.cmake 0
         → 比 CMakeLists.txt 与 Makefile 的修改时间
            ├─ 没变 → 什么都不做，继续编译
            └─ 变了 → 自动重新跑一遍配置 → 用新 Makefile 继续编译
```

1. **make 启动后先看第一个目标**：`Makefile` 的第一个目标是 `all`，而 `all` 依赖哨兵目标 `cmake_check_build_system`。
2. **哨兵目标跑一条检查命令**：`cmake --check-build-system CMakeFiles/Makefile.cmake 0`，参数 `0` 是"进入检查模式"的开关。
3. **检查命令读 `Makefile.cmake` 里登记的依赖清单**：本项目登记的是 `"../CMakeLists.txt"`。
4. **逐个比修改时间**：任何一个依赖比 `Makefile` 新，就判定"配置已过期"。
5. **过期就自动重配**：cmake 重跑一遍配置流程，重写 `Makefile`、`flags.make`、`link.txt`，然后 make 接着用新文件编译。【为什么】这样用户改完 `CMakeLists.txt` 直接 `make` 就行，不必记住"先 cmake 后 make"两步。

#### 7.3 编译子流程

```
→ 遍历 SOURCES 里的每个 .cpp
   → 从 build.make 取编译命令、从 flags.make 取选项
      → 产出 CMakeFiles/minicc.dir/src/<名字>.cpp.o
```

1. **逐个源文件取命令**：`build.make` 里每个 `.cpp` 一条；选项统一在 `flags.make`。
2. **把选项拼到命令上**：`-std=gnu++20` 来自 `CMAKE_CXX_STANDARD 20`；`-I<项目>/include` 来自 `target_include_directories`；`-isystem <标准库头目录>` 来自 `include_directories(SYSTEM ...)`；`-Wall -Wextra -pedantic` 来自 `target_compile_options`。
3. **只编过期的**：【为什么】`.o` 的时间戳比 `.cpp` 旧才重编——这是 make 的增量判据，`make` 自己决定，不用本项目操心。
4. **产出 `.o`**：落在 `CMakeFiles/minicc.dir/src/` 下，与源文件同名。

#### 7.4 链接子流程

```
→ 收集全部 .o
   → 从 link.txt 取整条链接命令
      → 产出 build/minicc
```

1. **收集全部 `.o`**（正文示例里是 `lexer.cpp.o` / `parser.cpp.o` / `type.cpp.o` / `semantic_analyzer.cpp.o` / `template_instantiation.cpp.o` / `codegen.cpp.o` / `main.cpp.o` 七件）。
2. **整条链接命令只有一行**，写在 `link.txt` 里：编译器 + 全部 `.o` + `-o minicc` + `link_directories` 翻译来的 `-L`。
3. **链接器同时干两件事**：把 `.o` 里的地址占位符填成真实地址；按 `-L` 顺序找 `-l` 指定的库。【为什么】`-L` 决定"去哪个目录找库"，找错目录就会拿到与头文件不配套的旧标准库（§3 第 1 条）。

### §8 异常流转表

| 异常 | 触发点 | 状态/字段变化 | 恢复路径 |
|---|---|---|---|
| `link_directories` 被注释 | 链接子流程第 2 步，链接器在 `-L` 列表里找不到 LLVM 21 的 libc++ | `link.txt` 里的 `-L` 段消失；链接器转用系统旧 libc++；目标文件 `minicc` **不会生成** | 改回 `CMakeLists.txt` 的 `link_directories`，跑 `make`（哨兵目标会自动重配） |
| 配置源路径写错（`cmake ..` 指错层级） | 配置子流程第 1 步 | 构建目录里不生成任何文件；`CMakeCache.txt` 不生成 | 切到正确的构建目录重跑 `cmake ..` |
| 编译器路径写错 | 配置子流程第 2 步，cmake 找不到 C++ 编译器 | 配置中止；`CMakeCache.txt` 不生成（或残留半截缓存） | 用 `-DCMAKE_CXX_COMPILER=<全路径>` 或环境变量 `CXX` 重配 |
| 语言标准低于特性要求 | 编译子流程第 2 步，头文件找到了但符号被标准版本挡掉 | `.o` 不生成 | 补 `set(CMAKE_CXX_STANDARD 20)`；Linux 上还要切标准库（见 `docs/toolchain-search-paths.md`） |
| `CMakeLists.txt` 改了但选项没变 | 变更检测子流程第 4 步发现配置源过期 | `Makefile` / `flags.make` / `link.txt` 被重写（内容可能逐字节相同） | 无需处置——这是正常路径 |

### §9 持久状态 / 空值语义

本主题**没有状态机**（构建是一次次无状态重入的过程），只有持久状态：构建目录里的文件。

```jsonc
{
  "build/CMakeCache.txt": {
    "//": "配置变量缓存，键值对文本；cmake 4.x 格式为 KEY:TYPE=VALUE",
    "CMAKE_CXX_COMPILER": "/usr/bin/clang++",      // 编译器全路径（配置期由 -D 或环境变量写入）
    "CMAKE_CXX_STANDARD": "20",                    // 语言标准；未显式设置时由 project() 的默认值决定
    "CMAKE_BUILD_TYPE": "",                        // 空串 = 未指定构建类型（不是 Debug 也不是 Release）
    "CMAKE_HOME_DIRECTORY": "/root/cppproject/mycompiler"  // 配置源所在目录
  },
  "build/CMakeFiles/minicc.dir/flags.make": {
    "//": "编译选项集合，被 build.make 里的每条编译命令引用",
    "CXX_FLAGS": "-std=gnu++20 -Wall -Wextra -pedantic",
    "CXX_DEFINES": "",                             // 空串 = 没有 -D 宏定义
    "CXX_INCLUDES": "-I/root/cppproject/mycompiler/include -isystem /opt/homebrew/opt/llvm/include/c++/v1"
  },
  "build/CMakeFiles/minicc.dir/link.txt": {
    "//": "整条链接命令，纯文本一行",
    "内容": "/opt/homebrew/opt/llvm/bin/clang++ ... -o minicc -L/opt/homebrew/opt/llvm/lib/c++"
  }
}
```

**空值语义**：

| 空值 | 含义 |
|---|---|
| `CMAKE_BUILD_TYPE` 为空串 | 未指定构建类型；此时既不加 `-O` 也不加 `-g`（除非显式写了 `target_compile_options`） |
| `CXX_DEFINES` 为空串 | 没有预定义宏，不是"宏被清空" |
| `link.txt` 里没有 `-L` | 链接器改用系统默认库目录；本项目这意味着换了一套标准库，通常直接链接失败 |

### §10 数据形状

构建目录的产物形状（一份构建目录该有什么、每个文件谁写谁读）：

```jsonc
{
  "build/": {
    "Makefile":                    { "写": "cmake（配置期）", "读": "make", "形状": "make 规则文本" },
    "CMakeCache.txt":              { "写": "cmake（配置期）", "读": "cmake（下次配置）", "形状": "KEY:TYPE=VALUE 文本" },
    "compile_commands.json":       { "写": "cmake（配置期）", "读": "IDE / clangd", "形状": "JSON 数组，每元素一条编译命令" },
    "cmake_install.cmake":         { "写": "cmake（配置期）", "读": "cmake --install", "形状": "cmake 脚本" },
    "CMakeFiles/": {
      "Makefile.cmake":            { "写": "cmake", "读": "cmake --check-build-system", "形状": "依赖清单（含 \"../CMakeLists.txt\"）" },
      "Makefile2":                 { "写": "cmake", "读": "make（递归调用）", "形状": "二级调度规则" },
      "minicc.dir/": {
        "build.make":              { "写": "cmake", "读": "make", "形状": "每个 .cpp 的编译命令 + 链接命令" },
        "flags.make":              { "写": "cmake", "读": "make", "形状": "编译选项变量" },
        "depend.make":             { "写": "cmake", "读": "make", "形状": "头文件依赖" },
        "link.txt":                { "写": "cmake", "读": "make", "形状": "一行链接命令" },
        "compiler_depend.ts":      { "写": "make", "读": "make", "形状": "编译器时间戳戳记" },
        "src/lexer.cpp.o":         { "写": "编译器", "读": "链接器", "形状": "目标文件" }
      }
    },
    "minicc":                      { "写": "链接器", "读": "用户", "形状": "可执行文件" }
  }
}
```

### §11 接口契约

本主题的外部接口就是**命令行**（cmake 与 make 各自的参数与产物）：

**cmake 侧**

| 参数 | 含义 | 产物 / 效果 |
|---|---|---|
| `cmake ..` | 配置源在上一级 | 生成整棵构建目录 |
| `cmake -S <源码目录> -B <构建目录>` | 显式指定两端 | 同上，但不受当前工作目录限制 |
| `cmake -S <src> -B <bld> --check-build-system CMakeFiles/Makefile.cmake 0` | 哨兵目标用的检查模式 | 配置过期就重配，否则什么都不做 |
| `-DCMAKE_CXX_COMPILER=<路径>` | 指定编译器 | 写入 `CMakeCache.txt` |
| `-DCMAKE_CXX_STANDARD=<n>` | 指定语言标准 | 翻译成 `-std=gnu++<n>` |
| `-G Ninja` | 换生成器 | 产物变成 `build.ninja` 而不是 `Makefile` |
| `--trace-expand` | 打印每条指令的展开过程 | 排配置问题时用 |

**make 侧**（由 cmake 生成的 `Makefile` 兑现）

| 目标 / 参数 | 含义 |
|---|---|
| `make` | 构建默认目标 `all`（依赖哨兵目标） |
| `make minicc` | 只构建可执行文件目标 |
| `make clean` | 清理构建产物 |
| `make -j<n>` | 并行构建（n 取 6 之类；本机 8 核留 2 核给交互任务） |
| `make -n` | dry-run：只打印要执行的命令，不执行 |

**边界（谁说了算）**：`CMakeLists.txt` 是唯一的配置来源；
`CMakeCache.txt` 是 cmake 自己的缓存，改它不如改 `CMakeLists.txt`（改了缓存不重配，下次配置会被覆盖）。

### §12 否决记录

| 被否方案 | 否决原因 | 谁否的 / 什么时候 |
|---|---|---|
| 在项目里只保留 ninja 一套构建目录 | `build.ninja` 是纯机器格式，人读不懂；排查"到底跑了哪条编译命令"时 make 的 `Makefile` 与 `flags.make` 更好读。两套目录并存（`build/` 用 make、`cmake-build-debug/` 用 ninja）成本几乎为零 | 本项目既有实践（正文「你的项目里两套构建目录」一节） |
| 只配 `include_directories` 不配 `link_directories` | 编译期用 LLVM 21 的头文件、链接期却用系统旧 libc++，ABI 不匹配 → Undefined symbols。两者必须配套 | 正文「为什么注释 link_directories 就编译失败」一节的实测结论 |
| 每次改完 `CMakeLists.txt` 都要手动重跑 `cmake ..` | cmake 已经把变更检测做成哨兵目标塞进 `Makefile`，手动重跑是多余的 | 本项目既有实践 |

---

## 核心问题：make 会读 CMakeLists.txt 吗？

**不会直接读。** 但 CMake 在生成的 Makefile 里埋了一个"检查点"机制：

```
你改了 CMakeLists.txt
     ↓
你执行 make
     ↓
make 第一个目标是 all
     ↓
all 依赖 cmake_check_build_system
     ↓
cmake_check_build_system 运行:
  cmake --check-build-system CMakeFiles/Makefile.cmake 0
     ↓
cmake 检查 ../CMakeLists.txt 的修改时间
     ↓
发现比 Makefile 新 → 自动重新跑 cmake 配置
     ↓
重新生成 Makefile（你的改动已生效）
     ↓
make 用新 Makefile 编译/链接
```

所以你不需要每次手动 `cmake ..`，CMake 的依赖追踪帮你做了。

---

## 构建全流程

```
项目根目录: mycompiler/
  └── CMakeLists.txt          ← 你写的构建配置

第一步: cmake 配置（只需一次，或 CMakeLists.txt 改动后自动触发）
  cd build
  cmake ..                     ← '..' 告诉 cmake: CMakeLists.txt 在上一级
  cmake 读取 CMakeLists.txt，生成:
    build/Makefile             ← make 用的构建规则
    build/CMakeCache.txt       ← 缓存所有配置变量
    build/CMakeFiles/          ← 编译/链接的具体命令
    build/compile_commands.json

第二步: make 构建
  make                         ← 读取 build/Makefile，编译+链接
  产出: build/minicc
```

### 关键理解

```
CMakeLists.txt   →   cmake 翻译   →   Makefile
 (你写的配置)                        (make 实际读的)

你改 CMakeLists.txt → make 自动检测到 → 自动重新跑 cmake → 重新生成 Makefile
```

`make` 只认 `Makefile`，不认 `CMakeLists.txt`。但 CMake 在 `Makefile` 里设了一个"哨兵"目标 `cmake_check_build_system`，每次 `make` 都先检查 `CMakeLists.txt` 有没有被改过。

---

## CMakeLists.txt 每一行的实际效果

### 你的 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(MiniCppCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    set(CXX_STDLIB_PATH "/opt/homebrew/opt/llvm/include/c++/v1")
    if(EXISTS ${CXX_STDLIB_PATH})
        include_directories(SYSTEM ${CXX_STDLIB_PATH})
        link_directories("/opt/homebrew/opt/llvm/lib/c++")
    endif()
endif()

set(SOURCES
    src/lexer.cpp
    src/parser.cpp
    src/type.cpp
    src/semantic_analyzer.cpp
    src/template_instantiation.cpp
    src/codegen.cpp
    src/main.cpp
)

add_executable(minicc ${SOURCES})
target_include_directories(minicc PRIVATE include)
target_compile_options(minicc PRIVATE -Wall -Wextra -pedantic)
```

### 每行对应到实际的编译/链接命令

| CMakeLists.txt 写的 | 做什么 | 变成什么 |
|---|---|---|
| `set(CMAKE_CXX_STANDARD 20)` | C++ 版本 | `-std=gnu++20` |
| `include_directories(SYSTEM ...)` | 编译时头文件搜索路径 | `-isystem /opt/homebrew/opt/llvm/include/c++/v1` |
| `target_include_directories(minicc PRIVATE include)` | 项目头文件路径 | `-I.../mycompiler/include` |
| `link_directories("...")` | **链接时**库文件搜索路径 | `-L/opt/homebrew/opt/llvm/lib/c++` |
| `target_compile_options(...)` | 编译警告选项 | `-Wall -Wextra -pedantic` |
| `add_executable(minicc ${SOURCES})` | 编译哪些源文件、输出什么 | 所有 `.cpp` → `.o` → `minicc` |

### 两条命令的区别

```
include_directories  →  编译阶段  →  #include <xxx> 去哪找头文件
link_directories     →  链接阶段  →  -lxxx 去哪找库文件
```

---

## 为什么注释 link_directories 就编译失败

### 你机器上有两套 C++ 环境

```
① Homebrew LLVM 21（你自己装的）
   编译器:  /opt/homebrew/opt/llvm/bin/clang++      版本 21.1.8
   头文件:  /opt/homebrew/opt/llvm/include/c++/v1
   库文件:  /opt/homebrew/opt/llvm/lib/c++/libc++.dylib

② Apple 系统自带
   编译器:  /usr/bin/clang++                        版本 17.0.0
   库文件:  /usr/lib/libc++.dylib                   ← 旧版
```

### 有 link_directories 时（正常）

```
编译: clang++ (LLVM 21) + LLVM 21 的头文件 → .o    ✅ 匹配
链接: clang++ (LLVM 21) + LLVM 21 的 libc++ → minicc  ✅ 匹配
```

### 注释掉 link_directories 后（失败）

```
编译: clang++ (LLVM 21) + LLVM 21 的头文件 → .o    ✅ 匹配
链接: clang++ (LLVM 21) + 系统的旧版 libc++ → 失败  ❌ ABI 不匹配！

  原因: LLVM 21 的头文件里用了一些新符号
        (比如 std::__1::__hash_memory)
        但 Apple Clang 17 的旧 libc++ 里没有这个函数
        → Undefined symbols → 链接失败
```

### 一句话

**头文件用谁家的，库文件也得用谁家的。`include_directories` 和 `link_directories` 必须配套。**

---

## CMake 生成的文件清单

```
build/
├── Makefile                        ← make 的入口文件
├── CMakeCache.txt                  ← 所有配置变量的缓存
├── compile_commands.json           ← 完整编译命令（IDE 用）
├── cmake_install.cmake             ← install 规则
│
├── CMakeFiles/
│   ├── Makefile.cmake              ← 追踪 CMakeLists.txt 依赖
│   │                                 （记录 "../CMakeLists.txt" 是依赖源）
│   │
│   ├── Makefile2                   ← 二级 Makefile（递归目标）
│   │
│   └── minicc.dir/
│       ├── build.make              ← 每个 .cpp 的编译命令 + 链接命令
│       ├── flags.make              ← 编译选项（-I, -std, -Wall...）
│       ├── depend.make             ← 头文件依赖
│       ├── link.txt                ← 最终的链接命令（一行）
│       ├── compiler_depend.ts      ← 编译器依赖时间戳
│       └── src/
│           ├── lexer.cpp.o         ← 编译产物
│           ├── parser.cpp.o
│           ├── ...
│           └── main.cpp.o
│
└── minicc                          ← 最终产物：编译器可执行文件
```

### 各文件的职责

| 文件 | 谁生成的 | 做什么 |
|------|---------|--------|
| `Makefile` | cmake | make 的入口，定义 all/clean/minicc 等目标 |
| `Makefile2` | cmake | 二级调度，递归调用 build.make |
| `minicc.dir/build.make` | cmake | 每个 .cpp → .o 的具体编译命令 |
| `minicc.dir/flags.make` | cmake | 编译选项（从 CMakeLists.txt 翻译来） |
| `minicc.dir/link.txt` | cmake | 最终链接命令（一行，包含所有 .o 和 -L） |
| `Makefile.cmake` | cmake | 记录 CMakeLists.txt 是依赖源 |
| `CMakeCache.txt` | cmake | 缓存编译器路径、构建类型等变量 |

---

## make 如何自动检测 CMakeLists.txt 变更

### 机制：Makefile 里的"哨兵"目标

CMake 在生成的 `Makefile` 中写了这个规则：

```makefile
# 每次 make 都先跑这个
all: cmake_check_build_system
    $(MAKE) -f CMakeFiles/Makefile2 all

# 检查构建系统是否需要重新生成
cmake_check_build_system:
    $(CMAKE_COMMAND) -S$(CMAKE_SOURCE_DIR) -B$(CMAKE_BINARY_DIR) \
        --check-build-system CMakeFiles/Makefile.cmake 0
```

### 这条命令做了什么

```
cmake -S.. -B. --check-build-system CMakeFiles/Makefile.cmake 0

  -S..            源码目录（CMakeLists.txt 在这里）
  -B.             构建目录（Makefile 在这里）
  --check-build-system
                  检查 Makefile.cmake 中记录的所有依赖文件
                  的修改时间，如果有任何一个比 Makefile 新，
                  就重新运行 cmake 配置

Makefile.cmake 里记录:
  "../CMakeLists.txt"    ← 这个文件被追踪为依赖
```

### 完整链路图

```
你执行 make
    │
    ▼
Makefile 的第一个目标是 all
    │
    ▼
all 依赖 cmake_check_build_system
    │
    ▼
cmake --check-build-system 检查 ../CMakeLists.txt 时间戳
    │
    ├── CMakeLists.txt 没改 → 什么都不做，继续编译
    │
    └── CMakeLists.txt 改了 → 自动重新跑 cmake 配置
        │
        ├── 重新生成 Makefile
        ├── 重新生成 flags.make（编译选项可能变了）
        ├── 重新生成 link.txt（链接选项可能变了）
        └── 然后继续用新 Makefile 编译/链接
```

---

## make vs ninja 对比

### 你的项目里两套构建目录

```
build/               ← 手动创建，用 make（Unix Makefiles 生成器）
cmake-build-debug/   ← CLion 自动创建，用 ninja（Ninja 生成器）
```

### 使用方式几乎一样

```bash
# 用 make
mkdir build && cd build
cmake ..                        # 生成 Makefile
make -j$(sysctl -n hw.ncpu)     # 编译

# 用 ninja
mkdir build && cd build
cmake .. -G Ninja               # 生成 build.ninja（注意 -G Ninja）
ninja                            # 编译
```

### 详细对比

| | make | ninja |
|---|---|---|
| **生成器** | `Unix Makefiles`（默认） | `Ninja`（需要 `-G Ninja`） |
| **生成文件** | `Makefile` + `CMakeFiles/` | `build.ninja` + `.ninja_deps` |
| **可读性** | Makefile 人能看懂 | build.ninja 是纯机器格式，很难读 |
| **速度** | 较慢（大项目明显） | 更快（专为速度设计，C++ 写的） |
| **并行编译** | 手动加 `-j8` | 默认就并行（自动检测 CPU 核心数） |
| **手写规则** | 支持（Makefile 可以手写） | 不支持（只接受工具生成） |
| **增量编译** | 基于文件时间戳 | 基于文件时间戳 + 内容哈希 |
| **谁在用** | 你手动 `build/` 目录 | CLion 的 `cmake-build-debug/` |

### 选哪个？

- **小项目**（如 minicc）：随便，感知不到差异
- **大项目**（如 Chromium、LLVM）：用 ninja，快很多
- **CLion 用户**：CLion 默认用 ninja，不用管
- **想调试构建过程**：用 make，Makefile 能看懂

### 核心结论

**因为你用 CMake，用哪个都一样。** CMake 帮你生成对应的构建文件，你只管 `cmake ..` 然后 `make` 或 `ninja` 就行。CMakeLists.txt 只写一份，两种生成器都能用。

---

## 实际编译和链接命令

### 编译命令（每个 .cpp 文件）

以 `lexer.cpp` 为例：

```bash
/opt/homebrew/opt/llvm/bin/clang++        \   ← 编译器: Homebrew LLVM 21
    -I.../mycompiler/include              \   ← target_include_directories
    -isystem .../llvm/include/c++/v1      \   ← include_directories(SYSTEM ...)
    -g -arch arm64                        \   ← Debug 模式 + ARM64
    -Wall -Wextra -pedantic               \   ← target_compile_options
    -std=gnu++20                          \   ← CMAKE_CXX_STANDARD 20
    -c src/lexer.cpp                      \   ← 编译（不链接）
    -o CMakeFiles/minicc.dir/src/lexer.cpp.o  ← 输出 .o 文件
```

### 链接命令（所有 .o → minicc）

```bash
/opt/homebrew/opt/llvm/bin/clang++        \   ← 编译器（也做链接器）
    -g -arch arm64                        \
    -L/opt/homebrew/opt/llvm/lib          \   ← Homebrew LLVM 的库路径
    CMakeFiles/minicc.dir/src/lexer.cpp.o \   ← 所有 .o 文件
    CMakeFiles/minicc.dir/src/parser.cpp.o    \
    CMakeFiles/minicc.dir/src/type.cpp.o      \
    CMakeFiles/minicc.dir/src/semantic_analyzer.cpp.o \
    CMakeFiles/minicc.dir/src/template_instantiation.cpp.o \
    CMakeFiles/minicc.dir/src/codegen.cpp.o   \
    CMakeFiles/minicc.dir/src/main.cpp.o      \
    -o minicc                             \   ← 输出可执行文件
    -L/opt/homebrew/opt/llvm/lib/c++          ← link_directories() 的效果！
```

注意最后一行 `-L/opt/homebrew/opt/llvm/lib/c++`——这就是 `link_directories()` 在 CMakeLists.txt 中的效果。注释掉它，这行就消失，链接器就找不到 LLVM 21 的 libc++，导致链接失败。
