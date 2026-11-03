# SPEC：解析期活作用域表（Stage 3 —— Parser 持 Sema 句柄，名字的种类与可见性由 Sema 裁决）

状态：已签字（2026-11-02）
签字：2026-11-02（用户拍板：「先基于当前代码，往 clang 正规方向去靠拢，然后用正统方式解决嵌套那些问题」）
来源：docs/BUGS.md **B38** 遗留项（Stage 3）＋ docs/learn/48 Stage 1 拍板表 ＋ docs/constraints/00-全局约束.md 红线 1（改写版）

> 阶段：**Stage 3**（红线 1 改写后的三阶段之末；承 Stage 1 = B37、Stage 2 = B38）
> 关联缺陷：docs/BUGS.md **B38** 遗留（Stage 3）＋ **B39**（符号净化，本批顺带修）
> 关联学习产物：docs/learn/51-parse-stage-live-scope.md

- 读者：改 Parser / Sema 的名字查找、消歧、作用域的人
- 审法：**这个名字的"种类"与"此处可见吗"是不是同一个答案？** 答案必须是"是" ——
  同问两答（登记表答前者、Sema 答后者）就是缺陷
- 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 首版：Stage 3 三批的边界与验收；批 1 已落地（活作用域表 + Actions 句柄 + 登记点） |

## 1. 概念

### 1.1 实体

| 实体 | 是什么 | 住在哪 |
|---|---|---|
| **作用域**（Scope） | 一层名字的容器：命名空间 / 类 / 函数 / 块 / 模板形参 | `ParseScopeTable` 的链 |
| **名字条目** | 一个名字 + 它的**种类位**（type / namespace / template / value）+（若它引入作用域）**作用域路径** | `ParseScopeTable::NameInfo` |
| **作用域路径** | 限定名解析的定位串（`N::C::`）；Global 层为空串 | `ParseScopeTable::Scope::path` |
| **活作用域表** | 解析期那一张作用域链 | `SemanticAnalyzer::m_parseScopes` |
| **持久上下文** | 离开作用域后仍能被限定名查到的那部分（clang 的 DeclContext） | `ParseScopeTable::m_contexts` |

### 1.2 状态枚举

| 状态 | 含义 |
|---|---|
| `ScopeKind::Global` | 全局作用域（表的底座） |
| `ScopeKind::Namespace` | 命名空间（**引入路径成分**、**持久**） |
| `ScopeKind::Class` | 类（**引入路径成分**、**持久**（局部类除外）） |
| `ScopeKind::Function` | 函数（引入路径成分，**不持久**） |
| `ScopeKind::Block` | 块 / for 的隐式作用域（**不引入路径成分**、**不持久**） |
| `ScopeKind::TemplateParam` | 模板形参域（**不引入路径成分**、**不持久**、对名字登记**透明**） |

### 1.3 枚举值对象

| 位 | 含义 | 谁会置它 |
|---|---|---|
| `ScopeBit::Type` | 类 / 结构体 / 枚举 / typedef / using 别名 | 类名、枚举名、别名名 |
| `ScopeBit::Namespace` | 命名空间名 | 命名空间名 |
| `ScopeBit::Template` | 模板名（类 / 别名 / 函数模板、推导指引、成员模板、模板模板形参） | 模板名、指引名、成员模板名 |
| `ScopeBit::Value` | 值名（变量 / 形参 / 函数 / 枚举项 / static 成员） | 变量名、形参名、函数名、枚举项名 |

★ 一枚名字可以同时是多种：**类模板名既是类型名也是模板名** ⇒ 位并集表达。

### 1.4 名词

- **登记**（declare）：把一个名字（+ 种类位）放进**当前**作用域。
- **开作用域**（openScope）：登记一个"引入作用域的名字"并进入新作用域，一步做完
  （clang 的 `PushOnScopeChains` + `EnterScope` 两步合一）。
- **分类**（classify）：沿作用域链自内向外，**首命中即返回**（内层遮蔽外层）。
- **限定名分类**（classifyQualified）：限定者逐段定位，末段只在限定者那层查
  （**不外溢**）。

## 2. 正常流程

```text
┌──────────────────────────────────────────────────────────────┐
│ 阶段 2 之前：构造 SemanticAnalyzer（无输出）                  │
│   → SemanticAnalyzer::m_parseScopes（底座 = Global 作用域）  │
└───────────────┬──────────────────────────────────────────────┘
                │
┌───────────────▼──────────────────────────────────────────────┐
│ 阶段 2：Parser 解析                                          │
│   parser.setSema(&semaAnalyzer)   ← Actions 句柄就位        │
│                                                               │
│   声明点 ──► declareName(name, bits)      （登记）            │
│   作用域 ──► openScope(kind, name, bits) （开作用域）         │
│           ──► closeScope()               （出作用域）         │
└───────────────┬──────────────────────────────────────────────┘
                │
                ▼
        消歧点（批 2/3 才接上）：问 Sema「这个名字在此处是什么」
```

### 2.1 本阶段做（三批，逐批可独立验收）

| # | 事项 | 批次 | 状态 |
|---|---|---|---|
| S3-1 | **活作用域表**（`ParseScopeTable`）+ Sema 持有 + Parser 持 `SemanticAnalyzer*` 句柄（clang 的 `Sema &Actions`） | 批 1 | ✅ 已完成 |
| S3-2 | **登记点铺满**（命名空间 / 类 / 枚举 / 别名 / 模板 / 形参 / 变量 / 函数 / 成员 / 块 / for 隐式作用域） | 批 1 | ✅ 已完成 |
| S3-3 | **value 位落地**（不再是占位恒 false）+ **C++20 `UndeclaredTemplate` 规则**（未声明的名字按 C++20 当模板名） | 批 2 | ⬜ 未做 |
| S3-4 | **三个消歧点切到活作用域表** + **登记表整类删除**（`NameRegistry` 退场） | 批 3 | ⬜ 未做 |

### 2.2 批 1 的行为约束（★ 本批必须行为中立）

批 1 只把**机制**接通，**不改任何消歧结论** —— 三个消歧点（`parser.cpp` 的语句层
裸名判定 / 语句层模板 id 链闸 / 表达式模板闸）仍读 `NameRegistry`。
⇒ build 0 警告 / ctest 全绿 / **集成日志逐字节零漂移** 三绿是本批的验收线。

### 2.5 更新对照

本主题是**只读**（不写任何持久化数据）：活作用域表只在一次编译内有效，
进程结束即销毁。

## 3. 异常处理

| # | 异常 | 如何处理 |
|---|---|---|
| 1 | 解析中途抛 `ParseError`（语法错） | 作用域链可能停在半开的层 —— **不影响正确性**：整个编译单元中止，表随之丢弃。**不可重试** |
| 2 | Parser 未挂 Sema 句柄（单测自建 Parser） | 登记 / 查询一律**空转**（`m_sema == nullptr` 守卫）—— 解析行为退化为"没有这张表"的样子。**可重试**（挂上句柄即可） |
| 3 | `leave()` 时链只剩 Global | 直接返回（Global 不可弹出）—— 防栈空。**不可重试** |
| 4 | 限定名查不到 | 返回 0（未声明）—— **由宽改严**的方向由调用方决定（消歧点据此走"不是模板"）。**可重试** |

## 4. Out of Scope

1. **不做** `using namespace N;`（本项目无此语法 ⇒ 无"非限定名回退到命名空间"这一路）。
2. **不做**嵌套类（`struct A { struct B; };`）—— ★ 这是 Stage 3 之后**下一批**的正题
   （用同一张作用域链的通用查询解决，不做特判）。
3. **不做**局部类（函数里的 `struct L{};`）的限定名可达性（本来就不该有）。
4. **不做**重载函数的**形参类型**参与身份（当前只登记名字与种类位；身份仍靠
   `memberMethodSymbolName` 的类型链）。
5. **不做**把活作用域表与语义期符号表合并（clang 也是两张：`Scope` 临时、
   `DeclContext` 持久）。

## 5. 观测点

| 键 | 何时打 | 说明 |
|---|---|---|
| （本批无新日志） | — | ★ 批 1 **刻意零日志**：行为中立是硬约束，新增日志会让 160 份集成基线全漂。消歧点切表时（批 3）再补"这个名字是什么/为什么不是模板"的解释行 |

## 附：实施记录（批 1）

**落点**

| 文件 | 角色 |
|---|---|
| `include/name_scope.h`（新） | 跨域契约：`ScopeKind` 与 `ScopeBit` 两枚编码（parse ⇄ sema 共用一份） |
| `src/sema/parse_scope.h/.cpp`（新） | 活作用域表：进出、登记、分类（裸名 / 限定名）、持久上下文 |
| `src/sema/semantic_analyzer.h/.cpp` | Sema 持有表 + 六条窄接口（`enterParseScope` / `leaveParseScope` / `declareParseName` / `openParseScope` / `classifyParseName` / `classifyParseQualified`） |
| `src/parse/parser.h/.cpp` | Parser 持 `SemanticAnalyzer*`（前向声明，clang 同款）+ 三个助手（`declareName` / `openScope` / `closeScope`，空句柄空转） |
| `src/driver/main.cpp` | Sema 提前到阶段 2 之前构造 + `parser.setSema(...)` |
| `tests/unit/test_parse_scope.cpp`（新） | 12 例：链 / 遮蔽 / 可见性 / 限定名 / 持久 / 透明 / 局部类 / 位并存 |

**验收数据**

- build：0 警告；
- ctest：376 → **388**（+12，全绿）；
- logdiff：160 份集成用例**逐字节零漂移**（唯一差异是 `_unit_cases.txt` 多 12 行、
  `_unit_summary.txt` 的 376 ⇒ 388）；
- 注释守卫 `check-comment-style.sh`：✅ 全达标（5 处裸锚点已按 clang 语料补全路径）。

**登记点清单（批 1 铺的 11 类）**：命名空间 / 类（含模板与前置声明）/ 枚举名 /
枚举项 / 类型别名（顶层与类内）/ 模板形参（类型 · NTTP · 模板模板）/ 模板名
（类 · 别名 · 函数 · 指引 · 成员模板）/ 全局变量 / 函数与方法 / 形参 / 局部变量 /
块与 for 的隐式作用域。

**下一步（批 2 / 批 3）**：value 位与 C++20 `UndeclaredTemplate` 规则落地 ⇒
三个消歧点切到活作用域表 ⇒ `NameRegistry` 整类删除。
