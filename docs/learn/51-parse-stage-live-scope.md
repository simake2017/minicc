# 51. 解析期活作用域表：Parser 持 Sema 句柄（Stage 3 批 1）

> 定位：把"这个名字在此处是什么/可见吗"这一问，从**前向扫 token 流的近似猜测**，
> 换成 clang 的形状 —— **解析期就有一张活的作用域链，Parser 登记、Sema 裁决**。
> 关联：docs/specs/parse-stage-live-scope.spec.md ＋ docs/designs/parse-stage-live-scope.design.md
> 前置：docs/learn/48（Stage 1：可见性裁决下沉 Sema）＋ docs/learn/49（Stage 2：作用域即身份）

## §0 定位与能力清单

| 能力 | 状态 | 落点 |
|---|---|---|
| 解析期作用域链（进 / 出 / 登记 / 分类） | ✅ 批 1 已做 | `src/sema/parse_scope.h/.cpp` |
| Sema 持有该表 + 六条窄接口 | ✅ 批 1 已做 | `src/sema/semantic_analyzer.h` |
| Parser 持 Sema 句柄（clang 的 `Sema &Actions`） | ✅ 批 1 已做 | `src/parse/parser.h` |
| 11 类登记点铺满（命名空间 / 类 / 枚举 / 别名 / 模板 / 形参 / 变量 / 函数 / 成员 / 块 / for） | ✅ 批 1 已做 | `src/parse/parser.cpp` |
| 限定名逐段定位 + 不外溢 | ✅ 批 1 已做 | `ParseScopeTable::classifyQualified` |
| 作用域名字持久（离开 N 后 `N::S` 仍可达） | ✅ 批 1 已做 | `ParseScopeTable::leave` |
| value 位（不再是占位恒 false） | ⬜ 批 2 | — |
| C++20 `UndeclaredTemplate` 规则（未声明的名字按模板名解释） | ⬜ 批 2 | — |
| 三个消歧点切到活作用域表 | ⬜ 批 3 | `parser.cpp` 三处 `names()` |
| `NameRegistry` 整类删除 | ⬜ 批 3 | `include/name_registry.h` / `src/sema/name_registry.cpp` |

## §1 理论背景：clang 的形状

### 1.1 标准怎么说

- **[basic.lookup.unqual]**：非限定名字只在**当前作用域链**上查找 ⇒ "此处可见吗"
  是**作用域链**的问题，不是"本 TU 某处声明过吗"的问题。
- **[basic.lookup.qual]**：限定名 `N::B` 先解析限定者 `N`，再在 `N` 的上下文里查 `B`
  ⇒ 查不到就是查不到，**不退到全局**。
- **[basic.scope.decl]** / **[basic.scope.pdecl]**：名字的作用域从其声明处开始；
  模板形参的名字从其声明符之后开始。
- **[temp.names]/3**：模板名后跟 `<` 时，`<` **总是**模板实参表的定界符。
- **C++20 [temp.names] 的 "UndeclaredTemplate" 规则**（clang 的实现）：
  名字**查不到**且下一个 token 是 `<` 时，**必须假定它是模板名**
  （`clang/lib/Sema/SemaDecl.cpp:995-1003`）。

### 1.2 编译原理怎么说

名字查找 = **符号表 + 作用域链**。三种典型实现：

| 形态 | 做法 | 代价 |
|---|---|---|
| 单张全局表 | 所有名字进一张表 | 查得到但分不清"此处可见吗" |
| 逐层嵌套表（词法作用域） | 每层一张表，查不到上溯 | 天然的可见性；但**要维持进出配对** |
| 作用域链（clang 的 Scope） | 一条链，每层挂自己的声明 | 同上，且**限定名**可以直接沿链定位 |

本项目此前是"单张全局表"的**最弱版**：前向扫 token 流，反推名字（**猜**），
而且对裸名**跨作用域取并集**（"本 TU 某处声明过吗"）——
这恰好回答不了 [basic.lookup.unqual] 问的那一问。

### 1.3 clang 的四个机制（逐条对照）

| clang | 干什么 | minicc 批 1 的对应物 |
|---|---|---|
| `clang/include/clang/Parse/Parser.h:639`：`Sema &Actions;` | Parser 持 Sema 句柄 | `Parser::m_sema`（`SemanticAnalyzer*`，前向声明） |
| `clang/lib/Parse/Parser.cpp:427/437`：`EnterScope` / `ExitScope` → `Actions.CurScope` | 作用域进出 | `Parser::openScope` / `closeScope` |
| `clang/lib/Sema/SemaDecl.cpp:1574`：`PushOnScopeChains` | 名字入作用域 | `ParseScopeTable::declare` / `openScope` |
| `clang/lib/Sema/SemaDecl.cpp:912`：`ClassifyName` | 沿链查并分类 | `SemanticAnalyzer::classifyParseName` |

★ clang 里 `Scope`（临时）与 `DeclContext`（持久）是**两张**；
minicc 对应"活作用域链"与"语义期符号表"，同样是两张 —— 不是重复造轮子。

## §2 设计决策

### 2.1 表住在 Sema，不住在 Parser

**候选 A**：Parser 自己维护作用域链。
**候选 B**：Sema 持有表，Parser 通过窄接口登记与查询。← **采纳**

采纳 B 的理由：**红线 1（改写版）**「名字的种类与可见性只由 Sema 裁决，
Parser 只能通过窄的只读查询接口向 Sema 提问并服从答复，**不得自建第二份表**」。
docs/specs/visible-name-lookup.spec.md 的拍板记录 #1 也明确拒绝"Parser 自建作用域链表"。

★ 但登记是**写**。clang 里 Parser 通过 `Actions.ActOn*` 往 Sema 写 ——
那是 Actions 模式的精髓（解析与语义**交织**）。minicc 是两段式（先解析出 AST，
再语义分析），于是"边解析边登记"这件事只能发生在**解析期那张表**上：
它由 Sema 拥有、由 Parser 通过 `ActOn*` 式的窄接口驱动 —— 形状与 clang 同构。

### 2.2 "透明"的模板形参作用域

`template<class T> struct Box { T v; };` 里有两个名字：**`T`**（形参）与 **`Box`**（模板）。
clang 的分法：`Box` 挂外层 `DeclContext`（命名空间），`T` 挂自己的
`TemplateParameterList`。

⇒ 表里加了一条判据：**"最近的外层非模板形参作用域"**（`nameScopeIndex()`）。
形参名登记在形参域里；模板自己的名字登记在形参域**之外**。

★ 这条判据若写反（模板名登记进形参域），后果是：`Box<int> x;` 在命名空间里
**查不到 `Box`** —— 模板名随形参域一起消失。

### 2.3 可见性（活链）与限定可达（持久上下文）是两件事

| 问题 | 答的是 | 查哪儿 |
|---|---|---|
| `S;`（`S` 在 `namespace N` 里，此处是全局） | **此处可见吗** | 活链 |
| `N::S`（已离开 `N`） | **限定名可达吗** | 持久上下文 |

⇒ 表里两张结构：`m_stack`（活）与 `m_contexts`（持久）。
★ 若只留一张，要么"离开 N 后 `N::S` 查不到"（漏 [basic.lookup.qual]），
要么"`S` 在全局裸用也查得到"（漏 [basic.lookup.unqual]）—— **两个都错**。

### 2.4 局部类不持久

函数里的 `struct L{};` 在 C++ 里**没有** `f::L` 这种限定名 ⇒ 类作用域的持久化
判据加一条：**父作用域是函数 / 块 ⇒ 不持久**。

### 2.5 批 1 刻意零日志

批 1 的硬约束是**行为中立**（三个消歧点仍读旧登记表）⇒ 新增任何日志都会让
160 份集成基线全漂。⇒ 本批**一条日志都不打**，观测点留到批 3（切表时）
再补"这个名字是什么 / 为什么不是模板"的解释行。

## §3 clang 源码对照表

| clang（2026-07 语料） | 本实现 | 简化了什么 |
|---|---|---|
| `clang/include/clang/Parse/Parser.h:639`（`Sema &Actions;`） | `src/parse/parser.h`（`SemanticAnalyzer* m_sema`） | clang 是**引用**且非空；minicc 是**指针**且可空（单测自建 Parser 时空转） |
| `clang/lib/Parse/Parser.cpp:427/437`（`EnterScope`/`ExitScope`） | `src/parse/parser.cpp`（`openScope`/`closeScope`） | clang 直接改 `Actions.CurScope`；minicc 转发给 Sema 的表 |
| `clang/lib/Sema/SemaDecl.cpp:1574`（`PushOnScopeChains`） | `src/sema/parse_scope.cpp`（`declare` / `openScope`） | clang 挂 `NamedDecl*`；minicc 挂"名字 + 种类位"（类型/偏移/符号仍归语义期表） |
| `clang/include/clang/Sema/Scope.h:41`（`class Scope`） | `src/sema/parse_scope.h`（`ParseScopeTable::Scope`） | clang 的 Scope 还有诊断、修饰符等；minicc 只要"名字 → 种类位" |
| `clang/lib/Sema/SemaDecl.cpp:912`（`ClassifyName`） | `semantic_analyzer.cpp`（`classifyParseName`） | ★ 批 3 才接上消歧点 |
| `clang/lib/Sema/SemaDecl.cpp:995-1003`（C++20 `UndeclaredTemplate`） | — | ★ 批 2 才做 |
| `clang/lib/Parse/ParseTentative.cpp:1957`（`isTemplateArgumentList`） | `Parser::parseTemplateArgumentList` ＋ `TokenCheckpoint` | clang 的试探不产副作用；minicc 靠 B32 的缓冲-冲刷机制保证 |

## §4 关键过程（ASCII 图）

```text
解析 `namespace N { struct S{}; }`：

  栈底 Global                        栈顶
  ┌──────────────┐
  │ Global ""    │  N 登记进这里（带 scopePath "N::"）
  ├──────────────┤
  │ Namespace    │  path = "N::"    ← openScope(Namespace,"N",Namespace)
  │   names:     │
  │     S → Type │  ← declareName("S", Type)
  └──────────────┘
        │ closeScope()
        ▼
  m_contexts["N::"] = { S → Type }        ← 离开之后 `N::S` 仍可达

查询 `N::S`（在全局）：
  第一段 "N" → 活链里找到（Global 层）⇒ scopePath = "N::"
  末段   "S" → 在 "N::" 那层查（活链没有 ⇒ 持久上下文有）⇒ Type ✓

查询裸名 `S`（在全局）：
  活链 Global → 没有 S ⇒ 0（不可见）★ 这正是 NameRegistry 判错的地方
```

## §5 可复现实验命令

```bash
# ① 编译（0 警告）
./build.sh

# ② 单元测试（388 例，含本批新增 12 例）
cmake -S . -B build-linux -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache
cmake --build build-linux -j6
./build-linux/unit_tests --gtest_filter='ParseScope*'

# ③ 集成日志基线（160 份逐字节比对）
./logdiff.sh diff

# ④ 注释规范守卫
./check-comment-style.sh

# ⑤ clang 对拍（探针：未声明的名字 / 命名空间里的类型裸用）
clang++-18 -std=c++20 -fsyntax-only /tmp/probe1/b.cpp
#   ⇒ error: use of undeclared identifier 'S'
clang++-18 -std=c++20 -fsyntax-only /tmp/probe1/d.cpp
#   ⇒ error: no template named 'Box'（模板先使用后声明）
```

## §6 真实版多做了什么、为什么

| 本实现 | clang | 为什么可以简化 / 为什么必须多做 |
|---|---|---|
| 表只记"名字 + 种类位" | clang 记 `NamedDecl*` | minicc 的类型/偏移/符号在**语义期**另一张表里；解析期只需要"这是不是模板/类型"这一个 bit |
| 名字条目多带一个 `scopePath` | clang 的 `DeclContext` 天然带路径 | minicc 的作用域是**值语义**的路径串，得自己记"这个名字引入哪层作用域" |
| `ScopeKind` 六种 | clang 的 `Scope::ScopeType` 十余种 | minicc 没有 `ObjC`/`CXXTryBlock`/`TemplateParamList` 之外的细分类 |
| 无 `Scope` 的修饰符（inline namespace / using-directive） | clang 支持 | 本项目无这两者 |
| 模板形参域"透明" | clang 用**两层 Scope**（`TemplateParamScope` 嵌在 `DeclScope` 里） | ★ 本项目是把"透明"做成一条**判据**（`nameScopeIndex`），不是两张表 —— 效果同源，代价是"模板名登记在哪儿"要靠判据而不是结构来保证 |
| 批 1 零日志 | clang 有 `-ast-dump` 等 | 行为中立是本批的硬约束 |

## §7 边界与已知取舍

| 项 | 现状 | 说明 |
|---|---|---|
| 同名重载的函数 | 共用一条作用域路径 | `void f(int)` 与 `void f(double)` 的函数作用域路径相同 ⇒ 两者的**局部类**会撞路径（C++ 里局部类本就不可限定名，影响面极小） |
| 局部类 | 不持久 | ✔ 与 C++ 一致（没有 `f::Local`） |
| 前置声明 `struct A;` | 登记名字、**不开作用域** | 没有类体 ⇒ 没有成员可查 |
| 枚举项 | 登记进**外层**作用域 | 不限���枚举的枚举项是外层作用域的名字（[basic.scope.enum]） |
| `for` 的隐式作用域 | 用块作用域表达 | [stmt.for]/2：init 的声明在条件与循环体里可见，出了 for 看不见 |
| 类模板的模板形参域 | 覆盖到类体解析结束 | 形参在类体里可见（`template<class T> struct Box { T v; };`） |
| 三个消歧点 | **仍读旧登记表** | ★ 批 1 刻意如此（行为中立）；批 3 才切 |

## §8 下一批（Stage 3 批 2 / 批 3）

- **批 2**：value 位落地（变量 / 形参 / 函数 / 枚举项 ⇒ Value 位）+ **C++20
  `UndeclaredTemplate` 规则**（名字查不到且下一个 token 是 `<` ⇒ 假定它是模板名，
  再用 `isTemplateArgumentList` 式的语法试探兜底）。
- **批 3**：三个消歧点切到活作用域表 ⇒ `NameRegistry` 整类删除。
  ★ 切表会改诊断文案（例如未声明的名字从 "Undefined variable" 变成 clang 的
  "use of undeclared identifier"），属**有意漂移**，须与用户确认口径后重刷基线。

## 演进记录

| 批次 | 日期 | 做了什么 | 验收 |
|---|---|---|---|
| Stage 1（B37） | 2026-11-02 | 可见性裁决下沉 Sema（`lookupVisibleType` 单点） | ctest 369/369 |
| Stage 2（B38） | 2026-11-02 | 作用域即身份（模板按完整限定名注册）；登记表退场一处 | ctest 376/376 |
| **Stage 3 批 1** | **2026-11-02** | **活作用域表 + Actions 句柄 + 11 类登记点**（消歧点仍读旧表 ⇒ 行为中立） | build 0 警告 / ctest **388/388** / 160 份集成日志**逐字节零漂移** |
| Stage 3 批 2 | ⬜ | value 位 + C++20 `UndeclaredTemplate` 规则 | ⬜ |
| Stage 3 批 3 | ⬜ | 三个消歧点切表 + `NameRegistry` 删除 | ⬜ |
| Stage 3 批 4 | ⬜ | **嵌套类**（用同一张作用域链的通用查询解决，不做特判） | ⬜ |
