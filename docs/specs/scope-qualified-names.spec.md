# 规格：作用域即身份（模板与命名空间成员的名字带上作用域）

> 阶段：**Stage 2**（承 Stage 1 的红线 1 改写 —— docs/learn/48）
> 关联缺陷：BUGS.md **B38**（跨命名空间同名模板撞实例 / 限定名绑错模板）
> 关联学习产物：docs/learn/49-scope-qualified-names.md
> 签字：2026-11-02（Stage 1 拍板表第 1 条「阶段 1 → 阶段 2（登记表退场）」）

## 1. 需求与动机

Stage 1 把**可见性**（名字在不在这里可见）判据搬进了 Sema。本阶段搬**种类**里
剩下的最后一块：**名字属于哪个作用域**。

clang 的模型一句话：**作用域是名字身份的一部分**。

- 模板声明属于 DeclContext（命名空间）⇒ `LookupName` 按上下文查，`N::B` 与 `::B`
  是两个不同的实体；
- mangling 带上下文：`_ZN1N1BIiE`（`N::B<int>`）vs `_Z1BIiE`（`::B<int>`）；
- 实例缓存键带上下文。

minicc 现状（探针实测，clang++-18 -std=c++20 对拍）：

| 程序 | clang | minicc 现状 | 性质 |
|---|---|---|---|
| `namespace N{ template<class T> struct B{T v;}; } N::B<int> x;` | rc=0 | rc=0，但**绑的是全局 B**（剥尾归一） | 静默算错 |
| `namespace N{…B…} namespace M{…B…} N::B<int> x; M::B<int> y;` | rc=0（两个实例） | **撞成同一个实例 `B_int`** ⇒ `No member 'w'` | 撞实例 |
| `namespace A{ namespace B{ int f(); } } A::B::f();` | rc=0 | **注册成 `B::f`** ⇒ `Undefined variable 'A::B::f'` | 丢外层 |

三处症状、同一个根因：**名字的作用域在 minicc 里不是身份的一部分**。

## 2. 范围

### 2.1 本阶段做（4 项）

| # | 事项 | 落点 |
|---|---|---|
| S2-1 | **命名空间成员前缀化用完整命名空间路径**（不再是"只取本层名字"） | `processNamespaceDecl` |
| S2-2 | **类模板 / 别名模板按作用域注册**（蓝图名带 `M::`），实例名/缓存键/mangling 随之带作用域 | `processTemplateDecl` + 实例化链 |
| S2-3 | **限定名模板 id 不再剥尾归一**：按作用域精确绑定；查不到即报错 | `resolveType` |
| S2-4 | **登记表退场一处**：Sema 侧的 `classifyQualified` 消费点删除（不再有第二份作用域事实） | `semantic_analyzer.cpp` |

### 2.2 本阶段不做（明确的边界）

1. **不做** `using namespace N;`（本项目不支持该语法 ⇒ 无"非限定名回退到命名空间"这一路）。
2. **不做**函数模板的作用域化（`N::f<int>(x)` 仍不支持；函数模板的候选集按裸名管理）。
3. **不做**嵌套类（`struct A { struct B; };`）。
4. **不做**模板模板实参的跨作用域匹配。
5. **不改**解析期"名字是不是模板"这一问（仍用登记表的模板位作过渡近似）——
   那是 Stage 3 的遗留（见 §5）。

## 3. 判据与规则

### 3.1 作用域前缀化（单点）

规则：**当前命名空间路径 + "::" + 裸名**。

```
namespace A { namespace B { int f(); } }   ⇒  "A::B::f"
namespace M { struct B {}; }              ⇒  "M::B"
```

★ 现状用 `decl->name`（**只取本层**）⇒ 嵌套时丢外层。改用 `m_currentNamespace`
（进入命名空间时已拼好的完整路径）。

### 3.2 模板按作用域注册

- 注册键 = §3.1 的前缀化名字（`m_classTemplates["M::B"]` / `m_aliasTemplates["M::B"]`）；
- 蓝图名同步前缀化（`classTemplate->name = "M::B"`）⇒ **实例名随之带作用域**
  （`M::B_int`）⇒ 汇编符号 `M__B_int`（`asmSymbol` 把 `::` 洗成 `__`）；
- 实例缓存键 = 使用点的限定名（`losslessArgumentsKey("M::B", …)`）⇒ 天然带作用域；
- mangling：`mangleTemplateInstance` 对名字先过 `symbolSafe`（与 `mangleRTTI`/
  `mangleVTable` 同一份净化），长度前缀按净化后的名字算 ⇒ `_Z4M__BIiE`。

### 3.3 限定名模板 id 的绑定

```
"N::B"  →  查 m_classTemplates / m_aliasTemplates（键即完整限定名）
           命中 ⇒ 用它（不再剥尾）
           未命中 ⇒ error: no template named 'B' in namespace 'N'   ← 与 clang 逐字
```

★ **行为收紧**（原来"剥尾归一到全局蓝图"是宽方向）：`N::B` 现在只在 N 里真有 B 时
才合法。clang 同（`N::B` 不会退到全局 `B`）。

### 3.4 登记表退场（Sema 侧）

`SemanticAnalyzer::m_names` 与 `setNameRegistry` 一并删除 —— Sema 不再需要第二份
名字/作用域事实。登记表（NameRegistry）**只服务 Parser 侧**的候选过滤，
定位为**过渡态**（红线 1 的措辞）。

## 4. 验收

| # | 用例 | 期望 |
|---|---|---|
| 1 | `namespace N{…B…} namespace M{…B…}` 两处 `N::B<int>` / `M::B<int>` | 两个实例，字段布局各自正确；rc=0，与 clang 对拍 |
| 2 | `N::B<int>`（N 里确有 B）+ 全局同名 `::B<int>` | 两个实例，符号不同 |
| 3 | `namespace N{}`（空）里的 `N::B<int>` | rc=1，`no template named 'B' in namespace 'N'`（与 clang 逐字） |
| 4 | `namespace A{ namespace B{ int f(); } }` + `A::B::f()` | rc=0，运行值正确 |
| 5 | 既有集成用例 | 逐字节零漂移（除 §6 声明的漂移） |

**突变验证**（红线 7）：
① 模板注册不前缀化 ⇒ 用例 1/2 红；
② 限定名仍剥尾 ⇒ 用例 3 红（不报错）；
③ 前缀化只用本层名字 ⇒ 用例 4 红。

## 5. 与 Stage 3 的关系

Stage 3 = clang 式「边解析边 `ActOn*` 登记」。届时 Parser 侧的
「名字是不是模板」这一问将由 Sema 的活符号表回答，登记表整体退场。
本阶段不动那一问 —— 它需要的是架构改造，不是判据调整。

## 6. 有意日志漂移（红线 4：显式列出）

| 文件 | 差异 | 原因 |
|---|---|---|
| `tests/lang/test_basics_23_name_registry.cpp` | `[resolve] 限定模板名 'M::B' ⇒ 裸名 'B'` 一行**消失**；`[instantiate:name] 'B<int>' → 'B_int'` 变成 `'M::B<int>' → 'M::B_int'`；汇编里的 `B_int` 符号变成 `M__B_int` | 剥尾归一取消（§3.3）+ 实例名带作用域（§3.2） |
| 新增集成用例 | 入列 | 本批新增 |
