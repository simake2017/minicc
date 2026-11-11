# minicc 项目记忆

## 项目定位（最高优先级）
本项目目的是**学习编译器理论**，不是造生产编译器。重点方向：C++ 模板机制
（实参推导≈合一算法、实例化≈结构化替换、两阶段查找、重载决议）。
一切设计决策以"可讲解、可观测"优先于"高性能、功能全"。

## 项目快照（已盘点，勿重复摸底）
- 管线：预处理器 → Lexer → Parser → SemanticAnalyzer → TemplateInstantiation → CodeGen（x86-64 AT&T .s）→ 自研链接器，约 18695 行 C++20，全阶段中文日志
- 已有：基础类型/auto/指针/左右值引用/const、类/public 继承/虚函数+vtable、new、类模板（仅显式实参）、if/while、字符串字面量、GCC 风格 mangling
- 缺口优先级：✅ P0 函数模板实参推导（S1~S6）✅ P0 预处理器（#include/#define/条件编译/pragma once）→ 下一项：**构造/析构** → 自动链接出可执行文件 → P2 for/break/continue、常量折叠、数组/enum/namespace
- 扩展点已定位：parseTemplateDecl 仅类模板分支（src/parser.cpp:264）；TemplateInstantiator 的 substituteType/clone* 引擎可复用于函数模板；符号表单符号需改候选集

## 交付三件套（每个子阶段必须全部产出，缺一不算完成）
1. **代码**：按固定顺序改动——README 语法表 → Ast/Parser → Sema → 新模块 → CodeGen → tests
2. **文档**：`docs/learn/NN-<主题>.md`，必含：理论背景（点名对应 C++ 标准章节如 [temp.deduct]、对应编译原理概念如合一算法）、设计决策、clang 源码对照表（文件:函数 → 本实现位置 → 简化了什么）、可复现实验命令、关键过程 ASCII 图
3. **测试**：`tests/test_tmpl_NN_*.cpp`（延续现有 01..10 编号），文件头注释写：预期行为、考察理论点；错误用例注明期望报错文案。回归红线：test_tmpl_01..10 永远全绿

★ **文档线收敛（2026-11-02）**：`docs/specs/` 与 `docs/designs/` **仅在接口/契约变更时才写**
——判据见 `docs/constraints/06-文档骨架规范.md` §2.0，由 `check-doc-standard.sh` 判据⑦机器检查
（每份 spec/design 必须声明一行「契约变更点」）。
纯特性实现 / 纯 bug 修复 / 纯重构 ⇒ **只出 `docs/learn/NN`**（学习产物），不补流程文档。
（业务规格仍按 biz-dev-pipeline 的 spec → design → 代码走；见 constraints/05 裁决 C1。）

## 开发规范
- 全阶段中文日志，关键算法要有 trace（如推导逐对打印 `P=T A=int ⇒ T := int ✓`）
- 语义 oracle：任何新特性先跑 `clang++-18 -emit-llvm -S` 确认预期，再写实现
- 参考源只移植算法、禁止整段拷贝 clang 代码；参照源码在 ~/cppproject/llvm-project/（即 /root/cppproject/llvm-project/）
- 工具链用本机 clang++-18；Makefile 的 macOS homebrew 路径是坏的要避开/修复
- **下一批做什么**：先读 `docs/ROADMAP.md` §0（当前优先级 = 执行序），别自己另起一条线

## 当前主线任务

★ **待办不在本文件** —— 统一到 `docs/ROADMAP.md` §0（当前优先级 = 执行序）。新会话接手：**先读 §0 再选活**。
★ **本文件只留"已完成"的台账**，且只留**最近 6 批的完整记录**（接手当前脉络要用的就是这几批）；★ **全量 51 批原文在 `docs/工作台账.md`**（每批的根因/修法/踩坑/边界都在那里，需要细节时读它）。
★ 下面的**历史索引表**一行一批，产物列指向 `docs/learn/NN` 与 `docs/BUGS.md` 的 B 编号 —— 按批号查详版，不必翻全文。

### 最近批次（完整记录）

✅ **Stage 3 批 2+3（名字可见性：切活表 + C++20 UndeclaredTemplate 规则 + 登记表整类退场）已完成
（docs/learn/52 + learn/51 + specs/designs）** ——
★ 起点是用户那句「**往 clang 工业级方向靠拢**」——不是"把 bug 修掉"，而是
**换掉整个机制**：minicc 原先的"名字登记表"是**前向扫 token 流**得到的
**跨作用域并集**（`NameRegistry`），它分不出遮蔽、分不出"这个 `<` 是不是模板实参表"。
**改法是照 clang 的 Actions 模式重画**：Parser 不再自建第二份表，改为
**向 Sema 提问**（clang 的 `Parser.h:639` `Sema &Actions;` 构造期前置条件 +
`SemaDecl.cpp:912 Sema::ClassifyName` 活链查询）；判据单点收口到
`SemanticAnalyzer::classifyParseQualified` + `ScopeTable`（活作用域表，
块作用域 push/pop）。
① **C++20 `UndeclaredTemplate` 规则落地**（[temp.names]/3 + P0846R0；
clang `SemaDecl.cpp:996-1003`：`getCPlusPlusVersion() >= CXX20 && SS.isEmpty()
&& NextToken.is(tok::less)` ⇒ `getAssumedTemplateName`）——
★ 关键判据：**问的是"是不是一个值"**，不是"是不是一个类型"：
声明过但是变量 ⇒ 不假定模板（[basic.scope.hiding]，变量可遮蔽模板名）；
**查不到** ⇒ 按 C++20 假定是模板名 ⇒ `unknown<int>(y)` 走 template-id 路径
⇒ 报 clang 逐字相同的 `use of undeclared identifier 'y'`（**含列号 33:18**）。
② **三处消歧点切活表**：语句层裸名 / 语句层模板 id 链闸 / 表达式模板闸；
★ `SS.isEmpty()` 只用在**表达式闸**（限定名不假定），
语句层链闸**保留** C++20 假定 —— 否则 `N::B<int> x;` 会退化成比较链、
丢掉 B38 建立的「no template named 'B' in namespace 'N'」逐字文案。
③ **两处 clang 对齐修复**：`Box;`（可见的模板名单独成句）此前**拒收合法程序**
（模板名不是类型名、却是**可见的名字** ⇒ 补第三问 `isVisibleTemplateName`，
clang 只 warning、rc=0）；裸名未声明诊断 `Undefined variable` ⇒
`use of undeclared identifier`（**仅限无限定名**，限定名保留旧文案）。
④ **`NameRegistry` 整类删除**（src/sema/name_registry.{h,cpp} +
tests/unit/test_name_registry.cpp 11 例 + CMakeLists/build.sh 三处登记）：
它是"第二份作用域事实"，与红线 1（语义裁决权单一）直接冲突。
★ 附带一个**真 bug 被抓出来**（BUGS.md B40）：活表能看见遮蔽 ⇒
`int S; S;` 终于判成**表达式**（clang 同款）—— 而**用例 test_basics_23 的注释
早就写着"遮蔽 ⇒ 是表达式"**，是实现与用例文档互相矛盾、靠对拍才暴露。
（第二次印证"**用例注释也是契约**"。）
★ 内建名字（`std::declval`）**必须由 Sema 构造期播种**进活表——
它在 token 流里根本不出现，漏播 ⇒ SFINAE 两个用例当场红（"查不到"≠"不是"）。
★ Parser 现在**自持一个 Sema**（`m_ownedSema`），主流程由 main.cpp
`parser.setSema(&semaAnalyzer)` 注入真 Sema —— 解析期查询与语义期裁决**同一张表**。
测试 tests/lang/test_basics_30（负例 rc=1，文案+列号与 clang 逐字同）+
basics_31（正例 rc=0）+ 单测 tests/unit/test_parse_scope.cpp（`ParseScopeRetirement.*` 4 例）；
文档 docs/learn/52（新）+ learn/51（§8 重写 + §8.1 给批 4 的三条交接）+
specs/parse-stage-live-scope.spec.md + designs/…design.md（§3.6 三处消歧点新旧判据对照）；
BUGS.md 新增 **B40**（遮蔽看不见）/ **B41**（模板名单独成句被拒收）。
★ **有意漂移 2 处**（basics_23/25：遮蔽终于在 Parser 层可见），基线已重刷。
全量 ctest **382/382** / build 0 警告 / **logdiff 零漂移**（162 集成用例逐字节一致）/
check-doc-standard.sh 六判据全绿。
★ 未做（下一批 = 批 4）：**嵌套类**（ROADMAP 主线 I 行 7-3）——
须用**同一条作用域链查询**回答，不许为"嵌套"开特判；
交接三条见 docs/learn/51 §8.1（不建第二张表 / 不开特判 / 先把源码现场对着 clang 写）。

✅ **Stage 3 批 4（嵌套类）已完成（docs/learn/53 + specs/designs）** ——
★ 这是用户最初那个提问（"class A 里面有个 B，然后外面还有个 class B"）的落地 ——
三阶段走完后（Stage 1 可见性 / Stage 2 作用域即身份 / Stage 3 活作用域链），
嵌套类**成为作用域链的自然结果**，而不是一条特判（红线：不建第二张表 / 不开特判）。
① **Parser**：`parseClassDecl` 成员循环遇 `class`/`struct` ⇒ 递归解析，
收进 `decl->nestedClasses`（名字进的是**当前类作用域**，不建第二张表）。
② **登记名带作用域**（learn/49 的"作用域即身份"）：`processClassDecl` 在
**字段解析之前**把嵌套类前缀化（登记名 = 外层类登记名 + `::` + 裸名 ⇒ "A::B"）
并递归处理 ⇒ 类表键/符号表键/mangling/汇编符号（`A__B`）全部派生。
★ **位置即语义**：必须先于字段解析 —— 嵌套类的名字在本类作用域内可见
（`struct A { struct B{}; B b; };` 的 `B` 指 `A::B`）。
③ **类作用域链**：`lookupVisibleType` 的 ① 分支从"一张别名表"扩成**一条链**
（①0 类自身 → ①a 类内别名 → ①b 嵌套类 → 沿 `ownerClassName` 外溯一级）。
④ **查找顺序改为由内向外**（[basic.lookup.unqual]）：**类作用域 → 命名空间链 → 全局**
—— 原先是全局符号表先命中 ⇒ 嵌套类 `A::B` 与全局 `B` 并存时，
A 的成员函数里的裸名 `B` 被绑到**全局 B** ⇒ **不报错，只是算错**
（症状 `No member 'v' in class 'B'`）。
⑤ **构造函数符号改由 Sema 选定并回填**（clang `BuildCXXConstructExpr` 同构）：
`VarDeclStmt::ctorSymbol` / `NewExpr::ctorSymbol` 两槽，CodeGen 优先用回填值。
★ 顺带修一个**四次同款**的老坑（B42）：`namespace N{struct S{S(){};};} N::S s;`
此前链接期 `undefined reference to 'N__S_N__S'`（定义点与调用点各算一套符号名），
现 rc=0。
⑥ **`new` 收限定名**（`new A::B(6)`，[dcl.type.elab]/2 的 type-id 可以是限定名）。
⑦ **限定名调用按登记名查表**（`B::sm()` 的限定者按可见性解析 ⇒ "A::B::sm"）。
⑧ **类自身的名字在本类作用域内可见**（[basic.scope.class]/2.1）——
`struct A { A* p; };` 此前**拒收合法程序**（自引用指针，B44）。
⑨ **嵌套类看得见外层类的 static 成员**（类链与基类链并列推进 owner）。
⑩ **诊断对齐 clang**：`A::C`（C 不存在）⇒ `no type named 'C' in 'A'`（逐字）。
★ **本批零既有漂移**：既有 **162** 份集成日志逐字节一致（新判据只在有嵌套类
或类作用域命中时才触发）；全量 ctest **389/389**（+7 `NestedClass.*`）；
build 0 警告；check-doc-standard.sh 六判据全绿；突变验证 **5/5** 实跑变红
（脚本 `tools/verify-nested-mutations.sh`）。
★ **教训（值得单独记）**：**"作用域"不是一张表，是一条链** —— 把它当表实现，
每加一层作用域（命名空间 → 类 → 嵌套类）就要重做一遍；把它当链实现，
加一层只是链上多一环。嵌套类这个"新特性"因此**没有写一行特判**。
★ **边界（本批未做，已登记）**：嵌套类在**类模板**里（蓝图不克隆嵌套类 ⇒
`unknown type name 'Inner'`，响亮拒收；修法 = 实例化步骤 5.8，ROADMAP 行 7-5）／
类外定义嵌套类成员（Parser 未支持任何类外成员定义）／嵌套类**按值返回**
（★ 与普通类同款的老缺口：`S x = f();` 的初始化式被丢弃 ⇒ 静默算错，
已记入 ROADMAP 主线 A 的遗留缺口行）。
文档 docs/learn/53（新）+ specs/nested-class.spec.md + designs/…design.md；
BUGS.md 新增 **B42**（构造函数符号硬拼）/ **B43**（嵌套类不支持）/ **B44**（类自身名字不可见）。

✅ **Stage 3 批 5（vtable 槽 → 声明句柄）已完成（docs/learn/54 + specs/designs）** ——
★ 起点是 ROADMAP 行 7-4「主线 G 的第一笔债」：**槽里放的是符号串 ⇒ 规则在多处各写一遍**。
① **槽里改挂声明句柄**：`VTableEntry::method`（`shared_ptr<FunctionDecl>`）——
   `type.h` 只前置声明（依赖单向 ast.h → type.h），取符号的桥接函数
   `vtableEntrySymbol()` 放 **ast.h**（那里 FunctionDecl 才完整）。
② **符号只有一个来源**：唯一定名点仍是 `registerFunction`（写 `decl->mangledName`）；
   三个填槽落点（**新槽 / 主表覆写 / 次表覆写**）只写句柄、**不再算符号**。
③ **覆写 = 换句柄**（基类自己的表不动 ⇒ A 的对象不会调到 B 的实现）。
④ **析构槽看声明名**（`~X`）而不是猜符号子串 ⇒ 修掉 **B46**。
⑤ **CodeGen 四个消费点**（主表 `.quad` / 次表 + thunk / 虚调用命中 / 析构判定）
   + Sema 两个 dump + main.cpp 布局 dump 统一走取符号入口。
★ **修掉 B45（真链接期错配）**：嵌套类作虚函数**重载**的形参类型时，
Pass 1 算的槽符号（未解析的 "Inner"）与 Pass 2 算的定义符号（已解析的
"Outer::Inner"）**不同** ⇒ `.quad Outer_f_1_Inner` 指向没人定义的符号 ⇒
`undefined reference`（clang rc=0）。★ 机理值得单独记：
**"两处共用同一个函数"只治了"写错"，治不了"算两次"** —— 两处调用发生在
不同时刻、看到的状态不同就还是会漂（B10 → B20 → B22 → B45 同一个坑的第四次现形）；
治本只有一条：**只算一次，别处只引用**。
★ 对照 clang：`VTableComponent::getFunctionDecl()` 返回 `CXXMethodDecl *`
（槽里本来就是声明，不是名字）；`isDestructorKind()` 看声明**种类**（同款思路）。
★ **本批零既有漂移**：既有 **164** 份集成日志逐字节一致（新文件 1 份）；
全量 ctest **396/396**（+6 `VTableHandle.*`）；build 0 警告；
check-doc-standard.sh 六判据全绿；突变验证 **5/5** 实跑变红
（脚本 `tools/verify-vtable-handle-mutations.sh`）。
★ **教训（值得单独记）**：**自指式断言测不出这类缺陷** —— 第一版单测把期望值
取自被测的 accessor 自己（"自己验自己"）⇒ 突变 m5 全绿；期望值必须来自
与被测物【无关】的来源（这里取"声明自己的 mangledName"）才抓住。
★ **边界（本批未做）**：槽位身份仍用形参类型链近似签名（B22 的老边界）／
析构按名字前缀而非声明种类（`FunctionDecl` 无"种类"字段）／
Itanium 真 mangling 与 `CK_VCallOffset`（教学版口径不变）。
文档 docs/learn/54（新）+ specs/vtable-slot-handle.spec.md + designs/…design.md；
BUGS.md 新增 **B45**（槽的符号两处各算）/ **B46**（析构槽靠子串判定）。

✅ **ROADMAP 重排 + 文档线收敛规则已完成（docs/ROADMAP.md §0 + docs/learn/55）** ——
**本批零代码改动**（唯一的"代码"是判据⑦与突变验证脚本）⇒ ctest/logdiff 保持全绿零漂移，
**这是应有结果而不是"没做事"**。
★ **病灶**：ROADMAP 原来按**批次先后**排列，第一屏全是 ✅ 的已完成主线，
真正待做的散落在中段，末尾"⏭ 后续计划"那句还已过期（主线 C/D/E 早已做完大半）。
⇒ **改法**：三段式「**§0 当前优先级（执行序）→ §1 下一条主线 → §2 既有主线存档**」，
历史内容**一段不删**（判据⑤ 资产逼着改法落在"挪位置 + 加索引"上）。
§0 含：§0.1 **批 0 缺陷与边界收口**（2 项：按值返回+拷贝省略 = ★静默算错；
实例化步骤 5.8 克隆嵌套类）/ §0.2 **主线 K 模板线六项** / §0.3 **降级区**（语法面 17 条，
不占主线位）/ §0.4 **暂不开新战线**（主线 F 深水区 / auto 推导 / 移动语义 / 并行编译 等 5 条）。
★ **排序判据写死成一条序**：`静默算错 > 静默不匹配 > 响亮拒收但文案不对 > 缺特性`
—— 与 BUGS.md 的 severity 口径一致；**"静默"排最前**是因为静默缺陷没有症状，
不会被任何测试/日志/用户报告抓到。
★ **主线 K 六项的现状全部用 clang++-18 重新探针**（不照抄旧文档）：
K-1 别名模板偏特化 —— ★ **标准下非法**（clang: "partial specialization of alias templates
is not permitted"）⇒ 目标是"**拒得更准**"而不是"补特性"，方向已在 §0.2 单列一行写死；
K-2 偏特化模式的值位（★ **常量值位早就支持**了，旧文档已过期；真正缺的是**待推导名形态**
`Box<int,N>` ⇒ 静默退回主模板 = 静默选错）；K-3 `template<auto V>` 偏特化（同款静默选错）；
K-4 模板模板形参的形参包/默认实参/嵌套（三种 clang 全接受、minicc 全拒）；
K-5 类外定义特化成员；K-6 函数模板显式 NTTP 实参（已响亮拒收但文案自造）。
★ **文档线收敛规则**：`docs/specs/` 与 `docs/designs/` **仅在接口/契约变更时才写**
—— 判据进 `docs/constraints/06-文档骨架规范.md` **§2.0**（新增整节 + 表），
并回写 `docs/constraints/00-全局约束.md` 红线 8 与本文件的交付三件套；
纯特性实现 / 纯 bug 修复 / 纯重构 ⇒ **只出 `docs/learn/NN`**（学习产物）。
★ **规则落成机器断言**：`check-doc-standard.sh` **判据⑦ 契约变更点**
（每份 spec/design 必须声明一行「契约变更点」，写不出来 ⇒ 这份文档不该存在；
★ **没有豁免通道**，否则规则等于给"随手补两份文档"发通行证）；
13 份存量 spec/design 全部补上该行（其中 `parser-stage-name-recognition` 注明
"该契约已于 Stage 3 批 2+3 整类撤销"——**历史文档不必装作契约还在**）；
突变验证新增 4 项（spec/design 各两条：引入违规红 / 放松判据绿）⇒ **30/30**。
★ **判据只挡"没声明"，不挡"声明错了"**（与判据⑥ 同理：它挡路径漂移，挡不住语义漂移）。
守卫脚本 7 条判据全绿 + check-comment-style 全绿 + ctest **396/396** + logdiff 零漂移。
文档 docs/learn/55（新）+ docs/constraints/06 §2.0 + docs/ROADMAP.md §0/§1。

✅ **类模板里的嵌套类已完成（实例化步骤 5.8，BUGS.md B47，docs/learn/56）** ——
★ 起点是 ROADMAP §0.1 批 0-2（learn/53 批 4 留的边界）：`template<class T> struct Box{
struct Inner{T v;}; }; Box<int>::Inner i;` ⇒ clang rc=0 / minicc rc=1
`no type named 'Inner' in 'Box_int'`（**合法程序被拒**）。
★ **根因**：实例化只搬"顶层四样"（字段/方法/别名/static），**没搬嵌套类** ——
而嵌套类按 [class.nest]/1 也是外层类的成员，按 [temp.inst]/5 该随实例化一起克隆。
★ 与 learn/26（命名空间递归漏方法）同款：**DeclContext 树本身就是递归的，扁平遍历必然漏**。
★ **对照 clang 根本没有"克隆嵌套类"这个专门步骤** —— 它只有一个成员循环
（`Sema::InstantiateClass` 的 `for (auto *Member : Pattern->decls())` → `Instantiator.Visit`），
遇 `CXXRecordDecl` 就递归进 `VisitCXXRecordDecl` ⇒ "嵌套类跟着实例化"是成员循环的**自然结果**。
**修法三处**（缺一不可）：① 实例化新增步骤 5.8（`cloneNestedClass`：字段/方法/static/
别名/成员模板随形参替换，嵌套类递归克隆，产物挂进实例类 `nestedClasses`）；
② **登记名不在克隆处拼** —— 克隆只交裸名，前缀化（`Box_int::Inner`）仍由 `processClassDecl`
统一做（learn/49 的"作用域即身份"；★ 理由与 B45 同源：**"两处共用一个函数"只治了
"写错"，治不了"算两次"** ⇒ 治本只有"只算一次，别处只引用"）；
③ **连带修 Pass 2/3 的扁平遍历** ⇒ 改递归原语 `forEachClassMethod`
（否则嵌套类的方法不在外层 `methods` 表里 ⇒ 不注册、不分析 ⇒ 链接期 undefined reference）。
★ 两条可复用判据：**"成员表"是递归结构，遍历它就必须递归**；**"只算一次，别处只引用"**。
★ 边界不放大：蓝图里没有的成员名仍响亮拒收（`Box<int>::Missing` ⇒ rc=1，
文案差在拼写：minicc 报实例名 `Box_int`、clang 报模板 id `Box<int>` —— 查的是实例的成员表）。
测试 tests/lang/test_basics_35..36（正例 rc=0 与 clang 对拍 + 负例 rc=1）
+ 单测 tests/unit/test_nested_class_in_template.cpp（`NestedClassInTemplate.*` 6 例，
断言**性质**而非拼写：被克隆/字段替换/两实例互不干扰/方法注册/嵌套套嵌套/边界不放大）。
全量 ctest 396 → **403**；**167 份集成日志逐字节零漂移**；build 0 警告。
文档 docs/learn/56（新）+ BUGS.md B47 + ROADMAP §0.1 批 0-2 标 ✅。

✅ **按值返回 + 拷贝省略已完成（BUGS.md B48/B49，docs/learn/57）** ——
★ 起点是 ROADMAP §0.1 批 0-1（主线 A 的延伸缺口）：`struct S{int v;};
S f(){S s; s.v=7; return s;} int main(){S x=f(); return x.v;}`
⇒ clang rc=**7** / minicc rc=**0**（**初始化式被丢弃**，对象退化成默认构造 ⇒ ★ 静默算错）。
★ 根因：`visit(VarDeclStmt)` 的平凡拷贝分支只接 `Var`/`Member` 两种**左值**形态
（源码注释里写着"函数返回同类对象不涉及"），调用表达式落进"无初始化的类声明"。
★ 修法走 **sret 约定**（Itanium ABI §3.2.3）：
**定义点** spill 隐藏缓冲指针到 `-8(%rbp)`、`return` 路径把源对象**逐字节拷进缓冲**
（`rep movsb`）并按约定返回缓冲地址；**调用点**把缓冲地址装进 `%rdi`、**显式实参右移**到
`%rsi` 起（成员调用 `this` 让到 `%rsi`、实参从 `%rdx` 起；虚调用同款）；
**拷贝省略**（[dcl.init]/17.6.1 + [class.copy.elision]/8）⇒ 缓冲**就是变量自己的槽**，
`S x = f();` 一步落位、无中间临时。
★ **判据单点**（B10→B20→B22→B45 同一个坑的**第五次**现形）：
"返回类型是不是类" 由 `Type::isClass()` 在**同一个 Type 对象**上判三次
（Sema 标 `initIsSretCall` / codegen 调用点 / codegen 定义点）⇒ 不会"两处算两次"。
★ **两个自踩的坑**（都写进 learn/57）：① **`%rbp` 偏移恒为负** ⇒
"槽 >= 0"这个 sentinel 判据**恒为假** ⇒ 拷贝省略的缓冲落到了临时槽、
变量值静默为 0 ⇒ 改用 `kNoSretTarget = INT_MIN` 显式哨兵；
② `std::format("movq ({}), %rax", reg)` 少一个 `%` ⇒ 汇编吐 `movq (rdi), %rax`
⇒ 链接期 `undefined reference to 'rdi'`。
★ **顺带修一个与本批无关的老坑（B48）**：成员赋值 `s.v = s.v + 4;` 把对象地址
留在 `%rcx` 越过右值求值 ⇒ 右值求值踩掉 `%rcx` ⇒ **写到地址 4** ⇒ SIGSEGV
（clang 给 7）⇒ 加 `pushq %rcx`/`popq %rcx` 保护（**无条件保护**，
常量右值也保护 —— 单测第三例钉住这点）。
★ 边界**不放大**：引用返回不是 sret（单测 T6 钉住）；全局类对象仍是坏的
（8 字节存储 + 地址约定未套用 ⇒ `g.v = 11` 直接 SIGSEGV）⇒ **登记 B50 为 open**，
下一批收口（两个根因要一起改：存储尺寸 + 全局取地址）。
测试 tests/lang/test_basics_37_return_class_by_value.cpp（9 检查点，与 clang 对拍 rc=0）
+ 单测 tests/unit/test_sret_return.cpp（`SretReturn.*` 7 例：隐藏指针 spill /
逐字节拷贝 / 调用点 rdi 装缓冲 / 拷贝省略=变量槽 / 实参右移 / 成员 this 让位 /
引用返回**不是** sret）+ 单测 tests/unit/test_member_assign_clobber.cpp
（`MemberAssignClobber.*` 3 例）；
突变验证 6/6 实跑变红（tools/verify-sret-mutations.sh）。
全量 ctest 403 → **413**；集成新增 1 份、**有意漂移 43 份**（B48 的 push/pop 两行 +
连带的 `.text` 尺寸/`main @` 地址位移；★ **rc 与运行输出逐字节不变** —— 漂移全是汇编行，
语义日志一行未动，已重刷基线）；build 0 警告。
文档 docs/learn/57（新）+ BUGS.md B48/B49（+ B50 登记 open）+ ROADMAP §0.1 批 0-1 标 ✅。

### 历史批次索引（51 批 → 产物指针）

| # | 批次 | 产物 |
|---|---|---|
| 1 | 函数模板实参推导 S1~S6 已完成 | learn/01..06 |
| 2 | 预处理器 P0 已完成 | learn/07 |
| 3 | 主线 A 构造/析构与顶层声明已完成 | learn/08 ｜ tests×1 |
| 4 | 主线 B 自研链接器已完成 | learn/15 |
| 5 | 非类型模板参数 NTTP 已完成 | learn/18 |
| 6 | 类模板特化已完成 | learn/19 ｜ tests×1 |
| 7 | 主线 H decltype / SFINAE / 偏序裁决已完成 | learn/20 ｜ tests×1 |
| 8 | 「名字在哪一层当真」一批修复已完成 | learn/22 |
| 9 | ⑨ cv 限定符位置修正已完成 | learn/23 ｜ tests×1 |
| 10 | 类内类型别名已完成 | learn/24 ｜ tests×2 |
| 11 | 依赖类型名 `typename T::type` 已完成 | learn/25 ｜ tests×1 |
| 12 | ADL + 限定名查找已完成 | learn/26 ｜ tests×1 |
| 13 | 别名模板已完成 | learn/27 ｜ tests×1 |
| 14 | CTAD + 推导指引已完成 | learn/28 ｜ tests×1 |
| 15 | AST 分派重构已完成（五批次，全项目零 RTTI） | learn/29 ｜ tests×1 |
| 16 | 语言基础补齐 + 模板三项收尾已完成 | learn/31..34 |
| 17 | 带参成员方法符号回填（bug 修复，docs/BUGS.md B10） | B10 ｜ tests×2 |
| 18 | 多继承「成员住在哪个子对象里」一批修复已完成（BUGS.md B11~B15） | B11..B15 ｜ tests×1 |
| 19 | 模板模板实参的逐位签名匹配已完成 | learn/33 ｜ tests×1 |
| 20 | NTTP 类型域扩展已完成 | learn/35 ｜ B17 ｜ tests×1 |
| 21 | 类模板里的成员模板（bug 修复，docs/BUGS.md B18） | learn/34 ｜ B18 ｜ tests×1 |
| 22 | static/virtual 的识别位置（bug 修复，docs/BUGS.md B19） | learn/34 ｜ B19 |
| 23 | vtable 槽里的符号名与定义点不同源（bug 修复，docs/BUGS.md B20） | learn/17 ｜ B20 ｜ tests×4 |
| 24 | 「身份 = 名字 + 形参类型」四处落点收口（B22 + B20 缺陷 c） | learn/17 ｜ B22/B20 ｜ tests×1 |
| 25 | 实参绑定与名字可见性一批修复已完成（BUGS.md B21 / B24 / B25 / B26 / B27） | learn/36 ｜ B21/B24/B25/B26/B27 ｜ tests×1 |
| 26 | ministl::map（红黑树）已完成（STL 覆盖第 1 轮） | — |
| 27 | 内建静态宏 `__MINICC__` 已完成（飞轮第一块砖，docs/learn/37） | learn/37 ｜ tests×1 |
| 28 | 主线 C 首批（for / break / continue / 三元 ?:）已完成（docs/learn/38） | learn/38 |
| 29 | ministl::set + 红黑树分层抽取已完成（STL 覆盖第 2 轮） | tests×1 |
| 30 | 主线 D 首批（常量折叠 + NTTP 常量表达式实参）已完成（docs/learn/39） | learn/39 ｜ tests×1 |
| 31 | 主线 D 二批（constexpr + 常量传播 + 常量变量作 NTTP 实参）已完成（docs/learn/40） | learn/40 |
| 32 | 主线 C 二批（switch / case / default / do-while / 逗号表达式）已完成（docs/learn/41） | learn/41 |
| 33 | 主线 E 首批（原生数组与下标寻址）已完成（docs/learn/42） | learn/42 ｜ tests×1 |
| 34 | static 数据成员已完成（[class.static.data]，BUGS.md B33，docs/learn/44） | learn/44 ｜ B33 |
| 35 | 主线 I 第 2、3 项已完成（B32，docs/learn/45） | learn/45 ｜ B32 |
| 36 | 主线 I 第 1 项已完成（B31，docs/learn/46） | learn/46 ｜ B31 |
| 37 | const 类对象端到端修复已完成（BUGS.md B34） | B34 |
| 38 | 语句层 [stmt.ambig] 前瞻重写已完成（BUGS.md B35） | B35 |
| 39 | 两处剩余手写试探收口已完成（B36 批次，承接 B35） | B36/B35 |
| 40 | parser 阶段名字识别已完成（biz-dev-pipeline 流水线首例：spec → 设计 → 代码；docs/specs + docs/designs 为流程产物，learn/47 为学习产物） | learn/47 |
| 41 | 名字可见性裁决下沉 Sema（B37，docs/learn/48 + specs/designs 首例） | learn/48 ｜ B37 ｜ tests×1 |
| 42 | 作用域即身份（B38，Stage 2，docs/learn/49 + specs/designs） | learn/49 ｜ B38 ｜ tests×1 |
| 43 | 解析期活作用域表已落地（B39 + Stage 3 批 1，docs/learn/51 + specs/designs） | learn/51 ｜ B39 ｜ tests×1 |
| 44 | 源码注释对齐「四段式」标准已完成（docs/learn/50 + specs/comment-standard-conformance.spec.md） | learn/50 |
| 45 | 文档骨架标准一批已完成（主线 J，docs/constraints/05 + 06） | — |
| 46 | Stage 3 批 2+3（名字可见性：切活表 + C++20 UndeclaredTemplate 规则 + 登记表整类退场）已完成 （docs/learn/52 + learn/51 + specs/designs） | learn/52,51 ｜ B40 ｜ tests×2 |
| 47 | Stage 3 批 4（嵌套类）已完成（docs/learn/53 + specs/designs） | learn/53 |
| 48 | Stage 3 批 5（vtable 槽 → 声明句柄）已完成（docs/learn/54 + specs/designs） | learn/54 |
| 49 | ROADMAP 重排 + 文档线收敛规则已完成（docs/ROADMAP.md §0 + docs/learn/55） | learn/55 |
| 50 | 类模板里的嵌套类已完成（实例化步骤 5.8，BUGS.md B47，docs/learn/56） | learn/56 ｜ B47 ｜ tests×1 |
| 51 | 按值返回 + 拷贝省略已完成（BUGS.md B48/B49，docs/learn/57） | learn/57 ｜ B48/B49 ｜ tests×3 |

### 交接状态

**未做（按优先级）**：★ **本文件不维护待办清单** —— 统一到 `docs/ROADMAP.md` §0（当前优先级 = 执行序）。
要点：§0.1 批 0（~~按值返回+拷贝省略~~ ✅ learn/57；~~实例化 5.8 克隆嵌套类~~ ✅ learn/56；
**批 0-3 = 全局类对象（B50）** = ★ 段错误，两根因：存储尺寸 + 全局取地址）→
§0.2 主线 K 模板线六项（别名模板特化的**非法程序拒收** / 偏特化模式的值位 /
`template<auto V>` 偏特化 / 模板模板形参的形参包·默认实参·嵌套 /
类外定义特化成员 / 函数模板显式 NTTP 实参）→ §0.3 降级区（语法面 17 条）→
§0.4 暂不开新战线。
**名字可见性三阶段（红线 1 改写后的路线，B37 起）**：Stage 1 = 可见性裁决下沉 Sema
✅（B37/learn/48）；Stage 2 = 作用域即身份 + 登记表退场一处 ✅（B38/learn/49）；
Stage 3 = **批 1** 活表就位（行为不变）+ **批 2+3**（✅，见上条：value 位落地 /
C++20 `UndeclaredTemplate` 规则 / 三处消歧点切活表 / `NameRegistry` 整类退场）
+ **批 4 嵌套类** ✅（见上条：类作用域链 + 登记名带作用域 + 查找顺序由内向外 +
构造函数符号回填；docs/learn/53）+ **批 5 虚表槽句柄化** ✅（见上条：槽挂声明 +
符号唯一定名点；docs/learn/54）；
★ 三阶段走完后，Parser 侧**不再有任何自建的名字/作用域事实**，
剩下的是"作用域链能表达的东西"（类作用域成员 / using 声明 / 嵌套类已在批 4 完成）；
**B37 查实、到 Stage 3 批 5 仍未修的一处缺口**：命名空间变量在块作用域不可见
（`namespace N { int g; }` 裸用 g 不可见）。
⏭ 后续计划见 **docs/ROADMAP.md §0**（★ 2026-11-02 起改为"第一屏给执行序"的三段式：
§0 当前优先级 → §1 下一条主线 → §2 既有主线存档）。新会话接手：**先读 §0 再选活**。
★ 文档**内容侧**欠账（骨架已齐、内容待补）：lines 扩到 10 条 / business「观测点」节
与实际日志键对齐 / learn 的「真实版多做了什么」落到函数级 / arch 再加 2~3 张
—— 见 TODO.md G 组。

**完整提示词与子阶段定义：docs/prompt-template-deduction.md**——新会话接手时先读它。

**完整提示词与子阶段定义：docs/prompt-template-deduction.md**——新会话接手时先读它。
