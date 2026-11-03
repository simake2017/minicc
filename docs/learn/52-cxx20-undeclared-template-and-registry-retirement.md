# 52. C++20 未声明模板规则 + 名字登记表退场（Stage 3 批 2+3）

> 定位：把"这个名字在此处是不是模板名"这一问的**最后一个近似**拆掉 ——
> 未声明的名字按 C++20 规则当模板名（clang 的 `UndeclaredTemplate` 分支），
> 并让前向扫描猜出来的「名字登记表」整类退场，只留 Sema 的活作用域链。
> 关联：docs/specs/parse-stage-live-scope.spec.md ＋ docs/designs/parse-stage-live-scope.design.md
> 前置：docs/learn/51（Stage 3 批 1：活作用域表 + Actions 句柄）

## §0 定位与能力清单

| 能力 | 状态 | 落点 |
|---|---|---|
| 三个消歧点改问活作用域表 | ✅ 本批 | `src/parse/parser.cpp` 三处 `names()` → `nameBits` |
| C++20 `UndeclaredTemplate` 规则（未声明的名字按模板名解释） | ✅ 本批 | `Parser::isTemplateNameHere` |
| value 位不再是占位（参与语句层分流） | ✅ 本批 | `include/name_scope.h` + 11 类登记点 |
| 内建名字种子（`std::declval`）进活表 | ✅ 本批 | `SemanticAnalyzer::SemanticAnalyzer` |
| 可见的模板名也是可见的名字（`Box;` 合法） | ✅ 本批 | `SemanticAnalyzer::isVisibleTemplateName` |
| 未声明名的诊断文案对齐 clang | ✅ 本批 | `semantic_analyzer.cpp` 的 `inferVar` 兜底 |
| `NameRegistry` 整类删除（头 / 实现 / 单测） | ✅ 本批 | `src/sema/name_registry.*` 已删 |
| 嵌套类（同一张作用域链的通用查询，不做特判） | ⬜ 批 4 | — |

## §1 理论背景

### 1.1 标准怎么说

- **[temp.names]/3 + C++20（P0846R0）**：模板名的后面跟 `<` 时，`<` 一律是模板实参表的
  定界符；且名字**在当前作用域查不到**时，解析器**必须假定**它是模板名
  （"we're required to assume that this is a template name"）。
- **[stmt.ambig]/3**：语句以标识符开头时，"能当声明就当声明" ⇒
  `S;`（S 是可见的类型名）是无声明符的声明；S 是变量时是表达式。
- **[basic.scope.scope]**：模板名与类型名**不同类** —— `Box` 是模板名不是类型名，
  但它仍是一个可见的名字，可以出现在声明的位置（clang 只给 warning）。
- **[basic.lookup.unqual]**：非限定名字沿**作用域链**上溯（内层遮蔽外层），
  不跨作用域取并集。

### 1.2 编译原理怎么说

消歧的本质是**上下文无关分析的"向前看"**：看到 `IDENT '<'` 时，解析器要在
"模板实参表"与"小于号"两条路之间选一条。教科书里的正统做法是
**按文法做试探性推导（tentative parsing）**，成功则提交、失败则回退；
而**"走哪条路"的判据**应当来自符号表（名字的种类），不是来自启发式。

本项目此前的判据来自一张**前向扫描 token 流**猜出来的表（跨作用域并集），
它既不是符号表、也不是试探推导 —— 两头都不占。

### 1.3 clang 的三个机制（逐条对照）

| 机制 | clang 落点 | 本实现（本批之后） |
|---|---|---|
| 名字分类沿活链做 | `clang/lib/Sema/SemaDecl.cpp:912:Sema::ClassifyName` | `ParseScopeTable::classify` / `classifyQualified` |
| C++20 未声明模板假定 | `clang/lib/Sema/SemaDecl.cpp:996-1003`（`SS.isEmpty() && NextToken.is(tok::less)` ⇒ `UndeclaredTemplate`） | `Parser::isTemplateNameHere`（裸名限定 + 下一个 token 是 `<`） |
| 语法试探复核 | `clang/lib/Parse/ParseTentative.cpp:1957:Parser::isTemplateArgumentList` | `Parser::parseTemplateArgumentList`（TokenCheckpoint 试探，失败即回滚） |
| 作用域链本体 | `clang/include/clang/Sema/Scope.h:41:class Scope` | `src/sema/parse_scope.h:ParseScopeTable` |

★ clang 的 C++20 分支带 `SS.isEmpty()` 条件 —— **限定名不适用**该假定
（`N::unknown<int>(y)` 里 unknown 在 N 中不存在时，clang 报
"no member named 'unknown' in namespace 'N'"，不当模板名）。
本实现的表达式闸照此收窄；语句层的模板 id 链闸不受影响
（那里的问题不是"是不是模板名"，而是"这是不是模板 id 语法"）。

## §2 设计决策

### 2.1 语句层分流判据为什么是"是不是值"而不是"是不是类型"

Parser 判 `S;` 是声明还是表达式时，问的是**活作用域表里的位**：

```
bits = nameBits("S")
  有 Value 位（这里有个变量）⇒ 表达式（变量遮蔽同名类型，clang 同）
  否则                      ⇒ 声明候选（可见性交给 Sema 裁决）
```

★ 为什么不问"是不是类型"：若要求"必须是可见的类型"，那么**完全没声明过的名字**
会落到表达式分支 ⇒ 报错变成 minicc 自造的 "Undefined variable 'q'"；
而 clang 在两种路径上都报 "use of undeclared identifier 'q'"。
按"不是值"分流，未声明的名字也走声明分支 ⇒ 交给 Sema 的 `EmptyStmt` 分支，
报出与 clang **逐字**（含列号）相同的文案。

### 2.2 C++20 假定为什么放在"名字查不到"而不是"名字不是模板"

clang 的条件是 `NotFound && NextTokenIsLess`（`SemaDecl.cpp:996`）——
**"查不到"**（NotFound）而不是"不是模板"。区别在于：名字**查得到**时，
它的种类是确定的（变量 / 类型 / 模板），此时再假定它是模板就是错的
（变量可以遮蔽模板名，[basic.scope.hiding]）。

### 2.3 为什么 Parser 自带一个 Sema

clang 的 `Parser` 持有 `Sema &Actions`（`clang/include/clang/Parse/Parser.h:639`），
**这是构造前提而不是可选配件** —— 解析期要登记、要查询，都在 Sema 那边。
本实现此前把"名字事实"放在一张独立的表里，于是 Parser 不需要 Sema；
表退场后，Parser 必须有 Sema 才能工作 ⇒ 单跑一个 Parser 时自建一个
（`std::unique_ptr<SemanticAnalyzer>`，析构函数在 .cpp 里定义以免暴露完整类型）。

### 2.4 内建名字为什么必须进活表

`std::declval<T>()` 是**表达式位置**的模板 id，而 `std::declval` 不经 token 流
（`registerBuiltins` 直接注入 AST，主线 H）⇒ 解析器看不到它的声明 ⇒
活表若不知道它，表达式闸就放行不了，解析直接失败。
clang 的对应物是 ASTContext 里预置的 `__builtin_*` 声明
（`Sema::isTemplateName` 查的就是那张表）。种子必须在**构造期**登记 ——
解析发生在语义分析之前。

### 2.5 登记表为什么必须删

那张表是**前向扫 token 流**猜出来的：它回答不了"此处可见吗"，
只能回答"本 TU 某处声明过吗"。同一个问题有两份答案时，
消歧点与语义期迟早分叉 —— 本批的两处 bug（遮蔽看不见、模板名被拒）
都是这个分叉的产物。留着它就是留着"第二份作用域事实"，
与红线 1（语义裁决权单一）直接冲突。

## §3 clang 源码对照表

| 主题 | clang | 本实现 | 简化了什么 |
|---|---|---|---|
| 名字分类 | `clang/lib/Sema/SemaDecl.cpp:912:Sema::ClassifyName` | `src/sema/parse_scope.cpp:ParseScopeTable::classify` | 只判"种类"（四个位），不返回实体 |
| C++20 未声明模板 | `clang/lib/Sema/SemaDecl.cpp:996-1003` | `src/parse/parser.cpp:Parser::isTemplateNameHere` | 不做 typo 修正（clang 注释里的 FIXME） |
| 实参表试探 | `clang/lib/Parse/ParseTentative.cpp:1957:Parser::isTemplateArgumentList` | `src/parse/parser.cpp:Parser::parseTemplateArgumentList` + TokenCheckpoint | 失败信号是 ParseError，不是 TPResult |
| 作用域链 | `clang/include/clang/Sema/Scope.h:41:class Scope` | `src/sema/parse_scope.h:ParseScopeTable` | Scope 节点不销毁，靠"持久上下文"补 |
| 名字入作用域 | `clang/lib/Sema/SemaDecl.cpp:1574:Sema::PushOnScopeChains` | `src/parse/parser.cpp:Parser::declareName` | 登记发生在解析点，不是 ActOn* 点 |
| Actions 句柄 | `clang/include/clang/Parse/Parser.h:639` | `src/parse/parser.h:SemanticAnalyzer* m_sema` | 指针而非引用（可空 ⇒ 单测可跑） |

## §4 关键过程（ASCII 图）

```
【语句层：`S;` 是什么】
  Parser                                  Sema（活作用域表）
    │ nameBits("S")  ────────────────────▶  沿链自内向外查，首命中即返回
    │                                     ├─ 命中且有 Value 位 ⇒ 表达式
    │  有 Value 位 ⇒ 表达式路径            └─ 否则 ⇒ 声明候选
    │  否则 ⇒ 声明路径                          │
    │                                           ▼
    │                                     lookupVisibleType("S")   ← 可见性裁决
    │                                     ├─ 命中 ⇒ 判为无声明符声明（rc=0）
    │                                     ├─ 是模板名 ⇒ 同上（rc=0，clang 只给 warning）
    │                                     └─ 都没命中 ⇒ use of undeclared identifier 'S'

【表达式层：`unknown<int>(y)` 是什么】
  Parser
    │ 下一个 token 是 '<' 吗？ ── 否 ⇒ 比较运算
    │ 是 ⇒ nameBits("unknown")
    │        ├─ 有 Template 位 ⇒ 模板 id，继续试探实参表
    │        ├─ 查得到（非 0）  ⇒ 不是模板（变量遮蔽）⇒ 比较运算
    │        └─ 查不到 ⇒ ★ C++20 假定它是模板名（裸名限定）
    │               ⇒ 试探实参表：成功 ⇒ 模板 id；失败 ⇒ 回滚成比较运算
    ▼
  Sema：实参 'y' 未声明 ⇒ use of undeclared identifier 'y'（与 clang 逐字）
```

## §5 可复现实验命令

```bash
# ① 编译（0 警告）
JOBS=6 ./build.sh

# ② 单元测试（382 例；本批新增 4 例 ParseScopeRetirement.*）
cmake --build build-linux -j6 && (cd build-linux && ctest)

# ③ 集成日志基线（162 份逐字节比对）
./logdiff.sh diff

# ④ clang 对拍（本批的四组探针）
cat > /tmp/p.cpp <<'EOF'
int main(){ unknown<int>(y); return 0; }
EOF
clang++-18 -std=c++20 -fsyntax-only /tmp/p.cpp   # ⇒ use of undeclared identifier 'y'
./minicc /tmp/p.cpp                              # ⇒ 同文案、同列号

# ⑤ 模板名单独成句（clang 只给 warning，rc=0）
cat > /tmp/q.cpp <<'EOF'
template<class T> struct Box { T v; };
int main(){ Box; return 0; }
EOF
clang++-18 -std=c++20 -fsyntax-only /tmp/q.cpp   # ⇒ warning: declaration does not declare anything
./minicc /tmp/q.cpp                              # ⇒ rc=0
```

## §6 真实版多做了什么、为什么

- **多做了①**：把"名字的种类"从一个**跨作用域并集**改成**沿链首命中**。
  代价是旧表的两个特性消失（"别的命名空间里声明过也算"、"先使用后声明也算"），
  换来的是"此处可见"这件事只有一个答案。后者靠 C++20 假定补回 ——
  先使用后声明的模板仍能被识别（名字查不到 ⇒ 假定是模板名）。
- **多做了②**：诊断文案也一并对齐 clang（"Undefined variable" ⇒
  "use of undeclared identifier"）。旧文案与同一名字在另一条路径上的文案
  **自相矛盾**（同一份代码里两套说法），对齐后只剩一套。
- **多做了③**：顺手修掉两个真 bug（详见 docs/BUGS.md B40/B41）：
  遮蔽在 Parser 层看不见（`int S;` 之后的 `S;` 被当成声明）、
  模板名单独成句被拒收（`Box;` 是合法程序）。
- **没做**：限定名的 C++20 假定（clang 的 `SS.isEmpty()` 条件）只落在**表达式闸**；
  语句层的模板 id 链闸仍对限定名放行（那里的问题不是"是不是模板名"），
  否则 `N::B<int> x;`（N 里没有 B）会退化成比较链，
  丢掉与 clang 逐字一致的 "no template named 'B' in namespace 'N'"。

## §7 边界与已知取舍

| 项 | 现状 | 说明 |
|---|---|---|
| `unknown<x>(y)` 的 'x' | minicc 不报 | clang 报 "use of undeclared identifier 'x'"（模板实参不是类型）；minicc 的实参类型只在**模板已知**时才解析 ⇒ 报不出来 |
| 限定名未声明模板 | minicc 报 parse error | clang 报 "no member named 'unknown' in namespace 'N'" + 一条 parse 恢复错误 |
| 先使用后声明的模板 | minicc 接受 | clang 拒（"no template named 'Box'"）—— Sema 三遍扫描先登记了全部模板；**宽方向**的已知边界 |
| 限定名未声明的诊断文案 | 仍是 "Undefined variable 'A::B::f'" | clang 是 "no member named 'f' in namespace 'A::B'"（要区分限定者是命名空间还是类） |
| 类作用域的路径 | 同名重载的函数共用一条路径 | 与批 1 相同（见 docs/learn/51 §7） |
| 嵌套类 | 未做 | 批 4：用同一张作用域链的通用查询解决，**不做特判** |

## §8 下一批（Stage 3 批 4：嵌套类）

用户原始问题：`class A { B; }` 里的 `B` 与全局的 `B` 会不会串。
本批把"名字在哪一层当真"这件事收敛到一张表之后，嵌套类的正解就是
**同一张表的通用查询**：嵌套类登记为外层类作用域的成员，
`A::B` 走限定名查询（限定者在类作用域里找，不外溢到全局）。
★ 不做 `findClassScopeType` 一类特判 —— 特判就是"为解决而解决"。

## 演进记录

| 批次 | 日期 | 做了什么 | 验收 |
|---|---|---|---|
| Stage 1（B37） | 2026-11-02 | 可见性裁决下沉 Sema（`lookupVisibleType` 单点） | ctest 369/369 |
| Stage 2（B38） | 2026-11-02 | 作用域即身份（模板按完整限定名注册） | ctest 376/376 |
| Stage 3 批 1 | 2026-11-02 | 活作用域表 + Actions 句柄 + 11 类登记点（行为中立） | ctest 388/388 |
| **Stage 3 批 2+3** | **2026-11-02** | **三个消歧点切表 + C++20 假定 + value 位落地 + `NameRegistry` 整类删除 + 诊断文案对齐** | build 0 警告 / ctest **382/382** / 162 份集成日志**逐字节零差异**（除本批两处有意漂移） |
| Stage 3 批 4 | ⬜ | **嵌套类**（同一张作用域链的通用查询，不做特判） | ⬜ |
