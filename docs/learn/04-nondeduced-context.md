# 04 不可推导上下文（S4）

> 子阶段 S4：模板参数只出现在"推不动"的位置时，诊断并提示显式指定。

## §0 定位与能力清单

模板参数有时**推不出来**——它只出现在返回类型、`sizeof(T)`、默认实参这类位置，
调用点的实参里没有任何信息能绑定它。标准为这类位置列了一张清单
（non-deduced contexts），编译器必须在此时**报错并告诉调用方怎么改**。
本文讲 minicc 用什么办法处理它：**不走枚举，走收尾**。

| 能力 | 一句话 | 状态 |
|---|---|---|
| [temp.deduct.type] 非推导上下文的检出 | 走完全部 P/A 配对后仍有模板参数未绑定 ⇒ 判定为不可推导 | 已实现 |
| 诊断给出修复建议 | 报错文案形如 `specify it explicitly, e.g. gen<int>()`，把 S4 与 S3 串成闭环 | 已实现 |
| 被引用形参间接带出的模板参数 | `void f(T&)` 中 T 可由实参类型反推 | 已实现（见 learn/02 的引用路径） |
| 按语法位置细分的诊断类别 | clang 区分"在返回类型里""在 `sizeof` 里""在默认实参里"等不同 non-deduced context | 未做 |
| 非类型实参表达式里的模板参数 | `template<int N, class T> void f(N, T)` 中 `N` 里的 T 不可推 | 未做（函数模板的 NTTP 路径整体未做，见 learn/35） |
| `auto` 形参的不可推导情形 | `template<auto V> void f()` 无实参时 V 推不出 | 未做（learn/35 引入 `template<auto V>`，但只走显式实参） |

## ① 理论背景

**标准依据**：[temp.deduct.type] 列出的 non-deduced contexts，常见：

- 只出现在**返回类型**（`template<typename T> T gen();` 调 `gen()`）
- 出现在 `sizeof(T)`、`decltype` 等非推导表达式中
- 非类型模板参数表达式里的 T、默认实参里的 T

本实现的判定是**收尾检查**：走完全部 P/A 配对后，凡未被绑定的模板参数
一律视为不可推导（上述位置都不会产生绑定，自然落到这里）。比逐语法位置
枚举简单，且对教学足够精确。

## ② 设计决策

- 检查点在 `deduce()` 末尾，错误信息直接给出修复建议：
  `specify it explicitly, e.g. gen<int>()` —— 把 S3 与 S4 串成闭环。
- 不区分"哪种不可推导上下文"（clang 有专门诊断类别）——简化。

## ③ clang 对照

| 本实现 | clang | 简化 |
|---|---|---|
| 收尾未绑定检查 | `SemaTemplateDeduction.cpp` 收尾 + `TemplateDeductionCallback` 在非推导位置主动跳过绑定并记录 `NonDeduced` | 无细粒度诊断分类 |
| 错误文案建议显式实参 | clang 诊断 `note: couldn't infer template argument 'T'` | 一致思路 |

## ④ 实验

```bash
./build-linux/minicc tests/test_tmpl_18_nondeduced_error.cpp; echo $?   # 1
# 期望：non-deduced context: template parameter 'T' cannot be deduced ...
# 修复演示：把 gen() 改成 gen<int>() 即可通过（S3 路径）
```

## ⑤ 过程图

```
gen()  →  参数对数量 = 0（没有 P/A 配对可做）
       →  收尾检查：T 未绑定
       →  ✗ non-deduced context 报错（指向返回类型位置）
```

## ⑥ 真实版多做了什么、为什么

★ 素材来源：本文 §③ 对照的「简化」列 + `docs/BUGS.md` B31/B35 + CLAUDE.md「未做」清单。
**未查证的写「（本项目未查证）」**。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `TemplateDeductionCallback` 在**每个非推导语法位置**上主动跳过绑定并记一条 `NonDeduced` | 本项目改成**收尾一刀切**：凡是"推不动的位置"都不会产生绑定，走完全部配对后自然落在"未绑定"集合里。少写一张位置枚举表 ⇒ 少一处可能漏枚举的地方；代价是诊断说不出"具体是哪个位置推不动" |
| 细粒度诊断分类（返回类型 / `sizeof` / 默认实参 / 非类型实参表达式…） | clang 分了多类；本项目统一报一条"cannot be deduced"并附修复建议——教学场景下**修复动作是同一个**（显式指定实参），分类的收益有限 |
| 诊断里附"哪些参数没推出来"的清单 | 本项目的报错文案已点名具体参数（`template parameter 'T'`） |
| 对非类型模板参数表达式的推导 | 本项目的函数模板 NTTP 显式实参直接报 not supported yet（learn/35），这条路径整体未做 |
| 显式指定后的**继续推导**（部分实参显式 + 部分推导） | 本项目已实现（S3 的前缀规则），是 S4 建议的落点 |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-08-10 | S4 落地：`deduce()` 末尾收尾检查 + `specify it explicitly` 建议 | 把 S3（显式实参）与 S4 串成闭环——报错直接给出可用的修复写法 |
| 2026-09-16 | 模板机制主线收官：不可推导判定随 tagged 实参细化（类模板侧） | 值位实参与类型位实参必须分开判 kind，否则"推不动"与"推错"分不开（learn/18） |
| 2026-09-30 | NTTP 类型域扩展（learn/35）后，类模板侧的不可推导位置扩大 | `template<auto V>` 的形态即类型 ⇒ 形态不匹配也落在"推不动"这一类 |
