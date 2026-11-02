# 08 构造/析构与顶层声明体系（主线 A & 语法扩展）

> 主线 A：构造函数 / 成员初始化列表 / 虚析构函数 / new & delete 表达式 / 全局变量 / 枚举 / 命名空间 / 类型别名。
> C++ 对象生命周期 [basic.life] 是面向对象编译实现的核心基石——"分配空间 ≠ 构造对象，析构对象 ≠ 释放空间"。

---

## §0 定位与能力清单

面向对象落到机器上的第一课是**对象的生死**：`new` 只是要了一块内存，
`delete` 只是还了一块内存——真正让那块内存"成为一个对象"的，是构造函数；
真正让它"不再是一个对象"的，是析构函数。本文讲 minicc 怎么把这条链子接通，
以及顶层声明体系（全局变量 / 枚举 / 命名空间 / 类型别名）怎么分派。

| 能力 | 一句话 | 状态 |
|---|---|---|
| [class.ctor] 构造函数 | 语义期检测类未声明则注入默认无参构造 | 已实现 |
| [class.base.init] 成员初始化列表 | 构造时逐个初始化成员（`Point(int px) : x(px) {}`） | 已实现 |
| [class.dtor] 析构函数 | 显式声明或由语义期合成默认析构 | 已实现 |
| [class.dtor] 虚析构 + 多态 delete | `delete` 基类指针时经 vtable[0] 派发到派生类析构 | 已实现 |
| [expr.new] / [expr.delete] | new = 分配 + 构造；delete = 析构 + 释放 | 已实现 |
| [dcl.dcl] 顶层声明分派 | 全局变量 / 函数 / 枚举 / 命名空间 / 类型别名 / 模板各走一支 | 已实现 |
| [dcl.enum] 枚举 | 非限定枚举项提升到外层作用域，`enum class` 限定在自身作用域 | 已实现 |
| [dcl.typedef] 类型别名 | `using` / `typedef` 注册为 `SymbolKind::Type`，语义期透明展开 | 已实现 |
| [namespace.def] 命名空间 | 符号名前缀化 + 命名空间作用域链 | 部分（命名空间变量在块作用域不可见、嵌套命名空间支持不完整，见 learn/48 / learn/49） |
| [class.base.init] 按**声明顺序**初始化 | clang 严格按成员在类内的声明顺序初始化，与列表书写顺序无关 | 未做（本实现按列表书写顺序依次发射） |
| [class.dtor] deleting destructor 变体 | clang 的 D0（deleting）/ D1（complete）两个析构变体 | 未做（本实现单析构槽位 + `free`） |
| [class.dtor] 基类析构链 | 派生类析构完自动析构基类 | 未做 |
| `.bss` 未初始化零值优化 | clang 把未初始化全局放进 .bss | 未做（本实现统一在 `.data` 段发射初始值） |
| 拷贝构造 / 移动构造 / 赋值运算符 | | 未做 |
| 类外成员定义 `int C::f() const {}` | | 未做 |
| 成员函数的默认实参 | | 未做 |

## ① 理论背景

**标准依据**：[class.ctor]、[class.dtor]、[class.base.init]、[expr.new]、[expr.delete]、[dcl.dcl]、[dcl.enum]、[namespace.def]、[dcl.typedef]。

- **对象生命周期与动态内存管理**：
  - `new T(args)`：在编译底层分解为两步：
    1. **内存分配**：`void* p = malloc(sizeof(T));`
    2. **对象构造**：`T::T(p, args);`（将分配所得的内存指针作为隐藏的 `this` 形参传入构造函数）
  - `delete p`：在编译底层分解为两步：
    1. **对象析构**：`p->~T();`（若声明为 `virtual ~T()`，则经由 `_vptr` 索引虚函数表实现多态析构派发）
    2. **内存回收**：`free(p);`
- **成员初始化列表（Member Initializer List）**：
  - 构造函数执行序：**基类构造 → 注入 `_vptr` → 成员初始化列表 → 构造函数体语句**。
  - 为什么要在进入函数体前执行初始化列表？因为 const 成员、引用成员必须在声明时完成绑定，不可在函数体内通过赋值后绑定。
- **虚析构函数（Virtual Destructor）与多态释放**：
  - Itanium C++ ABI 规范：基类指针 `Base* b = new Derived()`，当执行 `delete b;` 时，若 `~Base()` 为虚析构，则析构函数作为虚表（vtable）中的第一个槽位条目被调用，派生类的析构函数会在执行完自身后自动析构基类，最后调用 `free`，避免派生类资源泄漏与切片未完全清理。
- **顶层声明扩展体系**：
  - **全局变量（GlobalVarDecl）**：分配至 `.data` 数据段，采用 x86-64 RIP 相对寻址（`var(%rip)`），在链接阶段完成绝对重定位。
  - **作用域解析与命名空间（NamespaceDecl）**：将符号名修饰为限定名（`Math::add` $\to$ `Math::add` 并在符号表和函数注册表中单向索引）。
  - **枚举声明（EnumDecl / Scoped Enum）**：非限定 `enum` 将枚举项提升至外层全局符号表，`enum class` 将枚举项限定在其命名空间内。
  - **类型别名（TypeAliasDecl）**：`using` / `typedef` 注册为 `SymbolKind::Type`，语义分析阶段通过规范化类型展开（Canonical Type Resolution）透明剥离别名层。

---

## ② 关键流程与内存布局 ASCII 图

### 1. `new` 与 `delete` 对象构造/析构执行链

```
┌─────────────────────────────────────────────────────────┐
│                    new Point(3, 4)                      │
└─────────────────────────────────────────────────────────┘
                            │
                            ▼
           1. call malloc(sizeof(Point)) → %rax
                            │
                            ▼
           2. 移动 %rax 到 %rdi (作为 this 指针)
              传参: %esi = 3, %edx = 4
                            │
                            ▼
           3. call Point_Point (%rdi, %esi, %edx)
              ┌─────────────────────────────────────┐
              │ Point_Point(this, px, py):          │
              │   this->_vptr = Point_vtable        │
              │   this->x = px                      │
              │   this->y = py                      │
              │   body { ... }                      │
              │   return this                       │
              └─────────────────────────────────────┘
                            │
                            ▼
           4. %rax = this (构造完成的完整对象指针)
```

```
┌─────────────────────────────────────────────────────────┐
│                       delete ptr                        │
└─────────────────────────────────────────────────────────┘
                            │
                            ▼
           1. 检查虚析构: class.hasVTable?
              ├─ 是: 经由 vtable[0] 多态分派:
              │     movq (%rdi), %rax     ; 取 _vptr
              │     callq *(%rax)         ; 派发到 Derived_dtor
              └─ 否: 直接调用静态析构标号:
                    callq Point_dtor
                            │
                            ▼
           2. 调用 free(ptr) 归还堆内存
```

---

## ③ 设计决策

| 决策点 | 本编译器设计选择 | 理论权衡与理由 |
|---|---|---|
| **构造函数返回约定** | 构造函数将 `%rdi` (`this`) 作为 `%rax` 返回 | 符合 System V AMD64 ABI 规范，便于链式初始化与 `new` 表达式直接接管指针 |
| **隐式构造/析构函数合成** | 语义分析阶段检测，若类未显式声明，则自动注入默认无参构造与默认析构 | C++ 标准 [class.default.ctor]：保证每个类在 `new` 和 `delete` 下均有明确的生命周期入口 |
| **虚析构在 vtable 中的槽位** | 固定将虚析构函数置于 `vtable[0]` | 统一单继承体系下多态析构派发索引，简化 CodeGen 的动态发射逻辑 |
| **全局变量寻址模式** | 统一采用 `var_name(%rip)` 相对寻址 | 满足 x86-64 现代操作系统的位置无关代码（PIC / PIE）规范 |
| **类型别名展开机制** | 语义分析阶段透明别名规范化 `resolveType()` | 保持 AST 节点原始结构便于诊断，在类型兼容性检查和符号注册时按规范底层类型展开 |

---

## ④ Clang 源码对照表

| 机制 / 阶段 | 本实现位置 | Clang 对应源码实现 | 教学级简化说明 |
|---|---|---|---|
| **顶层声明多路分派** | `src/parser.cpp:parseDeclaration` | `clang/lib/Parse/ParseDeclTop.cpp:ParseTopLevelDecl` | Clang 支持声明说明符复杂的自由组合；本实现使用 LL(1) + 局部试探解析区分全局变量与函数 |
| **构造函数与初始化列表** | `src/parser.cpp:parseConstructorDecl` & `src/codegen.cpp:emitFunction` | `clang/lib/Sema/SemaDeclCXX.cpp:ActOnMemInitializers` | Clang 严格按类成员在类内的声明顺序执行初始化（无论列表顺序如何）；本实现按列表书写顺序依次发射 |
| **虚析构函数与多态 delete** | `src/semantic_analyzer.cpp:processDeleteStmt` & `src/codegen.cpp:emitDelete` | `clang/lib/CodeGen/CGExprCXX.cpp:EmitDeleteExpr` | Clang 包含 deleting destructor 与 complete object destructor 两个变体；本实现简化为单析构槽位 + `free` |
| **全局变量发射** | `src/codegen.cpp:generate` (`.data` 段) | `clang/lib/CodeGen/CodeGenModule.cpp:EmitGlobalVarDefinition` | Clang 支持 BSS 段未初始化零值优化；本实现统一在 `.data` 段发射初始值 |

---

## ⑤ 实验与验证手册

### 1. 运行完整单元测试集
```bash
cd /home/magene/runtime/cppproject/mycompiler/build-linux
cmake --build . && ctest --output-on-failure
```

### 2. 构造函数与初始化列表编译实验
```bash
./minicc ../tests/test_ctor_01_basic.cpp -o /tmp/ctor01.s
# 观察生成的汇编中 Point_Point 构造函数及字段初始化指令：
grep -A 15 "Point_Point:" /tmp/ctor01.s
```

### 3. 虚析构函数多态析构与 delete 实验
```bash
./minicc ../tests/test_ctor_02_virtual_dtor.cpp -o /tmp/dtor02.s
# 观察虚析构函数在 vtable 槽位中的安装与 delete 处的动态派发：
grep -A 10 "delete" /tmp/dtor02.s
```

### 4. 全局变量与命名空间限定名实验
```bash
./minicc ../tests/test_decl_01_global_and_namespace.cpp -o /tmp/decl01.s
# 观察 .data 段全局变量发射与 Math::scale 符号调用：
grep -A 10 ".data" /tmp/decl01.s
```

## ⑥ 真实版多做了什么、为什么

★ 素材来源：本文 §④ Clang 源码对照表的「教学级简化说明」列
+ `docs/BUGS.md` B9/B10/B11/B12/B13/B14/B15/B20/B22/B34 + CLAUDE.md「未做」清单。
**未查证的写「（本项目未查证）」**。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `ParseTopLevelDecl` 支持声明说明符的**复杂自由组合**（`static inline const volatile ...`） | 本项目用 LL(1) + 局部试探解析区分"全局变量 vs 函数"；说明符组合的自由度被砍到教学够用 |
| `SemaDeclCXX.cpp:ActOnMemInitializers` 严格按成员在类内的**声明顺序**初始化 | 本实现按**列表书写顺序**依次发射——写错顺序不会被编译器纠正。教学取舍：顺序错了语义也错，但错误可见（值不对），不会静默 |
| `CGExprCXX.cpp:EmitDeleteExpr` 的 deleting destructor（D0）/ complete object destructor（D1）**两个变体** | 本实现单析构槽位 + `free`；少一个变体 ⇒ 少一套槽位身份判据（`C1/C2/D0/D1/D2` 的区分），B20/B22 的教训已经说明槽位判据极易写错 |
| `CodeGenModule.cpp:EmitGlobalVarDefinition` 的 `.bss` 段未初始化零值优化 | 本实现统一在 `.data` 段发射初始值——少一段 ELF 布局规则（段合并/对齐），代价是产物里带着一堆显式 0 |
| 派生类析构后**自动析构基类**（析构链） | 未做：本项目的析构只调自己那一层 |
| `struct X : Base` 的默认继承级别 | clang 默认 **public**（[class.derived]/2）；本项目此前按 private 拒收，是**拒收合法程序**的 bug（B9），已修为 public |
| 拷贝构造 / 移动构造 / 赋值运算符的完整生成 | 未做：一旦要做，就要处理"平凡拷贝 vs 用户定义拷贝"的判据（B34 踩过：平凡拷贝判据用 `equals` 不成立 ⇒ 拷贝初始化静默退化成默认构造） |
| 成员函数的默认实参、类外成员定义（`int C::f() const {}`） | 未做：两者都要在符号表里维护"声明与定义分离"的第二条路径 |
| `enum class` 的底层类型显式指定（`enum class E : unsigned char`） | 未做 |
| 命名空间变量的**块作用域可见性** | 部分：clang 里命名空间成员在块作用域**可见**（经 using-directive 规则），本项目"命名空间变量在块作用域不可见"是 B37 查实、到 B38 仍未修的缺口（learn/48） |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-08-27 | 主线 A 落地：构造函数 / 初始化列表 / 虚析构 + delete / 全局变量 / 枚举 / 命名空间 / 类型别名 | 主线 A 的验收点（tests/unit/test_decl_and_ctor.cpp + tests/test_ctor_01..02 + tests/test_decl_01） |
| 2026-09-10 | 主线 B/G 批次：vtable 与 RTTI 打通，虚析构槽与 typeinfo 符号同源 | 链接器要按**裸字符串**匹配槽里的符号 ⇒ 槽名与定义名必须同源（B20，learn/13） |
| 2026-09-24 | 多继承成员/布局五条 bug 修复（B11~B15）+ 带参成员方法符号回填（B10） | ① vptr 压字段是真 miscompile ② 继承来的成员方法查不到 ③④⑤ 把显示名当成了索引；`c.f(1)` 一律 `undefined reference to 'C_f'`——**编译期全程绿灯** |
| 2026-10-08 | vtable 槽符号与定义点同源（B20） | 带参虚函数的 vtable 槽写 `.quad A_f`、定义却是 `.globl A_f_1` ⇒ 链接期 `undefined reference` |
| 2026-10-09 | 成员方法身份收口为「名字 + 形参类型」（B22 + B20 缺陷 c） | 同名同个数的重载曾静默取第一个并被误标 virtual ⇒ 运行返回 255 |
| 2026-10-27 | const 类对象端到端修复（B34） | `const` 包壳让类性判定全部失准：成员访问误报 + 掉标量路径 **SIGSEGV** + 拷贝静默退化 |
