# 05.5 NTTP —— 非类型模板实参

> 上级：[00-总览](00-总览.md）。

- 读者：加非类型模板参数的开发者
- 审法：值实参的形态必须全程携带；可表示性判据单点（canRepresentValue）
- 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 按 tech-doc 骨架补齐：诊断类别 / Out of Scope / 观测点 |

## §1 管什么（定位）

`template<int N>` 一族：值实参的表示、校验、编码、参与特化模式。

## §2 关键判据（详细设计）

- **TemplateArg tagged 值**取代裸 TypePtr（learn/18）：类型/值混排实参表
  （`Box<T, 4>`）、负数实参、形态校验。实例缓存键吃无损编码（→ 05.2）。
- **类型域扩展**（learn/35）：形参从只认 int 扩到整型家族 + 字符 + `auto`。
  三段独立：① 形参写法（parseBuiltinTypeSpecifierSeq 六组计数器再一次归一，
  对照 clang DeclSpec 位掩码；非法组合各自报错）② 实参字面量（[lex.icon]/2
  全形态）③ 可表示性判据（→ 04.1 canRepresentValue，B17）。
- **`template<auto V>`**（[temp.param]/6）：形态即类型 ⇒ K<4>(int) 与
  K<4L>(long) 是两个实例——形态只在 auto 形参位写入键。
- **常量表达式实参**（learn/39）：`Buf<2+2>`——Parser 只存树不评价
  （[expr.const] 裁决权在语义期，同 clang Expression→Value 形态转换）；
  **求值归一发生在算缓存键之前**，否则 Buf<2+2> 与 Buf<4> 撞/分键都错；
  归一后下游（形态归一/可表示性/mangling）一行未动、既有用例零漂移。
- **常量变量实参**（learn/40）：`Buf<k>` 经编译期常量表重解（→ 04.6）。
- **mangling 与 clang 逐字符相同**（12/12，含 char/signed char/unsigned
  char 三元区分）；TnDa（函数模板 auto NTTP 的 <template-param-decl>）
  有意不写——没有能到达该编码的路径。

## §3 诊断类别（异常处理）

本模块的诊断由 `src/sema/semantic_analyzer.cpp` 的 `error()` 打出（实参字面量的形态解释在
解析期由 `src/parse/parser.cpp` 的 `parseIntLiteral` 完成，但该函数**刻意不打日志**），
输出形态 `[Semantic Error] {行:列}: {文案}`。编译期诊断一律**不可重试**。

| # | 触发情形 | 诊断文案 | 可重试 |
|---|---|---|---|
| 1 | 类模板实参个数多于形参表 | `class template '{名}' expects at most {n} argument(s), but {m} given` | 否 |
| 2 | 类模板实参个数少于形参表（且缺的那个形参无默认值） | `class template '{名}' expects at least {n} argument(s) (parameter '{形参名}' has no default), but {m} given` | 否 |
| 3 | NTTP 形参的类型不在支持域（只支持整型家族与 `auto`） | `non-type template parameter '{形参名}' of '{模板名}' has unsupported type '{类型}' (only integral types and 'auto' are supported)` | 否 |
| 4 | NTTP 实参的类型不是整型 | `non-type template argument for '{模板名}' ('{形参名}') has type '{实际类型}', which is not an integral type (the parameter type is '{形参类型}')` | 否 |
| 5 | NTTP 实参的值不能表示成形参类型（可表示性判据，B17） | `non-type template argument evaluates to {值}, which cannot be narrowed to type '{形参类型}'` | 否 |
| 6 | `auto` 形参的实参没有类型信息 | `cannot deduce the type of non-type template parameter '{形参名}' of '{模板名}' from an untyped value argument` | 否 |
| 7 | NTTP 实参是常量表达式但求值失败（如 `Buf<1/0>`） | `non-type template argument is not a constant expression`（与 clang **逐字相同**） | 否 |
| 8 | 类型形参位收到值实参 | `template argument {位} for '{模板名}' ('{形参名}') must be {}, but '{实参}' is a value` | 否 |
| 9 | NTTP 形参位收到类型实参 | `template argument {位} for '{模板名}' ('{形参名}') must be {}, but '{实参}' is a type` | 否 |
| 10 | 函数模板的显式 NTTP 实参（尚不支持） | `explicit non-type template argument '{值}' for function template '{名}' is not supported yet (only class templates support non-type parameters)` | 否 |

★ 第 5 条是 B17 的核心：判据是**可表示性**（[temp.arg.nontype]/1 → [expr.const]/10 →
[dcl.init]/7），不是"形态精确相等"——旧判据把 clang 认可的 `Flag<1>`（bool←int 1）/
`A<4L>`（int←long 4）/ `A<true>` 三条合法程序全拒了。
★ 第 4、5 条的 clang 原文分别是
`non-type template argument for 'X' ('N') has type 'double', which is not an integral type` 与
`non-type template argument evaluates to 300, which cannot be narrowed to type 'int'`。

## §4 Out of Scope（本模块不做的事）

- **非整型 NTTP 不做**：指针 / 引用 / 枚举 / 字面量类的值实参一律不被接受（形参类型域只有整型家族 + `auto`）。
- **不做字面量溢出自动升格**：[lex.icon]/2 规定十进制 `3000000000` 的类型是 `long`，本项目**不做**这个升格（字面量按切出来的形态原样解释）。
- **不做 `\x` / `\u` 转义**（learn/35 登记的边界）。
- **函数模板上的 NTTP 显式实参不做**（第 10 条诊断：`not supported yet`）——因此 mangling 的 `TnDa`（`<template-param-decl>`）**有意不写**：没有能到达该编码的路径。
- **`template<auto V>` 的偏特化模式不做**：`template<auto V> struct K<V>` 的形态是**形态即类型**（`K<4>`(int) 与 `K<4L>`(long) 是两个实例），但偏特化里的值位模式未实现（见 05-模板/04 §4）。
- **不做 `bool` / `char` 之间的隐式 NTTP 提升之外的其他转换**：可表示性判据只回答"能不能表示"，不回答"该转成什么"。

## §5 观测点（日志键）

| 时机 | 打的键 |
|---|---|
| 模板实参位上的 NTTP 字面量（带形态） | `[parse:targ] ★ non-type argument (NTTP): negative integer {值} (形态 {形态})` |
| NTTP 形参位绑定 | `[sema:targ] ✓ param {位}: '{形参名}' ({形态}) ← {实参}` |
| 常量表达式实参求值（[temp.arg.nontype]/1） | `[sema:targ] ⤷ 常量表达式实参求值（[temp.arg.nontype]/1）：{表达式} ⇒ {值}（{形态}）` |
| auto 形参的类型反推 | `[sema:targ] ⤷ auto 形参推导（[temp.param]/6）：'{形参名}' := {类型}（类型由实参反推）` |
| 值位形态归一（`Buf<4L>` → `Buf<4>`） | `[sema:targ] ✓ param {位}: '{形参名}' ({归一后形态}) ← {实参}` |

## §6 代码地图（落点）

src/sema/template_instantiation.cpp（getOrInstantiateClass 归一点）、
src/sema/semantic_analyzer.cpp（checkTemplateArguments 可表示性）、
src/ast/type.cpp（canRepresentValue）

## §7 测试入口

tests/tmpl/（21..26、61..68、74..77）、tests/unit/test_nttp_type_domain.cpp
（10 例四条判据分支逐条突变验证）

## §8 深入读

learn/18（tagged 值两层替换）、learn/35（类型域）、learn/39、40（常量表达式/常量变量实参）
