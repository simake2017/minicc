# 10 AST：抽象语法树（编译器的心脏数据结构）

> AST（Abstract Syntax Tree）是编译器管线中承上启下的核心数据结构：
> Parser 把线性 Token 流"折叠"成树，后面所有阶段（语义分析、模板实例化、代码生成）
> 都只是在这棵树上做**遍历 + 变换**。
> 理解 AST 就理解了"编译器到底在操作什么东西"。

---

## §0 定位与能力清单

AST 是编译器的**中枢数据结构**：Parser 把线性的 Token 流"折叠"成一棵树，
之后所有阶段（语义分析、模板实例化、代码生成）都只是在这棵树上做**遍历 + 变换**。
理解 AST 就理解了"编译器到底在操作什么东西"。本文讲这棵树长什么样、
怎么被各阶段接力加工。

| 能力 | 一句话 | 状态 |
|---|---|---|
| [gram] 三大节点族 | 表达式（有类型可求值）/ 语句（控制流程）/ 声明（引入名字） | 已实现 |
| `NodeKind` 枚举 + 继承双轨 | 消费方 `switch` 分派；需要子节点字段时 `static_pointer_cast`（零 RTTI） | 已实现 |
| Visitor 虚分派 | `ASTNode::accept` + 32 个 `visit` 重载，全项目零 `dynamic_pointer_cast` | 已实现（learn/29） |
| 源码位置 | 每节点带 `SourceLocation`，报错定位（如 `3:7 期望 ';'`） | 已实现（只记起点） |
| 模板蓝图节点 | `TemplateDecl` 双槽位（`classTemplate` / `funcTemplate` 互斥） | 已实现 |
| 表达式类型槽 | `Expression::resolvedType`，Parser 阶段为空、语义期填充 | 已实现 |
| 根节点 | `TranslationUnit { vector<DeclPtr> }`，一个源文件一棵树 | 已实现 |
| 值类别细分 | lvalue / xvalue / prvalue 三态 | 部分（简化为语义阶段的布尔左值性） |
| `DeclContext` 作用域嵌套链 | clang 的声明上下文链 | 未做（符号表用简单 map；作用域链由 learn/49 的前缀化补上，但仍是平表） |
| `ASTContext` 内存池与类型唯一化 | clang 的 arena 分配 + 类型规范化 | 未做（`shared_ptr` + 类型字符串；类型字符串不单射的问题见 learn/23） |
| `RecursiveASTVisitor` **泛型**遍历框架 | clang 的万能遍历器 | 未做（本项目后来手写了 Visitor 模式，但仍是**手写重载**，不是泛型框架） |
| `ParenExpr` 节点 | clang 保留括号节点 | 未做（括号信息已被树形吸收） |
| 赋值作为表达式 | clang 里 `a = b = c` 可链式 | 未做（本项目的赋值是**语句**） |
| AST 打印 | `--dump-ast` 递归打印树形 | 已实现 |

## ① 理论背景

**标准依据**：C++ 标准本身没有"AST"一词——标准描述的是语法（[gram]）与语义（[basic]/[expr]/[stmt]/[dcl]）。
AST 是编译原理的实现概念，对应龙书（Aho et al.）第 4~5 章的 "Syntax Tree" / "Abstract Syntax"。

### 1.1 为什么需要 AST：从"线性文本"到"结构化程序"

源码是**一维字符串**，程序语义是**嵌套结构**。两者之间隔着两次降维：

```text
源码（一维字符流）
    int c = 1 + 2 * 3;
         │  Lexer（去掉空白/注释，切出词法单元）
         ▼
Token 流（一维，但有类别）
    [int] [c] [=] [1] [+] [2] [*] [3] [;]
         │  Parser（递归下降，把优先级/嵌套编码成层次）
         ▼
AST（二维树形结构）
    VarDeclStmt(c)
    └─ BinaryExpr(+)
       ├─ IntLiteral(1)
       └─ BinaryExpr(*)
          ├─ IntLiteral(2)
          └─ IntLiteral(3)
```

- **Token 流丢掉了优先级信息**：`1 + 2 * 3` 和 `(1 + 2) * 3` 的 Token 序列只差一对括号，
  但语义天差地别。Parser 的职责就是把优先级/结合性**编码进树的嵌套深度**
  （详见 09 号文档：优先级分层递归下降）。
- **AST 是"抽象"的**：相对 CST（Concrete Syntax Tree，具体语法树），
  AST 丢弃了纯语法噪音——括号、分号、逗号都不进树。
  `(1 + 2)` 在 AST 里就是 `BinaryExpr(+, 1, 2)`，
  因为括号的信息已被"树的形状"吸收（见 ② 设计决策）。

### 1.2 AST 的三大节点族

任何命令式语言的 AST 都按"节点在程序中扮演什么角色"分成三族。
本项目的分类与 clang 完全同构：

| 族 | 本项目基类 | 核心特征 | 标准章节 | clang 对应 |
|---|---|---|---|---|
| 表达式 Expression | `Expression` | 有类型、可求值，可嵌套进另一个表达式 | [expr] | `clang::Expr` |
| 语句 Statement | `Statement` | 控制流程、无值、不能作为子表达式 | [stmt] | `clang::Stmt` |
| 声明 Declaration | `Declaration` | 向符号表引入新名字 | [basic.scope]/[dcl] | `clang::Decl` |

判别口诀（ast.h 中三大基类注释的提炼）：

```text
表达式 = "算东西"（有类型有值）        42 / a + b / foo(x)
语句   = "做动作"（控制怎么跑）        return / if / while / x = 5;
声明   = "报户口"（引入名字）          int foo(); class Point; template<...>
```

一个典型的跨族组合：`int x = foo(a, b);`

```text
VarDeclStmt (语句族：声明一个局部变量是"动作")
├─ declaredType → int
└─ initializer → CallExpr (表达式族：右边是要算的值)
    ├─ callee  → VarExpr(foo)   ← foo 这个名字由声明族引入
    └─ arg[0]  → VarExpr(a)
```

### 1.3 NodeKind + 继承：双重类型标签

本项目用"**运行时枚举标签 + C++ 继承**"双轨制（ast.h 的 `NodeKind`）：

- `NodeKind kind`：消费方不必 `dynamic_cast` 就能 switch 分派（热路径更快），
  教学上等价于 clang 的 `Stmt::StmtClass` / `Decl::Kind` 枚举；
- 继承体系：`struct BinaryExpr : Expression`，需要访问子节点字段时再
  `std::static_pointer_cast`（kind 已确认，安全且零开销）。

clang 同样双轨：`Stmt` 有 `StmtClass` 枚举 + 虚析构，访问用 `cast<BinaryOperator>(S)`。

---

## ② 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| 节点所有权 | `std::shared_ptr<ASTNode>`（ExprPtr/StmtPtr/DeclPtr） | 模板实例化要"克隆子树 + 共享类型节点"；shared_ptr 免手动管理，教学直观 |
| 类型标签 | `NodeKind` 枚举 + 继承双轨 | switch 分派快；字段访问仍走类型系统 |
| 括号 | 不建节点 | 括号语义已被树形吸收；若未来需要保留（如调试显示），才加 `ParenExpr`（clang 有） |
| 赋值 | 独立 `AssignStmt`（语句族） | C++ 里赋值是表达式（`a = b = c` 可链式），本项目简化为语句，牺牲链式赋值换取"表达式无副作用"的简单心智 |
| 源码位置 | 每节点带 `SourceLocation` | 报错定位（"3:7 期望 ';'")；对应 clang 的 `SourceRange` |
| 模板蓝图 | `TemplateDecl` 双槽位（classTemplate / funcTemplate 互斥） | 对照 clang 的 ClassTemplateDecl/FunctionTemplateDecl 共同继承 TemplateDecl；本项目用"判别方法"代替继承，AST 保持扁平 |
| 类型信息 | `Expression::resolvedType` Parser 阶段为空，阶段 3 填充 | 职责分离：Parser 只管结构，类型是语义分析的产物（[expr]：每个表达式有唯一类型） |
| 根节点 | `TranslationUnit { vector<DeclPtr> }` | 对应 clang::TranslationUnitDecl；一个源文件一棵树 |

---

## ③ 例子：一段源码的完整 AST

```cpp
class Point {
public:
    int x;
    Point(int px) : x(px) {}
};

template<typename T> T twice(T v) { return v + v; }

int main() {
    int a = twice(21);
    if (a > 40) { a = 0; }
    while (a) { a = a - 1; }
    return a;
}
```

```text
TranslationUnit
├─ ClassDecl(Point)
│  ├─ fields: [ x : int ]
│  └─ methods: [ ConstructorDecl(Point, initList=[x(px)]) ]
├─ TemplateDecl(function)                 ← 蓝图，暂不做语义
│  └─ funcTemplate: FunctionDecl(twice)
│     ├─ returnType → T (TemplateParam 占位)
│     ├─ parameters → [ v : T ]
│     └─ body → BlockStmt
│                 └─ ReturnStmt
│                     └─ BinaryExpr(+)
│                        ├─ VarExpr(v)
│                        └─ VarExpr(v)
└─ FunctionDecl(main)
   └─ body → BlockStmt(4 条语句)
      ├─ VarDeclStmt(a : int)
      │  └─ CallExpr(1 个实参)
      │     ├─ callee → VarExpr(twice)
      │     └─ arg[0] → IntLiteral(21)
      ├─ IfStmt
      │  ├─ condition  → BinaryExpr(>) [VarExpr(a), IntLiteral(40)]
      │  ├─ thenBranch → BlockStmt[ AssignStmt ]
      │  └─ elseBranch → (空)
      ├─ WhileStmt
      │  ├─ condition → VarExpr(a)
      │  └─ body → BlockStmt[ AssignStmt(a, BinaryExpr(-)) ]
      └─ ReturnStmt
         └─ VarExpr(a)
```

观测方式（全阶段中文日志 + 递归树打印）：

```bash
# 阶段 2 的树形（--dump-ast）；obs_helpers.h 的 dumpExprTree/dumpStmtTree 是同一思想
./cmake-build-debug/minicc tests/test_ctor_01_basic.cpp --dump-ast
```

---

## ④ AST 在管线中的生命周期（各阶段对树做什么）

| 阶段 | 动作 | 代码位置 |
|---|---|---|
| Parser | **建造**：递归下降逐节点构造 | src/parser.cpp（parseTranslationUnit 入口） |
| SemanticAnalyzer | **标注 + 改写**：填 `resolvedType`、绑定符号、把模板调用 callee 原地改写为 mangled 名 | src/semantic_analyzer.cpp（inferCall:1386 改写例） |
| TemplateInstantiator | **克隆 + 替换**：克隆蓝图子树，T → int，产出全新子树挂回函数列表 | src/template_instantiation.cpp（cloneExpr/cloneStmt） |
| CodeGen | **只读遍历**：递归访问节点生成 x86-64 AT&T 汇编 | src/codegen.cpp |

关键认知：**AST 不是只读的**。语义分析会改写它（callee 改名），
实例化会复制它（克隆蓝图）。这正是"模板 = 代码生成器"在数据结构层面的体现。

---

## ⑤ 可复现实验

```bash
# 1. 跑本主题单测（节点族分类 / 树形断言 / 位置信息 / 蓝图冻结）
cmake --build cmake-build-debug --target unit_tests -j$(nproc)
./cmake-build-debug/unit_tests --gtest_filter='Ast*'

# 2. 语义 oracle：clang ast-dump 对照（结构应与本实现同形）
cat > /tmp/ast_demo.cpp <<'EOF'
int add(int a, int b) { return a + b; }
int main() { int x = add(1, 2); return x; }
EOF
clang++-18 -Xclang -ast-dump -fsyntax-only /tmp/ast_demo.cpp
# 预期：FunctionDecl add → CompoundStmt → ReturnStmt → BinaryOperator '+'
#        └─ DeclRefExpr 'a'  DeclRefExpr 'b'，与本项目 AST 一一对应

# 3. 本项目全管线观测
./cmake-build-debug/minicc /tmp/ast_demo.cpp --dump-tokens --dump-ast
# 对比 Token 流（一维）与 AST（嵌套树）：优先级信息只在后者中可见
```

---

## ⑥ clang 源码对照

| clang（AST 基础设施） | 本实现 | 简化了什么 |
|---|---|---|
| `include/clang/AST/Stmt.h`（Stmt + StmtClass 枚举） | include/ast.h `Statement` + `NodeKind` | 无虚函数分派表、无 ASTContext 分配器 |
| `include/clang/AST/Expr.h`（Expr 必有类型） | `Expression::resolvedType` | 无 value category 细分（lvalue/xvalue/prvalue 简化为语义阶段的布尔左值性） |
| `include/clang/AST/Decl.h`（Decl 层次） | `Declaration` 平铺一族 | 不做 DeclContext 嵌套作用域链，符号表用简单 map |
| `RecursiveASTVisitor`（万能遍历器） | 各阶段手写递归访问 | 不用访问者模式泛型框架 |
| `ASTContext`（内存池 + 类型唯一化） | `std::shared_ptr` | 无 arena 分配，教学可读优先 |
| `SourceRange`（begin/end 两个 SourceLocation） | 单 `SourceLocation` | 只记起点，够报错定位 |

---

## ⑦ 关键 ASCII 总图：从字符到汇编，AST 居中

```text
┌────────────┐   Lexer    ┌────────────┐   Parser(递归下降)   ┌────────────┐
│  源码文本   │ ─────────▶ │  Token 流   │ ──────────────────▶ │    AST     │
│  (一维字符) │            │ (一维类别)  │   优先级→嵌套深度    │ (二维树)   │
└────────────┘            └────────────┘                      └─────┬──────┘
                                                                    │
                        ┌───────────────────────────────────────────┼───────────────┐
                        ▼                                           ▼               ▼
                 SemanticAnalyzer                          TemplateInstantiator   CodeGen
                 标注类型/绑定符号/改写 callee              克隆蓝图+类型替换       遍历→x86-64
                        │                                           │               │
                        └─────────────────┬─────────────────────────┘               ▼
                                          ▼                                     汇编 .s
                                     改写/扩充后的 AST ─────────────────────────▶
```

一句话总结：**Lexer 去噪、Parser 塑形、Sema 赋值、实例化分身、CodeGen 翻译——
五个阶段接力传递的，始终是这棵树。**

## ⑧ 真实版多做了什么、为什么

★ 素材来源：本文 §⑥ clang 源码对照的「简化了什么」列 + learn/29 的分派判据
+ CLAUDE.md「AST 分派重构」条目。**未查证的写「（本项目未查证）」**。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `include/clang/AST/Stmt.h` 的 `Stmt` 虚函数分派表 + `ASTContext` 分配器 | 本项目用 `NodeKind` 枚举 + `static_pointer_cast`（零 RTTI）：kind 已确认，转换安全且零开销；clang 用 `dynamic_cast`/LLVM `cast<>` 的 `classof()` 查 `getKind()`，本项目是同一思路的手写版 |
| `include/clang/AST/Expr.h` 里 Expr **必有类型** + 完整 value category（lvalue/xvalue/prvalue） | 本项目 `resolvedType` 在 Parser 阶段为空、语义期才填（职责分离）；value category 简化为**布尔左值性**——够用且可讲清，代价是 `*p` 与 `v[i]` 这类"左值但不是变量"的情形要单独补（B26 踩过） |
| `include/clang/AST/Decl.h` 的 Decl 层次 + `DeclContext` **嵌套作用域链** | 本项目符号表是简单 map，作用域靠**名字前缀化**近似（learn/49）；不做链的好处是查找路径一眼看得见，代价是嵌套作用域/命名空间嵌套要逐个补丁（B36/B37/B38） |
| `RecursiveASTVisitor` **泛型**遍历框架 | 本项目是各阶段**手写递归**（后来 learn/29 收口成 Visitor 虚分派，仍是 32 个手写重载）。不引入泛型框架 ⇒ 少一层模板元编程的阅读负担 |
| `ASTContext`（内存池 + 类型唯一化 + 递归布局缓存） | 本项目用 `std::shared_ptr` + 字符串键的类型缓存；不建 arena ⇒ 教学可读优先。代价是类型标识不唯一（`const int*` 与 `int* const` 都印成 `const int*`），已在 learn/23 记录并单独修过一次 |
| `SourceRange`（begin/end 两个 `SourceLocation`） | 本项目单个 `SourceLocation`，只记起点——够报错定位（`3:7`），不做范围高亮 |
| `ParenExpr` 节点 | 括号不建节点，信息由树形吸收——AST 更干净（本文 §② 的取舍表） |
| 赋值作为**表达式**（`a = b = c`） | 本项目的赋值是语句族节点 ⇒ 牺牲链式赋值，换取"表达式无副作用"的心智模型 |
| `Decl` 的完整谱系（Function/Var/Field/Enum/Using/Typedef/Template…） | 本项目 `Declaration` 平铺一族 + `NodeKind` 区分；不追求与 clang 一一对应，只求每种声明在树上有位置 |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-08-27 | AST 理论文档 + 三大节点族 / `NodeKind` + 继承双轨落地 | AST 是管线中枢，先把数据结构钉死，后面的阶段才有共同的战场 |
| 2026-09-16 | AST 分派重构（learn/29）：Visitor 模式 + 标签分派，全项目 `dynamic_pointer_cast` 147 → 0 | 把 4 条 `if-else + dynamic_pointer_cast` 分派链收进类型系统；**判据单点**写在 `include/semantic_analyzer.h` 的注释里 |
| 2026-09-16 | 判据落定：handler 只要引用 → 访问者；还要 `shared_ptr` 所有权或返回值 → 标签分派 | 批次 3 曾因给**声明链**硬套访问者而编译失败（注册表存 `shared_ptr`，`visit` 只给引用）⇒ 回退催生了这条判据 |
| 2026-10-29 | src 按 clang 分域搬移（`ast.h` → `include/ast.h`，`type.h` → `src/ast/type.h`），12 个配对头文件与实现同域 | 用户 2026-10-23 拍板"代码结构对齐 clang"；配对头文件同域修 CLion 导航 |
| 2026-11-02 | 源码注释对齐「四段式」标准（learn/50） | 注释对语义零影响 ⇒ logdiff 零漂移是**标准保证**的，不是运气 |
