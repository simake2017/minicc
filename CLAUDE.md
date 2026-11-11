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
★ **本文件只留"已完成"的台账**，且只留**最近 6 批的完整记录**（接手当前脉络要用的就是这几批）；★ **全量 52 批原文在 `docs/工作台账.md`**（每批的根因/修法/踩坑/边界都在那里，需要细节时读它）。
★ 下面的**历史索引表**一行一批，产物列指向 `docs/learn/NN` 与 `docs/BUGS.md` 的 B 编号 —— 按批号查详版，不必翻全文。

### 最近批次（完整记录）

✅ **引用的别名语义已完成（BUGS.md B6，docs/learn/59）** ——
★ 起点是 BUGS.md 里一条**积压已久的高危缺陷**：引用变量/引用形参**没有别名语义**
⇒ 写回不到被绑定对象 ⇒ ★ **静默算错**（两个方向都中招）。
① **病灶**：`int& r = a;` 在 Sema 侧类型是对的，但 CodeGen 把 `r` 当**独立栈槽** ——
初始化时拷的是 `a` 的**值**，`r = 30` 写回 `r` 自己的槽 ⇒ `a` 全程不受影响
（clang rc=30 / minicc rc=5）。**引用形参同款**：调用点 `emitExpr(arg)` 传的也是值
⇒ 被调方的写回到不了调用方（clang rc=6 / minicc rc=5）。
★ **★ 台账原文写着"函数形参……走调用方传地址，天生正确"—— 这句话是错的**，
本批是**实测推翻**的（教训：「文档里写着这条路是对的」不等于它真的对）。
② **判据单点**：`isAliasBinding(type, operandHasAddress)` ——
【有没有被绑定对象的地址】。局部侧传"初始化式可寻址吗"的实情；形参侧恒传 `false`
（实参**可能是右值**，右值没有地址）⇒ 只有**可写左值引用**能保证实参必是左值
⇒ 走别名通道；其余（const T& / T&&）按值传并如实登记为边界 **B58**。
★ 调用点与被调点读**同一个 Type 对象**（新增 `m_funcBySymbol`：mangledName → FunctionDecl，
与 learn/54 的"vtable 槽挂声明句柄"同款思路）⇒ 不会"一边传地址、一边当值用"。
③ **别名表从平表改成作用域链**（`m_refScopes`，层 0 = 函数作用域，随块 push/pop）——
第一版实做时用了平表，自测用例**当场 SIGSEGV**：同名变量在不同块里各有一种形态
（块① `int& s` / 块② `S s`）时，块①的别名盖住了块②的类对象 ⇒ `s.v = 1` 写到别的地址。
★ 这是 learn/53「**作用域不是一张表，是一条链**」的**第二次现形**。
④ **读/写**：引用变量的槽放**地址**；读走间接加载（`movq 槽,%rcx; movq (%rcx),%rax`）、
写走间接存储；类对象的引用有专门分支（类值即地址，不解引用）。
⑤ **clang 对照**：`CGExpr.cpp:3502 EmitLoadOfReferenceLValue` /
`CGExpr.cpp:3620 EmitDeclRefLValue`（引用声明**从不**分配 alloca）/
`CGExpr.cpp:700 EmitReferenceBindingToExpr` / `CGCall.cpp:5235 EmitCallArg`。
测试 tests/unit/test_ref_alias.cpp（`RefAlias.*` 7 例，断言**性质**：
槽里放的是地址 / 读走间接加载 / 写走间接存储 / 形参两侧同口径 / const 形参按值传（边界钉住）/
三种绑定对象 / 作用域链遮蔽）+ 集成 tests/lang/test_basics_38_ref_alias.cpp（15 检查点，与 clang 对拍 rc=0）；
突变验证 **6/6** 实跑变红（脚本 `tools/verify-ref-alias-mutations.sh`）。
全量 ctest 421 → **428**；集成新增 1 份、**有意漂移 8 份**（★ **rc 与运行输出逐字节不变** ——
漂移全是汇编文本 + 段布局/符号地址；语义日志一行未动，已重刷基线）；build 0 警告；
check-doc-standard.sh 六判据全绿 + check-comment-style.sh 全绿。
文档 docs/learn/59（新）+ BUGS.md B6（status → fixed，连带登记 **B58**）
+ ROADMAP §0.1 批 0-5 标 ✅ + README 计数器（428 / 170 / learn 01..59）。
★ **边界**：const / 右值引用形参按值传（B58，实参可能是右值 ⇒ 要按地址传得在调用方物化临时量）／
按引用返回仍未做（与 B50 同根因）。


✅ **成员模板的形参表收口到自由模板那一份（BUGS.md B56，docs/learn/58）** ——
★ 起点是用户那句「**这些位置的解析模板是不是应该跟外面一样，实现一种复用，而不是全都新做**」
—— 评估后确认：**该复用，而且已经漂移出可见偏差**（不是洁癖问题）。
① **病灶**：类成员循环里的成员模板分支**另写了一份**形参表（简化版），
与自由路径那份（`parseTemplateDecl` 内联）**各写一遍** ⇒ 两条路径随批次演进而漂移：
成员侧缺【无名形参 / 模板模板形参 / 默认实参 / 默认实参约束 / **`frame.count` 同步**】。
★ 两条路径不是同时写的，而是**时间差**：成员侧是批 21（learn/34）的一次性快照，
自由侧此后逐批加料 ⇒ 能力差越拉越大。
② **最要命的一条**：`frame.count` 没同步 ⇒ `lookupTemplateParam` 在成员模板的帧里
**一个形参都取不到** ⇒ 方法体里的裸 `T` 建成 **Class 节点**而非 **TemplateParam 节点**。
★ 实测（同一段函数体里）：成员模板**自己**的 `U` 认不出（`base = U (class/tparam)`），
而**外层**类模板的 `T` 认得出（`base = T (template param, scope hit)`）——
因为外层那帧的 `count` 是同步的（自由路径同步了）。
⇒ 不是"成员模板不支持什么"，而是"**成员模板自己的形参作用域根本没生效**"。
③ **后果**：依赖限定名 `typename T::type`（[temp.res]/5）走不进依赖名分支
⇒ 报 `unknown type name 'T::type'`（**合法程序被拒**，clang rc=5）。
★ 而正确性此前是靠**按名兜底**偶然成立的：`substituteType` 的 Case 6
（按名字撞替换表）⇒ 自由模板靠作用域认形参、成员模板靠按名撞表，
**两条不同的依据走到同一个结果** ⇒ 一旦某形态只对一条路成立就分叉成 bug。
④ **修法（判据单点）**：抽 `Parser::parseTemplateParameterList`
（`src/parse/parser.cpp:1243`）为**唯一实现**，自由路径与成员路径都调它。
★ 形参作用域的"建帧 + 开作用域"**留在调用方**（必须活到模板体解析结束）。
★ **为什么不做"给 `parseTemplateDecl` 加 `isMember` 开关"**：那会把两套**模板体分派**
与两套**名字登记规则**塞进一个函数；判据单点说的是"同一个问题的答案只能有一处"，
不是"函数越少越好"——**形参表是同一个问题**（该合），**模板体形态是两个问题**（不该合）。
⑤ **★ 自踩的坑（评估阶段误判，突变验证时推翻）**：我判过一条"成员模板的名字在类作用域里
丢了值位"（以为 `parseFunctionDecl` 登记方法名时模板形参作用域还开着 ⇒ 值位被 closeScope 丢掉）
⇒ 实测把 `declareName` 的值位去掉，**日志一个字节都没变**。
真因：`openScope` 登记的名字落在 `m_stack[nameScopeIndex()]`（**最近的外层非模板形参作用域**
= 类作用域，模板形参作用域被跳过）⇒ 值位一直都在。
★ 教训：**"名字在哪一层登记"要看登记的 API** ——
`declare`（最内层）与 `openScope`（最近的外层非模板形参作用域）是两套落点规则。
⑥ **边界不放大**：推导侧两处缺口（模板模板实参 `C<T>` 推不出 + 未推导形参不用默认实参顶上）
**自由路径同款**（实测自由路径同样报错）⇒ **登记 B57 为 open**，本批只收口解析侧。
测试 tests/tmpl/test_tmpl_78_member_template_params.cpp（rc=22，与 clang 对拍）
+ 单测 tests/unit/test_member_template_params.cpp（`MemberTemplateParams.*` 6 例：
断言**性质** —— 形参节点种类 / 依赖限定名的限定者形态 / 四项能力 / 约束检查）；
突变验证 **5/5** 实跑变红（脚本 `tools/verify-member-template-mutations.sh`；
M5 = 整体退回旧实现 ⇒ 5 例全红；★ 每条突变在集成侧同样被抓）。
全量 ctest 413 → **421**；集成新增 1 份、**有意漂移 4 份**（成员模板相关：
成员侧现在也打形参表那套日志 + 形态从 Class 节点变 TemplateParam 节点 +
`[subst]` 从 Case 6 切到 Case 1；★ **rc、运行输出、汇编三者逐字节不变**），
另 **165 份零漂移**；build 0 警告；check-doc-standard.sh 六判据全绿。
文档 docs/learn/58（新）+ BUGS.md B56（已修）/ B57（open）+ ROADMAP §0.1 批 0-4 标 ✅。
★ 副作用（下一批用得上）：主线 K 的 **K-4**（模板模板形参）现在**只需改自由侧那一处**，
成员侧自动跟随。

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

### 历史批次索引（53 批 → 产物指针）

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
| 52 | 成员模板的形参表收口到自由模板那一份（BUGS.md B56，docs/learn/58） | learn/58 ｜ B56 ｜ tests×2 |
| 53 | 引用的别名语义（BUGS.md B6，docs/learn/59） | learn/59 ｜ B6/B58 ｜ tests×2 |

### 交接状态

**未做（按优先级）**：★ **本文件不维护待办清单** —— 统一到 `docs/ROADMAP.md` §0（当前优先级 = 执行序）。
要点：§0.1 批 0（~~按值返回+拷贝省略~~ ✅ learn/57；~~实例化 5.8 克隆嵌套类~~ ✅ learn/56；
~~成员模板的形参表收口~~ ✅ learn/58；~~引用的别名语义（B6）~~ ✅ learn/59；
**批 0-3 = 全局类对象（B50）** = ★ 段错误，两根因：存储尺寸 + 全局取地址）→
★ 另有 **B57**（函数模板上的模板模板形参推导 + 默认实参填充，**自由与成员同款**）
+ **B58**（const / 右值引用形参按值传，实参可能是右值）→
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
