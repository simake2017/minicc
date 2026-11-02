# 线：一次名字查找从 Token 到符号表

- 读者：想知道「源码里写一个 `S`，编译器怎么知道它指哪个东西」的人；想读懂 [basic.lookup] 与「名字种类 ≠ 名字可见性」这条分界的人
- 审法：每跳的 `文件:函数` 都能在 `src/` 里 grep 到；§4 的每条候选路径都能在源码里指出分支位置
- 名词版本：v1

> 仓库：mycompiler @ `main` / `c79656d`（源码锚点绑定此版本，换版本要复核）
> 入口：源码里的一个标识符 `S`（既可以是类型名，也可以是变量名）
> 结局：这个名字要么在符号表里**命中**（拿到类型/偏移/符号），要么**未命中**并响亮报错

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 首版：按 06-文档骨架规范 §5 的线文档骨架新建 |

## §1 这条线是什么

一个名字从**字符**变成**符号表里的一条记录**（或一条错误）的全过程。

入口是源码里的一处书写，例如 `S x;` 里的 `S`，或 `x = 1;` 里的 `x`。
结局有三类：

1. **类型名命中** —— `S` 被兑现成一个具体类型（带布局、带偏移、带 vtable）；
2. **值名命中** —— `x` 被兑现成一个类型 + 栈偏移（`stack@-8`）；
3. **未命中** —— 响亮报错（`unknown type name 'S'` / `Undefined variable 'x'` /
   `use of undeclared identifier 'S'`）。

★ 这条线的**真正难点不在"查一次表"**，而在三个岔路口：

- **名字的种类**：它是类型？是值？是命名空间？是模板名？（[basic.lookup] 的分类）
- **名字的可见性**：在这个**位置**、这个**作用域**里能看见吗？（[basic.lookup.unqual]）
- **同名遮蔽**：局部变量遮蔽全局类型、派生类字段遮蔽基类字段（[class.member.lookup]/3）

本项目把这三问**拆到了三个不同的层**：

| 问 | 答的层 | 判据单点 |
|---|---|---|
| 「这个名字**声明过**吗（哪种类）」 | Parser 侧只读登记表 | `NameRegistry::classify` |
| 「这个名字**在这里可见**吗」 | Sema 符号表 | `SemanticAnalyzer::lookupVisibleType` |
| 「这个名字**在这条路径上**指什么」 | Sema 各处上下文判据 | `inferVar` / `resolveType` / `findField` |

### 链路总图

```text
        源码字符：  S   x   Alias
                    │
                    ▼
[Lexer] scanIdentifierOrKeyword  → Token{Identifier, "S"}
   （查关键字表；不是关键字 ⇒ Identifier）
                    │
                    ├──►  ┌─────────────────────────────────────────┐
                    │     │ [NameRegistry::collect]  ★ 断链点⑤     │
                    │     │ 构造期【前向扫一遍 token 流】             │
                    │     │ 5 条规则 ⇒ (scope,name) → 位掩码         │
                    │     │ type 位 / namespace 位 / template 位      │
                    │     └─────────────────────────────────────────┘
                    │                    │
                    │                    ▼
                    │     [Parser] names().classify("S").isType ?
                    │     [Parser] names().isTemplateName("S") ?
                    │        （只问"种类"，不问"可见性"）
                    ▼
[Parser] 建名：
   类型位 → parseType   → Type::makeClass("S")   （空布局的 Class 节点）
   值位   → parsePrimaryExpr → VarExpr("S")
   语句位 → parseStatement → TokenCheckpoint + parseType() 试探
                     ⇒ EmptyStmt{typeName="S"}
                    │
                    ▼
[Sema Pass 1] 注册：processDecl → processClassDecl / registerFunction
   ⇒ [register] class 'Global'
   ⇒ [register] global variable 'N::g_count' : int
   （命名空间成员按【完整限定名】登记：N::g_count）
                    │
                    ▼
[Sema Pass 3] 名字裁决
   ┌──────────────────────────────────────────────────────────┐
   │ 类型位：resolveType(Class 节点)                          │
   │   ① 查 m_symbolTable（全局精确名）—— 命中即返回注册类型  │
   │   ② 未命中 ⇒ lookupVisibleType(name) ★ 断链点②           │
   │        ├─ 类作用域别名（当前类 typeAliases）  level=1     │
   │        ├─ 命名空间链（N::M::name → N::name → name）      │
   │        └─ 全局                                        level=3│
   │   ③ 仍未命中 ⇒ error "unknown type name 'S'"             │
   ├──────────────────────────────────────────────────────────┤
   │ 值位：inferVar(VarExpr) ★ 断链点④                         │
   │   ① 限定名成员（Q::v，限定者是类型 ⇒ 字段 + 隐式 this）  │
   │   ② m_symbolTable.lookup（作用域链）  ← 命中 ⇒ 回填类型   │
   │   ③ 当前类的实例字段（classLayout.findField）             │
   │   ④ 当前类 + 基类链的 static 成员                        │
   │   ⑤ m_functionMap（函数名）                              │
   │   ⑥ 全未命中 ⇒ error "Undefined variable 'S'"            │
   └──────────────────────────────────────────────────────────┘
                    │
                    ▼
   命中 ⇒ 类型/偏移/符号就绪，CodeGen 之后按偏移寻址
   未命中 ⇒ 响亮报错
```

★ 标星处是**跨层接缝**（§4 断链点）。

## §2 走线五步法的结论（一句话一跳的表）

| # | 跳 | 落在哪个文件:函数 | 这一跳干了什么 |
|---|---|---|---|
| 1 | 字符 → Token | `src/lex/lexer.cpp:Lexer::scanIdentifierOrKeyword` | 吃 `[A-Za-z0-9_]+`；查关键字表 ⇒ `Token{Identifier, text}` 或关键字 token |
| 2 | Token 流 → 名字登记表 | `src/driver/main.cpp:main`（`nameReg->collect(tokens)`）+ `src/sema/name_registry.cpp:NameRegistry::collect` | **构造期**前向扫一遍 token 流，5 条规则收进 `(scope,name) → 位掩码` 表 |
| 3 | Token → AST 名字节点 | `src/parse/parser.cpp:Parser::parseType` / `Parser::parsePrimaryExpr` / `Parser::parseStatement` | 类型位建 `Type::makeClass(name)`（空布局）；值位建 `VarExpr(name)`；语句位试探后建 `EmptyStmt{typeName}` |
| 4 | 名字分类 | `src/sema/name_registry.cpp:NameRegistry::classify` / `classifyQualified` / `isTemplateName` | 查表答「这个 name 是 type / namespace / template 中的哪几类」；**只答种类，不答可见性** |
| 5 | 作用域裁决（类型位） | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::lookupVisibleType` | 三级查找：类作用域别名 → 命名空间链（逐级外层）→ 全局；返回 `VisibleType{type, raw, level, scope}` |
| 6 | 符号表命中（值位） | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::inferVar` → `src/sema/semantic_analyzer.h:Scope::lookup` / `SymbolTable::lookup` | `Scope::lookup` 从当前层沿 `parent` 链向外找；命中即回填类型 |
| 7 | 未命中裁决 | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::isNameDeclared` → `SemanticAnalyzer::visit(EmptyStmt)` | 第二问「声明过吗」；两问都不中 ⇒ `use of undeclared identifier 'S'` |
| 8 | 限定名路径 | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::lookupVisibleType`（命名空间链） + `NameRegistry::classifyQualified` | `N::S` 按**完整限定名**查，**不剥尾**（B38 由宽改严） |
| 9 | 成员路径（旁路） | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::inferMember` → `findField` / `findMethodInClass` | `obj.x` 不走名字表，走类布局表（偏移）与类方法表 |
| 10 | 名字的回填 | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::inferVar` / `SemanticAnalyzer::resolveType` | 把「名字」换算成机器要的东西：类型 / 偏移 / 符号名 |

### 走线五步法的执行记录

| 步 | 做了什么 | 证据 |
|---|---|---|
| 1 跑通 | 真的跑了 `./minicc` 跑 5 个自制样例：全局类型+变量、命名空间类型、类作用域别名、裸 `S;`、未声明名字 | `S;` 命中 ⇒ `[sema:stmt] 'S' 可见的类型名 ⇒ 判为无声明符声明`；`namespace N { struct S{}; }` 里的 `S;` ⇒ `use of undeclared identifier 'S'` |
| 2 画图 | 上面的链路总图 | 本节 |
| 3 逐跳 | §2 表 + §3 详解 | §3 |
| 4 找动机 | 只在 ★ 处问 | §3 每跳末尾 |
| 5 写疑问 | 见 §5 的边界清单 | §5 |

## §3 逐跳详解

### 第 1 跳：字符 → Token

**源码锚点**：`src/lex/lexer.cpp:Lexer::scanIdentifierOrKeyword`。

**做了什么**：吃尽 `[A-Za-z0-9_]+`，然后查 `kKeywordMap`：
命中 ⇒ 返回关键字 token（`KwClass` / `KwInt` / …）；不命中 ⇒
`makeToken(TokenType::Identifier, text, loc)`。

★ **词法期不打任何日志**：`Lexer` 全文零 `std::cout`。可观测性只有一个聚合数字
（`main.cpp` 里 `N tokens generated`）和 `--dump-tokens` 时的整表转储。
这不是疏忽，是「日志即契约」纪律下的取舍：逐 token 打点会把 154+ 份集成基线全漂掉。

**为什么不用更简单的做法**：不在词法期就查符号表——词法器没有作用域概念，
它甚至不知道这个 `S` 是在命名空间里还是在类体里。

### 第 2 跳：Token 流 → 名字登记表

**源码锚点**：`src/driver/main.cpp:main`（`auto nameReg = std::make_shared<NameRegistry>();
nameReg->collect(tokens);`） → `src/sema/name_registry.cpp:NameRegistry::collect`。

**做了什么**：**在解析开始之前**，把整条 token 流前向扫一遍，按 5 条规则
往 `(scope, name) → 位掩码` 的 map 里塞：

| 规则 | 来源 token | 置的位 |
|---|---|---|
| 1 | `template` 头：形参表后的 `struct`/`class`/`using` 名；其它形态取第一个「标识符 + `(`」 | template 位 |
| 2 | `struct` / `class` / `enum` 声明（**template 头后的除外**） | type 位 |
| 3 | `typedef`（取 `;` 前最后一个标识符的近似） | type 位 |
| 4 | `using X = ...`（Ident + `=` 形；`using Base::f;` / `using namespace X;` **不收**） | type 位 |
| 5 | `namespace X {` / `namespace A = B;`（并推作用域栈，按花括号深度配对） | namespace 位 |

★ **template 头后的名字不置 type 位**：`Box;` 对一个类模板名应当保持「非类型」解释
（否则 `Box;` 会被当成无声明符的类型声明），模板位由规则 1 负责。

★ 三个位是**并集**语义：`int S;` 与 `struct S;` 可以并存于同一作用域，
所以 `(scope,name)` 的值是位掩码的 OR 累加，不是单选枚举。

★ 登记表**零日志**（`name_registry.cpp` 头注释：「全文件零日志（B31 纪律：
收集/查询打点会漂移全部基线）」）。

**为什么不用更简单的做法**：不在解析期边走边登记——那样 Parser 遇到
「先使用后声明」就查不到（`f()` 写在 `f` 的定义之前是合法 C++），
而前向扫描一次就解决了顺序问题。

### 第 3 跳：Token → AST 名字节点

**源码锚点**：`src/parse/parser.cpp:Parser::parseType`（类型位） /
`Parser::parsePrimaryExpr`（值位） / `Parser::parseStatement`（语句位）。

**做了什么**：同一个标识符在三个语法位置建成三个**不同形状**的节点：

| 位置 | 建什么 | 源码锚点 | 后续由谁兑现 |
|---|---|---|---|
| 类型位 | `Type::makeClass(name)` —— **空布局**的 Class 节点 | `parseType` 的 Identifier 分支 | `resolveType` |
| 值位 | `VarExpr(name)` | `parsePrimaryExpr` 的 Identifier 分支 | `inferVar` |
| 语句位（裸 `S;`） | `EmptyStmt{typeName="S"}` | `parseStatement` 的 Identifier 分支 | `visit(EmptyStmt)` |

★ **语句位是三岔里最微妙的一支**：裸 `S;` 在语法上可以是
「无声明符的声明」也可以是「表达式语句」。Parser 的做法是
`TokenCheckpoint` + `parseType()` **试探**（B35）：
试探成功且后面跟 `;` ⇒ 候选是声明；试探失败 ⇒ 走表达式。
试探期间打的 `[parse:type]` 日志随缓冲整体丢弃（未 commit 就析构回卷），
所以成功路径的输出与「回滚后正式 parseType」逐字节一致。

★ **限定名**：`N::S` 在 Parser 里被**拼成一个整串**（`name += "::" + ident`），
全项目都按「名字串」查表。类型位还会建 `nestedQualifier` 链
（`Type::makeClass(tail)` + `nested->nestedQualifier = qual`）。

**为什么不用更简单的做法**：不在 Parser 里就决定「`S` 是不是类型」——
Parser 不持 Sema 句柄，只能问「名字的种类」这一个 bit（见第 4 跳）。

### 第 4 跳：名字分类

**源码锚点**：`src/sema/name_registry.cpp:NameRegistry::classify` /
`NameRegistry::classifyQualified` / `NameRegistry::isTemplateName`。

**做了什么**：查表答「这个 name 是 type / namespace / template 中的哪几类」。

`classify(name)` 的三条查询路径（**全部列出**）：

| # | 路径 | 判据 | 命中范围 |
|---|---|---|---|
| ① | 全局精确 | `m_entries.find({"", name})` | 全局作用域的裸名 |
| ② | 限定名精确 | 名字含 `::` ⇒ `split` 成 `(scope, tail)` 后精确查 | 该命名空间里的那个名字 |
| ③ | 末段任意作用域 OR | 末段对**任意**作用域命中即可，位取并集 | 宽方向（宁宽勿窄） |

`classifyQualified(scope, name)` 只有一条：scope **必须精确相等**，不做末段回退。

★ **空值语义钉死**：查不到 = 返回默认对象（四位全 false），
**不是**「存在但 bits 为空」。消费点一律按「位未命中」处置，
**禁止**由「全 false」反向推断「它不是类型」。这是本项目反复强调的
「查不到 ≠ 不是」（spec 异常 1 / learn/47 §6）。

★ **`isValue` 位恒 false**（占位）：值名（变量/函数）不经 token 流收集，
所以登记表**答不了「`x` 是不是一个变量」**——这正是本项目与 clang 的分水岭
（clang 的 `Sema::ClassifyName` 查活符号表，能答；本项目的登记表是
「跨作用域并集」的只读投影，只能答种类）。

**本线走哪支**：第 4 跳在 Parser 侧走 `classify`（路径 ①③）；
Sema 侧的 `classifyQualified` **当前没有消费点**——B38 把 Sema 的
登记表成员（`m_names` / `setNameRegistry`）删掉了，限定名的裁决改由
Sema 自己的注册表回答。`classifyQualified` 这个入口留着，等 Stage 3
（边解析边 `ActOn*` 登记）接回去。

**为什么这么设计**：登记表的定位被 B37 明确改写为
**「值不值得让 Sema 查一下」的候选过滤器**，而不是可见性的答案。
把「种类」与「可见性」两件事交给同一个 bit 回答，是这条线上最大的坑。

### 第 5 跳：作用域裁决（类型位）

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::lookupVisibleType`。

**做了什么**：回答「这个名字**在此处**可见吗、且它是个类型吗」。
三级查找（**顺序即优先级**）：

| 级 | 查找面 | 命中时 `level` | 日志 |
|---|---|---|---|
| ① | 当前类的 `typeAliases`（`using X = T;` / `typedef T X;`）—— 走 `m_classDecls` 而不是符号表 | 1 | `[sema:alias] Alias 在类 'Outer' 作用域内 ⇒ int` |
| ② | 命名空间链：`N::M::name` → `N::name` → `name`（**逐级剥外层**） | 2 | `[resolve] 'S' 在命名空间 'N' 内 ⇒ N::S` |
| ③ | 全局 | 3 | （走 ② 的 `scope.empty()` 分支） |
| — | 全未命中 | 0 | `level=0` ⇒ 不可见 |

★ **类作用域先于命名空间**，与 clang 一致（类内别名遮蔽同名全局类型）。

★ **命名空间链逐级外层**是 clang 的 `DeclContext` 链语义：
`namespace A { struct S{}; namespace B { void f(){ S; } } }` 里 B 内的 S 可见 A 的 S
（此前只查一层 ⇒ 报 `unknown type name`，**拒收合法程序**）。

★ ① 走 `m_classDecls` 而不是符号表：类模板**实例**的别名只克隆进了
实例类的 `typeAliases`，没进符号表——走符号表会漏掉
`Box<int>::type` 在成员函数体内的非限定用法。

**本线走哪支**：取决于源码。本线的样例 `Alias`（类作用域别名）走 ①；
`S`（命名空间里的类型）走 ②。

**为什么不用更简单的做法**：不把三段手写查找摊在 `resolveType` 里——
B37 之前正是那么写的（全局 → 命名空间一层 → 当前类），
三处各写一份、各自演化。收口成一处的直接收益：
`resolveType` 的兜底链、`visit(ExprStmt)` 的 [stmt.ambig] 折叠、
`visit(EmptyStmt)` 的裸名裁决，三条路走的是**同一个** `lookupVisibleType`。

### 第 6 跳：符号表命中（值位）

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::inferVar`
→ `src/sema/semantic_analyzer.h:SymbolTable::lookup` → `Scope::lookup`。

**做了什么**：`inferVar` 的第 1 步（限定名成员除外）就是
`m_symbolTable.lookup(expr.name)`：

```cpp
Symbol* sym = m_symbolTable.lookup(expr.name);   // → m_currentScope->lookup(name)
```

`Scope::lookup` 的算法只有三行，但它是整条线最底层的原语：

```cpp
Symbol* Scope::lookup(const std::string& name) {
    auto it = m_symbols.find(name);
    if (it != m_symbols.end()) return &it->second;   // 本层命中即返回
    if (m_parent) return m_parent->lookup(name);      // 否则沿 parent 链向外
    return nullptr;
}
```

★ **内层优先**由链的顺序天然给出：局部变量遮蔽全局变量、局部作用域遮蔽函数作用域。
作用域链的形状：`global ← function ← block`（`enterScope` 压栈、
`exitScope` 弹栈，`m_allScopes` 保留对象供 `dump`）。

★ 命中后 `inferVar` 做一件机器要的事：**回填**。
若命中的是类类型，还要去 `m_classTypes` 换成带布局的注册版本
（符号表里存的是空布局的 Class 节点）。

实测日志：

```text
  [symbol] ✚ local : int    stack@-16   ← 加入符号表
    [resolve] 'local' → int    (kind=Variable, stack@-16)
```

**为什么不用更简单的做法**：不把作用域链拍平成一张全局表——
拍平了就丢掉了「同名在不同作用域是不同的东西」这条根本事实，
遮蔽规则也无从表达。

### 第 7 跳：未命中裁决

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::visit(EmptyStmt)`
→ `SemanticAnalyzer::isNameDeclared`。

**做了什么**：语句位的裸名走的是**两问**：

| 问 | 判据 | 命中 ⇒ |
|---|---|---|
| 第一问：「它是**此处可见的类型**吗」 | `lookupVisibleType` | 判为无声明符声明，折叠为空（不发射汇编） |
| 第二问：「这个名字**声明过**吗」（任意 kind） | `isNameDeclared` | 走表达式路径（局部变量遮蔽全局类 `S`） |
| 两问皆否 | — | 响亮报错 `use of undeclared identifier 'S'` |

`isNameDeclared` 的查找面与 `lookupVisibleType` **不同**：
它查任意 kind（变量/函数/类型都算），且**先查局部→全局**（`m_symbolTable.lookup(name)`），
再沿命名空间链逐级外层。

★ 两问的分工对应 clang `Sema::ClassifyName` 答的「它是什么」——
本实现拆成两问，因为 minicc 的类作用域没有变量、局部作用域没有类型，
两问的查找面天然不同。

★ 实测（两个样例，同一个 `S;` 写法，结论相反）：

```text
// 全局可见
  [parse:stmt] 'S' 判为声明（type 位）
  [sema:stmt] 'S' 可见的类型名 ⇒ 判为无声明符声明

// 只在 N 里，全局不可见
  [parse:stmt] 'S' 判为声明（type 位）
[ERROR] [Semantic Error] 3:5: use of undeclared identifier 'S'
```

——注意两例里 Parser 的判定**完全相同**（「判为声明」），差别只在 Sema 那一跳。
这就是「Parser 只给候选、Sema 才下结论」的可观测证据。

**为什么不用更简单的做法**：不由 Parser 直接判「`S` 是声明」——
登记表的 type 位是**跨作用域并集**，`namespace N { struct S{}; } int main(){ S; }`
里的 S 在 N 里、全局不可见，此前直接判声明 ⇒ **静默放行非法程序**（clang rc=1）。

### 第 8 跳：限定名路径

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::lookupVisibleType`
（命名空间链段）+ `src/sema/name_registry.cpp:NameRegistry::classifyQualified`。

**做了什么**：`N::S` 按**完整限定名**查（`scope + "::" + name`），
沿命名空间链逐级剥外层。★ **不剥尾**是 B38 明确改写的方向：

- 旧行为：`N::B` 查不到就剥掉 `N::` 拿裸名 `B` 再查全局 ⇒ **默默绑到全局 B**；
- 新行为：查不到就报 `no template named 'B' in namespace 'N'`（与 clang 逐字）。

**为什么**：剥尾兜底是**静默算错**（方向性错），报错是响亮拒收。

### 第 9 跳：成员路径（旁路）

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::inferMember`
→ `findField` / `findMethodInClass`。

**做了什么**：`obj.x` / `p->x` **不走名字表**，走类布局表：
`findField` 三级（全限定名 → **自身字段优先** → 继承），
命中后回填 `resolvedFieldOffset` / `resolvedFieldSize`。

实测日志：

```text
  [member] Global.v → int    (offset=0, size=4)
  [member] N::Inner.w → int    (offset=0, size=4)
  [member] Holder.field → int    (offset=0, size=4)
```

★ 这里的「名字」只是**键**；机器要的是**偏移**。
这就是 B21 那条教训的落点：「名字是给人看的，位置才是机器要的」
（项目里第三次撞上：B10 / B13~B15 / B22）。

### 第 10 跳：名字的回填

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::inferVar` /
`SemanticAnalyzer::resolveType`。

**做了什么**：命中之后，把「名字」换算成机器要的形态：

| 名字种类 | 回填成什么 | 谁消费 |
|---|---|---|
| 局部变量 | 类型 + `stackOffset`（`stack@-8`） | CodeGen 的 `leaq -8(%rbp)` |
| 全局变量 | 类型 + 符号名 | CodeGen 的 `leaq sym(%rip)` |
| 实例字段 | 类型 + **偏移** + 大小 | CodeGen 的 `movl off(%rax)` |
| static 成员 | 类型 + **全局符号名**（`A::count`） | CodeGen 的 `.data` 段标签 |
| 类型名 | 具体类型（带布局/vtable） | 后续成员访问、模板实例化 |
| 函数名 | 返回类型 + mangled 符号 | CodeGen 的 `callq` |

★ 最后一列是本条线的落点：**符号表不是终点**，
它是「名字」变成「偏移 / 符号 / 类型」的中转站。

## §4 断链点

### 断链点 ①：`resolveType` 的 Class 分支（类型位的多路分派）

**位置**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::resolveType`

`resolveType` 的整体分派顺序是：**数组 → decltype → std::void_t 归约 → 类分支 → 指针/引用/const 递归**。
类分支内部的全部实现（按代码先后）：

| # | 分支 | 判据 | 落点 |
|---|---|---|---|
| 1 | 依赖限定名（`T::x`，限定者是模板形参） | `type->isNestedName() &&` 限定者是 TemplateParam | 原样保留，等替换阶段（`[sema:alias] {}::{} 限定者仍是模板形参 ⇒ 保持依赖`） |
| 2 | 嵌套名（`S::type`） | `type->isNestedName()` | `findMemberType(qual, name)`；查不到且在直接上下文 ⇒ SFINAE 软失败，否则硬报错 |
| 3 | 限定名模板 id（`N::B<Box>`） | 带实参表 + 限定者是模板 id | 按**完整限定名**查蓝图（B38 起不再剥尾） |
| 4 | 别名模板 id | `m_aliasTemplates.count(type->name)` | `expandAliasTemplate` 解糖 |
| 5 | 类模板 id | 带实参表 + `m_classTemplates.count(...)` | `getOrInstantiateClass` |
| 6 | 裸类模板名 / 裸别名模板名（`Box b;` 不带实参） | `m_classTemplates.count(type->name)` | 报 `'Box' is a class template; provide template arguments` |
| 7 | **普通类名** | `m_symbolTable.lookup(type->name)` 命中且 kind==Type | 返回注册版类型（带布局） |
| 8 | **兜底：可见性查找** | `lookupVisibleType(type->name)` | 类作用域别名 / 命名空间链 / 全局 |
| 9 | **未命中** | — | `error "unknown type name 'S'"` |

**本线走哪支**：全局类型走 7；命名空间类型与类作用域别名走 8（因为 7 的
符号表键是**裸名**，而命名空间成员按完整限定名 `N::S` 登记、
类内别名根本不进符号表）。

★ 这一支是本项目「**同一语义两条路径**」的高发区：`token.h:236` 有专门的警告注释。

### 断链点 ②：`lookupVisibleType` 的三级查找

**位置**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::lookupVisibleType`

全部实现：类作用域别名（level=1）→ 命名空间链逐级外层（level=2）→ 全局（level=3）
→ 未命中（level=0）。详见第 5 跳的表。

**本线走哪支**：`Alias` 走 ①；`N` 里的 `S` 走 ②。

★ 查找顺序本身就是语义：**类作用域先于命名空间**（类内别名遮蔽同名全局类型）。

### 断链点 ③：`NameRegistry::classify` 的三条查询路径

**位置**：`src/sema/name_registry.cpp:NameRegistry::classify`

全部实现：① 全局精确 → ② 限定名精确 → ③ 末段任意作用域 OR。

**本线走哪支**：Parser 侧对裸名走 ①（不中）→ ③（OR 命中）；
对 `N::S` 走 ②。

★ ③ 是**宽方向**（宁宽勿窄）：语句层/表达式层问不出本地名，
误收由 `TokenCheckpoint` 试探回退兜底。

### 断链点 ④：`inferVar` 的六步查找

**位置**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::inferVar`

全部实现（按代码先后）：

| # | 分支 | 判据 | 命中日志 |
|---|---|---|---|
| 1 | 限定名成员 `Q::v` | `name` 含 `::` ∧ 限定者是当前类/基类 | `[resolve] 'Q::v' → T (qualified member, offset=N 隐式 this)` |
| 1.5 | 限定名 static 成员 | 同上 ∧ 命中的是 static 成员 | `[resolve] 'A::count' → int (static member, 全局符号 A::count)` |
| 2 | **符号表** | `m_symbolTable.lookup(name)` | `[resolve] 'x' → T (kind=Variable, stack@-8)` |
| 3 | 当前类的实例字段 | `m_classTypes[当前类]->classLayout.findField(name)` | `[resolve] 'x' → T (class field, offset=N)` |
| 4 | 当前类 + 基类链的 static 成员 | `m_symbolTable.lookup(类 + "::" + name)` | 同 1.5 |
| 5 | 函数表 | `m_functionMap.find(name)` | `[resolve] 'f' → function` |
| 6 | 全未命中 | — | `error "Undefined variable 'x'"` |

**本线走哪支**：普通局部变量走 2；类内成员走 3/4。

★ 顺序有讲究：3（实例字段）在 4（static 成员）**之前**——
同名时实例字段隐藏 static 成员（[class.member.lookup]/3），反了会静默绑错（B14 同类）。

### 断链点 ⑤：登记表的 5 条收集规则

**位置**：`src/sema/name_registry.cpp:NameRegistry::collect`

全部实现：规则 1（template 头 → template 位）/ 规则 2（struct/class/enum → type 位）/
规则 3（typedef → type 位）/ 规则 4（`using X =` → type 位）/ 规则 5（namespace → namespace 位 + 推栈）。

**本线走哪支**：`struct Global { int v; };` 走规则 2；
`namespace N { struct Inner {...}; }` 走规则 5（先推 scope 栈）再规则 2。

★ 规则 2 有一条例外：**template 头后的名字不置 type 位**。
`Box;` 对类模板名保持非类型解释。

### 断链点 ⑥：`Scope::lookup` 链式查找 vs `Scope::lookupLocal`

**位置**：`src/sema/semantic_analyzer.h:Scope::lookup` / `Scope::lookupLocal`

全部实现：`lookup` 沿 `parent` 链向外；`lookupLocal` 只查本层。

**本线走哪支**：`lookup`（值名查找要能看见外层）。

★ 两个原语的分工是重声明检查（[basic.scope.scope]）与可见性的分界：
「本层已有同名」用 `define` 的 `emplace` 失败来判，
「外层同名」不拦截（内层遮蔽外层合法）。

## §5 分叉与旁路

### 主线上的可选分支

| 分支 | 触发 | 走到哪跳改道 | 实测 |
|---|---|---|---|
| 裸 `S;` 是声明 | 登记表 type 位命中 ∧ Sema 判定可见 | 第 7 跳的第一问命中 ⇒ 折叠为空 | `[sema:stmt] 'S' 可见的类型名 ⇒ 判为无声明符声明` |
| 裸 `S;` 是表达式 | 名字声明过但不是类型 | 第 7 跳的第二问命中 ⇒ 走表达式路径 | 局部变量遮蔽全局类 `S` |
| 两者皆否 | — | 第 7 跳报错 | `use of undeclared identifier 'S'` |
| 类型在命名空间里 | 裸名 + 命名空间链命中 | 第 5 跳的 ② | `[resolve] 'S' 在命名空间 'N' 内 ⇒ N::S` |
| 类型是类作用域别名 | 当前类 `typeAliases` 命中 | 第 5 跳的 ① | `[sema:alias] Alias 在类 'Outer' 作用域内 ⇒ int` |
| 名字是模板名 | 登记表 template 位命中 | 第 3 跳的 `parseType` 走模板 id 分支 | `Box<int> b;` |
| 名字是成员 | `obj.x` 形态 | 第 9 跳（旁路） | `[member] Global.v → int (offset=0, size=4)` |
| 名字是限定名 | `N::S` 形态 | 第 8 跳 | `[register] class 'N::Inner'` |

### 旁路清单（属于本线但值得另开线的分支）

- **模板实参推导里的名字查找**——`T::type` 的依赖限定名（第 1 跳分支 1），
  它的可见性要到替换期才判定（`substituteType` 的 Case 5.5）。
- **SFINAE 探测里的名字查找**——`void_t<typename T::type>` 这类惯用法在
  直接上下文里的失败与降级，见 `src/sema/sfinae.h`。
- **CodeGen 侧的名字消费**——`asmSymbol` 净化、全局符号拼装、
  vtable 条目的符号名，是「名字 → 汇编符号」的另一半。

### 已知边界（本线走完但没做的事）

- **登记表答不了值名**：`isValue` 位恒 false（占位），所以 Parser 无法问
  「`x` 是不是一个变量」——这正是 clang 的 `Sema::ClassifyName` 能答、
  本项目答不了的差别。Stage 3（边解析边 `ActOn*` 登记）要补的就是这一位。
- **`classifyQualified` 当前没有消费点**：B38 把 Sema 侧的登记表成员删了，
  限定名裁决改由 Sema 自己的注册表回答，这个入口等 Stage 3 接回。
- **嵌套类不支持**：`Outer::Inner` 的嵌套类型查找在 `resolveType` 里有分支，
  但 Parser 与 Sema 的前置支持不全，嵌套类不是一条走通的线。
- **命名空间变量在块作用域不可见**：`namespace N { int g; }` 里裸用 `g` 不可见
  （登记在 `N::g`，裸名 `g` 查不到）。
- **词法期零日志**：逐 token 的可观测性只能靠 `--dump-tokens` 整表转储。
- **`std::void_t` 不进登记表**：它只在类型位置按名识别，不走本消歧。

## §6 观测点

这条线上打了哪些日志键（从源码 grep 实际前缀，非推测）：

| 环节 | 日志键 | 出处 | 什么时候打 |
|---|---|---|---|
| 词法 | （**无前缀**） | `src/driver/main.cpp` | 只有 `N tokens generated` 一行；`--dump-tokens` 时另有整表转储 |
| 收集 | （**无前缀**） | `src/sema/name_registry.cpp` | 全文件零日志 |
| 解析（类型） | `[parse:type]` | `src/parse/parser.cpp:parseType` | `base = int` / `★ final type = int` / `✦ const prefix detected` / `✦ typename prefix` |
| 解析（语句歧义） | `[parse:stmt]` | `src/parse/parser.cpp:parseStatement` | `'S' 判为声明（type 位）` |
| 解析（模板 id） | `[parse]` | `src/parse/parser.cpp:parsePrimaryExpr` | `template-id: conv<1 explicit arg(s)>` |
| 解析（别名） | `[parse:alias]` | `src/parse/parser.cpp` | `using Alias = int` |
| 注册 | `[register]` | `src/sema/semantic_analyzer.cpp:processClassDecl` / `registerFunction` / `processGlobalVarDecl` | `class 'Global'` / `global variable 'N::g_count' : int` / `main() → int [mangled: main]` |
| 注册 | `[namespace]` | `src/sema/semantic_analyzer.cpp:processNamespaceDecl` | `enter namespace 'N'` |
| 可见性（类作用域） | `[sema:alias]` | `src/sema/semantic_analyzer.cpp:lookupVisibleType` | `Alias 在类 'Outer' 作用域内 ⇒ int` / `Holder::Alias = int`（登记侧） |
| 可见性（命名空间） | `[resolve]` | `src/sema/semantic_analyzer.cpp:lookupVisibleType`（level=2 分支） | `'S' 在命名空间 'N' 内 ⇒ N::S` |
| 可见性（语句裁决） | `[sema:stmt]` | `src/sema/semantic_analyzer.cpp:visit(EmptyStmt)` | `'S' 可见的类型名 ⇒ 判为无声明符声明` |
| 命中（值名） | `[resolve]` | `src/sema/semantic_analyzer.cpp:inferVar` | `'x' → int (kind=Variable, stack@-8)` / `'x' → function` |
| 命中（成员） | `[member]` | `src/sema/semantic_analyzer.cpp:inferMember` | `Global.v → int (offset=0, size=4)` |
| 入表 | `[symbol]` | `src/sema/semantic_analyzer.cpp`（`visit(VarDeclStmt)`） | `✚ x : int stack@-8 ← 加入符号表` |
| 报错 | `[ERROR]` | `src/sema/semantic_analyzer.cpp:error` | `unknown type name 'S'` / `Undefined variable 'x'` / `use of undeclared identifier 'S'` |

★ 观察：**「可见性裁决」这一层的日志分成两个前缀**——
`[sema:alias]`（类作用域别名）与 `[resolve]`（命名空间命中），
但两者其实打的是**同一个** `lookupVisibleType` 的不同 `level` 分支。
判据单点之后，**日志出口还没收口**（同一函数两个前缀），
是这条线上一个可记的小账。

## 词汇表

| 名词 | 一句话（本篇专用） |
|---|---|
| 名字种类 | 这个名字是 type / namespace / template 中的哪几类（登记表答的） |
| 名字可见性 | 这个名字**在此处**能不能被找到、且是什么（符号表答的） |
| 登记表 | `NameRegistry`：`(scope,name) → 位掩码` 的只读投影，构造期前向收集 |
| 位并集 | 同一 (scope,name) 的位做 OR 累加（`int S;` 与 `struct S;` 可并存） |
| 空值语义 | 查不到 = 四位全 false 的默认对象，**不是**「存在但位为空」 |
| 作用域链 | `Scope` 的 `parent` 链，`lookup` 从当前层向外逐层找 |
| level | `VisibleType` 的可见层级：1=类作用域、2=命名空间、3=全局、0=不可见 |
| 无声明符声明 | `A<int,int>;` / `S;` 这种只有类型没有声明符的声明（[dcl.dcl]） |
| 遮蔽 | 内层/自身优先；分「按名字隐藏」（[class.member.lookup]/3）与「按类型可行」（[overload.viable]）两种正交筛选 |
| 回填 | 把名字换算成机器要的形态：类型 / 偏移 / 符号名 / 栈槽 |
