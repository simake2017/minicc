# 技术设计：名字可见性裁决下沉语义期（B37）

> 对应源码：`src/sema/semantic_analyzer.cpp`（判据与裁决）、
> `src/parse/parser.cpp`（给候选）、`include/ast.h`（携带名字的一槽）。
> 来源：`docs/specs/visible-name-lookup.spec.md`（2026-11-02 签字）。
> 审法：**每个消费点问的是同一个问题吗？** 答案必须是"是，且都走同一个函数"。

## 1. spec → 设计映射表

| spec 条款 | 设计落点 |
|---|---|
| §1.1 实体：名字可见性判据 | `SemanticAnalyzer::lookupVisibleType` + `isNameDeclared`（semantic_analyzer.cpp） |
| §2 步骤 1 Parser 给候选 | `Parser::parseStatement` 裸名分支 → `EmptyStmt(typeName)`（parser.cpp:2241 附近） |
| §2 步骤 2 Sema 裁决 | `SemanticAnalyzer::visit(EmptyStmt&)`（semantic_analyzer.cpp） |
| §2 步骤 3 三岔处置 | 同上三分支 |
| §2.5 更新对照 | `include/ast.h` 的 `EmptyStmt::typeName` 槽 |
| 异常1 | `visit(EmptyStmt&)` 末支：`error("use of undeclared identifier ...")` |
| 异常2 | `visit(EmptyStmt&)` 中支：构造 `ExprStmt{VarExpr}` 走既有表达式路径 |
| 异常3 | `lookupVisibleType` 的命名空间链 while 循环 |
| 异常4 | `lookupVisibleType` 的类作用域分支排在命名空间链之前 |
| 异常5 | `lookupVisibleType` 走 `m_classDecls[cls]->typeAliases`（不经符号表） |
| §4 OOS 1..6 | 不设计对应机制 |
| §5 日志键 | `visit(EmptyStmt&)` 的 `[sema:stmt]` 行；`resolveType` 沿用既有 `[resolve]` / `[sema:alias]` 行 |

## 2. 整体流程图

```text
┌────────────────────────────────────────────────────────────┐
│ Parser::parseStatement —— 标识符开头                        │
└───────────────┬────────────────────────────────────────────┘
                │ TokenCheckpoint 试探 parseType
                ▼
      ┌───────────────────────┐   失败/推翻
      │ 类型语法成立          │──────────────► 表达式路径（既有）
      └───────┬───────────────┘
              │ 裸名（无声明符、无模板 id 语法）
              │ 且名字登记表 type 位命中 ⇒ 只当【候选】
              ▼
   ┌────────────────────────────┐
   │ EmptyStmt{typeName="S"}     │   ★ 名字进 AST，Parser 不下"可见"的结论
   └─────────────┬──────────────┘
                 ▼
   ┌──────────────────────────────────────────┐
   │ Sema::visit(EmptyStmt&)                   │
   │  ① lookupVisibleType("S")  可见的类型？    │
   │     ├ 命中 ⇒ 判为无声明符声明（完成）      │
   │     └ 未命中 ↓                            │
   │  ② isNameDeclared("S")     声明过？       │
   │     ├ 是 ⇒ 表达式路径（VarExpr→ExprStmt） │
   │     └ 否 ⇒ error use of undeclared ident. │
   └──────────────────────────────────────────┘
```

## 3. 子流程图 + 业务说明

### 3.1 lookupVisibleType —— 名字可见性判据（单点）

**流程图**：

```text
      ┌─ 输入：裸名 name ─────────────────────────┐
      │   上下文：m_currentClassName / m_currentNamespace
      └───────────────────────┬───────────────────┘
                              ▼
              ┌───────────────────────────┐
              │ ① 当前类作用域            │ 命中 ⇒ level=1，返回别名目标
              │  （m_classDecls[cls]      │  ★ 不经符号表：实例类的别名
              │    ->typeAliases）        │    没进符号表（异常5）
              └───────────┬───────────────┘
                          │ 未命中
                          ▼
              ┌───────────────────────────┐
              │ ② 命名空间链              │ 命中 ⇒ level=2（scope 非空）
              │  scope=N::M → N → ""      │        或 level=3（全局）
              │  （逐级外层，异常3）      │
              └───────────┬───────────────┘
                          │ 未命中
                          ▼
                   level=0（不可见）
```

**业务说明**：
① 先查当前类的类型别名——clang 的 [basic.lookup.unqual] 在类体内先查类作用域；
   走类声明的 typeAliases 而不是符号表，因为**类模板实例的别名只克隆进了实例类**
   （template_instantiation.cpp 的克隆步骤），没进符号表。
② 再沿命名空间链逐级外层查——命名空间成员的登记方式就是名字前缀化（"N::S"），
   故按前缀逐级查；逐级外层是 clang 的 DeclContext 链语义（异常3）。
③ 全局在链的最后（scope 为空 ⇒ 查裸名）。

### 3.2 resolveType 的兜底链 —— 收敛到单点

**业务说明**：原先 resolveType 里手写着三段兜底（全局 → 命名空间一层 → 当前类），
与裸名声明那条路各写一份；B37 把它换成一次 `lookupVisibleType` 调用，
既满足红线 3（判据单点），又顺带把命名空间查找从"一层"升级成"逐级外层"。
既有日志行（`[resolve] …` / `[sema:alias] …`）逐字保留——`VisibleType.raw`
专门存未解糖的别名目标，供 `[sema:alias]` 行打印。

### 3.3 visit(EmptyStmt&) —— 裸名声明的裁决

**业务说明**：
① `typeName` 为空 ⇒ 普通空语句（`;`）或无声明符的模板 id 声明（`A<int,int>;`），
   无事可做 ⇒ 直接返回（不 resolveType ⇒ 不实例化模板，与 clang 同）。
② 第一问：可见的类型？命中 ⇒ 合法（clang 只给 -Wmissing-declarations warning）。
③ 第二问：声明过吗？声明过但不是类型 ⇒ 它是表达式语句（clang 是 warning
   expression result unused）⇒ 构造 `ExprStmt{VarExpr}` 交给既有表达式路径。
④ 都没声明过 ⇒ 响亮拒收，文案 `use of undeclared identifier 'X'`（与 clang 逐字）。

### 3.4 Parser —— 只给候选

**业务说明**：语句层的裸名分支命中名字登记表的 type 位时，产出
`EmptyStmt{typeName}` 而非无名 `EmptyStmt`；**Parser 不判断可见性**。
名字登记表是"本 TU 某处声明过这个类型"的跨作用域并集，拿它当"可见"会静默
放行非法程序（`namespace N { struct S{}; } int main(){ S; }`）。

## 4. 异常流转表

| 异常 | 触发子流程 | 后果 |
|---|---|---|
| 异常1 不可见且未声明 | 3.3 ④ | rc=1，`use of undeclared identifier 'X'` |
| 异常2 声明过但不是类型 | 3.3 ③ | 走表达式路径；变量/函数照常求值，rc 不变 |
| 异常3 外层命名空间可见 | 3.1 ② | 解析成功（此前会拒收合法程序） |
| 异常4 类作用域遮蔽 | 3.1 ① | 别名胜出 |
| 异常5 实例类别名 | 3.1 ① | 别名可见 |

## 5. 突变验证清单

| # | 摘掉的判据 | 预期变红的用例 |
|---|---|---|
| ① | `lookupVisibleType` 的命名空间链逐级外层（只查一层） | 单测 `NameVisibility.NamespaceChainOuterLevel` |
| ② | `lookupVisibleType` 的类作用域分支（挪到命名空间链之后） | 单测 `NameVisibility.ClassScopeAliasVisibleInsideClass` |
| ③ | `visit(EmptyStmt&)` 的第二问（不查 isNameDeclared，直接报错） | 单测 `NameVisibility.ShadowedByLocalVariable` |
| ④ | Parser 不带名字（回到无名 EmptyStmt） | 集成 `basics_26`（负例恢复 rc=0） |

## 6. 有意日志漂移

| 文件 | 差异 | 原因 |
|---|---|---|
| `tests/lang/test_basics_23_name_registry.cpp` | 多两行 `[sema:stmt] …` + 一行 `[resolve] 'S' → int (kind=Variable, …)` | 现场②的遮蔽语义改成与 clang 一致（变量被登记 ⇒ 走表达式路径） |
| `tests/tmpl/test_tmpl_54_error_tparam_scope_after_declarator.cpp` | unknown type name 的括注改写 | 旧文案自称"已由解析期名字登记表在语法期消歧"，B37 后由语义期裁决 |
| `tests/lang/test_basics_25..27_*.cpp` | 新文件入列 | 本批新增 |
