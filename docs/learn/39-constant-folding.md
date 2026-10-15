# 39 — 常量折叠（主线 D 第一批：Sema 层折叠 + NTTP 常量表达式实参）

> 主线：ROADMAP 主线 D。前接：35（NTTP 类型域）、38（for/三元，短路求值）。
> 测试：`tests/fold/test_fold_01_basic.cpp`（折叠行为）、
> `tests/tmpl/test_tmpl_74_nttp_const_expr.cpp`（`Buf<2+2>` 正例）、
> `tests/tmpl/test_tmpl_75_error_nttp_non_const_expr.cpp`（负例）。
> 新模块：`include/constant_folder.h` + `src/constant_folder.cpp`。

## 1. 理论背景

### 1.1 常量折叠是什么

把"操作数全是字面量"的表达式在**编译期**算成一个字面量：

```
2 + 3 * 4    ──fold──►    14        （运行期少做两次运算）
true ? a : b ──fold──►    a         （b 支成为死代码，不生成）
```

两个层面的话要说清：

- **折叠（folding）**：一棵表达式树自底向上归约。3*4 折成 12 之后，
  外层的 2+12 又能折 —— 折叠与常量传播是**交替进行**的，一遍递归
  自底向上正好完成交替。
- **常量求值（constant evaluation）**：[expr.const] 定义的"表达式
  能否在编译期算出唯一值"的**裁决**。折叠是优化（可不做），常量
  求值是语义（NTTP 实参、`constexpr` 初始化必须做）。两者共享同一棵
  树的遍历，但裁判标准不同：折叠"折不动就放过"，求值"折不动就报错"。

### 1.2 对应的标准与编译原理概念

| 概念 | C++ 标准 | 编译原理 |
|---|---|---|
| 核心常量表达式 | [expr.const]/5（禁止除零等） | 部分求值 / 解释器 |
| 转换后常量表达式 | [expr.const]/10 → [dcl.init]/7 | 类型安全的常量传播 |
| NTTP 实参须为转换后常量表达式 | [temp.arg.nontype]/1 | 同上 |
| 死代码消除 | —（优化，标准不管） | 控制流图可达性；本实现的最小形态 = 恒定条件三元 |

### 1.3 对照 clang

| clang | 本实现 | 简化了什么 |
|---|---|---|
| `lib/AST/ExprConstant.cpp`（求值器，constexpr 语义裁判；`IntExprEvaluator::VisitBinDiv` 报除零） | `ConstantFolder::evaluateIntegral` | clang 求任意类型常量（浮点/指针/引用/聚合），本项目只求整数值 + 形态 |
| Sema 随手折（`Sema::ActOnBinOp` 一路向上试折） | `SemanticAnalyzer` 6 个 fold 钩子（初始化式/return/表达式语句/三处条件） | clang 折的点更多（数组长度、case 标号……本项目数组还没做） |
| `TemplateArgument` 的 Expression 形态 → `Sema::CheckTemplateArgument` 里 `EvaluateAsInt` 转 Value 形态 | `TemplateArg::valueExpr`（Parser 存树）→ `getOrInstantiateClass` 求值归一 | 同构：Parser 不评价、Sema 才裁决 |
| CodeGen 对恒定条件只发选中分支 | 折叠后 AST 里只剩选中分支，CodeGen 无感知 | —— |
| LLVM InstCombine 折叠表（后端兜底） | 无 | 教学项目，一层就够 |

## 2. 设计决策

### 2.1 折叠发生在 Sema 层，结果写回 AST

语句访问点拿到表达式槽位后先 fold 再定型：

```
decl.initializer = m_constantFolder.fold(std::move(decl.initializer));
```

后续阶段（定型 / 代码生成）看到的只有字面量 —— **折叠对下游零感知**，
这正是 112 个既有 tmpl 用例逐字节零漂移的原因（漂移只发生在
`[fold]` 日志行本身，见 §5）。

### 2.2 折不动的要诚实（两条"不折"）

- **除零不折**：`tryBinary(Div, …, 0)` 返回 nullopt，`1/0` 原样保留。
  UB 不在编译期替程序"决定"。clang 同理（折叠器放过，求值器报错）。
- **含变量不折**：`x + 1` 原样保留到代码生成。日志里无 `[fold]` 行
  —— 可观测性正好充当测试 oracle（test_fold_01 检查点 6/7）。

### 2.3 恒定条件三元 = 死分支消除的最小形态

`true ? a : b` 折叠时**不碰**两个分支里的非字面量（它们可能有副作用），
只把整棵树替换成选中的那支 —— 死分支自然不再出现在 AST 里：

```
[fold] (true ? 11 : touch()) → 11     ← touch() 从 AST 消失
```

CodeGen 对三元本来就有短路跳转（docs/learn/38），但那是**运行期**短路；
折叠是**编译期**消除 —— `g_side_effect` 在运行期真的是 0
（test_fold_01 检查点 5 钉住这条）。

### 2.4 NTTP 常量表达式实参：`Buf<2+2>` 的三道工序

```
Parser                          Sema（getOrInstantiateClass）
─────────────                   ──────────────────────────────
2+2 是表达式吗？
  字面量 fast path              ① 算缓存键【之前】：求值归一
  （Buf<4> / Buf<-3>，              evaluateIntegral(valueExpr)
   逐字节保留零漂移）              成功 ⇒ value/valueType 落槽、
  字面量后紧跟运算符？              valueExpr 清空 —— 此后与
  → 解析完整表达式存               裸字面量实参完全同形
    TemplateArg::valueExpr      ② checkTemplateArguments 的
  [parse:targ] ★ constant           ②③ 各判据照常（形态相容/
   expression '2 + 2'               可表示性/③-c 归一）
  （求值推迟到语义期）           ③ 缓存键 / mangling / 替换 —— 无感
```

**为什么求值必须在缓存键之前**：键与实例名都吃实参的无损串。
不归一的话 `Buf<2+2>` 的键里值位是占位 0 —— 先来的把缓存占了，
`Buf<4>` 要么撞进 `Buf<2+2>` 的实例要么各建一份。这是把求值塞进
`checkTemplateArguments` 会踩的坑（它现在被键计算挡在后面）。

**为什么 evaluateIntegral 返回值要带形态**：`template<auto V>` 下
`K<2+2>` 是 `K<4:int>`、`K<4L>` 是 `K<4:long>` —— 两个实例
（docs/learn/35 的撞键教训：值同形不同，丢了类型就静默合并）。
求值结果的形态取折叠后字面量自带的 `literalType`（`2L+2` 折出的
字面量形态为 long —— 与 clang 的 usual arithmetic conversions 同向，
虽然没有完整实现那一套）。

**Parser 为什么不顺手求值**：[expr.const] 的裁决权在语义期
（需要符号表判断名字是不是 constexpr）。Parser 只存树，Sema 才裁决 ——
与 clang 的分层一致。

### 2.5 模板实参表里的 `>` 歧义（[stmt.ambig] 的实参表版）

`Buf<2+2> a;` 里 comparison 层会把收尾 `>` 吃成大于号
（`2 + 2 > a` → 解析崩）。处理：`m_stopAtGreater` 标志罩住常量
实参表达式的解析，comparison 层遇裸 `>` 停止；**括号分组内豁免**
（`Buf<(2>1)>` 里的是真比较）；`>=` 不受限（收尾永远是裸 `>`）。
对照 clang：C++11 起模板实参表里的 `>` 一律当收尾。

`isExprContinuation()`（字面量 fast path 的前瞻判据）**刻意不收**
Less/Greater —— `Buf<4>` 的收尾 `>` 与小于/大于同 token，收进来
每条裸字面量实参都会误走表达式路径（实测 tmpl 21..26 全部漂移）。

## 3. 负向用例与 clang 对照

| 用例 | clang++-18 | minicc |
|---|---|---|
| `Buf<1/0>` | error: non-type template argument is not a constant expression | 同文案逐字 ✓（rc=1） |
| `Buf<1/0>` 的语义链 | [expr.const]/5 禁除零 | tryBinary 除零不折 → 求值器见非字面量树 → 报错，两道工序接力 |

## 4. 可复现实验

```bash
# 折叠观测（日志里数 [fold] 行）
./minicc tests/fold/test_fold_01_basic.cpp -o /tmp/f1.out && /tmp/f1.out; echo "exit=$?"

# NTTP 常量表达式（注意 cache hit 行：Buf<2+2> 与 Buf<4> 同键）
./minicc tests/tmpl/test_tmpl_74_nttp_const_expr.cpp -o /tmp/t74.out && /tmp/t74.out

# 负向（与 clang++-18 -std=c++20 文案逐字相同）
./minicc tests/tmpl/test_tmpl_75_error_nttp_non_const_expr.cpp        # rc=1

# 三绿
./build.sh && (cd build-linux && ctest) && ./logdiff.sh diff
```

## 5. 边界与有意漂移

- **有意日志漂移 1 处**：`flow__test_flow_02_ternary`（fold 行替换
  infer 树；运行结果逐字节不变）。已重刷基线。
- **折的范围**：只折二元/一元/三元；函数调用、变量、成员访问原样
  保留。`sizeof`、取地址、字符码点等尚未进折叠表。
- **未做**（按优先级）：`constexpr` 关键字与"初始化式必须可折叠"
  强制、变量间常量传播（`const int n = 4; int a[n];` —— 与主线 E
  数组同批）、`Buf<k>`（const 变量作 NTTP 实参，需符号表参与求值）、
  浮点常量折叠（B23 相关）。
- **已知边界**：`Buf<x>`（x 是变量）走类型实参 fast path，报
  "unknown type name 'x'"而非 clang 的"not a constant expression" ——
  同为响亮拒收，文案不同；根治需要解析期符号表（[stmt.ambig] 完整
  裁决，已在清单）。
