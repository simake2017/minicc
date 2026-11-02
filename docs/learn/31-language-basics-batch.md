# 31. 语言基础补齐批：默认继承 / 一元 `*` / 后置 const / static 成员

> 本批四项有一个共同点：它们都**不是模板机制**，却挡在模板考点前面 ——
> 外部 25 个模板用例剥掉 I/O 后，暴露出的第一层全是这类基础缺口。
> 本批按「错误拒绝合法程序 → 基础缺失 → 次要缺失」排序处理。

---

## §0 定位与能力清单

本批四项有一个共同点：它们都**不是模板机制**，却挡在模板考点前面 ——
外部 25 个模板用例剥掉 I/O 后，暴露出的第一层全是这类基础缺口。
本批按「**错误拒绝合法程序 → 基础缺失 → 次要缺失**」排序处理：先修那条
"按标准本该合法、却被判非法"的（`struct X : Base` 的默认继承级别），
再补三项纯缺失，最后把**顺带发现、本批未修**的清单交出去。

| 能力 | 一句话 | 状态 |
|---|---|---|
| `struct X : Base` 默认 **public** 继承（[class.derived]/2） | 此前按 private 处理 —— 是**拒收合法程序**的 bug | 已实现 |
| 一元 `*p` **解引用**（[expr.unary.op]/1） | 结果按 pointee 宽度分派读 / 写 | 已实现 |
| `*p` 与 `v[i]` 的**值类别** | 二者都是**左值** | 已实现（后由 learn/36 的值类别模型收口） |
| **后置 const** 成员函数（[dcl.fct]/7） | `int f() const { }` | 已实现（**只记标志**） |
| 类内 **static** 成员函数（[class.static]/2） | 无 `this` ⇒ 参数寄存器从 rdi 起 | 已实现 |
| static **数据**成员（[class.static.data]） | 类内声明 / 类外定义 / 常量折叠 | 已实现（learn/44 后来补齐） |
| `f()` / `f() const` **重载区分** | 两个不同签名应发两个符号 | 未做（共用一个符号） |
| **const 正确性检查** | 往 const 对象写、调用非 const 成员 | 未做（只记录标志） |
| **类外成员定义** `int C::f() const { }` | — | 未做 |
| 解引用的 **cv 合并** | `*const ptr` 的结果类型 | 未做 |

> ★ §5「顺带发现、本批未修」是**成文时的快照**，本表按**当前**状态填写，差异在演进记录里逐条列出。

## 0. 起因：一次被数据推翻的判断

先把验收面摆清楚。对 `my-test/cpp/src/test/template/` 的 25 个文件逐个编译：

| 层 | 阻塞 | 命中 |
|---|---|---|
| 第 1 层 | `#include <iostream>` 找不到 | **24 / 25** |
| 第 2 层 | 剥掉 I/O 后暴露的真缺口 | 23 个（2 个干净通过） |

而第 2 层暴露出来的**几乎全是基础语言缺口，不是模板机制**：

| 缺口 | 文件数 |
|---|---|
| `struct X : Base` 被拒 | 5 |
| 类内 `static` 成员函数 | 5 |
| 类内成员模板 | 4~5 |
| `T[N]` 数组偏特化 | 2 |
| `friend` + `operator\|` | 3 |

结论：这批文件是"用 C++ 写的小程序"，不是模板机制专测 ——
**正确的用法是蒸馏其中的模板考点，而不是让编译器去追 STL**。
但其中"错误拒绝合法程序"的那一类必须先修，本批处理前两项 + 两个顺手的。

---

## 1. `struct` 的默认继承级别 —— 错误拒绝合法程序

### 理论

[class.derived]/2：

> If the class-name is followed by a colon, the base-clause specifies the base classes.
> **The default access for a base class is `public` if the derived class is declared with
> the `struct` keyword, and `private` if it is declared with the `class` keyword.**

即默认继承级别由**声明关键字**决定，与"省略说明符"无关：

```cpp
struct D : Base   {};   // public 继承  ← 完全合法
class  D : Base   {};   // private 继承 ← 语义不同
struct D : public Base {};   // 显式，两种关键字通用
```

### 修复前

`src/parser.cpp` 一律强制要求显式 `public`：

```cpp
if (!match(TokenType::KwPublic)) {
    error("仅支持 public 继承（每个基类前需写 'public'）");
}
```

判据只看了"有没有写 `public`"，没看**声明关键字**。于是：

```cpp
struct Derived : Base { int w; };
// clang:  rc=7 ✅
// minicc: [Parse Error] 2:18 at 'Base': 仅支持 public 继承（每个基类前需写 'public'）
```

这是**错误拒绝合法程序**（rejecting valid programs）—— 比"不支持某特性"更严重：
前者让用户以为自己的代码错了。

### 修复

`isStruct` 本来就在手上（决定类内默认访问级别），之前被 `(void)isStruct;` 丢弃。
现在让它同时决定**默认继承级别**：

```cpp
if (match(TokenType::KwPublic)) {
    // 显式 public：本项目唯一受支持的形态，放行
} else if (check(TokenType::KwPrivate) || check(TokenType::KwProtected)) {
    error("'...' inheritance is not implemented (only public inheritance is)...");
} else if (!isStruct) {
    // 省略说明符 + class ⇒ 默认 private 继承，语义未实现
    error("a class default-inherits privately ([class.derived]/2), which is not implemented; ...");
}
// 落到这里 = 省略说明符 + struct ⇒ 默认 public，继续
```

| 写法 | 修复前 | 修复后 |
|---|---|---|
| `struct D : Base` | ❌ 报错 | ✅ public 继承 |
| `struct D : public Base` | ✅ | ✅ |
| `class D : public Base` | ✅ | ✅ |
| `class D : Base` | ❌ 文案不准 | ❌ 文案说清"默认 private，未实现" |
| `struct D : private Base` | ❌ 文案不准 | ❌ 文案说清"private 未实现" |

> **设计决策**：private/protected 继承的**语义**（基类子对象的访问控制）本项目不做，
> 故仍然报错；但文案从"语法不允许"改成"语义未实现"——
> **报错理由必须准确**，否则学习者会建立错误的心智模型。

### clang 对照

| clang | 本实现 |
|---|---|
| `Sema::ActOnBaseSpecifier`（SemaDeclCXX.cpp）在**未写** AccessSpecifier 时取 `TagDecl::getTagKind()` 决定默认 | `parseClassDecl` 用 `isStruct` 决定 |

---

## 2. 一元 `*` 解引用

### 理论

[expr.unary.op]/1：

> The unary `*` operator performs **indirection**: the expression to which it is applied
> shall be a pointer to an object type ... **the result is an lvalue** referring to the object.

两个要点：

1. **判据是位置**：`*` 是一元还是二元，看它**前面有没有左操作数**；
2. **结果是左值** —— 所以 `*p = v` 合法，这条决定了 CodeGen 要多一条写路径。

### 修复前

```cpp
int main() { int a = 5; int* p = &a; return *p; }
// [Parse Error] 1:45 at '*': Unexpected token '*' in expression
```

Parser 的 `parseUnaryExpr` 只认 `-` `!` `&`，`*` 落到 primary 层报错。

**而且这个错误位置很误导**：报的是语法错误，但 `*3` 在 C++ 里**语法是合法的**，
错在语义（`3` 不是指针）。clang 对 `return *3;` 报的是：

```
error: indirection requires pointer operand ('int' invalid)
```

### 修复（四处 + 两个打印点）

```
                    ┌─ parseUnaryExpr 判据：'*' 前无左操作数 ⇒ 解引用
                    │
  ┌─────────────┐   │   ┌──────────────┐   ┌──────────────┐   ┌──────────────┐
  │   Parser    │───┼──▶│     Sema     │──▶│   CodeGen    │──▶│  CodeGen     │
  │ UnaryOp::   │   │   │ 结果 = pointee│   │  读：按被指  │   │  写：AssignStmt│
  │   Deref     │   │   │ 非指针 ⇒    │   │  宽度加载    │   │  的 Unary 分支 │
  └─────────────┘   │   │ 软硬报错     │   └──────────────┘   └──────────────┘
                    │   └──────────────┘
       与 '&' 完全同构的"位置判据"
```

| 层 | 改动 |
|---|---|
| `include/ast.h` | `UnaryOp` 加 `Deref` |
| `src/parser.cpp` | `parseUnaryExpr` 接受 `TokenType::Star` |
| `src/semantic_analyzer.cpp` | `inferUnary` 加 `Deref`：结果 = `pointeeType`，非指针报 clang 同款文案 |
| `src/codegen.cpp` | `visit(UnaryExpr)` 加解引用读；`visit(AssignStmt)` 加解引用**写** |
| `src/main.cpp` + `tests/unit/obs_helpers.h` | 打印映射（见下"顺带修"） |

**判据同构**（本批最值得记的一处）：

```
一元位置上的 '&'  ⇒ 取地址   （引用只出现在类型里，位与是二元）
一元位置上的 '*'  ⇒ 解引用   （指针声明只出现在类型里，乘法是二元）
          ↑
    两者用的是同一条判据：能走到 parseUnaryExpr，就说明【前面没有左操作数】
```

### 读 / 写的宽度

CodeGen 里局部变量一律 `movq`（8 字节槽，教学简化），但 `*p` 的目标**不一定是局部变量**：

```
*p = v   →  目标可能是 局部槽(8B) / 结构体字段(可能 4B) / 全局
            写 8 字节会踩坏邻居
```

故解引用的读与写都**按被指类型的宽度**分派（与 B3 的字段写同一条判据）：

| 被指类型 | 读 | 写 |
|---|---|---|
| `bool` | `movzbq (%rax), %rax` | `movb %al, (%rcx)` |
| `int` | `movl (%rax), %eax` | `movl %eax, (%rcx)` |
| 其它 | `movq (%rax), %rax` | `movq %rax, (%rcx)` |

对照 clang：`EmitLoadOfLValue` / `EmitStoreOfScalar` 按 lvalue 的 AST 类型选指令宽度。

### 顺带修：两处"二元三元式"打印

```cpp
// 改前：新增 Deref/Addr 后，它们全被印成 "!"
std::format("UnaryExpr: {}", e.op == UnaryOp::Neg ? "-" : "!")
```

`src/main.cpp`（`--dump-ast`）与 `tests/unit/obs_helpers.h`（单测观测）各有一处。
**这种"默认分支兜底"的写法在枚举扩张时必然静默错味** —— 改成逐个列举。

---

## 3. 后置 const 成员函数

### 理论

[dcl.fct]/7：cv-qualifier-seq 是**函数类型的一部分**：

> The effect of a cv-qualifier-seq in a function declarator is **not the same as adding
> cv-qualification on top of the function type**. ... it is used to define the
> **type of the implicit object parameter**.

即 `int get() const` 里那个 `const` 改变的是**隐式 this 参数**的类型：

```
        int get()        →  this 的类型是  C*
        int get() const  →  this 的类型是  const C*
                                  ↑
        这才是"const 对象调不了非 const 成员"的底层机制 —— 类型系统里的实打实差别
```

### 语法位置

[class.mem] 的 member-declarator 文法：

```
parameters-and-qualifiers :=
    ( parameter-declaration-clause ) cv-qualifier-seq? ref-qualifier? noexcept-specifier? attribute-specifier-seq?

member-declarator := ... parameters-and-qualifiers virt-specifier-seq? pure-specifier?
```

所以顺序是：**参数列表 → cv → ref → override/final**：

```cpp
struct D : B { int f() const override { return 2; } };   // ✓ const 在 override 之前
```

### 修复

在 `parseFunctionDecl` 里、`override` 之前收一个可选 `const`：

```cpp
if (check(TokenType::KwConst)) {
    if (ownerClass.empty()) {
        error("non-member function cannot have 'const' qualifier");   // clang 同款文案
    }
    advance();
    decl->isConstMethod = true;
}
```

### 未做（边界）

| 项 | 状态 |
|---|---|
| `f()` 与 `f() const` 的**重载区分** | ❌ clang 视其为两个不同重载；本实现共用同一个符号 |
| const 正确性检查（const 对象调非 const 成员应报错） | ❌ 只记录标志，未做检查 |
| 引用限定符 `&` / `&&`、`noexcept` | ❌ |
| **类外定义** `int C::f() const { }` | ❌ 独立缺口（限定名的函数定义），见下 |

---

## 4. 类内 static 成员函数

### 理论

[class.static]/2：

> A static member function does not have a **this** pointer.

它改变的是**调用约定**：

```
        普通成员函数            static 成员函数
        ────────────           ──────────────
  rdi = this（隐式第 0 参数）   rdi = 第 0 个【形参】
  rsi = 第 0 个形参             rsi = 第 1 个形参
  rdx = 第 1 个形参             rdx = 第 2 个形参
```

### 修复前

`static` **根本不是关键字** —— lexer 里没有它，于是被当类型名：

```cpp
struct C { static int f() { return 7; } };
// [Parse Error] 1:19 at 'int': Expected field name: expected Identifier, got 'int'
//                 ↑ 'static' 被当成类型，'int' 于是"不是字段名"
```

### 修复

| 层 | 改动 |
|---|---|
| `include/token.h` | 加 `KwStatic` + 关键字表项 |
| `src/lexer.cpp` | `tokenTypeName` 加分支（漏了会 `-Wswitch` 警告，本项目要求 0 警告） |
| `src/parser.cpp` | 类内成员循环收 `static` → `FunctionDecl::isStatic` |
| `src/codegen.cpp` | 形参装填按 `hasThis` 决定是否右移一格 |

**static 数据成员**（`static int count;`）明确报错：

```
[Parse Error] static data members are not implemented (only static member functions are)
```

> **为什么不静默当普通字段处理**：static 数据成员是**全类共享一份**，
> 当成普通字段会变成每实例一份 —— 语义完全相反，且编译/链接都不报错，只是算错。
> 宁可响亮地拒绝。

### ★ 本批最贵的一课：同一个判据散在两处

修复后 `C::add(3,4)` 仍返回 4（应为 7）。根因：

```cpp
// ── 位置 ①：this 是否占 rdi ──
if (!func->ownerClassName.empty()) {        // ← 改了这里，加上 !isStatic
    m_localVars["this"] = paramOffset;
    paramOffset -= 8;
}

// ── 位置 ②：形参从第几个寄存器起 ──
for (size_t i = 0; ...) {
    size_t regIdx = func->ownerClassName.empty() ? i : i + 1;   // ← 这里没改！
}
```

两处各写了一份 `ownerClassName.empty()`。改了一处，另一处照旧右移：

```
  调用端（自由函数约定）        被调端（仍按成员函数约定）
  ──────────────────────       ──────────────────────────
  rdi = 3 (a)                   rdi → this 槽（丢弃）
  rsi = 4 (b)                   rsi → a   ⇒ a = 4
                                rdx → b   ⇒ b = 垃圾(0)
                                          a + b = 4   ✗（期望 7）
```

**修法不只是补第二处，而是把判据提取成一个变量**：

```cpp
const bool hasThis = !func->ownerClassName.empty() && !func->isStatic;   // 只算一次
```

> 一般化的教训：**同一条语义判断出现在两处时，加特例必然漏改一处**。
> 这类"同一事实被复述"的代码，应当先合并再改。

实测症状极具迷惑性：**无参的 static 函数完全正常**（寄存器错位在无参时不可见），
只有带参数的才错 —— 若测试只写 `C::f()`，这个 bug 会一路潜伏。

---

## 5. 顺带发现、本批未修

| 缺口 | 现象 | 归属 |
|---|---|---|
| **类外成员定义** `int C::f() const { }` | `Expected ';' after global variable declaration` | 独立缺口 |
| **限定名调用成员函数** `C::f()` 的符号 | 曾拼成 `C__f`（定义端 `C_f`）⇒ 链接期 undefined | 本批修了名字改写，但**类外定义**仍缺 |
| static 数据成员 | 明确报错 | 见 §4 |
| `f()` / `f() const` 重载区分 | 共用一个符号 | 见 §3 |
| const 正确性检查 | 只记录标志 | 见 §3 |

**限定名调用**的修复值得一提：Sema 命中成员函数后**必须把 callee 名改写成发射符号**
（`C::f` → `C_f`），否则 CodeGen 拿源码名去 `asmSymbol` 净化（`::`→`__`）得到 `C__f`。
这与模板实例路径（`calleeVar->name = instance->mangledName`）是**同一手法**：

> **名字的"真身"在语义阶段兑现** —— Sema 定死符号名，CodeGen 照着发。

---

## 6. 可复现实验

```bash
# 构建
./build.sh

# ① struct 默认继承（clang=7）
cat > /tmp/inh.cpp <<'EOF'
struct Base { int v; };
struct Derived : Base { int w; };
int main() { Derived d; d.v = 3; d.w = 4; return d.v + d.w; }
EOF
./minicc /tmp/inh.cpp -o /tmp/inh && /tmp/inh; echo "rc=$?"

# ② 一元 * 解引用（clang=30）
cat > /tmp/dw.cpp <<'EOF'
int main() { int a = 5; int* p = &a; *p = 30; return a; }
EOF
./minicc /tmp/dw.cpp -o /tmp/dw && /tmp/dw; echo "rc=$?"

# ③ 后置 const（clang=23）
cat > /tmp/pc.cpp <<'EOF'
struct C { int v; int f() const { return v + 1; } int g() { return v + 2; } };
int main() { C c; c.v = 10; return c.f() + c.g(); }
EOF
./minicc /tmp/pc.cpp -o /tmp/pc && /tmp/pc; echo "rc=$?"

# ④ static 成员函数（clang=7；★ 必须带参数才测得出错位）
cat > /tmp/st.cpp <<'EOF'
struct C { static int add(int a, int b) { return a + b; } };
int main() { return C::add(3, 4); }
EOF
./minicc /tmp/st.cpp -o /tmp/st && /tmp/st; echo "rc=$?"

# 全部四项 + clang oracle 对照
clang++-18 -std=c++20 tests/lang/test_basics_01.cpp -o /tmp/b1 && /tmp/b1; echo "clang rc=$?"
./minicc tests/lang/test_basics_01.cpp -o /tmp/b1m && /tmp/b1m; echo "minicc rc=$?"
```

---

## 7. clang 源码对照表

| 主题 | clang 位置 | 本实现位置 | 简化了什么 |
|---|---|---|---|
| 默认继承级别 | `Sema::ActOnBaseSpecifier`（SemaDeclCXX.cpp）按 `getTagKind()` 取默认 | `Parser::parseClassDecl` 用 `isStruct` | 不做访问控制语义 |
| 一元 `*` 解析 | `ParseCastExpression` 的 `tok::star` 分支（ParseExpr.cpp） | `Parser::parseUnaryExpr` | 无 cast 层，直接落 unary |
| 解引用结果类型 | `Sema::CheckIndirectionOperand`（SemaExpr.cpp） | `SemanticAnalyzer::inferUnary` | 不算 cv 合并（`*const ptr` 等） |
| 解引用取/存 | `EmitLoadOfLValue` / `EmitStoreOfScalar`（CGExpr.cpp） | `visit(UnaryExpr)` / `visit(AssignStmt)` | 只按 bool/int/其它 三档宽度 |
| 后置 const | `ConsumeAndStoreFunctionPrologue` → `FunctionProtoType` 的 qualifier 位 | `Parser::parseFunctionDecl` | 只记标志，不做重载区分/const 检查 |
| static 成员 | `Sema::CheckFunctionDeclaration` 设 `CXXMethodDecl::isStatic`；CodeGen 的 `CGFunctionInfo` 不含 this | `FunctionDecl::isStatic` + `hasThis` | 不做 static 数据成员 |

---

## 8. 交付物

| 类型 | 产物 |
|---|---|
| 代码 | `include/token.h`、`include/ast.h`、`src/lexer.cpp`、`src/parser.cpp`、`src/semantic_analyzer.cpp`、`src/codegen.cpp`、`src/main.cpp` |
| 测试（集成） | `tests/lang/test_basics_01.cpp`（四项一次跑通，退出码汇总，clang 与本编译器同为 rc=0） |
| 测试（单元） | `tests/unit/test_expressions.cpp` 的 `Semantics.DerefRequiresPointer`（读 / 写 / 链式 + 非指针报错文案）；`tests/unit/test_expr_parser.cpp` 的 `ExprParserErrors` 用例同步（`*3` 已语法合法，改用 `1 *`） |
| 回归 | `logdiff.sh` 基线重刷；`struct : Base` 与 `static` 两处**新增日志行**已进基线 |

## 9. 真实版多做了什么、为什么

（把 §7 对照表第三列与 §5 的"顺带发现"表合并成一张两列表。）

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `Sema::ActOnBaseSpecifier` 按 `getTagKind()` 取默认继承级别，并落实**访问控制** | 本项目在 Parser 里用 `isStruct` 一个 bool 取默认 —— 不做访问控制的**语义效果**（`public:` / `private:` 段的实际约束） |
| 解引用结果类型走 `Sema::CheckIndirectionOperand` | 本项目在 `inferUnary` 里算，但**不合并 cv**（`*const ptr` 之类形态未做） |
| 解引用的取 / 存按**精确类型宽度**发射（`EmitLoadOfLValue` / `EmitStoreOfScalar`） | 本项目只按 bool / int / 其它 **三档宽度**分派 |
| 后置 const 进 `FunctionProtoType` 的 **qualifier 位**，因而参与重载区分 | 本项目**只记标志** ⇒ `f()` 与 `f() const` 共用一个符号（§5 的已知边界） |
| `Sema::CheckFunctionDeclaration` 设 `CXXMethodDecl::isStatic`，CodeGen 的 `CGFunctionInfo` 不含 this | 本项目用 `FunctionDecl::isStatic` + `hasThis` 两个字段表达同一件事 |
| clang 的表达式解析有 **cast 层**（`ParseCastExpression` 的 `tok::star` 分支） | 本项目无 cast 层，`*` 直接落 unary —— 教学上少一层 |
| clang 有完整的 **const 正确性检查** | 本项目只记录标志 —— **宽方向**（多放过，而不是误拒合法程序） |
| clang 的 static **数据成员**有完整的存储与初始化语义 | 本批不做 —— 后来由 learn/44（[class.static.data]）单独补齐 |
| `f()` / `f() const` 作为**两个重载**分别发射符号 | 未做 —— 符号名只编码"名字 + 形参类型"，不含成员函数的 cv 限定 ⇒ 二者仍撞符号 |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-09-22 | 语言基础补齐 + 模板三项收尾（commit e0ff653）：本批四项 + NTTP 值位参与偏特化模式 + 模板模板参数 + 成员模板 | 25 个外部模板用例剥掉 I/O 后暴露的第一层**全是基础缺口**，不是模板机制 —— 先补地基再谈考点 |
| 2026-09-22 | 顺带修 PITFALLS **I4**（蓝图摘要把 NTTP 打成 `typename N`）与 **F2** 残留（Phase 4 演示只看第二位形参） | 两处都是"文档 / 演示与实现不一致"的陈旧描述 |
| 2026-09-30 | 无声明符的声明被拒收（B16，commit bf19d93） | 与 §5 里"类外成员定义"同族 —— 都是声明解析的边界 |
| 2026-10-08 | static / virtual 的**识别位置**写反（B19，commit acbc48f） | 成员模板的 `static` 前缀问题 —— 与本批的 static 成员函数同族 |
| 2026-10-08 | 成员方法身份收口为「名字 + 形参类型」（B22，commit da7f110） | ★ **符号命名规则变了**（从"名字 + 参数个数"改成"名字 + 形参类型链"）；本批"重载区分"那条边界至今未做，但同族的"同名同个数撞符号"已修 |
| 2026-10-23 | static **数据**成员落地（[class.static.data]，B33，commit 36a1fda） | §5 边界表里"static 数据成员"那条被填上 |
| 2026-10-27 | const 类对象端到端修复（B34，commit 327a421）：类性判定剥穿 `Const` 包壳 | 一层 const 包壳、三层各自独立写错的判据（成员访问误报 / 掉标量路径 SIGSEGV / 拷贝静默退化）；回归用例含 const 对象上调 const 成员函数 |

