# 13 - C++ 名字修饰（Name Mangling）

## §0 定位与能力清单

汇编器与链接器只认**唯一**的裸符号名，而 C++ 允许同名重载、类成员、模板——
于是编译器必须把"标识符 + 类型签名"编码成一个全局唯一的字符串。
这就是名字修饰（name mangling）。本文讲 Itanium C++ ABI 的编码规则，
以及 minicc 的做法：**类符号（vtable / typeinfo）用真 ABI，虚函数名用简化格式**——
以及正是这个简化，逼出了"判据单点"这条铁律（§13.4.3）。

| 能力 | 一句话 | 状态 |
|---|---|---|
| Itanium ABI 前缀 | `_Z` 普通 / `_ZN` 嵌套 / `_ZTV` vtable / `_ZTI` typeinfo / `_ZTS` typeinfo name | 已实现（`_Z` / `_ZN` / `_ZTV` / `_ZTI`；`_ZTS` 未单独发射） |
| 长度前缀编码 | 标识符长度 + 标识符本身（`Dog` → `3Dog`） | 已实现 |
| 基本类型编码 | 整型家族 / 字符 / 浮点 / `void` 的编码 | 已实现（12 种逐字符与 clang 相同，见 learn/35） |
| 指针 / 引用 / const 修饰 | `P` / `R` / `K` | 已实现 |
| 函数签名编码 | `_Z` + 名 + 参数类型表 | 已实现 |
| 嵌套名 | `_ZN` + 类名 + 方法名 + `E` + 参数类型 | 已实现 |
| 模板实参编码 | `I ... E` | 已实现 |
| 非类型模板实参 | `L` + 类型 + 值 + `E`（`_Z3BufILi4EE`） | 已实现 |
| 作用域进符号名 | 命名空间模板实例名带完整作用域前缀（`N::B_int`） | 已实现（learn/49，B38） |
| 成员函数符号名 | | 部分（简化为 `类名_方法名[_<形参个数>]`；同类有同名同个数兄弟时再挂形参类型链） |
| 构造函数 / 析构函数的 C1/C2/D0/D1/D2 变体 | | 未做（本项目单析构槽位 + `free`） |
| 局部名字 / lambda 闭包类型（`Z` 段） | | 未做 |
| 运算符函数的编码（`pl` / `mi` …） | | 未做 |
| ABI tag（修饰符段 `B`） | | 未做 |
| MSVC 修饰方案 | | 未做（只做 Itanium——Linux/macOS 的事实标准） |
| 修饰名的**可逆性** | `c++filt` 能把 `_ZN3Dog5speakEv` 还原成 `Dog::speak()` | 部分（类符号可逆；简化虚函数名本就不可逆） |

## 13.1 什么是名字修饰？

名字修饰（Name Mangling）是编译器将 C++ 源代码中的标识符（函数名、类名、变量名等）转换为链接器能识别的唯一符号名的过程。

### 为什么需要名字修饰？

C 语言不允许函数重载，所以函数名就是符号名：

```c
int add(int a, int b) { return a + b; }
// 编译后符号表：add
```

C++ 允许函数重载，但汇编器和链接器只认唯一的名字：

```cpp
int add(int, int);      // 两个都叫 add
double add(double, double);  // 链接器怎么区分？

// 编译后符号表：
// _Z3addii    ← int add(int, int)
// _Z3addd      ← double add(double, double)
```

## 13.2 Itanium C++ ABI Mangling

GCC 和 Clang 使用 **Itanium C++ ABI** 的名字修饰方案。这是 Linux 和 macOS 上的事实标准（Windows MSVC 使用不同的方案）。

### 13.2.1 基本前缀

所有 C++ 符号以 `_Z` 开头：

| 前缀 | 含义 | 示例 |
|------|------|------|
| `_Z` | 普通函数/变量 | `_Z3foov` |
| `_ZN` | 嵌套名（类成员、命名空间） | `_ZN3Dog5speakEv` |
| `_ZTV` | vtable（虚函数表） | `_ZTV3Dog` |
| `_ZTI` | typeinfo（RTTI 类型信息） | `_ZTI3Dog` |
| `_ZTS` | typeinfo name（类型名字符串） | `_ZTS3Dog` |

### 13.2.2 编码规则

**长度前缀编码**：标识符长度 + 标识符本身

```
Dog       → 3Dog
speak     → 5speak
Animal    → 6Animal
my_func   → 7my_func
```

**基本类型编码**：

| 类型 | 编码 | 类型 | 编码 |
|------|------|------|------|
| `void` | `v` | `int` | `i` |
| `bool` | `b` | `long` | `l` |
| `char` | `c` | `long long` | `x` |
| `short` | `s` | `unsigned` | `j` |
| `float` | `f` | `unsigned long` | `m` |
| `double` | `d` | `unsigned long long` | `y` |

**指针和引用**：

| 类型修饰 | 编码 | 示例 |
|---------|------|------|
| `T*` | `P` + T | `int*` → `Pi` |
| `T&` | `R` + T | `int&` → `Ri` |
| `const T` | `K` + T | `const int` → `Ki` |

### 13.2.3 函数签名编码

格式：`_Z` + [嵌套] + 函数名 + 参数类型列表

```
void foo()
→ _Z3foov
  │  │  └─ v = void（无参数）
  │  └─ 3foo = 函数名长度+名字
  └─ _Z = C++ 符号前缀

int add(int, int)
→ _Z3addii
  │  │  └─ ii = int, int
  │  └─ 3add = 函数名
  └─ _Z

double compute(float, double)
→ _Z7computedf
       │  └─ fd = float, double（注意：参数顺序是从左到右）
       └─ 7compute = 函数名
```

### 13.2.4 嵌套名（类成员函数）

格式：`_ZN` + 类名 + 方法名 + `E` + 参数类型

```
Dog::speak()
→ _ZN3Dog5speakEv
  │ │  │   │    │  └─ v = void 参数
  │ │  │   │    └─ E = 嵌套结束标记
  │ │  │   └─ 5speak = 方法名
  │ │  └─ 3Dog = 类名
  │ └─ N = 嵌套名开始
  └─ _Z

Animal::speak(int)
→ _ZN6Animal5speakEi
  │ │    │   │    │ └─ i = int 参数
  │ │    │   │    └─ E = 嵌套结束
  │ │    │   └─ 5speak
  │ │    └─ 6Animal
  │ └─ N
  └─ _Z
```

**多层嵌套**（命名空间或嵌套类）：

```
std::vector<int>::push_back(int)
→ _ZNSt6vectorIiSaIiEE9push_backEi
  │  │  │      │  │     │ │       └─ i = int 参数
  │  │  │      │  │     │ └─ E = push_back 结束
  │  │  │      │  │     └─ 9push_back
  │  │  │      │  └─ E = vector<int> 结束
  │  │  │      └─ I...E = 模板参数 int
  │  │  └─ 6vector
  │  └─ 2std = std 命名空间
  └─ _ZN
```

### 13.2.5 模板实例化

格式：`_Z` + 模板名 + `I` + 模板参数 + `E`

```
template<typename T> T twice(T x);

twice<int>
→ _Z5twiceIiE
  │ │    │ │ └─ E = 模板参数结束
  │ │    │ └─ i = int
  │ │    └─ I = 模板参数开始
  │ └─ 5twice
  └─ _Z

twice<double>
→ _Z5twiceIdE
            └─ d = double
```

**模板参数编码**：

| 模板参数 | 编码 | 示例 |
|---------|------|------|
| 类型 | 类型编码 | `int` → `i` |
| 非类型值 | `L` + 类型 + 值 + `E` | `42` → `Li42E` |
| 模板模板参数 | 完整编码 | `std::vector` → `St6vector` |

### 13.2.6 特殊符号

**构造函数/析构函数**：

```
Dog::Dog()
→ _ZN3DogC1Ev    // C1 = 完整对象构造
→ _ZN3DogC2Ev    // C2 = 基类子对象构造

Dog::~Dog()
→ _ZN3DogD1Ev    // D1 = 完整对象析构
→ _ZN3DogD2Ev    // D2 = 基类子对象析构
→ _ZN3DogD0Ev    // D0 = 删除析构（调用 delete）
```

**虚函数表（vtable）**：

```
Dog 的 vtable
→ _ZTV3Dog
  │  │  └─ 3Dog = 类名
  │  └─ TV = vtable
  └─ _Z
```

**类型信息（typeinfo）**：

```
Dog 的 typeinfo
→ _ZTI3Dog
  │  │  └─ 3Dog = 类名
  │  └─ TI = typeinfo
  └─ _Z
```

**类型信息名（typeinfo name）**：

```
Dog 的 typeinfo name（字符串 "Dog"）
→ _ZTS3Dog
  │  │   └─ 3Dog = 类名
  │  └─ TS = typeinfo name
  └─ _Z
```

## 13.3 实际示例

### 13.3.1 简单函数

```cpp
void hello();
int add(int, int);
double compute(float, double, char);
```

```
_Z5hellov
_Z3addii
_Z7computedfdc
```

### 13.3.2 类成员函数

```cpp
class Animal {
public:
    void speak();
    int eat(int);
};
```

```
_ZN6Animal5speakEv
_ZN6Animal3eatEi
```

### 13.3.3 重载函数

```cpp
void print(int);
void print(double);
void print(const char*);
```

```
_Z5printi
_Z5printd
_Z5printPKc    // P = 指针, K = const, c = char
```

### 13.3.4 继承与虚函数

```cpp
class Base {
public:
    virtual void foo();
};

class Derived : public Base {
public:
    void foo() override;
};
```

```
Base:
  _ZTV4Base      // vtable
  _ZTI4Base      // typeinfo
  _ZN4Base3fooEv // Base::foo()

Derived:
  _ZTV7Derived   // vtable
  _ZTI7Derived   // typeinfo
  _ZN7Derived3fooEv // Derived::foo()
```

### 13.3.5 模板

```cpp
template<typename T>
T max(T a, T b) { return a > b ? a : b; }

// 实例化
int x = max(3, 5);
double y = max(3.14, 2.71);
```

```
_Z3maxIiET_T_      // max<int>(int, int)
_Z3maxIdET_T_      // max<double>(double, double)
```

## 13.4 minicc 的实现

minicc 对 **类符号**（vtable、typeinfo）使用真实的 Itanium ABI mangling，但对 **虚函数名** 使用简化格式以便阅读。

### 13.4.1 类符号（符合标准）

```cpp
// semantic_analyzer.cpp:1048
std::string typeinfoName = "_ZTI" + std::to_string(name.size()) + name;
// Animal → _ZTI6Animal

// semantic_analyzer.cpp:2798
std::string vtable_name = "_ZTV" + std::to_string(name.size()) + name;
// Dog → _ZTV3Dog
```

### 13.4.2 虚函数名（简化版）

```cpp
// semantic_analyzer.cpp 的 memberMethodSymbolName()（单点判据，见 13.4.3）
memberMethodSymbolName(owner, name, paramCount, earlierSameNameCount)
// Dog::speak     → Dog_speak      （而非 _ZN3Dog5speakEv）
// Dog::speak(int)→ Dog_speak_1
```

**为什么简化？**

- 学习目的：`Dog_speak` 比 `_ZN3Dog5speakEv` 更易读
- 避免复杂性：真实 mangling 需要处理重载、参数类型、嵌套等
- 自包含：minicc 生成的汇编用这些简化名，不需要与外部 C++ 代码链接

### 13.4.3 成员方法符号名的【单点】判据（BUGS.md B20 缺陷 a）

**规则**（`semantic_analyzer.cpp` 的 `memberMethodSymbolName`）：

| 情形 | 符号名 | 例 |
|---|---|---|
| 析构函数 | `类名_dtor` | `Dog_dtor` |
| 无参、且同类无先注册的同名方法 | `类名_方法名` | `Dog_speak` |
| 带形参，或同类已有先注册的同名方法 | `类名_方法名_<形参个数>` | `Dog_speak_1` |
| **同类里还有同名同个数的兄弟**（B22） | 上个形态再挂 `_<形参类型链>` | `C_f_1_int` / `C_f_1_S` |
| 同上、零参（类型链为空） | 挂 `_void` 占位 | `C_f_0_void` |

**"形参类型链"**是把每位形参的类型可读名拼起来（`paramTypeChain`），
每段过一遍全项目唯一的实例名清洗器 `sanitizeSymbolChars`
（`*`→`P`、`&`→`R`、`-`→`N`、`:`→`C`、`< > ,`→`_`），例如
`int` → `int`、`S*` → `S_P`、`Box<int>` → `Box_int`。

**为什么只在"有兄弟"时才挂**：挂了就动了**全部**既有产物（logdiff 基线 116 个集成用例
逐字节比对）。实测"有兄弟才挂"策略下既有用例**零漂移** —— 这条约束是硬的，
不是审美偏好。

★ 这里仍是**拿给人看的字符串当机器用的键**（`Type::toString()` 不是单射，
见 [docs/learn/23](23-cv-qualifier-position.md)）；真按 Itanium mangling 做才是正解，
教学版从简 —— 但**清洗器与判据必须单点**，这是 B20/B22 反复交学费换来的。

**为什么必须收口到一个函数**：这个名字会被**两个地方分别产出** ——

1. **定义点** `registerFunction`（Pass 2）：写 `decl->mangledName`，决定 `.globl <sym>`
2. **vtable 条目点** `processClassDecl`（Pass 1）：写 `entry.mangledName`，决定槽里的 `.quad <sym>`

链接器只按**裸字符串**比对这两者。任何一边的规则漂一点，就是"编译期全绿、
链接期 `undefined reference`"。对照 clang：定义与引用共用同一个
`ItaniumMangleContext::mangleName`，结构上不可能不一致；minicc 靠"判据单点 +
两处调用同一函数"达到同样效果。

**踩过的坑（B20 缺陷 a）**：vtable 条目点原本自己硬拼 `类名_方法名`（**不带**
`_<形参个数>` 后缀），于是

```text
struct A { virtual int f(int x); };
定义点产出  .globl A_f_1
vtable 槽    .quad A_f          ← 这个符号没人定义
⇒ [LINK ERROR] undefined reference to 'A_f'
```

**为什么之前的测试全绿**：无参虚函数两侧算出的都是 `A_f`，规则恰好一致 ——
缺口只在"带参虚函数"这条路径上，而此前没有任何用例写过带参虚函数。

**槽位身份的另一半（B20 缺陷 c，已修）**：vtable 槽位匹配**不能只比裸名** ——
`virtual int f(); int f(int);` 里后者会"认领"前者的槽位并被误标成 virtual
（编译全绿、运行返回 255）。修法是给 `VTableEntry` 加 `signature`（同一条
`paramTypeChain`），判据变成（裸名 + 形参类型）—— 这正是 [class.virtual]/2 的覆写判据：
**裸名只是查找的入口，不是身份的全体**。

对称地，符号定名那一侧也要跟上（否则两个同签名个数的重载在汇编期撞名 `C_f_1`，
as 报 `symbol is already defined`）：这就是 B22，四处落点见下表。

**"身份 = 名字 + 形参类型"的四个落点**（B22 修复后）：

| # | 落点 | 函数 |
|---|---|---|
| ① | 汇编符号名 | `memberMethodSymbolName` |
| ② | vtable 槽位身份 | `processClassDecl`（比对 `entry.signature`） |
| ③ | 成员调用选定 | `findMethodInClass` → `pickBestByArgs` |
| ④ | 构造函数选定 | `processVarDeclStmt` → `pickBestByArgs` |

③④ 共用同一个本地静态函数 `pickBestByArgs`（精确 → 可隐式转换 → 退回首个）——
**同一判据不许写两份**，见 docs/BUGS.md B10/B12/B20/B22。

### 13.4.4 在 dump 输出中的应用

**类层次图（`--dump-hierarchy`）**：

```
┌─ _ZTI6Animal ─────────────────────────────────────┐
│  typeinfo form: 'C' (__class_type_info)          │
│  ...                                              │
└───────────────────────────────────────────────────┘

┌─ _ZTI3Dog ────────────────────────────────────────┐
│  typeinfo form: 'S' (__si_class_type_info)       │
│  ...                                              │
│  RTTI chain:                                      │
│    _ZTI3Dog ['S'] ──base──→ _ZTI6Animal ['C']    │
└───────────────────────────────────────────────────┘
```

**内存布局（`--dump-layout`）**：

```
_ZTV3Dog:
  [-2] offset-to-top: 0
  [-1] typeinfo: _ZTI3Dog
  [0]  Dog_speak  (override)
  [1]  Dog_eat
```

**注意**：`_ZTV3Dog` 和 `_ZTI3Dog` 是真实符号，`Dog_speak` 是简化名。

## 13.5 名字修饰的意义

### 13.5.1 链接器视角

链接器只看到符号名，看不到源码：

```cpp
// file1.cpp
int add(int a, int b) { return a + b; }

// file2.cpp
int add(int, int);  // 声明
int x = add(1, 2);  // 调用
```

编译后：

```
file1.o: 定义符号 _Z3addii
file2.o: 引用符号 _Z3addii
链接器：匹配成功 ✓
```

如果没有 mangling，所有 `add` 都会冲突。

### 13.5.2 调试视角

调试器用 mangling 反向解析符号：

```
Breakpoint 1 at 0x401234: _ZN3Dog5speakEv
→ 显示为: Dog::speak()
```

`c++filt` 工具可以反修饰：

```bash
$ echo "_ZN3Dog5speakEv" | c++filt
Dog::speak()
```

### 13.5.3 性能视角

Mangling 增加符号长度，但对性能无影响：

- 符号名只在链接和调试时使用
- 运行时只关心地址，不关心名字
- 现代链接器用哈希表，符号长度不影响速度

## 13.6 与 C 的对比

C 语言不使用 mangling（或极简化）：

```c
// C 代码
int add(int, int);
// 编译后：add（或 _add，取决于平台）
```

这就是为什么 C++ 调用 C 函数需要 `extern "C"`：

```cpp
extern "C" {
    int c_function(int);  // 不修饰，保持 C 链接
}
```

## 13.7 总结

| 特性 | 说明 |
|------|------|
| **目的** | 将 C++ 标识符转换为链接器能识别的唯一符号 |
| **标准** | Itanium C++ ABI（GCC/Clang 使用） |
| **前缀** | `_Z`（普通）、`_ZN`（嵌套）、`_ZTV`（vtable）、`_ZTI`（typeinfo） |
| **编码** | 长度前缀 + 标识符（如 `3Dog`、`5speak`） |
| **minicc** | 类符号用真实 mangling，虚函数用简化名 |

名字修饰是 C++ 支持函数重载、类成员、模板等特性的基础设施。理解 mangling 有助于调试链接错误和理解编译器的内部工作。

---

**下一步**：
- 运行 `./minicc tests/mini_rtti_demo.cpp --dump-hierarchy` 观察真实的 `_ZTI*` 符号
- 使用 `nm` 或 `readelf` 查看编译后目标文件的符号表
- 对比 `c++filt` 的输出，理解反修饰过程

## 13.8 真实版多做了什么、为什么

★ 素材来源：本文 §13.4.2「为什么简化？」/ §13.4.3「判据单点」/ §13.4.4
+ `docs/BUGS.md` B2/B7/B8/B10/B20/B22 + CLAUDE.md「未做」清单。**未查证的写「（本项目未查证）」**。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| 虚函数名用**真 Itanium 编码**（`_ZN3Dog5speakEv`），可被 `c++filt` 反修饰 | 本项目用简化格式 `Dog_speak`——① 学习目的：更易读 ② 避免复杂性：真 mangling 要处理重载、参数类型、嵌套 ③ 自包含：产物不需要与外部 C++ 代码链接。代价是**符号不可逆**，且与 clang 产物不兼容 |
| `ItaniumMangleContext::mangleName` **单一入口**：定义点与引用点共用同一个 mangling 上下文，结构上不可能不一致 | 本项目没有句柄可共享，只能靠"**判据单点**"——`memberMethodSymbolName` 一个函数，被定义点（`registerFunction`）与 vtable 条目点（`processClassDecl`）**两处调用同一函数**（§13.4.3）。这是 B20/B22 反复交学费换来的 |
| vtable 条目里放的是**刚发射过的函数句柄**（`GlobalDecl`），不是拼出来的字符串 | minicc 没有句柄，槽里是**各自拼的字符串** ⇒ 判据必须收口，否则"槽里的名字"与"函数定义的名字"错配（编译全绿、链接期 undefined reference） |
| 形参类型**完整编码**进符号（`f(int)` 与 `f(S)` 天然不同） | 本项目只在"同类里还有同名同个数的兄弟"时才挂形参类型链（`C_f_1_int` / `C_f_1_S`）——挂了就动了**全部**既有产物（logdiff 基线逐字节比对是硬约束） |
| 构造函数 / 析构函数的 **C1 / C2 / D0 / D1 / D2** 变体（完整对象 / 基类子对象 / deleting） | 未做：本项目单析构槽位 + `free`；少一套变体就少一套槽位身份判据——而 B20 已经证明槽位判据极易写错 |
| **局部名字**（`Z` 段：局部类、lambda 闭包类型） | 未做：本项目不支持嵌套类与 lambda |
| **运算符函数**的编码（`pl` `mi` `ls` …） | 未做：本项目没有运算符重载 |
| **ABI tag**（修饰符段 `B`，如 `[[gnu::xxx]]`） | 未做 |
| **MSVC** 的修饰方案（`?foo@@YAHH@Z`） | 未做：只做 Itanium——Linux/macOS 的事实标准，也是本机 oracle（clang++-18）用的那套 |
| 修饰名的**双向可逆**（`c++filt` 反解） | 简化虚函数名本就不可逆；类符号（`_ZTI`/`_ZTV`）保持可逆 |
| 跨 TU 的**弱符号 / vague linkage** 去重 | 未做：教学编译器是单 TU 的 |
| `B2`/`B7`/`B8` 修掉的那些洞（函数模板 mangling 漏参数签名、自由函数重载根本不做 mangling、mangling 没有替换表） | 已在 2026-09 前后陆续修复 ⇒ 现在的符号名与 clang **逐字符相同**（learn/35 的 12/12 实测） |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-09-10 | 主线 B/G 批次：类符号（`_ZTV` / `_ZTI`）用真 Itanium mangling；虚函数名用简化格式 | 教学取舍——可读性优先，且产物自包含 |
| 2026-09-10 | 修好 B2 / B7 / B8：函数模板 mangling 漏参数签名、自由函数重载不做 mangling、mangling 没有替换表 | 修完后符号名与 clang **逐字符相同**——"名字修饰"这个主题的验收点 |
| 2026-09-24 | 成员方法符号名收口成**单点判据** `memberMethodSymbolName` | 定义点与 vtable 槽位点必须同源；同一判据不许写两份（承 B10/B12 那条铁律） |
| 2026-10-08 | B20 缺陷 a：vtable 槽里的符号名与定义点不同源 | 带参虚函数的槽写 `.quad A_f`、定义却是 `.globl A_f_1` ⇒ 链接期 `undefined reference to 'A_f'` |
| 2026-10-08 | B20 缺陷 c：槽位身份补上形参类型链 | `virtual int f(); int f(int);` 里后者认领前者的槽并被误标 virtual ⇒ 运行返回 255 |
| 2026-10-09 | B22：同名同个数的成员重载，符号名再挂**形参类型链** | 同签名个数的重载在汇编期撞符号 `symbol 'C_f_1' is already defined`——拒收合法程序 |
| 2026-09-30 | NTTP 类型域扩展（learn/35）：mangling 与 clang **逐字符**核对通过 | 值位要区分 `4` 与 `4L`，否则 `TemplateArg::toString()` 静默撞键 |
| 2026-11-02 | 作用域即身份（B38，learn/49）：修饰名带完整作用域前缀 | 同名模板跨命名空间曾撞成同一个实例 `B_int`；`::` 净化后长度前缀仍自洽 |
