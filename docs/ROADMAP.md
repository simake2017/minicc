# 后续计划（ROADMAP）

> 状态快照（2026-11-02）：P0 主线（函数模板推导 S1~S6、预处理器）+ 主线 A（构造/析构）
> + 主线 B（自研链接器）+ 主线 G（多继承布局，见 learn/17）已完成。
> 回归红线 test_tmpl_01..10 全绿；mi_03 为登记的既有失败（见主线 G 待办表）；
> **mi_04 已不再失败**（2026-10-08：B20 缺陷 b 修好后菱形 rc=0，与 clang 一致 ——
> 非虚继承的菱形在标准下合法；该文件已改写为私有继承错误用例）。
> 构建：`cmake -S . -B build-linux -DCMAKE_CXX_COMPILER=clang++-18 && cmake --build build-linux -j`
>
> ★ **2026-11-03**：§0.1 批 0-1（**按值返回 + 拷贝省略**）已完成
> （docs/learn/57 + BUGS.md B48/B49；连带登记 B50 全局类对象缺口）
> ⇒ §0.1 批 0 的三项全部收口，**下一批 = 批 0-3（B50）**，之后按 §0.2 进主线 K 六项。
>
> ★ **2026-11-02 重排**：本文件原来按"批次先后"排列（旧的在前面），
> 下个会话读到顶只看到一堆 ✅ 的已完成主线，看不出**下一批该做什么**。
> 现改为：**§0 当前优先级（执行序）→ §1 下一条主线 → §2 既有主线现状（存档）**。
> 重排只动**顺序与优先级**，不删任何历史内容（判据⑤ 资产）。

---

## 0. 当前优先级（下一批起按此顺序）★ 2026-11-02 重排

**一句话**：**理论密度优先** —— 先补「标准有明确条文、clang 有现成答案、且能讲清楚为什么」
的模板语义；语法面欠账整体降级到 §0.3；**暂不开新战线**（§0.4 明写，免得下个会话顺手加功能）。

**排序判据**（两条，缺一不可）：

1. **缺陷优先于特性**：严重度按
   `静默算错` > `静默不匹配` > `诊断不对（clang 也拒，只是文案不同）` > `功能缺失（clang 接受、minicc 拒）`
   —— 与 `docs/BUGS.md` 的 severity 口径一致。
   ★ 前两档之所以最重：**"静默"意味着没有症状**，不会被任何测试、任何日志、
   任何用户报告抓到（B27 / B34 / B46 三次都是这样溜过去的）。
   ★ 第三档与第四档的次序是成本序：改文案不动语义，比补一整条特性便宜一个数量级。
2. **理论密度**：能点名标准章节 + clang 源码对照点 ⇒ 值得单独成批；
   纯"多一个语法糖" ⇒ 降级。

### 0.1 批 0：缺陷与边界收口（成本低，先做）

| # | 项 | 性质 | 现状（clang++-18 对拍，2026-11-02） | 落点 | 验收 |
|---|---|---|---|---|---|
| 0-1 ✅ | **按值返回 + 拷贝省略**（主线 A 延伸） | ★ **静默算错** | `S f(){S s; s.v=7; return s;} S x=f();` ⇒ clang rc=**7** / minicc rc=**0**（初始化式被丢弃，对象退化成默认构造） | sret 风格返回缓冲 + 逐字节拷贝；`visit(VarDeclStmt)` 的平凡拷贝分支补"调用表达式返回类对象"形态 | ✅ **已完成 2026-11-03**：rc=7 与 clang 对拍；**sret 形态**（定义点 spill 隐藏指针 + 返回路径逐字节拷贝 / 调用点 rdi 装缓冲 + 实参右移 / 成员 this 让到 rsi / 虚调用同款）+ **拷贝省略**（缓冲 = 变量槽）；判据单点（"返回类型是不是类"，三处同源）；ctest 403 → **413**；集成新增 1 份、**有意漂移 43 份**（B48 的 push/pop 两行 + 连带的 `.text` 尺寸/`main @` 地址位移；**rc 与运行输出逐字节不变**）；docs/learn/57 + BUGS.md B48/B49 |
| 0-2 ✅ | **实例化步骤 5.8：克隆嵌套类**（批 4 留的边界） | 边界收口 | `template<class T> struct Box{ struct Inner{T v;}; }; Box<int>::Inner i;` ⇒ clang rc=0 / 修前 minicc rc=1 `no type named 'Inner' in 'Box_int'` | `instantiateClassTemplate` 增步骤 5.8：蓝图嵌套类克隆进实例（字段/方法随形参替换），登记名 `Box_int::Inner` | ✅ **已完成 2026-11-03**：rc=0 与 clang 对拍；ctest 396 → **403**；167 份集成日志逐字节零漂移；docs/learn/56 + BUGS.md B47 |
| 0-3 | **全局类对象**：存储尺寸 + 地址约定（BUGS.md B50） | ★ **段错误 / 静默踩内存** | `struct S{int v;}; S g; int main(){ g.v = 11; return g.v; }` ⇒ clang rc=**11** / minicc **SIGSEGV**（`g.v = 11` 这种基本写法就崩） | ① 全局变量发射按 `classLayout.totalSize` 分配存储（不再一律 `.quad 0`）；② `visit(VarExpr)` 的全局路径补"类对象 ⇒ 取地址"分支（与局部 `m_classLocals` 同款）；连带修"按引用返回类对象" | 运行期退出码与 clang 一致；三条用例（全局字段读写 / 全局类对象按值返回 / 按引用返回）全绿 |

### 0.2 批 1~6：主线 K —— 模板线六项（理论密度最高）

明细见 **§1**（每项含：标准章节 / clang 参照 / 实测现状 / 落点 / 验收 / 产物）。

| 序 | 项 | 性质 | 一句话 |
|---|---|---|---|
| K-1 | 别名模板的**偏特化 / 显式特化** | 诊断不对（clang 也拒） | 这两种写法在标准下**非法**（clang 明确报错）⇒ 目标不是"支持"，是**拒得与 clang 逐字相同** |
| K-2 | 偏特化模式里的**值位**（待推导名形态） | ★ **静默选错** | `Box<int,N>` 匹配不上 ⇒ 静默退回主模板 |
| K-3 | `template<auto V>` 的**偏特化** | ★ **静默选错** | 静默退回主模板 ⇒ 成员集不同（clang 选偏特化） |
| K-4 | 模板模板形参：**形参包 / 默认实参 / 嵌套**（depth>1） | 功能缺失 | 三种写法 clang 全接受，minicc 全拒 |
| K-5 | **类外定义特化成员** | 功能缺失 | clang 接受，minicc 解析期拒 |
| K-6 | **函数模板的显式 NTTP 实参** | 功能缺失（已响亮拒收，文案自造） | `add<4>(1)` ⇒ 先改 clang 同款文案，再真支持 |

★ **K-1 的方向要单说**：别名模板**不能**偏特化也不能显式特化（[temp.alias]/1）——
clang 的措辞是 `partial specialization of alias templates is not permitted` /
`explicit specialization of alias templates is not permitted`。
所以 K-1 是"**把非法程序拒得更准**"，**不是**"补一个特性"。
★ 这类"缺口其实是缺口"的条目最容易被后来的会话读成"待实现"—— 故在此单列一行说清方向。

### 0.3 降级区：语法面欠账（**不占主线位**）

触发条件：① 用户点单；② 主线 K 收尾后按余量捡；③ 某条欠账被别的特性**依赖**时顺手做。
共同形态：多为**语法面**（解析糖一类），理论点薄、clang 参照分散 ⇒ 不单独成批。

| 项 | 来源 | 现状一句话 |
|---|---|---|
| ④ [stmt.ambig] 完整裁决 | learn/36 §7 | 语句层只判了"声明 vs 表达式"，完整四选一（表达式/声明/标签/歧义）未做 |
| ⑥ 后置 const 的重载区分 + const 正确性检查 | learn/36 §7 | 后置 const 不参与重载；`const a.v = 5` 宽松放行（clang 拒收）⇒ **宽方向** |
| 类外成员定义 `int C::f() const {}` | learn/36 | 类外定义只做了 static 成员（`int C::g = 1;`），方法没有 |
| **NSDMI 类内成员初值**（`int x = 5;`）+ 多声明符 `int x, y;`（**ctor 增强批**，BUGS.md B53） | learn/08 延伸 | 字段文法收窄为 `type name [array] ;`；clang Parser.h 自身 19 处使用、llvm 千行级 ⇒ 不是冷门语法 |
| 函数默认实参 | learn/20 | `void f(int x = 0)` 不解析 ⇒ enable_if 惯用法写不出 |
| 函数形参里的 decltype 依赖表达式 | learn/20 | `f(T t, decltype(t.begin())* g)` 推导期不绑定 t |
| 限定名调用成员函数 `d.Base::f(2)` | learn/36 §7 | B21 只管数据成员 |
| `using Base::f;` 拉回被隐藏的名字 | learn/36 §7 | 无 using 声明 |
| 派生类到基类的引用绑定 `Base& b = d;` | learn/36 §7 | — |
| 转换等级排序 [overload.icp] | learn/36 §7 | 择优只有"精确 → 可隐式转换 → 首个" |
| `v[i]` 的值类别按 `at()` 返回类型判定 | learn/36 §7 | 现按节点种类判 |
| `operator|` / `operator||` 那半边 | learn/20 | 依赖 `std::enable_if_t`（类型级条件选择） |
| `std::enable_if_t` 类型级条件选择 | learn/27 | 需别名底层类型能表达 `cond ? X : Y` |
| 区间 for / 赋值表达式化 | 主线 C 剩余 | for-range 与 `for(...; i=i+1)` 的赋值表达式化 |
| constexpr 函数 / `if constexpr` / 浮点折叠 | 主线 D 剩余 | 需可解释器求值器 |
| `T[N]` 数组类型偏特化 | 主线 E 延伸 | 需给 `TypeKind::Array` 开偏特化路径 |
| 字面量溢出自动升格 / `\x` `\u` 转义 / 非整型 NTTP | learn/35 §5 | — |
| 纯右值 CTAD / 拷贝推导 / 聚合推导 / 指引参与重载决议 / CTAD 用于 `new` | learn/28 §5 | — |

### 0.4 暂不开新战线（明写出来，免得"顺手加个功能"）

| 候选 | 为什么现在不做 |
|---|---|
| 主线 F 深水区（运算符重载 / lambda / 异常 / 虚继承） | 各自是独立子系统（重写集 / 闭包转换 / LSDA / vbptr 布局），与模板线不共享地基；当前无教学刚需 |
| `auto` 类型推导（P2） | 与 `template<auto V>`（NTTP 的 deduced 形参）同名不同物、易混；且要新开一层推导 |
| 移动语义 / 右值引用全套 | 依赖拷贝语义先修（批 0-1），且要动 ABI（sret + move ctor） |
| 并行 / 增量编译 / JIT / LTO | 超出"学习编译器理论"的项目定位（CLAUDE.md 最高优先级） |
| 除 ministl 外的第三方库适配 | 教学项目；ministl 双栖桩已足够 |

---

## 1. 主线 K：模板线收尾六项（下一条主线，理论密度最高）

**为什么单独立线**：这六项都是**标准有明确条文、clang 有现成答案**的语义缺口，
不是"再加个语法糖"。它们共享同一套地基（模板实参表 / 模式匹配 / 合一算法），
一批做完，模板这块才算封口；再往下就是语法面（§0.3）与深水区（§0.4）。

### K-1 别名模板的偏特化 / 显式特化 ⇒ **拒得更准**

| 字段 | 内容 |
|---|---|
| 理论点 | [temp.alias]/1：别名模板**不是新类型**，只有"解糖"没有"实例化" ⇒ **不存在**偏特化与显式特化；写了就是非法程序 |
| clang 参照 | `clang/lib/Parse/ParseDeclCXX.cpp:Parser::ParseUsingDeclaration`（`err_alias_declaration_specialization`，`%select{partial specialization\|explicit specialization\|explicit instantiation}`）；诊断定义在 `clang/include/clang/Basic/DiagnosticParseKinds.td:err_alias_declaration_specialization` |
| 现状（实测） | clang：`partial specialization of alias templates is not permitted` / `explicit specialization of alias templates is not permitted`；minicc：也拒，但文案是 parse 层的 `Expected '=' in using declaration` ⇒ **拒对了程序、拒错了话** |
| 落点 | `src/parse/parser.cpp` 的别名模板解析分支：识别"这是特化形态"⇒ 报 clang 同款文案（带 `<` 的位置） |
| 验收 | 负向用例 rc=1 且文案与 clang **逐字相同**；正例（普通别名模板）零漂移 |
| 产物 | tests/tmpl + docs/learn/55 |

### K-2 偏特化模式里的值位（待推导名形态）

| 字段 | 内容 |
|---|---|
| 理论点 | [temp.spec.partial]：偏特化的**模式**里每个位置都是"待绑定的模式" —— 类型位绑类型、**值位同样绑值**（`Box<int,N>` 里的 N 是个待推导名，不是常量） |
| clang 参照 | `clang/lib/Sema/SemaTemplate.cpp:Sema::CheckTemplatePartialSpecialization`（形态校验）；匹配侧与 `clang/lib/Sema/SemaTemplateDeduction.cpp:Sema::DeduceTemplateArguments` 同族 |
| 现状（实测） | ★ **常量值位已支持**（`Box<T,0>` 匹配并选中，docs/learn/34 §3.3 打的地基）；❌ **待推导名形态不匹配** —— `Box<int,N>` 报 `type pattern 'N' does not match non-type argument '3'` ⇒ **静默退回主模板** ⇒ 静默选错（clang 选偏特化） |
| 落点 | `src/sema/semantic_analyzer.cpp` 的偏特化候选匹配：值位模式要能被实参**反向绑定**（像类型位那样产出 `N := 3`） |
| 验收 | `Box<int,3>` 选中偏特化；与 clang 对拍（成员集不同 ⇒ 访问偏特化独有的成员必须成立） |
| 产物 | tests/tmpl + docs/learn/56 |

### K-3 `template<auto V>` 的偏特化

| 字段 | 内容 |
|---|---|
| 理论点 | [temp.param]/6 的 deduced 形参：**形参即类型**（形态参与匹配）⇒ `template<int V> struct K<V>` 里的 `V` 既是值也是形态 ⇒ 偏特化匹配要同时看"值相等"与"形态相等" |
| clang 参照 | `clang/lib/Sema/SemaTemplate.cpp:Sema::CheckTemplatePartialSpecialization` |
| 现状（实测） | clang：接受并**选中偏特化**；minicc：`type pattern 'V' does not match non-type argument '3'` ⇒ 退回主模板 ⇒ 访问偏特化独有成员报 `No member 'b' in class 'K_3Cint'` ⇒ **静默选错** |
| 落点 | 同 K-2（同一个匹配引擎的两处形态差异：`auto` 形参位要按"形态 + 值"两维比） |
| 验收 | `K<3>` 命中 `template<int V> struct K<V>`；`K<3L>` 不命中（形态不同，learn/35 已建立"形态即身份"） |
| 产物 | tests/tmpl + docs/learn/57 |

### K-4 模板模板形参：形参包 / 默认实参 / 嵌套（depth>1）

| 字段 | 内容 |
|---|---|
| 理论点 | [temp.param]/8（形参包）、[temp.arg.template]/2（模板模板实参的**逐位签名匹配**）、[temp.param]/4（嵌套模板模板形参） |
| clang 参照 | `clang/lib/Sema/SemaTemplate.cpp:Sema::CheckTemplateTemplateArgument`（learn/33 已对齐它的"逐位签名"口径） |
| 现状（实测） | ① **形参包** `template<class...> class Box` 与 `template<template<class...> class C>`：minicc parse 期拒（`Expected '>' after template parameters`），clang 全接受；② **默认实参** `template<template<class> class C = B1>` + `W<>`：minicc 报 `'W' is a class template; provide template arguments`，clang 接受；③ **嵌套 depth>1** `template<template<template<class> class> class C>`：minicc 已响亮拒收（文案自造），clang 接受 |
| 落点 | Parser 的模板形参表（形参包 + 嵌套）、Sema 的模板模板形参登记（默认实参）、`src/sema/semantic_analyzer.cpp` 的签名匹配（形参包位按"包"匹配） |
| 验收 | 三种写法 rc=0 与 clang 对拍；签名不匹配仍拒（learn/33 的 `ttpSignatureMismatch` 三条判据不受影响） |
| 产物 | tests/tmpl + docs/learn/58 |

### K-5 类外定义特化成员

| 字段 | 内容 |
|---|---|
| 理论点 | [temp.expl.spec]：显式特化的成员**在类外定义**时，名字里要带**特化后的实参表**（`void Box<int>::f()`），不是蓝图的名字 |
| clang 参照 | `clang/lib/Sema/SemaTemplate.cpp` 的特化处理；类外成员定义走 `clang/lib/Sema/SemaDeclCXX.cpp` |
| 现状（实测） | clang rc=0；minicc parse 期拒：`Expected ';' after global variable declaration`（把 `Box<int>::f` 当成变量声明解析） |
| 落点 | `src/parse/parser.cpp` 的全局声明分支（先判"类外成员定义"形态）+ `src/sema/semantic_analyzer.cpp` 的定义点登记（名字要取特化后的实例名） |
| 验收 | rc=0 与 clang 对拍；链接期符号与调用点一致（★ 承 B10/B22 的教训：**同一条语义判断不许写两处**） |
| 产物 | tests/tmpl + docs/learn/59 |

### K-6 函数模板的显式 NTTP 实参

| 字段 | 内容 |
|---|---|
| 理论点 | [temp.arg.nontype]/2 + [temp.explicit]：函数模板也能有 NTTP 形参，且实参可显式给出（`add<4>(1)`）；此时**推导不参与**，实参表直接来自书写 |
| clang 参照 | `clang/lib/Sema/SemaTemplateDeduction.cpp:Sema::DeduceTemplateArguments`（显式实参走另一条路：`Sema::ActOnTemplateId`） |
| 现状（实测） | minicc 响亮拒收，文案自造：`explicit non-type template argument '4' for function template 'add' is not supported yet (only class templates support non-type parameters)`；clang rc=0 |
| 落点 | Parser 的显式实参表（函数模板分支）+ Sema 的显式实参校验（形态/可表示性，判据复用 `Type::canRepresentValue`）+ mangling（`TnDa` 只在这里出现） |
| 验收 | rc=0 与 clang 对拍；**mangling 逐字符对齐**（learn/35 已建 12/12 的核对表，本项要补 `TnDa` 那一格） |
| 产物 | tests/tmpl + docs/learn/60 |

★ **六项的共同纪律**（每项开批时照抄）：
① 语义 oracle 先用 `clang++-18` 探针确认，**写用例前先验证程序本身合法**
（mi_04 / pp_01 / B24 三次把非法程序固化进基线的教训）；
② 负向用例文案与 clang **逐字相同**（红线 6）；
③ 判据**单点**（红线 3）；
④ 三绿收尾：`./build.sh` 0 警告 + `ctest --test-dir build-linux -j6` 全绿 +
`./logdiff.sh diff` 零漂移（红线 5）；有意的日志漂移**显式列出**再重刷基线；
⑤ 突变验证：每条新判据都要有"摘掉它 ⇒ 测试变红"的负向验证（红线 7）。

---

## 2. 既有主线的现状（存档；执行序见 §0）

## 主线 A：构造/析构（主体已完成 2026-10；剩一条延伸见 §0.1 批 0-1）

**理论点**
- Itanium ABI 构造变体：C1（complete，含虚基类）/ C2（base，子对象构造）/ C3（allocating，配 new）
- 成员初始化顺序 = **声明顺序**（与初始化列表书写顺序无关——经典陷阱）
- 构造三步：分配 → vptr 安装 → 成员按序初始化；析构逆序 + 虚析构的 deleting destructor（D0/D1）
- 隐式默认构造/复制构造的生成条件（Sema 层）

**参照**
- `clang/lib/CodeGen/CGClass.cpp`：`EmitConstructorCall`、`InitializeVTablePtr`
- `clang/lib/Sema/SemaDeclCXX.cpp`：隐式特殊成员函数生成
- 本项目现有基础：`NewExpr.constructorArgs` 已存在（只缺真正的 ctor 声明与调用）

**任务分解**
1. AST/Parser：识别"与类同名的方法"为 ctor、`~名字` 为 dtor；初始化列表 `: a(x), b(y)`
2. Sema：无 ctor 时生成隐式默认 ctor；成员初始化列表排序校验（警告乱序）
3. CodeGen：ctor 发射 C2 变体（vptr 安装 + 成员初始化）；`new T(args)` → malloc + C3/C1 调用；`delete` → D1 + free
4. 虚析构：基类指针 delete 派生对象走 vtable[0]

**验收/产物**：tests/test_ctor_01..NN（含初始化顺序陷阱用例、多态 delete 用例）+ docs/learn/08

**遗留缺口（★ 静默算错，响亮拒收反而更好）** ⇒ ✅ **已收口（§0.1 批 0-1，docs/learn/57）**：
`S x = f();`（函数返回**类对象**）的初始化式被**丢弃** —— 对象退化成默认构造，
运行期读到未初始化字段。此前**不报错**（编译全绿、结果错）。
根因：`visit(VarDeclStmt)` 的平凡拷贝分支只接 `Var`/`Member` 两种左值形态
（源码注释里写着"函数返回同类对象不涉及"），调用表达式落进"无初始化的类声明"。
修法 = **按值返回 + 拷贝省略**（sret 风格的返回缓冲 + 逐字节拷贝），
属主线 A 的延伸，**单独立批**（与嵌套类无关 —— 普通类同样中招）。
探针：`struct S{int v;}; S f(){S s; s.v=7; return s;} int main(){S x=f(); return x.v;}`
⇒ clang 7 / minicc **0**（字段没被拷过去）⇒ **修后 rc=7，与 clang 一致**。

## ✅ 主线 B：自动链接出可执行文件（已完成 2026-09-03）

**实际落地方案**（摸底后调整，见 docs/learn/15）：不调用系统 `ld`，
而是自研教学链接器 `src/linker.cpp`（~700 行），借助系统 `as` 产出 .o 后
自行完成 读 .o → 合并节 → 布局 → 符号决议 → 重定位回填 → 写非 PIE 可执行 ELF。
内置 `_start`（call main + exit_group syscall）与 64KB bump 版 malloc/free，
不依赖系统 ld / crt / libc。`-S` 保留只吐汇编。
产物：执行类用例一条命令直出二进制运行（退出码与 clang oracle 一致）+
docs/learn/15-linker-elf-and-mini-ld.md（ROADMAP 原定编号 09，
因 09..14 已被其他主题占用，顺延为 15）。

**理论点**
- 编译流水线全貌：.s → as → .o → ld → ELF；`_start`(crt1.o) → __libc_start_main → main
- crt 文件链：Scrt1.o / crti.o / crtn.o 的 init/fini 段作用；-lc 与动态链接器 /lib64/ld-linux
- 静态 vs 动态链接；`gcc -v` 观察真实链接行

**参照**
- `clang/lib/Driver/ToolChains/Linux.cpp`：链接行构造算法（crt 选择、搜索路径）
- 本项目 Phase 6 目前只打印提示命令

**任务分解**
1. minicc 默认产出可执行文件（fork/exec 调用系统 as+ld 或直接 cc），`-S` 保留只吐汇编
2. 链接行最小实现：`crt1.o crti.o <obj> -lc crtn.o`（从 `gcc -print-file-name=` 动态取路径，勿写死）
3. 错误透传：链接失败时原样展示 ld 报错

**验收/产物**：现有执行类用例（tmpl 14/15/17/19、pp 01..03）一条命令直出二进制并运行 + docs/learn/09

## ✅ 主线 G：多继承布局（已完成 2026-09-09，见 docs/learn/17）

**落地内容**：Itanium ABI primary base（第一个**多态**基类恒占 offset 0，
全非多态时首个基类视作 primary）、次表 thunk 覆写（thunkAdjust = -base.offset）、
size/align 分离（新增 `alignOf()`，类类型 align = 成员 align 递归 max）、
嵌套类字段内联子对象（Sema 字段 `resolveType` + CodeGen `leaq` 折叠 +
ctor 初始化列表嵌套构造调用）。
产物：tests/mi/test_mi_01..07 + docs/learn/17-multiple-inheritance-layout.md。

**登记待办（既有失败，非本轮引入）**

| 项 | 现状 | 修复方向 |
|---|---|---|
| mi_03 | lexer 不支持 `?:` 三元运算符，COMPILE_FAIL | 主线 C 顺带做（Lexer+Parser+Sema+CodeGen 四层） |
| ~~mi_04~~ | ✅ **已消**：原本的 `Q_f` LINK ERROR 是 B20 缺陷 b（次表槽被重造名字），不是"菱形没被拒" | 菱形在标准下**合法**（clang rc=0）；正例见 tests/mi/test_mi_12 |
| ~~B20 缺陷 c~~ | ✅ **已消**（2026-10-08）：槽位身份补上形参类型链（`VTableEntry::signature`） | 与 **B22** 同轮完成（"身份 = 名字 + 形参类型"四处落点收口），见 docs/BUGS.md；正例 tests/mi/test_mi_13 + 单测 `MemberIdentity.VirtualCallOnPointerStaysVirtual` |
| ~~槽位名靠字符串拼~~ | ✅ **已消**（2026-11-02，Stage 3 批 5）：vtable 槽改挂 `shared_ptr<FunctionDecl>` 句柄，符号名的唯一定名点是 `registerFunction` | 派生类覆写时**换句柄**即可，符号不可能与定义点漂移；★ 修掉 B45（同一方法两个符号 ⇒ 链接期 undefined reference）与 B46（析构槽靠符号子串判定 ⇒ `my_dtor_helper` 被误认成析构 ⇒ 静默少调一次析构）；见 docs/learn/54 |

**P2 架构观察（只记录，暂不动手）**

1. `decl->fields` 与 `classLayout.fields` 双清单 + 末尾整体回填
   （semantic_analyzer.cpp:891）；clang 是一次成型不可变 ASTRecordLayout
2. 成员查找不穿透继承链：`obj->get()` 在 `D*` 上查不到 `P::get`
3. ~~vtable 覆写靠字符串剥/拼类名前缀匹配符号名~~ → **已全消**（2026-11-02）：
   B20/B22 先把"命名规则"与"槽位身份"收口到单点
   （`memberMethodSymbolName` + `paramTypeChain`/`VTableEntry::signature`）；
   Stage 3 批 5 再把**槽里的字符串换成声明句柄**（`VTableEntry::method`），
   符号由 `vtableEntrySymbol()` 从句柄取 ⇒ 槽、定义点、调用点**三者同源**
4. **内置 bump malloc 不做对齐（待修）**：`new T` 的堆地址对齐保证在真实
   世界里由 operator new 契约提供（[new.delete.single]，x86-64 恒 16B 对齐；
   过对齐类型走 `operator new(size, align_val_t)`，clang 参照
   CGExprCXX.cpp::EmitCXXNewExpr 的 getNewAlignment()）。minicc 的
   linker.cpp:319 注入 malloc 是 `heapPtr + size` 裸推进——连续 new 多个
   小对象后，后续对象首地址可能不满足类型 align，alignOf 的布局计算在堆上
   不再成立。现有 mi 测试每例只 new 一个对象故未踩雷。修复方向：推进处
   `size = alignUp(size,16)`（add $15 / and $-16，3 条指令），并补 mi_08
   连续 new 两个小对象验证地址对齐

## ✅ 类模板特化（已完成 2026-09-14，见 docs/learn/19）

**落地内容**：`template<class T, class U = void>` 默认模板实参（使用点补全 + 空洞检查）、
`template<class T> class Box<T*, T>` 偏特化（复用 `TemplateDeducer::deducePair` 跑合一）、
`template<> class Box<int*, int>` 全特化（逐位类型相等）、`struct` 模板体；
择优顺序 ①全特化 → ②偏特化 → ③主模板补默认实参。
安装点：`SemanticAnalyzer::selectClassTemplate` + `m_partialSpecs`/`m_explicitSpecs` 两张注册表，
实例名与 mangling 一律取**使用点实参**。测试 test_tmpl_27..32 + ctest `PartialSpec.*` 4 例。

**未做（各自独立可接手）**
- **`[temp.class.order]` 偏序裁决**：多个偏特化同时匹配时应"更特化者胜"，
  现在取先注册者并打印结论。算法与 S6 函数模板偏序同族，复用现成引擎即可，
  缺的是候选集构造与歧义诊断 → 可作为 S6 的收尾任务。
- **NTTP 特化模式** `template<int N> class Buf<N>` / `Box<int, N>`：Parser 直接报错。
- **类外定义特化成员**、**函数模板特化**、**特化体的延迟匹配**（现在要求先声明后使用）。

## ✅ 第一/第二梯队（已完成 2026-09-14，见 docs/learn/24..28）

来源：对 `/root/cppproject/my-test/cpp/src/test/template` 那批模板用例做能力盘点后
排出的两项待补清单。

**第一梯队**
- ⑨ cv 限定位置修正 —— `const int*` = Pointer(Const(Int))，见 docs/learn/23
- 类内类型别名 `using X = T;` / `typedef T X;`（三个使用点）—— docs/learn/24
- 依赖类型名 `typename T::type`（+ `void_t<typename T::type>` 探测）—— docs/learn/25

**第二梯队**
- 别名模板 `template<class T> using Vec = MyPtr<T>;`（解糖而非实例化；三处解糖点；
  顺带补齐类模板 id 结构合一 + 实例"出身"记录）—— docs/learn/27
- ADL + 限定名查找 —— docs/learn/26
- CTAD + 推导指引（`MyPtr m(7);` / `Two(int) -> Two<int,int>;`）—— docs/learn/28

**未做（各自独立可接手）**
- `auto m = MyPtr(7);`（纯右值 CTAD）、拷贝推导 `MyPtr m = other;`（需先有拷贝语义）、
  聚合推导、指引参与重载决议、CTAD 用于 `new` / 函数返回类型 —— 见 docs/learn/28 §5 边界表。
- `std::enable_if_t`：需要**类型级条件选择**（别名底层类型里表达 `cond ? X : Y`）。
- 别名模板偏特化、别名模板作模板模板实参。
- ⑧ 一元 `*` 解引用（`return *p;` 不解析）→ ④ [stmt.ambig] 完整裁决 → ⑥ 后置 const。

## ✅ 实参绑定与名字可见性批（已完成 2026-10，见 docs/learn/36）

**来源**：一次"非模板调用在哪里做引用绑定检查"的反查 —— 答案是**哪里都没有**，
顺着查出的五个缺陷（BUGS.md B21 / B24 / B25 / B26 / B27）：

| 缺陷 | 一句话 | 性质 |
|---|---|---|
| B26 | 引用绑定判据只写在模板推导里 | `const T&` 拒右值（拒收合法程序）+ `int&&` 收左值（接受非法程序） |
| B24 | 成员方法的**名字隐藏**失效 | 接受非法程序（`d.f(2)` 查到基类去） |
| B25 | 成员调用个数不符无人报错 | 接受非法程序 / 报错落在链接器 |
| B27 | 无可行候选静默退回候选集首个 | **静默算错** |
| B21 | 限定名访问成员数据（`S::v` / `obj.Q::m`） | 拒收合法程序（语义层无通路 + 语法层不认） |

**落地内容**：`referenceBindsValueCategory`（判据单点，两个调用点共用；
值类别模型补上 `*p` / `v[i]`）、`inferCall` 的"查到名字就下结论"
（个数诊断与 clang 逐字相同）、`pickBestByArgs` 删掉"退回首个"、
`resolveQualifiedField` + **偏移回填**（`VarExpr`/`MemberExpr::resolvedFieldOffset`）。

**未做（各自独立可接手）**：限定名调用**成员函数** `d.Base::f(2)`、
`using Base::f;` 拉回被隐藏的名字、`v[i]` 的值类别按 `at()` 返回类型判定、
派生类到基类的引用绑定 `Base& b = d;`、转换等级排序 [overload.icp]（主线 D）——
完整边界表见 docs/learn/36 §7。

## 主线 C：控制流补全（P2，KwFor token 已留坑）

**理论点**：结构化控制的 CFG 降级（for → 条件块 + 回边）；break/continue 的标签栈语义；switch 的跳转表 vs if 链决策
**参照**：`clang/lib/Sema/SemaJump.cpp`；LLVM Kaleidoscope 第 5 章
**任务**：ForStmt/SwitchStmt/Break/Continue AST 节点 → sema 标签栈校验（break 在循环外报错）→ codegen 标签
**验收/产物**：tests/test_flow_01..NN + docs/learn/10

**✅ 首批已完成（2026-10，docs/learn/38）**：for（声明式/表达式式/空 init）、
break、continue、三元 ?:（含短路语义）。Sema 循环深度栈（m_loopDepth，
报错文案与 clang 逐字相同）；CodeGen 五标签降级 + 循环上下文栈
（continue 落增量步——`for(;;)` 不死循环的关键）；for 头局部放宽
「赋值是语句」的边界（init/incr 两格收 StmtPtr）。
**✅ 二批已完成（2026-10，docs/learn/41）**：switch / case / default、do-while、
逗号表达式。Sema `m_switchDepth` 让 break 在 switch 内合法（文案至此名副其实）、
`m_caseValueSets` 栈保证 case 值常量且不重（文案与 clang 逐字同）；
CodeGen switch 降级成比较链（逐条 cmp-je）、do-while 体先于条件、continue 落条件式；
逗号表达式纯字面量在常量折叠里消除。
**剩余**：区间 for、赋值表达式化（⇒ 已降级到 §0.3 语法面欠账）。

## 主线 D：常量折叠 / constexpr —— ✅ 首批 2026-10（learn/39）/ 二批 2026-10（learn/40）

**理论点**：编译期求值 = 解释器（部分求值/抽象解释入门）；常量传播与折叠代数规则；constexpr 的"必须可折叠"约束
**参照**：`clang/lib/AST/ExprConstant.cpp`（递归求值器）；LLVM InstCombine 折叠表
**任务**：sema 层折叠常量二元/一元表达式（日志展示折叠前后）；`constexpr` 关键字最小语义（初始化式必须可折叠）
**验收/产物**：tests/test_const_01..NN + docs/learn/11

> **与 NTTP 的接口（已打通）**：NTTP 的实参解析原先只认字面量与一元负号
> （`Buf<4>` / `Buf<-3>`，见 `Parser::parseTemplateArgumentList`）。
> ★ 主线 D 首批已把该处换成「解析完整常量表达式 → 折叠求值」，
> 于是 `Buf<2+2>`、`Buf<N*2>`（依赖前面的 NTTP）随之打通。
> 这是主线 D 最自然的第一个消费者。
>
> ★ **更新（NTTP 类型域扩展那一批，`docs/learn/35`）**：接口的**另一半已经补齐** ——
> 形参与实参的**类型域**不再局限于 `int`：
>
> | 能力 | 状态 |
> |---|---|
> | 形参整型家族 `unsigned` / `long` / `short` / `char` / 多关键字写法 | ✅ `tests/tmpl/test_tmpl_61` |
> | 实参字面量进制与后缀 `4L` / `4u` / `0x10` / `0b101` / `010` / `1'000` | ✅ `test_tmpl_62` |
> | 字符字面量 `'a'` / `'\n'` / `'ab'`（[lex.ccon]） | ✅ `test_tmpl_63` |
> | 值位整型转换 [temp.arg.nontype]/1（`Flag<1>` / `A<4L>`，**回归 B17**） | ✅ `test_tmpl_64` |
> | 窄化拒绝（`F<2>` / `U<-1>` / `D<300>`，文案与 clang 逐字同） | ✅ `test_tmpl_65/66/67` |
> | `template<auto V>`（类型由实参反推，[temp.param]/6） | ✅ `test_tmpl_68` |
> | **任意常量表达式** `Buf<2+2>` | ✅ `test_tmpl_74/75`（主线 D 首批，docs/learn/39） |
> | `Buf<k>`（const/constexpr 变量作实参） | ✅ `test_tmpl_76/77`（主线 D 二批，docs/learn/40） |
>
> ★ **已兑现（主线 D 首批，docs/learn/39）**：字面量 fast path 逐字节保留
> （仅当字面量后紧跟运算符时回退解析完整常量表达式），`TemplateArg` 加
> `valueExpr` 载荷，求值发生在【算缓存键之前】的归一预处理 —— 下游
> （形态归一 / 可表示性检查 / 缓存键 / mangling）确实一行没动。
> 剩余：constexpr **函数**（需可解释器的求值器）、`if constexpr`、浮点折叠
> ⇒ 已降级到 §0.3 语法面欠账。
> mangling 的 `TnDa`（`<template-param-decl>`，只用于**函数**模板的 `auto` NTTP）
> 仍未实现 —— 函数模板的显式 NTTP 实参目前直接报
> `explicit non-type template argument … for function template … is not supported yet`，
> 故没有写死代码。★ 该项已排进 **§1 K-6**（连同 `TnDa` 那一格 mangling 核对）。

## 主线 E：数组 / enum / namespace —— ✅ 主体已完成（数组 2026-10 learn/42；enum/namespace 2026-09 learn/08）

**理论点**：数组类型与指针退化（array-to-pointer decay）；enum 的底层类型；namespace 的限定名查找与 using 指令
**任务**：Type 增加 Array 种类 + 下标表达式；EnumDecl；NamespaceDecl + `::` 限定查找
**验收/产物**：tests/test_misc_01..NN + docs/learn/12

> **与 NTTP 的接口（已打通）**：NTTP 最经典的用法就是数组长度
> `template<int N> class Buf { int data[N]; }`。
> ★ **卡点已消除**（docs/learn/42）：`TypeKind::Array` 已落地
> （持 elementType / arraySize / arraySizeExpr，`sizeInBytes()` 由位宽推导），
> Parser 的 `parseArrayDeclaratorSuffix` 把 `[ expr ]` 挂到局部变量 / 全局变量 / 类成员字段
> 三处声明点，Sema 经常量折叠器把 `arraySizeExpr` 求值为正整数，
> CodeGen 局部数组栈分配 + 零初始化、全局数组 `.data` 节发 `.zero`，
> 数组名向首元素地址退化（array-to-pointer decay）。
> **剩余**：`T[N]` **数组类型偏特化**（需给 `TypeKind::Array` 开偏特化路径）⇒ 已降级到 §0.3。

## 主线 F（选做，深水区 ⇒ 2026-11-02 明确"暂不开新战线"，见 §0.4）

| 特性 | 理论点 | 参照 |
|---|---|---|
| 运算符重载 | 重写集（rewrite）vs 真正重载；成员/非成员形式 | SemaOverload.cpp |
| lambda | 闭包转换（closure conversion）：匿名类 + operator() + 捕获 | SemaLambda.cpp |
| 异常 | Itanium EH ABI：personality 函数、LSDA、zero-cost 展开 | libunwind + CGException.cpp |
| 虚继承 | 虚基类指针 vbptr、菱形继承布局 | CGRecordLayoutBuilder |

---

## 主线 H：decltype / SFINAE（编译期类型查询）—— ✅ 已完成（2026-09-14）

> **落地结果**：文档 `docs/learn/20-decltype-and-sfinae.md`（decltype + SFINAE）
> 与 `docs/learn/21-partial-ordering.md`（偏序裁决）；
> 测试 `tests/tmpl/test_tmpl_33..43` + `tests/unit/test_decltype_sfinae.cpp`
> 的 `Decltype.*` / `Sfinae.*` / `PartialOrder.*` 三套件（ctest 145/145）。
> 下方原始规划保留，末尾附「实际完成 / 未做」对照。



**为什么单独立线**：这不是"再补一个语法糖"，而是给编译器加一个**「表达式 → 类型」的求值子系统**。
它同时是主线 D（常量折叠/constexpr）的接口——两者都是"编译期求值"，共享同一套 AST 遍历。

**现状**：`decltype` 在 `src/` `include/` 里**零出现**，词法阶段就没有该关键字。
重载决议里的**软失败机制已具备**（`semantic_analyzer.cpp:2144`：候选推导失败仅 `candidate rejected`，循环继续；
全部失败才 `error`）——这正是 SFINAE 的引擎。缺的是**在源码里表达条件**的能力。

**落地内容（已做）**
- `decltype(expr)`：新增表达式求值入口 `inferDecltype`，复用既有 `inferType`，返回 TypePtr
- SFINAE 触发条件：推导失败即移除候选（复用既有软失败路径），不新增报错
- `template<typename T, typename = void>`：无名形参 + 默认值
- 尾置返回类型 `auto f() -> T`：返回类型位置接受 `->` 后的类型
- 最小 `std` 垫片：`void_t` / `false_type` / `true_type` / `declval` / `enable_if_t` / `decay_t`

**理论点**
- `decltype` 的两套规则：`decltype(e)` 与 `decltype((e))` 的差异（[dcl.type.decltype]）
  —— 未加括号的 id-expression 取**声明类型**，加括号取**值类别推导类型**
- SFINAE（[temp.deduct]/8）：**推导失败不是错误**（immediate context 内），
  这是"合一算法失败态"的正面表达 —— 与本项目主线「实参推导≈合一算法」直接呼应
- `void_t` 探测惯例（[temp.deduct]/8 的 CWG 1558 修正）：别名模板实参替换失败**也**是软失败
- 尾置返回类型与 SFINAE 的配合：返回类型参与替换 → 失败即移除候选

**参照**
- `clang/lib/Sema/SemaExprCXX.cpp` → `BuildDecltypeType` / `Sema::ActOnDecltypeExpression`
- `clang/lib/Sema/SemaTemplateDeduction.cpp` → SFINAE 的 `TDK_ImmediateContext` 判定
- `clang/lib/Sema/SemaType.cpp` → 尾置返回类型的 `ActOnFunctionDeclarator` 分支

**任务分解**
1. 词法 + Parser：`decltype` 关键字、`decltype(expr)` 类型节点
2. Sema：`inferDecltype` —— 求表达式类型，区分加括号/不加括号两套规则
3. Parser：尾置返回类型（`->` 分支接入函数声明解析）
4. Parser：无名形参 + 默认值 `typename = void`
5. std 垫片头文件 + `::value` 静态成员访问
6. 测试 `tests/decl/test_decltype_NN.cpp` + `tests/tmpl/test_tmpl_33_sfinae_*.cpp`
7. 文档 `docs/learn/20-decltype-and-sfinae.md`

**验收**：`is_range<decltype(numbers)>::value == 1` 且 `is_range<decltype(1)>::value == 0`；
SFINAE 探测不命中时**静默回退**而非报错；与 clang oracle 对拍一致。

**依赖**：模板管线（✅ 已完成）→ 本线 → 主线 D（常量折叠）可复用求值框架。

### 实际完成 / 未做对照

| 规划项 | 状态 | 说明 |
|---|---|---|
| `decltype(expr)` 求值 | ✅ | `SemanticAnalyzer::evaluateDecltype`；两套规则齐备 |
| `decltype` 依赖上下文延迟求值 | ✅ | `TypeKind::Decltype` 节点 + `substituteType` Case 1.5 两段式 |
| SFINAE 软失败 | ✅ | 新增 `SubstitutionFailure` 异常类型，与真错误显式区分 |
| `void_t` 探测惯例 | ✅ | `resolveType` + `TemplateDeducer::reducePattern` 按名识别归约 |
| `declval` | ✅ | `inferCall` 内建分支，按名识别给出 `T&&` |
| `false_type` / `true_type` | ✅ | `registerBuiltins` 内建注入（含 `value` 静态常量） |
| 无名形参 `typename = void` | ✅ | 合成名 `$unnamedN` |
| **偏序裁决**（原属 learn/19 遗留） | ✅ | **超额完成**：本线顺带把 `[temp.class.order]` 补全，见 learn/21 |
| 引用结构匹配修正 | ✅ | 顺带修 bug：此前 `Probe<int>` 会错误匹配 `Probe<T&>` |
| 尾置返回类型 `auto f() -> T` | ⚠️ 部分 | 解析可用，但未系统验证；见 `test_tmpl_12_func_forms` |
| `enable_if_t` / `decay_t` | ❌ 未做 | 别名模板缺口（Parser 不支持 `using X = Y;`），故 `operator|` 那半边仍不通 |
| 函数默认实参 | ❌ 未做 | Parser 不支持 `void f(int x = 0)`，enable_if 惯用法暂时写不出 |
| 函数形参里的 decltype 依赖表达式 | ❌ 未做 | `f(T t, decltype(t.begin())* g)` 推导期不会把 `t` 绑定到实参类型 |

**验收对拍**：`is_range<decltype(numbers)>::value == 1` 且 `is_range<decltype(1)>::value == 0` ✅
（`tests/tmpl/test_tmpl_39_is_range_demo.cpp`，退出码与 clang 一致）。

---

## 主线 I：工程化演进——Parser 消歧机制 + 日志/回滚收口（生产方式渐进）

> 定位：**不改语言特性，改机制质量**。上述主线都在"加功能"，本条主线回答的问题是
> "同样的功能，生产编译器是怎么写的"——逐条把试探/回滚/日志这些**机制层偷懒**
> 换成 clang 同款的工程惯例。每一项都已在 BUGS.md 登记（附 clang 对照与正确写法），
> 此处排期与验收。

**背景**：[stmt.ambig] 两轮修复（B16/B21/655369a）确立了"语法期推迟 + Sema 裁决"的
正确分层，但落地机制有两处欠账：消歧判据纯靠"紧跟 token"启发式（**B31**），
试探回滚只救 `m_pos` 一个维度、异常当控制流、日志散点直写 cout（**B32**）。

| # | 任务 | 对应 | 修法要点（生产惯例） | 验收 |
|---|---|---|---|---|
| 1 ✅ | 模板-id 消歧引入只读模板名登记表 | [B31](BUGS.md#b31-模板-id-试探把-abc-误判为模板调用拒收合法程序) | Parser 构造时前向扫 token 流建"名字→是否模板"只读登记表（类/别名/函数模板、推导指引、成员模板、模板模板形参 + Sema 内建 declval 种子），消歧第一层闸 = 查表；查不到不试探直接按比较解析；查询面只到名字种类，不做方法 lookup，不打破 Sema 分层 | `a<b>(c)` 全变量比较链 rc=0 且与 clang 对拍（tests/lang/test_basics_20）；既有 150 份基线零漂移；`>` 不入 `isExprContinuation`（docs/learn/46） |
| 2 ✅ | 试探-回滚升级为 RAII 存档点 + 日志收口 | [B32](BUGS.md#b32-模板-id-试探回滚只撤-m_pos不撤日志幽灵-parsetarg-行) | ① `TokenCheckpoint`（构造快照全维度/析构回卷/`commit()` 确认），取代 try/catch 与裸 `m_pos = saved`——新状态维度进快照是编译期强制；② 全部 `[parse:targ]`/`[parse]` 打点收进 `Parser::logParse(...)` 单一出口，配 `m_quietDepth` 静默开关（试探期日志缓冲/抑制） | `a < Box > c;` 日志无幽灵行；基线零漂移；`parseIntLiteral` 刻意不打日志约定不破 |
| 3 ✅ | 异常退出试探路径（顺带回卷语义修对） | B31 附带 | `a < b > c` 是常见形态不是异常情况；catch `std::exception` 全集会吞掉试探内部的真 bug 并静默降级——换 TokenCheckpoint 后异常只用于"真错误"，且析构保证回卷 | 故意在试探内制造 logic_error ⇒ 响亮报错而非静默当比较 |
| 4 ✅ | 个人阅读注释出源码 → **全项目注释对齐「四段式」标准** | 代码审查（非 bug） | ① 13 处 `// mrd` 按先例提炼进 `docs/NOTES-阅读笔记.md`（69 处 `// wangyang` 同款处置），源码注释只讲 why 不讲 what；② 体内超预算块 10 块瘦身到 0（纯机制内容搬进对应 `docs/learn/NN`，源码只留指针）；③ 76 条 clang 锚点统一成「`clang/lib/Xxx/Xxx.cpp:Symbol::method`」并逐条在本地语料 grep 三分类（✓74 / ⚠2 / ✗0，已修 19 条真错与 6 处路径拼写）；④ 标准变成机器断言：`check-comment-style.sh`（判据①②③④）+ `verify-comment-mutations.sh`（每条判据"能抓到 + 放松后不报"两次实测） | 源码 grep 无 `// mrd`；NOTES 新增 §七；体内超预算块 = 0；守卫脚本 0 违规；突变验证 8/8；ctest **376/376** + logdiff 零漂移（160 集成用例逐字节一致）；docs/learn/50 |
| 5 ✅ | **Stage 1**：可见性裁决下沉 Sema（红线 1 改写：语法期零语义分层 → 语义裁决权单一） | [B37](BUGS.md#b37-裸类型名声明的可见性无人裁决非法程序静默通过判据答错了问题) | 裸名 `S;` 的"可见吗"改由 Sema 的 `lookupVisibleType` 回答（类作用域 → 命名空间链逐级外层 → 全局，判据单点）；Parser 只给候选、名字带进 AST（`EmptyStmt::typeName`）；登记表降级为候选过滤器 | `namespace N{struct S{}}` 全局裸 `S;` ⇒ rc=1 `use of undeclared identifier 'S'`（与 clang 逐字，含列号）；tests/lang/test_basics_25..27 + tests/unit/test_name_visibility.cpp（7 例）；docs/learn/48 |
| 6 ✅ | **Stage 2**：作用域即身份（模板/命名空间成员的名字带上作用域）+ 登记表退场一处 | [B38](BUGS.md#b38-作用域不是名字身份的一部分限定名模板绑错同名模板撞实例嵌套命名空间丢外层) | 前缀化单点改用 `m_currentNamespace`（完整路径）；模板按作用域注册（注册键/实例名/缓存键/mangling 全部派生）；限定名不再"剥末段查全局蓝图"，查不到即报错（与 clang 逐字）；Sema 侧 `classifyQualified` 消费点删除 | `N::B<int>` 与 `M::B<int>` 是两个实例；`A::B::f()` rc=0；`N::B<int>`（N 里没有 B）⇒ rc=1 `no template named 'B' in namespace 'N'`；tests/lang/test_basics_28..29 + tests/unit/test_scope_qualified_names.cpp（5 例，突变验证 4 条）；docs/learn/49 |
| 7 ✅ | **Stage 3**：边解析边 `ActOn*` 登记（clang 的 Actions 模式）+ 登记表整体退场（含 value 位） | 路线终点 | Parser 通过窄的只读接口向 Sema 提问（`Parser.h:639` 的 `Sema &Actions;` + `Parser.cpp:1696` 的 `Actions.ClassifyName(...)`）；"名字是不是模板"由活符号表回答，语法试探（`isTemplateArgumentList` + `RevertingTentativeParsingAction`）只做未声明名的兜底 | ★ **已由批 1~5 全部落地**（批 1 活表 / 批 2+3 切表与 `NameRegistry` 退场 / 批 4 嵌套类 / 批 5 vtable 句柄）：`a<b>(c)` 由 Sema 判定（变量遮蔽）而非登记表近似；`unknown<x>(y)` 按 C++20 当 template-id（clang：报 undeclared 'x'）；登记表类整体删除 |
| 7-1 ✅ | **Stage 3 批 1**：活作用域表 + Actions 句柄 + 登记点铺满（**行为中立**） | docs/learn/51 | 新模块 `include/name_scope.h`（`ScopeKind`/`ScopeBit` 跨域契约）+ `src/sema/parse_scope.h/.cpp`（活链 + 持久上下文）；Sema 持有表并开六条窄接口；Parser 持 `SemanticAnalyzer*`（前向声明、空句柄空转）并铺 11 类登记点；★ 模板形参作用域对名字登记**透明**；★ 可见性（活链）与限定可达（持久上下文）分两张结构 | build 0 警告；ctest 376 → **388**；**160 份集成日志逐字节零漂移**（消歧点仍读旧表 ⇒ 行为中立）；tests/unit/test_parse_scope.cpp（12 例）；docs/learn/51 + specs/designs |
| 7-2 ✅ | **Stage 3 批 2+3（合并）**：value 位落地 + C++20 `UndeclaredTemplate` 规则 + 三个消歧点切表 + `NameRegistry` 整类删除 | `SemaDecl.cpp:995-1003` + B31/B37 的收口 | ① 三个消歧点（语句层裸名判定 / 语句层模板 id 链闸 / 表达式模板闸）改问活作用域表；② 名字查不到且下一个 token 是 `<` ⇒ 按 C++20 假定它是模板名（复核仍在试探实参表那一步）；③ **value 位不再是占位** —— "是不是值"参与语句层分流（变量遮蔽同名类型 ⇒ `S;` 是表达式，clang 同）；④ `NameRegistry` 三文件（头/实现/单测）整类删除，Parser 自带 Sema（clang `Sema &Actions` 是构造前提） | 登记表类删除；`unknown<int>(y)` ⇒ rc=1 `use of undeclared identifier 'y'`（与 clang 逐字**含列号**）；裸 `q;` ⇒ 同款文案（旧：Undefined variable）；`Box;`（可见的模板名）⇒ rc=0（**修掉拒收合法程序**）；tests/lang/test_basics_30..31 + tests/unit/test_parse_scope.cpp（16 例）；★ **有意漂移 2 处**（basics_23/25：遮蔽终于在 Parser 层可见）；docs/learn/52 |
| 7-3 ✅ | **Stage 3 批 4**：嵌套类（用户拍板：用同一张作用域链的通用查询解决，**不做特判**） | 用户原始问题（`class A{ B; }` 与全局 `B`） | 嵌套类登记为外层类作用域的成员（登记名带作用域 `A::B`）；类作用域从"一张别名表"升级成**一条链**（类自身 → 别名 → 嵌套类 → 外层类）；查找顺序改为 **类作用域 → 命名空间链 → 全局**（由内向外）；构造函数符号改由 Sema 选定并回填（CodeGen 不再硬拼） | `A::B` 与全局 `B` 并存不串；`A::B` 限定名可达；rc=0 与 clang 对拍；tests/lang/test_basics_32..33 + tests/unit/test_nested_class.cpp（7 例，突变验证 5/5）；docs/learn/53 + specs/designs |
| 7-4 ✅ | **Stage 3 批 5**：vtable 槽 → `FuncDeclPtr` 句柄（主线 G 的第一笔债） | B20/B22 的根因（槽里放的是**字符串符号**，规则在多处各写一遍） | 虚表槽存**声明句柄**而非符号串 ⇒ 符号名由唯一的定名函数产出（`registerFunction`）；CodeGen 只发句柄给出的符号（`vtableEntrySymbol`）；三个填槽落点只换句柄、**不算符号**；析构槽改看声明名（不再猜符号子串） | ★ 修掉 **B45**（嵌套类作虚函数重载的形参 ⇒ 槽与定义点两个符号 ⇒ 链接期 undefined reference）与 **B46**（名字含 "dtor" 的普通虚函数被误认成析构 ⇒ 静默少调一次析构）；164 份既有集成日志**逐字节零漂移**；ctest 390 → **396**；tests/lang/test_basics_34 + tests/unit/test_vtable_handle.cpp（6 例，突变验证 5/5）；docs/learn/54 + specs/designs |
| 7-5 ✅ | 实例化步骤 5.8：克隆嵌套类（类模板里的嵌套类） | docs/learn/53 §7 边界 1 | 类模板实例化时把蓝图的嵌套类克隆进实例（字段/方法随形参替换），登记名 `Box_int::Inner` | ✅ **已完成（2026-11-03，§0.1 批 0-2）**：实例化新增步骤 5.8（`cloneNestedClass`：字段/方法/static/别名/成员模板随形参替换，嵌套类递归克隆）；★ 登记名**不在克隆处拼** —— 克隆只交裸名，前缀化仍由 `processClassDecl` 统一做（判据单点）；★ 连带修 Pass 2/3 的扁平遍历 ⇒ 改递归原语 `forEachClassMethod`（否则嵌套类的方法不注册 ⇒ 链接期 undefined reference）；ctest 396 → **403**；167 份集成日志**逐字节零漂移**；tests/lang/test_basics_35..36 + tests/unit/test_nested_class_in_template.cpp（6 例）；docs/learn/56 + BUGS.md B47 |

**原则**：每项独立成批、三绿（build 0 警告 / ctest 全绿 / logdiff 零差异）再进下一项；
批次间互不阻塞，可与特性主线穿插。完成一条就在 BUGS.md 转 ✅ 并记回归用例。

---

## ✅ 主线 J：文档骨架标准（已完成 2026-11-02）

> 定位：**不改语言特性**，把散落的文档按"族 → 技能"归位、补齐骨架，
> 并把规范变成机器断言。口径由用户拍板：**补齐骨架、保留内容资产**（正文不重写）。
> ★ 与主线 I 的关系：主线 I 治**源码注释**，主线 J 治**文档骨架** —— 两套标准不互相套用。

| # | 任务 | 修法要点 | 验收 |
|---|---|---|---|
| 1 ✅ | **冲突裁决先行**（不先裁决就动手 = 返工） | 新建 `constraints/05-文档标准与冲突裁决.md`：三条原则（P1 项目级约束优先 / P2 一个族一个技能 / P3 保留资产）+ 14 条裁决 C1..C14，解决"tech-doc 不拆 spec+design" ⟷ "biz-dev-pipeline 拆两份"、"wiki-postmortem 要 frontmatter" ⟷ "BUGS.md 是单文件"等 | 裁决表逐条可查；后续改动按裁决执行 |
| 2 ✅ | **骨架规范成文** | `constraints/06-文档骨架规范.md`：8 个文档族各自的骨架（技术/开发/学习/复盘/端到端线/架构图）+ 头部三行（读者/审法/名词版本）+ 变更记录 | 规范本身 0 占位符（判据④豁免它） |
| 3 ✅ | **根目录与重名归位** | `2.md` → `docs/00-项目章程.md`（需求原文）；`docs/learn.md` → `docs/upstream/clang-stdlib-and-header-search.md`（**消解与 `docs/learn/` 目录重名**）；`docs/superpowers/specs/` → `docs/specs/legacy/`（归档 + 指针） | 全仓引用改指新路径；重名消失 |
| 4 ✅ | **各族补骨架**（保留正文资产） | 技术文档（business 28 份 + 专题 7 份）/ 开发文档（constraints + specs + designs + 归档类）/ 学习文档（01..50 三段式：定位与能力清单 + 真实版多做了什么 + 演进记录）/ 复盘文档（BUGS 38 条补 severity/status/related + PITFALLS + NOTES） | `check-doc-standard.sh` 0 违规 |
| 5 ✅ | **新增两类产物** | `docs/lines/`（line 技能：一条线 = 一条跑出来的端到端子流程，首批 3 条）；`docs/arch/`（archify：编译流水线 HTML + **可复现的 JSON 源**，图挂了能重生成） | archify validate 9/9（0 error / 0 warning）+ visual-check 四视口不溢出 |
| 6 ✅ | **规范变机器断言** | `check-doc-standard.sh`（7 条判据：①头部 ②骨架节 ③元数据 ④占位符 ⑤资产——⑤ 专门防"重写时丢内容"——⑥引用可达——管"文档说的和代码对不对"，实测一次抓出 38 处不可达引用——⑦契约变更点——管"spec/design 该不该存在"）+ `verify-doc-mutations.sh`（每条判据**两次实测**：引入违规必须红 + 放松判据必须绿） | 突变验证 26/26（判据⑦ 落地后 30/30）；守卫 0 违规 |
| 7 ✅ | **文档线收敛规则**（2026-11-02）：spec/design **仅在接口/契约变更时**才写 | 判据见 `docs/constraints/06-文档骨架规范.md` §2.0；每份 spec/design 必须声明「契约变更点」并被 `check-doc-standard.sh` 判据⑦机器检查（缺 ⇒ 违规） | 规则进约束（唯一权威）；判据⑦进守卫脚本 + 突变验证 |

★ **方法论沉淀**：**先裁决、再立规范、后建断言、最后才动文档** ——
断言建在动手之前，基线（初始违规数）才是可测的；反过来做，
"改好了没有"就变成了不可证伪的自我感觉。
★ **2026-11-02 追加一条**：**"重排"也算内容变更**——ROADMAP 这次按"执行序"重排，
历史内容**一段不删**（判据⑤ 资产），只是顺序与优先级变了。
理由：下个会话读 ROADMAP 是为了知道**下一批做什么**，不是为了考古。

---

## 约定（所有主线通用）

1. 遵循 CLAUDE.md 的交付三件套与改动顺序（README → Ast/Parser → Sema → 新模块 → CodeGen → tests → docs/learn）
2. 每条主线配 docs/learn/NN（编号接续 08 起）与同系列测试，回归红线不破
3. 语义拿 `clang++-18 -emit-llvm -S` / `-Xclang -ast-dump` 当 oracle
4. 动手前按 docs/prompt-template-deduction.md 的格式产出：文件清单 + EBNF + 算法伪代码，确认后实现
5. ★ **文档线收敛（2026-11-02）**：`docs/specs/` 与 `docs/designs/` **仅在接口/契约变更时才写**
   —— 判据与豁免通道见 `docs/constraints/06-文档骨架规范.md` §2.0；
   纯特性实现、纯 bug 修复、纯重构 ⇒ **只出 `docs/learn/NN`**（学习产物），
   不再顺手补两份流程文档。理由：本项目一轮主线动辄产出十几份文档，
   文档一旦与代码不同步就成负债；"接口/契约变了"是唯一值得多写两遍的场合。
6. ★ **每批开做前先读 §0**（当前优先级），批次之间可穿插但**不并行开新战线**；
   §0.3 降级区的条目只在"被依赖"或"用户点单"时才捡。
