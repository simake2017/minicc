# 18 非类型模板参数（NTTP）

> 主题：`template<int N> class Buf` 与 `Buf<4>` —— 模板形参/实参的
> 「类型 vs 值」两形态分流。本文记录一次**真实缺陷的定位与修复**：
> 旧实现在替换映射一层无法区分 `template<class T>` 与 `template<int N>`。

## §0 定位与能力清单

模板实参不只有类型一种——`Buf<4>` 里的 `4` 是一个**值**，不是类型。
要让编译器同时接受 `Box<int>` 和 `Buf<4>`，实参的表示必须是 **tagged**
（带一个"这是类型还是值"的标签）。本文记录一次**真实缺陷**的定位与修复：
旧实现在替换映射一层无法区分 `template<class T>` 与 `template<int N>`——
报错点在第 166 行，根因却分散在三处（见 §②）。本文也顺带讲清一件事：
**替换 ≠ 实例化**——NTTP 让替换天然分成两层（类型位 / 表达式位）。

| 能力 | 一句话 | 状态 |
|---|---|---|
| [temp.param] 类型形参 | `typename T` / `class T`，两种拼写等价 | 已实现 |
| [temp.param] 非类型形参 | `int N`（最初只认 `int`） | 部分（2026-09-30 扩展到整型家族 + 字符 + `auto`，见 learn/35） |
| [temp.arg] **tagged 实参** | `TemplateArg{kind, type, value}`，对应 `clang::TemplateArgument` | 已实现 |
| [temp.subst] 替换**分两层** | 类型位置走 `substituteType`、表达式位置走 `cloneExpr` | 已实现 |
| 形参 kind 判据 | 看**该位形参自己**是什么（`templateParams[i].kind`），不看实参长什么样 | 已实现 |
| 值的**可表示性** | `Type::canRepresentValue`（[expr.const]/10 → [dcl.init]/7） | 已实现（learn/35） |
| 形态归一 | `Buf<4L>` 与 `Buf<4>` 归一到同一形态 ⇒ 认作同一个实例 | 已实现（learn/35） |
| `template<auto V>` | deduced non-type parameter：形态即类型（`K<4>` 与 `K<4L>` 是两个实例） | 已实现（learn/35） |
| NTTP **值位**参与偏特化模式 | `enable_if<true, T>` 这类模式里值位参与匹配 | 已实现（learn/32） |
| NTTP 的**任意常量表达式**实参 | `Buf<2+2>` / `Buf<k>` | 已实现（learn/39 / learn/40） |
| NTTP **形参本身**的偏特化 | `Box<int, N>`（按 N 的值挑偏特化） | 未做（learn/32 §5 边界） |
| 字面量溢出自动升格 | `3000000000` 的类型应是 `long`（[lex.icon]/2） | 未做 |
| 非整型 NTTP | 指针 / 左值引用 / 枚举 / `std::nullptr_t` / 字面量类 | 未做 |
| 函数模板的 NTTP | | 未做（显式实参直接报 not supported yet） |
| `VarExpr` 按**名字**匹配替换 | clang 走 `DeclRefExpr` 的 `ValueDecl*` 绑定 | 未做（已知边界：同名局部变量遮蔽 NTTP 时会误伤） |
| 包展开 / 可变参数模板 | | 未做 |

## ① 理论背景

### 两类模板形参（[temp.param]）

| 形参写法 | 标准名称 | 形态 | 实参写法 | 对应的 clang 声明 |
|---|---|---|---|---|
| `typename T` / `class T` | type-parameter | **类型** | `Box<int>` | `TemplateTypeParmDecl` |
| `int N` | non-type-parameter (NTTP) | **值** | `Buf<4>` | `NonTypeTemplateParmDecl` |

[temp.param]/6 规定 NTTP 可以是整型、枚举、指针、左值引用、`std::nullptr_t`
或字面量类类型。本项目只实现 **`int`**，够讲清"值不是类型"这条主线。

### 实参也是 tagged 的（[temp.arg]）

这是问题的核心。模板实参**不只有类型一种**：

```
template-argument := type-id | constant-expression | id-expression | ...
```

clang 用 `clang::TemplateArgument`（`clang/AST/TemplateBase.h`）表达：

```cpp
struct TemplateArgument {
  enum ArgKind { Null, Type, Declaration, NullPtr, Integral,
                 Template, TemplateExpansion, Expression, Pack };
  union { TypeSourceInfo *TypeInfo; ValueDecl *Decl; ... };
  llvm::APSInt Integer;        // ← Integral 形态用
};
```

**一个槽位要能装下"类型"或"值"**。旧实现把实参一律存成 `TypePtr`，
于是 `Buf<4>` 的 `4` **在结构上无处安放** —— 这不是"没写判断"，
而是**类型系统里根本没有表示该判断结果的地方**。

### 替换是两层的（[temp.subst]）

NTTP 比类型形参多出一层，这是最容易被忽略的一点：

| | 出现在哪 | 节点形态 | 替换函数 |
|---|---|---|---|
| 类型形参 `T` | **类型位置** | `Type{TemplateParam}` | `substituteType` |
| 非类型形参 `N` | **表达式位置** | `VarExpr{"N"}` | `cloneExpr` ★ |

模板体 `int size() { return N; }` 里的 `N`，Parser 产出的是 **`VarExpr`**
（一个变量引用），不是类型节点。所以值替换必须落在**表达式树**上：

```
cloneExpr(VarExpr{"N"}, {N → Integral:4})  →  IntLiteralExpr{4}
```

## ② 缺陷定位：信息在哪一层丢的

这是本文最有价值的部分——**报错点在 166 行，根因却分散在三处**。

### 信息链对照

| 阶段 | 位置 | 有 kind 吗 | 说明 |
|---|---|---|---|
| Parser 解析形参 | `parser.cpp:545-575` | ✅ **有** | `TemplateParam{kind, name, nonType}` 分三支构造 |
| TemplateDecl 存储 | `ast.h:627` | ⚠️ **半丢** | `templateParams` 带 kind；并存的 `typeParams`（`vector<string>`）丢了 |
| 实参 `Buf<4>` 解析 | `parser.cpp` 模板实参处 | ❌ **丢** | 只走 `parseType()`，装不下 `4` |
| `Type::TemplateId` | `type.h:268` | ❌ **丢** | `vector<TypePtr> templateArgs` |
| `instantiate()` | `template_instantiation.cpp:166` | ❌ **丢** | 签名只有 `vector<TypePtr>`，读的还是 `typeParams` |

### 三处断裂

**① 值表示装不下（最根本）**

```cpp
using TypeSubstitution = std::unordered_map<std::string, TypePtr>;
//                                                          ^^^^^^^
//                                    只能表达"type"，{N → 4} 写不进去
```

**② `instantiate()` 没读 `templateParams`**

带 kind 的结构化形参表就在同一个 struct 里，但代码读的是退化的
`templateDecl->typeParams`（`vector<string>`）——**信息在那儿，没去拿**。

**③ 实参侧根本没解析数字**

两处模板实参解析都是 `parseType()`，`Buf<4>` 的 `4` 直接报
`Expected type name`。更隐蔽的是表达式上下文的歧义消解：
失败被 `catch` 吞掉后判定"这不是 template-id"，回滚成比较表达式，
报出一堆与根因无关的错。

### 顺带修掉的真 bug

`parser.cpp` 的 NTTP 分支里也执行了 `decl->typeParams.push_back(param.name)`，
所以 `template<class T, int N>` 的 `typeParams` = `["T","N"]`（**2 个"类型"形参**），
而实参侧只收得到 1 个类型 → 个数校验必然错位。
`typeParams` 是历史遗留的退化表，**校验必须用 `templateParams`**。

## ③ 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 实参表示 | 新增 `TemplateArg{kind, type, value}`（tagged） | 对应 `clang::TemplateArgument`；一个槽位容纳两形态 |
| `Type::templateArgs` 类型 | `vector<TypePtr>` → `vector<TemplateArg>` | 模板 id 节点必须能携带值实参 |
| 替换映射 | `TypeSubstitution` 的 value 同样改 `TemplateArg` | 表要同时装 `{T→int}` 与 `{N→4}` |
| 类型实参转换 | `TemplateArg(TypePtr)` **隐式** | 对齐 clang 的隐式转换构造；且 `{{"T", Type::makeInt()}}` 读起来自然 |
| 值实参转换 | `TemplateArg::ofValue(4)` **必须显式** | 刻意保留书写摩擦：写 NTTP 实参时必须表态"这是值" |
| 形参分派依据 | `templateParams[i].kind` | **不是**看实参长什么样，而是看该位形参自己是什么 |
| 值替换落点 | `cloneExpr` 里 `VarExpr` → `IntLiteralExpr` | NTTP 名在表达式位置是 VarExpr |
| 支持的常量表达式 | 仅整数字面量 + 一元负号 | 常量折叠（`Buf<2+2>`）属 ROADMAP 主线 D |
| 函数模板 NTTP | **不实现**，显式报错 | 推导引擎只产出类型；类模板路径已完整 |

### 一个刻意的取舍

`cloneExpr` 判定"这个 `VarExpr` 是不是 NTTP"用的是**名字匹配**，没查符号表：

```cpp
auto it = subst.find(e->name);
if (it != subst.end() && it->second.isValue()) { ... }   // ← 只比对名字
```

若模板体内有同名局部变量遮蔽了 NTTP，会误伤。
正确做法是 `VarExpr` 绑定到声明（clang 走 `DeclRefExpr` 的 `ValueDecl*`
而非名字字符串）。本项目蓝图期的 `VarExpr` 尚未绑定，故以此简化——
**这是明确记录的已知边界，不是疏忽**。

## ④ clang 对照

| 本实现 | clang 源码 | 简化了什么 |
|---|---|---|
| `TemplateArg` | `clang/AST/TemplateBase.h` `TemplateArgument` | 9 种 ArgKind → 只留 2 种（Type/Integral）；无 APSInt 任意精度 |
| `TemplateParamKind` | `clang/AST/DeclTemplate.h` `TemplateTypeParmDecl` / `NonTypeTemplateParmDecl` | 用枚举 + 单结构体，不用继承（保持 AST 扁平） |
| `parseTemplateArgumentList` | `Parser::ParseTemplateArgumentList`（ParseTemplate.cpp） | 不解析任意常量表达式，只认整数字面量与一元负号 |
| `checkTemplateArguments` | `Sema::CheckTemplateArgumentList`（SemaTemplate.cpp） | 不做隐式转换（`Buf<4>` 的 4 → unsigned）、无默认实参填充、无包展开 |
| `cloneExpr` 的 VarExpr 分支 | `TreeTransform::TransformDeclRefExpr`（TreeTransform.h） | 按名字匹配而非按 `ValueDecl*` 绑定 |
| `instantiate()` 按 kind 分派 | `Sema::InstantiateClass` + `MultiLevelTemplateArgumentList` | 无多层级（外层模板）实参表 |
| `L...E` mangling | Itanium C++ ABI §5.1.8 `<expr-primary>` | 只支持整型字面量 |

## ⑤ 过程图

### 一次完整的 NTTP 实例化

```
源码:  template<int N> class Buf { int size() { return N; } };   Buf<4> b;
                │
   ┌────────────┴─ Phase 2 Parser ─────────────────────────────┐
   │ 形参: TemplateParam{kind=NonType, name="N", nonType=int}  │
   │       ↳ 只把 Type 形参压入 tparam 作用域（N 不是类型名！） │
   │ 实参: parseTemplateArgumentList()                         │
   │       '<' → 见到 IntLiteral 4 → TemplateArg{Integral, 4}  │
   └────────────┬──────────────────────────────────────────────┘
                ▼
   ┌──────────── Phase 3 Sema: checkTemplateArguments ─────────┐
   │ 逐位比对 templateParams[i].kind 与 args[i].kind           │
   │   ① 1 == 1 ✓   ② NonType ⇔ Integral ✓   ③ int 受支持 ✓    │
   │ （换成 Buf<int> 则在 ② 拦下：must be a non-type argument） │
   └────────────┬──────────────────────────────────────────────┘
                ▼
   ┌──────────── Phase 4 TemplateInstantiator::instantiate ────┐
   │ 读 templateParams（← 不是 typeParams）按 kind 填槽:       │
   │     subst = { "N" → TemplateArg{Integral, 4} }            │
   │ 实例名: Buf + "_4" = Buf_4        （'-'→'N'，'<'/'>'→'_'）   │
   │                                                           │
   │   ┌── 类型位置 ──► substituteType ──► 本次无 T，空转      │
   │   └── 表达式位置 ► cloneExpr:                             │
   │          VarExpr{"N"} ──查 subst──► IntLiteralExpr{4}  ★  │
   │                                                           │
   │ mangling: _Z 3Buf I L i 4 E E = _Z3BufILi4EE              │
   │                └┬┘ └┬┘│└─┬─┘│└┘                           │
   │                名  实参表 L i 4 E ← <expr-primary>        │
   └────────────┬──────────────────────────────────────────────┘
                ▼
       实例方法体已是 `return 4;` —— 语义分析与 CodeGen 只见常量
```

### 两形态对照总表

```
                    类型形参                  非类型形参（NTTP）
形参声明      template<class T>            template<int N>
形参 AST      TemplateParam{Type}          TemplateParam{NonType, nonType=int}
实参源码      Box<int>                     Buf<4>
实参 AST      TemplateArg{Type, type=int}  TemplateArg{Integral, value=4}
替换表        {"T" → TemplateArg{Type}}    {"N" → TemplateArg{Integral}}
模板体中出现  T  在【类型位置】             N  在【表达式位置】
模板体 AST    Type{TemplateParam,"T"}      VarExpr{"N"}
替换函数      substituteType()             cloneExpr()          ★ 两层
替换结果      int 类型节点                 IntLiteralExpr{4}    ★ 值落地
mangling      i                            Li4E
```

## ⑥ 实验

```bash
# ① 基础：NTTP 单参数
./minicc tests/tmpl/test_tmpl_21_nttp_basic.cpp
#   关注: [parse:targ] non-type argument / [subst:map] 'N' (non-type) := 4
#         ║ Blueprint: Buf <int N>      ← 修复前误打成 "typename N"
#         [subst] ★ NTTP value substitution: VarExpr 'N' → IntLiteral 4
#         ║ Mangled: Buf_4 → _Z3BufILi4EE
./tests/tmpl/test_tmpl_21_nttp_basic; echo $?      # 4

# ② 混排：类型 + 值
./minicc tests/tmpl/test_tmpl_22_nttp_mixed.cpp    # Pair_int_8 → _Z4PairIiLi8EE
./tests/tmpl/test_tmpl_22_nttp_mixed; echo $?      # 8

# ③ 不同值 → 不同实例（缓存键必须区分）
./minicc tests/tmpl/test_tmpl_23_nttp_value_distinct.cpp
#   应見两个独立实例: Buf_4/_Z3BufILi4EE 与 Buf_8/_Z3BufILi8EE

# ④ 负数: Buf<-3> → Buf_N3 / _Z3BufILin3EE（N 表负数）
./tests/tmpl/test_tmpl_24_nttp_negative; echo $?   # 253（-3 低 8 位）

# ⑤ 错误用例（应 exit=1）
./minicc tests/tmpl/test_tmpl_25_error_nttp_got_type.cpp
#   template argument 1 for 'Buf' ('N') must be a non-type argument
#   of type 'int', but 'int' is a type
./minicc tests/tmpl/test_tmpl_26_error_type_got_value.cpp
#   template argument 1 for 'Box' ('T') must be a type argument,
#   but '4' is a value

# ⑥ 单元测试（进 ctest，共 10 个，与 shell 用例互补）
ctest --test-dir build-linux -R 'Nttp|Mangle.Nttp' --output-on-failure
#   Nttp.ParamKindIsNonType             形参登记为 NonType（并固定 typeParams 的退化行为）
#   Nttp.TypeVsNonTypeDistinguished     两类形参的 kind 必须不同
#   Nttp.ValueSubstitutionReachesExprTree  方法体 → return 4（值替换落到表达式树）
#   Nttp.DifferentValuesDifferentInstances  Buf_4 / Buf_8 两个独立实例
#   Nttp.ValueUsedAsTypeThrows          NTTP 名出现在类型位置 → 抛错
#   Nttp.ArityMismatchThrows            实参个数不符 → 抛错
#   Nttp.KindMismatchThrows             形态不符 → 抛错
#   Mangle.NttpInteger / NttpNegative / NttpMixedWithType   Itanium 编码

# ⑦ 语义 oracle：mangling 与 clang 逐字符核对
clang++-18 -std=c++20 -c tests/tmpl/test_tmpl_21_nttp_basic.cpp -o /tmp/t21.o
nm /tmp/t21.o | grep Buf
#   → _ZN3BufILi4EE4sizeEv        ILi4EE 部分与本实现一致
clang++-18 -std=c++20 -c tests/tmpl/test_tmpl_24_nttp_negative.cpp -o /tmp/t24.o
nm /tmp/t24.o | grep Buf
#   → _ZN3BufILin3EE4sizeEv       Lin3E 部分与本实现一致
```

## ⑧ 已知边界与后续

| 边界 | 现状 | 归属 |
|---|---|---|
| 常量表达式 | 只认字面量与一元负号，`Buf<2+2>` 不支持 | ROADMAP 主线 D（常量折叠） |
| NTTP 类型 | 只支持 `int`（bool/枚举/指针未实现） | 主线 E |
| 数组字段 | `int data[N]` 语法尚不支持，故本文示例避开数组 | 主线 E |
| 函数模板 NTTP | 未实现，显式报错而非静默错算 | 后续 |
| 模板模板参数 / 包展开 | 未实现 | 未排期 |
| `VarExpr` 遮蔽误伤 | 按名字匹配，未查符号表绑定 | 见 ③ 的取舍说明 |
| 混排的任意组合 | `Pair<int,8>` 与 `Pair<double,8>` 均已验证（`_Z4PairIiLi8EE` / `_Z4PairIdLi8EE`，与 clang 一致） | — |

**仍未闭合的一处**：`main.cpp` 的类模板演示块会对**每个**类模板无条件实例化
一组固定实参。有了形态校验后，`template<int N>` 的模板若被演示块用类型实参
硬填会直接报错——已按 `templateParams[0].kind` 分流修正，但该演示块本身
（对用户源码里的模板"擅自实例化"）仍是既有设计，不在本次范围内。

## ⑨ 真实版多做了什么、为什么

★ 素材来源：本文 §④ clang 对照的「简化了什么」列 + §③ 的取舍说明 + §⑧ 边界表
+ `docs/BUGS.md` B17 + learn/32 / learn/35 / learn/39 的条目 + CLAUDE.md「未做」清单。
**未查证的写「（本项目未查证）」**。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `clang::TemplateArgument` 的 **9 种 ArgKind**（Null / Type / Declaration / NullPtr / Integral / Template / TemplateExpansion / Expression / Pack） | 本项目只留 **2 种**（Type / Integral）——够讲清"值不是类型"这条主线。代价：包展开（Pack）、模板模板实参（Template）等形态另走专门的表示（learn/33） |
| `llvm::APSInt` 的**任意精度**整数 | 本项目用机器整型（`long long`）——教学场景的值位不会超出 64 位；代价是超大常量需要提升处理 |
| `TemplateTypeParmDecl` / `NonTypeTemplateParmDecl` 的**继承层次** | 本项目用 `TemplateParamKind` 枚举 + 单结构体，保持 AST 扁平（与全项目"零继承"的取向一致） |
| `Parser::ParseTemplateArgumentList` 解析**任意常量表达式** | 本项目最初只认整数字面量与一元负号；后来补上常量折叠（learn/39）与常量变量（learn/40）⇒ `Buf<2+2>` / `Buf<k>` 端到端可用 |
| `Sema::CheckTemplateArgumentList` 的**隐式转换**（`Buf<4>` 的 4 → unsigned） | 本项目改为**可表示性**判据（[expr.const]/10 → [dcl.init]/7）+ **形态归一**——比 clang 更严的是"形态精确相等"那条旧规则（B17：clang 认可的三条合法程序被拒） |
| 形参的**默认实参**填充 | 未做：形参表里没有"默认值"这个槽位 |
| **包展开**（`Args...`） | 未做：形参表不能表达"参数包" |
| `MultiLevelTemplateArgumentList`（外层模板的实参表） | 未做：本项目的模板实例只处理单层实参表 |
| `TreeTransform::TransformDeclRefExpr` 按 `ValueDecl*` **绑定**替换 | 本项目按**名字**匹配（§③ 的取舍）——蓝图期的 `VarExpr` 尚未绑定声明。代价：模板体内同名局部变量遮蔽 NTTP 时会误伤（§⑧ 已登记为已知边界） |
| Itanium ABI `<expr-primary>` 的完整编码（`L...E` 支持多种值类型） | 本项目只支持**整型字面量**——mangling 的 `L i 4 E` 一段与 clang 逐字符一致 |
| NTTP **形参本身**的偏特化（`Box<int, N>`） | 未做（learn/32 §5 边界）；已做的是"NTTP **值位**参与偏特化模式"（`enable_if<true,T>`） |
| 字面量溢出自动升格（[lex.icon]/2：十进制 `3000000000` 的类型是 `long`） | 未做 |
| 非整型 NTTP（指针 / 左值引用 / 枚举 / `std::nullptr_t` / 字面量类） | 未做 |
| 函数模板的 NTTP（含 `template<auto V>` 在**函数**模板上的编码 `TnDa`） | 未做：函数模板的显式 NTTP 实参目前直接报 not supported yet，**没有能到达该编码的路径** ⇒ Itanium 的 `TnDa` 段有意不写 |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-09-16 | NTTP 首次落地：实参改 tagged（`TemplateArg`）、替换**分两层**（`substituteType` / `cloneExpr`）、形参 kind 判据改看"该位形参自己" | 旧实参一律存 `TypePtr` ⇒ `Buf<4>` 的 `4` **在结构上无处安放**——这不是"没写判断"，是类型系统里没有表示该判断结果的地方 |
| 2026-09-16 | 顺带修掉 NTTP 分支误把名字 push 进 `typeParams` 的真 bug | `template<class T,int N>` 的 `typeParams` = `["T","N"]`（2 个"类型"形参）⇒ 实参侧只收得到 1 个 ⇒ 个数校验必然错位 |
| 2026-09-16 | 类模板特化批次（learn/19 / learn/32）：NTTP **值位**参与偏特化模式 | `enable_if<true,T>` 的地基——旧的"实参含值位就跳过特化"守卫把这条路堵死了 |
| 2026-09-30 | NTTP 类型域扩展（learn/35）：整型家族 + 字符 + `auto` 形参 + 可表示性判据 + 形态归一 | 旧版要求形态**精确相等** ⇒ clang 认可的三条合法程序被拒（B17）；同时修掉一个**静默撞键**（`K<4>` 与 `K<4L>` 键相同 ⇒ 第二个静默复用第一个） |
| 2026-10-15 | 主线 D：常量折叠 + 常量传播 + NTTP 常量表达式实参（learn/39 / learn/40） | `Buf<2+2>` / `Buf<k>` 由此可用；求值归一必须发生在**算缓存键之前**，否则 `Buf<2+2>` 与 `Buf<4>` 撞键/分键都错 |
| 2026-11-02 | 源码注释对齐「四段式」标准（learn/50） | 注释对语义零影响 ⇒ logdiff 零漂移是**标准保证**的 |
