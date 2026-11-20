# 62. 嵌套类的构造/析构身份 + 初始化列表的构造目标

> 定位：构造函数的名字是【本类的裸名】，作用域身份是【登记名】—— 两件事必须分开；
> 初始化列表里"该调哪个构造函数"由 Sema 选定并回填，调用点不许按名字重算。
> 标准：[class.ctor]/1、[class.base.init]、[basic.lookup.unqual]/6、[temp.inst]/5。
> 关联：[多继承布局](17-multiple-inheritance-layout.md)、[作用域即身份](49-scope-qualified-names.md)、
> [vtable 槽句柄化](54-vtable-slot-handle.md)、[成员类模板](61-member-class-template.md)。

## §0 定位与能力清单

| 形态 | 修前 | 修后 |
|---|---|---|
| `struct Outer { struct Inner { int v; }; }; Outer::Inner i;` | 合成 ctor 名 = `Outer::Inner`（登记名）⇒ 符号 `Outer::Inner_Outer::Inner` | 合成 ctor 名 = `Inner`（裸名）⇒ 符号 `Outer::Inner_Inner` |
| `struct H { struct Part { int v; Part(int x){v=x;} }; Part m; H() : m(9) {} };` | ★ **汇编失败**：`as` 报 `junk '::Inner_Outer::Inner_1'` | 编译通过，运行 rc=9（与 clang 对拍） |
| `namespace N { struct Base { Base(int); }; } struct D : N::Base { D() : Base(7) {} };` | 合法程序被拒（`Member 'Base' not found ... initializer list`） | 通过，构造符号 `N::Base_Base_1` 由 Sema 回填 |
| `namespace N { struct S { int x; }; } int main() { S; }` | 静默通过（成员名泄漏成全局可见） | 拒收，文案与 clang 同：`use of undeclared identifier 'S'` |
| `Outer::Mem<int> m;`（成员类模板实例） | 合成 ctor 名 = `Outer::Mem_int`（含作用域的登记名） | `Mem_int`（裸名） |

## 1 理论背景

**① 构造函数没有"自己起的名字"**。按 [class.ctor]/1，构造函数的名字是**注入类名**
（injected-class-name）—— 在类作用域里它就是那个裸 `Inner`。`Outer::` 是**作用域的写法**，
不是函数名的一部分。所以 `Outer::Inner` 的构造函数符号应该是 `Outer::Inner_Inner`
（登记名 + 裸名），不是 `Outer::Inner_Outer::Inner`。

**② 初始化列表要"选一个构造函数"**。按 [class.base.init]，每条 mem-initializer
（`: m(9)` / `: Base(7)`）都要以对应子类型为对象做一次直接初始化 ⇒ **有一次重载决议**。
选谁、调用符号是什么，是**语义决定的**（定名还是有状态的：同名多参追加 `_N`、
同参数个数再挂类型链）—— 调用点按"类名_类名_N"重算必然和定义点漂移。

**③ 成员名不是全局名**。按 [basic.lookup.unqual]/6，成员的名字在类作用域外不可见。
把成员函数按**裸名**登记进全局符号表，就等于让 `S` 这样的名字在全局"存在"。

编译原理上，这三条是同一个问题的三个面：**名字的身份（identity）必须由作用域决定，
而符号的拼写只由身份派生一次**。learn/49「作用域即身份」是本批的直接前身。

## 2 病灶：同一个身份，三处各算一遍

```text
             ┌────────────────────── 登记名 decl->name ──────────────────────┐
             │ "Outer::Inner" / "Box_int::Inner" / "Outer::Mem_int"          │
             └───────┬──────────────────────────────┬────────────────────────┘
      用户写的 ctor  │                              │  编译器合成的 ctor
      名来自 token   │                              │  ★ 修前：name = decl->name
        "Inner"      │                              │  ⇒ "Outer::Inner"
                     ▼                              ▼
        [register] Outer::Inner::Inner()   [register] Outer::Mem_int::Outer::Mem_int()
                 mangled: ..._Inner               mangled: ..._Outer::Mem_int
                     │                              │
                     └──────────┬───────────────────┘
                                ▼
              CodeGen 调 init-list 的嵌套字段构造：
              ★ 修前：拼 "类名_类名_N"（还要再算一遍）⇒ 与上面两个都不一定相等
```

实测（修前）：

```text
[register] Outer::Inner::Inner(int x) → void    [mangled: Outer::Inner_Inner_1]
[register] Outer::Mem_int::Outer::Mem_int()     [mangled: Outer::Mem_int_Outer::Mem_int]
callq Outer::Inner_Outer::Inner_1                # ← 三者各写各的
as: Error: junk `::Inner_Outer::Inner_1' after expression
```

## 3 设计决策

**① 判据单点 `unqualifiedName`**（`src/ast/type.h`）：限定名的最后一段。
凡是"把类名当函数名用"的地方都走它 —— Sema 合成 ctor/dtor、实例化时给克隆体改名、
CodeGen 兜底拼符号。**理由**：构造函数的裸名与"登记名"是两个用途，
合写在一处就会有一处走偏（这正是本批的起点）。

**② 符号表只登记自由函数**（`registerFunction`）。成员的名字靠
`m_functionMap` + 类的 `methods` 表提供，类内查找走 `findMethodInHierarchy`。
**★ 不能改成"用限定名登记"**：`类名::成员名` 这个键空间是**静态成员**的判据
（`inferMember` / `inferVar` 的 static 分支按它查）—— 成员函数占进去，
`d.g()` 会被误判成 static member（本批实测踩到，见单测 `MemberFunctionIsNotMistakenForStaticMember`）。

**③ 初始化列表的构造目标由 Sema 回填**（`CtorInitializer::isBaseInit` / `ctorSymbol`）。
判据两段：`resolveCtorInitTarget`（这条 init 指向哪个类 —— Pass 1 校验与 Pass 3 回填**共用**）
+ `pickBestByArgs`（与 `T x(args)` 同一套重载决议）。CodeGen 只发符号，兜底也按裸名口径。

```text
processClassDecl (Pass 1)                analyzeFunctionBody (Pass 3)
  校验 init 的名字是否存在  ──共用──▶  回填 init.ctorSymbol
        resolveCtorInitTarget             ↓
                                     pickBestByArgs(候选, 实参类型)
                                          ↓
                                     CodeGen: callq init.ctorSymbol
```

**④ 对比："合成"与"用户写"必须同口径**。clang 合成默认构造时用
`Context.DeclarationNames.getCXXConstructorName(ClassType)` —— **从类的规范类型取名字**，
不是从"带作用域的名字"拼。本实现的对应物就是 `unqualifiedName(decl->name)`。

## 4 clang 源码对照

| clang 位置 | 本实现 | 简化了什么 |
|---|---|---|
| `clang/lib/Sema/SemaDeclCXX.cpp:14356` `CXXConstructorDecl::Create(... getCXXConstructorName(ClassType) ...)`（合成默认构造的名字取自类类型） | `processClassDecl` 里 `defaultCtor->name = unqualifiedName(decl->name)` | 不建 `DeclarationName/DeclarationNameInfo`，名字就是 `std::string`；没有 `setDefaulted` 等属性 |
| `clang/lib/Sema/SemaDeclCXX.cpp:4431` `Sema::BuildMemInitializer`（按名字查基类表 / 成员表，选出被构造的子对象） | `resolveCtorInitTarget` | 不处理匿名联合、委托构造（`D() : D(otherArgs)`）、默认成员初始化式 |
| `clang/lib/Sema/SemaDeclCXX.cpp:16431` `Sema::BuildCXXConstructExpr`（选定构造声明，交给 `CXXConstructExpr`） | `resolveCtorInitList` + `pickBestByArgs` | 无用户自定义转换函数、无列表初始化窄化检查 |
| `clang/lib/CodeGen/CGExprCXX.cpp:612` `CodeGenFunction::EmitCXXConstructExpr`（只读 `CXXConstructExpr` 的声明，不再算符号） | CodeGen 两个构造调用点读 `init.ctorSymbol` / `decl.ctorSymbol` | 兜底保留"类名_裸名_N"约定（回填缺失时才走） |
| `clang/lib/Sema/SemaTemplateInstantiateDecl.cpp:3567` `VisitCXXConstructorDecl` → `VisitCXXMethodDecl`（实例化时按新类名重建 DeclName） | `instantiateClassTemplate` 里用 `unqualifiedName(blueprintName/instanceName)` 改名 | 不做完整的 TreeTransform，只替换类型与表达式 |

## 5 可复现实验

```bash
# ① 核心故障：嵌套类作字段 + 初始化列表（修前 as 报 junk，修后 rc=9）
cat > /tmp/q1.cpp <<'EOF'
struct Outer { struct Inner { int v; Inner(int x) { v = x; } }; Inner m; Outer() : m(9) {} };
int main() { Outer o; return o.m.v; }
EOF
./build-linux/minicc /tmp/q1.cpp -o /tmp/q1 && /tmp/q1; echo "minicc rc=$?"
clang++-18 -std=c++20 /tmp/q1.cpp -o /tmp/q1c && /tmp/q1c; echo "clang rc=$?"

# ② 合成 ctor 的符号形态（修后应为 Outer::Plain_Plain）
echo 'struct Outer { struct Plain { int w; }; }; int main(){ Outer::Plain p; p.w=1; return p.w; }' > /tmp/q2.cpp
./build-linux/minicc /tmp/q2.cpp 2>&1 | grep -E "register\] Outer::Plain|ctor-init"

# ③ 成员名不泄漏成全局名（必须拒收）
echo 'namespace N { struct S { int x; }; } int main(){ S; return 0; }' > /tmp/q3.cpp
./build-linux/minicc /tmp/q3.cpp 2>&1 | tail -2

# ④ 判据突变验证（5 条判据 × 单测/集成侧，全部应变红）
./tools/verify-nested-ctor-mutations.sh
```

## 6 真实版多做了什么、为什么

| 真实 C++ | 本实现 | 为什么 |
|---|---|---|
| 构造函数的名字是注入类名（类内裸名） | ✅ 一样（`unqualifiedName`） | [class.ctor]/1 的定义；类名与函数名是同一个字符串，最容易混 |
| 合成默认构造的名字取自类的规范类型（`getCXXConstructorName(ClassType)`） | ✅ 同思路 | clang 也是"从类取名字"，不是拼作用域前缀 |
| 初始化列表的每条 mem-initializer 走一次完整重载决议 | ⚠ 只做"精确匹配 / 可隐式转换"两档 | 没有用户转换函数、无列表初始化的窄化检查；与 `T x(args)` 共用 `pickBestByArgs` |
| 委托构造 `D() : D(args)` | ⬜ 未支持 | `resolveCtorInitTarget` 只认基类名与字段名，委托构造要另认"目标是自己" |
| 默认成员初始化式（NSDMI） | ⬜ 未支持（B53） | 解析期就没有这条语法 |
| 名字查找区分"类作用域可见 / 全局可见" | ✅ 一样（成员不进全局符号表） | [basic.lookup.unqual]/6；反例见 B60 的"限定名登记"支路 |
| 构造函数模板 / 继承构造函数（`using Base::Base`） | ⬜ 未支持 | 主线 K 的后续项 |

## 7 边界（本批未做，全部登记在案）

| 边界 | 现象 | 登记 |
|---|---|---|
| `p->v`（指针取**字段**） | 生成 `leaq &p` 而不是 `p` 的值 ⇒ 读到错误地址（成员**方法**调用正常） | B50 同族 |
| `m_classLocals` 是平表 | 不同块里同名的类对象互相串栈偏移（块退出不清理） | **B61** |
| 基类是嵌套类（`struct D : Outer::Base`） | 基类名按裸名查不到 ⇒ 拒收 | B43 遗留，未修 |
| 委托构造 / 默认成员初始化式 | 解析期不支持 | 语法面欠账 |
| const / 右值引用形参按值传 | 与构造无关的既有边界 | B58 |

## 8 演进记录

| 日期 | 变化 |
|---|---|
| 2026-11-20 | 首版：判据 `unqualifiedName`（构造函数名字 = 本类裸名，三处落点收口）；`CtorInitializer::{isBaseInit,ctorSymbol}` 由 Sema 回填（`resolveCtorInitTarget` 校验/回填共用 + `pickBestByArgs`）；符号表只登记自由函数；单测 8 例 + 集成 1 份 + 突变 5 条（10/10 实跑变红）；连带登记 B61（`m_classLocals` 平表） |
