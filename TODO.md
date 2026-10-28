# TODO —— 未来优化与功能（待办）

> 活文档：完成一项打勾并注明完成日期与 learn 文档编号。
> 优先级判据延续项目定位——**可讲解、可观测优先**；每项给出理论点与 clang 参照。
> 历史主线（A~I）的大部分已完成，留存于 docs/ROADMAP.md 作复盘档案，不再更新。

## A. 后端优化（性价比最高，当前代码生成是学生水平）

代码生成现为栈机式求值（push/pop + SIB 寻址），无寄存器分配、无优化 Pass。
这是与真实小型编译器（tcc 等）差距最大的一块。

| 优先级 | 项 | 理论点 | clang/LLVM 参照 | 验收 |
|---|---|---|---|---|
| ★★★ | **线性扫描寄存器分配** | 活跃区间、空闲列表、spill 代价 | RegAllocLinearScan.cpp | 帧内 push/pop 对数下降；既有 155 份基线汇编允许整体漂移一次（申报后重刷） |
| ★★★ | **窥孔优化** | 冗余 mov 消除、常量操作数折叠、跳转链收敛 | 传统编译教材 Peephole | 每条模式配正负用例 |
| ★★☆ | 死代码消除 | 不可达块（恒定条件已折） | — | `if(false){...}` 不产生代码 |
| ★★☆ | 公共子表达式消除（基本块级） | 可用表达式数据流 | Muchnick §13 | 对拍运行结果不变 |
| ★☆☆ | 尾调用优化 | sibling call 约定 | TailRecursionElim.cpp | 自测递归不爆栈用例 |

## B. 语言广度（C++ 覆盖率扩展）

### B1. 语言核心

| 优先级 | 项 | 理论点 | clang 参照 |
|---|---|---|---|
| ★★★ | **运算符重载** | 成员/非成员重写集（rewrite candidate） | SemaOverload.cpp / ParseCXX.cpp |
| ★★★ | **lambda** | 闭包转换：匿名类 + `operator()` + 捕获列表 | SemaLambda.cpp |
| ★★☆ | constexpr 函数 + if constexpr | [dcl.constexpr] 常量求值器（解释器雏形） | ExprConstant.cpp（clang 常量求值器） |
| ★★☆ | 函数默认实参 | [dcl.fct.default] 缺省实参在调用点回填 | SemaDecl.cpp |
| ★★☆ | multi-declarator `int a = 1, b = 2;` | 声明符列表 | ParseDecl.cpp |
| ★★☆ | 类外成员定义 `int C::f() const {}` | 限定名决议 + const 成员 | SemaDeclCXX.cpp |
| ★☆☆ | 函数形参里的 decltype 依赖表达式 | 依赖上下文两段式 | SemaType.cpp |
| ★☆☆ | `operator|` / `operator||` 那半边 | — | — |

### B2. 深水区（选做）

| 优先级 | 项 | 理论点 | clang 参照 |
|---|---|---|---|
| ★☆☆ | 异常 | Itanium EH ABI：personality、LSDA、zero-cost 展开 | libunwind + CGException.cpp |
| ★☆☆ | 虚继承 | vbptr、菱形布局 | CGRecordLayoutBuilder |
| ☆☆☆ | 概念约束 `requires` | 原子约束合取规范化 | SemaConcept.cpp |

## C. 语义精度（收口已记录的宽方向边界）

设计取向是「宁宽勿窄、响亮拒收」，以下边界从宽改严：

| 优先级 | 项 | 出处 |
|---|---|---|
| ★★☆ | const 写检查（`const a.v = 5` 现放行） | CLAUDE.md ROADMAP ⑥ |
| ★★☆ | 变量遮蔽模板名的精确裁决（现登记即可见） | spec OOS 3 |
| ★★☆ | 派生类到基类的引用绑定 `Base& b = d;` | learn/36 §7 |
| ★★☆ | 转换等级排序 [overload.icp]（整数提升 vs 转换） | learn/36 §7 |
| ★☆☆ | `using Base::f;` 把隐藏名字拉回来 | learn/36 §7 |
| ★☆☆ | 限定名调用成员函数 `d.Base::f(2)`（B21 只管数据成员） | learn/36 §7 |
| ★☆☆ | `v[i]` 值类别按 `at()` 返回类型判定（现按节点种类判） | learn/36 §7 |
| ★☆☆ | 裸名常量表加作用域链（现内层覆盖外层） | learn/40 边界 |

## D. 模板机制补全

| 优先级 | 项 | 理论点 | 参照 |
|---|---|---|---|
| ★★☆ | 形参包 `class... Args` + 包展开 | 模式匹配式替换 | SemaTemplateVariadic.cpp |
| ★★☆ | NTTP 偏特化模式（`Box<int, N>`） | 值位模式合一 | SemaTemplate.cpp |
| ★★☆ | `template<auto V>` 的偏特化模式 | 形态即类型 | — |
| ★☆☆ | 模板位默认实参 `template<template<class> class C = Box>` | [temp.param] | — |
| ★☆☆ | 类外定义特化成员 `template<> int Box<int>::f() {...}` | [temp.expl.spec] | — |
| ★☆☆ | 别名模板偏特化（语法糖决议） | [temp.alias] 边界 | learn/27 §5 |
| ☆☆☆ | 非整型 NTTP（指针/引用/枚举/字面量类） | [temp.arg.nontype] 地址常量性 | ExprConstant.cpp |
| ☆☆☆ | 嵌套模板模板参数（depth > 1） | — | learn/33 §5 |

## E. 健壮性（让"响亮"更可信）

| 优先级 | 项 | 做法 | 验收 |
|---|---|---|---|
| ★★★ | **随机差分 fuzz** | 生成随机小程序（模板/继承/常量表达式组合），minicc 与 clang++-18 对拍 rc + 运行输出 | 每周跑一轮，分歧即新 BUGS 条目 |
| ★★☆ | ministl 对拍扩展 | map/set 已有 5000 步对拍；扩 unordered_map / ranges 视图 | 同框架 |
| ★★☆ | 错误恢复（panic 模式续编） | 同步点恢复，单文件报多个错 | 错例文件一次报全 |
| ★☆☆ | 字面量溢出自动升格（[lex.icon]/2） | 十进制超 int ⇒ long ⇒ long long | 与 clang 形态对拍 |
| ★☆☆ | `\x` / `\u` 转义 | 词法扩展 | 字符用例 |

## F. 工具链与可观测性

| 优先级 | 项 | 说明 |
|---|---|---|
| ★★☆ | `-g` 调试信息（DWARF 子集） | 行号表起步，可 gdb 单步 minicc 产物 |
| ★★☆ | 诊断带源码行打印（`~~~^` 波浪线） | 现仅行列号 |
| ★☆☆ | `--version` / `--help` | 正规 CLI 门面 |
| ★☆☆ | `-c` 多 TU 独立编译 + 链接器合并 | 自研链接器已就位，差驱动接线 |
| ☆☆☆ | 编译耗时统计（per 阶段） | 可观测性传统 |

## 排期建议

1. **近期**（1~2 轮）：A1 寄存器分配（后端从"学生作业"拉到"能用"）+ E1 随机差分 fuzz
2. **中期**：B1 运算符重载 → B1 lambda（.closure 转换是最值的理论点）+ C 组按 ★★ 收口
3. **远期**：深水区按需；D 组模板补全是"主线任务"血统，随时可插

## 完成记录

| 日期 | 完成项 |
|---|---|
| 2026-10-23 | 本文档建立；基线快照：362 单测 / 155 集成基线 / learn 01..47 |
