# 规格：嵌套类（[class.nest]，Stage 3 批 4）

状态：已签字（2026-11-02）
签字：2026-11-02（用户拍板；Stage 1 拍板表第 3 条「阶段 3 → 阶段 3 批 4（嵌套类）」，拍板记录见 docs/learn/48）
来源：docs/BUGS.md **B43**（嵌套类完全不支持）＋ docs/learn/51 §8.1 三条交接 ＋ 上游技能 biz-dev-pipeline（spec 骨架）
契约变更点：`include/ast.h`（嵌套类在**类作用域**的登记面与限定名可达性）＋ Sema 的类作用域链窄接口（`lookupVisibleType` 一族：类自身 → 别名 → 嵌套类 → 外层类）＋ 诊断文案契约（`no type named 'X' in 'Y'`）

> 阶段：**Stage 3 批 4**（承 Stage 3 批 2+3 的活作用域链 —— docs/learn/52）
> 关联缺陷：BUGS.md **B42**（命名空间显式构造函数符号错配）/ **B43**（嵌套类不支持）/ **B44**（类自身的名字在本类作用域内不可见）
> 关联学习产物：docs/learn/53-nested-class.md
> 签字：2026-11-02（Stage 1 拍板表第 3 条）

- 读者：改类注册 / 名字查找 / 构造函数符号 / `new` 表达式解析的人
- 审法：嵌套类是不是**用同一张作用域链的通用查询**答出来的？答案必须是「是」——
  任何"为嵌套类单开一条特判"都是缺陷（docs/learn/51 §8.1 交接第 2 条）
- 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 首版：Stage 3 批 4 的需求 / 范围 / 判据 / 验收 / 观测点 |

## 1. 需求与动机

用户最初的提问（"class A 里面有个 B，然后外面还有个 class B，感觉有问题"）
在 Stage 1/2/3 批 1~3 之后仍未落地：**嵌套类本身**还不支持。

clang 的模型一句话：**嵌套类就是外层类的一个成员**，没有特殊地位。

- 它的名字活在外层类的**类作用域**里（[basic.scope.class]/2.1）；
- 它的**登记名带作用域**（`A::B`），与全局 `B` 是两个实体（learn/49 的"作用域即身份"）；
- 它的**外层类作用域**是它自己的作用域链的上一环（clang 的 DeclContext 父链）；
- 它的构造函数与析构函数由**语义期选定**（clang 的 `BuildCXXConstructExpr`），
  CodeGen 只发被选中的那个符号。

minicc 现状（探针实测，clang++-18 -std=c++20 对拍）：

| 程序 | clang | minicc 现状 | 性质 |
|---|---|---|---|
| `struct A { struct B { int v; }; };` | rc=0 | rc=1 `Expected type name` | 拒收合法程序 |
| `struct A { A* p; };`（自引用指针） | rc=0 | rc=1 `unknown type name 'A'` | 拒收合法程序 |
| `namespace N { struct S { S(){}; }; } N::S s;` | rc=0 | rc=1 `undefined reference to 'N__S_N__S'` | 链接期炸（符号错配） |
| `struct A { struct B{}; }; A::C c;` | rc=1 `no type named 'C' in 'A'` | rc=1 `unknown type name 'A::C'` | 诊断指向错 |

★ 三条根因各自独立，但**共用一条修法**：把"类作用域"从"当前类的一张别名表"
升级成**一条作用域链**（类 → 外层类 → …），并让登记名带作用域。

## 2. 范围

### 2.1 In Scope

1. 嵌套类的**声明**（Parser 收类体内的 `class`/`struct` 定义）；
2. 嵌套类的**登记**（登记名 = 外层类登记名 + `::` + 裸名；类表键 / 符号表键同源）；
3. 类作用域链的**查找**（类型名 / 静态成员 / 限定名调用三处共用同一条链）；
4. 嵌套类的**构造与析构**（含带参构造）、**`new` 限定名类型**；
5. 构造函数符号由**语义期选定并回填**（CodeGen 不再自己拼）；
6. 类自身的名字在**本类作用域内可见**（自引用指针）。

### 2.2 Out of Scope

| 项 | 现状 | 备注 |
|---|---|---|
| 嵌套类写在**类模板**里 | ⬜ 不支持（响亮报错 `unknown type name 'Inner'`） | 蓝图不克隆嵌套类；需在实例化时克隆（步骤 5.8） |
| **类外定义**嵌套类成员（`struct A::B { … };`） | ⬜ 不支持 | Parser 尚未支持任何类外成员定义 |
| 嵌套类**按值返回**（`A::B f();`） | ⬜ 不支持 | 与普通类同款，属"按值返回 / 拷贝省略"缺口 |
| 嵌套类**模板**（类模板里的成员类模板） | ⬜ 不支持 | 承 learn/34 §3.5「本项目只做函数模板」 |
| 嵌套类的**访问权限**（private 嵌套类的外部可见性） | ⬜ 不做 | minicc 的类成员一律 public |

## 3. 规格

### 3.1 嵌套类 = 外层类的成员，登记名带作用域

```
Parser:  类体内见 class/struct ⇒ 递归 parseClassDecl ⇒ 收进 decl->nestedClasses
Sema:    processClassDecl(A)
           ├─ 登记 A（m_classDecls["A"] / m_classTypes["A"] / 符号表 "A"）
           └─ 对每个嵌套类 B：
                ownerClassName = "A"          （类作用域链的一环）
                name           = "A::B"       （登记名带作用域）
                processClassDecl(B)           （递归：布局 / 虚表 / 符号照普通类）
```

★ **嵌套类的处理位置**：必须在**本类字段类型解析之前**——嵌套类的名字在本类
作用域内可见（`struct A { struct B{}; B b; };` 里的 `B` 指 `A::B`）。
放晚了 ⇒ `B b;` 报 `unknown type name 'B'`。

★ **前缀化只写一遍**：登记名（`A::B`）是唯一事实，实例名 / 缓存键 / mangling /
汇编符号（`A__B`）全部由它派生（learn/49）。

### 3.2 类作用域链：一条链，三处消费

```
lookupVisibleType(name)          —— 类型名（别名 / 嵌套类 / 类自身）
  ├─ ①0 类自身的名字（[basic.scope.class]/2.1）：`struct A { A* p; };`
  ├─ ①a 类内类型别名（using X = T; / typedef T X;）
  ├─ ①b 嵌套类（按 "本类登记名::名字" 配）
  └─ 沿 ownerClassName 外溯一级（嵌套类 → 外层类 → …）
inferVar 第 2.5 步               —— 静态数据成员的裸名（沿 ownerClassName + 基类链）
inferCall 的限定名改写            —— `B::sm()` 的限定者按可见性解析 → 登记名
```

★ **查找顺序**（[basic.lookup.unqual]，由内向外）：
**类作用域 → 命名空间链 → 全局**。
类作用域必须**先于**全局符号表：嵌套类 `A::B` 与全局 `B` 并存时，
A 的成员函数里的裸名 `B` 必须是 `A::B`（clang 同款）。

### 3.3 构造函数符号由语义期选定并回填

```
Sema:  inferNew / VarDeclStmt 选定构造函数 ⇒ 回填 decl.ctorSymbol / expr.ctorSymbol
CodeGen: 优先用回填值；空则退回旧的硬拼（兜底，不是主路径）
```

★ 与 clang `BuildCXXConstructExpr` 同构：**Sema 选，CodeGen 发**。
旧实现让 CodeGen 自己按 `类名_类名[_N]` 拼 ⇒ 与定义点的定名规则是两份拷贝 ⇒
限定名类（命名空间 / 嵌套类）一撞就错配（B42）。

### 3.4 诊断文案对齐 clang

| 情形 | 文案 | clang |
|---|---|---|
| `A::C`（C 不存在） | `no type named 'C' in 'A'` | 逐字相同 |
| 未知裸类型名 | `unknown type name 'X' —— …` | minicc 自有文案（既有约定） |

## 4. 验收

| # | 用例 | 期望 |
|---|---|---|
| 1 | `struct A { struct B { int v; }; }; A::B b;` | rc=0，`b.v` 读写正确 |
| 2 | 嵌套类 `A::B` 与全局 `B` 并存 | 两个类型、两个符号、两个 vtable，互不干扰 |
| 3 | A 的成员函数里裸写 `B b;` | 解析成 `A::B`（不是全局 `B`） |
| 4 | `struct A { A* p; };` | rc=0（自引用指针合法） |
| 5 | `new A::B(6)` | rc=0，取值正确 |
| 6 | `B::sm()`（A 的成员函数内） | 调用按登记名 `A::B::sm` 解析 |
| 7 | 嵌套类里读外层类的 `static` 成员 | rc=0，值正确 |
| 8 | `A::C`（C 不存在） | rc=1，`no type named 'C' in 'A'`（与 clang 逐字） |
| 9 | `namespace N { struct S { S(){}; }; } N::S s;` | rc=0（B42 的链接期错配修复） |
| 10 | 既有集成用例 | 逐字节零漂移（除 §6 声明的漂移） |

**突变验证**（红线 7，脚本 `tools/verify-nested-mutations.sh`）：
① 嵌套类不前缀化 ⇒ 用例 2 红；
② 类作用域链不查嵌套类 ⇒ 用例 3 红；
③ 类自身的名字不可见 ⇒ 用例 4 红；
④ 限定名调用不改写成登记名 ⇒ 用例 6 红；
⑤ 缺成员时不报错 ⇒ 用例 8 红。

## 5. 与 Stage 3 的关系

本批是 Stage 3 的收尾批：**没有新建任何作用域事实**，只是把已有的类作用域
从"当前类的一张别名表"升级成**一条链**，让嵌套类成为这条链的自然结果。

承 learn/51 §8.1 三条交接：

| 交接条 | 本批怎么落的 |
|---|---|
| 不建第二张表 | 嵌套类的名字进**同一张** `m_classDecls`（登记名带作用域），查找走**同一条** `lookupVisibleType` 链 |
| 不开特判 | 没有 `findClassScopeType` 之类的嵌套类专用查询；嵌套类只是"类作用域里的一个类名" |
| 先把源码现场对着 clang 写 | 判据逐条对着 clang 的 `ParseCXXMemberSpecification` / `PushDeclContext` / `BuildCXXConstructExpr` 写（见 docs/learn/53 §3） |

## 6. 有意日志漂移（红线 4：显式列出）

| 文件 | 差异 | 原因 |
|---|---|---|
| `tests/lang/test_basics_32_nested_class.cpp` | 新增 | 本批新增（正例，rc=0） |
| `tests/lang/test_basics_33_nested_class_error.cpp` | 新增 | 本批新增（负例，rc=1） |
| `tests/unit/test_nested_class.cpp` | 新增 7 例 | 本批新增（`NestedClass.*`） |

★ **既有 162 份集成日志逐字节零漂移** —— 本批是**纯新增**特性：
新判据只在"有嵌套类"或"类作用域命中"时才触发，既有程序一条不碰。

## 7. 观测点

| 观测点 | 键名 | 必打时机 | 本批的变化 |
|---|---|---|---|
| 嵌套类被识别 | `[parse:member] ★ '{}' 是嵌套类` | Parser 收类体内的类定义 | **新增** |
| 嵌套类登记 | `[register] class 'A::B'` | Sema 处理嵌套类 | **新增**（登记名带作用域） |
| 类作用域命中（嵌套类） | `[sema:nested] {} 在类 '{}' 作用域内 ⇒ {}` | 裸名解析到嵌套类 | **新增** |
| 类作用域命中（类自身） | `[sema:self] {} 在类 '{}' 作用域内 ⇒ {}` | 裸名解析到类自身（自引用） | **新增** |
| 限定名调用改写 | `[call] 限定名 '{}' ⇒ 登记名 '{}'` | 限定者的可见名 ≠ 登记名 | **新增** |
| 限定名成员不存在 | `[Semantic Error] … no type named 'C' in 'A'` | 限定名的成员查不到 | **新增**（文案与 clang 逐字） |
| 嵌套类方法注册 | `[register] A::B::get() → int [mangled: A::B_get]` | Pass 2 | **新增** |

★ 观测方式：编译一个含嵌套类的程序，看 `[sema:nested]` / `[sema:self]` 两行是否
出现在**预期的那一处**（类作用域命中时），而不是全局符号表命中时。

## 附：实施记录

**范围**见 §2.1；本节回填**实际怎么落的**与**验收数据**。

| 项 | 实测值 | 出处 |
|---|---|---|
| Parser 收嵌套类 | `parseClassDecl` 的成员循环新增 `class`/`struct` 分支 ⇒ 收进 `decl->nestedClasses` | §3.1、docs/learn/53 |
| 登记名前缀化 | `processClassDecl` 在**字段解析之前**递归处理嵌套类：`ownerClassName = 本类名`，`name = 本类名 + "::" + 裸名` | §3.1 |
| 类作用域链 | `lookupVisibleType` 的 ① 分支扩成链：①0 类自身 → ①a 别名 → ①b 嵌套类 → 沿 `ownerClassName` 外溯 | §3.2 |
| 查找顺序 | `resolveType` 改为**类作用域 → 命名空间链 → 全局**（原为全局符号表优先） | §3.2 |
| 静态成员 | `inferVar` 第 2.5 步的类链**并列**推进 `ownerClassName`（嵌套类看得到外层类的 static） | §3.2 |
| 限定名调用 | `inferCall` 开头把限定者按可见性解析 → 登记名（可见名 ≠ 登记名时改写） | §3.2 |
| `new` 限定名 | Parser 的 `new` 分支收 `IDENT (:: IDENT)*` | §3.1 |
| 构造函数符号 | Sema 选定并回填 `decl.ctorSymbol` / `expr.ctorSymbol`；CodeGen 优先用回填值 | §3.3、B42 |
| 类自身名字可见 | `lookupVisibleType` ①0 分支 | §3.1、B44 |
| 诊断对齐 | `resolveType` 的限定名兜底：查不到 ⇒ `no type named 'C' in 'A'` | §3.4 |
| 新增集成用例 | `tests/lang/test_basics_32_nested_class.cpp`（正例 rc=0）、`tests/lang/test_basics_33_nested_class_error.cpp`（负例 rc=1） | docs/learn/53 |
| 新增单测 | `tests/unit/test_nested_class.cpp`（`NestedClass.*` 7 例） | docs/learn/53 |
| 突变验证 | **5 条**逐条实跑变红（脚本 `tools/verify-nested-mutations.sh`） | §4 |
| 回归 | 全量 **389/389** 单测全绿 / build 0 警告 / 集成日志 **164** 份逐字节零漂移 | docs/learn/53 §5 |
| 学习产物 | `docs/learn/53-nested-class.md` | — |
| 台账 | `docs/BUGS.md` **B42** / **B43** / **B44** | — |
