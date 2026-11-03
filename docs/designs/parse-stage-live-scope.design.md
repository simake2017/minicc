# 技术设计：解析期活作用域表（Stage 3 批 1~3 —— Actions 句柄、作用域链、三个消歧点切表）

> 对应源码：`include/name_scope.h`（跨域契约）、`src/sema/parse_scope.h/.cpp`（活作用域表）、
> `src/sema/semantic_analyzer.h/.cpp`（Sema 持有 + 窄接口）、`src/parse/parser.h/.cpp`（句柄 + 登记点）、
> `src/driver/main.cpp`（构造顺序）。
> **本项目按 clang 目录组织，不采用 skill 的"1 设计 = 1 flow 文件"** —— 用户 2026-10-23 拍板，
> 见 `docs/constraints/00-全局约束.md` 头部。
> 来源：`../specs/parse-stage-live-scope.spec.md`（2026-11-02 签字）。
> 标准：不看 spec 也能照本文实现。
> 审法：**表住在 Sema、判据只写一处、Parser 不持任何作用域数据** —— 三条任破即缺陷。
> 读者：改 Parser / Sema 的名字查找、消歧、作用域的人。
> 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 首版：批 1 的表结构、Sema 窄接口、Parser 登记点、clang 对照 |

## 1. spec → 设计映射表

| spec 条款 | 设计落点 |
|---|---|
| §1.1 实体（作用域 / 名字条目 / 路径 / 活表 / 持久上下文） | `ParseScopeTable::Scope` / `NameInfo` / `Scope::path` / `m_parseScopes` / `m_contexts` |
| §1.2 状态枚举 `ScopeKind` | `include/name_scope.h:ScopeKind` |
| §1.3 枚举值对象 `ScopeBit` | `include/name_scope.h:ScopeBit` |
| §2.1 S3-1 活表 + Actions 句柄 | 子流程 3.1 表结构、3.3 Sema 窄接口、3.4 Parser 登记点 |
| §2.1 S3-2 登记点铺满 | 子流程 3.4（11 类登记点清单） |
| §2.2 批 1 行为中立 | §6「本批不做的事」（消歧点仍读登记表） |
| §3 异常处理 1~4 | 子流程 3.2（空句柄空转）、3.5（持久化判据） |
| §4 Out of Scope 1~5 | §7 裁量区清单（嵌套类留给下一批） |
| §5 观测点（零新日志） | §8（批 1 刻意零日志） |

## 2. 整体流程图

```text
        ┌──────────────────────────────┐
        │ 构造 SemanticAnalyzer（零输出）│
        │  └ m_parseScopes = {Global}   │
        └───────────────┬──────────────┘
                        │  &sema
                        ▼
┌───────────────────────────────────────────────────────────────┐
│ Parser 解析（递归下降）                                        │
│                                                               │
│  parseNamespaceDecl ──► openScope(Namespace,"N",Namespace)    │
│        │                        │                            │
│        │                   ┌────▼─────┐  declareName(S,Type) │
│        │                   │ "N::" 层 │◄─────────────────────┤
│        │                   └────┬─────┘                      │
│        │                        │ closeScope() → 持久化到      │
│        │                        ▼                            │
│        │                   m_contexts["N::"]                 │
│                                                               │
│  parseClassDecl    ──► openScope(Class,"C",Type[|Template])   │
│  parseTemplateDecl ──► openScope(TemplateParam,"",0)          │
│  parseFunctionDecl ──► openScope(Function,"f",Value)          │
│  parseBlockStmt    ──► openScope(Block,"",0)                  │
└───────────────────────────────────────────────────────────────┘
```

## 3. 子流程 + 说人话步骤

### 3.1 表结构（`src/sema/parse_scope.h`）

**说人话**：一张"作用域链"，每层是一个小抽屉，抽屉里放着"这层声明过的名字 →
它是什么"。链底是全局作用域。旁边另有一张"长期档案柜"（`m_contexts`），
记着**已经离开**的命名空间 / 类里的名字 —— 因为 `N::S` 在离开 N 之后还得能查到。

```cpp
struct NameInfo {
    unsigned    bits = 0;        // 种类位（Type/Namespace/Template/Value 的并集）
    std::string scopePath;       // 非空 ⇒ 它引入一层作用域（namespace / class）
};
struct Scope {
    ScopeKind kind;
    std::string path;                               // "N::C::"
    std::unordered_map<std::string, NameInfo> names; // 裸名 → 条目
};
std::vector<Scope> m_stack;                          // 活链
std::unordered_map<std::string, std::unordered_map<std::string, NameInfo>>
    m_contexts;                                      // 持久上下文（路径 → 名字）
```

### 3.2 三条判据（各写一处）

| # | 判据 | 落点 | 说人话 |
|---|---|---|---|
| ① | 内层遮蔽外层 | `classify()` 沿链自内向外、首命中即返回 | 局部变量 `x` 盖住全局的 `x` |
| ② | 限定名不外溢 | `classifyQualified()` 在限定者那层查，查不到即无 | `N::B` 里 N 没有 B 就是没有，不退到全局 B |
| ③ | 作用域名字可持久 | `leave()` 把命名空间 / 类的名字并进 `m_contexts` | 出了 N，`N::S` 仍要查得到 |

★ ③ 与 ①② **不是一回事**：①② 看**活链**（可见性），③ 看**持久上下文**
（限定可达）—— 一个命名空间里的类型在离开该命名空间后，依然能用 `N::T` 指到。

### 3.3 Sema 侧的窄接口（`src/sema/semantic_analyzer.h`）

```cpp
void enterParseScope(ScopeKind kind, const std::string& name);
void leaveParseScope();
void declareParseName(const std::string& name, unsigned bits);
std::string openParseScope(ScopeKind kind, const std::string& name, unsigned bits);
unsigned classifyParseName(const std::string& name) const;
unsigned classifyParseQualified(const std::string& qualified) const;
```

**说人话**：六条，全是转发 —— 判据住在表里，这层薄到"只转调"是刻意的。
★ 若哪天这层开始长出 `if` 分支，说明判据跑偏了，该修的是表而不是这层。

### 3.4 Parser 侧的登记点（`src/parse/parser.cpp`）

Parser 只持 `SemanticAnalyzer*`（**前向声明**，clang 同款：`clang/include/clang/Parse/Parser.h:639`
的 `Sema &Actions;`），三个助手全部带空句柄守卫：

```cpp
void Parser::declareName(const std::string& name, unsigned bits) {
    if (!m_sema) return;                     // 单测自建 Parser ⇒ 空转
    m_sema->declareParseName(name, bits);
}
```

**登记点清单（11 类）**

| # | 声明形状 | 登记什么 | 落点 |
|---|---|---|---|
| 1 | `namespace N {…}` | N → Namespace 位；**开作用域** | `parseNamespaceDecl` |
| 2 | `struct/class C {…}` | C → Type 位（模板再加 Template 位）；**开作用域** | `parseClassDecl` |
| 3 | `struct C;`（前置声明） | C → Type 位；**不开作用域**（没有类体） | `parseClassDecl` |
| 4 | `enum E {…}` | E → Type 位；枚举项 → Value 位（**登记进外层**，不限���枚举的枚举项是外层作用域的名字） | `parseEnumDecl` |
| 5 | `using X = T;` / `typedef T X;` | X → Type 位 | `parseTypeAliasDecl` |
| 6 | 类内 `using` / `typedef` | 同上，登记进**类作用域** | `parseClassDecl` |
| 7 | `template<class T>` 形参 | T → Type 位 / NTTP → Value 位 / 模板模板形参 → Template 位 | `parseTemplateDecl` |
| 8 | 模板名（类 / 别名 / 函数 / 指引 / 成员模板） | 追加 Template 位 | `parseTemplateDecl` / `parseClassDecl` |
| 9 | 全局变量 / static 成员 / 字段 | → Value 位 | `parseGlobalVarDecl` / `parseClassDecl` |
| 10 | 函数 / 方法 / 形参 | → Value 位；函数**开作用域** | `parseFunctionDecl` / `parseParameterList` |
| 11 | 局部变量 / 块 / for 隐式作用域 | → Value 位；块**开作用域** | `parseVarDeclStmt` / `parseBlockStmt` / `parseForStmt` |

★ **模板形参作用域是"透明"的**：它只让形参名（`T`）登记在自己这层，
但模板自己的名字（`Box`）登记在**外层** —— 对照 clang：模板的名字挂在外层
`DeclContext`，形参挂在自己的 `TemplateParameterList`。
实现落在 `ParseScopeTable::nameScopeIndex()`（找最近的外层非形参作用域）。

### 3.5 持久化判据（`isPersistent`）

| 作用域种类 | 持久？ | 为什么 |
|---|---|---|
| Namespace | ✅ | `N::S` 在离开 N 之后仍要可达 |
| Class | ✅（**局部类除外**） | `A::B`（嵌套类）要可达；函数 / 块里的类是**局部类**，C++ 里没有 `f::Local` 这种限定名 |
| Function / Block / TemplateParam | ❌ | 局部声明不该被限定名查到 |

### 3.6 三个消歧点的新判据（批 2+3）

三个消歧点从"查前向扫描猜出来的登记表"改成"问活作用域表"，判据各写一处：

| 消歧点 | 位置 | 旧判据（登记表） | 新判据（活表） |
|---|---|---|---|
| 语句层裸名 `S;` | `parser.cpp` 的 `parseStatement` | `classify(name).isType`（跨作用域并集 ⇒ **看不见遮蔽**） | `!(bits & Value)` ⇒ 声明候选（可见性交 Sema 裁决） |
| 语句层模板 id 链闸 | 同上 | `isTemplateName(基名)` | `isTemplateNameInTemplateId(基名)`：有模板位 ∨ 查不到（C++20 假定） |
| 表达式模板闸 `f<T>(x)` | `parser.cpp` 的 `parsePrimaryExpr` | `isTemplateName(name)` | `isTemplateNameHere(name)`：有模板位 ∨ **查不到且是裸名且下一个 token 是 `<`** |

★ 三条判据的**分歧点**只有一处：C++20 假定在表达式闸**限定名不适用**
（clang 的 `SS.isEmpty()`），在语句层链闸**适用**（那里问的是"是不是模板 id 语法"，
不是"是不是模板名"；若不适用，`N::B<int> x;` 会退化成比较链，
丢掉与 clang 逐字一致的 "no template named 'B' in namespace 'N'"）。

## 4. 异常流转表

| 异常 | 触发点 | 处理 |
|---|---|---|
| 解析中途 `ParseError` | 任意声明点 | 作用域链停在半开层 —— 整个编译单元中止，表随之丢弃，**无影响** |
| Parser 无 Sema 句柄 | 显式置空（正常路径不会发生：Parser 自带 Sema） | 三个助手全部空转（`if (!m_sema) return;`）—— 行为退化为"没有这张表" |
| `leave()` 时链只剩 Global | 逻辑错误 | 直接返回（Global 不可弹出） |
| 限定名查不到 | `classifyQualified` | 返回 0；**由宽改严的方向由调用方决定**（消歧点据此走"不是模板"） |

## 5. 状态机 / 持久状态 / 空值语义

- **状态机**：`m_stack` 是一条栈；`enter` 压、`leave` 弹。Global 是底座，不可弹。
- **持久状态**：无 —— 活作用域表只在一次编译内有效。
- **空值语义**：`classify` 返回 **0 = 未声明**（不是"假名字"）；位为 0 的名字
  等价于没登记过（`isDeclared` 判 `!= 0`）。

## 6. 目录落点 + 数据形状

```text
include/name_scope.h            跨域契约：ScopeKind + ScopeBit
src/sema/parse_scope.h          活作用域表（声明）
src/sema/parse_scope.cpp        活作用域表（实现）
src/sema/semantic_analyzer.h    Sema 持有 m_parseScopes + 六条窄接口
src/parse/parser.h              SemanticAnalyzer* 句柄 + 三个助手声明
src/parse/parser.cpp            助手实现 + 11 类登记点
src/driver/main.cpp             Sema 提前构造 + parser.setSema(&semaAnalyzer)
tests/unit/test_parse_scope.cpp 12 例单测
```

**数据形状（名字条目）**

```jsonc
{
  "bits": 5,                 // Type(1) | Template(4)
  "scopePath": "N::C::"     // 非空 ⇒ 该名字引入作用域
}
```

## 7. 裁量区清单

| AI 可以自决 | 必须问人 |
|---|---|
| 登记点的**具体位置**（在声明点后、在作用域内） | ~~三个消歧点何时切到活作用域表~~ ⇒ **批 2+3 已切**，口径按 clang（诊断文案对齐 + 基线重刷） |
| 持久化判据的细节（局部类不持久） | ~~C++20 规则是否照 clang 逐字实现~~ ⇒ **已定：照 clang**（裸名限定，`SS.isEmpty()`） |
| 表的内部表示（`unordered_map` vs `map`） | ~~是否顺带把 "Undefined variable" 改成 clang 文案~~ ⇒ **已做**（裸名改；限定名待做，见 learn/52 §7） |
| 单测的用例切分 | 嵌套类是否用**同一张作用域链**解决 ⇒ **用户已拍板：要"正统方式"，不做特判**（批 4） |
| 限定名的 C++20 假定要不要做 | clang 的 `SS.isEmpty()` 说明**不做** —— 但语句层的模板 id 链闸仍对限定名放行（那里问的是"是不是模板 id 语法"，不是"是不是模板名"） |

## 8. 与 clang 的对照

| clang | minicc | 说明 |
|---|---|---|
| `clang/include/clang/Sema/Scope.h:Scope` | `ParseScopeTable::Scope` | 作用域节点 + parent 链 |
| `clang/lib/Sema/SemaDecl.cpp:1574:Sema::PushOnScopeChains` | `ParseScopeTable::declare` / `openScope` | 名字入作用域 |
| `clang/lib/Parse/Parser.cpp:427/437:Parser::EnterScope/ExitScope` | `Parser::openScope` / `closeScope` | 作用域进出 |
| `clang/include/clang/Parse/Parser.h:639:Sema &Actions` | `Parser::m_sema`（`SemanticAnalyzer*`） | Parser 持 Sema 句柄 |
| `clang/lib/Sema/SemaDecl.cpp:912:Sema::ClassifyName` | `SemanticAnalyzer::classifyParseName` / `classifyParseQualified` | 问名字的种类（批 3 已接上三个消歧点） |
| `clang/lib/Sema/SemaDecl.cpp:996-1003`（C++20 `UndeclaredTemplate`） | `Parser::isTemplateNameHere` | 名字查不到 + 下一个 token 是 `<` ⇒ 假定是模板名（批 2 已做，**裸名限定**，照 clang 的 `SS.isEmpty()`） |
| `clang/lib/Parse/ParseTentative.cpp:1957:Parser::isTemplateArgumentList` | `Parser::parseTemplateArgumentList` + `TokenCheckpoint` | 复核：解析不出实参表 ⇒ 回滚成比较运算 |
