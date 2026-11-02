# 线：一条模板实参从调用点到实例化

- 读者：想知道「`f(7)` 这个调用点是怎么变成 `f<int>` 的」的人；想读懂 [temp.deduct] 与 [temp.inst] 落地的人
- 审法：每跳的 `文件:函数` 都能在 `src/` 里 grep 到；S2/S3/S4/S5/S6 五个阶段标签在日志里都能对上
- 名词版本：v1

> 仓库：mycompiler @ `main` / `c79656d`（源码锚点绑定此版本，换版本要复核）
> 入口：源码里的一个函数模板调用，例如 `int b = twice(a);`（`twice` 是 `template <typename T> T twice(T x)`）
> 结局：AST 里 `twice` 这个名字被原地改写成 mangled 符号 `_Z5twiceIiET_T_`，且 `twice<int>` 的函数体被发射成汇编

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 首版：按 06-文档骨架规范 §5 的线文档骨架新建 |

## §1 这条线是什么

一条**函数模板实参**从调用点出发、走到具体实例诞生的全过程。

入口是源码里的一处调用 `twice(a)`——`twice` 是个函数模板蓝图，实参 `a` 的类型
（`int`）此刻还没被推导出来。结局是两件事同时发生：

1. AST 里 `twice` 这个 `VarExpr` 的 `name` 字段被**原地改写**成 mangled 符号
   `_Z5twiceIiET_T_`，CodeGen 之后照着它发 `callq`；
2. `twice<int>` 这个**具体函数**被造出来（函数体被替换成 int 版本），进
   `m_functions` 列表，CodeGen 之后照着它发函数体。

这条线是「一次调用」的内部剖面，竖穿 `parse` / `sema` / `ast` 三层，
其中核心是**实参推导引擎**（`src/sema/template_deduction.cpp`）与
**实例化引擎**（`src/sema/template_instantiation.cpp`）两个模块。

本项目把这条线拆成教学可读的 6 段（`src/sema/template_deduction.h` 顶部的阶段分工表）：

| 段 | 名字 | 本线落在哪一跳 |
|---|---|---|
| S1 | 解析 | 第 1~2 跳（Parser 建蓝图 + 建调用点） |
| S2 | 基础推导（逐对合一） | 第 6~7 跳 |
| S3 | 显式实参 | 第 5 跳 |
| S4 | 不可推导上下文 | 第 6 跳（收尾检查） |
| S5 | 实例化 | 第 8~9 跳 |
| S6 | 重载决议 | 第 6 跳（赢家挑选） |

### 链路总图

```text
        源码：int b = twice(a);
                    │
     ┌──────────────┴───────────────┐
     ▼                              ▼
[Parser]                     [Sema Pass 1]
parsePrimaryExpr             processTemplateDecl
  → VarExpr("twice")            → m_functionTemplateCandidates["twice"]
parsePostfixExpr                  （蓝图入候选集，不查函数体）
  → CallExpr(callee, args)
     │
     ▼
[Parser] parseTemplateDecl  ──── S1：蓝图成形
  → TemplateDecl{funcTemplate}
     │
     ▼
[Sema Pass 3] analyzeFunctionBody
  → processStmt → visit(VarDeclStmt)
  → inferType → inferCall            ★ 断链点①：调用点分派
       │
       ├─ 成员方法调用？ ──→ findMethodInClass
       ├─ 类模板构造？   ──→ deduceCtadCore
       ├─ 普通查找/ADL？ ──→ 候选池裁决（非模板优先）
       ├─ 成员模板？     ──→ getOrInstantiateMemberFunction
       └─ 函数模板？     ──→ resolveTemplateCall   ← 本线走这支
              │
              ▼
       resolveTemplateCall            ★ 断链点③：候选集
         取 m_functionTemplateCandidates["twice"]
         对每个候选调 deducer.deduce(...)
              │
              ▼
       TemplateDeducer::deduce       ★ S3 → S2 → S4
         ├─ S3 显式实参占前缀
         ├─ 实参个数检查
         ├─ S2 逐对 deducePair(P, A)  ★ 断链点②
         │      └─ bind()：同一形参两次绑定必须一致
         └─ S4 收尾：还有未绑定的形参 ⇒ 不可推导上下文 ⇒ 失败
              │
              ▼
       S6 偏序：isAtLeastAsSpecialized  ★ 断链点④
         （多个可行候选时按 [temp.func.order] 挑最特化的）
              │
              ▼
       S5 实例化：getOrInstantiateFunction
         ├─ 缓存键 = mangled 名（命中 ⇒ 跳过重实例化）
         ├─ TemplateInstantiator::instantiateFunction
         │     ├─ 建替换表 {T → int}
         │     ├─ cloneMethod（深拷贝函数体 + 逐节点替换）
         │     └─ 定 mangled 名 _Z5twiceIiET_T_
         ├─ ★ 落地解析：resolveType(返回类型/形参类型)
         │     （替换 ≠ 实例化：残留的半成品模板 id 要再兑现一次）
         ├─ 注册进 m_functionMap / m_functions
         └─ analyzeFunctionBody(实例)  ← 两阶段查找的第二阶段
              │
              ▼
       calleeVar->name = instance->mangledName
         （AST 原地改写 ⇒ CodeGen 直接 callq _Z5twiceIiET_T_）
```

★ 标星处是**跨层接缝**（§4 断链点）。

## §2 走线五步法的结论（一句话一跳的表）

| # | 跳 | 落在哪个文件:函数 | 这一跳干了什么 |
|---|---|---|---|
| 1 | 建调用点 AST | `src/parse/parser.cpp:Parser::parsePrimaryExpr` → `Parser::parsePostfixExpr` | `twice` 建成 `VarExpr`；`(` 后建 `CallExpr(callee, arguments)` |
| 2 | 建模板蓝图 | `src/parse/parser.cpp:Parser::parseTemplateDecl` | 吃形参表（建形参作用域帧）、吃函数签名、存成 `TemplateDecl{funcTemplate}` |
| 3 | 蓝图入候选集 | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::processTemplateDecl` | 函数模板蓝图进 `m_functionTemplateCandidates[name]`，**不查函数体**（两阶段查找第一阶段） |
| 4 | 走到调用点 | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::analyzeFunctionBody` → `processStmt` → `visit(VarDeclStmt)` → `inferType` → `inferCall` | 进函数作用域、登记形参、推导初始化式类型，走到 `inferCall` |
| 5 | **S3** 显式实参 | `src/sema/template_deduction.cpp:TemplateDeducer::deduce`（显式前缀段） + `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::inferCall`（拆 `explicitTemplateArgs`） | `twice<int>(x)` 的 `<int>` 拆成类型实参表；`deduce` 把它按位置绑到形参表前缀 |
| 6 | **S2+S4** 逐对推导 | `src/sema/template_deduction.cpp:TemplateDeducer::deducePair` → `TemplateDeducer::bind` | 逐对 (模式 P, 实参 A) 结构化合一；绑定的形参两次必须一致；收尾查未绑定形参 |
| 7 | **S6** 重载决议 | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::resolveTemplateCall` → `SemanticAnalyzer::isAtLeastAsSpecialized` | 多个可行候选时按偏序挑最特化的那个 |
| 8 | **S5** 实例化 | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::getOrInstantiateFunction` → `src/sema/template_instantiation.cpp:TemplateInstantiator::instantiateFunction` → `cloneMethod` | 建替换表 → 深拷贝函数体并替换 → 定 mangled 名；带缓存 |
| 9 | 落地与注册 | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::getOrInstantiateFunction`（后半段） | ★ 替换 ≠ 实例化：签名再过一次 `resolveType`；注册进 `m_functionMap` / `m_functions`；分析实例函数体 |
| 10 | 改写 callee | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::resolveTemplateCall`（末段） | `calleeVar->name = instance->mangledName` —— 名字在语义期就定死 |

### 走线五步法的执行记录

| 步 | 做了什么 | 证据 |
|---|---|---|
| 1 跑通 | 真的跑了 `./minicc /tmp/linedemo/main.cpp`（`twice(7)`）与 `./minicc /tmp/linedemo/ovl.cpp`（两个 `pick` 重载）、`/tmp/linedemo/nd.cpp`（不可推导上下文）、`/tmp/linedemo/ex.cpp`（显式实参） | 退出码 14；`nd.cpp` 报 `no matching function for call to 'make'` |
| 2 画图 | 上面的链路总图 | 本节 |
| 3 逐跳 | §2 表 + §3 详解 | §3 |
| 4 找动机 | 只在 ★ 处问 | §3 每跳末尾 |
| 5 写疑问 | 见 §5 的边界清单 | §5 |

## §3 逐跳详解

### 第 1 跳：建调用点 AST

**源码锚点**：`src/parse/parser.cpp:Parser::parsePrimaryExpr` → `Parser::parsePostfixExpr`。

**做了什么**：`parsePrimaryExpr` 的 Identifier 分支把 `twice` 建成
`std::make_shared<VarExpr>(name)`；随后 `parsePostfixExpr` 的循环看到 `(`，
`parseArgumentList()` 收实参列表，产出 `CallExpr(expr, args)`。

★ 若写成 `twice<int>(x)`：`parsePrimaryExpr` 里 `check(TokenType::Less) &&
names().isTemplateName(name)` 两道闸同时成立 ⇒ 用 `TokenCheckpoint` 试探解析实参表，
试探成立（后面跟 `(` 或 `::`）才 `commit()`，实参挂进
`VarExpr::explicitTemplateArgs`。★ 闸门的意义：`a<b>(c)` 这种全变量比较链
若被误判成模板调用，是**拒收合法程序**（实测摘掉闸门 ⇒ 链接期
`undefined reference to 'a'`，变量被当函数模板实例发 callq）。

**为什么不用更简单的做法**：不在解析期就确定「`twice` 是模板」——
Parser 不持 Sema 句柄，只能问「这个名字**是不是模板名**」这一个 bit，
具体是哪个模板、实参是什么类型，要到语义期答。

### 第 2 跳：建模板蓝图

**源码锚点**：`src/parse/parser.cpp:Parser::parseTemplateDecl`。

**做了什么**：吃 `template` `<` 形参表 `>`，然后按形态分派到类/别名/函数/推导指引。
函数模板这一支解析签名（返回类型 + 形参表 + 函数体），产出
`TemplateDecl{ funcTemplate = <FuncDecl> }`。

★ 形参表解析期间会**开一帧模板形参作用域**（`TemplateParamFrame frame(this, decl.get())`，
构造即入栈、析构即出栈）。这帧的作用只有一个：让 `parseType` 能把裸 `T` 认成
`TemplateParam` 节点而不是 `Class("T")` 节点。对照 clang：
`clang/include/clang/Parse/Parser.h:Parser:MultiParseScope`（构造 Enter、析构 Exit）
+ `clang/lib/Parse/ParseTemplate.cpp:Parser::ParseTemplateParameters` 开
`TemplateScopes.Enter(TemplateParamScope)`。

**为什么不用更简单的做法**：不开这帧就要在 `parseType` 里维护一张「当前在哪些形参名里」
的全局表，而作用域链（`parent` 指针）天然给出「内层优先」的语义。

### 第 3 跳：蓝图入候选集

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::processTemplateDecl`。

**做了什么**：函数模板这一支只做三件事——打印蓝图摘要、
`auto& candidates = m_functionTemplateCandidates[func->name]; candidates.push_back(decl);`、
把蓝图塞进 `m_templates`。**不分析函数体**：模板体里的 `T` 是依赖类型，
要等实参到位才能定型。

★ 四种模板蓝图进四张不同的表（判据是「匹配算法不同」）：

| 蓝图 | 表 | 消费点 |
|---|---|---|
| 类模板主模板 | `m_classTemplates`（名字 → 蓝图，O(1)） | `getOrInstantiateClass` |
| 偏特化 | `m_partialSpecs`（名字 → 列表） | `selectClassTemplate` |
| 全特化 | `m_explicitSpecs`（名字 → 列表） | 同上 |
| **函数模板** | **`m_functionTemplateCandidates`（名字 → 列表）** | **`resolveTemplateCall`（本线）** |
| 别名模板 | `m_aliasTemplates` | `expandAliasTemplate`（解糖，不是实例化） |

**为什么不用更简单的做法**：不把函数模板塞进 `m_classTemplates`——
两者的匹配算法完全不同（类模板是「用实参表匹配特化模式」，函数模板是「用实参逐对合一」）。

### 第 4 跳：走到调用点

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::analyzeFunctionBody`
→ `processStmt` → `visit(VarDeclStmt)` → `inferType` → `inferCall`。

**做了什么**：`analyzeFunctionBody` 进函数作用域、登记形参与 `this`，
逐语句 `processStmt`（一次虚表跳转）。`visit(VarDeclStmt)` 先 `resolveType(声明类型)`、
折叠初始化式，再 `inferType(decl.initializer)` 推初始化式的类型——
`twice(a)` 这个 `CallExpr` 就是在这里被 `inferCall` 接住的。

`inferCall` 开头做三件事：

1. **认 callee**：`expr.callee->kind == NodeKind::Var` ⇒ 取 `calleeVar` 与 `funcName`。
2. **推实参类型**：`for (auto& arg : expr.arguments)` 逐个 `inferType(arg)`，
   同时记录**左值性**——`NodeKind::Var` / `Member` / `Index` /
   `Unary(Deref)` 算左值，其余算右值。左值性是推导的输入：
   `T&` 的绑定检查与 `T&&` 的万能引用折叠都要用。
3. **备好引用绑定判据**：`argsBindTo` / `checkArgBinding` 两个 lambda，
   谓词是 `referenceBindsValueCategory`（判据单点，在 `src/ast/type.cpp`）。

**为什么不用更简单的做法**：不把实参类型算好了再传给推导——
实参类型本身就是**推导的产物之一**（`auto` 形参、`T&&` 折叠都要在推导过程中才知道）。

### 第 5 跳：S3 显式实参

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::inferCall`
（`explicitTemplateArgs` 拆分段） → `src/sema/template_deduction.cpp:TemplateDeducer::deduce`
（S3 段）。

**做了什么**：调用点把 `VarExpr::explicitTemplateArgs`（`TemplateArg` 列表）拆成
`std::vector<TypePtr> explicitTypeArgs`。★ 拆的时候遇到**值形态**的实参会响亮报错：

```text
explicit non-type template argument '4' for function template 'twice' is not
supported yet (only class templates support non-type parameters)
```

——函数模板的 NTTP 尚未实现（类模板的 NTTP 已实现，见
`SemanticAnalyzer::checkTemplateArguments`），明确报错而不是把空 `TypePtr`
塞进推导引擎引发崩溃。

`deduce` 的 S3 段把显式实参**按位置**绑到形参表前缀：

```text
[deduction]   显式给定: T := int
[deduction]   ── 显式 1 个 / 推导补全剩余 1 个 ──
```

**为什么不用更简单的做法**：显式实参不做「后缀覆盖」而是「前缀占位」——
[temp.arg.explicit] 规定显式实参只能占形参表的**前缀**，后面的由推导补全；
若允许任意位置覆盖，`conv<int>(1, 2)` 的语义就说不清了。

### 第 6 跳：S2 + S4 逐对推导

**源码锚点**：`src/sema/template_deduction.cpp:TemplateDeducer::deduce`
→ `TemplateDeducer::deducePair` → `TemplateDeducer::bind`。

**做了什么**：`deduce` 的骨架是三段：

```text
S3 显式前缀 → 实参个数检查 → S2 逐对 deducePair → S4 收尾检查
```

**S2 逐对合一**：`deducePair(P, A, argIsLValue, ...)` 自外向内递归剥壳，
按 P 的形态分派（这是本线最核心的一段，§4 断链点② 列全部分支）。
每次要绑定某个形参时走 `bind`：形参第一次绑定直接写入替换表；
第二次绑定要求**新旧类型相等**，否则整次推导失败：

```text
[deduction]   P=T            A=int          ⇒ T := int
[deduction]   P=T            A=int          ⇒ T := int（一致 ✓）
[deduction]   ✗ conflicting types for deduction of 'T': previously deduced 'int', now 'double'
```

**S4 收尾**：走完全部实参后扫一遍形参表，**还有没被绑定的形参** ⇒ 不可推导上下文 ⇒ 失败：

```text
[deduction]   ✗ non-deduced context: template parameter 'T' cannot be deduced
               (only appears in non-deduced positions such as the return type;
                specify it explicitly, e.g. make<...>())
```

实测：`template <typename T> T make(); make();` ⇒ 上面这行 + 随后的
`[sfinae] ⤵ 候选【函数模板重载 'make'】被移出候选集` +
最终 `no matching function for call to 'make' (no template candidate deduces successfully)`。

**为什么不用更简单的做法**：不用「试所有可能的类型再回溯」——
合一算法（受限的 Hindley-Milner 类型推断）一次遍历就解出替换表，
且「同一变量只能绑定一次、重复绑定必须一致」这条不变量天然由 `bind` 兜住。

### 第 7 跳：S6 重载决议

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::resolveTemplateCall`
（赢家挑选段） → `SemanticAnalyzer::isAtLeastAsSpecialized`。

**做了什么**：先把**全部**候选跑一遍 `deduce`，成功的进 `viables`、
失败的走 `Sfinae::rejected` 记一笔（软失败，不是错误）。
若 `viables` 为空 ⇒ 报 `no matching function for call to '...' (no template candidate deduces successfully)`。

有多个可行候选时按**偏序**挑最特化的：

```cpp
size_t winnerIdx = 0;
for (size_t i = 1; i < viables.size(); i++) {
    if (isAtLeastAsSpecialized(viables[i].decl, viables[winnerIdx].decl)) {
        winnerIdx = i;
    }
}
```

`isAtLeastAsSpecialized(a, b)` 的方向容易搞反：**用 a 自己的参数类型当合成实参，
去推导 b**。b 能推导成功 ⇒ b 覆盖 a 的全部输入域 ⇒ a 接受的类型更少 ⇒ a 更特化。

实测（两个候选 `pick(T x)` 与 `pick(T* p)`，实参 `int*`）：

```text
[overload] partial ordering check (deduction-based)...
[deduction] ▶ pick — 模板参数 <T>，实参 1 个
[deduction]   P=T            A=T*           ⇒ T := T*
[deduction] ◀ 推导成功: <T=T*>
[overload]   ⇒ 前者至少与后者同样特化（前者胜）
  [overload] 2 viable candidate(s), partial ordering picks #1
```

**为什么不用更简单的做法**：不用「第一个成功的候选就是答案」——
两个候选都能推导出（`pick(T)` 与 `pick(T*)` 对 `int*` 都能推），
先到先得会**静默调错函数**。偏序是 [temp.func.order] 的要求。

### 第 8 跳：S5 实例化

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::getOrInstantiateFunction`
→ `src/sema/template_instantiation.cpp:TemplateInstantiator::instantiateFunction`
→ `TemplateInstantiator::cloneMethod`。

**做了什么**：

1. **算缓存键**：`NameMangler::mangleFunctionTemplateInstance(name, targs, returnType, parameters, typeParams)`
   —— 编出的是**完整签名**（返回类型 + 参数表），不只是模板实参。
   ★ 踩坑史：早期只编到模板实参，于是 `f(1)` 与 `f(1,2)`（两个重载、T 同为 int）
   编出同一个键，后者撞上前者的缓存项被当成「已实例化」⇒ **静默调用错误的实例**。
2. **查缓存**：命中 ⇒ 打印 `[instantiate] cache hit: <mangled> (skip re-instantiation)` 直接返回。
3. **实例化**：`instantiateFunction` 建替换表 `{T → int}`（tagged 的 `TemplateArg`），
   复用 `cloneMethod` 深拷贝函数体并逐节点替换，最后定 mangled 名。

实测（`twice<int>`）：

```text
╔══ Function Template Instantiation (S5) ═══════╗
║ Blueprint: twice <T>
║ Substitution: { T → int, }
║ Symbol: twice → _Z5twiceIiET_T_
╚═══════════════════════════════════════════════╝
```

★ `cloneMethod` 的替换日志逐节点打出：

```text
[clone:method] 'twice' return type: T →     [subst] ★ TemplateParam 'T' → 'int' (direct replacement)
[clone:method]   param 'x' : T →     [subst] ★ TemplateParam 'T' → 'int' (direct replacement)
```

**为什么不用更简单的做法**：不「按需现场替换 AST」——
模板实例化必须产出**独立的一份**（同一模板 + 同一实参表全局只实例化一次），
否则两次调用同一实例会各造一份函数体、符号重复定义。

### 第 9 跳：落地与注册

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::getOrInstantiateFunction`
（`instantiateFunction` 之后那一段）。

**做了什么**：三件事，顺序不可换。

1. **★ 落地解析**：`instance->returnType = resolveType(instance->returnType);`
   以及每个形参 `param.type = resolveType(param.type);`
   ——因为**结构化替换只做替换不做实例化**：`MyPtr<T>` 配 `{T:=int}` 出来的是
   `MyPtr<int>` 这个**半成品类型节点**（名字是模板名 + 实参表），不是 `MyPtr_int`。
   不补这一步，函数体里 `p.value` 在 `m_classTypes` 查不到 `MyPtr` ⇒ 报
   `No member 'value' in class 'MyPtr'`（类型其实是对的，只是「还没被兑现」）。
2. **注册**：`m_functionMap[mangled] = instance; m_functions.push_back(instance);`
   ——前者让后续同名调用直接命中，后者让 CodeGen 遍历得到它。
3. **分析函数体**：`analyzeFunctionBody(instance);` ——这就是
   [temp.names] 两阶段查找的**第二阶段**：非依赖名现在查，依赖名（`T`）此刻已成 concrete。

★ `analyzeFunctionBody` 会**嵌套调用**自己（实例体里可能再实例化别的模板），
所以它开头要保存/恢复 `m_stackOffset` / `m_currentClassName` / `m_currentReturnType` /
`m_currentFuncName` 四个外层上下文。

**为什么不用更简单的做法**：不在 `instantiateFunction` 里就把签名兑现——
实例化模块不该知道「怎么把模板 id 变成实例类型」（那是 Sema 的
`resolveType` 的活），分层上 Sema 负责查表与兑现，Instantiator 只负责替换。

### 第 10 跳：改写 callee

**源码锚点**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::resolveTemplateCall`（末段）。

**做了什么**：

```cpp
FuncDeclPtr instance = getOrInstantiateFunction(winner.decl, winner.result.deducedArgs);
calleeVar->name = instance->mangledName;
```

AST 里 `twice` 这个名字被**原地改写**成 `_Z5twiceIiET_T_`。
CodeGen 之后在 `visit(CallExpr)` 里 `funcName = asmSymbol(var->name)` 拿到这个串，
直接发 `callq _Z5twiceIiET_T_`。

实测日志：`[call] twice → _Z5twiceIiET_T_ (template resolved) → int`

**为什么不用更简单的做法**：不让 CodeGen 回头查「这个 callee 是不是模板」——
名字在语义期就定死，CodeGen 只负责照着发。这是本项目反复出现的同一手法：
**名字的「真身」在语义阶段兑现**（成员方法符号回填 `resolvedCalleeSymbol`、
`IndexExpr::atSymbol`、ADL 命中时把 `calleeVar->name` 改成 `N::get`，全是同一招）。

## §4 断链点

### 断链点 ①：`inferCall` 的调用点分派

**位置**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::inferCall`

全部实现（按代码里的先后顺序）：

| # | 分支 | 判据 | 落点 | 本线走这支？ |
|---|---|---|---|---|
| 1 | `std::declval<T>()` 内建 | `funcName == "std::declval"` | 直接给 `T&&` | 否 |
| 2 | **成员方法调用** | `expr.callee->kind == NodeKind::Member` | `findMethodInClass` / `findMethodInHierarchy`（BFS 沿基类链）+ `m_classMemberTemplates` 里的成员模板 | 否（本线 callee 是裸名） |
| 3 | **类模板构造表达式** | `m_classTemplates.count(tplName) \|\| m_aliasTemplates.count(tplName)` | `deduceCtadCore`（CTAD）或 `resolveType` + `pickBestByArgs` 选构造函数 | 否 |
| 4 | **普通查找 + ADL 候选池** | `m_functionMap.find(funcName)` + 由实参类型反推关联命名空间 | 候选池裁决（精确匹配优先） | **走了一半**：普通查找这一段先跑，但 `twice` 是模板、不在 `m_functions` 的「普通自由函数」池里（`isPlainFreeFunction` 判据：`ownerClassName.empty() && mangledName == name`）⇒ 池里没有它 |
| 5 | **函数模板路径** | `calleeVar` 非空 | `resolveTemplateCall` | **是** |
| 6 | 兜底 | 前五条都没命中 | `inferType(expr.callee)` ⇒ 报 `Undefined variable` | 否 |

**本线走哪支**：第 5 支（函数模板路径）。

**为什么**：★ 函数模板**不在**第 4 支的候选池里——池子只收「普通自由函数」
（`mangledName == name`），而函数模板实例的 `name` 保留模板名、`mangledName` 是 `_Z` 开头的
Itanium 名。若不设这道闸，`twice<int,int>(3,4)` 会被「精确匹配」抢先命中，
调用的却是一个**尚未按调用点重写名字**的实例 ⇒ 链接期 `undefined 'twice'`
（`test_tmpl_17` 的原始症状）。

### 断链点 ②：`deducePair` 的模式分派

**位置**：`src/sema/template_deduction.cpp:TemplateDeducer::deducePair`

全部实现（自外向内剥壳，顺序即代码顺序）：

| # | 判据 | 分支 | 干什么 |
|---|---|---|---|
| 0 | `A->isReference()` 且非 structuralMatch | 剥 A 的引用壳 | [expr.type]/1：表达式永远没有引用类型 |
| 0.5 | P 是别名模板 id | `desugarAlias` 解糖后递归 | [temp.alias]/1：别名不是新类型，只是另一个名字 |
| 1 | `P->isConst()` | 剥 P 的顶层 const 后递归 | [temp.deduct.type]：顶层 cv 不参与推导 |
| 2 | `P->isLValueReference()` | `referenceBindsValueCategory` 检查 → 递归 referencedType | [dcl.init.ref] 的绑定规则 |
| 3 | `P->isRValueReference()` | 万能引用路径：实参是左值 ⇒ `T := A&`；否则递归 | [temp.deduct.call]/3 |
| 4 | `P->isPointer()` | 实参必须是指针 → 递归 pointee | 结构化等价 |
| 5 | `isTemplateParamName(P)` | **值传递调整**后 `bind` | 剥顶层引用 + 剥顶层 const |
| 6 | `P->isClass() && !P->templateArgs.empty()` | 同类模板 ⇒ 逐位递归合一 | [temp.deduct.type]/8 的 `T<T1..>` 情形 |
| 7 | 兜底 | `P->equals(A)` | 非依赖部分必须恒等 |

★ 分支 0 与分支 5 都要剥壳，分支 3 的「万能引用」与分支 2 的「左值引用」
共用同一个 `argIsLValue` 开关——`structuralMatch`（偏特化匹配用）
与它**正交**，两个开关一起动必错。

**本线走哪支**：本线 `P = T`（裸形参）、`A = int` ⇒ 走分支 5（值传递调整后 bind）。

**为什么要逐条列**：分支 3（万能引用）把左值实参绑成 `T := A&`，
分支 5（值传递）把左值实参剥成 `T := int`——**同一个实参、两种绑法**，
选错就是符号名不同（`_Z2idIRiE` vs `_Z2idIiE`）的静默错误。

### 断链点 ③：候选集从哪儿来

**位置**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::resolveTemplateCall`（开头）

全部实现：

| 候选来源 | 表 | 何时进池 |
|---|---|---|
| 函数模板蓝图 | `m_functionTemplateCandidates[name]` | Pass 1 `processTemplateDecl` |
| 成员模板蓝图 | `m_classMemberTemplates[clsName]` | `instantiateClassTemplate` 的步骤 5.6 |
| 普通自由函数 | `m_functions`（`isPlainFreeFunction` 过滤） | Pass 2 `registerFunction` |

**本线走哪支**：第一支（`m_functionTemplateCandidates`）。

**为什么**：本项目**没有 clang 那样的 `OverloadCandidateSet`**——
三类候选分处三张表，各有各的匹配算法（模板候选要推导，普通函数要按类型择优，
成员模板要走类作用域 BFS）。把它们硬塞进一个候选集是 clang 的做法，
本项目教学上更愿意把三张表分开、把「谁先谁后」的裁决写在调用点。

### 断链点 ④：SFINAE 的三个吸收点

**位置**：`src/sema/sfinae.h`（协议）+ `src/sema/sfinae.cpp`（唯一日志出口）

全部实现（三方协议）：

| 角色 | 位置 | 动作 |
|---|---|---|
| ① 产生方 | `TemplateInstantiator::substituteType` / `SemanticAnalyzer::evaluateDecltype` | `Sfinae::fail(原因)` 抛 `SubstitutionFailure` |
| ② 传播方 | 各层递归、回调链 | **不捕获**，任其冒泡 |
| ③ 吸收方 | `Sfinae::attempt` | 捕获 ⇒ 移除候选 ⇒ 试下一个 |

三个吸收点（全项目仅此三处，`src/sema/sfinae.h` 的「收口点」表列明）：

| 吸收点 | 位置 | 失败形态 |
|---|---|---|
| 偏特化模式匹配 | `src/sema/template_deduction.cpp:TemplateDeducer::matchPattern` | 异常式（`Sfinae::attempt` 包住 `reducePattern`） |
| **函数模板重载** | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::resolveTemplateCall` | **返回码式**（`deduce` 返回 `success=false`，再 `Sfinae::rejected`） |
| 偏序裁决 | `src/sema/semantic_analyzer.cpp:SemanticAnalyzer::classSpecAtLeastAsSpecialized` | 异常式 |

**本线走哪支**：第二个（返回码式）——本线的 `deduce` 失败不是异常，而是返回值。

**为什么两种写法并存**：返回码式**不包 `SfinaeContext`**——它压根没有异常在栈上回退，
也就没有「直接上下文」这回事，硬塞标记只会让状态失真（`src/sema/sfinae.h` 的注释原话）。

### 断链点 ⑤：偏序裁决的方向

**位置**：`src/sema/semantic_analyzer.cpp:SemanticAnalyzer::isAtLeastAsSpecialized`

全部实现：

| 语义 | 判据 | 结果 |
|---|---|---|
| a 至少与 b 同样特化 | 用 **a 自己的参数类型**当合成实参去推导 **b** | b 推成功 ⇒ a 更特化 ⇒ a 胜 |

**本线走哪支**：唯一一支（本项目没做 clang 那种完整的偏序合成类型前缀 +
dominance 循环，只做「基于推导的偏序」这一条主干）。

**为什么方向易错**：注释里写死了——「用 a 自己的参数类型当合成实参，去推导 b。
b 能推导成功 ⇒ b 覆盖 a 的全部输入域 ⇒ a 接受的类型更少 ⇒ a 更特化」。
实测 `pick(T)` vs `pick(T*)` 对 `int*`：合成实参取 `T*` 去推 `pick(T)` 成功 ⇒
`pick(T*)` 更特化 ⇒ 赢家是 #1 ⇒ 实例符号 `_Z4pickIiET_PT_`（参数是 `PT_` = `int*`）。

## §5 分叉与旁路

### 主线上的可选分支

| 分支 | 触发 | 走到哪跳改道 | 实测 |
|---|---|---|---|
| 显式模板实参 `twice<int>(x)` | `VarExpr::explicitTemplateArgs` 非空 | 第 5 跳（S3 生效），第 6 跳的 S2 仍会跑但形参已被占位 | `conv<int>(1, 2)` |
| 不可推导上下文 | 形参只出现在返回类型 | 第 6 跳失败 ⇒ 第 7 跳的 `viables` 为空 ⇒ 报 `no matching function` | `make()` |
| 实参个数不符 | 实参个数 ≠ 形参个数 | 第 6 跳开头直接失败（`argument count mismatch`） | — |
| 绑定冲突 | 同一形参两次绑到不同类型 | 第 6 跳的 `bind` 失败（`conflicting types for deduction of 'T'`） | — |
| 多个可行候选 | 同名模板 ≥ 2 | 第 7 跳走偏序；只有一个时不打 `[overload]` 行 | `pick(T)` / `pick(T*)` |
| 缓存命中 | 同模板 + 同实参表已实例化 | 第 8 跳直接返回，打 `[instantiate] cache hit: ...` | 同一 TU 里调两次 `twice<int>` |
| 成员模板 | callee 是 `obj.m(...)` | 断链点①第 2 支 → `getOrInstantiateMemberFunction`（与本线**逐字同构**，只是符号是 `类名_方法名_实参`） | `tests/tmpl/test_tmpl_69` |
| CTAD | `MyPtr m(7);` | 断链点①第 3 支 → `deduceCtadCore` 复用同一套 `deducePair` | `tests/tmpl/test_tmpl_51` |

### 旁路清单（属于本线但值得另开线的分支）

- **类模板的特化择优**（`selectClassTemplate` 的三路择优：全特化 → 偏特化 → 主模板，
  以及 [temp.class.order] 的 dominance 循环）——本线只覆盖函数模板。
- **SFINAE 惯用法的完整链路**（`void_t` 探测、`decltype` 求值、`is_range` 偏特化）——
  本线只覆盖「候选被移除」这一个侧面。
- **别名模板的解糖**（`expandAliasTemplate`）——本线在 `desugarAlias` 里用到它，
  但解糖本身的三个落点（使用点 / 替换期 / 推导期）值得单开线。

### 已知边界（本线走完但没做的事）

- **函数模板的 NTTP 未实现**：`twice<4>(x)` 响亮报错（`explicit non-type template argument
  '4' for function template 'twice' is not supported yet`）。类模板的 NTTP 已实现。
- **偏序只做了一条主干**（基于推导），没做 clang 那种合成类型前缀 + dominance 循环的
  完整 [temp.func.order]。
- **`[temp.deduct.call]` 的调整不完整**：不做用户定义转换、不算隐式转换序列的 rank
  （`pickBestByArgs` 只分「精确」与「可隐式转换」两档）。
- **实参类型未知时不做判定**：`pickBestByArgs` 遇到 `argTypes` 里有 `nullptr`
  （依赖上下文 / SFINAE 探测）时退回「取声明序首个」，避免把「判不了」误报成「匹配失败」。
- **浮点字面量不支持**：`2.5` 在词法期就被切错（`scanNumber` 只处理整数字面量），
  实测 `conv<int>(1, 2.5)` 报 `[Parse Error] 3:28 at '5': Expected member name`。

## §6 观测点

这条线上打了哪些日志键（从源码 grep 实际前缀，非推测）：

| 环节 | 日志键 | 出处 | 什么时候打 |
|---|---|---|---|
| 解析 | `[parse:template]` | `src/parse/parser.cpp:parseTemplateDecl` | 模板声明解析（`▶` / `★ type parameter registered` / `◀ ... blueprint` / `blueprint summary`） |
| 解析 | `[parse:type]` | `src/parse/parser.cpp:parseType` | 形参/返回类型解析（`base = T (template param, scope hit)`） |
| 解析 | `[parse:targ]` | `src/parse/parser.cpp:parseTemplateArgumentList` | 模板实参表（`★ template argument list: <N argument(s)>`） |
| 解析 | `[parse]` | `src/parse/parser.cpp:parsePrimaryExpr` | `template-id: conv<1 explicit arg(s)>` |
| 注册 | `[register]` + `↳ overload candidate set 'X' size = N` | `src/sema/semantic_analyzer.cpp:processTemplateDecl` | 函数模板蓝图入候选集 |
| 调用点 | `[call]` | `src/sema/semantic_analyzer.cpp:inferCall` | 候选集大小（`twice — 1 function template candidate(s), overload resolution begins`）、`candidate: template <T> twice`；实参类型与左值性是**无前缀的续行** `arg: int (lvalue)` |
| 调用点 | `[call] X → <mangled> (template resolved) → <type>` | `src/sema/semantic_analyzer.cpp:resolveTemplateCall` | 决议完成 |
| 推导 | `[deduction]` | `src/sema/template_deduction.cpp:deduce` / `deducePair` / `bind` | `▶` 开头（模板参数 + 实参数）、`显式给定:`、`── 显式 N 个 / 推导补全剩余 M 个 ──`、逐对 `P=… A=… ⇒ T := …`、`◀ 推导成功: <T=int>`、`✗` 各种失败原因 |
| 推导 | `[deduce:alias]` | `src/sema/template_deduction.cpp:reducePattern` | 模式位里的别名模板解糖 |
| 偏序 | `[overload]` | `src/sema/semantic_analyzer.cpp:isAtLeastAsSpecialized` / `resolveTemplateCall` | `partial ordering check (deduction-based)...` / `⇒ 前者至少与后者同样特化（前者胜）` / `N viable candidate(s), partial ordering picks #K` |
| 实例化 | `╔══ Function Template Instantiation (S5) ══╗` 框 | `src/sema/template_instantiation.cpp:instantiateFunction` | 实例化时（框内含 `Blueprint` / `Substitution` / `Symbol`） |
| 实例化 | `[instantiate]` | `src/sema/semantic_analyzer.cpp:getOrInstantiateFunction` | 缓存命中（`cache hit: <mangled> (skip re-instantiation)`） |
| 实例化 | `[instantiate:name]` | `src/sema/template_instantiation.cpp` | 实例名生成 |
| 替换 | `[subst]` / `[subst:map]` | `src/sema/template_instantiation.cpp:substituteType` | 逐节点替换（`★ TemplateParam 'T' → 'int' (direct replacement)` / `★ Pointer substituted: T* → int*`） |
| 克隆 | `[clone:method]` | `src/sema/template_instantiation.cpp:cloneMethod` | 克隆函数体的每个成员（`return type` / `param 'x'`） |
| SFINAE | `[sfinae]` | `src/sema/sfinae.cpp`（全项目唯一出 `[sfinae]` 的文件） | 发出替换失败信号（`⚡`）、候选被移出候选集（`⤵`）、void_t 探测链路、decltype 降级 |
| 实参校验 | `[sema:targ]` | `src/sema/semantic_analyzer.cpp:checkTemplateArguments` | 类模板实参逐位校验（`✓ param N: 'T' (type) ← int`） |

★ 观察：**函数模板这条线全程没有 `[deduction]` 之外的推导日志前缀**，
`[sema:targ]`（实参校验）只在**类模板**路径上出现——函数模板的实参由推导引擎产出，
不走 `checkTemplateArguments`。

## 词汇表

| 名词 | 一句话（本篇专用） |
|---|---|
| 蓝图 | 模板声明的 AST 半成品，等实参到位才变成具体函数 |
| 模式 P / 实参 A | 推导的一对：P 是形参类型（含形参变量的模式），A 是实参类型 |
| 合一 | 受限的 Hindley-Milner 类型推断：逐对匹配、解出替换表、同一变量只能绑一次 |
| 替换表 Subst | 形参名 → 已推出的类型的映射 |
| 显式实参 | 用户在调用点写的 `<...>`，占形参表前缀 |
| 不可推导上下文 | 只出现在返回类型等位置的形参，无法从实参推出，必须显式给定 |
| 偏序 | 多个可行模板候选之间的「谁更特化」裁决（[temp.func.order]） |
| 落地解析 | 替换之后签名里残留的半成品模板 id，再过一次 `resolveType` 兑现成实例类型 |
| 半成品类型节点 | 名字是模板名 + 实参表、但还没实例化的类型节点（如 `MyPtr<int>`） |
