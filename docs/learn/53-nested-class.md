# 53. 嵌套类（[class.nest]，Stage 3 批 4）

> 定位：让**嵌套类**成为作用域链的自然结果，而不是一条特判 ——
> 外层类里的 `B` 与全局的 `B` 是两个实体，靠"类作用域链"这一个查询分开。
> 关联：docs/specs/nested-class.spec.md ＋ docs/designs/nested-class.design.md
> 前置：docs/learn/48（可见性下沉 Sema）/ 49（作用域即身份）/ 51（活作用域链）/ 52（C++20 未声明模板）

## §0 定位与能力清单

| 能力 | 状态 | 落点 |
|---|---|---|
| 类体内声明嵌套类 | ✅ 本批 | `src/parse/parser.cpp` `parseClassDecl` 成员循环 |
| 嵌套类的登记名带作用域（`A::B`） | ✅ 本批 | `processClassDecl`（前缀化 + 递归） |
| 类作用域链（类自身 → 别名 → 嵌套类 → 外层类） | ✅ 本批 | `lookupVisibleType` ① 分支 |
| 查找顺序：类作用域 → 命名空间链 → 全局 | ✅ 本批 | `resolveType` |
| 嵌套类的构造 / 析构 / 带参构造 | ✅ 本批 | `inferNew` / `visit(VarDeclStmt)` |
| `new A::B(...)`（限定名类型） | ✅ 本批 | `parser.cpp` `new` 分支 |
| 限定名调用 `B::sm()`（可见名 → 登记名） | ✅ 本批 | `inferCall` 开头 |
| 嵌套类看得见外层类的 `static` 成员 | ✅ 本批 | `inferVar` 第 2.5 步 |
| 类自身的名字在本类作用域内可见（自引用指针） | ✅ 本批 | `lookupVisibleType` ①0 |
| 构造函数符号由语义期选定并回填 | ✅ 本批 | `inferNew` / `visit(VarDeclStmt)` + CodeGen 改用回填值 |
| 诊断 `no type named 'C' in 'A'`（clang 逐字） | ✅ 本批 | `resolveType` 限定名兜底 |
| 嵌套类在**类模板**里 | ⬜ 边界 | 蓝图不克隆嵌套类（响亮报错） |
| **类外定义**嵌套类成员 | ⬜ 边界 | Parser 未支持任何类外成员定义 |
| 嵌套类**按值返回** | ⬜ 边界 | 与普通类同款（按值返回 / 拷贝省略缺口） |

## §1 理论背景

### 1.1 标准怎么说

- **[class.nest]/1**：嵌套类**是外层类的成员**，没有特殊地位。它有外层类的
  访问权，但**没有**外层类的隐式 `this`（外层的实例字段必须经显式对象访问）。
- **[basic.scope.class]/2.1**：**类名被插入到本类作用域里** ⇒
  `struct A { A* p; };` 里的 `A` 指本类（自引用指针，常见惯用法）。
- **[basic.lookup.unqual]**：非限定名查找**由内向外**：块作用域 → 类作用域
  （成员函数里）→ 命名空间链 → 全局。
- **[basic.lookup.qual]**：限定名 `Q::m` 的**限定者 Q 单独按可见性解析**，
  再在 Q 的成员表里找 `m`。
- **[dcl.type.elab]/2**：`new` 后面跟的是 **type-id**，type-id 可以是限定名 ⇒
  `new A::B(6)`。
- **[class.member.lookup]**：嵌套类是外层类的成员 ⇒ 嵌套类里能看见外层类的
  `static` 成员（裸名）。

### 1.2 编译原理怎么说

**嵌套类不是新机制，是作用域链多了一环**。词法分析器只管切 token；
语法分析器把 `class A { class B { }; };` 建成"A 的成员表里有一个 B 的声明"；
语义分析器只需要回答一个问题：**"B" 这个名字在这里指谁？**

于是整件事塌缩成**名字查找**（name lookup）的一个经典问题：
一条从内到外的链，逐级问"这一层有没有这个名字"，先命中者胜。
clang 用 `DeclContext` 父链（`Sema::PushDeclContext`）表达这条链，
本项目用 `ClassDecl::ownerClassName` 表达同一条链。

★ 关键结论：**"嵌套类"这个概念本身不需要任何代码**——它只需要"类作用域"这条链
能走到外层类，并且**登记名带作用域**。

### 1.3 clang 的三个机制（逐条对照）

| clang 机制 | 位置 | 本实现 | 简化了什么 |
|---|---|---|---|
| 类体内的嵌套类 vs 局部类：沿作用域链问 `S->isClassScope()` | `clang/lib/Parse/ParseDeclCXX.cpp:Parser::ParseCXXMemberSpecification` | `parseClassDecl` 成员循环遇 `class`/`struct` ⇒ 收进 `decl->nestedClasses` | clang 用 `Scope` 类区分"类作用域"与"块作用域"；本项目只有类作用域一档 |
| `PushDeclContext`：嵌套类的 DeclContext = 外层 CXXRecordDecl | `clang/lib/Sema/SemaDecl.cpp:Sema::ActOnTagStartDefinition` | `nested->ownerClassName = 外层类登记名` | clang 存指针；本项目存**名字**（登记名）⇒ 代价是字符串比较，收益是序列化简单 |
| `LookupQualifiedName` 按 DeclContext 查成员 | `clang/lib/Sema/SemaLookup.cpp:Sema::LookupQualifiedName` | `lookupVisibleType` ①b + `findMemberType` | clang 走 USR/Decl 指针；本项目走登记名 |
| `BuildCXXConstructExpr`：构造调用由 Sema 选定 | `clang/lib/Sema/SemaDeclCXX.cpp:Sema::BuildCXXConstructExpr` | `inferNew` / `visit(VarDeclStmt)` 选定并回填 `ctorSymbol` | clang 跑完整重载决议；本项目用 `pickBestByArgs`（精确 → 可隐转 → 首个） |

## §2 设计决策

### 2.1 为什么"嵌套类"不能做成一条特判

learn/51 §8.1 的交接第 2 条写死了：**不许为"嵌套"开特判**。

诱惑很大：`A::B` 一看就是"嵌套类"，写个 `findNestedClass("A", "B")` 最省事。
但那会造出**第二份作用域事实**——而这正是 Stage 1~3 花了三个批次拆掉的东西
（`NameRegistry` 整类删除就是这个决定的兑现）。

★ 判据：**一个查询能不能回答"这个名字在这里指谁"，答案是"能"** ⇒ 不许特判。
本批的做法是把"类作用域"从"当前类的一张别名表"升级成**一条链**，
嵌套类只是链上多了一种成员（一个类名）。

### 2.2 为什么查找顺序必须由标准定死

现象（探针实测）：

```cpp
struct A { struct B { int v; }; int use() { B b; return b.v; } };
struct B { int w; };
```

A 的成员函数里的裸名 `B` —— 是 `A::B` 还是全局 `B`？clang 答 **`A::B`**
（类作用域比全局作用域**内**）。

minicc 改之前的答法：全局符号表先命中 ⇒ 绑到**全局 B** ⇒
`No member 'v' in class 'B'` —— **不报错，只是算错**。
这类"静默算错"比报错难发现得多：编译全绿、链接全绿、运行期才炸。

⇒ 顺序改成 **类作用域 → 命名空间链 → 全局**（[basic.lookup.unqual] 由内向外）。
★ 这一改的**连带效应**：命名空间里的同名类型现在也正确地**遮蔽**全局同名类型
（此前全局符号表先命中 ⇒ 命名空间的成员被全局的压住）。

### 2.3 为什么登记名必须带作用域

`A::B` 与全局 `B` 是两个类 ⇒ 它们的**字段布局、虚表、构造/析构符号**全都要分开。
minicc 的类型身份是**一个字符串**（`Type::makeClass(name)` + `m_classTypes[name]`）
⇒ 字符串不同 = 身份不同 ⇒ **登记名带作用域是唯一的分家手段**。

派生链（learn/49 的"作用域即身份"）：

```
登记名 "A::B"  ⇒  类表键 / 符号表键 / 实例名 / 缓存键 / mangling / 汇编符号
                   m_classDecls["A::B"]      m_classTypes["A::B"]
                   m_symbolTable["A::B"]     "A::B_get" → 汇编 "A__B_get"
```

★ **一处前缀化，其余全是派生**（learn/49 的审法）——嵌套类是这条规则的又一次兑现。

### 2.4 为什么构造函数符号必须由语义期选定

旧实现：CodeGen 按 `类名_类名[_N]` **自己拼**构造函数符号；
定义点（Sema）按另一套规则命名 ⇒ **同一处语义判断写了两份**。

这在本项目已经炸过三次（B10 / B20 / B22），第四次是限定名类：

```cpp
namespace N { struct S { S() { v = 3; } }; }
int main() { N::S s; return s.v; }
```

定义点算出的符号是 `N::S_S`，CodeGen 拼的是 `N::S_N::S`
⇒ `undefined reference to 'N__S_N__S'` —— **编译期全绿，链接期才炸**。

修法照 clang 的 `BuildCXXConstructExpr`：**Sema 选定并回填，CodeGen 只发**。
★ 这条修法顺带把上面那个 bug 也修了（本批实测 rc=0，clang 同）。

### 2.5 为什么"类自身的名字"也可见

`struct A { A* p; };` —— A 指本类。这是 [basic.scope.class]/2.1 的明文，
也是**极常见**的惯用法（链表 / 树的节点）。

minicc 改之前报 `unknown type name 'A'` ⇒ **拒收合法程序**。
根因：类作用域的查找只问"类里有没有叫这个名字的**别名**"，
没问"**这个类自己**叫什么"。

★ 修法一行：链上每一环先问"可见名 == 名字吗"。
（"可见名" = 登记名的最后一段：`N::A::B` 的可见名是 `B`。）

## §3 clang 源码对照表

| clang 源码 | 本实现位置 | 简化了什么 |
|---|---|---|
| `clang/lib/Parse/ParseDeclCXX.cpp:Parser::ParseCXXMemberSpecification` | `src/parse/parser.cpp:parseClassDecl`（成员循环） | clang 用 `Scope::isClassScope()` 区分嵌套类/局部类；本项目只有类作用域 |
| `clang/lib/Sema/SemaDecl.cpp:Sema::ActOnTagStartDefinition` → `PushDeclContext` | `src/sema/semantic_analyzer.cpp:processClassDecl`（`nested->ownerClassName = 本类名`） | clang 存 DeclContext 指针；本实现存**登记名字符串** |
| `clang/lib/Sema/SemaDecl.cpp:Sema::PushOnScopeChains` | `src/sema/semantic_analyzer.cpp:lookupVisibleType`（① 分支链式外溯） | clang 维护多条 scope chain；本实现一条类链 + 一条命名空间链 |
| `clang/lib/Sema/SemaLookup.cpp:Sema::LookupQualifiedName` | `src/sema/semantic_analyzer.cpp:findMemberType` + `resolveType` 限定名分支 | clang 按 USR 查；本实现按登记名查 |
| `clang/lib/Sema/SemaDeclCXX.cpp:Sema::BuildCXXConstructExpr` | `src/sema/semantic_analyzer.cpp:inferNew` / `visit(VarDeclStmt)` | clang 完整重载决议；本实现 `pickBestByArgs` 三档 |
| `clang/lib/Sema/SemaExpr.cpp:Sema::BuildDeclRefExpr` | `src/sema/semantic_analyzer.cpp:inferCall`（限定名改写） | clang 走 `NestedNameSpecifier`；本实现走字符串拆分 + 可见性解析 |

## §4 关键过程（ASCII 图）

### 4.1 名字查找：由内向外

```
        "B"（A 的成员函数里的裸名）
                 │
                 ▼
    ┌────────────────────────┐
    │ 类作用域链（m_currentClassName）│
    │  cls = "A"                    │
    │   ①0 可见名("A")=="B"? 否      │
    │   ①a typeAliases["B"]?   否   │
    │   ①b nested "A::B"?     ★命中 │
    │   ⇒ A::B                       │
    └────────────────────────┘
                 │（若未命中）
                 ▼
    ┌────────────────────────┐
    │ 命名空间链（m_currentNamespace）│ ⇒ 全局
    └────────────────────────┘
                 │（若未命中）
                 ▼
    ┌────────────────────────┐
    │ 全局符号表                   │ ⇒ 全局 B（★排在类作用域【之后】）
    └────────────────────────┘
```

### 4.2 登记名的派生链

```
   源码            登记名              类表键            汇编符号
   A { struct B    "A::B"              m_classDecls       A__B_get
       { int get(); }                ["A::B"]           （'::' 洗成 '__'
     }; }                                                 —— asmSymbol）
```

### 4.3 构造函数符号：选定与发出分离

```
  Sema（选定）                          CodeGen（发出）
  ─────────────────────────────          ──────────────────────────
  inferNew / VarDeclStmt                 visit(NewExpr)
    候选 = 该类的构造函数表                ctorSym = expr.ctorSymbol
    择优 = pickBestByArgs                  （空 ⇒ 退回旧硬拼兜底）
    ⇒ expr.ctorSymbol                    callq <ctorSym>
```

## §5 可复现实验命令

```bash
# ① 编译（0 警告）
./build.sh

# ② 单元测试（389 例；本批新增 7 例 NestedClass.*）
cmake --build build-linux -j6 && ctest --test-dir build-linux --output-on-failure

# ③ 集成日志基线（164 份逐字节比对）
./logdiff.sh diff

# ④ 突变验证（5 条判据逐条实跑变红）
./tools/verify-nested-mutations.sh

# ⑤ clang 对拍：本批的六组探针
cat > /tmp/nested.cpp <<'EOF'
struct A {
    struct B { int v; B() { v = 1; } B(int x) { v = x; } int get() { return v; } };
    int use() { B b(7); return b.get(); }
};
struct B { int w; };
int main() { A a; A::B b1; A::B b2(9); B g; g.w = 4;
             return a.use() + b1.get() + b2.get() + g.w; }   // 期望 21
EOF
clang++-18 -std=c++20 /tmp/nested.cpp -o /tmp/nested.c && /tmp/nested.c; echo $?   # 21
./minicc /tmp/nested.cpp -o /tmp/nested.m && /tmp/nested.m; echo $?                 # 21
```

★ 探针对照表（本批实测，全部与 clang 一致）：

| 探针 | clang | minicc |
|---|---|---|
| 同名共存 + 构造 + 成员函数 | 21 | 21 |
| `new A::B(6)` + `static` 成员 + 限定名调用 | 22 | 22 |
| 命名空间里的嵌套类 `N::A::B::C` | 11 | 11 |
| 自引用指针 `struct A { A* p; };` | 5 | 5 |
| 嵌套类读外层类的 `static` | 32 | 32 |
| `A::C`（C 不存在） | rc=1 `no type named 'C' in 'A'` | rc=1 同文案 |

## §6 真实版多做了什么、为什么

| 真实 C++ | 本实现 | 为什么这么取舍 |
|---|---|---|
| 嵌套类的访问权限（`private: struct B;`） | 一律 public | minicc 的类成员访问控制尚未建模；嵌套类本身先跑通 |
| 嵌套类是**成员类模板**（类模板里的嵌套类） | 不支持 | 蓝图不克隆嵌套类 ⇒ 需要实例化步骤 5.8（另一批的量级）；当前是**响亮拒收**不是静默算错 |
| `struct A::B { … };` 类外定义 | 不支持 | Parser 未支持任何类外成员定义（不止嵌套类） |
| 嵌套类按值返回 / 拷贝省略 | 不支持 | 与普通类同款缺口（`S x = f();` 的初始化式被丢弃），不是嵌套类特有 |
| clang 用 `DeclContext` 指针表达作用域链 | 用**登记名字符串** | 本项目的类型身份本来就是字符串（`Type::makeClass(name)`）；换成指针是主线 G 的债（见 ROADMAP） |
| clang 的 `BuildCXXConstructExpr` 完整重载决议 | `pickBestByArgs` 三档 | 本项目没有转换序列排序（[overload.icp] 尚未实现）；择优口径与成员方法调用同源 |
| 嵌套类模板参数可访问外层模板形参 | 不支持 | 承 learn/34 §3.5「本项目只做函数模板」 |

## §7 边界与已知取舍

| # | 边界 | 现状 | 性质 |
|---|---|---|---|
| 1 | 嵌套类在**类模板**里 | `unknown type name 'Inner'` | 响亮拒收（待做：实例化步骤 5.8 克隆嵌套类） |
| 2 | **类外定义**嵌套类成员 | Parser 不支持 | 与普通类的类外成员定义同款缺口 |
| 3 | 嵌套类**按值返回** | 初始化式被丢弃 | ★ **静默算错**（与普通类同款）—— 已登记为已知缺口，见 §8 |
| 4 | 嵌套类的**访问权限** | 一律 public | minicc 尚未建模类成员访问控制 |
| 5 | 嵌套类**模板**（成员类模板） | 不支持 | 承 learn/34 §3.5 |
| 6 | 嵌套类的**虚函数 / vtable** | 支持（按普通类处理） | 嵌套类有自己的 vtable ⇒ 与全局同名类互不干扰 |

★ 边界 3 值得单独说：`S x = f();`（函数返回类对象）在 minicc 里
**初始化式被丢弃**（对象退化成默认构造）—— 这是**普通类**就有的老缺口
（`semantic_analyzer.cpp` 的平凡拷贝分支注释里写着"函数返回同类对象不涉及"），
不是嵌套类引入的，但它让"嵌套类按值返回"这条边界看起来像嵌套类的问题。
★ 处理方式：**不掩盖**（写进本表 + ROADMAP 的欠账行），而不是在嵌套类这条路上打补丁。

## §8 下一批

1. **主线 G：vtable 槽 → `FuncDeclPtr` 句柄**（ROADMAP 已登记的欠账）——
   把"槽里放一个字符串符号"换成"槽里放一个声明句柄"，从根上收掉
   "名字的规则在多处各写一遍"这笔债；
2. **ROADMAP 重排**：模板线六项提前（理论密度最高），语法面降级；
3. **实例化步骤 5.8**：克隆嵌套类 ⇒ 边界 1 关闭。

## 演进记录

| 日期 | 阶段 | 变了什么 | 证据 |
|---|---|---|---|
| 2026-11-02 | Stage 3 批 4 | 嵌套类落地：Parser 收嵌套类 / 登记名带作用域 / 类作用域链 / 查找顺序改为由内向外 / 构造函数符号改由 Sema 选定并回填 / `new` 收限定名 / 类自身的名字可见 / 诊断对齐 clang | 集成 164 份零漂移、单测 389/389、突变验证 5/5、clang 六组探针全对 |
