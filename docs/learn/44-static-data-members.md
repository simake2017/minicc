# 44 — static 数据成员（[class.static.data]）：挂在类名上的全局变量

> 对应 BUGS.md **B31**。修复前类体内 `static int count;` 在 Parser 被
> 硬性拒收（"static data members are not implemented"），整条特性缺失。
> 本文记：存储模型、三路径查找的判据单点、const 成员的编译期折叠、
> 类外定义的四条校验，以及两个值得记住的坑（折叠时序、命名空间误伤）。

## 1. 理论背景

### 1.1 [class.static.data]/1 —— static 成员不是对象的一部分

C++ 标准对 static 数据成员的定义性陈述：**它不是对象表示的一部分**。
`sizeof(A)` 不含任何 static 成员；`A a, b;` 里 `a.count` 与 `b.count`
是**同一个实体**。它的存储期是静态（static storage duration），
行为与全局变量完全一致 —— 只是【名字】挂在类作用域里，
并且受访问控制与 [class.member.lookup] 的名字查找规则管辖。

```
struct A { static int count; int x; };
A a, b;
                    ┌─ a: [ x ]          ← sizeof(A) == 4（static 不进布局）
  数据段 .data ────→│   count: 0x601030  ← 全局唯一存储，与任何对象无关
                    └─ b: [ x ]
```

对照 clang：static 成员是 `VarDecl` + `isStaticDataMember()`，
IR 里就是一条 `GlobalVariable`，名字 mangled 为
`_ZN1A5countE` —— 与普通全局变量同等待遇。

### 1.2 [class.static.data]/3 —— 类内初始化只放行"常量性"成员

只有 **const 整型族**（以及 constexpr）的 static 成员允许类内初始化；
此时该成员是【编译期常量】，不占用存储（除非被 odr-use 且需要地址）。
非 const 成员写 `static int count = 3;` 直接 ill-formed，报错：

```
error: non-const static data member must be initialized out of line
```

存储只能来自类外定义 `int A::count = 3;`。漏定义又取用 ⇒ 链接期
`undefined reference to 'A::count'`（clang 由 ld 报出；minicc 由
自研链接器等价报出）。

### 1.3 [class.member.lookup]/3 —— 实例字段先于 static 成员

成员查找按【名字】，同一查找域里声明过即了结。派生类的**实例字段**
可以合法**隐藏**基类的 static 成员（它们是不同的实体，可以共存）：

```
struct Base { static int s; };
struct Hides : Base { int s; };   // Hides 的对象布局里有一个 int s
Hides h;  h.s = 100;              // ← 实例字段（布局偏移寻址）
          h.Base::s = 2;          // ← 基类静态成员（全局符号寻址）
```

查找顺序必须"实例字段（扁平布局）→ static 成员（符号表）"，
做反了就是 B14 那一类**静默绑错**。

## 2. 设计决策

### 2.1 存储模型：三处登记，各司其职

类内声明点（processClassDecl）做三件事：

| 登记处 | 内容 | 消费者 |
|---|---|---|
| 符号表 `"A::count"`（全名） | 类型 + 全局符号 | 一切读写路径的决议 |
| `ClassDecl::staticConsts` | 名字 → (值, 类型) | `Cls<Args>::value` 的 isTypeAccess 折叠（既有设施，std::true_type 注入在用） |
| 编译期常量表（registerConstVar） | 全名 → (值, 形态) | 裸名常量传播、`Buf<A::twice>` 的 NTTP 实参重解 |

类内初始化的 const 成员【不发全局符号】 —— 它没有存储，读它必须
折叠成立即数（见 §2.3）。

### 2.2 读取三路径，判据单点（承 B21/B22 的同一教训）

同一个静态成员有三种写法，决议必须收口：

| 写法 | Parser 产物 | 决议路径 |
|---|---|---|
| `A::count` | VarExpr，名字粘成 `"A::count"` | 符号表全名直查 |
| `a.count` | MemberExpr 裸名 | 布局落空 → **符号表回退**（沿基类链 BFS） |
| `a.A::count` | MemberExpr，memberName 粘成 `"A::count"` | resolveQualifiedField |

`a.A::count` 与类内 `Q::v`（B21）共用 `resolveQualifiedField` ——
本实现给它加了**第二查找源**：布局三分条件循环落空后，从限定类起
沿基类链 BFS 查符号表 `"类名::成员"`。**一个函数，两条调用路径**，
隐藏顺序（实例字段先于 static）就写在循环的先后里。

`a.count` 的回退在 inferMember 普通分支：findFields 落空 → 方法查找
**之前** → 同样的符号表 BFS（数据先于方法，与字段一致）。

### 2.3 折叠槽：没有存储的常量怎么读

类内初始化的 `static const int limit = 7;` 没有全局符号。
若 CodeGen 按 `A__limit(%rip)` 寻址 ⇒ 链接期
`undefined reference to 'A__limit'`（探针实测踩到）。

修法（MemberExpr 回填槽的又一例，B10/B20/B21/B22 同一形状）：

```
inferMember（两条 static 分支）
    lookupStaticConst(类名, 成员名) 命中（沿基类链）
        → MemberExpr.staticConstFolded = true
          MemberExpr.staticConstValue  = 7
CodeGen visit(MemberExpr)
    staticConstFolded ⇒ movq $7, %rax    # 立即数，无存储
```

`A::limit`（VarExpr 全名）走既有的常量传播：折叠器查常量表，
VarExpr 原地替换成字面量。成员函数里的裸名 `limit` 见 §2.4 的坑。

### 2.4 坑一：折叠先于改名（时序）

ReturnStmt 的 visit 是【先 fold 后 infer】：

```
return count + limit;     // 成员函数体
         │      │
  fold 时它还叫裸名 "limit" —— 常量表里的键是 "A::limit"，查不到！
  infer 时 inferVar 第 2.5 步才改名 —— 晚了
```

修法：constResolver 回调补第 ② 路 —— 直查落空时，按
**当前类 + 基类链 BFS** 补查 `"类名::裸名"`（与 inferVar 2.5 同一
判据形状）。这也是本项目"求值器回调"分层形状的又一次应用
（DecltypeEvaluator / MemberTypeResolver / AliasTemplateResolver /
constResolver —— 第四次出现的那一个，此处给它加了类作用域回退）。

### 2.5 坑二：命名空间成员定义被误伤（判据收窄）

类外定义校验第一版的判据是"名字含 `::`" —— 当场把
`namespace Math { int g_factor = 2; }` 的成员定义拦下
（`'Math' is not a class, namespace, or enumeration`，
decl_01 集成用例变红）。`int Math::g_factor` 与 `int A::count`
**同形不同义**：前者是命名空间全局变量（旧设施），后者才是
static 成员定义。判据收窄为：**限定者在类注册表里**才走 static
校验。同一个"同形不同义"陷阱，B30 的构造表达式/CTAD 前瞻也踩过
（靠 `->` 区分推导指引）。

### 2.6 类外定义的四条校验（processGlobalVarDecl 头部）

| # | 校验 | clang 文案（逐字对齐） |
|---|---|---|
| ① | 限定者是类 | `'x' is not a class, namespace, or enumeration` |
| ② | 类里声明过 | `no member named 'other' in 'A'` |
| ③ | 定义类型 **equals** 声明类型 | `redefinition of 'count' with a different type: 'long' vs 'int'` |
| ④ | 全程序只定义一次（ODR） | `redefinition of 'count'` |

★ ③ 用 equals 而非 typeCompatible：后者是"可隐式转换即兼容"，
`long`/`int` 会被判兼容而**静默放行** —— 负向全绿掩盖缺陷，
探针校准时实测抓到（第一版 c5 用例编译通过，clang rc=1）。

### 2.7 模板类静态成员

类模板蓝图的 staticDataMembers 在**实例化时**克隆到实例类
（instantiateClassTemplate 步骤 5.7：类型 substituteType、
初始化式 cloneExpr、constexpr 位原样搬）。实例类走 processClassDecl
⇒ 常量表/符号注册自然发生 —— 与 C++ 语义一致（蓝图本身不过
processClassDecl）。`C<int>::cid` 复用既有的 isTypeAccess 折叠
（foldStaticConst，与 std::true_type::value 同一条路）。

## 3. clang 源码对照表

| clang（llvm-project/） | 本实现 | 简化 |
|---|---|---|
| `Sema::ActOnCXXMemberDeclarator` 识别 static 数据成员 | parser.cpp 类体循环 else 分支收 `static` + 类型 + 名 + 可选初始化式 | clang 由完整声明符反推 Decl 种类；本项目直接按文法收 |
| `Sema::CheckStaticMemberInitialization`（[class.static.data]/3 守卫） | processClassDecl 静态成员循环的 const-整型族判据（剥 Const 再查家族） | clang 还放行字面量类类型（C++17 inline 变量语境）；本实现只整型族 |
| `VarDecl::isStaticDataMember` + 全局符号发射 | 符号表 `"A::count"` + emitGlobalVars 走既有全局变量路径 | 同构 |
| `Expr::EvaluateAsInt`（常量上下文的折叠） | lookupStaticConst + staticConstFolded 回填槽；VarExpr 走常量传播 | clang 是通用常量求值器；本实现只整型 |
| `Sema::ActOnCXXDeclarator` 判 static 成员定义 + 四校验 | processGlobalVarDecl 头部 isStaticMemberDef 分支 | clang 部分校验在模板实例化后重做；本实现一遍 |
| Itanium mangling `_ZN1A5countE` | asmSymbol("A::count") → `A__count` | 自有方案，全项目统一净化 |

## 4. 可复现实验

```bash
# ① 正例（13 检查点，运行钉 0；clang 对拍一致）
./minicc tests/lang/test_basics_13_static_member.cpp -o /tmp/t13 && /tmp/t13; echo "run=$?"
clang++-18 -std=c++20 tests/lang/test_basics_13_static_member.cpp -o /tmp/t13c && /tmp/t13c; echo "clang run=$?"

# ② 五条负向文案（全部与 clang++-18 逐字相同）
./minicc tests/lang/test_basics_14_static_init_out_of_line_error.cpp -o /tmp/x 2>&1 | grep ERROR
#   → non-const static data member must be initialized out of line
./minicc tests/lang/test_basics_18_static_type_mismatch_error.cpp -o /tmp/x 2>&1 | grep ERROR
#   → redefinition of 'count' with a different type: 'long' vs 'int'

# ③ 漏类外定义 ⇒ 链接期响亮报错（与 clang 的 ld 报错同形态）
printf 'struct A { static int count; };\nint main() { return A::count; }\n' > /tmp/c6.cpp
./minicc /tmp/c6.cpp -o /tmp/c6
#   → [LINK ERROR] undefined reference to 'A__count'

# ④ 全量回归
ctest --test-dir build-linux -j6 | tail -1     # 100% tests passed
./logdiff.sh diff                               # ✅ 日志逐字节零差异
```

## 5. 关键过程 ASCII 图：`a.A::limit` 的决议与折叠

```
源码        a.A::limit
                │
Parser      MemberExpr(object=VarExpr(a), memberName="A::limit")   ← :: 粘进成员名
                │
Sema        inferMember
            ├─ rfind("::") ⇒ qualifier="A", member="limit"
            ├─ resolveQualifiedField("A", "A", "limit")
            │   ├─ 布局三分条件循环：A 无实例字段 limit ⇒ 落空
            │   └─ 第二源：符号表 BFS "A::limit" ⇒ 命中（Symbol, type=const int）
            ├─ resolvedStaticSymbol = "A::limit"
            └─ lookupStaticConst("A", "limit") ⇒ 7（类内初始化登记过）
               staticConstFolded = true, staticConstValue = 7
                │
CodeGen     emitExpr(a)                    # [expr.ref]：对象照常求值，值弃用
            movq $7, %rax                  # 静态常量折叠 .limit（无存储）
```

## 6. 边界与未做

| 项 | 现状 |
|---|---|
| 非整型 static 成员（浮点 / 类类型 / 指针 / 数组） | 声明可解析（类型经 resolveType），数组后缀未挂在 static 分支上；类类型成员的构造/析构不发射 —— 用到再补 |
| 类内初始化非整型 const（如 `static const double pi = 3.14;`） | 按 [class.static.data]/3 字面拦下（教学级收窄，C++ 其实允许且要类外定义） |
| 模板 static 成员的类外定义 `template<class T> int C<T>::tag = 1;` | **未做**：Parser 的类外定义限定名循环只收裸 `::` 链，`<T>` 形态会解析失败（响亮拒收而非静默错） |
| 对常量成员的赋值 `a.A::limit = 9;` | 折叠槽只管读；写路径照常发 RIP 存储写 —— const 写检查整体是另一条未做的线 |
| `foldStaticConst` 的挂点 | 只在【变量声明初始化式】（processVarDeclStmt）；表达式语句 / return 里的 `C<int>::cid` 会报 Undefined variable（老设施边界，非本批引入） |
| odr-use 探测 | 未做：常量成员的取地址 / 引用绑定应要求类外定义，现一律按编译期常量折掉 |
| 多态类 + static 成员的组合 | vptr 布局与 static 无关（不进布局），无交互 |
