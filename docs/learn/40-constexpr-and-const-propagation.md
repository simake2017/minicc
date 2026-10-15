# 40 — constexpr / 常量传播 / 常量变量作 NTTP 实参（主线 D 二批）

> 主线：ROADMAP 主线 D 二批。前接：39（常量折叠 + `Buf<2+2>`）。
> 测试：`tests/fold/test_fold_02_constexpr_and_propagation.cpp`、
> `tests/tmpl/test_tmpl_76_nttp_const_var.cpp`（正）、
> `tests/tmpl/test_tmpl_77_error_constexpr_non_const_init.cpp`（负）。

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
