# 07 预处理器（P0）

> 主线 P0：#include / #define / 条件编译 / #pragma once。
> 预处理是"编译之前的编译"——C++ 标准 [cpp.phase] 把它定义为独立的翻译阶段 2~4。

## §0 定位与能力清单

预处理的产物是一段**新的文本**：宏被展开了、头文件被并合进来了、条件编译的
死分支被删掉了。后面所有阶段（词法/语法/语义）看到的都是它，而不是用户写的那份源文件。
本文讲 minicc 的这一段：`#include` 怎么找文件、`#define` 怎么展开、
条件编译怎么求值、`#pragma once` 怎么去重。

| 能力 | 一句话 | 状态 |
|---|---|---|
| [cpp.phase] 翻译阶段 2~4 | 行拼接 → 指令行识别 → 宏展开 + include 并合 | 已实现（**文本级**，非 token 级） |
| [cpp.include] `#include` 搜索 | `"..."` 先搜包含者所在目录再搜 `-I`；`<...>` 直接搜 `-I` | 已实现 |
| [cpp.replace]/[cpp.subst] 对象宏与函数宏 | 参数按**词边界**替换；实参先展开（`#`/`##` 除外） | 已实现 |
| [cpp.rescan] 重扫描与 hide 集 | 展开结果继续扫描找宏，正在展开的宏名被"涂蓝" | 已实现（hide 集按值传递） |
| [cpp.cond] 条件编译 | `#if/#ifdef/#ifndef/#elif/#else/#endif`，`takenBranch` 保证分支互斥 | 已实现 |
| [cpp.cond] `defined(X)` 先于展开求值 | 否则 `defined` 本身可能被展开掉 | 已实现 |
| [cpp.cond] 未定义标识符按 0 | `#if 某平台宏` 是安全写法 | 已实现 |
| `#if` 表达式求值 | 三步：defined → 宏展开 → 递归下降（`\|\|` `&&` 比较 加减乘除 一元 `!`） | 已实现 |
| `#pragma once` | 按 canonical 路径去重，免手写 include guard | 已实现 |
| `#error` | 命中即报错并终止 | 已实现 |
| `-E` | 只输出预处理结果，不进后续阶段 | 已实现 |
| 动态宏 `__LINE__` / `__FILE__` | 就地生成 | 已实现 |
| 内建静态宏 `__MINICC__` | 构造期注入宏表，值恒 1 | 已实现（learn/37） |
| `#` 字符串化 / `##` 粘贴 | 记号级操作 | 未做 |
| 变参宏 `__VA_ARGS__` / `__VA_OPT__` | | 未做 |
| `_Pragma` 与诊断类 `#pragma` | | 未做 |
| `#line` 与 linemarker | 指令行目前只输出空行占位，行号会随 include 漂移 | 未做 |
| `#include_next` | | 未做 |
| module map / framework 搜索 | | 未做 |
| `#assert` / `#unassert` / `#ident` / `#sccs` | | 未做 |
| 传统 include guard 的优化识别 | clang 识别出 guard 模式后等价于 pragma once | 未做 |

## ① 理论背景

**标准依据**：[cpp.phase]（翻译阶段）、[cpp.include]、[cpp.cond]、[cpp.replace]/[cpp.subst]/[cpp.rescan]。

- **翻译阶段视角**：阶段 2 行拼接（`\`+换行）→ 阶段 3 拆分为预处理记号、
  识别指令行 → 阶段 4 宏展开 + include 并合。预处理完成后，
  后续阶段看到的才是"干净"的程序文本。
- **include 搜索算法**（HeaderSearch）：
  `"..."` 先搜**包含者所在目录**（自包含友好），再搜 `-I` 序列；
  `<...>` 直接搜 `-I` → 系统目录。顺序即优先级，全部落空报错并列出搜索过的路径。
- **#pragma once vs include guard**：按文件（canonical 路径）而非宏名去重，
  无需手写 `#ifndef X / #define X`。clang 对传统 guard 也有"多次包含检测"
  优化（识别出 guard 模式后等价于 pragma once）。
- **宏展开三定律**：
  1. **实参先展开**（除非用于 `#`/`##`）——`add(1, ANSWER)` 中 `ANSWER` 先变 42；
  2. **替换按词边界**——参数名在串字面量内、作为更长词的一部分都不替换；
  3. **重扫描 + 涂蓝**（[cpp.rescan]）：展开结果继续扫描找宏，
     但正在展开的宏名被"涂蓝"（hide 集），自引用不再展开——
     这就是 `#define A A` 不会死循环的原因。
- **条件编译语义**：`#elif`/`#else` 互斥靠 `takenBranch` 标志；
  `defined(X)` 必须在宏展开**之前**求值（否则 `defined` 本身可能被展开掉）；
  `#if` 中未定义标识符按 **0** 处理（所以 `#if 某平台宏` 是安全写法）。

## ② 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| **文本级 vs token 级** | 行/文本级：预处理产出展开后的纯文本，再交给既有 Lexer | clang 是 token 级（更精确，能保留记号边界）；文本级实现量小一个数量级，教学上"看得见全文"（`-E`） |
| 行号保持 | 指令行输出空行占位 | include 插入会使主文件后续行号偏移——已知简化，clang 用 linemarker（`# 12 "file"`）解决 |
| hide 集传值 | `expand(text, hide)` 按值传递再 insert | 天然实现作用域式涂蓝，无需手动进出栈 |
| #if 求值 | 三步：defined → 宏展开 → 递归下降（‖/&&/比较/加减乘除/一元!-） | 对照 PPEpressions.cpp 的运算符优先级表裁剪 |
| 不支持项 | `#` 字符串化、`##` 粘贴、变参宏、`_Pragma`、宏内注释语义 | 都是记号级操作，文本级实现代价高、理论增量低，明确写进文档 |

## ③ clang 对照表

| 本实现 | clang 源 | 简化了什么 |
|---|---|---|
| `processText` 指令循环 + 条件栈 | `lib/Lex/PPDirectives.cpp`（指令分派）+ `PPConditionalDirectiveRecord` | 无 `#line`/`#assert`、无诊断 pragma、条件栈只记活跃性 |
| `resolveInclude` | `lib/Basic/HeaderSearch.cpp → LookupFile` | 无 `#include_next`、无 module map、无 framework 搜索、无 guard 宏识别优化 |
| `expand` + `substituteParams` | `lib/Lex/PPMacroExpansion.cpp + TokenConcatenation` | 无 `#`/`##`、无变参 `__VA_ARGS__`、无延迟展开（delayed expansion） |
| `evalConstantExpr` | `lib/Lex/PPExpressions.cpp → EvaluateDirective` | 无字符常量、无枚举/sizeof（标准上 #if 不允许 sizeof，但允许真常量表达式） |
| `#pragma once` 按 canonical 路径 | `HeaderSearch` 的 `ShouldEnterIncludeFile` + 文件 ID 去重 | 无硬链接/符号链接同文件识别（weakly_canonical 已覆盖大部分） |

## ④ 实验手册

```bash
cd /home/magene/runtime/cppproject/mycompiler

# -E：只看预处理结果（同 gcc -E），最直观
./build-linux/minicc tests/test_pp_01_include.cpp -E | head -30

# include + pragma once（日志：skipped (pragma once)）
./build-linux/minicc tests/test_pp_01_include.cpp -o /tmp/pp01.s
gcc /tmp/pp01.s -o /tmp/pp01 && /tmp/pp01; echo $?          # 0

# 宏展开 trace（[pp] expand 逐条）
./build-linux/minicc tests/test_pp_02_macros.cpp 2>&1 | grep "\[pp\] .* expand"

# 条件分支决策（branch taken/skipped）
./build-linux/minicc tests/test_pp_03_conditional.cpp 2>&1 | grep "branch"

# 错误用例：#ifdef 无 #endif
./build-linux/minicc tests/test_pp_04_error_unterminated.cpp; echo $?   # 1

# oracle：与真预处理器对比
clang++-18 -E tests/test_pp_02_macros.cpp | tail -8     # 对照宏展开形态
gcc -E tests/test_pp_03_conditional.cpp | grep -A2 main # 对照条件结果
```

## ⑤ 关键过程图

```
源文本（主文件）
  │ 阶段2：行拼接（'\'+换行）
  ▼
逐行扫描 ──'#'开头?──是──→ 指令分派
  │ 否                        ├─ ifdef/if：条件栈 push（parentActive && eval）
  │                          ├─ define ：MacroDef{name, params?, body} 入表
  │                          ├─ include：resolveInclude 搜索路径
  │                          │           ├─ canonical ∈ pragmaOnce? → skip
  │                          │           └─ 递归 processText(头文件) 并合
  │                          └─ endif：pop（栈空检查在 EOF）
  ▼ 活跃分支的普通行
expand(line, hide={})
  ├─ 词 == 宏名？
  │   ├─ 对象宏：expand(body, hide∪{名})        ← 涂蓝防自引用
  │   └─ 函数宏：收实参(括号配平) → 实参各自 expand
  │             → substituteParams(词边界替换) → 整体重扫描
  └─ __LINE__/__FILE__ → 动态替换
  ▼
展开后纯文本 → Lexer（阶段5 起）
```

## ⑥ 真实版多做了什么、为什么

★ 素材来源：本文 §③ 对照表的「简化了什么」列 + `docs/BUGS.md` B37（可见性裁决）
+ CLAUDE.md「预处理器 P0」条目与「未做」清单。**未查证的写「（本项目未查证）」**。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| **token 级**预处理（clang 的 `TokenConcatenation` / `MacroExpander` 作用于记号流） | 本项目是**文本级**：产出展开后的纯文本再交给既有 Lexer。代价是记号边界可能丢失、字符串字面量里的宏名处理不精确；收益是实现量小一个数量级且 `-E` 的输出**肉眼可读**（教学上"看得见全文"是硬需求） |
| `#`（字符串化）与 `##`（记号粘贴） | 两者都是**记号级**操作，文本级实现代价高、理论增量低——本文 §② 已明确把它们写进"不支持项" |
| 变参宏 `__VA_ARGS__` 与 `__VA_OPT__` | 同上；变参宏还要求实参表能容纳"零个实参"，本项目的函数宏收实参走括号配平，没有零参形态 |
| `linemarker`（`# 12 "file"`）保持行号对应 | 本项目指令行输出**空行占位**，行号会随 include 漂移——这是文本级的固有代价；clang 用 linemarker 精确还原 |
| `#include_next` / module map / framework 搜索 | 属于"真实工程的头搜索"，与本主题的核心（宏展开与条件编译）正交 |
| 传统 include guard 的**模式识别优化** | clang 识别出 `#ifndef X / #define X / ... / #endif` 模式后等价于 pragma once；本项目只认显式 `#pragma once` |
| `#line` / `#assert` / `#unassert` / `#ident` / `#sccs` / 诊断类 `#pragma` | 与"宏展开 + 条件编译 + include"三条主线无关，一律响亮拒收 |
| `Preprocessor::EvaluateDirective` 的**完整**表达式求值 | 本项目只做 `\|\|` `&&` 比较 加减乘除 一元 `!`；无字符常量、无枚举、无 `sizeof`（标准上 `#if` 也不允许 `sizeof`） |
| 宏展开的**延迟展开**（delayed expansion）与 painted/blue 色的精细实现 | 本项目的 hide 集**按值传递再 insert**，天然实现作用域式涂蓝，无需手动进出栈 |
| 跨 `.so` 的同一宏合并 / 内建预定义宏族 | 本项目只内建 `__MINICC__` 一个静态宏（learn/37），值恒 1 |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-08-10 | 预处理器 P0 落地：`#include` 搜索路径 / `#define` 对象+函数宏 / 条件编译 / `#pragma once` / `-E` | 主线 P0 的验收点（测试 tests/test_pp_01..04 + tests/pp/ 头文件夹具） |
| 2026-10-15 | 内建静态宏 `__MINICC__`（learn/37） | 给 ministl 一个"我是 minicc"的双栖开关（`#ifdef __MINICC__` 走自供桩头） |
| 2026-10-09 | 修好 `tests/pp/test_pp_01_include.cpp` 的 `#include` 路径（原先写成 `"pp/math_helper.h"`） | 该路径必然搜不到 ⇒ **`#include`/`#pragma once` 这个 P0 特性从来没有被真正测到**，rc=1 被 logdiff 基线固化成"契约" |
| 2026-11-02 | 名字可见性裁决下沉 Sema（B37） | 预处理出来的文本里的名字同样要走可见性裁决，不能只看"这个 TU 里声明过吗"（learn/48） |
