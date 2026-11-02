# 02 模板实参推导（S2，核心）

> 子阶段 S2：对每个 (参数模式 P, 实参类型 A) 配对做受限合一，
> 绑定模板参数；同一参数多次绑定必须一致。这是 C++ 模板机制的理论核心。

## §0 定位与能力清单

实参推导回答的是一个很具体的问题：调用 `twice(21)` 时，形参里的 `T` 到底是 `int`
还是别的？C++ 的做法是**受限合一**——把形参类型当**模式 P**、实参类型当**具体类型 A**，
逐对配对，把 P 里的类型变量绑到 A 上；绑不出来就说明这个候选不可行。
它与 Hindley-Milner 的合一算法同源，但**受限在**：只有模式一侧可以含变量，
实参一侧永远是具体类型。本文讲的是这套合一算法的第一条路径——**从函数调用的实参推**。

| 能力 | 一句话 | 状态 |
|---|---|---|
| [temp.deduct.call] P/A 逐对配对 | 形参模式 P 与实参类型 A 逐对合一，绑出模板参数 | 已实现 |
| [temp.deduct.call]/4 值传递调整 | 裸形参 `T` 对非引用实参先剥顶层 cv/ref 再绑定 | 已实现 |
| [temp.deduct.call]/4.2 万能引用折叠 | `T&&` 对左值实参折叠成 `A&`，对右值继续向内层推 | 已实现 |
| [temp.deduct.call]/4.3 引用形参 | `T&` 要求实参是左值，否则该候选直接不可行 | 已实现 |
| [temp.deduct.call]/4.4 指针形参 | `T*` 要求实参是指针，对 pointee 递归 | 已实现 |
| 多次绑定的一致性 | 同一模板参数被推成两个不同类型 ⇒ 推导失败（候选被否决，不报错） | 已实现 |
| 左值性作为推导输入 | `inferCall` 为每个实参记录 `isLValue`，而不只是类型 | 已实现 |
| [temp.deduct.type]/8 结构化匹配 | `Box<T>` 对 `Box<int>` 这类模板 id 做结构合一 | 已实现（别名模板批次补齐，见 learn/27） |
| [dcl.ref]/6 引用折叠 | 替换期把 `T& &` / `T& &&` / `T&& &` 折成 `T&` | 已实现（见 learn/21） |
| 数组 / 函数指针类型退化 | `int(&)[3]`、`void(*)(int)` 在推导期先做类型退化 | 未做 |
| 非类型模板参数的推导 | `template<int N>` 从实参推出 `N` | 未做（函数模板的 NTTP 显式实参直接报 not supported yet，见 learn/35） |
| 模板模板参数的推导 | `template<template<class> class C>` 的形参位推导 | 未做 |
| 偏序专用的推导入口 | clang 为 partial ordering 单独开一条推导路径 | 未做（S6 复用普通推导，见 learn/21 补齐） |

## ① 理论背景

**标准依据**：[temp.deduct.call]（函数调用实参推导）。

- **推导 = 受限合一**：与 Hindley-Milner 类型推断的合一算法同源，但受限在——
  只有"参数模式"一侧可以含变量（T），实参一侧是具体类型；
  没有通用方程求解，也没有 let-多态。
- **P/A 配对规则**（本实现的五条，对应标准逐款）：
  | P 形态 | 处理 |
  |---|---|
  | `const X` | 剥 P 顶层 const，继续 |
  | `T&` | 实参必须左值（否则拒绝），对 P 内层继续 |
  | `T&&` | 万能引用：左值实参 ⇒ T := A&（折叠为 A&）；右值 ⇒ 对内层继续 |
  | `T*` | 实参必须指针，对 pointee 递归 |
  | `T`（裸参数） | 值传递调整：剥 A 顶层引用/const 后绑定 |
  | 非依赖类型 | 要求 P == A 恒等 |
- **一致性 = 替换合成**：`mix2(1, true)` 第一次 T := int，第二次 T := bool →
  合一失败。这就是替换必须满足的"幂等且一致"约束。
- **左值性是推导输入**：所以 sema 在 `inferCall` 里为每个实参记录
  `isLValue`（VarExpr/MemberExpr 为左值），而不只是类型。

## ② 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 独立模块 `TemplateDeducer` | `deduce()` 返回 `DeductionResult{success, deducedArgs, trace}` | 推导与实例化解耦；trace 逐对记录供教学日志 |
| T 的表示 | 同时认 `TemplateParam("T")` 和 `Class("T")` | parseType 把裸 T 解析成 Class 类型（S1 的简化），推导层吸收这个差异 |
| 左值性来源 | sema 侧按表达式种类判定（变量/成员访问 = 左值） | 没有独立的值类别系统（无 xvalue/prvalue 细分），够用且可讲清 |
| 失败即否决候选 | 任一对失败 → 该候选不可行，继续试下一个 | 为 S6 重载决议铺路：否决不等于报错 |
| 恒等判定 `Type::equals` | 不做隐式转换（int≠double） | 转换排序属于 S6 范畴，此处保持推导纯粹 |

## ③ clang 对照表

| 本实现 | clang 源 | 简化了什么 |
|---|---|---|
| `TemplateDeducer::deduce` | `SemaTemplateDeduction.cpp → DeduceTemplateArguments`（3 个重载：from call / from type / for partial ordering） | 只实现 from call；无 partial ordering 专用入口（S6 复用普通推导） |
| `deducePair` 五分支 | `TemplateDeductionCallback::Deduce`（P/A 递归核心，约 400 行） | 无 non-type 模板参数、模板模板参数、函数指针参数、初始化列表实参 |
| 万能引用折叠 | 同文件 `DeduceAutoType` 与 [temp.deduct.call]/4.2 实现 | 无 auto&&、无转发 `std::forward` |
| 值传递调整（剥顶层 cv/ref） | 同文件开头对 A 的调整（"If P is not a reference..."） | 一致 |
| 一致性检查 `bind` | `DeducedTemplateArgument` 合并时的冲突诊断 | clang 区分"转换后冲突"与"多次推导冲突"两类诊断 |

## ④ 实验手册

```bash
cd /home/magene/runtime/cppproject/mycompiler

# 基础推导（逐对日志 + 实例化 + 执行）
./build-linux/minicc tests/test_tmpl_14_deduce_basic.cpp -o /tmp/t14.s
gcc /tmp/t14.s -o /tmp/t14 && /tmp/t14; echo $?     # 0

# 引用/const/万能引用形态
./build-linux/minicc tests/test_tmpl_15_deduce_forms.cpp -o /tmp/t15.s
gcc /tmp/t15.s -o /tmp/t15 && /tmp/t15; echo $?     # 0

# 冲突绑定（错误用例）
./build-linux/minicc tests/test_tmpl_16_conflict_error.cpp; echo $?   # 1，见 conflicting

# oracle：clang 的推导诊断与产物
clang++-18 tests/test_tmpl_16_conflict_error.cpp -fsyntax-only        # 对照冲突报错措辞
clang++-18 -emit-llvm -S tests/test_tmpl_14_deduce_basic.cpp -o - | grep define   # 对照实例化符号
```

## ⑤ 关键过程图

```
调用 twice(21)
    │  sema.inferCall: 实参类型 [int]，左值性 [false]
    ▼
deduce(tmpl=twice, A=[int])
    │
    ├─ 参数对 0:  P = T          A = int
    │             P 是裸模板参数 → 值传递调整（A 无顶层 ref/const，不变）
    │             bind(T, int)   →  subst = {T := int}   ✓
    │
    ├─ 收尾检查：typeParams [T] 全部已绑定            ✓（否则 S4 报错）
    ▼
deducedArgs = [int]
    │
    ▼  （S5）instantiateFunction(twice, [int])
subst {T→int} 克隆蓝图 → twice<int>，符号 _Z5twiceIiE
    │
    ▼  callee 重写：VarExpr "twice" → "_Z5twiceIiE"
codegen: callq _Z5twiceIiE   ←→   _Z5twiceIiE: 标签   ✓ 链接闭环
```

## ⑥ 真实版多做了什么、为什么

★ 素材来源：本文 §③ 对照表的「简化了什么」列 + `docs/BUGS.md` B26/B28
+ CLAUDE.md「未做」清单。**未查证的写「（本项目未查证）」**。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `DeduceTemplateArguments` 的**三个重载**（from call / from type / for partial ordering） | 本项目只实现 from call 一条；partial ordering 直接复用普通推导（见 §6 的偏序说明）——教学上"用同一套合一算法做两件事"比"拆三条路径"更好讲 |
| `TemplateDeductionCallback::Deduce` 约 400 行，覆盖 non-type 模板参数、模板模板参数、函数指针参数、初始化列表实参 | 本项目只做**类型形参**这一条主干；其余几类各自需要新的实参表示（NTTP 的 tagged 实参见 learn/18，模板模板参数的签名匹配见 learn/33） |
| `DeduceAutoType` 与 `auto&&` 的独立推导路径 | 本项目没有 `auto` 形参；`auto` NTTP 是后来 learn/35 才加的，且走的是"形态即类型"的另一条路 |
| 区分"转换后冲突"与"多次推导冲突"两类诊断 | 本项目统一报"conflicting deduction"一类信息——两类冲突在教学场景里指向同一条修复动作（显式指定实参） |
| 对 A 侧做**完整的类型调整**（数组→指针、函数→函数指针、cv 剥离） | 本项目只做剥顶层 cv/ref；数组与函数指针的退化未做——需要先有原生数组类型（learn/42 才引入 `TypeKind::Array`） |
| 推导失败时区分"不可推导"与"推导冲突" | 本项目在 S4 收尾统一处理（见 04 号文档）；clang 的细粒度诊断类别未做 |
| 推导期的引用折叠（`T&&` 绑左值） | 本项目的折叠放在**替换期**（`substituteType` 的 [dcl.ref]/6 分支）而非推导期——同一个折叠规则只写一遍，避免两处口径漂移 |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-08-10 | S2 落地：独立 `TemplateDeducer` 模块，`deduce()` 返回 (success, deducedArgs, trace) | 推导与实例化解耦；trace 逐对打印供教学日志 |
| 2026-09-16 | 模板机制主线收官：推导引擎补上类模板 id 结构合一与 `desugarAlias` | 别名模板形参位不解糖 ⇒ 名字不等 ⇒ 候选被**静默剔除**（learn/27） |
| 2026-09-21 | 引用折叠正确性 + 模板形参作用域帧化（对齐 clang） | `T&&` 折叠规则原先散在两处、口径不一（learn/21） |
| 2026-10-09 | 实参绑定与名字可见性收口（B21/B24/B25/B26/B27） | 引用绑定判据此前**只写了模板推导这一半**，非模板调用那条路把引用剥掉再比类型 ⇒ 什么都不挡（B26） |
| 2026-10-14 | `deducePair` 入口按 [expr.type]/1 剥 A 的引用壳（B26 追补） | 模板推导这条路上引用壳也得剥，否则 `T&` 形参匹配不上 |
