# 技术设计：作用域即身份（模板与命名空间成员的名字带上作用域，B38）

> 对应源码：`src/sema/semantic_analyzer.cpp`（前缀化与限定名绑定）、
> `src/sema/template_instantiation.cpp`（mangling 净化）。
> 来源：`docs/specs/scope-qualified-names.spec.md`（2026-11-02 签字）。
> 审法：**同一个名字的"作用域"是不是只写了一遍？** 答案必须是"是，只写一遍，其余全是派生"。

## 1. spec → 设计映射表

| spec 条款 | 设计落点 |
|---|---|
| §2.1 S2-1 前缀化用完整路径 | `processNamespaceDecl` 的 4 个成员分支 |
| §2.1 S2-2 模板按作用域注册 | `processNamespaceDecl` 新增的 `case NodeKind::Template` |
| §2.1 S2-3 限定名不再剥尾 | `resolveType` 的模板 id 分支（`m_classTemplates` / `m_aliasTemplates` 查表） |
| §2.1 S2-4 登记表退场一处 | 删 `SemanticAnalyzer::m_names` / `setNameRegistry` / `main.cpp` 的注入 |
| §3.2 mangling 净化 | `NameMangler::mangleTemplateInstance` 调 `symbolSafe` |
| §4 突变验证 1~4 | 见 §5 |

## 2. 整体流程图

```text
┌───────────────────────────────────────────────────────────────┐
│ processNamespaceDecl（进入 N，m_currentNamespace = "N"）      │
└──────────────┬────────────────────────────────────────────────┘
               │
     ┌─────────┴──────────────────────────────────────────┐
     │ ★ 前缀化单点：全名 = m_currentNamespace + "::" + 裸名 │
     └─────────┬──────────────────────────────────────────┘
               │
   ┌───────────┼───────────────┬─────────────────┬──────────────────┐
   │           │               │                 │                  │
 Function    Class         GlobalVar          Enum            Template ★新增
 "N::f"     "N::C"          "N::g"           "N::E"      "N::B"（类模板）
                                                        "N::Vec"（别名模板）
   │           │               │                 │                  │
   └───────────┴───────────────┴─────────────────┴──────────────────┘
                                    │
                                    ▼
              ┌──────────────────────────────────────┐
              │ 注册键 = 全名                        │
              │  m_functions / m_classDecls / …      │
              │  m_classTemplates["N::B"]            │
              │  m_aliasTemplates["N::Vec"]          │
              └───────────────┬──────────────────────┘
                              │ 使用点 "N::B<int>"
                              ▼
              ┌──────────────────────────────────────┐
              │ resolveType：按完整限定名查表          │
              │  命中 ⇒ 用它（不剥尾）                 │
              │  未命中 ⇒ error:                      │
              │    no template named 'B' in ns 'N'    │
              └───────────────┬──────────────────────┘
                              │
                              ▼
       ┌────────────────────────────────────────────────────────┐
       │ 实例化（TemplateInstantiator::instantiate）             │
       │   实例名  = 蓝图名 + "_" + 实参 ⇒ "N::B_int"            │
       │   缓存键  = 使用点限定名 + 实参 ⇒ "N::B<int>"           │
       │   mangled = symbolSafe(蓝图名) ⇒ "_Z4N__BIiE"          │
       │   汇编符号 = asmSymbol("N::B_int") ⇒ "N__B_int"        │
       └────────────────────────────────────────────────────────┘
```

## 3. 子流程图 + 业务说明

### 3.1 前缀化（单点）

**业务说明**：命名空间成员的全名 = **完整命名空间路径** + `::` + 裸名。
修前用 `decl->name`（本层名字）⇒ 嵌套时丢外层（`A::B::f` 注册成 `B::f`）。
改用 `m_currentNamespace`（进入命名空间时已拼好的路径）—— 一处改动，
函数 / 类 / 全局变量 / 枚举 / 模板五种成员同时受益（判据单点，红线 3）。

★ **模板是本批新增的分支**：模板原先走 `default:`（不前缀化）⇒ 按裸名注册 ⇒
两个命名空间的同名模板撞成同一个实例。补上这一分支后，模板与类走同一套约定。

### 3.2 限定名绑定（不再剥尾）

**业务说明**：使用点的名字 `N::B` 已经是**完整限定名** ⇒ 直接查注册表。
查不到就是查不到（clang 同）—— 不再"剥 :: 末段查全局蓝图"。

★ 这一步同时消灭了**第二份作用域事实**：修前 Sema 要靠名字登记表的
`classifyQualified(scope, tail)` 回答"这个限定名是不是模板"，而 Parser
也在用同一张表回答"这个名字是不是模板"—— 同一件事两个数据源。
本批把 Sema 侧那一份删掉（Sema 自己就有注册表了），登记表**只剩 Parser 侧**
的候选过滤用途（过渡态，Stage 3 消灭）。

### 3.3 实例身份的三处派生

**业务说明**：模板名带作用域后，实例的**身份**在三处自然带上作用域，
不需要各写一份：

| 派生点 | 公式 | 例子 |
|---|---|---|
| 实例名 | 蓝图名 + `_` + 实参 | `N::B_int` |
| 缓存键 | `losslessArgumentsKey(使用点名, 实参)` | `N::B<int>` |
| mangled | `symbolSafe(蓝图名)` + 实参编码 | `_Z4N__BIiE` |

★ `symbolSafe` 把 `::` 洗成 `__`（等长 ⇒ 长度前缀仍自洽）；`asmSymbol` 在
汇编期做同一件事（`N::B_int` ⇒ `N__B_int`）。**两处清洗规则不同但各司其职**：
前者是 mangling 的合法字符集，后者是汇编标签的合法字符集。

## 4. 异常流转表

| 异常 | 触发子流程 | 后果 |
|---|---|---|
| 限定名查不到 | 3.2 | rc=1，`no template named 'B' in namespace 'N'`（与 clang 逐字） |
| 同名模板跨命名空间 | 3.1 + 3.3 | 两个实例、两个符号（不再撞） |
| 嵌套命名空间 | 3.1 | 注册名含完整路径，`A::B::f()` 可用 |
| 全局模板 vs 命名空间模板 | 3.3 | 缓存键不同 ⇒ 两个实例（原先撞一个） |

## 5. 突变验证清单

| # | 摘掉的判据 | 实跑变红的用例 |
|---|---|---|
| ① | 前缀化用 `decl->name`（本层）而非 `m_currentNamespace` | `ScopeQualifiedNames.NestedNamespaceKeepsFullPath` |
| ② | 模板注册不前缀化 | `ScopeQualifiedNames.SameNameTemplatesInDifferentNamespacesStayDistinct`（+2 例连带） |
| ③ | 限定名改回"剥尾归一" | `ScopeQualifiedNames.QualifiedNameNotFoundIsAnError` |
| ④ | mangling 不净化 | `ScopeQualifiedNames.MangledNameHasNoIllegalChars` |

## 6. 有意日志漂移

| 文件 | 差异 | 原因 |
|---|---|---|
| `tests/lang/test_basics_22_stmt_ambig_tentative_parsetype.cpp` | 模板蓝图名 / 实例名 / mangled 带作用域；`[resolve] 限定模板名 …` 一行消失 | §3.1 + §3.2 + §3.3 |
| `tests/lang/test_basics_23_name_registry.cpp` | 同上（M::B） | 同上 |
| `tests/lang/test_basics_28..29_*.cpp` | 新文件入列 | 本批新增 |
| `.logbaseline/_unit_cases.txt` / `_unit_summary.txt` | +7 例 | 5 个新单测 + 2 个新集成的子进程包装；★ 旧基线的 369 本身是没重建二进制的假象 |
