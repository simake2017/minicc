# Make 完全教程

本教程从零开始讲解 Make，结合 minicc 项目的实际构建过程。

- 读者：想读懂 `Makefile` 的人，以及想手写一份不依赖 CMake 的 minicc 构建脚本的人
- 审法：每一段 Makefile 片段都要能"照着敲进去跑一遍"；每一条命令都要说明它是干什么用的
- 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 按 tech-doc 完整规格骨架补齐 |

---

## 目录

- [第一部分：概要设计（这一块对外承诺什么）](#第一部分概要设计这一块对外承诺什么)
- [第二部分：详细设计（怎么做）](#第二部分详细设计怎么做)
- [第一章：Make 是什么](#第一章make-是什么)
- [第二章：Makefile 基本语法](#第二章makefile-基本语法)
- [第三章：变量](#第三章变量)
- [第四章：自动变量](#第四章自动变量)
- [第五章：模式规则](#第五章模式规则)
- [第六章：内置函数](#第六章内置函数)
- [第七章：条件判断](#第七章条件判断)
- [第八章：phony 目标](#第八章phony-目标)
- [第九章：依赖与增量编译](#第九章依赖与增量编译)
- [第十章：多目录项目](#第十章多目录项目)
- [第十一章：实战——手写一个 minicc 的 Makefile](#第十一章实战手写一个-minicc-的-makefile)
- [第十二章：make 命令行技巧](#第十二章make-命令行技巧)
- [速查表](#速查表)

---

## 第一部分：概要设计（这一块对外承诺什么）

### §1 概念

#### 1.1 实体

| 实体 | 是什么 | 在哪些场景起作用 |
|---|---|---|
| **目标**（target） | 你想得到的东西：一个文件名（`minicc`、`build/lexer.o`），或者一个动作的名字（`clean`） | 每一行规则的头 |
| **依赖**（dependency） | 得到目标需要什么：文件列表（`src/lexer.cpp include/lexer.h`） | 决定"要不要重建" |
| **配方**（recipe） | 怎么从依赖得到目标：若干条 shell 命令，**每行必须以 Tab 开头** | 真正干活的那些行 |
| **变量** | 有名字的值：延迟展开（`=`）或立即展开（`:=`） | 复用编译选项、路径 |
| **自动变量** | make 在执行配方时自动设好的特殊变量（`$@` `$<` `$^` `$?` `$*`） | 写通用规则时省掉重复 |
| **模式规则** | 带 `%` 通配的通用规则（`%.o: %.cpp`） | 一条规则管一族文件 |
| **静态模式规则** | 把模式规则限定到一份显式清单上（`$(OBJECTS): build/%.o: src/%.cpp`） | 只想让指定文件走特殊规则 |
| **phony 目标** | 声明"这个目标不是文件，是动作"（`.PHONY: clean all`） | `clean` / `test` / `help` 这类 |
| **顺序依赖** | `|` 后面的依赖只保证"命令执行前存在"，**不参与时间戳比较** | `build/` 这类目录 |
| **依赖文件**（`.d`） | 编译器自动生成的"这个 .o 依赖哪些头文件"清单 | 头文件改动后的正确增量 |

#### 1.2 名词

| 名词 | 一句话（大白话） | 在哪出现 |
|---|---|---|
| 增量编译 | 只重编被改动的源文件，其余产物沿用 | make 的核心价值 |
| 时间戳判定 | 比目标文件与依赖文件的修改时间，新旧决定要不要重建 | 章节二「执行逻辑」 |
| dry-run | 只打印要执行的命令，不真跑 | `make -n` |
| 强制重建 | 无视时间戳，全部重做 | `make -B` |
| 配方回显 | 命令要不要打印在屏幕上 | 行首的 `@` 抑制 |
| 通配符 | 模式规则里的 `%`，匹配任意一段 | `%.o: %.cpp` |

### §2 正常流程

**整体流程图**（方框图）：

```
┌──────────────────────────────────────┐
│ 读 Makefile + 命令行 + 环境变量        │
│ 目标 / 依赖 / 配方 / 变量              │
└──────────────────┬───────────────────┘
                   ▼
┌──────────────────────────────────────┐
│ 选目标：命令行指定的那个，或第一个目标   │
└──────────────────┬───────────────────┘
                   ▼
┌──────────────────────────────────────┐
│ 展开变量（= 延迟 / := 立即）            │
└──────────────────┬───────────────────┘
                   ▼
┌──────────────────────────────────────┐
│ 匹配规则：找得到目标的配方吗？          │
│   → 找不到 ⇒ *** No rule to make target│
└──────────────────┬───────────────────┘
                   ▼
┌──────────────────────────────────────┐
│ 判时间戳                              │
│   ◇ 目标文件不存在？        → 要重建   │
│   ◇ 有依赖比目标新？        → 要重建   │
│   ◇ 全部依赖都比目标旧？    → 最新，不动│
└──────────────────┬───────────────────┘
                   │ 要重建
                   ▼
┌──────────────────────────────────────┐
│ 执行配方（逐条 shell 命令）             │
│   → 递归处理依赖（先编 .o 再链）       │
│   → -j 时并行执行                     │
└──────────────────────────────────────┘
```

**步骤**：

1. **读三处输入**：Makefile 里的规则、命令行上给的变量与目标、环境变量里的变量。【为什么】三处的优先级是「命令行 > Makefile > 环境变量」。
2. **选定要建的目标**：命令行点名了哪个就建哪个；没点名就建**Makefile 里的第一个目标**。
3. **展开变量**：`=` 定义的变量等到用的时候才展开，`:=` 定义的当场展开。
4. **匹配规则**：找到目标的配方；找不到就报 `No rule to make target`。
5. **判时间戳**：目标不存在 → 重建；有依赖比目标新 → 重建；否则"已经是最新的"，不动它。
6. **先建依赖再干自己的活**：`.o` 是 `minicc` 的依赖，所以先把 `.o` 建出来。
7. **执行配方**：每行配方丢给 shell 跑；行首的 `@` 让这条命令不在屏幕上回显。
8. **并行**：`-j<n>` 让互不依赖的目标同时建。【为什么】`.o` 之间互不依赖，是并行的主要收益点。

### §2.5 更新对照

本主题**有写契约**：改 Makefile 会改变"改一个头文件会不会触发重编"。
下面这个对照直接取自正文第九章「自动生成头文件依赖」一节。

改前（手写每个 `.o` 的头文件依赖）：

```jsonc
{
  "规则形状": {
    "目标": "lexer.o",
    "依赖": ["src/lexer.cpp", "include/lexer.h", "include/token.h"],   // ← 改：漏写一个头文件就永远不会因它重编
    "配方": ["clang++ -c $< -o $@"]
  },
  "后果": "改了 include/token.h（若它没被列进依赖）⇒ make 认为 lexer.o 已是最新 ⇒ 不重编 ⇒ 目标文件与源码不一致"
}
```

改后（让编译器自己生成依赖文件）：

```jsonc
{
  "规则形状": {
    "编译选项": "-std=c++20 -Wall -MMD -MP",     // ← 改：加 -MMD（生成 .d）与 -MP（为头文件加伪目标）
    "目标": "build/%.o",
    "依赖": "src/%.cpp",
    "配方": ["$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@"]
  },
  "附带规则": {
    "目标": "-include $(OBJS:.o=.d)",
    "含义": "把编译器生成的 .d 文件也读进来；前置的 - 表示文件不存在也不报错"
  },
  "后果": "改了任何头文件 ⇒ make 自动检测到并重编对应的 .o"
}
```

★ 生成出来的 `build/lexer.d` 长这样（正文第九章原文）：

```makefile
# build/lexer.d
build/lexer.o: src/lexer.cpp include/lexer.h include/token.h
```

### §3 异常处理

| # | 异常（现象） | 如何处理 | 可重试 |
|---|---|---|---|
| 1 | `Makefile:2: *** missing separator.  Stop.`（GNU Make 4.3 实测） | 配方行前面用了空格而不是 Tab。用 `cat -A Makefile \| grep "    "` 查（Tab 显示为 `^I`），把空格换成 Tab | 否——格式错 |
| 2 | `make: *** No rule to make target 'xxx'.  Stop.`（GNU Make 4.3 实测；make 报的是**第一个**找不到的依赖） | 依赖名写错、或那个文件根本不存在；正文给的稳妥写法是用 `$(wildcard include/*.h)` 把真实存在的头文件列进来 | 否 |
| 3 | `make: 'clean' is up to date.`（GNU Make 4.3 实测） | 当前目录里恰好有个叫 `clean` 的文件，make 以为它是个文件目标且没有依赖 ⇒ 什么都不做。加 `.PHONY: clean` | 否 |
| 4 | 变量值莫名多一个前导空格（`CXXFLAGS =  -Wall`） | `=` 两边的空格会被吃掉，但**值内部**的空格会保留；改成 `CXXFLAGS := -Wall` 或去掉多余空格 | 否 |
| 5 | 改了头文件却没触发重编 | 头文件没被列进对应 `.o` 的依赖；加 `-MMD -MP` 让编译器自动生成 `.d`，并用 `-include` 读进来 | 否 |
| 6 | `build/` 目录一有变动就触发全体重编 | `build/` 被写成了普通依赖，它的 mtime 一变所有 `.o` 都"过期"；改成顺序依赖 `\| $(BUILDDIR)` | 否 |

### §4 Out of Scope

- **GNU make 的内部实现**：递归变量展开算法、哈希表结构、作业服务器与 `-j` 的调度策略——本文只讲"看得见的规则语言"。
- **`define` / `eval` / `call` 的完整语义**：本文只提 `$(shell ...)` 与 `$(info ...)` / `$(warning ...)` / `$(error ...)`。
- **其它构建系统**：CMake 的生成器机制另见 `docs/cmake-build-system.md`；本文只讲手写 Makefile。
- **非 GNU make 的方言**：`nmake`（Windows）、BSD make 的差异不在本文范围。
- **条件编译与平台矩阵**：`uname` 分支只给了一个跨平台最小示例，不覆盖 macOS/Linux/Windows 三方的完整配置。
- **打包与安装**：`install` 目标只出现一个 `cp` 的最简形式，不涉及 DESTDIR、权限与卸载。
- **递归 Makefile**（子目录各自一份 + `$(MAKE)` 递归）：本文只做单层 Makefile。

### §5 观测点

| 时机 | 命令 / 键 | 看到什么算正常 |
|---|---|---|
| 想知道 make 到底要执行什么 | `make -n` | 打印全部配方命令，一行不执行 |
| 想知道某个变量的最终值 | `make -p 2>/dev/null \| grep "^CXXFLAGS"` | 打出展开后的完整值 |
| 想看全部变量与规则 | `make -p` | 很长，但含所有隐式规则 |
| 想看 make 的决策过程 | `make -d` | 极详细的调试输出（含"为什么重建"） |
| 排查 Tab / 空格问题 | `cat -A Makefile \| grep "    "` | Tab 显示为 `^I`；配方行应以 `^I` 开头 |
| 想看完整命令而非 `@echo` 美化输出 | `make V=1`（**需要 Makefile 自己支持**，见第十二章） | 打出完整编译命令行 |
| 排查"改了没重编" | `ls -l --time-style=full-iso build/lexer.o src/lexer.cpp include/token.h` | 比对三个时间戳 |
| 确认自动依赖生成了 | `cat build/lexer.d` | 一行 `build/lexer.o: src/lexer.cpp include/lexer.h …` |

---

## 第二部分：详细设计（怎么做）

### §6 整体流程图

分解视角：把 make 跑一轮拆成四个子流程，框内只写子流程名。

```
┌───────────────────────────────┐
│ 读取子流程                     │
│ Makefile / 命令行 / 环境变量    │
└──────────────┬────────────────┘
               ▼
┌───────────────────────────────┐
│ 规则匹配子流程                 │
│ 目标 → 找配方 / 找依赖         │
└──────────────┬────────────────┘
               │ 依赖先建
               ▼
┌───────────────────────────────┐
│ 时间戳判定子流程               │
│ 不存在 / 有新依赖 / 全旧        │
└──────────────┬────────────────┘
               │ 判定为"要重建"
               ▼
┌───────────────────────────────┐
│ 配方执行子流程                 │
│ Tab 开头 → 交 shell → @ 抑制回显 │
│ -j 时并行                      │
└───────────────────────────────┘
```

★ 与 §2 的差别：§2 是**使用者视角**（敲什么命令、看到什么）；
这里是**分解视角**（哪一步会断、每个子流程读什么写什么）。

### §7 子流程 + 说人话步骤

#### 7.1 读取子流程

```
→ 读 Makefile
   → 读命令行（make 目标名 / make VAR=value）
      → 读环境变量
         → 三处合并成一个变量表
```

1. **规则全部来自 Makefile**；Makefile 一般以 `Makefile` 或 `makefile` 命名，也可以用 `make -f <文件>` 指定别的。
2. **命令行上的变量优先级最高**：`make CXX=g++` 会盖掉 Makefile 里的 `CXX = clang++`。
3. **环境变量排在 Makefile 之后**：`CXX=g++ make` 能换编译器，但 Makefile 里显式写死的 `CXX = clang++` 会盖掉环境变量。【为什么】所以更推荐用 `?=`（只在变量未定义时赋值）或在命令行上覆盖。
4. **特殊目标 `.PHONY` 不产生文件**，只声明"这些名字是动作"。

#### 7.2 规则匹配子流程

```
→ 要建的目标叫 X
   → 有没有现成的规则？
      ├─ 有显式规则（X: 依赖…）      → 用它
      ├─ 有模式规则（%.o: %.cpp）    → 用它，% 代入 X 的相应部分
      └─ 都没有                        → *** No rule to make target
```

1. **显式规则优先**于模式规则。
2. **模式规则**用 `%` 匹配任意一段：`build/%.o: src/%.cpp` 能匹配 `build/lexer.o` ← `src/lexer.cpp`。
3. **静态模式规则**把模式限定在一份清单上：`$(OBJECTS): build/%.o: src/%.cpp`，只有清单里的文件走这条。
4. **找不到规则就报错停下**（GNU Make 4.3：`make: *** No rule to make target 'xxx'.  Stop.`）。

#### 7.3 时间戳判定子流程

```
→ 目标 X 要建
   → X 这个文件存在吗？
      ├─ 不存在        → 要重建
      └─ 存在 → 有依赖比 X 新吗？
                 ├─ 有  → 要重建
                 └─ 没有 → "已经是最新的"，跳过
```

1. **目标不存在 ⇒ 一定重建**。
2. **目标存在 ⇒ 逐个比依赖的修改时间**，任何一个比目标新就重建。
3. **全部依赖都比目标旧 ⇒ 什么都不做**，make 打印 `make: 'minicc' is up to date.`。
4. **make 自己不存任何状态**：状态就是文件系统的时间戳。【为什么】这也是为什么 `-t`（touch 时间戳）能骗过 make。
5. **顺序依赖不参与这一步**：只有 `|` 前面的普通依赖参与时间戳比较。

#### 7.4 配方执行子流程

```
→ 判定"要重建"
   → 把配方逐行交给 shell
      → 行首有 @ ⇒ 不回显；没有 ⇒ 回显
         → 有一条失败 ⇒ 停下（除非 -k）
```

1. **配方行必须以 Tab 开头**（不是空格）。
2. **行首的 `@` 抑制回显**；正文第十一章用 `@echo` 做美化输出就是这样来的。
3. **某条命令返回非零 ⇒ make 停下**，`-k` 可以让它"尽量多编译"。
4. **`-j<n>` 时互不依赖的目标并行**。
5. **`.DELETE_ON_ERROR`** 可以让命令失败时删掉半成品目标文件。

### §8 异常流转表

| 异常 | 触发点 | 状态/字段变化 | 恢复路径 |
|---|---|---|---|
| 配方行用空格缩进 | 读取子流程解析规则时，发现配方行没有以 Tab 开头 | make 在解析阶段就停；**一个配方也没执行** | 把空格换成 Tab（`cat -A` 查），重跑 make |
| 依赖文件不存在 | 规则匹配子流程找不到满足依赖的规则 | 同样在解析/匹配阶段停；已建好的其它目标不受影响 | 修依赖名，或用 `$(wildcard ...)` 只列真实存在的文件 |
| 目标名撞上同名文件 | 时间戳判定子流程第 3 步，目标是文件且没有依赖 | make 跳过该目标，什么也不做 | 加 `.PHONY: <目标名>` |
| 改了头文件没重编 | 时间戳判定子流程第 2 步，头文件不在依赖列表里 | `.o` 与源码不一致；**make 认为一切正常** | 加 `-MMD -MP` 生成 `.d` 并 `-include` 它 |
| 目录 mtime 触发全体重编 | 时间戳判定子流程第 2 步，`build/` 被当成普通依赖 | 全部 `.o` 被重编（慢，但结果正确） | 把 `build/` 改成顺序依赖 `\|` |
| 配方命令失败 | 配方执行子流程第 3 步 | 该目标不更新；make 停止（除非 `-k`） | 看报错修命令；`-k` 可以先看还有哪些会挂 |

### §9 目标状态机 / 持久状态

make **没有自己的状态库**——它的全部状态就是文件系统上的文件与时间戳。
下面是每个目标在一次 make 运行中的判定状态（行式）：

```
目标文件不存在 ──────────────▶ 【要重建】(执行配方)
      │
      │ 存在
      ▼
有依赖比目标新 ──────────────▶ 【要重建】(执行配方)
      │
      │ 没有任何依赖比目标新
      ▼
【已最新】(跳过配方，不打印命令)
```

**持久状态（就是磁盘上这些东西）**：

| 文件 | 谁写 | 谁读 | 作用 |
|---|---|---|---|
| `build/*.o` | 编译器 | 链接器 + make（看时间戳） | 目标文件 |
| `build/*.d` | 编译器（`-MMD`） | make（`-include` 读进来） | 头文件依赖清单 |
| `Makefile` | 人 | make | 规则与变量 |
| `build/minicc` | 链接器 | 人 | 最终产物 |

**空值语义**：

| 空值 | 含义 |
|---|---|
| `.d` 文件不存在 | 不是错误——`-include` 的前置 `-` 就是"不存在也不报错"；此时该 `.o` 只能靠 Makefile 里手写的依赖判定 |
| 目标文件存在但依赖列表为空 | make 判定"已最新"，永远不重建（这正是 `.PHONY` 要解决的问题） |
| 变量未定义 | 展开成空串（不是报错）；`?=` 正是用来"未定义时才赋值"的 |

### §10 数据形状

Makefile 的规则形状（本文所有片段的统一抽象）：

```jsonc
{
  "规则": {
    "目标": "build/minicc",              // 要得到什么
    "依赖": ["build/lexer.o", "build/parser.o"],  // 需要什么（普通依赖参与时间戳比较）
    "顺序依赖": ["build"],               // | 之后的：只保证存在，不参与时间戳比较
    "配方": ["clang++ $^ -o $@ $(LDFLAGS)"]     // 怎么得到（每行必须以 Tab 开头）
  },
  "变量": {
    "CXX": "clang++",                   // 编译器
    "CXXFLAGS": "-std=c++20 -Wall -Wextra -pedantic -g -MMD -MP",
    "INCLUDES": "-Iinclude",            // 头文件搜索路径
    "LDFLAGS": "-L/opt/homebrew/opt/llvm/lib/c++",   // 库搜索路径
    "SOURCES": "src/lexer.cpp src/parser.cpp …",     // 源文件清单
    "OBJECTS": "$(SOURCES:.cpp=.o)"      // 后缀替换得到的清单
  },
  "自动变量": {
    "$@": "当前目标的文件名（build/minicc）",
    "$<": "第一个依赖（build/lexer.o）",
    "$^": "全部依赖，去重（build/lexer.o build/parser.o）",
    "$?": "比目标新的依赖（只有它变了时）",
    "$*": "模式规则里 % 匹配到的那一段"
  },
  "特殊目标": {
    ".PHONY": ["all", "clean", "install", "uninstall", "test", "help"],
    ".SUFFIXES": "清空或设置后缀规则",
    ".DEFAULT": "默认规则",
    ".DELETE_ON_ERROR": "命令失败时删除目标文件"
  }
}
```

### §11 接口契约

**make 命令行契约**

| 命令 / 参数 | 含义 | 契约 |
|---|---|---|
| `make` | 构建默认目标（Makefile 里的第一个目标） | 没有默认目标时报 `No targets specified` |
| `make <目标>` | 构建指定目标 | 目标不存在时报 `No rule to make target` |
| `make -j<n>` | 并行构建 n 路 | `-j` 不带数字 = 不限并发（本机 8 核建议 `-j6`，留 2 核给交互任务） |
| `make -n` | dry-run：只打印命令 | 一条都不执行 |
| `make -B` | 强制重建全部目标 | 无视时间戳 |
| `make -k` | 出错继续 | 尽量多编译，把所有错误一次性暴露 |
| `make -C <目录>` | 切到该目录再执行 | |
| `make -f <文件>` | 用指定 Makefile | 默认找 `Makefile` 或 `makefile` |
| `make VAR=value` | 覆盖变量 | 优先级最高 |

**Makefile 里的变量契约**（正文第十一章实战 Makefile 用的那一组）：

| 变量 | 谁读它 | 约定 |
|---|---|---|
| `CXX` | 编译配方 | 编译器可执行文件名或路径 |
| `CXXFLAGS` | 编译配方 | 编译选项（含 `-std=c++20`、`-MMD -MP`） |
| `INCLUDES` | 编译配方 | 头文件搜索路径（`-I` / `-isystem`） |
| `LDFLAGS` | 链接配方 | 库搜索路径与链接选项（`-L`） |
| `SRCS` / `OBJS` / `DEPS` | 规则依赖 | 分别是源文件、目标文件、依赖文件三份清单 |

**边界（谁说了算）**：**Makefile 说了算，命令行与环境变量只能覆盖**；
反过来不成立——Makefile 里写死的 `CXX = clang++` 会盖掉环境里的 `CXX=g++`。

### §12 否决记录

| 被否方案 | 否决原因 | 谁否的 / 什么时候 |
|---|---|---|
| 手写每个 `.o` 依赖哪些 `.h` | 头文件一多必漏，漏了就"改了没重编"。改用编译器生成的 `.d` 文件（`-MMD -MP`）+ `-include` 读进来 | 正文第九章「自动生成头文件依赖」 |
| 直接写 `clean:` 不加 `.PHONY` | 目录里恰好有个叫 `clean` 的文件时，make 说 `'clean' is up to date.` 什么也不做 | 正文第八章「phony 目标」 |
| 把 `build/` 写成普通依赖 | 目录 mtime 一变（哪怕只是新增文件）就触发全体重编；改成顺序依赖 `\|` 后它只保证"命令执行前目录存在" | 正文第十章「顺序依赖」 |
| 用 `$(wildcard)` 硬列头文件依赖 | 仍然要人维护；`.d` 文件是编译器吐的，永远跟得上 | 正文第十二章「常见陷阱」的"✅ 用通配符更安全"（更稳的解法见第九章 `.d`） |
| 用空格缩进配方行 | make 只认 Tab，空格会报 `missing separator` | 正文第二章与第十二章 |

---

## 第一章：Make 是什么

Make 是一个**构建自动化工具**。你告诉它：

- **目标**（target）：你想得到什么（比如 `minicc` 可执行文件）
- **依赖**（dependencies）：得到它需要什么（比如 `.o` 文件）
- **命令**（recipe）：怎么从依赖得到目标（比如 `clang++ -o minicc *.o`）

Make 会**自动判断哪些文件需要重新编译**——只重建过期的部分，不浪费时间去碰没改过的文件。

### 为什么需要 Make？

```
没有 Make 的世界：
  每次都要手动敲：
    clang++ -std=c++20 -Iinclude -c src/lexer.cpp -o build/lexer.o
    clang++ -std=c++20 -Iinclude -c src/parser.cpp -o build/parser.o
    clang++ -std=c++20 -Iinclude -c src/type.cpp -o build/type.o
    clang++ -std=c++20 -Iinclude -c src/semantic_analyzer.cpp -o build/sema.o
    clang++ -std=c++20 -Iinclude -c src/template_instantiation.cpp -o build/tmpl.o
    clang++ -std=c++20 -Iinclude -c src/codegen.cpp -o build/codegen.o
    clang++ -std=c++20 -Iinclude -c src/main.cpp -o build/main.o
    clang++ build/*.o -o build/minicc

  改了 lexer.cpp 一个文件，上面 8 条命令全部重跑一遍……

有了 Make：
  敲一个词: make
  Make 自动检测只改了 lexer.cpp，只编译那一个文件 + 重新链接
```

---

## 第二章：Makefile 基本语法

### 最小的 Makefile

```makefile
# 目标: 依赖
# ←Tab→命令

hello: hello.cpp
	clang++ hello.cpp -o hello
```

**注意：命令前必须是 Tab 字符，不能是空格！** 这是 Make 最经典的坑。

### 运行

```bash
make hello    # 构建 hello 目标
make          # 构建 Makefile 中的第一个目标
```

### 多个目标

```makefile
# 默认目标（make 不带参数时执行这个）
all: hello goodbye

hello: hello.cpp
	clang++ hello.cpp -o hello

goodbye: goodbye.cpp
	clang++ goodbye.cpp -o goodbye

clean:
	rm -f hello goodbye
```

```bash
make           # 构建 all → 构建 hello + goodbye
make hello     # 只构建 hello
make clean     # 删除编译产物
```

### 规则的结构

```
目标(target): 依赖(dependencies)
←Tab→命令(recipe)
←Tab→命令(recipe)
```

- **目标**：要生成的文件名，或者一个动作的名字（如 `clean`）
- **依赖**：目标依赖的文件列表（空格分隔）
- **命令**：如何从依赖生成目标的 shell 命令（每行一个，必须以 Tab 开头）

### 执行逻辑

```
make 执行一个目标时：
  1. 检查目标文件是否存在
  2. 如果不存在 → 执行命令
  3. 如果存在 → 比较目标文件和依赖文件的修改时间
     ├── 目标比所有依赖都新 → "已经是最新的"，不执行
     └── 任何一个依赖比目标新 → 执行命令（重新构建）
```

---

## 第三章：变量

### 定义和使用

```makefile
# 定义变量（= 或 :=）
CXX = clang++
CXXFLAGS = -std=c++20 -Wall
SOURCES = src/lexer.cpp src/parser.cpp src/main.cpp

# 使用变量 $(变量名) 或 ${变量名}
hello: hello.cpp
	$(CXX) $(CXXFLAGS) hello.cpp -o hello
```

### 两种赋值方式

```makefile
# =  延迟赋值：使用时才展开（可以引用后面定义的变量）
CXXFLAGS = -Wall $(OPTIMIZE)
OPTIMIZE = -O2
# 使用时 CXXFLAGS 展开为 -Wall -O2

# := 立即赋值：定义时就展开
CXXFLAGS := -Wall $(OPTIMIZE)
OPTIMIZE = -O2
# 使用时 CXXFLAGS 展开为 -Wall（因为定义时 OPTIMIZE 还没值）

# ?= 条件赋值：只在变量未定义时赋值
CXX ?= clang++
# 如果环境变量已经设了 CXX，就不覆盖

# += 追加
CXXFLAGS = -Wall
CXXFLAGS += -Wextra
# CXXFLAGS 现在是 -Wall -Wextra
```

### 实战：minicc 的变量

```makefile
CXX       = clang++
CXXFLAGS  = -std=c++20 -Wall -Wextra -pedantic -g
INCLUDES  = -Iinclude
LDFLAGS   = -L/opt/homebrew/opt/llvm/lib/c++

SOURCES   = src/lexer.cpp src/parser.cpp src/type.cpp \
            src/semantic_analyzer.cpp src/template_instantiation.cpp \
            src/codegen.cpp src/main.cpp
OBJECTS   = $(SOURCES:.cpp=.o)    # 把 .cpp 替换成 .o
TARGET    = minicc
```

### 环境变量

```bash
# Make 会自动读取环境变量
CXX=g++ make              # 用 g++ 而不是 clang++
CXXFLAGS="-O3" make       # 覆盖编译选项

# 也可以在 Makefile 中读取环境变量
ifdef DEBUG
    CXXFLAGS += -DDEBUG -g
endif
```

---

## 第四章：自动变量

Make 在命令执行时自动设置一些特殊变量：

| 变量 | 含义 | 例子 |
|------|------|------|
| `$@` | 目标文件名 | `minicc` |
| `$<` | 第一个依赖文件 | `src/main.cpp` |
| `$^` | 所有依赖文件（去重） | `a.o b.o c.o` |
| `$?` | 比目标新的依赖 | `a.o`（只有 a.o 改了） |
| `$*` | 匹配的模式部分 | 见模式规则 |

### 实际使用

```makefile
# 不用自动变量（啰嗦）
minicc: lexer.o parser.o main.o
	clang++ lexer.o parser.o main.o -o minicc

# 用 $@ 和 $^（简洁）
minicc: lexer.o parser.o main.o
	clang++ $^ -o $@
	# 展开为: clang++ lexer.o parser.o main.o -o minicc

# 用 $< 编译单个文件
lexer.o: src/lexer.cpp
	clang++ -c $< -o $@
	# 展开为: clang++ -c src/lexer.cpp -o lexer.o
```

---

## 第五章：模式规则

### 后缀规则（简单）

```makefile
# 所有 .cpp → .o 的通用规则
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
```

这条规则的含义：
- `%` 是通配符，匹配任意字符串
- `%.o: %.cpp` 表示：任何 `.o` 文件依赖同名的 `.cpp` 文件
- `$<` 是对应的 `.cpp` 文件
- `$@` 是要生成的 `.o` 文件

### 带路径的模式规则

```makefile
# build/ 目录下的 .o 依赖 src/ 目录下的 .cpp
build/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
```

```bash
make build/lexer.o
# 匹配: build/lexer.o 依赖 src/lexer.cpp
# 执行: clang++ -std=c++20 -Wall -Iinclude -c src/lexer.cpp -o build/lexer.o
```

### 静态模式规则（限定范围）

```makefile
OBJECTS = build/lexer.o build/parser.o build/main.o

$(OBJECTS): build/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
```

只对 `$(OBJECTS)` 列表中的文件应用这个规则。

---

## 第六章：内置函数

### 字符串替换

```makefile
SOURCES = src/lexer.cpp src/parser.cpp src/main.cpp

# $(变量:旧后缀=新后缀)
OBJECTS = $(SOURCES:.cpp=.o)
# 结果: src/lexer.o src/parser.o src/main.o

# $(patsubst 模式,替换,文本)
OBJECTS = $(patsubst src/%.cpp,build/%.o,$(SOURCES))
# 结果: build/lexer.o build/parser.o build/main.o
```

### 文件名操作

```makefile
SOURCES = src/lexer.cpp src/parser.cpp src/main.cpp

# $(wildcard 模式) —— 匹配文件
SRCS = $(wildcard src/*.cpp)
# 结果: src/lexer.cpp src/parser.cpp src/type.cpp ...

# $(notdir 路径) —— 去掉目录部分
$(notdir src/lexer.cpp)    # 结果: lexer.cpp

# $(dir 路径) —— 只保留目录部分
$(dir src/lexer.cpp)       # 结果: src/

# $(basename 文件名) —— 去掉后缀
$(basename src/lexer.cpp)  # 结果: src/lexer

# $(suffix 文件名) —— 只保留后缀
$(suffix src/lexer.cpp)    # 结果: .cpp
```

### 列表操作

```makefile
# $(sort 列表) —— 排序并去重
$(sort c b a c)            # 结果: a b c

# $(words 列表) —— 计数
$(words src/lexer.cpp src/parser.cpp)  # 结果: 2

# $(firstword 列表) —— 第一个
$(firstword a b c)         # 结果: a

# $(filter 模式,列表) —— 过滤
$(filter %.cpp,$(SOURCES)) # 只保留 .cpp 文件
```

### Shell 命令

```makefile
# $(shell 命令) —— 执行 shell 命令
CORES = $(shell sysctl -n hw.ncpu)
DATE  = $(shell date +%Y%m%d)
```

---

## 第七章：条件判断

```makefile
DEBUG = 1

# ifeq / ifneq —— 字符串比较
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g -DDEBUG
else
    CXXFLAGS += -O2
endif

# ifdef / ifndef —— 变量是否定义
ifdef VERBOSE
    $(info 详细模式已开启)
endif

ifndef CXX
    CXX = clang++
endif
```

### 实战：跨平台

```makefile
UNAME = $(shell uname)

ifeq ($(UNAME), Darwin)
    # macOS
    LDFLAGS = -L/opt/homebrew/opt/llvm/lib/c++
    CXXFLAGS += -arch arm64
else ifeq ($(UNAME), Linux)
    # Linux
    LDFLAGS =
    CXXFLAGS +=
endif
```

---

## 第八章：phony 目标

### 问题

```makefile
clean:
	rm -f *.o minicc
```

如果当前目录恰好有个文件叫 `clean`，Make 会说：

```
make: 'clean' is up to date.
```

因为它以为 `clean` 是一个文件目标，而且这个"文件"没有依赖，所以是"最新的"。

### 解决

```makefile
.PHONY: clean all install

clean:
	rm -f *.o minicc

all: minicc

install: minicc
	cp minicc /usr/local/bin/
```

`.PHONY` 告诉 Make：这些目标不是文件，是动作，每次都执行。

### 常见的 phony 目标

```makefile
.PHONY: all clean install uninstall test help

all: minicc              # 默认目标
clean:                   # 清理编译产物
	rm -rf build/*.o build/minicc
install: minicc          # 安装
	cp minicc /usr/local/bin/
test: minicc             # 运行测试
	./run_tests.sh
help:                    # 显示帮助
	@echo "可用目标:"
	@echo "  make          - 编译 minicc"
	@echo "  make clean    - 清理"
	@echo "  make test     - 运行测试"
	@echo "  make install  - 安装"
```

---

## 第九章：依赖与增量编译

### Make 的核心逻辑

```
目标: 依赖1 依赖2 依赖3
	命令

Make 的判断流程:
  1. 目标文件不存在？ → 执行命令
  2. 目标文件存在，但依赖1比目标新？ → 执行命令
  3. 目标文件存在，所有依赖都比目标旧？ → 不执行（"已经是最新的"）
```

### 增量编译示例

```makefile
minicc: lexer.o parser.o main.o
	clang++ $^ -o $@

lexer.o: src/lexer.cpp include/lexer.h include/token.h
	clang++ -c $< -o $@

parser.o: src/parser.cpp include/parser.h include/ast.h
	clang++ -c $< -o $@

main.o: src/main.cpp include/lexer.h include/parser.h
	clang++ -c $< -o $@
```

```bash
# 第一次 make
make
# 编译 lexer.o parser.o main.o + 链接 minicc（全部重新编译）

# 只改了 src/lexer.cpp
make
# 只编译 lexer.o + 重新链接 minicc（parser.o main.o 不动）

# 只改了 include/token.h
make
# 只编译 lexer.o + 重新链接 minicc（因为 lexer.o 依赖 token.h）

# 什么都没改
make
# "make: 'minicc' is up to date."
```

### 自动生成头文件依赖

手动写每个 `.o` 依赖哪些 `.h` 太痛苦了。编译器可以帮你生成：

```makefile
# -MMD 让编译器自动生成 .d 依赖文件
CXXFLAGS = -std=c++20 -Wall -MMD -MP

%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# 包含自动生成的依赖文件（- 表示文件不存在也不报错）
-include $(OBJECTS:.o=.d)
```

编译器生成的 `.d` 文件长这样：

```makefile
# build/lexer.d
build/lexer.o: src/lexer.cpp include/lexer.h include/token.h
```

这样改了任何头文件，Make 都能自动检测到。

---

## 第十章：多目录项目

### 目录结构

```
mycompiler/
├── Makefile
├── include/
│   ├── lexer.h
│   └── parser.h
├── src/
│   ├── lexer.cpp
│   ├── parser.cpp
│   └── main.cpp
└── build/         ← 编译产物放这里
```

### Makefile

```makefile
CXX      = clang++
CXXFLAGS = -std=c++20 -Wall -Wextra -g -MMD
INCLUDES = -Iinclude

SRCDIR   = src
BUILDDIR = build
TARGET   = $(BUILDDIR)/minicc

# 自动扫描所有 .cpp 文件
SRCS = $(wildcard $(SRCDIR)/*.cpp)
# src/lexer.cpp src/parser.cpp src/main.cpp

# 转换成 build/ 目录下的 .o 文件
OBJS = $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))
# build/lexer.o build/parser.o build/main.o

# 默认目标
all: $(TARGET)

# 链接
$(TARGET): $(OBJS) | $(BUILDDIR)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# 编译（build 目录下的 .o 依赖 src 目录下的 .cpp）
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# 创建 build 目录（| 表示"顺序依赖"，只保证目录存在）
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# 清理
clean:
	rm -rf $(BUILDDIR)

# 包含自动生成的依赖
-include $(OBJS:.o=.d)

.PHONY: all clean
```

### 顺序依赖（Order-Only Prerequisites）

```makefile
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
                                   ↑
                              这个是顺序依赖
```

`|` 后面的依赖只保证在命令执行前存在，**不参与时间戳比较**。

如果不加 `|`：每次 `build/` 目录的修改时间变了（比如新增了文件），所有 `.o` 都会重新编译——因为 `build/` 比 `.o` 新了。

---

## 第十一章：实战——手写一个 minicc 的 Makefile

不用 CMake，纯手写一个完整功能的 Makefile：

```makefile
# =============================================================================
# minicc 的 Makefile（纯手写版，不依赖 CMake）
# =============================================================================

# ─── 编译器设置 ───────────────────────────────────────────────────────────────
CXX       = /opt/homebrew/opt/llvm/bin/clang++
CXXFLAGS  = -std=c++20 -Wall -Wextra -pedantic -g -MMD -MP
INCLUDES  = -Iinclude -isystem /opt/homebrew/opt/llvm/include/c++/v1
LDFLAGS   = -L/opt/homebrew/opt/llvm/lib/c++

# ─── 目录设置 ─────────────────────────────────────────────────────────────────
SRCDIR    = src
INCDIR    = include
BUILDDIR  = build

# ─── 文件设置 ─────────────────────────────────────────────────────────────────
TARGET    = $(BUILDDIR)/minicc
SRCS      = $(wildcard $(SRCDIR)/*.cpp)
OBJS      = $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))
DEPS      = $(OBJS:.o=.d)

# ─── 默认目标 ─────────────────────────────────────────────────────────────────
all: $(TARGET)

# ─── 链接：所有 .o → minicc ──────────────────────────────────────────────────
$(TARGET): $(OBJS) | $(BUILDDIR)
	@echo "  LINK    $@"
	@$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# ─── 编译：每个 .cpp → .o ────────────────────────────────────────────────────
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	@echo "  CXX     $< → $@"
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ─── 创建构建目录 ─────────────────────────────────────────────────────────────
$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

# ─── 清理 ─────────────────────────────────────────────────────────────────────
clean:
	@echo "  CLEAN   $(BUILDDIR)/"
	@rm -rf $(BUILDDIR)

# ─── 运行测试 ─────────────────────────────────────────────────────────────────
test: $(TARGET)
	@echo "  TEST    running all template tests..."
	@for f in tests/test_tmpl_*.cpp; do \
	    echo "  RUN     $$f"; \
	    $(TARGET) "$$f" -o /dev/null 2>/dev/null && \
	        echo "  ✅      $$(basename $$f)" || \
	        echo "  ❌      $$(basename $$f)"; \
	done

# ─── 帮助 ─────────────────────────────────────────────────────────────────────
help:
	@echo ""
	@echo "  minicc 构建系统"
	@echo "  ════════════════════════════════════"
	@echo "  make          编译 minicc 编译器"
	@echo "  make clean    清理编译产物"
	@echo "  make test     运行所有模板测试"
	@echo "  make help     显示本帮助"
	@echo ""

# ─── 包含自动生成的头文件依赖 ─────────────────────────────────────────────────
-include $(DEPS)

# ─── phony 目标 ───────────────────────────────────────────────────────────────
.PHONY: all clean test help
```

### 使用

```bash
make              # 编译
make test         # 跑测试
make clean        # 清理
make help         # 看帮助
make VERBOSE=1    # 显示完整命令（需要去掉 @ 前缀）
```

### 显示完整命令

上面用了 `@echo` 来美化输出。想看完整命令：

```bash
make -n           # 只打印命令，不执行（dry-run）
make V=1          # 需要自己在 Makefile 里支持（见下方）
```

支持 `V=1` 显示完整命令：

```makefile
# 在文件开头加:
ifeq ($(V),1)
    Q =
else
    Q = @
endif

# 然后把所有 @ 换成 $(Q)
$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp | $(BUILDDIR)
	$(Q)echo "  CXX     $< → $@"
	$(Q)$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
```

```bash
make         # 简洁输出: "  CXX     src/lexer.cpp → build/lexer.o"
make V=1     # 完整输出: "/opt/homebrew/opt/llvm/bin/clang++ -std=c++20 ..."
```

---

## 第十二章：make 命令行技巧

### 常用选项

```bash
make                  # 构建默认目标
make target           # 构建指定目标
make -j8              # 8 个并行任务（加速编译）
make -j$(sysctl -n hw.ncpu)  # 自动检测 CPU 核心数

make -n               # dry-run：只打印命令，不执行
make -B               # 强制重建所有目标（忽略时间戳）
make -k               # 遇到错误继续（尽量多编译）

make -C build         # 切换到 build/ 目录执行 make
make -f MyMakefile    # 使用指定的 Makefile（默认用 Makefile 或 makefile）

make VAR=value        # 覆盖变量
make CXX=g++          # 用 g++ 编译
make CXXFLAGS="-O3"   # 覆盖编译选项
```

### 调试 Makefile

```bash
# 查看变量的值
make -p 2>/dev/null | grep "^CXXFLAGS"

# 打印所有变量
make -p

# 查看 make 会执行什么（不真正执行）
make -n

# 查看依赖关系图
make -d              # 极详细的调试输出

# 在 Makefile 中打印变量值
$(info CXXFLAGS = $(CXXFLAGS))
$(warning 这是一个警告)
$(error 这是一个错误，会停止构建)
```

### 常见陷阱

```
❌ 错误: 命令前用了空格而不是 Tab
   Makefile:
       target:
           echo "hello"     ← 4个空格，报错！

   ✅ 正确:
       target:
    ←Tab→echo "hello"       ← 必须是 Tab

   检查方法: cat -A Makefile | grep "    "  （Tab 显示为 ^I）
```

```
❌ 错误: 变量赋值多了空格
   CXX = clang++       ← 正确
   CXX = clang++       ← 也正确（= 两边的空格被忽略）
   CXX=clang++         ← 也正确

   但注意:
   CXXFLAGS = -Wall    ← CXXFLAGS 的值是 "-Wall"（没有前导空格）
   CXXFLAGS =  -Wall   ← CXXFLAGS 的值是 " -Wall"（有前导空格！）
```

```
❌ 错误: 依赖写错了
   main.o: main.cpp header.h    ← header.h 不存在
   # make 报错: *** No rule to make target 'header.h'

   ✅ 用通配符更安全:
   HEADERS = $(wildcard include/*.h)
   main.o: main.cpp $(HEADERS)
```

---

## 速查表

### 变量

| 语法 | 含义 |
|------|------|
| `VAR = value` | 延迟赋值 |
| `VAR := value` | 立即赋值 |
| `VAR ?= value` | 条件赋值（未定义时） |
| `VAR += value` | 追加 |
| `$(VAR)` | 使用变量 |

### 自动变量

| 变量 | 含义 |
|------|------|
| `$@` | 目标文件 |
| `$<` | 第一个依赖 |
| `$^` | 所有依赖（去重） |
| `$?` | 比目标新的依赖 |
| `$*` | 模式匹配的部分 |

### 函数

| 函数 | 用途 |
|------|------|
| `$(wildcard pattern)` | 匹配文件 |
| `$(patsubst from,to,text)` | 模式替换 |
| `$(var:old=new)` | 后缀替换 |
| `$(dir path)` | 取目录部分 |
| `$(notdir path)` | 取文件名部分 |
| `$(basename file)` | 去后缀 |
| `$(suffix file)` | 取后缀 |
| `$(sort list)` | 排序去重 |
| `$(words list)` | 计数 |
| `$(shell cmd)` | 执行 shell |
| `$(info msg)` | 打印信息 |

### 命令行

| 命令 | 用途 |
|------|------|
| `make` | 构建默认目标 |
| `make target` | 构建指定目标 |
| `make -j8` | 8 并行 |
| `make -n` | dry-run |
| `make -B` | 强制重建 |
| `make clean` | 清理 |
| `make V=1` | 详细输出（需 Makefile 支持） |
| `make -p` | 打印所有变量 |

### 特殊目标

| 目标 | 含义 |
|------|------|
| `.PHONY` | 声明非文件目标 |
| `.SUFFIXES` | 清空/设置后缀规则 |
| `.DEFAULT` | 默认规则 |
| `.DELETE_ON_ERROR` | 命令失败时删除目标文件 |
