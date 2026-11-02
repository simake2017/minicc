# 线：一条 .cpp 从磁盘到可执行文件

- 读者：想知道「`./minicc main.cpp` 之后到底发生了什么」的人；写编译器导读的人
- 审法：每跳的 `文件:函数` 都能在 `src/` 里 grep 到同名函数，且该函数的职责与本跳描述一致；跳与跳之间首尾相接、无跳空
- 名词版本：v1

> 仓库：mycompiler @ `main` / `c79656d`（源码锚点绑定此版本，换版本要复核）
> 入口：命令行 `./minicc main.cpp`（等价 `./minicc main.cpp -o out`）
> 结局：磁盘上出现一个非 PIE 的 x86-64 ELF 可执行文件，且能直接运行

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 首版：按 06-文档骨架规范 §5 的线文档骨架新建（线是"跑出来的调用链"，每跳带源码锚点） |

## §1 这条线是什么

一条**编译命令**从头跑到尾的全过程。

入口是用户敲下的 `./minicc main.cpp`——没有任何回调、没有插件、没有库调用，
就是 `main()` 里一串顺序执行的阶段调用。结局是磁盘上多出一个能 `./out` 直接跑起来的
可执行文件。

这条线竖穿了 8 个目录层（`driver` / `lex` / `sema` / `parse` / `ast` / `codegen` / `link`），
是本项目里**层次最全**的一条线；其余两条线（模板实参、名字查找）都是它的局部放大。

### 链路总图

```text
       ./minicc main.cpp
              │
              ▼
  ┌───────────────────────┐
  │ main() 命令行解析      │  driver/main.cpp:main
  │  -o / -I / -S / -E     │
  └───────────┬───────────┘
              ▼
  ┌───────────────────────┐
  │ Phase 0 预处理         │  lex/preprocessor.cpp:processFile → processText
  │  行拼接→剥注释→指令栈  │  （整条管线唯一的"文本级"阶段）
  │  →#include 并合→宏展开 │
  └───────────┬───────────┘
              ▼  展开后的纯文本
  ┌───────────────────────┐
  │ Phase 1 词法           │  lex/lexer.cpp:tokenizeAll → nextToken
  │  → Token 流（以 Eof 收尾）│
  └───────────┬───────────┘
              ▼
  ┌───────────────────────┐
  │ 名字登记表前向收集      │  sema/name_registry.cpp:collect
  │ （Parser/Sema 共用的   │
  │  只读"名字种类"投影）  │
  └───────────┬───────────┘
              ▼
  ┌───────────────────────┐
  │ Phase 2 语法           │  parse/parser.cpp:parseTranslationUnit
  │ → AST (TranslationUnit)│
  └───────────┬───────────┘
              ▼
  ┌───────────────────────────────────────────────┐
  │ Phase 3 语义（三遍扫描）                       │
  │  Pass 1 注册类/模板/全局变量  processDecl      │
  │  Pass 2 注册所有函数          registerFunction │
  │  Pass 3 分析函数体            analyzeFunctionBody│
  │  ⇒ 类型标注 + 类布局/vtable/RTTI + 符号表      │
  └───────────┬───────────────────────────────────┘
              ▼
  ┌───────────────────────┐
  │ Phase 4 模板实例化     │  sema/template_instantiation.cpp:instantiate
  │ （类模板；函数模板的    │  （函数模板在调用点实例化，见线②）
  │  实例化由调用点驱动）  │
  └───────────┬───────────┘
              ▼
  ┌───────────────────────┐
  │ Phase 5 代码生成       │  codegen/codegen.cpp:generate
  │ → x86-64 AT&T .s       │
  └───────────┬───────────┘
              ▼
  ┌───────────────────────┐
  │ Phase 6a 汇编          │  main.cpp 里的 std::system("as -o ...")
  │  .s → .o（借系统 as）  │
  └───────────┬───────────┘
              ▼
  ┌───────────────────────┐
  │ Phase 6b 链接          │  link/linker.cpp:MiniLinker::link
  │  符号决议+重定位回填   │
  │  +内置 _start/malloc   │
  │ → 非 PIE ELF 可执行文件 │
  └───────────┬───────────┘
              ▼
          ./out（可运行）
```

★ 星号处是**跨层接缝**（层与层的边界），§3 的「为什么不用更简单的做法」只在这里问。

## §2 走线五步法的结论（一句话一跳的表）

| # | 跳 | 落在哪个文件:函数 | 这一跳干了什么 |
|---|---|---|---|
| 1 | 命令行解析 | `src/driver/main.cpp:main` | 解析 argv，算出输出路径与四个 `--dump-*` 标志，构造 `PreprocessorOptions` |
| 2 | 预处理主文件入口 | `src/lex/preprocessor.cpp:Preprocessor::processFile` | 主文件路径 canonical 化后压 include 栈（防循环 include），读全文 |
| 3 | 翻译阶段 2~4 主循环 | `src/lex/preprocessor.cpp:Preprocessor::processText` | 行拼接 → 剥注释 → 逐行识别指令并维护条件栈 → 宏展开 |
| 4 | 预处理指令分派 | `src/lex/preprocessor.cpp:Preprocessor::handleDefine` / `handleInclude` / `handlePragma` / `expand` | 按指令词分派：登记宏 / 并合头文件 / 登记 `#pragma once` / 就地展开宏 |
| 5 | 词法扫描 | `src/lex/lexer.cpp:Lexer::tokenizeAll` → `nextToken` → `scanNumber` / `scanIdentifierOrKeyword` / `scanString` / `scanChar` / `scanOperator` | 把展开后的文本切成 Token 流，以 Eof 收尾 |
| 6 | 名字登记表收集 | `src/sema/name_registry.cpp:NameRegistry::collect` | 前向扫 Token 流，把「本 TU 里声明过哪些类型/命名空间/模板」记成三位并集 |
| 7 | 语法分析 | `src/parse/parser.cpp:Parser::parseTranslationUnit` → `parseDeclaration` | 递归下降把 Token 流变成 `TranslationUnit`（顶层声明列表） |
| 8 | 语义 Pass 1：注册 | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::analyze` → `processDecl` → `processTemplateDecl` / `processClassDecl` | 登记类（含字段/方法/继承）、模板蓝图、全局变量、枚举、命名空间 |
| 9 | 语义 Pass 2：注册函数 | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::registerFunction` | 定名（mangled name）、写 `m_functionMap` / `m_functions` / 符号表 |
| 10 | 语义 Pass 3：分析函数体 | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::analyzeFunctionBody` → `processStmt` → `inferType` | 进函数作用域、登记形参、逐语句定型与符号决议 |
| 11 | 模板实例化 | `src/sema/template_instantiation.cpp:TemplateInstantiator::instantiate` | 类模板：深拷贝蓝图 + 结构化替换 → 具体类（函数模板走线②） |
| 12 | 代码生成 | `src/codegen/codegen.cpp:CodeGen::generate` → `emitVTable` / `emitRTTI` / `emitFunction` | 发 vtable/RTTI 数据段、发全局变量数据段、逐函数发汇编 |
| 13 | 汇编 | `src/driver/main.cpp:main`（`std::system("as -o ...")`） | 借系统 `as` 把 .s 变成 .o（汇编器只做机械翻译） |
| 14 | 链接 | `src/link/linker.cpp:MiniLinker::link` → `readObject` / `layoutSections` / `injectRuntime` / `resolveSymbols` / `applyRelocations` / `writeExecutable` | 自研链接器：读 .o、布局段、注入 `_start`+malloc/free、决议符号、回填重定位、写 ELF |
| 15 | 运行 | —（生成的 ELF 的 `_start`） | `_start` 调 `main`，把返回值交给 `exit_group` 系统调用 |

### 走线五步法的执行记录

| 步 | 做了什么 | 证据 |
|---|---|---|
| 1 跑通 | 真的跑了一遍 `./minicc /tmp/linedemo/main.cpp -o /tmp/linedemo/out`，并执行 `/tmp/linedemo/out` | 退出码 14（= `twice(7)` = 14），与 clang++-18 对拍一致 |
| 2 画图 | 上面的链路总图（8 个跨层接缝用 ★ 标出） | 本节 |
| 3 逐跳 | §2 表 + §3 详解，每跳有 `文件:函数` 锚点 | §3 |
| 4 找动机 | 只在 ★ 处问「为什么不用更简单的做法」 | §3 每跳末尾的「为什么」 |
| 5 写疑问 | 见 §5「分叉与旁路」末尾的边界清单 | §5 |

## §3 逐跳详解

### 第 1 跳：命令行解析

**源码锚点**：`src/driver/main.cpp:main` —— 编译器驱动，解析命令行并依次编排 7 个阶段。

**做了什么**：把 `argv[1]` 当输入文件；`for (int i = 2; i < argc; i++)` 循环里认
`-o`（吃掉紧随其后的值）、`-I`（可重复，顺序即搜索优先级）、`-E`、`-S`、
`--dump-tokens` / `--dump-ast` / `--dump-hierarchy` / `--dump-layout`。
输出路径未指定时按 `std::filesystem::path(inputFile).replace_extension(...)` 算出
（`-S` 出 `.s`，否则去扩展名出可执行文件）。

**为什么不用更简单的做法**：不用 `getopt`——参数只有 8 种且 `-I` 需要保序，
手写循环比引入依赖更直白；`-I` 保序是因为预处理器的头文件搜索顺序就是
「`-I` 出现顺序」（`Preprocessor::resolveInclude` 里那个 `for (auto& d : m_opts.includePaths)`）。

### 第 2 跳：预处理主文件入口

**源码锚点**：`src/lex/preprocessor.cpp:Preprocessor::processFile` —— 预处理总入口，
主文件与被 `#include` 的文件走同一条路。

**做了什么**：`fs::weakly_canonical(path)` 把主文件路径归一后压进 `m_includeStack`
（主文件自身也受循环检测保护：`a.cpp` include `b.cpp`、`b.cpp` 又 include `a.cpp`
会在这里被 `handleInclude` 发现并报错，而不是无限递归）；然后
`processText(readFileContents(path), path)` 干完全部翻译阶段 2~4。

**为什么不用更简单的做法**：不用 `std::ifstream` 逐行喂进来——`#include` 需要
把头文件全文**插到当前行位置**，全文进内存才能拼。

### 第 3 跳：翻译阶段 2~4 主循环

**源码锚点**：`src/lex/preprocessor.cpp:Preprocessor::processText` —— 本预处理器的心脏，
对一段源文本执行完整的翻译阶段 2~4。

**做了什么**：按 [lex.phases] 的顺序做四件事：

| 阶段 | 做什么 | 代码位置 |
|---|---|---|
| 阶段 2 行拼接 | 删掉「`\` + 换行」，全文一次性扫 | `spliced` 那个 for 循环 |
| 阶段 3 剥注释 | `stripComment(line, inBlockComment)`，字符串/字符字面量感知 | 逐行调用 |
| 阶段 3 指令识别 | `trim` 后以 `#` 开头即指令；`#ifdef`/`#ifndef`/`#if`/`#elif`/`#else`/`#endif` 维护 `condStack` | 指令分派块 |
| 阶段 4 宏展开 | 活跃分支里的普通行走 `expand(clean, {}, fileName, lineNo)` | 循环末尾 |

条件栈每个元素 `Cond` 有四个字段：`active`（本层是否活跃）、`takenBranch`
（本层是否已有分支取真，`#elif`/`#else` 互斥用）、`parentActive`（进入本层时外层活跃性的快照）、
`sawElse`（拦截重复 `#else`）。★ `parentActive` 是「死分支里的真条件救不活自己」
这条规则的载体：外层不活跃时 `#if` 的表达式**根本不求值**（`if (parent) { ... }` 那道闸），
否则死分支里引用未定义宏会误报。

**为什么不用更简单的做法**：不把预处理做成 clang 那样的 token 级（clang 在
`clang/lib/Lex/PPDirectives.cpp:Preprocessor::HandleDirective` 层面处理指令），
而是做成**文本级**——本项目的定位是「可讲解、可观测优先」，文本级让每一行
源码在日志里都能原样看到（`[pp] file:line [src-line] <SRC>...</SRC>`）。

### 第 4 跳：预处理指令分派

**源码锚点**：`src/lex/preprocessor.cpp:Preprocessor::handleDefine` / `handleInclude` /
`handlePragma` / `expand` —— 四条指令各自的处理函数。

**做了什么**：

| 指令 | 落点 | 干什么 |
|---|---|---|
| `#define` | `handleDefine` | 读宏名；`'('` 紧跟宏名 ⇒ 函数宏并逐个读形参；宏体读到行尾 |
| `#undef` | `handleUndef` | 从 `m_macros` 移除（名字本来不存在也不报错，与 [cpp.undef] 一致） |
| `#include` | `handleInclude` | 引号形式先搜当前文件目录、再搜 `-I`；尖括号形式先搜 `-I`、再兜底 `/usr/include` |
| `#pragma` | `handlePragma` | 只实现 `once`（按 canonical 路径登记）；其余警告后忽略 |
| 宏展开 | `expand` | 递归重扫描 + 「涂蓝」（hide 集）防自引用死循环 |

`handleInclude` 里有三道闸，顺序不可换：① `#pragma once` 已登记 ⇒ 返回空串；
② canonical 路径在 include 栈上 ⇒ 报 `circular #include detected`；
③ 压栈 → 递归 `processText` → 弹栈。

**为什么不用更简单的做法**：`#include` 的搜索路径不用 clang 那套
`clang/lib/Basic/HeaderSearch.cpp:HeaderSearch::LookupFile`（框架化、可配置），
取最常见约定（当前目录 → `-I` → `/usr/include`）并把**试过的每条路径**都列进
错误消息——教学价值大于灵活性。

### 第 5 跳：词法扫描

**源码锚点**：`src/lex/lexer.cpp:Lexer::tokenizeAll` → `Lexer::nextToken` →
`scanNumber` / `scanIdentifierOrKeyword` / `scanString` / `scanChar` / `scanOperator`。

**做了什么**：`tokenizeAll` 反复调 `nextToken` 直到（含）Eof。`nextToken` 是调度器，
**只看首字符**决定分支：数字 → `scanNumber`；字母/下划线 → `scanIdentifierOrKeyword`；
`"` → `scanString`；`'` → `scanChar`；其余 → `scanOperator`。

三处值得单说的设计：

- `scanIdentifierOrKeyword` 先按标识符规则扫完整单词，**再**查 `kKeywordMap` 决定
  是关键字还是 Identifier——新增关键字只改一张表，状态机不动。
- `scanNumber` 只负责「最长匹配切一刀」，**不解读后缀与进制**：`4L` 的形态（long）
  是语义信息，由 `Parser::parseIntLiteral` 解读。与 clang 的
  「Lexer 产出原始串 → Sema::ActOnNumericConstant 配 TargetInfo 定类型」分工一致。
- `scanOperator` 先试双字符（`->` `::` `<=` …），组不成已知双字符运算符才退回单字符
  ——这就是 [lex.pptoken] 的最长匹配注记。

**为什么不用更简单的做法**：不用正则切词——`//` 与 `/*` 的区别、
字符串字面量里的 `//` 不是注释、跨行块注释，都需要逐字符状态，
正则表达式的转义地狱不值得。

### 第 6 跳：名字登记表收集

**源码锚点**：`src/sema/name_registry.cpp:NameRegistry::collect` —— 构造期一次性前向收集。

**做了什么**：把整条 Token 流扫一遍，记下三类名字的「种类」：
`BitType`（struct/class/enum/typedef/using 别名）、`BitNamespace`（`namespace X {`）、
`BitTemplate`（template 头家族：类模板/别名模板/函数模板/推导指引/成员模板）。
键是 `(scope, name)`，scope 空串 = 全局，嵌套命名空间拼 `N1::N2`；
同名跨类的位做 **OR 累加**（位并集语义，不是单选枚举）。

★ 这张表**不做可见性裁决**，只回答「本 TU 里某处声明过这个名字吗、它是哪一类」。
可见性的裁决权在 Sema（见线③）。

**为什么不用更简单的做法**：不边解析边登记——名字可以先使用后声明，
前向收集保证「使用点在声明之前」时表里也已经有它（clang 查的是活符号表，
天然没有这个问题；本项目因「Parser 不持 Sema 句柄」的红线只能退到构造期投影）。

### 第 7 跳：语法分析

**源码锚点**：`src/parse/parser.cpp:Parser::parseTranslationUnit` → `Parser::parseDeclaration`。

**做了什么**：`parseTranslationUnit` 就是一个 `while (!isAtEnd())` 循环，
反复调 `parseDeclaration`。`parseDeclaration` 是 LL(1) 分派树：
`template` → `parseTemplateDecl`；`class`/`struct` → `parseClassDecl`；
`enum` → `parseEnumDecl`；`namespace` → `parseNamespaceDecl`；
`using`/`typedef` → `parseTypeAliasDecl`；「标识符 + `(`」→ `looksLikeDeductionGuide`
→ `parseDeductionGuide`；剩下「函数 vs 全局变量」靠 **TokenCheckpoint 试探**分岔。

★ TokenCheckpoint 是本项目 B32 批次落的机制：构造时快照 `m_pos` + `m_stopAtGreater`
+ 日志进缓冲栈；`commit()` 保留进度并冲刷缓冲；析构时未 commit 则**全维度回卷 + 缓冲丢弃**
——试探日志在机制上不可能泄漏成幽灵行。四个试探点（模板-id 消歧、语句层声明、
顶层声明、类体成员判定）全部走它。

**为什么不用更简单的做法**：不用 yacc/bison——递归下降让每个非终结符对应一个函数，
和 clang 的 `clang/lib/Parse/Parser.cpp` 同一手法，且教学上可逐函数读。

### 第 8 跳：语义 Pass 1：注册

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::analyze` →
`SemanticAnalyzer::processDecl` → `processTemplateDecl` / `processClassDecl`。

**做了什么**：`analyze` 先 `registerBuiltins()`（内建 libc 原型 + std 垫片），
再 `for (auto& decl : unit.declarations) processDecl(decl)` 走 Pass 1。
`processDecl` 是一次 `switch (decl->kind)` 标签分派，**不查函数体**。

`processTemplateDecl` 按蓝图种类分四张表：类模板主模板进 `m_classTemplates`，
偏特化进 `m_partialSpecs`，全特化进 `m_explicitSpecs`，函数模板进
`m_functionTemplateCandidates`（重载候选集），别名模板进 `m_aliasTemplates`。

**为什么不用更简单的做法**：函数体不在 Pass 1 分析——模板体里的 `T` 是**依赖类型**，
要等实参到位才能做类型检查，这就是两阶段名称查找的第一阶段。

### 第 9 跳：语义 Pass 2：注册函数

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::registerFunction`。

**做了什么**：三处登记一次做完——`m_functionMap`（调用点按裸名命中）、
`m_functions`（CodeGen 按序遍历它发射汇编）、符号表（`kind=Function`）。
同时定 `mangledName`：自由函数就是名字本身；成员方法走
`memberMethodSymbolName(ownerClass, name, parameters, sameNameCount, needsTypeTag)`
——**这同一个函数还被 vtable 三个落点共用**（判据单点，见 BUGS.md B20/B22）。

遍历用 `forEachFunctionDecl(unit.declarations, ...)` 而不是扁平循环——
它会**递归进命名空间**（B26 的教训：扁平遍历漏掉命名空间里的类/函数，
症状是 Sema 全对但 CodeGen 没收到方法 ⇒ 链接期 `undefined reference to 'N__S_N__S'`）。

**为什么不用更简单的做法**：不在 Pass 1 顺手注册函数——那样会在
「同名多参要加后缀」的规则上出现两处不一致（BUGS.md B10 的根因）。

### 第 10 跳：语义 Pass 3：分析函数体

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::analyzeFunctionBody`
→ `SemanticAnalyzer::processStmt` → `SemanticAnalyzer::inferType`。

**做了什么**：`analyzeFunctionBody` 进函数作用域（`m_symbolTable.enterScope(funcLabel)`）、
登记形参（`[param] x : int stack@-8`）、成员函数还登记隐式 `this`；
然后逐语句 `processStmt`。`processStmt` 是一次**虚表跳转**（`stmt->accept(*this)`），
落到 `visit(VarDeclStmt)` / `visit(AssignStmt)` / … 等 14 个 visit 重载之一。

`inferType` 又是一次 `switch (expr->kind)` 标签分派，落到 `inferVar` / `inferCall` /
`inferMember` / `inferBinary` / … 之一。

**为什么不用更简单的做法**：不把语句与表达式统一成一套访问者——语句处理链要的是
「引用」，表达式推导要的是「返回值」，判据写在 `src/sema/semantic_analyzer.h` 的注释里：
**handler 只要引用 → 访问者；还要 shared_ptr 所有权或返回值 → 标签分派**。

### 第 11 跳：模板实例化

**源码锚点**：`src/sema/template_instantiation.cpp:TemplateInstantiator::instantiate`。

**做了什么**：类模板的按需实例化。深拷贝蓝图 → 按 `templateParams` 逐位建替换表
（类型位进 type 槽、NTTP 值位进 value 槽，**分派依据是每一位形参自己的 kind**）
→ 结构化替换 → 生成实例类（名字 `Box_int`、mangled `_Z3BoxIiE`）。

★ 但要分清：**函数模板不在这里实例化**。`main.cpp` 的阶段 4 对函数模板蓝图只打印
「blueprint stored; instantiation is call-site driven」，真正的实例化发生在
调用点（语义 Pass 3 里的 `getOrInstantiateFunction`）——那是线②。

**为什么不用更简单的做法**：不在解析期就展开——模板是「蓝图」，要等实参。

### 第 12 跳：代码生成

**源码锚点**：`src/codegen/codegen.cpp:CodeGen::generate` → `emitVTable` / `emitRTTI` /
`emitFunction`。

**做了什么**：`generate` 按顺序发四段东西：

1. **数据段**：每个有多态的类发 vtable（`_ZTV3Dog`）与 RTTI（`_ZTI3Dog`）；
   非多态基类的 RTTI 单独补发（多态派生类的 typeinfo 会引用基类的 `_ZTI`，
   漏发就是链接期 `undefined reference to '_ZTINonPolyBase'`）。
2. **全局变量数据段**：`.globl` + `.align 8` + `.quad <值>`。
3. **代码段**：遍历 `functions`，**只发 `func->body` 非空的**——
   `registerBuiltins` 登记的 libc 原型 `body == nullptr`，所以 .s 里只有 `callq malloc`，
   没有 malloc 的函数体。
4. **只读段**：字符串字面量。

`emitFunction` 里符号名取 `asmSymbol(func->mangledName.empty() ? func->name : func->mangledName)`
——`asmSymbol` 把 `::` 之类净化成合法汇编标签，**标签发射端与使用端必须走同一个函数**
（否则 `Math::scale` 一边发 `Math__scale`、一边引用 `Math::scale` ⇒ 链接期找不到符号）。

**为什么不用更简单的做法**：不直接生成 ELF —— 借用系统 `as` 做机械翻译，
把自研精力留给「语义到汇编」这一段（见第 13 跳的取舍说明）。

### 第 13 跳：汇编

**源码锚点**：`src/driver/main.cpp:main`（`std::system(("as -o " + objPath + " " + asmPath).c_str())`）。

**做了什么**：`.s` 写到系统临时目录（`/tmp/minicc_<stem>.s`，不污染用户目录），
调系统 `as` 产 `/tmp/minicc_<stem>.o`；`as` 退出码非 0 ⇒ 报「汇编失败」并以 1 退出。

**为什么不用更简单的做法**：自研汇编器 —— 汇编器只做「助记符 → 机器码」的机械翻译，
不产生任何语言语义；自研它对「学编译器理论」没有增量，却要付出几百行。
（链接器不同：链接器要处理符号决议与重定位回填，是编译器后端理论的一部分，故自研。）

### 第 14 跳：链接

**源码锚点**：`src/link/linker.cpp:MiniLinker::link` → `readObject` / `layoutSections` /
`injectRuntime` / `resolveSymbols` / `applyRelocations` / `writeExecutable`。

**做了什么**：六步流水线。

| 步 | 函数 | 干什么 |
|---|---|---|
| ① | `readObject` | 校验 ELF 魔数/`ET_REL`/`EM_X86_64`，读节头表、符号表、重定位表 |
| ② | `layoutSections` | 合并 `.text`/`.data`/`.bss`/`.rodata`，算出各段虚拟地址 |
| ③ | `injectRuntime` | 注入 `_start`（调 main → `exit_group`）+ 64KB bump 分配的 malloc + nop 的 free |
| ④ | `resolveSymbols` | 全局符号决议，`main` 地址在此确定 |
| ⑤ | `applyRelocations` | 重定位回填（PLT32 等） |
| ⑥ | `writeExecutable` | 写非 PIE ELF，写 RX 段与 RW 段 |

★ `injectRuntime` 的槽位划分写在代码里：`0~15` 是 `_start`、`16~47` 是 malloc、
`48~` 是 free；`_start` 的字节码是 `call main` / `mov %eax,%edi` / `mov $231,%eax` /
`syscall`（231 = `exit_group`）。

**为什么不用更简单的做法**：不用系统 `ld` —— 那会让「链接」这一环变成黑盒；
自研链接器让「符号决议 + 重定位回填」变成可讲解、可观测的一课。

### 第 15 跳：运行

**源码锚点**：生成 ELF 的 `_start`（`MiniLinker::injectRuntime` 里手写的 14 字节）。

**做了什么**：`_start` 调 `main`，把 `main` 的返回值（eax）交给 `exit_group` 系统调用。
用户看到的结局是 `./out` 打印结果并返回退出码。

**为什么不用更简单的做法**：不链 libc —— 自带 `_start` + malloc/free，
可执行文件不依赖任何系统 C 运行时（`docs/learn/15` 记了这条取舍）。

## §4 断链点

「断链点」= 调用链上**一处调用有多支实现、必须选一支**的地方。按红线，
每处都必须列出全部实现，并说清本线选了哪支、为什么。

### 断链点 ①：`processDecl` 的声明分派（`switch (decl->kind)`）

**位置**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::processDecl`

全部实现（`src/sema/semantic_analyzer.cpp` 的 switch 全支）：

| NodeKind | 落点 | 本线走这支？ |
|---|---|---|
| `Class` | `processClassDecl` | 是（示例源码含 `struct` 时） |
| `Template` | `processTemplateDecl` | 是（示例源码含 `template` 时） |
| `GlobalVar` | `processGlobalVarDecl` | 是（含全局变量时） |
| `Enum` | `processEnumDecl` | 视源码 |
| `TypeAlias` | `processTypeAliasDecl` | 视源码 |
| `Namespace` | `processNamespaceDecl` | 视源码 |
| `DeductionGuide` | `registerDeductionGuide` | 视源码 |
| `default` | 空 | 是——**函数声明不在此处理**（由 `analyze` 的 Pass 2/3 各自处理） |

**本线走哪支**：示例源码 `template <typename T> T twice(T x) {...}` + `int main() {...}`
⇒ `Template` 支进 `processTemplateDecl`，`main` 走 `default`（不在 Pass 1 出现）。

**为什么**：分派依据是节点自带的 `kind` 标签，不是名字字符串——
零 RTTI 的向下转换 = `node->kind == NodeKind::X` + `static_pointer_cast`，
与 LLVM `cast<>` 用 `classof()` 查 `getKind()` 同理。

### 断链点 ②：AST 节点分派的两套手法（访问者 vs 标签）

**位置**：`src/sema/semantic_analyzer.h` 的类注释 + `src/include/ast_visitor.h`

全部实现：

| 分派手法 | 落点 | 用在哪 |
|---|---|---|
| **访问者（虚分派）** | `include/ast_visitor.h` 的 32 个 `visit` 重载 + `ASTNode::accept` 纯虚挂钩 | 语句处理链（`processStmt` → `stmt->accept(*this)`）、CodeGen 的语句/表达式发射 |
| **标签分派（switch）** | `processDecl` 的 `switch(decl->kind)`、`inferType` 的 `switch(expr->kind)`、`cloneExpr` / `cloneStmt` / `estimateBlockSize` | 声明链、类型推导、克隆、帧大小估算 |

**本线走哪支**：`main` 的函数体是**语句链** ⇒ 走访问者（`processStmt` 虚表跳转）；
`int b = twice(a);` 的初始化式是**表达式** ⇒ `inferType` 走标签分派（要返回值）。

**为什么**：判据写死在 `src/sema/semantic_analyzer.h` 的注释里——
**handler 只要引用 → 访问者；还要 shared_ptr 所有权或返回值 → 标签分派**。
★ `accept` 必须类内 inline 定义，否则成为 Itanium ABI 的**键函数**、vtable 只在定义它的
TU 发射 ⇒ 链接期满屏 `undefined reference to vtable`（docs/learn/29 的实测教训）。

### 断链点 ③：阶段 4 的模板蓝图五分类

**位置**：`src/driver/main.cpp:main`（阶段 4 的 `for (auto& tmpl : templates)` 循环内）

全部实现：

| 蓝图种类 | 判定 | 阶段 4 做什么 |
|---|---|---|
| 别名模板 | `tmpl->isAliasTemplate()` | 打印「no instantiation — desugars at use sites」后 `continue` |
| 推导指引 | `tmpl->isDeductionGuide()` | 打印「CTAD-only rule」后 `continue` |
| 函数模板 | `tmpl->isFunctionTemplate()` | 打印「call-site driven」后 `continue` |
| 特化（偏/全） | `tmpl->isSpecialization()` | 打印「matched on demand at use sites」后 `continue` |
| 主模板（类模板） | 以上皆否 | **真的调 `instantiator.instantiate(...)`** |

**本线走哪支**：本线示例是**函数模板** ⇒ 走第三支，`continue`。
★ 这四道 `continue` 不是可有可无的防御——下面每个分支都假设
`tmpl->classTemplate != nullptr`，别名模板的 `classTemplate` 是空的，
不拦下会直接空指针崩。

**为什么**：类模板的「演示性实例化」在驱动里做（拿 int/double/int*/int&/int&&/const int&
六种实参各实例化一遍，演示替换机制），而函数模板的实例化**必须**由调用点的实参驱动
（线②），否则算不出 `T`。

### 断链点 ④：阶段 5 的「函数发不发」

**位置**：`src/codegen/codegen.cpp:CodeGen::generate`（`for (auto& func : functions)` 循环）

全部实现：

| 条件 | 结果 |
|---|---|
| `func->body` 非空 | `emitFunction(func)` —— 发完整函数体 |
| `func->body` 为空 | **跳过**，.s 里只剩调用点的 `callq` |

**本线走哪支**：`main` 与 `twice<int>` 实例体都有 body ⇒ 发；`malloc`/`free`/`memcpy`/
`realloc` 与 std 垫片由 `registerBuiltins` 登记、`body == nullptr` ⇒ 跳过。

**为什么**：内建原型只有「语义外壳」（名字 + 形参表 + 返回类型），
没有函数体可发；它们由链接器绑定到系统 libc（`[link] 符号决议` 阶段处理）。

### 断链点 ⑤：链接这一步——借系统 `as` 还是全自研？

**位置**：`src/driver/main.cpp:main`（阶段 6）

全部实现：

| 方案 | 本项目 |
|---|---|
| 借系统 `as` 做汇编 + 自研链接器 | **采用**（`std::system("as ...")` + `MiniLinker::link`） |
| 自研汇编器 + 自研链接器 | 未采用 |
| 借系统 `as` + 借系统 `ld` | 未采用（会让「链接」变黑盒） |

**本线走哪支**：第一支。

**为什么**：汇编是机械翻译，对「学编译器理论」无增量；链接涉及符号决议与
重定位回填，是后端理论的一部分，值得自研（`docs/learn/15`）。

## §5 分叉与旁路

### 主线上的可选分支

| 分支 | 触发 | 走到哪跳返回 | 实测 |
|---|---|---|---|
| `-E`（只预处理） | `main.cpp` 的 `emitPreprocessed` | 第 3 跳之后立即 `return 0` | `./minicc tests/pp/main.cpp -I tests/pp -E` |
| `-S`（只出汇编） | `emitAsmOnly` | 第 12 跳之后、第 13 跳之前 `return 0` | `./minicc main.cpp -S` |
| `--dump-tokens` | `dump.tokens` | 第 5 跳之后打印 Token 流 | `./minicc main.cpp --dump-tokens` |
| `--dump-ast` | `dump.ast` | 第 7 跳之后打印 AST 树 | `./minicc main.cpp --dump-ast` |
| `--dump-hierarchy` / `--dump-layout` | `dump.hierarchy` / `dump.layout` | 第 10 跳之后打印类层次/布局 | `./minicc main.cpp --dump-hierarchy` |
| `#include` | 预处理指令 | 第 4 跳内递归 `processText` | `tests/pp/test_pp_01_include.cpp` |
| `#if` / `#ifdef` | 预处理指令 | 第 3 跳的条件栈 | `tests/pp/` 下的条件编译用例 |
| 宏展开 | `#define` + 使用点 | 第 4 跳的 `expand` | `tests/pp/` 下的宏用例 |

### 旁路清单（属于本线但值得另开线的分支）

- **模板实参从调用点到实例化**（`getOrInstantiateFunction` 整条）——已单列为线②。
- **一次名字查找从 Token 到符号表**（`inferVar` / `resolveType` 的可见性裁决）——已单列为线③。
- **类模板特化择优**（`selectClassTemplate` 的三路择优 + `[temp.class.order]` dominance 循环）
  ——线② 只覆盖函数模板；类模板那一支另开线。
- **虚函数运行期派发**（vtable 三部曲 `movq (%rdi),%rax` → `movq idx*8(%rax),%rax` → `callq *%rax`）
  ——本线只发 vtable 数据段，运行期派发是另一个主题。

### 已知边界（本线走完但没做的事）

- `as` 之后没有 `.o` 的调试信息，链接产物不带 DWARF。
- 链接器只支持 ET_REL x86-64、单文件输入，无静态库、无动态链接。
- `-E` 模式输出的是**文本级**展开结果，与 clang 的 token 级展开不完全等价（宏内的 `#` 特殊处理未做）。

## §6 观测点

这条线上打了哪些日志键（从源码 grep 实际前缀，非推测）：

| 阶段 | 日志键 | 出处 | 什么时候打 |
|---|---|---|---|
| 驱动 | `Phase N: ...` 横幅 | `src/driver/main.cpp:printPhase` | 每阶段开头 |
| 驱动 | `N file(s) included, N macro(s) defined` | `src/driver/main.cpp:main` | 阶段 0 结束 |
| 驱动 | `N tokens generated` | `src/driver/main.cpp:main` | 阶段 1 结束 |
| 驱动 | `N top-level declarations parsed` | `src/driver/main.cpp:main` | 阶段 2 结束 |
| 驱动 | `N classes analyzed` / `N functions analyzed` | `src/driver/main.cpp:main` | 阶段 3 结束 |
| 阶段 0 | `[pp]` | `src/lex/preprocessor.cpp` | **每一行**（`[src-line] <SRC>…</SRC>`）、每条指令、每次宏展开的 enter/return |
| 阶段 0 | `[pp] … #define X = …` / `#include X → path` / `#pragma once → registered` | 同上 | 对应指令发生时 |
| 阶段 1 | **（无）** | — | 词法期**不打任何日志**；只有 `--dump-tokens` 才在驱动里打印 Token 流 |
| 阶段 2 | `[parse:template]` | `src/parse/parser.cpp:parseTemplateDecl` | 模板声明解析 |
| 阶段 2 | `[parse:type]` | `src/parse/parser.cpp:parseType` | 类型解析（含 `base =` / `final type =` / `template-id` / `nested name`） |
| 阶段 2 | `[parse:targ]` | `src/parse/parser.cpp:parseTemplateArgumentList` | 模板实参表 |
| 阶段 2 | `[parse:class]` / `[parse:fct]` / `[parse:member]` / `[parse:alias]` / `[parse:guide]` / `[parse:param]` / `[parse:new]` / `[parse:literal]` / `[parse:stmt]` | `src/parse/parser.cpp` 各解析函数 | 对应语法构造 |
| 阶段 3 | `[builtin]` | `src/sema/semantic_analyzer.cpp:registerBuiltins` | 内建原型与 std 垫片登记 |
| 阶段 3 | `[register]` | `src/sema/semantic_analyzer.cpp:registerFunction` / `processClassDecl` / `processTemplateDecl` | 每个声明注册时 |
| 阶段 3 | `[param]` | `src/sema/semantic_analyzer.cpp:analyzeFunctionBody` | 形参与 `this` 登记 |
| 阶段 3 | `[symbol]` | `src/sema/semantic_analyzer.cpp:visit(VarDeclStmt)` | 局部变量入符号表 |
| 阶段 3 | `[var decl]` | 同上 | 变量声明 + 初始化式类型推断 |
| 阶段 3 | `[infer]` | `src/sema/semantic_analyzer.cpp:inferType` 各分支 | 字面量/二元/一元/条件/逗号 的类型 |
| 阶段 3 | `[resolve]` | `src/sema/semantic_analyzer.cpp:inferVar` / `resolveType` | 名字解析命中（含 `kind=` / `stack@` / `offset=`） |
| 阶段 3 | `[call]` | `src/sema/semantic_analyzer.cpp:inferCall` | 调用点决议 |
| 阶段 3 | `[member]` | `src/sema/semantic_analyzer.cpp:inferMember` | 成员访问 |
| 阶段 3 | `[return]` | `src/sema/semantic_analyzer.cpp:visit(ReturnStmt)` | 返回语句类型检查 |
| 阶段 3 | `[const]` / `[fold]` / `[auto]` / `[conv]` / `[poly]` | `src/sema/semantic_analyzer.cpp` / `src/ast/constant_folder.cpp` | 常量表登记 / 折叠 / auto 推导 / 隐式转换 |
| 阶段 3 | `[overload]` | `src/sema/semantic_analyzer.cpp:pickBestByArgs` / `isAtLeastAsSpecialized` | 重载择优、偏序 |
| 阶段 3 | `[namespace]` | `src/sema/semantic_analyzer.cpp:processNamespaceDecl` | 进入/退出命名空间 |
| 阶段 3 | `[sema:targ]` / `[sema:alias]` / `[sema:stmt]` / `[sema]` | `src/sema/semantic_analyzer.cpp` | 模板实参校验 / 别名解糖 / 裸名声明裁决 / 依赖保持 |
| 阶段 3 | `[deduction]` / `[deduce:alias]` | `src/sema/template_deduction.cpp` | 模板实参推导（逐对 trace） |
| 阶段 3 | `[instantiate]` / `[instantiate:class]` / `[instantiate:name]` | `src/sema/template_instantiation.cpp` / `src/sema/semantic_analyzer.cpp` | 实例化与缓存命中 |
| 阶段 3 | `[subst]` / `[subst:map]` / `[clone:method]` | `src/sema/template_instantiation.cpp` | 替换表与克隆 |
| 阶段 3 | `[spec:select]` | `src/sema/semantic_analyzer.cpp:selectClassTemplate` | 类模板特化择优 |
| 阶段 3 | `[sfinae]` | `src/sema/sfinae.cpp`（全项目唯一出 `[sfinae]` 的文件） | 替换失败信号、候选移出、void_t 探测 |
| 阶段 3 | `[decltype]` / `[void_t]` / `[declval]` | `src/sema/semantic_analyzer.cpp` | decltype 求值 / void_t 归约 / declval 内建 |
| 阶段 5 | **（无 `[cg:*]` 前缀）** | — | 代码生成**不打日志**；可观测手段是 `--dump-*` 与生成的 .s 本身 |
| 阶段 6 | `[link]` | `src/link/linker.cpp`（6 行：读入/段布局/堆 arena/注入运行时/符号决议/重定位回填/写出 ELF） | 链接各步 |
| 阶段 6 | `[as] <in> → <out>` | `src/driver/main.cpp:main` | 调用系统汇编器前 |

★ 一条重要的观测事实：**阶段 1（词法）与阶段 5（代码生成）没有日志前缀**。
词法期靠 `--dump-tokens`，代码生成期靠看 .s 文件本身。
所以「日志即契约」这条纪律在这两段是断的——排查这两段要换手段。

## 词汇表

| 名词 | 一句话（本篇专用） |
|---|---|
| 翻译阶段 | [lex.phases] 的 8 个阶段；本项目只实现阶段 2~4（预处理），之后直接进词法 |
| 蓝图 | 模板声明的 AST 半成品，等实参到位才变成具体类型/函数 |
| 两阶段查找 | 模板体里的非依赖名在第一阶段查，依赖名推迟到实例化时查 |
| `.s` / `.o` | 汇编文本 / 可重定位目标文件（ELF64 ET_REL） |
| 非 PIE | 可执行文件的绝对地址在链接期固定，不经动态重定位 |
| bump 分配 | 只前进不回收的堆分配器（自带的 malloc 用的就是它） |
