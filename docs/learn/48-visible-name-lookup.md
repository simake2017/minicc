# 48. 名字可见性：把"能不能用"交回符号表（红线改写 + B37）

> 需求原文：「能否改成类似 clang 的正规模式，或者一边 parser 一边进行简单语义识别，
> 不需要像后面那么复杂的语义识别」。
> 流程产物：`docs/specs/visible-name-lookup.spec.md`（2026-11-02 签字）、
> `docs/designs/visible-name-lookup.design.md`。
> 学习产物：本篇。
> 台账：BUGS.md B37；红线：`docs/constraints/00-全局约束.md` §二.1（措辞改写）。

## §0 定位与能力清单

"这个 TU 里声明过 S 吗"与"**在这里**可见吗"是两个不同的问题 ——
名字登记表答前者，符号表答后者；拿前者的答案去回答后者，就是本批修掉的
**静默放行**（非法程序通过）。修法不是给登记表再加一个位，而是把可见性
裁决**交回有完整信息的那一侧**（Sema 的符号表），并把红线从"手段"改写成
"目标"。

| 能力 | 一句话 | 状态 |
|---|---|---|
| 非限定名字查找沿作用域链上溯 [basic.lookup.unqual] | 类作用域别名 → 命名空间链逐级外层 → 全局 | 已实现 |
| 命名空间链**逐级外层** | `namespace A { namespace B { … } }` 里 B 能看到 A 的 S | 已实现 |
| 类作用域**先于**命名空间 | 类内别名遮蔽同名全局类型 | 已实现 |
| 同名遮蔽（局部变量遮蔽全局类型） | 判为表达式路径而非声明 | 已实现 |
| [stmt.ambig]/3「能当声明就当声明」 | 裸名 `S;` 的三岔处置（见 §6.2） | 已实现 |
| 未声明名的诊断 | `use of undeclared identifier 'S'`，与 clang 逐字（含列号 28:5） | 已实现 |
| 判据单点 `lookupVisibleType` | `resolveType` / `visit(ExprStmt)` / `visit(EmptyStmt)` 三个消费点共用 | 已实现 |
| 类模板实例的别名在成员函数体内裸用 | 走 `m_classDecls` 的 typeAliases（实例类的别名没进符号表） | 已实现 |
| 可见类型名的诊断（clang 发 warning） | minicc 不发 warning，走合法路径 | 部分 |
| 基类成员查找 / `using Base::f;` 引入名 | 只有"当前类的 typeAliases" | 未做 |
| 嵌套类 | 解析期即拒收（`Expected type name`） | 未做 |
| 命名空间成员的**变量**可见性 | `namespace N { int g; void f(){ g; } }` 里 g 查不到 | 未做 |
| 命名空间模板蓝图的前缀化 | 修前按**书写名**注册 ⇒ `N::B` 的模板在全局以裸名可见（错绑） | 修前未做（learn/49 已修，B38） |
| 解析期活符号表（clang 的 Actions 模式） | 无 | 未做（Stage 3） |

---

## 1. 理论背景

### 1.1 [basic.lookup.unqual]：非限定名字查找沿作用域链上溯

C++ 里一个裸名字 `S` 的含义**由它出现的位置决定**，而不是由"本 TU 里有没有
S"决定。查找顺序（[basic.lookup.unqual]）：

```text
裸名 S 的查找（当前在 class C 的成员函数体内）
   │
   ├─ ① 最内层块作用域（局部变量/参数）—— 找到就【遮蔽】外层同名类型
   ├─ ② 外层块作用域（逐层上溯）
   ├─ ③ 类作用域 C（成员、类型别名）—— [basic.lookup.unqual]/2
   ├─ ④ 命名空间作用域：最内层命名空间 → 逐级外层 → 全局
   └─ ⑤ 全局命名空间
```

★ 三条推论（每一条都对应本项目的一个坑）：

1. **同名遮蔽**：`int S = 5;` 在块作用域里遮蔽全局的 `struct S` ⇒ 该处 `S;`
   是**表达式**不是声明（clang：`warning: expression result unused`）。
2. **命名空间链逐级外层**：`namespace A { struct S{}; namespace B { … } }` 里
   B 内**能**看到 A 的 S（clang 的 DeclContext 链）。
3. **类作用域先于命名空间**：类内别名遮蔽同名全局类型。

### 1.2 [stmt.ambig]/3：「能当声明就当声明」

语句 `S;` 有两种合法解释：S 是类型名 ⇒ 无声明符的声明；S 是对象 ⇒ 表达式语句。
标准规定**类型名优先**——但前提是"能判定 S 是不是类型名"。这就要求解析器能问
"这个名字在此处是不是类型"，而这**是语义问题**。

### 1.3 clang 怎么答：Sema::ClassifyName

clang 的 `Parser::TryAnnotateTypeToken`（语句层前瞻）在需要判定时**问 Sema**：

```cpp
// clang/lib/Parse/Parser.cpp:1696
Sema::NameClassification Classification = Actions.ClassifyName(
    getCurScope(), SS, Name, NameLoc, Next, SS.isEmpty() ? CCC : nullptr);
```

- `Actions` 就是 `Sema&`（`clang/include/clang/Parse/Parser.h:639`：`Sema &Actions;`）
  —— **clang 的 Parser 就持 Sema 句柄**；
- `getCurScope()` 是**当前作用域** ⇒ 查的就是"此处可见吗"；
- `Sema::ClassifyName`（`clang/lib/Sema/SemaDecl.cpp:912`）沿
  Scope/DeclContext 链查**活符号表**（`LookupParsedName`）。

⇒ **工业级的形态不是"Parser 不碰语义"，而是"Parser 问、Sema 裁、只有一张表"。**

另一条线：clang 的**模板 id 消歧不靠名字分类**，靠语法试探
（`Parser::isTemplateArgumentList`，`clang/lib/Parse/ParseTentative.cpp:1957`，
用 `RevertingTentativeParsingAction` 试解析 `<...>`）。名字分类只提供
"UndeclaredTemplate" 这一个补充信号，clang 还要**再试探一次**确认
（`Parser.cpp:1702-1712`）。

## 2. 本项目怎么落地：分两层（阶段 1 / 阶段 2）

### 2.1 为什么原来会错：B31 的登记表答的是另一个问题

B31（learn/46）起，Parser 侧有一张**构造期前向收集**的名字登记表
（NameRegistry）：它记录"本 TU 某处声明过哪些名字、属于哪一类"。

语句层裸名判定用的就是它的 **type 位**：

```cpp
// src/parse/parser.cpp（B37 之前）
if (!isDecl && !hasTemplIdSyntax
    && names().classify(type->name).isType) {   // ← 跨作用域并集！
    isDecl = true;
}
```

而 `classify()` 对裸名是**"对任意作用域 OR"**的宽近似
（`name_registry.cpp:46-50`）⇒ 它回答的是"本 TU 某处有类型 S 吗"，
**不是**"此处可见吗"。两者不是同一个问题 ⇒ 静默放行：

```cpp
namespace N { struct S { int x; }; }
int main() { S; return 0; }     // clang rc=1；B37 之前 minicc rc=0 ★ 非法程序通过
```

### 2.2 阶段 1（本批完成）：可见性裁决交回符号表

```text
Parser（无符号表）                Sema（有符号表）
─────────────────────            ─────────────────────
裸名 S; 语法试探成功
  └─ 产出 EmptyStmt{typeName="S"} ──► visit(EmptyStmt)
        （只是【候选】）              ├─ lookupVisibleType("S")
                                      │    类作用域 → 命名空间链 → 全局
                                      ├─ 命中 ⇒ 合法（无声明符声明）
                                      ├─ 未命中但声明过 ⇒ 表达式路径
                                      └─ 都没声明 ⇒ use of undeclared identifier
```

**判据单点**（红线 3）：`lookupVisibleType` 一处，三个消费点共用 ——
`resolveType` 的兜底链、`visit(ExprStmt)` 的 [stmt.ambig] 折叠、
`visit(EmptyStmt)` 的裸名裁决。原先 resolveType 与 ExprStmt 各写一份
查找（全局 + 命名空间一层），现已收敛。

**顺带修好的三处**（原先都拒收合法程序）：

| 程序 | 修前 | 修后 | 原因 |
|---|---|---|---|
| `namespace A { struct S{}; namespace B { void f(){ S; } } }` | ❌ unknown type name | ✅ | 命名空间链逐级外层 |
| 类模板实例的别名在成员函数体内裸用 | ❌ 查不到 | ✅ | 走 m_classDecls 的 typeAliases（实例类的别名没进符号表） |
| 类内别名遮蔽同名全局类型 | ❌ 用了全局那个 | ✅ | 类作用域先查（clang 顺序） |

### 2.3 阶段 2（下一步）：名字登记表退场

把 Parser 侧"是不是模板名"的闸也换成 clang 的形态——**语法试探为主、语义回查兜底**，
登记表只留 Sema 侧的限定名裁决；最终退场。详见 ROADMAP 主线 I。

## 3. 红线 1 的措辞改写（本次的元命题）

原措辞（2026-10-23 签字）：

> **语法期零语义分层**：Parser 只许持「构造期前向收集的只读名字投影」，
> 禁止持 Sema 活句柄、禁止查活符号表。

它把**手段**写成了红线，而且这个手段恰好是 clang 的反面（clang 的 Parser 就持
`Sema &Actions`）。否决记录里的三条理由，逐条看：

| 理由 | 性质 |
|---|---|
| ① 破坏语法期零语义分层（红线 1） | 循环论证 |
| ② "Sema 三趟在解析完成后才开始，活符号表回答不了单趟解析问题" | ★ 对 minicc 现状成立，**但那是架构现状不是原理约束** —— clang 边解析边 ActOn\* 登记，声明一进作用域符号表就有 |
| ③ "宁宽勿窄验收口径失效" | 关于验收标准，可通过改口径解决 |

⇒ 改写成**目标式**（2026-11-02 拍板）：

> **语义裁决权单一**：名字的种类与可见性只由 Sema 的符号表裁定（单一事实来源）。
> Parser 可以**通过窄的只读查询接口向 Sema 提问**并服从其答复（clang 的 Actions
> 模式）；**不得**自行维护第二份名字/作用域表来下结论。

按这条措辞：clang 模式 ✅ · 现方案（构造期前向扫描投影）降格为"过渡态"（它就是
第二份表）· "Parser 自建作用域链表" ❌（第二张表）。

★ **这就是为什么本批不做"Parser 侧 scope 栈"**：那张表与 Sema 的符号表会把
"这个名字此处可见吗"这个判据写两处（红线 3），而 Sema 的表**本来就有**这个能力，
且更强（还有类型实体/重载集）。

## 4. clang 源码对照表

| clang 位置 | 本实现位置 | 简化了什么 |
|---|---|---|
| `Parser::TryAnnotateTypeToken`（Parse/Parser.cpp:1660-1780） | `Parser::parseStatement` 裸名分支（parse/parser.cpp:2241 附近） | clang 问 `Actions.ClassifyName`；本实现给**候选**、把可见性推到语义期 |
| `Actions` = `Sema&`（Parse/Parser.h:639） | —（本实现 Parser 不持 Sema 句柄） | 阶段 3 才谈得上对齐 |
| `Sema::ClassifyName`（Sema/SemaDecl.cpp:912） | `SemanticAnalyzer::lookupVisibleType`（sema/semantic_analyzer.cpp） | clang 沿 Scope/DeclContext 链查活符号表；本实现是"当前类 + 命名空间链 + 全局"三段近似（**没有基类链、没有 using-declaration 引入名**） |
| `Sema::LookupParsedName`（活符号表查找） | `m_symbolTable.lookup` + `m_currentNamespace` + `m_currentClassName` | minicc 的作用域链是"三个变量拼的"，不是链 |
| `Parser::isTemplateArgumentList`（Parse/ParseTentative.cpp:1957） | TokenCheckpoint 试探（learn/45） | 本实现多一道"登记表 template 位"闸（阶段 2 拆掉） |
| `Sema::NameClassification`（Sema/Sema.h:3722，kind + payload 的 union） | `NameClassification`（learn/47，四 bool 骨架） | clang 的 payload 是具体实体（NamedDecl\*），本实现只有 bit |

## 5. 可复现实验

```bash
# ① 三个新集成用例（正例 + 两个负例）
./minicc tests/lang/test_basics_25_bare_type_name_visibility.cpp -o /tmp/t25 && /tmp/t25
./minicc tests/lang/test_basics_26_bare_type_name_namespace_not_visible_error.cpp -o /tmp/t26
./minicc tests/lang/test_basics_27_bare_type_name_class_alias_not_visible_error.cpp -o /tmp/t27

# ② 单测（7 例）
cmake --build build-linux --target unit_tests -j6
./build-linux/unit_tests --gtest_filter='NameVisibility.*'

# ③ 全量回归 + 基线
ctest --test-dir build-linux -j6
./logdiff.sh diff
```

**对照 clang 的输出**（逐字）：

```text
# basics_26（负例）
clang : test_basics_26...cpp:28:5: error: use of undeclared identifier 'S'
minicc: [ERROR] [Semantic Error] 28:5: use of undeclared identifier 'S'

# basics_25（正例）
clang : warning: declaration does not declare anything [-Wmissing-declarations]   （rc=0）
minicc:  [sema:stmt] 'S' 可见的类型名 ⇒ 判为无声明符声明                          （rc=0）
```

## 6. 关键过程 ASCII 图

### 6.1 三个"同一个问题"的判据点，收敛到一处

```text
        修前（三处各写一份查找，容易分叉）
        ───────────────────────────────
   resolveType 兜底链   visit(ExprStmt) 折叠   裸名声明判定
   ┌──────────────┐    ┌────────────────┐   ┌──────────────────┐
   │全局符号表    │    │全局符号表      │   │名字登记表 type 位│
   │当前命名空间  │    │当前命名空间    │   │（跨作用域并集！）│
   │当前类别名    │    │（无类作用域）  │   │                  │
   └──────────────┘    └────────────────┘   └──────────────────┘
        ×2 份                 ×1 份                 ×1 份 —— 三份不是同一件事

        修后（一处）
        ───────────────────────────────
              lookupVisibleType(name)
              ┌──────────────────────────────┐
              │① 当前类作用域 typeAliases     │
              │② 命名空间链（内→外，逐级）    │
              │③ 全局                         │
              └──────────────────────────────┘
                    ▲         ▲         ▲
                    │         │         │
             resolveType  ExprStmt   EmptyStmt
```

### 6.2 裸名声明的三岔处置

```text
                    EmptyStmt{typeName="S"}
                              │
                    ① lookupVisibleType("S")
                    ┌─────────┴─────────┐
                   命中                未命中
                    │                   │
                    ▼            ② isNameDeclared("S")
             无声明符声明          ┌──────┴──────┐
             （合法，rc=0）      声明过        没声明过
                                │              │
                                ▼              ▼
                        表达式路径        use of undeclared
                        （clang:         identifier 'S'
                         warning）        （clang 逐字，rc=1）
```

## 7. 边界与后续

| 边界 | 现状 | 计划 |
|---|---|---|
| 类作用域链 | 只有"当前类的 typeAliases"，无基类成员、无 `using Base::f;` | 阶段 2/3 |
| 嵌套类 | 解析期即拒收（`Expected type name`） | 特性未做，另案 |
| 命名空间模板蓝图 | 按**书写名**登记 ⇒ `N::B` 的模板在全局以裸名 `B` 可见（错绑） | B38 另案（蓝图前缀化） |
| 命名空间成员**变量**的可见性 | `namespace N { int g; void f(){ g; } }` 里 g 查不到 | 另案（与本条同族：作用域链不完整） |
| 解析期活符号表 | 无 | 阶段 3（Sema 边解析边登记 ⇒ clang Actions 模式） |

## 8. 自检清单（这一批学到了什么）

1. **"某处声明过" ≠ "此处可见"** —— 两者是不同的问题，登记表答前者、符号表答后者；
   拿前者的答案去回答后者，就是本批修的那个静默放行。
2. **判据要单点，也要"单点得对地方"** —— 收敛到 `lookupVisibleType` 只是形式；
   关键是它落在**有完整信息的那一侧**（Sema 的符号表）。
3. **同名遮蔽是规则，不是边角** —— `int S = 5;` 让 `S;` 从声明变成表达式，
   与 clang 对齐后连"变量有没有被登记进符号表"都变了。
4. **红线该写目标，不该写手段** —— "Parser 不持 Sema 句柄"是手段，
   "语义裁决权单一"才是目标；把手段写成红线，会把 clang 的正解一并禁掉。
5. **命名空间链要逐级外层** —— 只查一层就会拒收合法程序（`namespace A { namespace B { … } }`）。

---

## 9. 真实版多做了什么、为什么

素材来自 §4 对照表的「简化了什么」列与 §7 的边界表。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `Actions` = `Sema&`（Parser 持 Sema **活句柄**），`Actions.ClassifyName(getCurScope(), …)` 查**当前作用域** | 本实现阶段 1 让 Parser 只产出**候选**（`EmptyStmt{typeName}`），把可见性推到语义期；阶段 3 才谈得上对齐 |
| `Sema::ClassifyName` 沿 Scope / DeclContext 链查活符号表 | 本实现是"当前类 + 命名空间链 + 全局"三段近似：**没有基类链、没有 using-declaration 引入名** |
| `Sema::LookupParsedName` 的完整查找 | minicc 的作用域链是"三个变量拼的"（`m_currentNamespace` / `m_currentClassName` / 符号表），不是链 |
| `Sema::NameClassification` 的 payload（具体 `NamedDecl*` 实体） | 本实现只有 bit，没有实体 —— 判据只需要"可见的类型名"这一个结论 |
| `Parser::isTemplateArgumentList` 的**语法试探**（clang 的模板 id 消歧不靠名字分类，还要再试探一次） | 本实现多一道"登记表 template 位"闸；Stage 2 拆掉、Stage 3 换成 clang 式 |
| clang 的 `did you mean` 附注（`use of undeclared identifier 'S'` 后附建议） | 文案逐字对齐优先；建议属诊断体系，未做 |
| clang 边解析边 `ActOn*` 登记（声明一进作用域符号表就有） | minicc 是三趟架构 —— 这条当初被写成红线，learn/48 已把它改写成"架构现状不是原理约束" |

---

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-11-02 | 落地 B37：可见性裁决下沉 Sema —— 新增 `lookupVisibleType`（判据单点）、`EmptyStmt` 增 `typeName` 槽把裸名带到语义期、裸名三岔处置；同时把红线 1 的措辞从"语法期零语义分层"改写为"**语义裁决权单一**" | 原措辞把**现方案**写成了红线，顺带把 clang 的实际做法（Parser 持 `Sema &Actions`）一并禁掉 —— 红线该写目标不写现状（git 4f1fb49） |
| 2026-11-02 | 顺带查实两处既有缺口（未修，已登记）：命名空间变量在块作用域不可见、嵌套类不支持 | 两者与"作用域链不完整"同族，留在 §7 边界表里 |
