# 01 函数模板解析（S1）

> 子阶段 S1：让 Parser 认识 `template<typename T> T twice(T x) {...}`，
> 把函数模板与类模板一样"冻结"成蓝图，并建立同名候选集，为 S2 实参推导铺路。

## §0 定位与能力清单

同一个 `twice` 写法，`twice(21)` 与 `twice(2.5)` 是两个**毫不相干**的函数——
C++ 用"一份语法骨架造出无数个函数"实现这件事，而那份骨架就是**蓝图**。
本文讲的是这条流水线的**第一段**：把 `template<typename T> T twice(T x)` 识别成蓝图、
登记起来，**而不是**一个此刻就能调用的函数。蓝图本身不产生任何符号，
真正的函数要等调用点推出实参后才被造出来（见 05 号文档）。

| 能力 | 一句话 | 状态 |
|---|---|---|
| [temp.fct] 函数模板蓝图 | `template<...>` 后的声明冻结成 `TemplateDecl`，登记但不产生可调用符号 | 已实现 |
| [temp.decl] 模板形参表 | `typename T` / `class T` 收进形参表（`typeParams`），两种拼写等价 | 已实现 |
| [temp.deduct] 形参类型模式 | `T&` / `const T*` 解析成可参与 P/A 配对的类型**模式**（不是具体类型） | 已实现 |
| [temp.dep] 两阶段查找第一阶段 | 蓝图期只注册、**不分析函数体**，依赖名推迟到实例化时才解析 | 已实现 |
| 同名候选集 | 函数模板进 `m_functionTemplateCandidates`，供 S6 重载决议遍历 | 已实现 |
| [temp.arg.explicit] 裸模板名诊断 | 裸写 `Box` 不带实参时响亮报错，提示补实参 | 已实现 |
| 显式实例化与链接语义 | `extern template`、重复声明检查、`friend`/`export` | 未做 |
| 模板模板形参 / 模板形参默认实参 | `template<template<class> class C, class T = int>` | 部分（模板模板形参见 learn/33；形参默认实参未做） |
| requires 子句与概念（concepts） | `template<class T> requires ...` / `concept` 关键字 | 未做 |
| 可变参数模板与包展开 | `template<class... Ts> void f(Ts...)` | 未做 |

## ① 理论背景

**标准依据**：C++ [temp.fct]（函数模板）、[temp.decl]（模板声明总则）。

- **函数模板不是函数**，是"造函数的模具"。`template<typename T> T twice(T x)`
  声明了一个参数化的蓝图；只有被调用（`twice(21)`）或显式实例化时才产生真正的函数。
- **两个参数层**：`template<...>` 是**模板参数层**（编译期的类型变量），
  `(...)` 是**函数参数层**。S2 的 P/A 配对推导就发生在函数参数层的类型模式上——
  所以 S1 必须把 `T&`、`const T*` 这些形态解析为可匹配的类型模式。
- **两阶段名称查找**（[temp.dep]）：模板体里依赖 T 的名字（如 `x + x` 的运算符）
  推迟到实例化时检查。这就是为什么 S1 的 sema **只注册蓝图、不分析函数体**——
  和 clang 的行为一致：模板定义在声明时只做"非依赖"部分的检查。
- 对应编译原理概念：参数化语法产物（parameterized production）——
  文法的一个产生式带上了"类型变量"，实例化是对这些变量做替换后展开。

## ② 设计决策

| 决策 | 选择 | 理由 |
|---|---|---|
| AST 形态 | `TemplateDecl` 增加 `funcTemplate` 槽位，与 `classTemplate` 互斥 | clang 用继承（`FunctionTemplateDecl : TemplateDecl`）；本项目 AST 扁平无继承体系，双槽位 + `isClassTemplate()/isFunctionTemplate()` 判别法改动最小、最易讲解 |
| 分派依据 | `template<...>` 后的第一个 token：`class` → 类模板；类型关键字/标识符 → 函数模板 | 返回类型可以是模板参数名 `T`（词法上是 Identifier），不能只靠关键字判断 |
| 函数体处理 | sema 只注册蓝图，**不进 Pass 3 函数体分析** | 两阶段查找：依赖名推迟。模板体在 S5 实例化克隆后才走正常函数分析流程 |
| 候选集存放 | sema 新增 `m_functionTemplateCandidates: name → vector<TemplateDeclPtr>`，不进 Scope/SymbolTable | Scope 保持"名字→单符号"的教学模型；clang 的重载集同样挂在 DeclContext 上而非普通查找表。S6 重载决议遍历此候选集 |
| 复用 | 函数模板签名直接复用 `parseFunctionDecl()` | 模板参数名 T 走现有 `parseType()` 的 Identifier→class 分支，类型系统已有 TemplateParam 种类（type.h），S2 推导时再精确区分 |

## ③ clang 对照表

| 本实现 | clang 源（llvm-project/clang） | 简化了什么 |
|---|---|---|
| `parseTemplateDecl` 类/函数分派（src/parser.cpp） | `lib/Parse/ParseTemplate.cpp → Parser::ParseDeclarationStartingWithTemplate`：按后续 token 分派到类模板/函数模板/概念等 6 种 | clang 还要处理 requires 子句、模板模板参数、默认实参、别名模板；本项目只留类与函数两种 |
| `TemplateDecl{classTemplate,funcTemplate}` 双槽位 | `include/clang/AST/DeclTemplate.h`：`TemplateDecl`（基类，持有 TemplateParameterList + templated decl）→ `FunctionTemplateDecl` / `ClassTemplateDecl` | 无继承、无 TemplateParameterList 对象（参数名用 `vector<string>`），不支持非类型模板参数 |
| `processTemplateDecl` 注册候选集（src/semantic_analyzer.cpp） | `lib/Sema/SemaTemplate.cpp → Sema::CheckFunctionTemplate`：创建 FunctionTemplateDecl 并注册进 DeclContext 的重载集 | 无重复声明检查、无链接语义、无 friend/export |
| 蓝图不分析函数体 | `Sema::ActOnFinishFunctionBody` 对模板体只做非依赖名检查，依赖名留给实例化 | 本项目完全不检查模板体（更极端的简化） |

## ④ 实验手册

```bash
cd /home/magene/runtime/cppproject/mycompiler

# 本实现：观察 S1 日志
./build-linux/minicc tests/test_tmpl_11_func_basic.cpp --dump-ast
#   关注：[parse:template] function blueprint / [register] candidate set / TemplateDecl(function)

# 边界形态（多参数、T&、const T&、typename/class 混用）
./build-linux/minicc tests/test_tmpl_12_func_forms.cpp

# 错误用例（模板体是变量声明）→ 期望 [ERROR] Expected '(' after function name，退出码 1
./build-linux/minicc tests/test_tmpl_13_error_not_decl.cpp; echo "exit=$?"

# oracle：clang 里函数模板的 AST 形态
clang++-18 -Xclang -ast-dump -fsyntax-only tests/test_tmpl_11_func_basic.cpp | head -20
#   对照点：FunctionTemplateDecl 包裹 FunctionDecl + TemplateTypeParmDecl，
#   与本项目 TemplateDecl(function) → FunctionTemplate 的两层结构同构
```

## ⑤ 关键过程图

```
源码                        Token 流                         AST
─────────────────────────────────────────────────────────────────────────
template<typename T>    [template < typename T >]
T twice(T x) {          [Ident:T Ident:twice ( ...]
  return x + x;              │
}                            ▼
                    parseTemplateDecl()
                    ├─ 解析模板参数层 → typeParams = [T]
                    └─ 分派：当前 token = T（Identifier）
                         ≠ class → parseFunctionDecl()
                                   ├─ parseType → T（模式！不是具体类型）
                                   ├─ 参数 (T x)
                                   └─ body 冻结不分析
                                     │
                                     ▼
                    TemplateDecl {
                      typeParams: [T]
                      funcTemplate: FunctionDecl {
                        name: twice
                        returnType: T
                        parameters: [T x]      ← S2 推导的"模式 P"
                        body: <冻结>
                      }
                    }
                                     │
                    sema Pass 1      ▼
                    m_functionTemplateCandidates["twice"] = [蓝图]
                                     │
                    S2（下一阶段）：调用 twice(21)
                    遍历候选集 → P=T, A=int ⇒ T := int → 克隆蓝图 → 实例
```

## ⑥ 补注：类模板注册表的对称化（2026-09 修订）

S1 落地时函数模板拿到了名字→候选集（`m_functionTemplateCandidates`），
而类模板只进了公共的 `m_templates` 线性 vector——`resolveType` 判定
`Box<int>` 是否为模板、`getOrInstantiateClass` 找蓝图，都要 O(n) 扫描。
本次修订补齐对称性：

| 决策 | 做法 | 对照 clang |
|---|---|---|
| 类模板注册表 | `m_classTemplates: name → TemplateDeclPtr`，`processTemplateDecl` 类模板分支 `emplace` 注册（重名取先注册者，与旧线性扫描语义一致） | 类模板名经 `DeclContext::lookup` 命中 `ClassTemplateDecl` |
| 裸名诊断 | `resolveType` 中实参为空且命中注册表 → 早期报 `'Box' is a class template; provide template arguments`（[temp.arg.explicit]） | clang：`use of class template 'Box' requires template arguments`；若启用 CTAD 则先报 `no viable constructor or deduction guide for deduction of template arguments`（见 test_tmpl_20） |
| 不动的部分 | `m_templates` 保留有序列表（Phase 4 驱动遍历），符号表不登记蓝图 | 实例化产物是 `ClassTemplateSpecializationDecl`，与模板 Decl 分开 |

```bash
# 复现：裸类模板名错误（错误用例 20）
./build-linux/minicc tests/tmpl/test_tmpl_20_bare_template_error.cpp  # 期望 exit≠0
# 注册日志：Sema Phase 3 可见 "↳ class template 'Box' registered"
./build-linux/minicc tests/tmpl/test_tmpl_01_basic.cpp | grep registered
```

## ⑦ 真实版多做了什么、为什么

★ 素材来源：本文 §③ 对照表的「简化了什么」列 + `docs/BUGS.md` B31/B32/B35/B36/B37/B38
+ CLAUDE.md「名字可见性三阶段」与「未做」清单。**凡是本项目未查证的写「（本项目未查证）」**。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `Parser::ParseDeclarationStartingWithTemplate` 按后续 token 分派到类模板 / 函数模板 / 概念 / 变参模板 / 别名模板等 6 种 | 本项目只留**类模板与函数模板**两条分支——这是"蓝图冻结"这条主线的全部；别名模板与模板模板参数后来单独补（learn/27 / learn/33），概念与变参模板至今未做 |
| `TemplateDecl` 继承体系（`FunctionTemplateDecl` / `ClassTemplateDecl`）＋独立的 `TemplateParameterList` 对象 | 本项目 AST **扁平无继承**，用双槽位 + `isClassTemplate()/isFunctionTemplate()` 判别法代替；形参名存 `vector<string>`，没有独立形参对象 ⇒ 无法承载"形参有默认值/有种类"（后来 NTTP 批次才补上 kind） |
| `Sema::CheckFunctionTemplate` 的重复声明检查、链接语义、`friend`/`export` | 教学编译器不做 ODR 判定；这三项会牵出整套可见性与链接语义机制，**理论增量远大于实现量** |
| `Sema::ActOnFinishFunctionBody` 对模板体做**非依赖名**检查（依赖名才推迟） | 本项目**完全不检查**模板体（更极端的简化）：S5 实例化后才走完整分析。代价是模板体里的语法错误要等到有实例才报出来 |
| 模板形参默认值 / 变参形参 / 模板模板形参的完整解析 | 形参表只承载"名字 + 可选非类型类型"两个槽位；默认值与包展开未做，变参模板未做 |
| 模板实参的**语境消解**（clang 走 `Actions::ClassifyName` 问符号表） | Parser 不持 Sema 活句柄，用**结构判据**近似（名字 `<` 类型列表 `>` 后紧跟 `(`）。这条路后来被 B31/B32/B35/B36 追打了四轮才收敛（learn/45 / learn/46 / learn/48 / learn/49）——**"结构判据"能起步，但撑不住模板名当变量名的场景** |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-08-10 | S1 落地：Parser 认识函数模板、冻结成蓝图、建立同名候选集 | 为 S2 实参推导铺路（本文成文时的状态） |
| 2026-09-10 | 补注：类模板注册表对称化（`m_classTemplates` 名字→蓝图映射 + 裸名提前诊断） | 类模板名查找从 O(n) 线性扫描改成查表 |
| 2026-10-23 | 模板名登记表上线（learn/46，B31）：消歧从"纯后缀启发式"升级为"只读查表 + 试探" | `a<b>(c)` 这种全变量比较链被旧判据误判成模板调用（**拒收合法程序**） |
| 2026-11-02 | 名字可见性与作用域下沉 Sema（B37/B38，learn/48 / learn/49） | 模板按**裸名**注册 ⇒ `N::B` 绑到全局 `B`、同名模板跨命名空间撞成同一个实例——作用域是名字身份的一部分 |
