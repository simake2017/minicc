# SPEC：名字可见性裁决下沉语义期（裸类型名声明 + 作用域判据单点）

状态：已签字（2026-11-02）
签字：2026-11-02（用户在会话中拍板"按推荐方式走，尽量贴切 clang"；三项拍定见文末「拍板记录」）
来源：需求原文「能否改成类似 clang 的正规模式，或者一边 parser 一边进行简单语义识别，不需要像后面那么复杂的语义识别」＋ docs/BUGS.md B37（裸类型名声明的可见性无人裁决）＋ 上游技能 biz-dev-pipeline（spec 骨架）
契约变更点：Sema 的**可见性裁决窄接口**（`lookupVisibleType`：类作用域 → 命名空间链逐级外层 → 全局）＋ `include/ast.h` 的 `EmptyStmt::typeName` 槽（裸名从语法层带到语义期）＋ 诊断文案契约（`use of undeclared identifier 'S'`）

> 场景类型：**合成判定型**（一个判据的多处消费点收敛，判定结果可观测）。
> 关联：docs/specs/parser-stage-name-recognition.spec.md（2026-10-23 签字，
> 本篇是它的**修正**：把"可见性"从解析期的名字投影挪回语义期的符号表）、
> docs/learn/46（B31 登记表）、docs/learn/47（多 bit 分类）、
> docs/BUGS.md B37、ROADMAP 主线 I。
> 状态：**已签字**（2026-11-02，用户在会话中拍板"按推荐方式走，尽量贴切 clang"）。

- 读者：改名字查找 / 作用域链 / 裸名声明判据的人
- 审法：每个消费点问的是**同一个问题**吗？答案必须是「是，且都走同一个函数」；
  拿「本 TU 某处声明过」当「此处可见」是本批修的那个静默放行
- 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 按文档骨架规范补齐头部与变更记录；§5 标题改为「观测点（日志键）」；补文末「附：实施记录」 |

## 1. 概念

### 1.1 实体：名字可见性判据（visible-name lookup）

一句话：**"这个名字在此处能不能用"的唯一裁决处**。

```jsonc
// 实体：名字可见性判据   落点：SemanticAnalyzer::lookupVisibleType + isNameDeclared
//                       （src/sema/semantic_analyzer.cpp）
{
  "name": "S",             // 被查的名字（书写形，裸名）
  "scope": "N::M",         // 提问时所处的位置：当前类 / 当前命名空间链 / 全局
  "visible": true,         // 裁决结果：此处能不能用这个名字
  "kind": "Type",          // 可见的是什么：Type / 变量 / 函数 / 不可见
  "level": 2               // 命中在哪一级：1=类作用域 2=命名空间 3=全局
}
// 谁写：Sema（唯一）。谁读：resolveType、visit(ExprStmt)、visit(EmptyStmt)。
// 空值语义：查不到 = 不可见（不是"存在但不可用"）；不可见时由调用点决定报什么。
```

**在哪些场景起作用**：

| 场景 | 角色 |
|---|---|
| 裸类型名声明 `S;` | 判定"是不是可见的类型名"⇒ 声明 or 报错 |
| 类型名解析 `S s;` | 判定"这个类型名此处可见吗"⇒ 解析 or unknown type name |
| [stmt.ambig] 表达式折叠 | 判定"这个裸名是不是类型"⇒ 折叠成声明 or 维持表达式 |

### 1.2 名词

- **可见**：这个名字在**当前作用域链**上能查到（[basic.lookup.unqual]）。
  ★ 不是"本 TU 某处声明过"——那是名字登记表（NameRegistry）回答的另一个问题。
- **作用域链**：当前类作用域 → 命名空间链（内→外）→ 全局。
- **裸类型名声明**：`S;` 这种没有声明符、只有一个类型名的语句（[stmt.ambig]/3）。

## 2. 正常流程

```text
语句层遇到裸名字 `S;`
        │
        ▼
  Parser：语法试探 parseType 成功 ⇒ 产出【候选】声明节点
  （节点携带名字 S；★ Parser 不下"可见"的结论 —— 它没有符号表）
        │
        ▼
  Sema::visit(EmptyStmt)
        │
   ┌────┴─────────────────────────────────────┐
   第一问：lookupVisibleType("S")               │
   （类作用域 → 命名空间链 → 全局）              │
   └────┬─────────────────────────────────────┘
        │                              │
   命中（可见的类型）              未命中
        │                              │
        ▼                    ┌─────────┴──────────┐
   判为无声明符声明            第二问：isNameDeclared("S")
   （合法；clang 只 warning）   （任意 kind 都算）
        │                       │              │
        │                  声明过            没声明过
        │                       │              │
        │                       ▼              ▼
        │                走表达式路径      响亮拒收
        │                （clang：warning  （clang 逐字：
        │                 expression result   use of undeclared
        │                 unused）            identifier 'S'）
        ▼
     完成
```

**步骤**（一步一个动作，关键处带【为什么】）：

1. **Parser 给候选**：语句层 `S;` 试解析成类型语法成功 ⇒ 产出携带名字的
   无声明符声明节点。 【为什么】Parser 没有符号表，"可见"它答不了；
   硬答就是拿名字登记表（跨作用域并集）当"可见" ⇒ 静默放行非法程序。
2. **Sema 裁决**：用**自己的**符号表 + 当前作用域上下文回答两问。
   【为什么】判据单点（红线 3）：同一语义判据不许写两处。
3. **不可见时的三岔**：可见的类型 ⇒ 合法；声明过但不是类型 ⇒ 表达式路径；
   什么都没声明 ⇒ 报错，文案与 clang 逐字相同。

### 2.5 更新对照

本业务不写任何磁盘表——"写操作"是 AST 的一槽（`EmptyStmt::typeName`）。

```jsonc
// 改前（AST 形状）
{ "kind": "EmptyStmt" }                       // ← 改：裸名声明丢了名字
// 改后
{ "kind": "EmptyStmt", "typeName": "S" }      // ← 改：携带名字，Sema 才裁得了
```

## 3. 异常处理

| # | 异常 | 如何处理 |
|---|---|---|
| 异常1 | 名字在本 TU 某处声明过，但此处不可见 | 响亮拒收（`use of undeclared identifier 'X'`，与 clang 逐字）。不可重试（修源码重编） |
| 异常2 | 名字声明过但不是类型（变量/函数遮蔽同名类型） | 走表达式路径（clang 是 warning expression result unused，minicc 无 warning 机制 ⇒ 不报错）。不是错 |
| 异常3 | 名字在【更外层】命名空间可见（namespace A { namespace B { … } }） | 沿命名空间链逐级外层查找 ⇒ 可见（clang 的 DeclContext 链语义）。不是错 |
| 异常4 | 类作用域的别名遮蔽同名全局类型 | 类作用域先查 ⇒ 别名胜出（[basic.lookup.unqual]）。不是错 |
| 异常5 | 名字在类模板【实例】的别名表里 | 走 m_classDecls[实例类]->typeAliases（不经符号表——实例类的别名没进符号表）。不是错 |

（可重试性：编译器业务无运行时重试概念——上表全部"不可重试（修源码重编）"；
异常 2/3/4/5 不是错。）

## 4. Out of Scope（AI 禁止自行扩展）

1. 不做 Parser 持 Sema 活句柄 / 解析期活符号表（clang 的 `Actions` 模式）——
   那是 ROADMAP 的远期项（红线 1 改写后另议）。
2. 不做名字登记表的退场（消费点 ①② 的搬迁另案）。
3. 不做命名空间模板蓝图的前缀化（`N::B` 在全局以裸名可见的错绑，另案 B38）。
4. 不做嵌套类（`struct A { struct B {}; }`）——解析期即响亮拒收，是特性未做。
5. 不做 did-you-mean / Fix-it 诊断建议。
6. 不做完整的类作用域链（基类成员、using-declaration 引入名）——本实现只有
   "当前类的 typeAliases"一级近似。

## 5. 观测点（日志键）

| 事件点 | 键名 | 必打时机 |
|---|---|---|
| 可见性裁决（命中） | `[sema:stmt] '<名字>' 可见的类型名 ⇒ 判为无声明符声明` | 每次命中 |
| 命名空间命中（类型解析） | `[resolve] '<名字>' 在命名空间 '<scope>' 内 ⇒ <scope>::<名字>` | 沿用既有行，逐字不变 |
| 类作用域命中（类型解析） | `[sema:alias] <名字> 在类 '<class>' 作用域内 ⇒ <类型>` | 沿用既有行，逐字不变 |
| 不可见 | `[Semantic Error] <位置>: use of undeclared identifier '<名字>'` | 报错时 |

## 拍板记录（2026-11-02）

| # | 问题 | 拍定 |
|---|---|---|
| 1 | 走哪条路 | **阶段 1（可见性裁决下沉 Sema）→ 阶段 2（登记表退场）**，不做"Parser 自建作用域链表"（那是第二张表，踩红线 3） |
| 2 | 红线 1 的措辞 | 改写成**目标式**："语义裁决权单一"（详见 docs/constraints/00-全局约束.md） |
| 3 | 不可见时的文案 | 与 clang 逐字：`use of undeclared identifier 'X'`（minicc 走声明分支、clang 走表达式分支，语义结论相同） |

---

## 附：实施记录

**拍板**见上方「拍板记录」；本节回填**实际怎么落的**与**验收数据**。

| 项 | 实测值 | 出处 |
|---|---|---|
| 判据单点 | Sema 新增 `lookupVisibleType`（类作用域别名表 → 命名空间链逐级外层 → 全局）；`resolveType` 里原三处手写回退全部换成它一次调用 | §1.1、§2 步骤 2、docs/learn/48 |
| AST 一槽 | `EmptyStmt` 增 `typeName` 槽携带裸名到语义期（`include/ast.h`） | §2.5 |
| 裁决三支 | 可见的类型名 ⇒ 判为无声明符声明（[dcl.dcl]/1）；声明过但非类型 ⇒ 走表达式路径（局部变量遮蔽全局类 S，与 clang 同）；两不中 ⇒ `use of undeclared identifier` | §2 步骤 3 |
| 红线 1 改写 | 「语法期零语义分层」→「**语义裁决权单一**」（约束文件里同时记明：原措辞把**现方案**写成了红线，顺带禁掉了 clang 的实际做法——红线该写目标不写现状） | 拍板 2、docs/constraints/00-全局约束.md §二.1 |
| 登记表降级 | NameRegistry 定位改写为**过渡态的候选过滤器**（「值不值得让 Sema 查一下」），不得当可见性的答案 | §2 步骤 1、docs/learn/48 |
| 新增集成用例 | `tests/lang/test_basics_25_bare_type_name_visibility.cpp`（正例）、`test_basics_26_bare_type_name_namespace_not_visible_error.cpp`、`test_basics_27_bare_type_name_class_alias_not_visible_error.cpp`（两个负例，文案与 clang **逐字同**，含列号 28:5 / 29:5） | docs/learn/48 |
| 新增单测 | `tests/unit/test_name_visibility.cpp`（`NameVisibility.*` 7 例：全局 / 命名空间内 / 命名空间链外层 / 类作用域别名 / 全局不可见 / 类作用域别名全局不可见 / 局部变量遮蔽） | docs/learn/48 |
| 突变验证 | **4 条**逐条实跑变红：①命名空间链只查一层 ②类作用域分支挪到命名空间链之后 ③去掉第二问 `isNameDeclared` ④Parser 不带名字 | docs/designs/visible-name-lookup.design.md §5 |
| 回归 | 全量 **369/369** 全绿（362 ＋ 7）；有意漂移 4 处（basics_23 源注释位移 + 新增 `[sema:stmt]` 2 行 / tmpl_54 过时括注改写 / 新增 basics_25..27 三份 / 单测 +7） | docs/learn/48、BUGS.md B37 |
| 学习产物 | `docs/learn/48-visible-name-lookup.md` | — |
| 台账 | `docs/BUGS.md` **B37** | — |
| 落库提交 | `4f1fb49`（2026-11-02，与 B38 同批） | git log |

★ **本批查实、但未修的三处既有缺口**（已登记，见 docs/learn/48 §7 边界表）：
命名空间变量在块作用域不可见（`namespace N { int g; }` 里裸用 `g` 不可见）／
命名空间模板蓝图按**裸名**注册 ⇒ `N::B` 会绑到全局 B（**已在下一批 B38 修掉**）／
嵌套类不支持。

★ **单测曾被既有缺口绊住**：`NameVisibility.NamespaceChainOuterLevel` 里调用
`A::B::f()` 报 `Undefined variable 'A::B::f'`——命名空间嵌套函数的**不限定调用**
本身不支持，改成不调用函数（Sema Pass 3 照样走查函数体）。
★ **这条坑在下一批被追平**：根因是 B38 的「嵌套命名空间丢外层」
（注册名是 `B::f` 而使用名是 `A::B::f`）。
