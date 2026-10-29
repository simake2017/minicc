# minicc —— Mini C++ Compiler

一个用现代 C++20 编写的**教学级 C++ 编译器**：从源码到 x86-64 可执行文件的完整流水线
全部自研——预处理器、Lexer、Parser、语义分析、模板实例化、代码生成、**链接器**。

**核心理念**（项目最高优先级）：这是学习编译器理论的仪器，不是生产工具。
一切设计以「**可讲解、可观测**」优先于「高性能、功能全」：

- 每个阶段输出**中文日志**，推导逐对打印（如 `P=T A=int ⇒ T := int ✓`）；
- 每个新判据先拿 `clang++-18` 当语义 oracle 探针实测，报错文案逐字对齐；
- **日志即契约**：155 份集成基线逐字节比对，重构不许改变任何可观测行为。

> 难度选择：项目主战场是 C++ 里最难的**模板机制**（实参推导 ≈ 合一算法、
> 实例化 ≈ 结构化替换、两阶段查找、重载决议、SFINAE、偏序裁决）——
> 这是"在编译器里再实现一台类型层面的 Prolog"。

## 快照（2026-10-23）

| 指标 | 数值 |
|---|---|
| 核心代码（include + src） | 约 2.4 万行 C++20，全阶段中文日志 |
| 单测 | **362** 例（googletest，断言不变量而非具体符号名） |
| 集成测试 | **155** 份日志基线（编译日志 + 汇编全文 + 退出码 + 运行输出） |
| 学习文档 | docs/learn/ 01..47（每篇点名标准章节 + clang 源码对照表） |
| 配套标准库 | ministl：vector / list / map / set（红黑树）/ 智能指针 / function / algorithm / ranges，与 std 随机对拍 |
| 外部依赖 | 编译期零依赖；运行期借系统 `as` 产 .o，链接与运行时（_start + bump malloc）自研，**不依赖 libc/crt** |

## 快速开始

```bash
# 编译编译器（两种等价方式，都会用 ccache）
./build.sh                      # 自研脚本 → 项目根 ./minicc
# 或
cmake -S . -B build-linux -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache && cmake --build build-linux -j6

# 编译一个测试文件（默认直出可执行文件，内部自动 as + 自研链接器）
./minicc tests/lang/test_basics_01.cpp -o /tmp/t && /tmp/t

# 终端会打印阶段 0~6 的完整编译日志 ✨
```

## 命令行

```text
minicc <source.cpp> [-o output] [-I dir] [-S] [-E]
       [--dump-tokens] [--dump-ast] [--dump-hierarchy] [--dump-layout]

  -S                只输出汇编（跳过链接）
  -E                只跑预处理器（#include 并合 + 宏展开 + 条件编译）
  --dump-tokens    阶段 1 后打印 Token 流（行:列 类型 文本）
  --dump-ast       阶段 2 后打印 AST 树（访问者遍历）
  --dump-hierarchy 类继承层次图（含 typeinfo 链）
  --dump-layout    三层内存布局详图（对象 → vtable → typeinfo）
```

调试观测常用组合（日志量很大，建议 grep）：

```bash
./minicc tests/tmpl/test_tmpl_12.cpp -o /tmp/t 2>&1 | grep "\[deduce\]"   # 推导逐对
./minicc tests/mi/test_mi_04.cpp     -o /tmp/t 2>&1 | grep "vtable"       # 虚表
./minicc tests/lang/test_basics_10_array.cpp -o /tmp/t 2>&1 | grep "layout"  # 布局
```

## 架构：七个阶段

```text
源码 ──► [0] 预处理器 ──► [1] Lexer ──► [2] Parser ──► [3] Sema
                                                        │  （三趟：注册→决议→走查）
        [6] 自研链接器 ◄── [5] CodeGen ◄── [4] 模板实例化 ◄┘
        （节合并/符号决议/  （x86-64 AT&T，
         重定位回填）        借系统 as 产 .o）
```

| 阶段 | 模块 | clang 对照 | 一句话 |
|---|---|---|---|
| 0 预处理 | preprocessor.cpp | Lexer.cpp + PPDirectives | #include 搜索路径 / 对象+函数宏 / 条件编译 / `#pragma once` / 内建 `__MINICC__` |
| 1 词法 | lexer.cpp | Lexer.cpp | 切片即词法；整数字面量的形态解释推迟到 Parser（对照 NumericLiteralParser） |
| 2 语法 | parser.cpp | Parse/ | 递归下降；歧义一律 TokenCheckpoint 试探回卷（对照 TentativeParsingAction） |
| 3 语义 | semantic_analyzer.cpp | Sema/ | 类型检查 / 类布局 / vtable / 符号表 / ADL / 重载决议 |
| 4 实例化 | template_instantiation.cpp + template_deduction.cpp | SemaTemplate | 合一推导 + 结构化替换；SFINAE 软失败独立成 sfinae.cpp |
| 5 代码生成 | codegen.cpp | CodeGenFunction | 栈机式求值，按宽度分派访存；零 RTTI（Visitor + 标签分派双轨） |
| 6 链接 | linker.cpp | LLVM ELF Linker | 合并节 / 符号决议 / 重定位；内置 _start + 64KB bump malloc/free |

**解析期语义识别**（本项目特色机制）：Parser 不持 Sema 句柄（红线），改为
构造期前向收集**名字登记表**（type/namespace/template 三位 + 作用域键，
对齐 `Sema::ClassifyName` 骨架）——`a < b > c;` 比较链、裸 `S;` 声明判定、
限定模板名作用域裁决全靠查这张只读投影（docs/learn/46、47）。

## 支持的特性

### 模板机制（主战场）

| 特性 | 标准章节 | 测试 |
|---|---|---|
| 类模板（显式实参） | [temp.class] | tmpl 01..10（回归红线，永远全绿） |
| 函数模板实参推导 | [temp.deduct] 合一算法 | tmpl 11..19 |
| NTTP（整型家族/字符/`auto`/值位参与偏特化） | [temp.arg.nontype] | tmpl 21..26、61..68 |
| 类模板特化 / 偏特化 / 全特化 / 三路择优 | [temp.spec] | tmpl 27..32 |
| decltype / SFINAE / void_t 探测 / 偏序裁决 | [dcl.type.decltype] CWG 1558 [temp.class.order] | tmpl 33..45 |
| 别名模板（三处解糖 + 推导期 desugar） | [temp.alias] | tmpl 50 |
| CTAD + 推导指引 | [dcl.type.class.deduct] [temp.deduct.guide] | tmpl 51 |
| 模板模板参数（P0522R0 逐位签名） | [temp.arg.template] | tmpl 53、57..60 |
| 成员模板（含类模板体内，两层形参绑定时机） | [temp.mem] | tmpl 52、54、56、69 |
| 依赖类型名 `typename T::type` | [temp.res]/5 | tmpl 49 |

### 语言基础

- 类型系统：auto / 指针 / 左右值引用 / const（cv 位置精确：[learn/23](docs/learn/23-cv-position-and-type-identity.md)）/ `decltype` / 原生数组 `T[N]`（含下标 SIB 寻址）
- 类：构造/析构/初始化列表、虚函数 + vtable + RTTI、public 继承与多继承布局、static 数据成员、类内类型别名
- 函数：重载（类型链择优）、默认实参之外的常用形态、ADL 与限定名查找
- 语句：if / while / do-while / for / switch-case-default / break / continue / 三元 `?:` / 逗号表达式
- 常量：`constexpr` 变量 / 常量折叠 / 常量传播 / 常量表达式作 NTTP 实参
- 声明：全局变量 / 枚举 / 命名空间 / 类型别名 / 命名空间别名 / using
- 宏与条件编译、GCC 风格 mangling（与 clang 逐字符对拍）

### 配套 ministl

`ministl/` 双栖头文件（`#ifdef __MINICC__` 分支）：vector、list、map/set
（红黑树全移植 libc++ `__tree` 算法 + 5000 步随机对拍 std）、unordered_map、
智能指针、function、algorithm、ranges、type_traits。ASan+UBSan 全绿。

## 已知边界（宽方向，显式记录）

设计取向：**宁可响亮拒收，不可静默算错**。以下近似均有文档记录：

- 名字遮蔽不精确裁决（登记即视为可见；遮蔽场景响亮拒收）
- `using Base::f;` 名字拉入、限定名调用成员函数 `d.Base::f(2)` 未做
- const 写检查未做（`const a.v = 5` 宽松放行）
- 诊断为 panic 模式单错误即停（无错误恢复续编）
- 代码生成无寄存器分配与优化 Pass（栈机求值）

完整清单见 [TODO.md](TODO.md) 与各 docs/learn/NN §边界节。

## 测试与验证体系

```bash
./build.sh                          # ① 0 警告
ctest --test-dir build-linux -j6    # ② 362/362
./logdiff.sh diff                   # ③ 155 份基线逐字节零漂移
```

三件套缺一不算完成。纪律（血泪换的，见 [PITFALLS.md](docs/PITFALLS.md)）：

1. **语义 oracle 先行**：写任何用例/判据前先 `clang++-18` 探针；
2. **突变验证**：新判据必须配「摘掉 ⇒ 测试变红」的负向证据；
3. **判据单点**：同一语义判定禁止写两处；
4. **陈旧产物是失败面具**：跑测试前确认被测二进制是刚编的（教训见 learn/47 §6）。

## 项目结构

```text
include/  src/      编译器本体（阶段 0~6 + 名字登记表 + SFINAE 模块）
ministl/            配套标准库（双栖头文件 + 独立 gtest + ASan 构建）
tests/
  lang/ tmpl/ mi/ flow/ fold/ decl/ ctor/ pp/ stl/ rtti/   集成用例（logdiff 收集）
  unit/               googletest 单测
  pp/ search-path/    预处理器 fixtures
docs/
  business/00..08     ★ 业务流水线文档（每个业务一份：定位/判据/代码地图/测试/学习索引）
  learn/01..47        学习文档（理论背景 + 设计决策 + clang 对照 + 可复现实验 + ASCII 图）
  specs/ designs/     业务流水线产物（spec 签字 → 设计 → 代码）
  constraints/        项目约束唯一权威
  BUGS.md  PITFALLS.md  ROADMAP.md  TODO.md
logdiff.sh  build.sh  CMakeLists.txt
```

## 文档索引

| 想学什么 | 读哪 |
|---|---|
| 想按业务直接看（预处理/解析/语义/模板/代码生成/链接/ministl） | **business/00..08**（重点业务 03 解析 / 04 语义 / 05 模板已细分为子文档） |
| 模板推导（合一算法） | learn/01..06（S1~S6 全链条） |
| 预处理器 | learn/07 |
| 构造/析构与顶层声明 | learn/08、15 |
| 多继承布局与虚表 | learn/13、17 |
| NTTP / 特化 / SFINAE | learn/18..21、35 |
| ADL / 别名模板 / CTAD | learn/26、27、28 |
| AST 分派双范式（零 RTTI） | learn/29、REFACTOR-ast-visitor.md |
| 解析期语义识别（名字登记表） | learn/46、47 |
| 所有踩坑复盘 | BUGS.md（B01..B36）、PITFALLS.md |

## 未来计划

见 **[TODO.md](TODO.md)**——按「后端优化 / 语言广度 / 语义精度 / 健壮性 / 工具链」
五类列出，含优先级与理论参照。历史主线规划见 [docs/ROADMAP.md](docs/ROADMAP.md)
（大部分已完成，留存作复盘档案）。

## 常用命令速查

```bash
./build.sh                                # 编译编译器
cmake --build build-linux -j6             # 单测/集成 gtest 目标
ctest --test-dir build-linux -j6          # 全量测试
./logdiff.sh save && ./logdiff.sh diff    # 基线快照 / 比对
clang++-18 -std=c++20 -fsyntax-only x.cpp # 语义 oracle 探针
```
