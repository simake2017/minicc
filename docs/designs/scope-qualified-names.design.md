# 技术设计：作用域即身份（模板与命名空间成员的名字带上作用域，B38）

> 对应源码：`src/sema/semantic_analyzer.cpp`（前缀化与限定名绑定）、
> `src/sema/template_instantiation.cpp`（mangling 净化）。
> **本项目按 clang 目录组织，不采用 skill 的"1 设计 = 1 flow 文件"**
> —— 用户 2026-10-23 拍板，见 `docs/constraints/00-全局约束.md` 头部。
> 来源：`../specs/scope-qualified-names.spec.md`
> 契约变更点：名字与符号的形状（注册键 / 实例名 / 缓存键 / mangling 全部派生自作用域）
> （= 仓库内 `docs/specs/scope-qualified-names.spec.md`，2026-11-02 签字）。
> 标准：不看 spec 也能照本文实现。
> 审法：**同一个名字的"作用域"是不是只写了一遍？** 答案必须是"是，只写一遍，其余全是派生"。
> 读者：改命名空间成员注册 / 模板注册 / mangling / 限定名绑定的人。
> 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 按文档骨架规范补齐头部与变更记录；§1 映射表补行；补「§7 裁量区清单」 |

## 1. spec → 设计映射表

| spec 条款 | 设计落点 |
|---|---|
| §2.1 S2-1 前缀化用完整路径 | `processNamespaceDecl` 的 4 个成员分支 |
| §2.1 S2-2 模板按作用域注册 | `processNamespaceDecl` 新增的 `case NodeKind::Template` |
| §2.1 S2-3 限定名不再剥尾 | `resolveType` 的模板 id 分支（`m_classTemplates` / `m_aliasTemplates` 查表） |
| §2.1 S2-4 登记表退场一处 | 删 `SemanticAnalyzer::m_names` / `setNameRegistry` / `main.cpp` 的注入 |
| §2.2 Out of Scope（本阶段不做） | 不设计对应机制：无 `using namespace N;` / 无函数模板作用域化 / 无嵌套类 / 无模板模板实参跨作用域匹配 / 不改解析期「名字是不是模板」这一问 |
| §3.1 作用域前缀化（单点） | 子流程 3.1 前缀化（单点）：`processNamespaceDecl` 里 `m_currentNamespace` 拼接 |
| §3.2 模板按作用域注册 | 子流程 3.1 的注册键（`m_classTemplates["M::B"]` / `m_aliasTemplates["M::Vec"]`）＋ 子流程 3.3 实例身份三处派生 |
| §3.3 限定名模板 id 的绑定 | 子流程 3.2 限定名绑定（不再剥尾）：按完整限定名查注册表 |
| §3.2 mangling 净化 | `NameMangler::mangleTemplateInstance` 调 `symbolSafe` |
| §4 突变验证 1~4 | 见 §5 |
| §5 与 Stage 3 的关系 | 不设计对应机制（Stage 3 = 边解析边 `ActOn*` 登记，属架构改造，不是判据调整） |
| §6 有意日志漂移 | §6 有意日志漂移表（逐文件列差异与原因） |

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

## 7. 裁量区清单

| AI 可以自决 | 必须问人 |
|---|---|
| 两个净化器**各自保留哪些字符**（`symbolSafe` 给 mangling、`asmSymbol` 给汇编标签）——只要 `::` 洗成 `__`（等长 ⇒ Itanium 长度前缀仍自洽），两个清单各自自决 | **收紧方向**已定（`N::B` 查不到 ⇒ 报错，不回退全局 B），但**是否附 clang 的 did-you-mean 建议**须问人——clang 有 "did you mean simply 'B'?"，本项目不做 |
| 将来新增的成员种类（`using` 声明、`inline namespace` 等）**是否自动并入 §3.1 的前缀化单点**——并入，单点的收益才成立 | **命名空间别名**（`namespace A = B;`）的前缀化口径：按别名 A 还是按被别名 B？本批未覆盖（`processNamespaceDecl` 只处理 `namespace X {`） |
| `m_currentNamespace` 的**拼接时机**（进入命名空间时拼 vs 注册时拼）——本批定为进入时拼，具体实现自决 | **命名空间成员变量**在块作用域的可见性（`namespace N { int g; }` 里裸用 `g` 不可见）——是缺陷还是特性，须问 |
| 缓存键 `losslessArgumentsKey` 与 mangling 的**具体编码串**——只要四者同源于蓝图名（§3.1 前缀化单点），串怎么拼自决 | 缓存键 / mangling 是否**随作用域一起变**——会改既有实例名 ⇒ 破坏基线 ⇒ 须签字 |
| §5 突变验证 4 条的**具体写法与用例挑选** | 任何**非 §6 声明范围**的基线漂移 |
