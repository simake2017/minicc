# 40 — constexpr / 常量传播 / 常量变量作 NTTP 实参（主线 D 二批）

> 主线：ROADMAP 主线 D 二批。前接：39（常量折叠 + `Buf<2+2>`）。
> 测试：`tests/fold/test_fold_02_constexpr_and_propagation.cpp`、
> `tests/tmpl/test_tmpl_76_nttp_const_var.cpp`（正）、
> `tests/tmpl/test_tmpl_77_error_constexpr_non_const_init.cpp`（负）。

## §0 定位与能力清单

`const` 只承诺"我不改"，`constexpr` 承诺"我编译期可知" —— 承诺落空即
ill-formed（[dcl.constexpr]/9 的强制义务，不是优化）。要能裁决这条承诺，
编译器必须有一张**编译期常量表**，并且能在**语义期**把它查出来。
minicc 的常量表住在 Sema（那里才有符号表与声明顺序），折叠器通过
`setConstResolver` 回调问它 —— 这是本项目"求值器回调"分层形状的
**第四次**出现（前三：DecltypeEvaluator / MemberTypeResolver /
AliasTemplateResolver）。

| 能力 | 一句话 | 状态 |
|---|---|---|
| `constexpr` 关键字 | token + parseType 前缀【隐含 const】 | 已实现 |
| constexpr 初始化式必须可常量求值 | 不可求值 ⇒ 报错，文案与 clang 逐字同 | 已实现 |
| 编译期常量表 | const / constexpr 整型家族 + 初始化式此刻可常量求值 | 已实现 |
| 常量传播 | 折叠器经 `constResolver` 回调把 `VarExpr` 换成字面量 | 已实现 |
| 常量变量作 NTTP 实参 `Buf<k>` | 语义期"类型 vs 常量"消歧，改写成值实参 | 已实现 |
| 形态取**声明类型** | `constexpr long k = 5;` ⇒ `K<5:long>` | 已实现 |
| 传播的地址边界 | Unary 折叠只收 Neg / Not，`&b` / `*p` 整棵子树原样保留 | 已实现 |
| 只登记整型家族 | 指针 const（`int* const`）不进表 ⇒ `*p` 的目标永不被折掉 | 已实现 |
| constexpr 函数 | 需能走语句 / 循环的常量求值解释器 | 未做 |
| `if constexpr` | 依赖上一项；且丢弃分支要求"未实例化部分不参与语义检查" | 未做 |
| 跨作用域遮蔽恢复 | 常量表键是裸名，内层覆盖外层；外层名复辟需栈式符号表 | 未做 |
| `static constexpr` 成员与常量表打通 | 类内静态常量已有 `foldStaticConst` 通道，但未与常量表打通 | 未做 |
| 浮点 / 指针 / 引用 / 聚合类型的常量 | 只登记整型家族 | 未做 |

---

## 1. 理论背景

### 1.1 三个名字分清楚

| 名字 | 是什么 | C++ 里的落点 |
|---|---|---|
| 常量折叠（folding） | 优化：字面量树归约成一个字面量 | 编译器自由裁量，可不做 |
| 常量求值（constant evaluation） | 语义裁决：表达式是不是 [expr.const] 意义上的常量 | NTTP 实参、`constexpr` 初始化——**必须做** |
| constexpr | **承诺**：此变量编译期可知，承诺落空即 ill-formed | [dcl.constexpr]/9 的强制义务 |

`constexpr int m = touch();` 哪怕运行期完全算得出，也是**错误** ——
因为它把"编译期可知"的承诺说了出去。这就是它和 `const` 的本质区别：
const 只承诺"我不改"，初始化可以发生在运行期。

### 1.2 常量传播（constant propagation）

```
const int c = 4;      ──登记──►   编译期常量表 { c ↦ (4, int) }
int x = c * 2;        ──传播──►   int x = 4 * 2;   （折叠器查表替换 VarExpr）
                      ──折叠──►   int x = 8;
```

传播与折叠**交替进行**：替换成字面量后外层表达式又变得可折。
教科书里这两步是同一条流水线的两个工序，本实现用"折叠器内嵌
查表回调"把它们合进了一遍递归（自底向上，天然交替）。

### 1.3 对照 clang

| clang | 本实现 | 简化了什么 |
|---|---|---|
| `Sema::isTypeName`（解析期符号表查询，区分 `Buf<k>` 的 k 是类型还是常量） | 语义期重解：形参 kind + 编译期常量表 | 推迟到语义期两步近似；解析期没有符号表（[stmt.ambig] 同款边界） |
| `ValueDecl::getEvaluatedValue`（Sema 层的常量值缓存，传播源） | `m_constValues` + `ConstantFolder::setConstResolver` 回调 | 只收整型家族；不做跨函数/跨 TU 传播 |
| [dcl.constexpr]/9 诊断：`constexpr variable 'm' must be initialized by a constant expression` | 同文案逐字 | —— |
| constexpr 函数（`constexpr int f(int x) { return x * 2; }`） | ❌ 未做（需可解释器的求值器，见 ROADMAP） | —— |

## 2. 设计决策

### 2.1 常量表住 Sema，查询走回调（第四次同一形状）

`ConstantFolder` 不依赖符号表 —— 它通过
`setConstResolver([this](name) { return lookupConstValue(name); })`
问 Sema。这与 Instantiator 的三个求值器回调（decltype / 成员类型 /
别名模板）是**同一分层形状**：能力住在 Sema（那里有符号表与声明
顺序），引擎只持抽象接口，构造时注入。

登记表的两条消费路径：

1. **常量传播**：fold 遇 `VarExpr` 查表，命中替换成字面量
   （`[fold] k → 5 （常量传播…）`），外层继续折；
2. **NTTP 实参重解**：见 §2.2。

登记的判据（`registerConstVar`，单点）：

- 声明类型是 **const 的整型家族**（constexpr 隐含 const，parseType
  前缀处理时已并入）；
- 初始化式**此刻**可常量求值（已经过 fold，再求值幂等）；
- 普通 const 求不出值 ⇒ 不登记（运行期初始化合法），不报错；
- **constexpr 求不出值 ⇒ 报错**（clang 文案逐字）。

形态取**声明类型**（剥 const）：`constexpr long k = 5;` 的 k 是
long —— `K<k>` 必须是 `K<5:long>`（docs/learn/35 撞键教训的正面应用）。

### 2.2 `Buf<k>`：语义期"类型 vs 常量"消歧

Parser 没有符号表，`Buf<k>` 的 k 只能建成 `Class("k")` 类型节点。
旧行为：resolveType 查不到类 k ⇒ 报 "unknown type name 'k'"
（响亮拒收，但 clang 认这段程序）。新行为在 resolveType 的模板 id
分支：该位**形参是 NTTP** 且实参是**裸名字**（无 `<...>`、无 `::`
  限定）时，先查编译期常量表 ——

- 命中 ⇒ 改写成值实参（`TemplateArg::ofValue`），此后与 `Buf<5>`
  完全同形（同缓存键、同 mangling、同替换路径）；
- 查不到 ⇒ 维持原路径（响亮报 unknown type name）。

类模板与**别名模板两个分支各有一份重解**（`BufA<k>`），判据同源。

消歧的正确性论证：这个改写只可能"把非法程序变合法"吗？若 k 是
常量名 ⇒ C++ 里 `Buf<k>` 本来就是常量实参（[temp.arg.nontype]），
类型解释反而是错的 —— 改写是**向标准靠拢**；若 k 不是常量 ⇒
不改写，行为同旧。两个方向都不放过错误程序。

### 2.3 传播的地址边界：Addr / Deref 不折

`&b`、`*p` 的操作数必须是真 lvalue（栈上有地址的存储），折成
字面量会让 codegen 生成野地址。因此 Unary 折叠**只收 Neg/Not**，
Addr/Deref 整棵子树原样保留 —— `const int b = 7; const int* pb = &b;
*pb` 照常工作（test_fold_02 检查点 5 钉住）。

同理只登记**整型家族**：指针 const（`int* const p`）不进表，
`*p` 的目标永远不会被折掉。

### 2.4 作用域取舍

常量表键是裸名，无作用域链：内层声明**覆盖**外层同名（与命名空间
回退"只记最近一层"同款取舍）。遮蔽恢复（内层出作用域后外层名
复辟）未做 —— 需要栈式符号表，超出本轮范围。

## 3. 可复现实验

```bash
# constexpr + 传播（[const]/[fold] 日志对照阅读）
./minicc tests/fold/test_fold_02_constexpr_and_propagation.cpp -o /tmp/f2 && /tmp/f2; echo $?

# Buf<k>：全局/局部 constexpr、别名、auto 形态（cache hit 行验证同实例）
./minicc tests/tmpl/test_tmpl_76_nttp_const_var.cpp -o /tmp/t76 && /tmp/t76

# 负向（与 clang++-18 -std=c++20 逐字同）
./minicc tests/tmpl/test_tmpl_77_error_constexpr_non_const_init.cpp   # rc=1

# 三绿
./build.sh && (cd build-linux && ctest) && ./logdiff.sh diff
```

## 4. 边界与有意漂移

- **有意日志漂移 3 处**（均已重刷基线）：
  `flow__02`（fold 行，首批已有）、`fold__01`（describe 修复后
  日志显示**折后**孩子：`(2 + 12)` 取代 `(2 + (3 * 4))` —— 新形态
  更准确）、`lang__basics_02`（`const int b = 1;` 触发了登记与传播，
  infer 树塌缩成字面量，rc=0 不变）。
- **未做**：constexpr 函数（需真正的常量求值解释器，能走语句/循环）、
  `if constexpr`、跨作用域遮蔽恢复、`static constexpr` 成员
  （类内静态常量已有 foldStaticConst 通道，但未与常量表打通）。

---

## 5. 真实版多做了什么、为什么

素材来自 §1.3 对照表的「简化了什么」列与 §4 的边界清单。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| 解析期符号表查询（`Sema::isTypeName`）区分 `Buf<k>` 的 k 是类型还是常量 | minicc 的 Parser 没有符号表，只能在语义期两步近似；根治需解析期符号表（[stmt.ambig] 完整裁决，未做） |
| `ValueDecl::getEvaluatedValue` 的常量值缓存 + 跨函数 / 跨 TU 传播 | 本项目只登记整型家族、只在本 TU 内传播；跨 TU 需要完整求值器与链接期常量池 |
| constexpr 函数（走语句与循环的常量求值解释器） | 求值器要能执行控制流，与折叠器的"纯树归约"是两套引擎；教学版先保"变量承诺"这一层 |
| `if constexpr` 的丢弃分支语义 | 依赖 constexpr 函数；且要求未实例化的分支不参与语义检查，本项目无此机制 |
| 作用域**链式**的常量表（内层出作用域后外层名复辟） | 当前是裸名键 + 内层覆盖外层的单层近似；栈式符号表超出本轮范围 |
| clang 的常量求值器支持浮点 / 指针 / 引用 / 聚合 | 只登记整型家族 —— 否则 `int* const p` 进表会把 `*p` 的目标折掉，生成野地址 |
| clang 的 `constexpr` 变量隐含 `const` 且参与模板实参的**转换后常量表达式**判定 | 本实现按"可表示性 + 形态归一"两条判据近似（learn/35 的 B17 口径） |

---

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-10-15 | 落地主线 D 二批：`constexpr` 关键字 + 编译期常量表 `m_constValues` + 常量传播 + `Buf<k>` 语义期重解（类模板与别名模板两个分支） | 常量表住 Sema、查询走回调（求值器回调分层形状第四次）；改写只可能"把非法程序变合法" ⇒ 两个方向都不放过错误程序（git 5af6604） |
