# 05.3 SFINAE 与依赖名 —— 软失败协议

> 上级：[00-总览](00-总览.md）。

- 读者：加 SFINAE 相关特性或依赖名的开发者
- 审法：软失败只走 Sfinae::fail 三处收口；出了直接上下文就是硬错误
- 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 按 tech-doc 骨架补齐：诊断类别 / Out of Scope / 观测点 |

## §1 管什么（定位）

「替换失败不是错误」（[temp.deduct]/8）的落地：三方协议 + 依赖名两阶段裁决。

## §2 关键判据（详细设计）

- **三方协议**（独立模块，收口点全项目仅三处）：SubstitutionFailure（异常
  信号）+ Sfinae::attempt（吸收器，把软失败变 optional）+ SfinaeContext
  （直接上下文标记——**出了直接上下文就是硬错误**）。对照 clang 的 SFINAETrap。
- **依赖类型名 `typename T::type`**（learn/25，[temp.res]/5 悬案）：定义期判
  不出 `T::x` 是类型还是值——Parser 收 typename 前缀建「限定者=TemplateParam」
  节点，Sema 原样保留，**替换当场**（Case 5.5）查实例成员表解糖；查不到即软
  失败 ⇒ `void_t<typename T::type>` 探测惯用法成立。硬错误文案与 clang 逐字。
- **void_t 与 CWG 1558**：别名模板特例——CWG 1558 前 void_t 探测在别名上会
  硬失败；本项目按现行标准实现（learn/20）。
- **std 垫片注入**：false_type/true_type/void_t/declval 走 registerBuiltins
  **内建注入**——绕开别名模板与类内 static 的语法缺口；declval 名字同时是种
  进 parser 名字登记表（内建名字 parser 必须知道，同 clang 名单制）。

## §3 诊断类别（异常处理）

SFINAE 的本质是**把错误降级成"候选出局"**，所以本模块的诊断分两档：
**硬错误**（出了直接上下文）与**软失败**（直接上下文内，降级为候选被移出）。

硬错误由 `src/sema/semantic_analyzer.cpp` 的 `error()` 打出，输出形态
`[Semantic Error] {行:列}: {文案}`；软失败由 `src/sema/sfinae.cpp` 的 `Sfinae::fail` 打出，
形态 `[sfinae] ⚡ 发出替换失败信号（{上下文}）：{原因}`。编译期诊断一律**不可重试**。

| # | 触发情形 | 诊断文案 | 可重试 |
|---|---|---|---|
| 1 | 依赖限定者里的成员名不是类型（**硬错误**：出了直接上下文） | `no type named '{名}' in '{类名}'`（与 clang++-18 **逐字相同**） | 否 |
| 2 | 依赖限定者指向的成员不是 static 成员（硬错误） | `no static member '{名}' in class '{类名}' —— 类型限定访问 Cls<Args>::member 只解析静态成员；若此处是 SFINAE 探测，说明偏特化被移除后回退到的主模板没有该成员（对照 test_tmpl_36 的兜底写法）` | 否 |
| 3 | 探测一个非类类型的成员（**软失败**：被 `Sfinae::demote` 降级） | `Cannot access member '{名}' on non-class type '{类型}'` | 否 |
| 4 | 探测一个空类型的成员（软失败） | `Cannot access member of null type` | 否 |
| 5 | 限定模板名在命名空间里不存在（硬错误） | `no template named '{名}' in namespace '{命名空间}'` | 否 |
| 6 | 名字从未声明过（硬错误） | `use of undeclared identifier '{名}'` | 否 |

★ 第 1 条的硬错误文案与 clang++-18 **逐字相同**：`no type named 'type' in 'WithoutType'`
（learn/25 实测对拍）。
★ 第 3、4 条在普通代码里是硬错误，在 decltype / SFINAE 探测里被降级——**同一个文案，
两种命运**，判据是"是否在直接上下文内"。

## §4 Out of Scope（本模块不做的事）

- **软失败只在直接上下文（immediate context）内生效**：出了直接上下文就是硬错误（[temp.deduct]/8）。
- **不做异常说明（exception specification）参与 SFINAE**：C++17 起 noexcept 参与重载决议的规则未实现。
- **不做 `requires` 子句与 concept**：探测惯用房是 `enable_if` + `void_t`。
- **不做 CWG 1558 之前的历史行为**：本项目按**现行标准**实现（别名模板上的 `void_t` 探测成立）。
- **std 垫片只注入四个内建**：`false_type` / `true_type` / `void_t` / `declval`——它们走 `registerBuiltins` 内建注入，**绕开**别名模板与类内 static 的语法缺口；别的 std 名字不注入。
- **不做 `decltype` 的完整两套规则**：[dcl.type.decltype] 的"被声明的类型"与"表达式类型"两套规则已实现（learn/20），但 `decltype(auto)` 的推导规则**不做**。

## §5 观测点（日志键）

★ `[sfinae]` 是**全项目唯一**打印这个前缀的地方（src/sema/sfinae.cpp）——输出里每一行 `[sfinae]`
都能在这里找到出处。

| 时机 | 打的键 |
|---|---|
| 发出替换失败信号 | `[sfinae] ⚡ 发出替换失败信号（{上下文}）：{原因}` |
| 候选被移出候选集（软失败，非错误） | `[sfinae] ⤵ 候选【{候选}】被移出候选集（SFINAE 软失败，非错误）` + `[sfinae]    原因：{原因}` |
| void_t 探测开始 | `[sfinae] void_t<{n} 个实参> —— 逐个替换 + 求值探测` |
| void_t 单个实参探测通过 | `[sfinae]   ├─ 实参 {i} 探测通过 → {类型}` |
| void_t 全部实参合法 | `[sfinae]   └─ 全部实参合法 ⇒ void_t<...> 归约为 void ✓` |
| void_t 无实参 | `[sfinae] void_t<> 无实参 ⇒ 归约为 void ✓` |
| decltype 操作数在内层报错 → 降级为软失败 | `[sfinae] ⟲ decltype 操作数在内层报错，降级为【软失败】:` + `[sfinae]    {内层错误}` |
| 依赖限定者保持依赖（等替换阶段） | `[sema:alias] {限定者}::{成员} 限定者仍是模板形参 ⇒ 保持依赖，等替换阶段` |

## §6 代码地图（落点）

src/sema/sfinae.cpp（唯一 "[sfinae]" 日志出口）、src/sema/sfinae.h（协议契约）、
substituteType Case 5.5/5.8（当场解糖的两个闸）

## §7 测试入口

tests/tmpl/（33..39 SFINAE 套件）、tests/unit/test_decltype_sfinae.cpp
（Decltype./Sfinae./PartialOrder./SfinaeProtocol. 四套件）

## §8 深入读

learn/20（SFINAE 协议）、learn/25（依赖类型名）、learn/21（偏序，→ 05.4）
