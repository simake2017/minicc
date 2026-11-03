# 技术设计：嵌套类（[class.nest]，Stage 3 批 4）

> 对应源码：`src/parse/parser.cpp`（收嵌套类 / `new` 限定名）、
> `src/sema/semantic_analyzer.cpp`（类作用域链 / 登记名 / 构造函数回填）、
> `src/codegen/codegen.cpp`（用回填的构造函数符号）。
> **本项目按 clang 目录组织，不采用 skill 的"1 设计 = 1 flow 文件"**
> —— 用户 2026-10-23 拍板，见 `docs/constraints/00-全局约束.md` 头部。
> 来源：`../specs/nested-class.spec.md`
> （= 仓库内 `docs/specs/nested-class.spec.md`，2026-11-02 签字）。
> 标准：不看 spec 也能照本文实现。
> 审法：嵌套类是不是**用同一张作用域链的通用查询**答出来的？必须是「是」。
> 读者：改类注册 / 名字查找 / 构造函数符号 / `new` 表达式解析的人。
> 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 首版：Stage 3 批 4 的设计映射 / 流程图 / 异常流转 / 突变验证清单 |

## 1. spec → 设计映射表

| spec 节 | 设计落点 | 文件:符号 |
|---|---|---|
| §3.1 嵌套类登记 | Parser 分支 + Sema 递归处理 | `parser.cpp:parseClassDecl` / `semantic_analyzer.cpp:processClassDecl` |
| §3.1 `new` 限定名 | `new` 分支收限定名链 | `parser.cpp:parseExpression`（`new` 分支） |
| §3.2 类作用域链 | 链式查找（①0/①a/①b + 外溯） | `semantic_analyzer.cpp:lookupVisibleType` |
| §3.2 查找顺序 | 类作用域 → 命名空间链 → 全局 | `semantic_analyzer.cpp:resolveType` |
| §3.2 静态成员 | 类链并列推进 owner | `semantic_analyzer.cpp:inferVar`（第 2.5 步） |
| §3.2 限定名调用 | 限定者解析 → 登记名 | `semantic_analyzer.cpp:inferCall`（开头） |
| §3.3 构造函数符号 | Sema 选定 + 回填 | `semantic_analyzer.cpp:inferNew` / `visit(VarDeclStmt)`；`codegen.cpp:visit(VarDeclStmt)` / `visit(NewExpr)` |
| §3.4 诊断对齐 | 限定名兜底 | `semantic_analyzer.cpp:resolveType`（限定名分支） |

## 2. 整体流程图

```
  源码                          Parser                     Sema                     CodeGen
  ─────────────────────────────────────────────────────────────────────────────────────────────
  struct A {              ┌─ parseClassDecl(A)
    struct B { int v; };  │   └─ 成员循环见 class/struct
    B b;                  │        └─ parseClassDecl(B)  ⇒ decl->nestedClasses = [B]
    int use() { B b; }    │   （不建第二张作用域表：名字进的是【当前类作用域】）
  };                      └─ 名字随 decl 交给 Sema
                                   │
                                   ▼
                        processClassDecl(A)
                          ① m_classDecls["A"] = A
                          ② 嵌套类前缀化 + 递归：     ← ★ 必须在字段解析之前
                               B.ownerClassName = "A"
                               B.name = "A::B"
                               processClassDecl(B)
                                  ⇒ m_classDecls["A::B"]、布局、虚表、符号
                          ③ 字段类型解析：B ⇒ 类作用域链命中 ⇒ A::B
                                   （不是全局 B ★）
                          ④ 方法表 / 虚表 / 布局
                                   │
                                   ▼
                        Pass 2 registerFunction
                          A::B::get() ⇒ mangled "A::B_get" ⇒ 汇编 "A__B_get"
                                   │
                                   ▼
                        Pass 3 分析函数体
                          m_currentClassName = "A::B"
                          裸名 B ⇒ 类作用域链 ⇒ A::B（自身）
                          裸名 sval ⇒ 类链 + 基类链 ⇒ 外层类的 static
                                   │
                                   ▼
                        CodeGen
                          构造调用 ⇒ 优先用 Sema 回填的 ctorSymbol
                          （旧路径：自己拼 "类名_类名[_N]" ⇒ 限定名类一撞就错配 B42）
```

## 3. 子流程 + 业务说明

### 3.1 类作用域链（唯一的类作用域事实）

```
lookupVisibleType(name)
   │
   ├─ ①0  cls 的可见名 == name ?      ⇒ 类自身（[basic.scope.class]/2.1）
   │        "A::B" 的可见名是 "B"（最后一段）
   ├─ ①a  cls.typeAliases[name]       ⇒ 类内类型别名
   ├─ ①b  存在 nested.name == cls::name ⇒ 嵌套类
   │
   └─ cls = cls.ownerClassName         ← 外溯一级（嵌套类 → 外层类 → …）
        （链的每一环由 ClassDecl::ownerClassName 给出 —— Sema 前缀化时写入）
```

★ **为什么"类自身的名字"排在最前**：它是本类作用域里**最内层**的名字
（[basic.scope.class]/2.1 把类名插入本类作用域）⇒ 任何同名的东西都被它遮蔽。
`struct A { A* p; };` 里的 `A` 必须是本类。

★ **为什么用"可见名 = 登记名的最后一段"**：登记名是限定名（`N::A::B`），
但在源码里它写作 `B` ⇒ 查找的键必须是**可见名**，不是登记名。
（登记名只在**登记**与**限定名改写**两处出现。）

### 3.2 名字查找顺序（由内向外）

```
resolveType(类型名)
   │
   ├─ ① 类作用域（lookupVisibleType 的 level 1）★ 必须先于全局
   ├─ ② 命名空间链（level 2）
   ├─ ③ 全局符号表（level 3）
   │
   └─ 兜底：限定名 `Q::member` ⇒ 限定者解析成类型 ⇒ 查它的成员表
              查不到 ⇒ error: no type named 'member' in 'Q'   ← 与 clang 逐字
```

★ **原顺序错在哪**：全局符号表先命中 ⇒ 嵌套类 `A::B` 与全局 `B` 并存时，
A 的成员函数里的裸名 `B` 被绑到**全局 B** —— **不报错，只是算错**
（症状：`No member 'v' in class 'B'`，B 指全局那个）。
这类缺陷（静默算错）比报错难发现得多，故顺序必须由标准定死，不由实现顺带。

### 3.3 构造函数符号：选定与发出分离

```
Sema（选定）                        CodeGen（发出）
─────────────────────────────────    ────────────────────────────────
inferNew / VarDeclStmt
  按实参类型挑构造函数
  ⇒ expr.ctorSymbol / decl.ctorSymbol    callq <ctorSymbol>
                                          （空 ⇒ 退回旧硬拼兜底）
```

★ 与 clang `BuildCXXConstructExpr` 同构。旧实现让 CodeGen 自己按
`类名_类名[_N]` 拼，与定义点的定名规则是**两份拷贝** ⇒ 限定名类一撞就错配：
`namespace N { struct S { S(){}; }; }` 的定义点是 `N::S_S`、调用点是 `N::S_N::S`
⇒ 链接期 `undefined reference`（编译期全绿）。

### 3.4 限定名调用：可见名 → 登记名

```
inferCall:  callee 名含 "::"
   │
   ├─ 限定者 Q 按【可见性】解析（lookupVisibleType）⇒ 得到登记名 R
   ├─ funcName = R + "::" + 成员名      ← 查表用登记名
   └─ 若 R + "::" + 成员名 == 原名（全局类）⇒ 不改写（零漂移）
```

★ 只有**嵌套类**会出现"可见名 ≠ 登记名"（`B::sm()` vs `A::B::sm`）；
全局类 `C::sm()` 两边一致 ⇒ 这条路径对既有程序零影响。

## 4. 异常流转表

| 触发点 | 现象 | 处置 |
|---|---|---|
| 类体内 `class`/`struct` | 不是嵌套类而是别的语法 | 响亮报错（`Expected type name`） |
| `A::C`（C 不存在） | 限定名的成员查不到 | `error: no type named 'C' in 'A'`（clang 逐字） |
| 裸名既不是类型也不是值 | 查不到 | `use of undeclared identifier 'x'`（既有文案） |
| 嵌套类在类模板里 | 蓝图不克隆嵌套类 | `unknown type name 'Inner'`（响亮拒收；边界） |
| 限定名调用但限定者不是类 | 不是类作用域成员 | 走普通名字查找；查不到则普通报错 |

## 5. 突变验证清单

脚本：`tools/verify-nested-mutations.sh`（每条判据**两次实测**：引入违规必须红）

| # | 突变 | 预期变红的用例 |
|---|---|---|
| ① | 嵌套类不前缀化（登记成裸名 `B`） | `NestedAndGlobalStayDistinct` |
| ② | 类作用域链不查嵌套类（①b 去掉） | `UnqualifiedNameResolvesToNested` |
| ③ | 类自身的名字不可见（①0 去掉） | `ClassNameVisibleInOwnScope` |
| ④ | 限定名调用不改写成登记名 | `QualifiedCallUsesRegisteredName` |
| ⑤ | 缺成员时不报错（限定名兜底去掉） | `MissingNestedMemberIsAnError` |

★ 5/5 实跑变红（2026-11-02）。

## 6. 有意日志漂移

| 文件 | 差异 |
|---|---|
| 新增 `tests/lang/test_basics_32_nested_class.cpp` | 入列（rc=0） |
| 新增 `tests/lang/test_basics_33_nested_class_error.cpp` | 入列（rc=1） |
| 新增 `tests/unit/test_nested_class.cpp` | `_unit_cases` +7 |

★ 既有 **162** 份集成日志**逐字节零漂移**。

## 7. 裁量区清单

| 裁量点 | 本实现的选择 | 备选 | 理由 |
|---|---|---|---|
| 嵌套类的处理时机 | 字段解析**之前** | 之后 | 名字必须先可见（`B b;` 的字段类型） |
| 登记名的分隔符 | `::`（与命名空间同款） | `_` | 作用域即身份；汇编期由 `asmSymbol` 洗成 `__` |
| 类自身的名字是否可见 | 可见（[basic.scope.class]/2.1） | 不可见 | 自引用指针是极常见惯用法，拒收它是"拒收合法程序" |
| 查找顺序 | 类 → 命名空间 → 全局 | 全局优先 | [basic.lookup.unqual] 由内向外 |
| 构造函数符号 | Sema 选定并回填 | CodeGen 硬拼 | clang 同构；硬拼已在限定名类上炸过（B42） |
| 嵌套类在类模板里 | 本批不做，响亮报错 | 克隆进实例 | 需要实例化步骤 5.8（另一批的量级） |
