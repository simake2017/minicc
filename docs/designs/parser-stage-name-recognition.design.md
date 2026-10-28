# 技术设计：parser 阶段名字识别

> 对应源码：`src/name_registry.cpp`（收集器+查询）＋ 4 处消费点
> （`src/parser.cpp` 三处、`src/semantic_analyzer.cpp` 一处）＋ 接线
> （`src/main.cpp`）。按项目约束（00-全局约束.md 头部拍板）：结构对齐
> clang，不建 flow 聚合文件。
> 来源：`docs/specs/parser-stage-name-recognition.spec.md`（已签字 2026-10-23）。
> 标准：不看 spec 也能照本文实现。
> 审法：每个消费点问一句——「查不到时走哪条支，会不会静默」。

## 1. spec → 设计映射表

| spec 条款 | 设计落点 |
|---|---|
| §1.1 实体登记表（落点拍板 2） | 新模块：`include/name_registry.h`（契约）+ `src/name_registry.cpp`（实现）；数据形状见 §6 |
| §1.2 分类位四穷举（value 占位，拍板 1） | `NameClassification` 四 bool 结构体；收集器只置 type/namespace/template 三位 |
| §2 步骤 1 收集（前向扫描、零日志、declval 种子） | 子流程 3.1 `NameRegistry::collect()`（parser.cpp:78-120 现有逻辑搬迁+扩展） |
| §2 步骤 2 语句层裸名判定（新增消费点） | 子流程 3.2 `parseStatement` 试探块（parser.cpp:2282-2291 的 `else if` 内新增分支） |
| §2 步骤 3 表达式模板闸（换入口） | 子流程 3.3 `parsePrimaryExpr`（parser.cpp:3113，`isKnownTemplateName` → `m_names->isTemplateName`） |
| §2 步骤 4 Sema 限定名裁决（N::B 作用域链） | 子流程 3.4 `resolveType` 模板 id 分支（semantic_analyzer.cpp:920-935 尾剥改裁决） |
| §2 步骤 5 诊断文案对齐 | semantic_analyzer.cpp:1156 错误括注改写（有意漂移，申报重刷基线） |
| §2.5 更新对照实例 A | 3.2 新增分支产 EmptyStmt + `[parse:stmt]` 日志行 |
| §2.5 更新对照实例 B | 3.4 裁决失败报错 rc=1；真存在 N::B 的用例走命中路径零漂移 |
| 异常 1 查不到 → 全位 false | 查询契约：`classify()` 未命中返回四 false 的默认对象；三个消费点全部只在「位命中」时走新支 |
| 异常 2 同名跨类并集 | 收集器按 (scope,name) 累加位（OR）；3.2 只看 `isType`，不看 isValue |
| 异常 3 误收 → 试探回退兜底 | 3.3 维持 TokenCheckpoint 试探不动；登记只决定「要不要试」 |
| 异常 4 N::B 不存在 | 3.4 裁决失败报「no template named 'B' in namespace 'N'」 |
| 异常 5 只允许实例 B 形态漂移 | §4 异常表 + 验收命令（grep 核实 + logdiff 零漂移） |
| §4 OOS 1-7 | 不设计对应机制（无值名收集、无遮蔽裁决、无 using 拉入、无 did-you-mean、无 multi-declarator、无 S() 消费点） |
| §5 日志键 | 3.2 的 `[parse:stmt]` 行；其余消费点维持零日志 |

## 2. 整体流程图

```text
main.cpp 接线
    │
    ▼
建登记表（前向扫一遍 token，写一次，之后只读）        ◄── 3.1
    │
    ▼
语法分析（Parser 全程只读查询）
    │
    ├─ 语句层前瞻 ── 模板 id 链任一层不在表 ──► 推翻声明解释，走表达式（现状，3.2/3.3 换入口）
    │                │
    │                ▼
    │           类型收尾无声明符 ── type 位命中 ──► 声明解释 + [parse:stmt] 日志（新增，3.2）
    │                │
    │                └─ type 位未命中 ──► 表达式解释（现状）
    │
    └─ 表达式主部 ── template 位命中 ──► 进 <...> 试探（现状，3.3）
                     │
                     └─ 未命中 ──► 比较运算（现状）
    │
    ▼
语义分析（Sema 只读查询）
    │
    └─ 限定模板名 N::B<...> ── 作用域链配不上 ──► 报错「no template named 'B' in
                                                  namespace 'N'」rc=1（新增，3.4）
                     │
                     └─ 命中 ──► 归一裸名，照常实例化（现状，零漂移）
```

## 3. 子流程图 + 业务说明

### 3.1 收集（对应源码 `NameRegistry::collect()`）

**流程图**：

```text
→ 播种子名单（declval）→ 逐 token 扫描 →
    ├─ 遇 template 头 ⇒ 跳过形参表 ⇒ 按形态登记 template 位（B31 现状原样搬迁）
    ├─ 遇 struct/class/enum 声明 ⇒ 登记 type 位
    ├─ 遇 typedef ... ; ⇒ 登记 ';' 前最后一个标识符为 type 位
    ├─ 遇 using X = ... ; ⇒ 登记 X 为 type 位
    ├─ 遇 namespace X { ⇒ 登记 X 为 namespace 位 + 推入作用域栈
    ├─ 遇 namespace A = B ; ⇒ 登记 A 为 namespace 位（命名空间别名）
    └─ 遇与栈顶配对的花括号 ⇒ 弹出作用域栈
→ 结束（全程零日志）
```

**业务说明**：
① 播种子名单：内建的 declval 不在 token 流里，直接写入（漏收的症状是
test_tmpl_35..39 当场 rc=1，B31 实证）。std::void_t 不收（只在类型位置按名识别）。
② template 头扫描逻辑**原样搬迁** B31（parser.cpp:78-117）：struct/class/using 取
紧跟的标识符；其它形态取形参表后第一个「标识符 + '('」；循环不跳过已处理区间
（模板模板形参的内层头照常收）。
③ 作用域栈：遇 `namespace X {` 记下【X + 花括号深度】；扫描中每遇 `{` 深度 +1、
`}` 深度 -1，深度回落到记录值即弹出 X。类体/函数体的 `{}` 只改深度、不入栈
（类内名字归属其外层命名空间——局部类误登记为全局是宽方向，见异常表恢复路径）。
④ typedef 近似：取 `typedef` 起、`;` 止区间内最后一个 Identifier（跳过 `*`、`&`、
`(`、`)`）——`typedef unsigned long ulong;` 收 ulong。函数指针 typedef 收名字即可，
形态细节不裁决（B31 纪律：只收集不验证）。
⑤ 重复声明/先使用后声明：同一 (scope,name) 已存在 ⇒ 位 OR 累加，不覆盖。

### 3.2 语句层裸名判定（对应源码 `parseStatement` 试探块内新增分支）

**流程图**：

```text
（B35 试探已成立，type 解析成功，下一个 token 是 ';'）
    │
    ▼
类型带模板 id 语法？ ──是──► 现有 B16 判定（isDeclaratorlessDecl，不动）
    │否
    ▼
查登记表 type 位 ──命中──► 判为声明：commit 试探 → 消费 ';' → 产 EmptyStmt
    │                        + 打 [parse:stmt] 日志
    │否
    ▼
维持现状：不 commit（纯试探回卷）→ 表达式路径
```

**业务说明**：
① 触发位置：parser.cpp:2282 的 `else if (check(TokenType::Semicolon))` 分支内、
现有 `hasTemplIdSyntax` 判定为否时（该处现注释「裸 S; 刻意不算」整段由本分支取代）。
② 判定只问 type 位一个 bit；查不到（条目不存在）= 位未命中 = 走表达式，**禁止**
反向推断（spec 异常 1）。
③ 限定名 `N::S;` 一并覆盖：查询走与 B31 相同的「全名或 :: 末段」近似（Parser 侧
不问作用域——语句层的宽方向与 B31 一致）。
④ 日志在 commit 之后打（正式路径），格式：
`[parse:stmt] '{}' 判为声明（type 位）\n`——与 parse:type / parse:targ 同族，键名
已拍板。
⑤ `int S = 5;` 与 `struct S {};` 并存时 `S;` 判声明（spec 异常 2，[stmt.ambig]
声明优先，clang 同）。

### 3.3 表达式模板闸（对应源码 `parsePrimaryExpr`）

**业务说明**：仅换查询入口：`isKnownTemplateName(name)` → `m_names->isTemplateName(name)`
（内部 = `classify(name).isTemplate`，全名/末段近似与 B31 逐字节等价）。试探、回退、
日志全部不动 ⇒ 本消费点零漂移由等价性保证，不靠测试撞。

### 3.4 Sema 限定名裁决（对应源码 `resolveType` 模板 id 分支）

**流程图**：

```text
限定模板名 N::B<...>，蓝图表（别名/类模板）按全名查不到
    │
    ▼
拆 scope="N" name="B" ──► 精确查登记表：scope 匹配 ∧ name=B ∧ template 位
    │
    ├─ 命中 ──► 打现有 [resolve] 归一日志 → type->name = "B"（现状路径，零漂移）
    │
    └─ 未命中 ──► 报错 "no template named 'B' in namespace 'N'"（rc=1，clang 逐字）
```

**业务说明**：
① 触发位置：semantic_analyzer.cpp 现尾剥分支（约 920-935 行，注释含「宽方向：
N::B 真不存在时会误绑全局 B」处）。
② 拆掉尾剥：`rfind("::")` 取末段查蓝图**全局表**的旧逻辑删除，改为查登记表的
(scope, name, template 位)——登记表带作用域链，全局 B（scope=""）配不上
scope="N"，误绑闭环。
③ 命中路径打原有的 `[resolve] 限定模板名 'N::B' ⇒ 裸名 'B'` 行（basics_22
的 `N::B<Box> y;` 真存在，逐字节零漂移是验收项）。
④ Sema 经 setter 持登记表 const 指针（main.cpp 在 analyze 前注入）；Sema 其余
符号表逻辑一律不动。

## 4. 异常流转表

| 异常N | 触发点（3.x 第 N 步） | 状态/字段变化 | 恢复路径 |
|---|---|---|---|
| 异常1 名字查不到 | 3.2 ③ / 3.3 / 3.4 查询未命中 | 无（登记表只读）；AST 走表达式解释或报错 | 非错；Sema 照常报错兜底，rc=1 |
| 异常2 同名跨类 | 3.1 ⑤ 位 OR 累加；3.2 ⑤ 只看 type 位 | 条目的 bits 字段为并集 | 非错；`S;` 判声明（clang 同） |
| 异常3 登记表误收 | 3.3 试探仍失败 | 试探回卷，无 AST 变化 | ParseError 路径响亮拒收 rc=1（现状机制） |
| 异常4 N::B 不存在 | 3.4 精确查询未命中 | 无实例化副作用（裁决先于实例化） | 报错 rc=1，修源码重编 |
| 异常5 非预期漂移 | 全量 logdiff | 任何 [parse:stmt] 之外的新增/删除行 | 实现错了，打回重做；唯一合法漂移 = 1156 括注改写 + 新测试文件进基线 |

## 5. 空值语义（无状态机，声明不画）

- 查询未命中 = **条目不存在**（不是「存在但 bits 全 false」）——消费点一律
  按「位未命中」处置，禁止反向推断。
- scope 字段空串 = 全局作用域；namespace 别名 A 的条目 scope = A 的外层作用域。
- 登记表生命周期 = 一次编译：collect() 后零写入，Parser/Sema 各持 const 访问。

## 6. 目录落点 + 数据形状

```text
include/name_registry.h      新：NameClassification / NameRegistry 契约（注释钉死 §5 空值语义）
src/name_registry.cpp        新：collect()（parser.cpp:78-120 搬迁+扩展）+ classify()/isTemplateName()
include/parser.h             改：m_templateNames set → const NameRegistry* m_names（含构造参数）
src/parser.cpp               改：删 collectTemplateNames/isKnownTemplateName（搬入新模块）；
                             消费点 3.2（新增分支）/ 3.3（换入口）/ 语句层闸（换入口）
include/semantic_analyzer.h  改：成员 const NameRegistry* m_names + setNameRegistry()
src/semantic_analyzer.cpp    改：3.4 尾剥→裁决；1156 括注改写
src/main.cpp                 改：构造 Parser 前先建 NameRegistry 并 collect；analyze 前注入 Sema
CMakeLists.txt               改：src/name_registry.cpp 入源列表
tests/lang/test_basics_23_name_registry.cpp        新集成（§下）
tests/unit/test_name_registry.cpp                  新单测（gtest）
```

数据形状（与代码类型逐个对应）：

```jsonc
// include/name_registry.h
struct NameClassification {           // 对齐 clang NameClassification 骨架
    bool isType = false;              // class/struct/enum/typedef/using/别名 声明
    bool isNamespace = false;         // namespace X { 或 namespace A = B;
    bool isTemplate = false;          // template 头家族（B31 全集）
    bool isValue = false;             // ★ 恒 false（拍板 1：占位，不参与任何判定）
};
class NameRegistry {
    // 键 = (scope, name)；值 = 位并集。scope 空串 = 全局。
    std::map<std::pair<std::string,std::string>, unsigned> m_entries;
public:
    void collect(const std::vector<Token>& tokens);   // 写一次（3.1 五规则 + declval 种子）
    NameClassification classify(const std::string& name) const;
        // Parser 侧：全名命中，或取 '::' 末段命中（B31 近似原样保留）
    NameClassification classifyQualified(const std::string& scope,
                                         const std::string& name) const;
        // Sema 侧：scope 必须精确相等——3.4 裁决的唯一依据
    bool isTemplateName(const std::string& name) const;  // = classify(name).isTemplate
};
```

clang 对照表：

| clang（~/cppproject/llvm-project/） | 本实现 | 简化了什么 |
|---|---|---|
| `Sema::ClassifyName`（clang/lib/Sema/SemaDecl.cpp:912） | `NameRegistry::classify` | clang 查活符号表（Scope/DeclContext 链）+ 重载/模板特化决议；本实现查构造期只读投影，只回答「哪类」一个 bit |
| `Sema::isTemplateName`（clang/lib/Sema/SemaTemplate.cpp:179） | `NameRegistry::isTemplateName` | 同上：无 TemplateNameKind 细分类（模板/特化/重载集） |
| `TentativeParsingAction`（clang/include/clang/Parse/Parser.h:8479） | `TokenCheckpoint`（learn/45，B32） | 已对齐，本轮不动 |
| `Parser::TryAnnotateTypeToken` | B35 语句层试探（parser.cpp:2273） | 已对齐，本轮只加 type 位消费 |

## 7. 裁量区清单

| AI 可以自决 | 必须问人 |
|---|---|
| m_entries 的具体容器与键编码（pair map / 拼接串） | 若 grep 核实发现既有基线含「裸 `S;`（类型名）」形态（新日志行会漂旧基线）→ 申报后签字重刷 |
| [parse:stmt] 日志行的字段次序与措辞细节 | 异常 4 报错文案终稿（实现前以 clang++-18 探针逐字复核） |
| 单测用例命名与分组 | 1156 括注改写后的新文案 |
| 收集规则的近似粒度（typedef 取末标识符等） | — |

## 8. 现状盘点

| 部分 | 结论 |
|---|---|
| 可复用 | B31 扫描骨架（parser.cpp:78-120）原样搬迁，template 位规则零改动；TokenCheckpoint / B35 试探块 / 尾剥分支 / main 接线位置全部已定位（见 §6） |
| 要改 | 7 文件 + CMakeLists（src/name_registry.cpp 入源）；parser.h 成员换型；semantic_analyzer.h 加 setter；main.cpp 三行接线 |

## 9. 否决记录

| 被否方案 | 否决原因 | 谁否的/什么时候 |
|---|---|---|
| Parser 持 Sema 只读句柄，边解析边问符号表 | 破坏语法期零语义分层（红线 1）；Sema 三趟在解析完成后才开始，活符号表回答不了单趟解析问题；「宁宽勿窄」验收口径失效 | 用户，2026-10-23（Q&A 当场） |
| flow 聚合目录（1 设计 = 1 flow 文件） | 代码结构对齐 clang 即可，不另造目录 | 用户，2026-10-23 |
| Sema 自建第二张名字表 | 同一判据写两处，必不一致（B10/B22 教训，红线 3） | 设计自否，2026-10-23 |
| 语句层 Parser 查询也带作用域链 | 语句层的本地名（局部类型/形参）登记表看不到，精确查询会漏收合法程序；维持 B31「全名/末段」宽近似，误收由试探回退兜底 | 设计自否，2026-10-23 |

## 10. 验证计划（验收命令）

```bash
# ① 收集器规则单测 + 判据突变验证（3 条：摘 type 位判定 / 回退尾剥 / 摘 struct 登记）
ctest --test-dir build-linux -j6 -R NameRegistry

# ② 新集成用例（clang 对拍）+ 既有全量
./minicc tests/lang/test_basics_23_name_registry.cpp -o /tmp/t23 && /tmp/t23   # rc/返回值对拍
ctest --test-dir build-linux -j6                                               # 346+ 全绿

# ③ 基线零漂移（唯一合法漂移：1156 括注 + 新文件入列）
./logdiff.sh diff
grep -rn "^\s*[A-Z_a-z][A-Za-z0-9_:]*\s*;" tests/ --include=*.cpp | grep -v "//"  # 核实裸名语句形态存量
```
