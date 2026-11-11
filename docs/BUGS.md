---
title: minicc 缺陷台账（BUGS）
date: 2026-11-02
category: compiler
tags: [bug-ledger, postmortem, compiler, minicc, defect-tracking]
type: index
severity: high
status: maintained
related: ["[[docs/learn/13-name-mangling]]", "[[docs/learn/17-multiple-inheritance-layout]]", "[[docs/learn/36-argument-binding-and-name-visibility]]", "[[docs/learn/47-parser-stage-name-recognition]]", "[[docs/learn/49-scope-qualified-names]]", "[[docs/constraints/05-文档标准与冲突裁决]]", "[[docs/constraints/06-文档骨架规范]]", "[[docs/PITFALLS]]"]
---

# bug 台账（BUGS）

> 与 [PITFALLS.md](PITFALLS.md) 的分工：
>
> - **本文件 = bug 台账**，每条含「复现 → 根因 → 修法 → 影响面 → 修复记录」；
> - **PITFALLS.md = 设计陷阱**（踩坑史/推理复盘），两者主题不同不必互挪。
>
> 每条都附**可复现的最小用例**——没有用例的 bug 条目不算数。
> 修复状态直接看下表的「状态」列；已修条目保留原文（根因有教学价值），
> 末尾追加「**修复**」小节记录改法与回归用例。

## 索引

| 编号 | 一句话 | 位置 | 影响面 | 状态 |
|---|---|---|---|---|
| [B1](#b1-auto-占位符被壳包住) | `const auto` / `auto*` / `auto&` 全编不过 | `src/semantic_analyzer.cpp:2537` | 小（一个分支） | ✅ 已修 |
| [B2](#b2-函数模板-mangling-漏了参数签名) | `_Z5twiceIiE` 少了签名 ⇒ 重载撞名、实例缓存假命中 | `src/template_instantiation.cpp` + `src/semantic_analyzer.cpp:3963` | **大**（全量重刷基线） | ✅ 已修 |
| [B3](#b3-字段写入路径-3-硬编码-movl) | 裸字段名赋值写死 `movl` ⇒ 8B 字段高 32 位被截断 | `src/codegen.cpp:1123` | 小（一处指令） | ✅ 已修 |
| [B4](#b4-带-return-的函数里局部对象析构是死代码) | 析构发射在 `leave; ret` **之后** ⇒ RAII 不执行 | `src/codegen.cpp` emitFunction 尾 | 中（RAII 语义） | ✅ 已修 |
| [B5](#b5-块注释不跨行) | 多行块注释的后续行被当代码分词 | `src/preprocessor.cpp:88` | **大**（25 个外部用例里 15 个） | ✅ 已修 |
| [B6](#b6-引用变量没有别名语义) | `int& r = a; r = 30;` 改不到 `a`（引用形参同款） | `src/codegen/codegen.cpp` 局部槽分配 + 引用形参传递 | 高（静默算错；两个方向都中招） | ✅ 已修 |
| [B7](#b7-自由函数重载根本不做-mangling) | `pick(int)` 与 `pick(S)` 同为裸名 `pick` ⇒ 汇编期符号重复 | `src/semantic_analyzer.cpp:2091` | **大**（全量重刷基线 + main/libc 特例） | ⬜ 未修 |
| [B8](#b8-mangling-没有替换表--符号与-clang-不逐字符等同) | 缺 Itanium 替换表（`S0_`）⇒ `_Z5twiceIiET_S0_` 缩不成 | `src/template_instantiation.cpp` encodeType | 中（只影响与 clang 逐字符等同） | ⬜ 未修 |
| [B9](#b9-struct-的默认继承级别被当成-private-处理) | `struct D : Base` 被拒（默认继承级别看错关键字） | `src/parser.cpp` parseClassDecl | 小（一个分支） | ✅ 已修 |
| [B10](#b10-带参成员方法的调用符号拼不出来) | 带参成员方法调用全线 `undefined reference`（连下标糖一起） | `src/semantic_analyzer.cpp` inferCall + `src/codegen.cpp` | **大**（4 个集成测试长期 rc=1） | ✅ 已修 |
| [B11](#b11-本类自身多态而基类全非多态时-_vptr-与首基类字段压在同一-offset) | 本类有虚函数且无多态基类 ⇒ `_vptr` 与字段都占 0 | `src/semantic_analyzer.cpp` `[relocate]` | **大**（合法程序编译通过、运行 SIGSEGV） | ✅ 已修 |
| [B12](#b12-继承来的成员方法查不到) | `D d; d.g()`（g 在基类）报 `No member 'g'` | `src/semantic_analyzer.cpp` inferMember | 中（合法程序被拒） | ✅ 已修 |
| [B13](#b13-两个次基类各有一个同名字段时显示名塌陷且第二条偏移算成-0) | 两条记录压成同名 ⇒ 第二条偏移算成 0；歧义不报错 | 扁平化循环 + `computeClassLayout` | 中（非法程序被静默接受 + 布局打印错） | ✅ 已修 |
| [B14](#b14-派生类同名字段的隐藏方向做反了) | `d.x` 静默指向 `A::x` 而非 `D::x`（隐藏方向反了） | `include/type.h` `findField` | **大**（合法程序静默取错成员） | ✅ 已修 |
| [B15](#b15-祖辈前缀与直接基类撞名时偏移算到错的子对象) | 祖辈带来的前缀被当成"本类直接基类" ⇒ 偏移算错 | 扁平化循环 + `computeClassLayout` | 中（同 B13 的性质，根因更本质） | ✅ 已修 |
| [B16](#b16-无声明符的声明被拒收aint-int-报-parse-error) | 无声明符的声明（`A<int*,int**>;` / `int;`）被拒收 | `src/parser.cpp` `parseStatement` + `isDeclaratorlessDecl` | 小-中（拒收合法程序，且报错点离根因远） | ✅ 已修 |
| [B17](#b17-值位实参要求形态精确相等-拒收合法程序) | 值位实参要求"形态精确相等" ⇒ `Flag<1>` / `A<4L>` / `A<true>` 全被拒 | `src/semantic_analyzer.cpp` checkTemplateArguments ③-b | 中（拒收合法程序；同批还牵出实例键撞车） | ✅ 已修 |
| [B18](#b18-类模板里的成员模板在实例类里丢失) | 类模板里的成员模板没被带进实例类 ⇒ 实例上调成员模板全线失败 | `src/template_instantiation.cpp` instantiateClassTemplate | 中（合法程序不可用；同批牵出 static/virtual 位置） | ✅ 已修 |
| [B19](#b19-static--virtual-的识别位置写反标准写法被拒非法写法被收) | `template<class U> static U f(U)` 被拒、`static template<…>` 反被收 | `src/parser.cpp` 成员模板分支 | 小（两个方向都反了） | ✅ 已修 |
| [B20](#b20-vtable-槽里的符号名与定义点不同源带参虚函数链接失败--次基类假符号) | vtable 槽名与定义点名**两处各拼一遍** ⇒ 带参虚函数链接失败、次基类捏出假符号；槽位身份只比裸名 ⇒ 静默算错（缺陷 c） | `src/semantic_analyzer.cpp` processClassDecl / registerFunction | 中（合法程序链接失败；另有一条静默算错） | ✅ 缺陷 a/b/c 已修 |
| [B21](#b21-限定名-sv-访问成员数据被拒收) | `S::v`（限定名 + 隐式 this 访问成员**数据**）报 `Undefined variable 'S::v'`；`obj.Q::m` 连语法都不认 | `src/semantic_analyzer.cpp` inferVar / inferMember（Parser 把 `A::B` 拼成名字串） | 小（拒收合法程序，多写一个限定者而已） | ✅ 已修 |
| [B22](#b22-同名同个数的成员重载sema-静默取第一个汇编期撞符号) | 成员重载只按"名字 + 个数"选 ⇒ 同名同个数在汇编期撞符号 `C_f_1`；构造函数同病（静默选错） | `src/semantic_analyzer.cpp` 四处落点：符号定名 / 槽位身份 / 调用点选择 / 构造函数选定 | 中（拒收合法程序，报错点落在 as 的符号上） | ✅ 已修 |
| [B23](#b23-浮点字面量缺失--运算按整数发射) | 无浮点字面量（`1.5` 切成 `1` `.` `5`）；`double` 运算发 `idivq` ⇒ 1/2 得 0 | `src/lexer.cpp`（[lex.fcon]）+ `src/codegen.cpp`（零 SSE 指令） | 中（a 响亮失败但文案误导；b **静默算错**） | ⬜ 未修 |
| [B24](#b24-成员方法的名字隐藏派生类一声明同名方法基类整族应被隐藏) | 成员方法**名字隐藏**：`Derived::f()` 声明后，基类 `f(int)` 整族应被隐藏，minicc 仍能查到 | `src/semantic_analyzer.cpp` inferCall 成员查找 BFS + findMethodInHierarchy | 小（接受非法程序） | ✅ 已修 |
| [B25](#b25-成员调用实参个数不匹配无人认领一条静默接受一条只剩链接器) | 实参个数不匹配无人报错：无参方法那条**静默接受**，带参那条只剩 `undefined reference to 'C_f'` | `src/semantic_analyzer.cpp` inferCall 成员查找 BFS | 中（b **静默接受非法程序**；a/c 报错点落在链接器） | ✅ 已修 |
| [B26](#b26-引用绑定判据只写了模板那一半) | 引用绑定判据只写在模板推导里 ⇒ `const T&` 拒右值（**拒收合法程序**）、`int&&` 收左值（**接受非法程序**） | `src/template_deduction.cpp` deducePair + `src/semantic_analyzer.cpp` inferCall（非模板路径**没有**） | 中（一个判据缺一半，两个方向都错） | ✅ 已修 |
| [B27](#b27-无可行候选时静默退回候选集首个) | 候选集非空而无一可行 ⇒ 静默选第一个（`c.f(p)` 调进了 `f(int)`），编译全绿、运行算错 | `src/semantic_analyzer.cpp` pickBestByArgs | 中（**静默算错** + 接受非法程序） | ✅ 已修 |
| [B28](#b28-偏特化匹配误用调用推导的剥壳规则structuralmatch-的另一半) | 偏特化匹配套用调用推导的剥壳规则 ⇒ 具体引用位 `Duo<T,int&>` 配 `int&` 误拒（**拒收合法程序**）、`Same<T,T>` 配 `<int&,int>` **静默选错特化** | `src/template_deduction.cpp` deducePair（引用分支递归 + 裸形参位调整两处漏网） | 中（三种症状各一） | ✅ 已修 |
| [B29](#b29-全局命名空间内的类型别名存了未实例化模板-id--裸名两处都是没做完) | 全局/命名空间别名存了**未实例化模板 id** + 裸名（两处"没做完"）⇒ `IntBox b;` 报 No member / `N::IntBox` 查无此名 | `src/semantic_analyzer.cpp` processTypeAliasDecl | 小（拒收合法程序） | ✅ 已修 |
| [B30](#b30-表达式级构造调用-boxint7-无路可走--平凡拷贝类赋值静默算错) | 表达式级构造调用 `Box<int>(7)` 无路可走 + 平凡拷贝/类赋值静默算错 | `src/semantic_analyzer.cpp` inferCall + `src/codegen.cpp` | 中（一拒收 + 一静默算错 + 链接期错配） | ✅ 已修 |
| [B31](#b31-模板-id-试探把-abc-误判为模板调用拒收合法程序) | 模板-id 试探只看"后跟 `(`/`::`" ⇒ `a<b>(c)` 全变量比较链被**拒收合法程序**（clang 按 `(a<b)>(c)` 接受） | `src/parser.cpp` parsePrimaryExpr 歧义消解三步曲 | 中（罕见写法，但方向性分歧） | ✅ 已修 |
| [B32](#b32-模板-id-试探回滚只撤-m_pos不撤日志幽灵-parsetarg-行) | 试探失败回滚只撤 `m_pos`，`[parse:targ]` 日志已打印撤不回 ⇒ 幽灵日志行（"日志即契约"基线被污染） | `src/parser.cpp` parsePrimaryExpr + parseTemplateArgumentList | 低（纯日志，不影响正确性） | ✅ 已修 |
| [B34](#b34-const-类对象端到端缺失成员访问误报--掉标量路径-sigsegv--拷贝静默退化) | const 类对象三层判据不看穿 Const 包壳：成员访问误报（拒收）/ 掉标量路径读成员当指针解引用（SIGSEGV）/ 拷贝初始化静默退化成默认构造（算错） | `src/semantic_analyzer.cpp` inferMember + 拷贝/构造升级；`src/codegen.cpp` 类表查询 7 处 | 高（miscompile + 静默算错 + 拒收，一层包壳三层错） | ✅ 已修 |
| [B35](#b35-语句层前瞻只认固定-token-顺序限定名模板-id-与全变量比较链双双被拒收) | parseStatement 2245 前瞻是 parseType 语法的【残缺复制品】：`<` 跳过只认第一个标识符后的位置 ⇒ `N::B<Box> x;`（限定名+模板 id）漏跳被拒收；无任何"是不是模板名"的判据 ⇒ `a < b > c;` 全变量比较链被误判成声明 `a<b> c`（B31 的语句层兄弟） | `src/parser.cpp` parseStatement 类名前瞻分支（2245-2323 手写 token 跳过人） | 中（罕见写法 ×2，但都是拒收合法程序 + 语法复制品是维护隐患） | ✅ 已修 |
| [B36](#b36-限定模板名-nb-误绑全局模板-bsema-尾剥无作用域裁决) | Sema 限定名归一只取 `::` 末段查【全局】蓝图表：N::B 真存在时正确，真不存在而全局有同名模板 B 时误绑（方向性错） | `src/semantic_analyzer.cpp` resolveType 模板 id 分支（B35 尾剥） | 中（静默走错路，报错指向派生类型） | ✅ 已修 |
| [B33](#b33-static-数据成员整体缺失parser-硬性拒收--无存储模型) | `static` 数据成员整体缺失：Parser 硬性拒收 + 无存储模型 | `src/parse/parser.cpp` 类体成员分支 | 中（合法程序被拒） | ✅ 已修 |
| [B37](#b37-裸类型名声明的可见性无人裁决非法程序静默通过判据答错了问题) | 裸类型名声明的可见性无人裁决：非法程序静默通过（判据答错了问题） | `src/sema/semantic_analyzer.cpp` 可见性裁决 | 中（静默接受非法程序） | ✅ 已修 |
| [B38](#b38-作用域不是名字身份的一部分限定名模板绑错同名模板撞实例嵌套命名空间丢外层) | 作用域不是名字身份的一部分：限定名模板绑错、同名模板撞实例、嵌套命名空间丢外层 | `src/sema/semantic_analyzer.cpp` 登记名带作用域 | 高（静默走错路） | ✅ 已修 |
| [B39](#b39-成员调用点没把符号名过净化命名空间里的类调用其成员方法链接期炸) | 成员调用点没把符号名过净化：命名空间里的类，调用其成员方法链接期炸 | `src/codegen/codegen.cpp` 成员调用点 | 中（链接期 undefined reference） | ✅ 已修 |
| [B40](#b40-变量遮蔽在-parser-层看不见同名类型被当成可见裸名语句判错) | 变量遮蔽在 Parser 层看不见：同名类型被当成可见，裸名语句判错 | `src/parse/parser.cpp` 名字识别 | 中（判错） | ✅ 已修 |
| [B41](#b41-模板名不是类型名但仍是可见的名字box-被拒收合法程序被拒) | 模板名不是类型名，但仍是可见的名字：`Box;` 被拒收（合法程序被拒） | `src/sema/semantic_analyzer.cpp` 模板名解析 | 中（合法程序被拒） | ✅ 已修 |
| [B42](#b42-构造函数符号在-codegen-硬拼限定名类一撞就错配链接期才炸) | 构造函数符号在 CodeGen 硬拼：限定名类一撞就错配（链接期才炸） | `src/codegen/codegen.cpp` + `src/sema/semantic_analyzer.cpp` | 高（链接期 undefined reference） | ✅ 已修 |
| [B43](#b43-嵌套类完全不支持合法程序被拒) | 嵌套类完全不支持（合法程序被拒） | `src/parse/parser.cpp` + `src/sema/semantic_analyzer.cpp` | 中（合法程序被拒） | ✅ 已修 |
| [B44](#b44-类自身的名字在本类作用域内不可见自引用指针被拒) | 类自身的名字在本类作用域内不可见（自引用指针被拒） | `src/sema/semantic_analyzer.cpp` 类作用域链 | 中（合法程序被拒） | ✅ 已修 |
| [B45](#b45-vtable-槽的符号在两处各算一遍同一方法两个符号链接期错配) | vtable 槽的符号在两处各算一遍：同一方法两个符号（链接期错配） | `include/ast.h` + `src/codegen/codegen.cpp` | 高（链接期 undefined reference） | ✅ 已修 |
| [B46](#b46-析构槽靠符号子串判定my_dtor_helper-被误认成析构静默少调一次) | 析构槽靠符号子串判定：`my_dtor_helper` 被误认成析构（静默少调一次） | `src/codegen/codegen.cpp` 析构判定 | 中（静默少调一次析构） | ✅ 已修 |
| [B47](#b47-类模板里的嵌套类不支持蓝图不克隆成员合法程序被拒) | 类模板实例化只搬"顶层四样"（字段/方法/别名/static），**嵌套类没克隆** ⇒ `Box<int>::Inner` 报 no type named 'Inner'（合法程序被拒） | `src/sema/template_instantiation.cpp` 实例化步骤 5.8 + `src/sema/semantic_analyzer.cpp` forEachClassMethod | 中（合法程序被拒；连带链接期 undefined reference） | ✅ 已修 |
| [B48](#b48-成员赋值右值求值踩掉对象地址写字段写到了错的地址静默算错) | 成员赋值把对象地址暂存 `%rcx`，**右值求值会踩 %rcx** ⇒ `movl %eax,0(%rcx)` 写到错的地址（`s.v = s.v + 4` ⇒ SIGSEGV） | `src/codegen/codegen.cpp` `visit(AssignStmt)` 成员分支 | 中（段错误；rc 恰是合法地址时静默踩内存） | ✅ 已修 |
| [B49](#b49-按值返回类对象初始化式被丢弃对象退化成默认构造静默算错) | `S x = f();` 的初始化式被丢弃 ⇒ 对象按默认构造生成 ⇒ `x.v` 恒 0（clang 7 / minicc 0） | `src/sema/semantic_analyzer.cpp` 变量声明处理 + `src/codegen/codegen.cpp` sret 形态 | **大**（类对象按值返回全族；静默算错） | ✅ 已修 |
| [B50](#b50-全局类对象存储只按-8-字节分配--类值的地址约定未在全局路径生效段错误未修) | 全局类对象按 `.quad 0` 只分 8 字节 + 全局路径按标量加载（拿到字段值而非地址）⇒ `g.v = 11` SIGSEGV | `src/codegen/codegen.cpp` 全局变量发射 + `visit(VarExpr)` | **大**（`g.v = 11` 这种基本写法就崩） | ⬜ 未修 |
| [B51](#b51-模板id成员-语句被无声明符声明分支整个吞掉-静默接受非法程序未修) | `模板id::成员;` 只查"有模板 id 语法"不查链尾名字种类 ⇒ 静态成员语句被 EmptyStmt 吞、`Box<int>::typo;` **静默接受**（clang 报 no member named） | `src/parse/parser.cpp` B16 分支（~2427） | 中（静默接受非法程序；表达式被吞） | ⬜ 未修 |
| [B52](#b52-macos-本机端到端验证被堵apple-as-拒收汇编注释里的全角标点未修) | Apple as（clang-1700）拒收汇编注释的全角标点 ⇒ macOS 端到端必败；ctest 全单元测试不经过 as 所以一直绿 | `src/driver/main.cpp` 汇编步骤 | 低（只影响 macOS 本机验证） | ⬜ 未修 |
| [B53](#b53-非静态数据成员的类内初始化式nsdmi未实现int-x--5-类内被拒未修) | NSDMI（`int x = 5;` 类内成员默认值，[class.mem]）未实现 ⇒ 拒收合法程序；clang Parser.h 自身 19 处使用、llvm 千行级 | `src/parse/parser.cpp` 类体字段分支 | 中（拒收合法程序；修法三件套机制现成） | ⬜ 未修 |
| [B54](#b54-全局限定名-x-不支持-parser-拒收--空段也不落全局层) | 全局限定名 `::X` 不支持：Parser 直接拒收 `::`（"Unexpected token '::'"）；Scope 表的空段分支也只是**跳过**、不把路径落到全局层 | `src/parse/parser.cpp` 表达式/类型位置 + `src/sema/parse_scope.cpp:167` | 低（语法面欠账；clang 支持，minicc 拒收） | ⬜ 未修 |
| [B55](#b55-按-path-认层时同-path-的多层只查最内层潜伏边界) | `lookupInPath` 按 path 认层 ⇒ 同 path 的多层（函数作用域 + 它的块作用域）只查**最内层**那层，外层那份被跳过 | `src/sema/parse_scope.cpp:197` | 低（潜伏边界，今天不可达） | ⬜ 未修 |
| [B56](#b56-成员模板的形参表另写一份自己的形参在方法体里认不出依赖限定名直接被拒) | 成员侧另写了一份简化形参表 ⇒ 帧计数没同步 ⇒ 成员模板**自己**的 `T` 建成 Class 节点 ⇒ `typename T::type` 报 `unknown type name` | `src/parse/parser.cpp` 类成员循环的成员模板分支 | 中（合法程序被拒；连带 5 项形参能力缺失） | ✅ 已修 |
| [B57](#b57-函数模板上的模板模板形参推导--默认实参填充未实现自由与成员同款) | 模板模板实参 `C<T>` 推不出来 + 未推导的形参不用默认实参顶上 ⇒ 两条形态的函数模板都推不出候选 | `src/sema/template_deduction.cpp` deducePair / 收尾检查 | 中（合法程序被拒；**自由与成员同款**，非成员侧独有） | ⬜ 未修 |
| [B58](#b58-const-引用形参与右值引用形参按值传不跟踪被绑定对象) | `const int&` / `T&&` 形参的实参**可能是右值**（无地址）⇒ 本实现按值传，被调方看不到被绑定对象在调用期间的变化 | `src/codegen/codegen.cpp` 引用形参传递（判据 `isAliasBinding` 的边界） | 低（极窄：只有调用期间对象被改才有差异） | ⬜ 未修 |

> ★ **索引欠账（历史遗留，已补）**：B33、B37~B46 一度**未补进上表**（条目正文都在，
> 只是索引没跟上）—— 本表自 B36 之后断档；2026-11-11 已把缺失行补齐。

> **B11~B15 是同一轮排查的产物**（起点是"多继承下基类字段要不要改名"这个问题）。
> B13/B14/B15 三条根因相同 —— 见文末[小结](#小结-字符串兼任-id-与路径)。

---

## B1. auto 占位符被壳包住

- severity: medium ｜ status: fixed ｜ related: docs/learn/31, tests/tmpl/test_tmpl_55_auto_cv_forms.cpp

**现象**（六形态，只有裸 `auto` 活）

```cpp
int main() {
    int a = 5;
    const auto  v1 = a;    // ❌ Type mismatch in 'v1': declared 'const auto', got 'int'
    auto const  v2 = a;    // ❌ 同上
    auto*       v3 = &a;   // ❌ declared 'auto*', got 'int'
    auto&       v4 = a;    // ❌ declared 'auto&', got 'int'
    const auto& v5 = a;    // ❌ declared 'const auto&', got 'int'
    auto&&      v6 = 7;    // ❌ declared 'auto&&', got 'int'
    auto        v7 = a;    // ✅
    return 0;
}
```

**oracle**（clang++-18 全过，推导结果依次为 `const int` / `const int` / `int*` / `int&` / `const int&` / `int&&`）

**根因**：auto 分支只认**光杆** `Auto`，而占位符外面包了壳就看不见了。

| 源码 | parseType 建的树 | `type->isAuto()` | 落到哪 |
|---|---|---|---|
| `auto` | `Auto` | ✅ | auto 分支，正确 |
| `const auto` | `Const(Auto)` | ❌ | else 分支 ⇒ 严格类型检查 ⇒ 误报 |
| `auto*` | `Pointer(Auto)` | ❌ | 同上 |
| `auto&` | `LValueRef(Auto)` | ❌ | 同上 |

两个来源：前缀 const 由 `parser.cpp:336`（Step 2.5）套上；`*` / `&` / 后缀 const 由
`parser.cpp:350` 的后缀循环套上。**parser 无错**——按 [dcl.type.cv]/1，`const auto`
就是「const 修饰声明类型的基类型」，clang 的表示同构（const 在 QualType 上、AutoType 在里面）。
缺的是下游没人去壳里找占位符。

**修法**（[dcl.spec.auto]/7：用初始化式反推，再套回声明的 cv/指针/引用）

| declared | initializer | 重建 |
|---|---|---|
| `Auto` | `int` | `int` |
| `Const(Auto)` | `int` | `Const(int)` ← cv 是**声明的一部分**，不参与反推 |
| `Pointer(Auto)` | `int*` | `Pointer(int)` ← 要求实参确实是指针 |
| `LValueRef(Auto)` | `int` | `LValueRef(int)` |
| `RValueRef(Auto)` | `int` | `RValueRef(int)` |

一层递归剥壳 + 同层剥初始值，形状与 `TemplateDeducer::deducePair` 的合一循环同源。
对照 clang：`Sema::DeduceAutoType`。

**改动点**：`src/semantic_analyzer.cpp:2537` 与 `:2570` 的 `type->isAuto()` 两处入口。

**修法（已落地）** ✅

新增两个成员方法：

| 方法 | 作用 | 位置 |
|---|---|---|
| `containsAuto(t)` | 递归下钻 `Const`/`Pointer`/`Reference` 三层壳，判壳底是不是 `Auto` | `src/semantic_analyzer.cpp` |
| `deduceAutoType(pattern, init, ...)` | 合一：外壳照抄到结果，遇 `Auto` 把 A 绑上去 | 同上 |

`visit(VarDeclStmt&)` 的两处入口由 `type->isAuto()` 换成 `containsAuto(type)`；
日志里的硬编码 `"auto"` 换成 pattern 的 `toString()` —— **裸 `auto` 仍输出 `auto`**，
故既有用例日志逐字节不变（logdiff 已验证）。

实现与 `TemplateDeducer::deducePair` 同源，只是模式侧的"待绑定变量"是 `Auto`
而非 `TemplateParam`。

**回归**：
- `tests/tmpl/test_tmpl_55_auto_cv_forms.cpp` —— 六形态编译 + 求值，
  `main` 返回 32（与 clang++-18 同值）。求值式里六个变量都被读到，
  于是"某个形态被静默当成别的类型"也会让返回值改变。
- 推导【结果】的钉子不在运行期（`int&` 与 `int` 的拷贝语义当前不可区分，
  见 [B6](#b6-引用变量没有别名语义)）：靠 `logdiff` 基里那几行
  `[auto] ★ v4 : auto& ⟹ int&` 逐字节钉住。

**教训**

（无）

---

## B2. 函数模板 mangling 漏了参数签名

- severity: high ｜ status: fixed ｜ related: docs/learn/13, docs/learn/11, tests/unit/test_template_deduction.cpp

**现象**（同名模板的两个 arity 重载，T 都推导成 int）

```cpp
template <class T> int f(T x)       { return 1; }
template <class T> int f(T x, int n) { return 2; }
int main() { return f(1) + f(1, 2); }   // clang=3，修复前 minicc=2
```

修复前**只实例化出一个**符号：`f(1,2)` 的推导结果 `f(T,int){T:=int}` 与 `f(1)` 编出
同一个名字，撞上缓存里已有的实例被当成"已实例化"直接返回 ⇒ 两次调用都落到单参函数体，
得 `1+1=2`。

**根因**（两处，缺一不可）

| # | 位置 | 症状 |
|---|---|---|
| ① | `TemplateInstantiator::instantiateFunction` | 只编到模板实参 `_Z1fIiE`，缺 Itanium 的 `<bare-function-type>` |
| ② | `SemanticAnalyzer::getOrInstantiateFunction` | **用同一串当实例缓存的键** ⇒ 不同签名撞成一次"假命中" |

②比①更凶险：①只是符号名不精确；②直接让调用点**静默取回错误的函数体** ——
不报错、不崩溃，只是算错。

| | f(T) | f(T,int) |
|---|---|---|
| clang | `_Z1fIiEiT_` | `_Z1fIiEiT_S0_i` |
| minicc（修复后） | `_Z1fIiEiT_` | `_Z1fIiEiT_i` |

**修法**：新增 `NameMangler::mangleFunctionTemplateInstance`，在 `mangleTemplateInstance`
的 `_Z<名>I<模板实参>E` 之后追加 `<bare-function-type>`（返回类型 + 各参数类型）；
两处调用点**同源**改用新函数 —— 缓存键与实例符号必须逐字符一致。

模板形参在签名段里编成 Itanium `<template-param>`（序号 0 ⇒ `T_`，1 ⇒ `T0_`…），
故 `encodeType` 多收一个 `typeParams` 形参，**带默认空表** ⇒ 类模板路径不传，
符号逐字节不变。

**修法（已落地）** ✅

| 改动 | 位置 |
|---|---|
| 新增 `mangleFunctionTemplateInstance` | `src/template_instantiation.cpp` |
| `encodeType` 加 `typeParams` 默认形参 + `TemplateParam` 分支 | `src/template_instantiation.cpp:35` |
| 实例化端改用新函数 | `src/template_instantiation.cpp:430` |
| ★ 缓存键改用新函数（与上一条同源） | `src/semantic_analyzer.cpp:3963` |

**回归**：`tests/unit/test_template_deduction.cpp` 新增
`Instantiate.MangledNameCarriesSignature`（同名模板不同 arity、同 `T=int`，断言两符号
互不相同且分别为 `_Z1fIiEiT_` / `_Z1fIiEiT_i`）；同文件 `Instantiate.FunctionTemplate`、
`Instantiate.DifferentArgsDifferentSymbols` 的期望值同步更新为含签名的新名。

**影响面（大，已实测）**：131 行 logdiff 差异，**全部**是 `_Z…` 符号名，
`rc` 与程序输出**零变化** ⇒ 基线已全量重刷（`./logdiff.sh save`，89 个集成测试）。

**与 clang 的残留差异**：clang 会把参数表里**重复的类型**压成替换表引用
（`_Z1fIiEiT_S0_i`：第二个 `T` 编成 `S0_`），本实现一律展开成 `T_`。
符号仍唯一（模板实参段已区分实例），此差异单列为 [B8](#b8-mangling-没有替换表--符号与-clang-不逐字符等同)。

**现役案发现场**：`demos/tmpl/04_partial_order.cpp` 的「⚠ 已知边界」注释。

**教训**

（无）

---

## B3. 字段写入路径 3 硬编码 movl

- severity: high ｜ status: fixed ｜ related: docs/learn/31, docs/PITFALLS.md#c4-字段写入硬编码-movl, tests/unit/test_codegen_field_write.cpp

**位置**：`src/codegen.cpp:1123`（`emitAssign` 的 `NodeKind::Var` 分支 —— 裸字段名赋值）

```cpp
emit(std::format("movl %eax, {}(%rcx)    # .{} = ...（偏移 {}，4 字节写入）", ...));
```

**现象**（`ptr` 是 8 字节指针，写入被截掉高 32 位）

```cpp
struct MyPtr {
    int* ptr;
    MyPtr(int* x) : ptr(x) {}     // ← 初始化列表走路径 2，是对的
    void set(int* x) { ptr = x; } // ← 路径 3，写死 movl ⇒ 高 32 位丢
};
```

**根因**

**三条写字段的路**（读路径已按宽度分派，写路径只修了两条）

| 路径 | 写法 | 指令 |
|---|---|---|
| ① 成员表达式 | `q.p = v;` | 已按宽度分派 ✅ |
| ② 初始化列表 | `: ptr(x)` | 已按宽度分派 ✅ |
| ③ **裸字段名（函数体内）** | `ptr = x;` | **硬编码 `movl`** ❌ |

同理路径 1 的初始化端 `movq`（标量一律 8 字节）也会踩坏相邻字段——与
[PITFALLS C3/C4](PITFALLS.md#c4-字段写入硬编码-movl) 同族，那两条修的是 ①②。

**修法**：与读路径对称，按 `field->type` 的宽度选 `movb/movw/movl/movq`。

**影响面（小）**：只改这一条指令的发射逻辑，`size <= 4` 的分支保持原指令与原文案不动，
避免无谓的汇编/日志漂移。

**现役案发现场**：`demos/tmpl/07_alias_and_ctad.cpp` 的「⚠ 已知边界」注释。

**修法（已落地）** ✅

`emitAssign` 的 `NodeKind::Var` 分支改按 `field->size` 分派
（与读路径、初始化列表路径同构）：

| `field->size` | 指令 | 文案 |
|---|---|---|
| `== 1` | `movb %al` | `（偏移 N，1 字节写入）` |
| `<= 4` | `movl %eax` | `（偏移 N，4 字节写入）` ← **原文案一字未改** |
| 其余 | `movq %rax` | `（偏移 N，8 字节写入）` |

`<= 4` 分支保持原样，是为了让既有汇编零漂移 —— 修复只影响此前**写错**的 8B 路径。
重刷基线时的差异恰好是 3 处 `movl %eax, 0(%rcx) # .ptr = ...` → `movq %rax, ...`。

**回归**：`tests/unit/test_codegen_field_write.cpp`（`CodegenFieldWrite.*`，3 例）
—— 断言的是**不变量**「每条字段写入指令的宽度 == 该偏移上字段声明的宽度」，
而不是症状文案；三条写路径（成员表达式 / 初始化列表 / 裸字段名）各覆盖一遍，
否则"某条路径整体消失"会让测试静默变弱。

**负向已验证**：把 codegen 退回硬编码 `movl`，该用例立刻报
`偏移 8 的字段应为 q 宽度指令，实际用了 movl —— 指令：movl %eax, 8(%rcx)`。

**教训**

（无）

---

## B6. 引用变量没有别名语义

- severity: high ｜ status: fixed ｜ related: docs/learn/12, docs/learn/31, docs/learn/59, tests/tmpl/test_tmpl_55_auto_cv_forms.cpp, tests/unit/test_ref_alias.cpp, tests/lang/test_basics_38_ref_alias.cpp

**位置**：`src/codegen.cpp`（局部变量声明的槽分配 + `visit(VarDeclStmt&)` 发射路径）

**现象**（不需要 auto，裸引用一样）

```cpp
int main() {
    int a = 5;
    int& r = a;
    r = 30;
    return a;        // clang=30，minicc=5
}
```

**根因**：`int& r = a;` 在 Sema 侧类型是对的（`LValueRef(int)`），
但 CodeGen 把 `r` 当成一个**独立的栈槽**：初始化时把 `a` 的**值**拷进去，
`r = 30` 再写回 `r` 自己的槽 —— `a` 全程不受影响。

引用在语义上应当是**别名**（[dcl.ref]/1：引用不是对象、不占存储）：
`r = 30` 要翻译成"对 `r` 绑定的地址做间接写"，而不是"写 `r` 的槽"。
对照 clang：`CodeGenFunction::EmitDeclRefLValue` 对引用声明返回
`EmitLValueForReference`（拿到被绑定的地址），**从不给引用分配 alloca**。

**教训**

**为什么一直没被发现**：现有用例里引用只出现在两处**恰好**都正确的位置 ——
① **函数形参**（`void f(T& x)`）—— ★ 这条**台账原文写着"走调用方传地址，天生正确"，
是错的**（2026-11-11 实测推翻，见下方"修复"：调用点传的其实是**值**）；
② **类型位置**（`Box<int&>` 参与偏特化匹配，不涉及运行期）。
"局部引用改写被引用对象"这个组合此前没有任何用例。

**影响面（中）**：`r = v;`、`r += 1;`、把 `r` 再传给 `f(int&)` 全都不对。
它同时约束了 B1 的测试设计 —— `auto&` 推成 `int&` 还是 `int`，
在运行期**不可区分**（两者读出来都是拷贝值），
所以 `tests/tmpl/test_tmpl_55` 只能钉住"能编过 + 值可读"，
推导结果靠 logdiff 基里的日志行钉住。

**修法**（原计划三步，2026-11-11 实做时扩到两处 —— 见下方"修复"）：

1. **声明**：`visit(VarDeclStmt&)` 遇引用类型时不分配槽，而是求出初始化式的
   **地址**（复用 `&a` 那条路径）并记进别名表 `r → 被绑定槽的偏移`；
2. **读**：`emitVar` 见别名表先取地址再间接加载（`movq off(%rbp),%rcx; movq (%rcx),%rax`）；
3. **写**：赋值左侧同理走间接存储。

★ 注意与 [B4](#b4-带-return-的函数里局部对象析构是死代码) 的交互：
栈对象析构取址（`leaq off(%rbp), %rdi`）走的是**对象自身地址**，
引用别名不能影响它 —— 即"引用是别名，被引用对象仍在原槽"。

### 修复（2026-11-11，docs/learn/59）

**★ 第一处更正：原文"函数形参……天生正确"是错的，实测推翻了它。**

```cpp
void bump(int& x) { x = x + 1; }
int main() { int a = 5; bump(a); return a; }   // clang=6，minicc=5
```

旧实现里调用点传的是**值**（`emitExpr(arg)` 把 a 的值装进 rdi），
被调方把形参槽当普通 int 用 ⇒ callee 的写回不到调用方 ⇒ **同款静默算错**。
⇒ 修法从"局部引用变量"扩到"局部引用变量 + 引用形参"两处。

**修法三件套（判据单点）**：

1. **判据**：`isAliasBinding(type, operandHasAddress)` ——
   【有没有被绑定对象的地址】。局部侧传"初始化式可寻址吗"的实情；
   形参侧恒传 `false`（实参**可能是右值**，右值没有地址），
   只有"可写左值引用"能保证实参必是左值 ⇒ 走别名通道。
   ★ 调用点与被调点读**同一个 Type 对象**（经 `m_funcBySymbol`：
   mangledName → FunctionDecl）⇒ 不会"一边传地址、一边当值用"。
2. **声明/读/写**：局部引用变量的槽放**地址**；读走间接加载，写走间接存储；
   引用形参在序言登记为别名槽。
3. **作用域链**：别名表从平表改成**作用域链**（`m_refScopes`）。
   ★ 第一版实做时用了平表，自测用例当场 SIGSEGV ——
   同名变量在不同块里各有一种形态（块① `int& s` / 块② `S s`）时，
   块①的别名盖住了块②的类对象 ⇒ `s.v = 1` 写到别的地址。
   这正是 docs/learn/53 那条教训的第二次现形：**作用域是链，不是表**。

**回归**

- 单测 `tests/unit/test_ref_alias.cpp`（`RefAlias.*` 7 例）：
  槽里放的是地址 / 读走间接加载 / 写走间接存储 / 形参两侧同口径 /
  const 形参按值传（边界钉住）/ 三种绑定对象 / 作用域链遮蔽。
- 集成 `tests/lang/test_basics_38_ref_alias.cpp`（15 个检查点，与 clang 对拍 rc=0）。
- 突变验证 6 条逐条实跑变红（`tools/verify-ref-alias-mutations.sh`）。
- 全量 ctest 421 → **428**；既有 169 份集成日志**运行退出码逐字节不变**
  （漂移只在汇编文本与链接期段布局）。

**遗留边界**：const 引用形参 / 右值引用形参**按值传**（实参可能是右值，
要按地址传得在调用方物化临时量）⇒ 新登记
[B58](#b58-const-引用形参与右值引用形参按值传不跟踪被绑定对象)；
按引用返回仍未做（与 B50 同根因，见 ROADMAP §0.1）。

**教训**

- **"文档里写着这条路是对的"不等于它真的对** —— 本条台账里的
  "为何一直没被发现"那段本身就是错的，是**实测**推翻的。
  ⇒ 修缺陷前先探针实测，别信旧文档的结论。
- **"作用域"不是一张表，是一条链**（learn/53）—— 本批第二次现形：
  新加一种"形态"（别名）时，平表立刻暴露出遮蔽规则缺失。

---

## B7. 自由函数重载根本不做 mangling

- severity: medium ｜ status: open ｜ related: docs/learn/13, docs/learn/36

**现象**

```cpp
struct S { int v; };
int pick(int x) { return 1; }
int pick(S x)   { return 2; }
int main() { S s; s.v = 5; return pick(s.v) + pick(s); }   // clang=3，minicc: 汇编失败
```

`as` 报 `pick` 符号重复定义，汇编里两条 `.globl pick`。

**根因**：`SemanticAnalyzer::registerFunction` 对自由函数（`ownerClassName` 为空）
直接填裸名：

```cpp
if (decl->ownerClassName.empty()) {
    decl->mangledName = decl->name;   // ← 重载在这里全塌成一个名字
}
```

成员函数走 `Cls_method_N` 那套（另一条教学简化路径），函数模板实例走 NameMangler ——
只有**自由函数**是裸名。

上游还有一处**同一个病**：`m_functionMap[decl->name] = decl;` 也以裸名作键 ⇒
后注册的重载**覆盖**先注册的，调用点决议阶段连"存在两个候选"都看不见。

**教训**

**为什么 `mangleFunction` 没救**：它**写好了却零调用点**（全仓只有定义与声明）——
`_Z4picki` / `_Z4pick1S` 的编码逻辑现成，只是从来没人调它。

**修法**（草案）

1. `registerFunction` 的自由函数分支改填
   `NameMangler::mangleFunction(decl->name, "", decl->parameters)`；
2. `m_functionMap` 增设 mangled 键（或改存候选列表），调用点按**实参类型**挑候选 ——
   这是真正的工作量：当前决议靠"形参类型精确匹配"扫 `m_functionMap`，键塌了就没得挑；
3. **两个必须保留裸名的特例**：`main`（`_start` 要调它）与内置 libc 原型
   （`registerBuiltins` 的 `malloc`/`free`/`memcpy`/`realloc` 要由系统链接器绑定）；
4. CodeGen 的 `call` 目标需与 Sema 选中的候选**同源**（沿用既有的
   `mangledName.empty() ? name : mangledName` 约定）。

**影响面（大）**：**每一个**集成测试的汇编都会变（所有自由函数符号）⇒
需独立重刷基线并逐项核对 `rc` 与程序输出不变。

**现状**：⬜ 未修 —— 与 B2 同源（都是 NameMangler 没被善用），但改动波及**决议**与
**CodeGen**，风险高于 B2，宜单独立项。

**回归**

（无）

**教训**

（无）

---

## B8. mangling 没有替换表 ⇒ 符号与 clang 不逐字符等同

- severity: low ｜ status: open ｜ related: docs/learn/13, docs/learn/35

**现象**（B2 修复后的残留差异）

| 源码 | clang | minicc |
|---|---|---|
| `template<class T> T twice(T x)` | `_Z5twiceIiET_S0_` | `_Z5twiceIiET_T_` |
| `template<class T> int f(T x, int n)` | `_Z1fIiEiT_S0_i` | `_Z1fIiEiT_i` |

**根因**：Itanium 的 **substitution**（ABI §5.1.9）—— 编码途中遇到的 `<type>` 进一张表，
**再次出现时编成 `S<n>_`**。clang 编返回类型 `T_` 时把它放进 #0，第二个 `T` 于是压成
`S0_`；minicc 的 `encodeType` 无状态，一律展开。

**性质**：**不是正确性问题** —— 符号仍唯一（模板实参段已区分实例），
链接、决议、执行都不受影响。记在这里是因为它影响"与 clang 逐字符核对"这个
教学惯例（NTTP 那批就是逐字符对过的）。

**修法（要修的话）**：`encodeType` 需带一张 substitution 表（引用或成员），并确定哪些节点入表
（Itanium 规则里复合类型入表，内建类型的行为需实测确认）。
**注意**：minicc 的类模板实例类型是**改名后的扁平 Type**（`MyPtr_int`），
没有可压缩的嵌套结构，故替换表只对**函数模板签名的参数表**有意义 —— 收益有限。

**现状**：⬜ 未修（教学差异，非缺陷）。

**回归**

（无）

**教训**

（无）

---

## B9. `struct` 的默认继承级别被当成 private 处理

- severity: medium ｜ status: fixed ｜ related: docs/learn/31, tests/lang/test_basics_01.cpp

**现象**

```cpp
struct Base { int v; };
struct Derived : Base { int w; };
int main() { Derived d; d.v = 3; d.w = 4; return d.v + d.w; }   // clang=7
```

minicc：`[Parse Error] 2:18 at 'Base': 仅支持 public 继承（每个基类前需写 'public'）`

**性质**：**错误拒绝合法程序** —— 比"不支持某特性"更严重：它让用户以为自己的代码写错了。
外部 25 个模板用例里 5 个倒在这里。

**根因**：[class.derived]/2 规定默认继承级别由**声明关键字**决定（`struct` ⇒ public，
`class` ⇒ private），而 `parseClassDecl` 只看"有没有写 public"、没看关键字 ——
`isStruct` 本来就在手上，却被 `(void)isStruct;` 丢弃了。

**修法** ✅

| 写法 | 修复前 | 修复后 |
|---|---|---|
| `struct D : Base` | ❌ 报错 | ✅ public 继承 |
| `struct D : public Base` | ✅ | ✅ |
| `class D : public Base` | ✅ | ✅ |
| `class D : Base` | ❌ 文案不准 | ❌ 说清"默认 private，未实现" |
| `struct D : private Base` | ❌ 文案不准 | ❌ 说清"private 未实现" |

**教训**

**设计决策**：private/protected 继承的**语义**（基类子对象的访问控制）本项目不做，
故仍报错；但文案从"语法不允许"改成"语义未实现" —— **报错理由必须准确**。

**回归**：`tests/lang/test_basics_01.cpp` 第 ① 项；文档 `docs/learn/31` §1。

---

## B10. 带参成员方法的调用符号拼不出来

- severity: medium ｜ status: fixed ｜ related: docs/learn/13, docs/learn/36, tests/stl/test_stl_02..05

**现象**（不需要下标糖 —— 普通方法调用就够了）

```cpp
class C { public: int f(int x) { return x; } };
int main() { C c; return c.f(1) - 1; }   // clang = 0
```

minicc：`[LINK ERROR] undefined reference to 'C_f'`

**性质**：**正常程序编不过**，且错误一路推迟到链接期 —— 编译期的 Parser/Sema 全程绿灯，
用户看到的是"符号没定义"，完全指不到真正的原因。

**根因**：同一条语义判断写在了两个地方，两侧算法不一致。

| 位置 | 规则 | `IntVec::at(int)` 的产物 |
|---|---|---|
| 定义点 `semantic_analyzer.cpp` registerFunction | 带参成员方法名追加"参数个数"后缀 | `IntVec_at_1` |
| 调用点 `codegen.cpp` visit(CallExpr) | 硬拼 `类名 + "_" + 方法名` | `IntVec_at` |

```text
定义：.globl IntVec_at_1   .globl IntVec_set_2
调用：callq IntVec_at      callq IntVec_set        ⇒ 谁也匹配不上
```

**影响面**：**大** —— 凡"类方法带参数"即中招。集成测试里 `test_stl_02..05`
四个长期 rc=1；因为 logdiff 基线把 rc=1 当成了契约，**坏了很久没人发现**
（这本身就是教训：*基线固化失败 ⇒ 缺陷变成"预期行为"*）。

**修法** ✅ —— 沿用项目既有的"符号回填"套路

`MemberExpr::resolvedCalleeSymbol` 早已存在（成员模板 `Acc_add_int` 靠它绕开硬拼），
本次把**普通成员方法**也接上同一根线：

| 改动点 | 内容 |
|---|---|
| `semantic_analyzer.cpp` `inferCall` | 成员方法命中 ⇒ `mem->resolvedCalleeSymbol = method->mangledName` |
| `include/ast.h` `IndexExpr` | 新增 `atSymbol` / `setSymbol` 两个回填槽 |
| `semantic_analyzer.cpp` `inferIndex` | 查 `at()` 时顺带回填 `set()` 符号（`set` 只填不强制） |
| `codegen.cpp` `visit(IndexExpr)` / `AssignStmt(Index)` | 优先用回填符号，空则退回旧硬拼 |

**为什么虚调用不受影响**：`CodeGen::visit(CallExpr)` 先查 vtable 条目，命中即
`emitVirtualCall` 并 `return` —— 回填值只作用于其后的"非虚直接调用"分支。
修复后的基线差异**恰好只有那 4 个 stl 文件**，其余 90 个集成测试逐字节不变，
即是此判断的实证。

**回归**：`tests/stl/test_stl_02..05`（四个文件头都写明了本回归点）。

**顺带暴露（未处理）**：`inferIndex` 只校验 `at()`、从不校验 `set()` ——
类里没写 `set()` 时，错误同样一路推到链接期。本次按"只回填不强制"保持既有行为，
补校验是另一件事。

**教训**

（无）

---

## B4. 带 return 的函数里局部对象析构是死代码

- severity: high ｜ status: fixed ｜ related: docs/learn/08, docs/learn/12, tests/ctor/test_ctor_03_raii_return.cpp

**现象**（析构里写全局变量，看有没有跑）

```cpp
int g = 0;
struct Dog { int v; ~Dog() { g = 5; } };

int f() {
    Dog d;
    return 0;        // ← 函数体里有 return
}

int main() { f(); return g; }    // clang=5，minicc=0
```

**根因**：`return` 语句自己发射了 `leave; ret`，而「函数末尾的统一析构」是在函数体**发完之后**
无条件追加的 —— 控制流早已返回，那几条析构指令是**不可达的死代码**。

实测 `f` 的汇编（`minicc -S`）：

```asm
    # return expr
    movq $0, %rax           # 整数字面量载入 rax
    leave                         # 恢复栈帧
    ret                           # 返回调用者
    # ~Dog() auto at function end (RAII)
    leaq -16(%rbp), %rdi       # ← 永远执行不到
    callq Dog_dtor
```

**为什么单测没抓到**：函数**没有** `return`（自然落到末尾）时析构是可达的，
所以既有的 RAII 用例全绿；只有「有 return + 有局部对象」这个组合才暴露。

**修法**：析构不该绑在「函数末尾」这个**位置**上，而该绑在**作用域退出点**上
（[stmt.return]/[class.dtor]：形参/局部对象在离开作用域时析构）。两条路：

| 方案 | 做法 | 代价 |
|---|---|---|
| ① 就近发射 | `return` 前先发本作用域的析构，再 `leave; ret` | 每个 return 点都要发一遍；嵌套块要逐层 |
| ② 统一出口 | 各 `return` 只 `jmp` 到函数末尾的收尾标签，收尾处发析构 | 改动小，但所有 return 路径的 rax 传递要保证不被打扰 |

②更接近 clang 的做法（`ReturnStmt` → `EmitBranchThroughCleanup` → 统一 cleanup block）。

**影响面（中）**：这是 **RAII 语义**问题，不是指令细节 —— 目前所有「有 return 的函数里
放局部对象」的代码都在静默泄漏/不析构。修完需要补回归用例。

**修法（已落地）** ✅

采用**方案①「就近发射」**（`CodeGen::emitDtorsOnReturn`，`src/codegen.cpp`）：
`visit(ReturnStmt)` 在 `leave; ret` **之前**把栈上所有活跃作用域层的对象
从内到外、层内逆序各析构一遍，返回值先 `pushq %rax` 保起来再逐层 `callq`。

选①不选②的理由：②（统一出口）要跨作用域收集对象、把每个 `return` 改成 `jmp`，
会改动**所有**函数的 return 发射；①只动 `visit(ReturnStmt)` 一处，且
**无局部对象时一个字节都不多发** —— 87 个既有集成测试的汇编因此逐字节不变。

实测 `int f() { Dog d; return 0; }`：

```asm
    movq $0, %rax           # 返回值
    pushq %rax              # 保护返回值（析构调用会踩掉 rax）
    # ~Dog() before return (RAII, scope exit)
    leaq -16(%rbp), %rdi
    callq Dog_dtor          # ← 现在在 leave; ret 之前
    popq %rax
    leave
    ret
```

代价：return 之后那段"块尾/函数尾析构"仍会发射（已不可达，无害的冗余）。

**回归**：`tests/ctor/test_ctor_03_raii_return.cpp`（集成测试）
—— 返回值 121 同时钉住"跨层"与"LIFO"两件事：
`f()` 析构 a(1) ⇒ g=1；`h()` 先 b(2) 再 a(1) ⇒ g=1*10+2=12 → 12*10+1=121。
只析构最外层 / 顺序反了 / 漏了内层，都会得到别的数。

**教训**

（无）

---

## B5. 块注释不跨行

- severity: medium ｜ status: fixed ｜ related: docs/learn/07, tests/unit/test_preprocessor.cpp

**位置**：`src/preprocessor.cpp:88`（`Preprocessor::stripComment`）

**现象**（`/*` 独占一行、内容在后续行 —— 最普通的注释写法）

```cpp
/*
 * 说明文字
 */
int main() { return 0; }
```

minicc 报 `Unexpected character '�'`（中文注释），或对 ASCII 注释把
`*` `ascii` `*` `/` 四个记号喂给词法器。

**根因**：`stripComment` 是**逐行**调用的，`*/` 不在同一行时它丢掉本行剩余部分、
**却不记住"还在块注释里"**：

```cpp
size_t end = line.find("*/", i + 2);
out += ' ';
i = (end == std::string::npos) ? line.size() : end + 1;   // ← 跨行状态在此丢失
```

`[lex.phases]` 阶段 3 规定：注释在**分词前**整体替换成一个空格，块注释可以跨行 ——
这是硬要求，不是可选的简化。

**实测**（5 个最小探针）

| 输入 | 旧行为 |
|---|---|
| `/* 中文 */ int x;` | ✅ 同行闭合，正常 |
| `/*` ⏎ `中文` ⏎ `*/` | ❌ `Unexpected character '�'` |
| `/*` ⏎ ` * ascii` ⏎ ` */` | ❌ 分词成 `*` `ascii` `*` `/` |
| `int a; // 中文` | ✅ 行注释无跨行问题 |

**为什么词法器不是元凶**：`Lexer::skipWhitespaceAndComments`（`src/lexer.cpp:183`）
对跨行 `/* */` 的处理是**正确**的 —— 它拿到的是被预处理器破坏过的文本，锅在上一阶段。
判据：`-E` 输出里第 1 行是空格、第 2~3 行**原样保留**（正常应全是空白）。

**影响面（大）**：外部 25 个模板验收用例里 **15 个**倒在这里，且报错形态
（`Unexpected character '�'`）完全掩盖了真实缺口 —— 修掉后 25 个文件**全部**
推进到 Parser 阶段，暴露出的才是真语言缺口。

**修法**：给 `stripComment` 加**跨行状态**参数 `bool& inBlockComment`，
由 `processText` 的逐行循环按文件保管（`#include` 递归时各层独立，不串味）。

**修法（已落地）** ✅

| 改动 | 位置 |
|---|---|
| 签名加 `bool& inBlockComment` | `include/preprocessor.h` |
| 注释内逐字符换空格、见 `*/` 复位 | `src/preprocessor.cpp` |
| `processText` 循环外持有状态 | `src/preprocessor.cpp:178` |

**回归**：`tests/unit/test_preprocessor.cpp` 的 `LexUtils.StripCommentAcrossLines`
—— 断言 ① 两行后仍在注释内 ② 见 `*/` 复位 ③ 同行闭合不留残状态。

**教训**

> 工程教训：这个 bug 能潜伏这么久，是因为**既有测试里的块注释恰好都写在同一行**，
> 而多行块注释（`/*` 换行正文再 `*/`）才是最普通的写法 —— 测试覆盖的是"能过"的形态。

---

## B11. 本类自身多态而基类全非多态时 _vptr 与首基类字段压在同一 offset

- severity: high ｜ status: fixed ｜ related: docs/learn/17, docs/learn/50, tests/mi/test_mi_08_self_poly_no_poly_base.cpp, tests/unit/test_layout_lookup.cpp

**位置**：`src/semantic_analyzer.cpp` 的 `[relocate]` 子对象摆放阶段（`processClassDecl` 内）

**现象**（最朴素的单继承 + 一个虚函数）

```cpp
struct B { int name; };
struct A : B { virtual int f() { return 5; } };
int main() { A a; a.name = 3; return a.f() + a.name; }   // clang = 8
```

| | 编译 | 运行 | A 的布局 |
|---|---|---|---|
| clang++-18 | ✅ | `8` | `sizeof(A)=16`、`offsetof(A,name)=8` |
| minicc（修复前） | ✅ rc=0 | **SIGSEGV** | `+0: _vptr`、`+0: B.name` ← 同一格 |

**性质**：**真 miscompile** —— 合法程序编译通过、运行崩溃。写基类字段即写坏虚表指针，
下一次虚调用经 `[vptr=3]` 跳飞（实测 `exit=139`）。

**根因**：primary 的选择只扫了基类，没扫自己。

Itanium 的主基类优化：**primary base 只在动态（多态）基类里选**。一个多态基类都没有、
而本类**自身**有虚函数时 ⇒ **没有主基类**：vptr 自己占 offset 0，全部基类子对象从 8 起摆。

`[relocate]` 里那份判据只写了两态：

| 基类里有多个态基类吗 | 本类自身多态吗 | 旧行为 | 正确行为 |
|---|---|---|---|
| 有 | 任意 | primary = 第一个多态基类 @0 | ✅ 同左 |
| 无 | 否 | 首个基类当 primary @0 | ✅ 同左（全非多态，单继承兼容） |
| 无 | **是** | 首个基类当 primary @0 ❌ | **无 primary**：vptr@0、基类从 8 起 |

漏的正是第三态。而"本类自身多态"要到**更后面**的方法注册循环才置
`classLayout.hasVTable = true`，`[relocate]` 时还看不见 ——
两处各自看到的"多态性"不一致，又是一次"同一判断两处各算一份"。
`struct A : B, C { virtual ... }`（两个非多态基类）同理：首个基类 @0 与 vptr 重叠。

**影响面（大）**：单继承最朴素的写法就会中招，且**编译期全程绿灯**。
不崩只是因为恰好没人读那个被踩烂的 vptr —— 本 bug 的第一版用例返回 6"看着对"，正是如此。

**修法** ✅：判据补上第三态。

| 改动 | 内容 |
|---|---|
| 新增 `selfPoly` 预判 | 无多态基类时，扫 `decl->methods` 有无带 `virtual` 的方法 |
| 新增 `noPrimary` | `!anyPoly && selfPoly` ⇒ 所有子对象 `isPrimary=false`，`place` 从 8 起 |
| 日志 | 该分支打印"本类自身多态 ⇒ 无主基类，_vptr 占 0" |

★ `selfPoly` 只需看 `virtual` 关键字：无多态基类 ⇒ 没有可覆写的虚函数，
"覆写"这条来源不可能成立（与下方 override 检测同源，不重复实现）。

**教训**

★ 既有分支的日志**逐字节不变**（`primary='X' 占 0` 原文保留）——
94 个集成测试零漂移，是"只影响此前算错的那一支"的实证。

**回归**：
- `tests/mi/test_mi_08_self_poly_no_poly_base.cpp` —— 两场景合一，返回 `111`
  （`5+3` 单非多态基类、`100+1+2` 双非多态基类）。
- `tests/unit/test_layout_lookup.cpp` 的 `LayoutLookup.VptrNeverOverlapsFields`
  —— 断言**不变量**「多态类的任何字段都不得落在 `[0,8)`」，覆盖三种继承形态；
  而不是"某类某字段在 8"这种症状值。
- **负向已验证**（单测）：`noPrimary` 改回恒 `false` ⇒ 立刻报
  "字段 'B.name' 偏移 0 落在 _vptr 区（0..7）—— 写它即写坏虚表指针"。
- **负向已验证**（集成）：同一突变重编 minicc ⇒ `test_mi_08` 运行 `-11`（SIGSEGV）。

---

## B12. 继承来的成员方法查不到

- severity: medium ｜ status: fixed ｜ related: docs/learn/13, docs/learn/17, tests/mi/test_mi_09_inherited_member.cpp

**位置**：`src/semantic_analyzer.cpp` 的 `inferMember`（查方法那一段）

**现象**

```cpp
struct A { int g() { return 5; } };
struct D : A { };
int main() { D d; return d.g(); }        // clang = 5
```

minicc：`[ERROR] [Semantic Error] 3:27: No member 'g' in class 'D'` —— **合法程序被拒**。

**根因**：成员查找只查了本类。

| 成员种类 | 存放在哪 | 查得到吗 |
|---|---|---|
| 字段 | 已**扁平化**进 `classLayout.fields`（含继承来的） | ✅ |
| 方法 | 各基类自己的 `decl->methods` 里，**没有**扁平化 | ❌ 只扫了本类 |

讽刺的是 `inferCall` 里**另有一条**搜基类的 BFS（成员调用走它），
但它被更早的 `inferType(callee)` → `inferMember` 的 `error()` 挡死 ——
`d.g()` 永远走不到那条 BFS。**同一条查找规则写在两处，只有一处带基类链**，
与 [B10](#b10-带参成员方法的调用符号拼不出来) 的"双份判据"同型。

**修法** ✅：把判据收口成两个原语，两条通路共用。

| 原语 | 作用 | 调用者 |
|---|---|---|
| `findMethodInClass(类, 名, arity)` | 在**单个类**里找方法；`arity < 0` 表示不看参数个数 | `inferCall`（arity=实参个数）、`findMethodInHierarchy` |
| `findMethodInHierarchy(类, 名, *declaringClass)` | 沿 `baseClassNames` 遍历（含本类），回填命中层 | `inferMember` |

★ 遍历顺序与 `inferCall` 的搜索**逐字一致**（基类名 `insert` 到队首、从队尾取）——
否则"两个基类都有同名方法"时两条通路会选中不同的那一个。
★ 隐藏规则由此自动成立：本类先于基类 ⇒ `D::g` 胜过 `A::g`。
★ 日志保持原文，命中基类时追加 `via 'A'`（本类命中不加 ⇒ 既有输出逐字节不变）。

**回归**：`tests/mi/test_mi_09_inherited_member.cpp`（多级链 `C:B:A`、指针形态、
同名隐藏三件事合一，返回 8）；单测 `LayoutLookup.InheritedMethodIsFound`。
**负向已验证**：`findMethodInHierarchy` 换回 `findMethodInClass` ⇒ 单测红；
重编 minicc ⇒ `test_mi_09` 编译 `rc=1`。

**教训**

（无）

---

## B13. 两个次基类各有一个同名字段时显示名塌陷且第二条偏移算成 0

- severity: medium ｜ status: fixed ｜ related: docs/learn/17, docs/learn/49, tests/mi/test_mi_11_ambiguous_member.cpp

**现象**

```cpp
struct B { int name; };
struct C { int name; };
struct A : B, C { };
struct D : A { int tag; };
```

修复前的 `--dump-layout`：

```text
    ══ Memory Layout of 'D' ══
    +0: A.name : int (4 bytes)     ← 应该是 B::name
    +0: A.name : int (4 bytes)     ← ★ 应该是 +8，却算成了 0
    +16: tag : int (4 bytes)
```

**性质**：**只影响真 C++ 本来就 ill-formed 的程序**（两个子对象各有一份 `name`
⇒ [class.member.lookup]/8 歧义，`d.name` 必须报错）—— 不是 miscompile，
而是"静默接受非法程序 + 布局打印错"。但它与 B14/B15 同根，故一并根治。

**根因**（两处叠加）

| # | 位置 | 症状 |
|---|---|---|
| ① | 扁平化循环 | 改名时"剥掉第一个 `.` 再拼直接基类名" ⇒ `B.name` 与 `C.name` 剥成裸名 `name` 后都变成 `A.name` |
| ② | `computeClassLayout` | 主/次基类分支都按**裸名**在基类布局里找**第一个**命中 ⇒ 两条记录都命中 `B.name@0` |

**修法（根治）：让名字不再兼任"路径"**

`FieldInfo` 补三项，把"住在哪个子对象里"变成可精确回答的问题：

| 新字段 | 含义 |
|---|---|
| `declaredName` | 权威裸名 —— 名字查找的键（不再用显示名反拼前缀） |
| `viaBase` | 装着它的**直接基类**子对象名（空 = 本类自身字段） |
| `baseFieldIndex` | 在 `viaBase` 那个基类布局 `fields[]` 中的**下标** |

`computeClassLayout` 的字段放置随之合并成一条式子：

```text
field.offset = 子对象偏移(viaBase) + 基类布局[baseFieldIndex].offset
```

主基类子对象偏移恒为 0 ⇒ 该式自动退化成"直接复用基类偏移"，与旧实现同值；
两条分支合一后，`currentPrimaryBase` 变量连同"先挑主基类再分派"的逻辑一并删除。

**教训**

**对照 clang**：成员是 `FieldDecl*` + `getFieldIndex()`，`RecordLayoutBuilder` 从不做字符串匹配。

**配套：歧义诊断**（[class.member.lookup]/8）

`findField` 只能回答"取哪一条"，回答不了"是不是只有一条"。新增
`ClassLayout::findFields(name)`（自身字段命中即隐藏基类，否则返回**全部**继承命中），
`inferMember` 在 `size() > 1` 时报：

```text
[ERROR] [Semantic Error] 43:6: Member 'name' is ambiguous in class 'D':
  found in 2 base-class subobjects (经 'A' 的 'A.name'、经 'A' 的 'A.name')
```

**回归**：`tests/mi/test_mi_11_ambiguous_member.cpp` —— **预期编译失败 rc=1**，
基线固化其报错原文（rc 若变 0，即说明歧义检测退化）；
单测 `LayoutLookup.LookupIsPrefixIndependentAndAmbiguityIsVisible` 的后半段断言必须抛错。
**负向已验证**：关掉歧义检测 ⇒ 单测红；重编 minicc ⇒ `test_mi_11` 变成 `rc=0`
（正是修复前"静默接受"的样子）。

---

## B14. 派生类同名字段的隐藏方向做反了

- severity: high ｜ status: fixed ｜ related: docs/learn/17, docs/learn/49, tests/mi/test_mi_10_field_hiding.cpp

**位置**：`include/type.h` 的 `ClassLayout::findField`

**现象**

```cpp
class A { public: int x; void setA(int v) { x = v; } int getA() { return x; } };
class D : public A { public: int x; };
int main() { D d; d.setA(7); d.x = 5; return d.getA() * 10 + d.x; }   // clang = 75
```

minicc（修复前）= **55** —— `d.x` 落到了 `A::x`（`d.getA()` 也读出 5 而不是 7）。

**性质**：**真 miscompile** —— 合法、**无歧义**（[class.member.lookup]/3：派生类声明
**隐藏**基类同名声明，`d.x` 唯一 = `D::x`），却静默绑到基类那一份。

**根因**：把**显示名**当成了**查找键**。

| 步骤 | 旧行为 |
|---|---|
| 扁平化 | 自身字段 `x` 与基类字段撞名 ⇒ 把**自身**改名成 `D.x`，基类那条留 `A.x` |
| 查找 | 第二步按 `f.name == f.sourceClass + "." + name` **反拼前缀** ⇒ 裸名 `x` 精确配上 `A.x` ⇒ 命中基类 |

方向就这样反了：为了让字符串不撞，动的是自身字段；而"精确匹配先到先得"
又让 `d.x` 指到了基类成员 —— **显示层的改动泄漏进了索引层**。

**修法** ✅：`findField` 改三级，键换成 `declaredName`

| 顺序 | 命中条件 | 依据 |
|---|---|---|
| ① | `f.name == name` | 调用方自己写了全限定名 `"A.a"` |
| ② | **自身字段**（`sourceClass` 空或 == 本类名）且 `declaredName == name` | [class.member.lookup]/3 隐藏 |
| ③ | 继承字段且 `declaredName == name` | 兜底 |

**教训**

★ 显示名（`D.x` / `A.x`）仍照旧生成 —— 它只用于打印，不再参与判定。

**回归**：`tests/mi/test_mi_10_field_hiding.cpp`（返回 75）；
单测 `LayoutLookup.OwnFieldHidesInheritedOne`（断言**不变量**：`d.x` 命中的那条
必须 `viaBase` 为空 = 本类自身）。
**负向已验证**：把 ② 的条件改回"只认 `sourceClass` 为空"（= 旧行为）⇒ 单测红；
重编 minicc ⇒ `test_mi_10` 运行 `55`。

---

## B15. 祖辈前缀与直接基类撞名时偏移算到错的子对象

- severity: medium ｜ status: fixed ｜ related: docs/learn/17, docs/learn/49, tests/unit/test_layout_lookup.cpp

**位置**：扁平化循环 + `computeClassLayout`（与 B13 同一段代码）

**现象**

```cpp
struct Q { virtual int g() { return 0; } };
struct B { int x; };
struct P : Q, B { int y; };
struct X : P, B { int tag; };
```

修复前：从 `P` 主基类链带出来的那份 `B.x` 被算到了 **X 的直接 B 子对象**上，
于是两条记录**同偏移**（本该一条 +8、一条 +24）。

**性质**：clang 对 `o.x` 报 ill-formed ——
`non-static member 'x' found in multiple base-class subobjects of type 'B'`。
与 B13 同属"本就非法 + 布局错"，单列是因为它的根因更本质。

**根因**：`sourceClass` 一个字段兼任了两件事。

| 它想说的事 | 它实际是 | 破口 |
|---|---|---|
| 谁**声明**了这个字段 | 一个裸类名 | 同一类名在一条链上出现两次就分不开 |
| 它住在哪个**子对象**里 | 靠 `sourceClass` 去 `bases[]` 里**反查** | 反查命中的是**本类**的同名直接基类 |

主基类分支是**原样拷贝**（`FieldInfo fi = baseField;`），祖辈留下的 `sourceClass="B"`
一路带到 X —— 而 X 恰好真有一个直接基类叫 `B`。

**修法** ✅：`viaBase` 在扁平化时**覆盖**为本层的直接基类、`baseFieldIndex` 记录下标；
`computeClassLayout` 按 `(viaBase, 下标)` 定位，不再看 `sourceClass`、也不再按名字匹配。

**教训**

祖辈前缀从此只是显示标签，去多少代都不影响查找；它与"源自 B"这件事各由
`declaredName` / `sourceClass` 独立承载。

**回归**：单测 `LayoutLookup.LookupIsPrefixIndependentAndAmbiguityIsVisible`
—— 断言两条同名记录**偏移必须不同**、且各自的 `viaBase` 必须是**本类的直接基类**。
**负向已验证**：把派发改回 `sourceClass` + 按名字查内部偏移（**忠实复刻旧代码**）⇒
该单测报 `Expected: (hits[0]->offset) != (hits[1]->offset), actual: 24 vs 24` —— 正是旧 bug 的形态。

---

## B16. 无声明符的声明被拒收（`A<int*, int**>;` 报 Parse Error）

- severity: medium ｜ status: fixed ｜ related: docs/learn/46, docs/learn/47, tests/unit/test_declaratorless_decl.cpp, tests/lang/test_basics_02_declaratorless.cpp

**现象**

```cpp
template<class a, class b> struct A { };
template<class T>          struct A<T, T*> { };   // 偏特化

int add(int a, int b) { return a + b; }

int main() {
    A<int*, int**>;                 // ← 无声明符的声明
    auto result = add(1, 2);
    return 0;
}
```

minicc：`[Parse Error] 12:7 at 'int': Unexpected token 'int' in expression`（rc=1）
clang++-18：接受，只给 `warning: declaration does not declare anything [-Wmissing-declarations]`

**四形态对照**（前两行是 bug，后两行是"本来就该拒"的对照组）

| 语句 | clang++-18 `-std=c++20` | minicc | 备注 |
|---|---|---|---|
| `A<int*, int**>;` | ✅ 仅 warning | ❌ Parse Error，**报错点在实参中间** | 走前瞻分支 |
| `int;` | ✅ 仅 warning | ❌ `Expected variable name` | 走类型关键字分支 |
| `int*;` | ❌ `declaration of 'int *' has no name` | ❌ 同上 | ✅ 两边一致：`*` 后不能省名字 |
| `S;`（S 为类名） | ✅ 仅 warning（**判为声明**） | ✅ **但判为表达式语句** | 分类不同、当前都无副作用 |

**性质**：**错误拒绝合法程序** —— 与 [B9](#b9-struct-的默认继承级别被当成-private-处理) 同类。

**根因**：[dcl.dcl]/1 的 simple-declaration 里 `init-declarator-list` 是**可选**的 ——
`decl-specifier-seq ;` 本身合法，语义是"什么都不声明"（clang 的 `-Wmissing-declarations`
正是为它准备的）。minicc 的两条声明路径都**默认"类型后面必有名字"**：

1. **类型关键字开头**：`src/parser.cpp:1928` → `parseVarDeclStmt` →
   `src/parser.cpp:2025` 的 `expect(Identifier, "Expected variable name")` 撞死在 `;` 上。
2. **标识符开头**：`src/parser.cpp:1982` 的前瞻跑完"跳类型名 + `<...>` 深度配对 + `*`/`&`"
   之后停在 `;`，`check(Identifier)` 不成立 ⇒ 回滚到 `src/parser.cpp:1990` 走表达式分支
   ⇒ `parseExprOrAssignStmt` 把 `A` 当标识符表达式，再撞死在第 2 个实参的 `int` 上。

**教训**

★ 第 2 条的**诊断质量**尤其差：报错位置指向实参中间的 `int`，用户完全看不出
真正的问题是"这条声明没有声明符"。根因在语句层，症状在表达式层 —— 中间隔着一次回滚。

**为何不影响偏特化结论**（本轮排查的起点是"`A<int*, int**>` 走哪个裁决"）：
本 bug 只卡在**无声明符**这一种写法上。改成 `A<int*, int**> x;` 后链路完全正常，
`selectClassTemplate` 三路走 ②（`A<T, T*>`，合一推出 `T := int*`），与 clang 的
`static_assert(A<int*,int**>::tag == 1)` 一致 —— **偏特化匹配本身是好的，卡的是 Parser。**

**影响面**：小（纯语法缺口，不会产出错误代码）。但两处：一是合法程序被拒，
二是**报错点离根因太远**，属于"诊断指向派生现象而非根因"的老毛病。

**修法**：新增 `EmptyStmt` 节点（`NodeKind::Empty`，对应 clang 的 `NullStmt`），
把「空语句 `;`」与「无声明符的声明」收在同一个节点上 —— 两者语义同为"不做任何事"。
空语句是**纯叶子**（只有位置、无子节点、无类型），于是 Sema 根本无从 `resolveType`，
"不实例化任何模板"由**结构**保证，而不是靠约定。

**修法（已落地）** ✅

| 改动点 | 文件 | 内容 |
|---|---|---|
| 新节点 | `include/ast.h` | `NodeKind::Empty` + `struct EmptyStmt : Statement` |
| 访问者 | `include/ast_visitor.h` | `visit(EmptyStmt&)`（语句 8 → 9，总节点 32 → 33） |
| 路径 ① | `src/parser.cpp` `parseStatement` 开头 | 裸 `;` ⇒ `EmptyStmt`（[stmt]/1） |
| 路径 ② | `src/parser.cpp` 类型关键字分支 | 类型后跟 `;` 且**声明符为空** ⇒ `EmptyStmt` |
| 路径 ③ | `src/parser.cpp` 标识符前瞻分支 | 前瞻吃到了**类型 id 语法**且停在 `;` ⇒ 回滚 `parseType` ⇒ `EmptyStmt` |
| 判据 | `src/parser.cpp` `isDeclaratorlessDecl` | 剥掉最外层 `const` 后仍是指针/引用 ⇒ 声明符非空 ⇒ **不放行** |

★ 判据 `isDeclaratorlessDecl` 是这条修复的**难点所在**：minicc 的 `parseType`
把 ptr-operator 一并吃进类型（`int*` ⇒ `Pointer(Int)`），所以"声明符是否为空的
要在**类型上反推** —— 剥掉最外层 `const`（它属 decl-specifier）后若仍是
指针/引用，说明吃进了 ptr-operator。逐例与 clang 核对：

| 写法 | 类型 | 剥 const 后 | clang | minicc 修复后 |
|---|---|---|---|---|
| `int;` | `Int` | `Int` | ✅ 仅 warning | ✅ |
| `const int;` | `Const(Int)` | `Int` | ✅ 仅 warning | ✅ |
| `unsigned int;` | `UInt` | `UInt` | ✅ 仅 warning | ✅ |
| `A<int*,int**>;` | `Class("A")` | 同左 | ✅ 仅 warning | ✅ ★ 正主 |
| `Box<int>::type;` | 成员类型 | 同左 | ✅ 仅 warning | ✅ |
| `int*;` | `Pointer(Int)` | `Pointer` | ❌ `no name` | ❌ （不回归） |
| `int* const;` | `Const(Pointer(Int))` | `Pointer` | ❌ `expected unqualified-id` | ❌ |
| `int&;` | `LValueRef` | `LValueRef` | ❌ | ❌ |

★ **路径 ③ 的判据必须带 `sawTypeIdSyntax`**：`<...>` 与 `::` 在表达式里不可能出现，
故一旦见到就坐实了"这是类型"。裸 `S;` **不走这条路** —— 它也可能是【同名对象】的
表达式语句，Token 前瞻分不出来，需要查符号表（留待 Sema）。这与 `[stmt.ambig]` 是
同族问题：**能当声明就当声明**，而"能不能当"要靠名字查找。

**下游三处必须同步补**（漏一处就是静默丢语句或运行期崩溃）：

| 位置 | 漏了会怎样 |
|---|---|
| `src/template_instantiation.cpp` `cloneStmt` | 落到 `default: return nullptr` ⇒ 实例化方法体里混进空指针节点 |
| `src/main.cpp` `AstDumper::visit` | `--dump-ast` 对该节点无输出 |
| `tests/unit/test_ast_visitor.cpp` `auditStmt` | 该测试的 `default:` 分支报"未覆盖的语句 kind" |

**回归**：

- `tests/unit/test_declaratorless_decl.cpp`（`DeclaratorlessDecl.*` 8 例）——
  含三条**不变量**断言，而非具体实现细节：
  ① `DoesNotInstantiateAnyTemplate`：裸声明**零实例化**（日志里既无 `[spec:select]`
     也无 `A_intP_intPP`），且同文件的变量声明版**必须实例化**作对照；
  ② `EmptyStmtSurvivesInstantiationClone`：实例化方法体里**不得出现空指针节点**、
     空语句**条数守恒**；
  ③ `PointerDeclaratorIsStillRejected`：三条 ptr-operator 形态**必须继续报错**。
- `tests/lang/test_basics_02_declaratorless.cpp`（集成级）——
  偏特化体里埋 `typename T::nope boom;` 作**实例化探针**（★ 必须是**依赖名**：
  非依赖名在两阶段查找的第一阶段就被查了，clang 定义期即报错，探针失效）。
  裸语句 ⇒ 两边都编过；改成变量声明 ⇒ 两边都报错（clang: `type 'int *' cannot
  be used prior to '::'`；minicc: `no type named 'nope' in 'int*'`）。

**负向验证**（四条改动逐个突变，全部变红）：

| 突变 | 单测 | 集成 |
|---|---|---|
| M1 拆掉 `!isPointer() && !isReference()` 守卫 | `PointerDeclaratorIsStillRejected` ✗ | — |
| M2 拆掉类型关键字分支的 `;` 放行 | `TypeKeywordWithout…` ✗ | 编译失败 ✗ |
| M3 拆掉标识符前瞻分支 | `TemplateId…` / `QualifiedTypeId…` / `DoesNotInstantiate…` ✗ | 编译失败 ✗ |
| M4 拆掉 `cloneStmt` 的 `case NodeKind::Empty` | `EmptyStmtSurvivesInstantiationClone` ✗ | 编译失败 ✗ |

**残留（本轮未覆盖，与本 bug 同族）**

- `S;`（类名 / 任何裸标识符开头的无声明符声明）：minicc 仍按**表达式语句**处理，
  clang 按**声明**。当前两者都无副作用，但分类不同 —— 要正确定性需要 Parser
  有符号表，或引入"声明/表达式待定"的中间节点。与 `[stmt.ambig]` 的完整裁决
  （`a < b > c;`）是同一件事，见 docs/ROADMAP 主线 C。
- `T;`（模板形参作为裸类型名）同理：定义期无法判定 `T` 是类型还是值。

**影响面**：小-中（原先拒收合法程序；修复后 110 个既有集成用例**零漂移**，
仅新增用例使基线数量 110 → 111、单测 249 → 257）。

**状态**：✅ 已修

---

## B17. 值位实参要求「形态精确相等」⇒ 拒收合法程序

- severity: high ｜ status: fixed ｜ related: docs/learn/23, docs/learn/35, tests/tmpl/test_tmpl_64_nttp_integral_conversion.cpp, tests/tmpl/test_tmpl_68_nttp_auto_param.cpp, tests/unit/test_nttp_type_domain.cpp

**现象**（三形态，只有"形态恰好等于形参类型"的那一种活）

```cpp
template<bool B> struct Flag { int v; };
template<int  N> struct A    { int v; };

int main() {
    Flag<1>  f;   // int 1  ⇒ bool ：本应合法
    A<4L>    a;   // long 4 ⇒ int  ：本应合法
    A<true>  b;   // bool   ⇒ int  ：本应合法
    return 0;
}
```

minicc：三条全报 `non-type template argument … cannot be narrowed to type '…'`（rc=1）
clang++-18 `-std=c++20`：三条**全过**，零诊断。

**性质**：**错误拒绝合法程序** —— 与 [B9](#b9-struct-的默认继承级别被当成-private-处理) 同类
（B9 拒的是 `struct D : Base` 的默认继承级别，这条拒的是合法实参转换）。

**根因**：判据选错了。旧版第三道检查写的是

```cpp
else if (!a.valueType->equals(p.nonType)) { error("… cannot be narrowed …"); }
```

即要求"实参的字面量形态与形参类型**精确相等**"。标准要的不是这个：

| 标准位置 | 说的是什么 |
|---|---|
| [temp.arg.nontype]/1 | 实参须是形参类型的 **converted constant expression** |
| [expr.const]/10 | 该术语的定义直接引用 [dcl.init]/7 的初始化规则 |
| [dcl.init]/7 | 窄化禁令，但**常量表达式豁免"值恰好装得下"的那部分** |

⇒ 正确判据是**可表示性**（值装不装得下），不是**形态相等**。三条合法用例恰好各踩一个面：

| 用法 | 标准依据 | 形态 | 值 |
|---|---|---|---|
| `Flag<1>` | [conv.bool]：int → bool，1 ⇒ true | int ≠ bool | 1 ∈ {0,1} ✅ |
| `A<4L>` | [conv.integral]：long → int | long ≠ int | 4 ∈ int ✅ |
| `A<true>` | [conv.prom]：bool → int 整型提升 | bool ≠ int | 恒不窄化 ✅ |

**为什么"碰巧拒对了"掩盖了它**：`F<2>`（2 → bool）这类**真该拒**的用例旧版也拒，
于是负向测试全绿 —— 只有拿正向用例去撞才会露馅。这正是
「负向测试全绿 ≠ 判据正确」的又一例（与 [B10](#b10-带参成员方法的调用符号拼不出来) 的
"错误停在链接期、编译期全程绿灯"是同一类盲区）。

**修法**：判据换成 `Type::canRepresentValue(v)`（[dcl.init]/7 + [expr.const]/10 的合体）：

```cpp
else if (p.nonType && !p.nonType->canRepresentValue(a.value)) {
    // clang 原文：non-type template argument evaluates to N, which cannot be
    // narrowed to type 'T' [-Wc++11-narrowing]（本项目按错误处理）
    error("non-type template argument evaluates to {}, which cannot be narrowed to type '{}'");
}
else if (!a.valueType->equals(p.nonType)) {
    // ③-c 形态【归一】：把实参形态改写成形参类型
    a.valueType = p.nonType;
}
```

★ 两条设计约束：

1. **判据单点**：`canRepresentValue` 定义在 `src/type.cpp`，被 semantic_analyzer
   与 `src/main.cpp` 的 Phase 4 演示路径共用 —— 不允许在调用点再写一份
   （承 B10 / B12 的教训："同一判据不许写两份"）。
2. **归一不是修饰而是语义必需**：归一之后 `Buf<4L>` 与 `Buf<4>` 的可读串 / 缓存键 /
   mangling 才会一致。clang 也认它们是同一实例（`template struct A<4>;
   template struct A<4L>;` 报 duplicate explicit instantiation）。

**同批牵出的第二个 bug（同源，也是"字符串当键"）**：
`template<auto V>` 下 `K<4>`（int）与 `K<4L>`（long）是**两个实例**，
但实例名 / 缓存键此前由 `TemplateArg::toString()` 生成 —— 它对两者都产 `"4"`，
于是第二个**静默复用**第一个实例。既没有报错，也没有算错（值恰好都是 4），
**连"看起来不对"的症状都没有**。修法：键与实例名同源于
`NameMangler::losslessArgumentsKey` / `renderArgLossless`，形态只在 `auto` 形参位写入
⇒ `K_4Cint` / `K_4Clong`（`C` 是 `:` 的安全转义，`sanitizeSymbolChars` 同批补的）。

**教训**

★ 这与 [B13~B15](#小结-字符串兼任-id-与路径) 和 docs/learn/23 的坑**同源**：
**拿"给人看的字符串"当"机器用的键"，早晚出事。**

**影响面**：中。
① 拒收合法程序（正向用例）；
② 静默撞键（`auto` 形参下的实例复用）—— 后者无声无息，更难发现。

**修复记录**：`src/semantic_analyzer.cpp` checkTemplateArguments ③-b/③-c；
`Type::canRepresentValue` 新增于 `include/type.h` + `src/type.cpp`；
`NameMangler::losslessArgumentsKey` / `renderArgLossless` 新增于
`include/template_instantiation.h` + `src/template_instantiation.cpp`。

**回归**：
· 集成 `tests/tmpl/test_tmpl_64_nttp_integral_conversion.cpp`（三条正向，文件头注明回归 B17）
· 集成 `tests/tmpl/test_tmpl_65/66/67_error_nttp_*`（三条负向，文案与 clang 逐字同）
· 集成 `tests/tmpl/test_tmpl_68_nttp_auto_param.cpp`（`K<4>` / `K<4L>` 各自成实例 + 全特化只吃 int 形态）
· 单测 `tests/unit/test_nttp_type_domain.cpp`（`NttpTypeDomain.*` 10 例）

**突变负向验证**（每条判据删掉后必须变红，已逐条实跑）：

| 突变 | 红掉的用例 |
|---|---|
| `canRepresentValue` 的无符号分支改成 `return true` | `CanRepresentValueBoundaries` + `NarrowingIsRejectedWithClangWording` |
| 判据退回 `a.valueType->equals(p.nonType)` | `IntegralConversionIsAcceptedWhenValueFits` + `ConversionNormalizesArgumentFormToOneInstance` |
| 缓存键退回 `args[i].toString()` | `AutoParamDistinguishesArgumentForms` |
| 字符字面量日志不转义 | `CharLiteralLogStaysOnOneLine` |

**状态**：✅ 已修

---

## B18. 类模板里的成员模板在实例类里丢失

- severity: medium ｜ status: fixed ｜ related: docs/learn/34, tests/tmpl/test_tmpl_69_member_template_in_class_template.cpp, tests/unit/test_member_template_in_class_template.cpp

**现象**

```cpp
template <class T>
struct Box {
    template <class U>
    U pick(U x) { return x; }
};
int main() { Box<int> b; return b.pick(7); }
```

minicc：`[Semantic Error] 3:34: No member 'pick' in class 'Box_int'`（rc=1）
clang++-18 `-std=c++20`：正常，rc=7。

**对照组**（把缺口钉死在同一处）：同样的成员模板放在**普通类**里（
`struct Acc { template<class T> T add(T x); };`，见 tests/tmpl/test_tmpl_56）
一路正常。所以缺的不是"成员模板"这个特性，而是**"类本身也是模板"时多出来的那一跳**。

**性质**：**错误拒绝合法程序**（与 [B9](#b9-struct-的默认继承级别被当成-private-处理)、
[B17](#b17-值位实参要求形态精确相等-拒收合法程序) 同类）。★ 症状停在**语义期**，
而解析期一切正常 —— 日志里明明白白印着
`[parse:member] ★ 'pick' 是成员模板（1 个模板形参，[temp.mem]）`，
"解析对了"不等于"这条特性通了"。

**根因**：两层模板形参的绑定时机不同，而"类实例化"这一步只搬了一半东西。

| 层 | 谁绑定 | 何时绑定 |
|---|---|---|
| 外层 `T` | 类实例化（`Box<int>`） | 建实例类那一刻 |
| 内层 `U` | 调用点实参推导（`b.pick(7)`） | 每次调用 |

于是"类实例化"必须产出：把蓝图里的成员模板**复制一份、只替换外层形参、保留内层形参**，
挂到实例类名下。旧版三个环节恰好都缺：

1. `instantiateClassTemplate`（src/template_instantiation.cpp）克隆了字段（步骤 4）、
   方法（步骤 5）、类内类型别名（步骤 5.5），**唯独没有 `memberTemplates`**；
2. `src/semantic_analyzer.cpp:1822` 那张成员模板登记表挂在 `processClassDecl` 里
   —— 而类模板蓝图在 Pass 1 只登记模板（`↳ class template 'Box' registered`），
   **从不经过 `processClassDecl`** ⇒ 蓝图那张表从来没进过注册表；
3. 调用点 `inferCall` 查的键是**对象的类名**（src/semantic_analyzer.cpp:3375
   `m_classMemberTemplates.find(clsName)`）⇒ `"Box_int"` 查不到 ⇒ No member。

**修法**：`instantiateClassTemplate` 新增步骤 **5.6 克隆成员模板**（
`mt` 取自 `templateDecl->classTemplate` —— 本次**命中**的那份蓝图，主模板 / 偏特化 /
全特化一视同仁）：

```cpp
for (auto& mt : templateDecl->classTemplate->memberTemplates) {
    auto clonedMt = std::make_shared<TemplateDecl>();
    clonedMt->templateParams = mt->templateParams;   // 内层形参 U 原样带过
    clonedMt->typeParams     = mt->typeParams;
    clonedMt->isMemberTemplate = true;
    clonedMt->funcTemplate   = cloneMethod(mt->funcTemplate, subst, instanceName);
    newClass->memberTemplates.push_back(clonedMt);
}
```

★ 关键在 `subst` 里**只有外层形参**（`{T → int}`）：`substituteType` 的 Case 1
对不在表中的名字走"原样保留"分支（src/template_instantiation.cpp 的注释早已写明
"如外层模板的形参"，这次是它第一次真正派上用场）。
实跑日志即证据：`[clone:method] 'convert' return type: T → int` 与
`[clone:method] param 'x' : U → U` **同一行输出里两种命运**。

对照 clang：`SemaTemplateInstantiateDecl.cpp` 的 `InstantiateDecl` 对 `TemplateDecl`
走 `TransformTemplateDecl` —— 同样是"带着类的实参重建一遍、内层形参保持未绑定"。

**三种错法各有各的症状**（本条的判据表）：

| 做法 | 症状 | 严重性 |
|---|---|---|
| 忘了搬（旧版） | `No member 'pick' in class 'Box_int'` | 错误**拒绝合法程序** |
| 搬了、但替换**就地改蓝图** | 第二个实例串到第一个的绑定（`Box<long>` 的 `convert` 也返回 `int`） | **静默算错** |
| 把内层 `U` 也替换掉 | 调用点无可推导 ⇒ 每个调用都推不出来 | 错误拒绝 |

**教训**

**影响面**：小-中。原先拒收合法程序；修复后既有 **111 个集成用例逐字节零漂移**
（只有新增用例进基线）—— 反过来证明**此前没有任何既有用例覆盖"类模板 × 成员模板"
这个组合**：一个真缺口可以长时间藏在"特性各自都有测试"的缝里。

**修复记录**：`src/template_instantiation.cpp` instantiateClassTemplate 步骤 5.6
（+ 日志 `║ ── Member Template Substitution ──`，仅在该表非空时打印 ⇒ 既有用例零漂移）。

**回归**：
· 集成 `tests/tmpl/test_tmpl_69_member_template_in_class_template.cpp`
  （外层 T 兑现 / 两实例不串味 / 内层 U 两实例两符号 / 偏特化蓝图同样生效）
· 单测 `tests/unit/test_member_template_in_class_template.cpp`（`MemberTemplateInClassTemplate.*` 5 例）

**突变负向验证**（把步骤 5.6 的循环整体停掉后实跑）：

| 突变 | 红掉的用例 |
|---|---|
| `for (auto& mt : templateDecl->classTemplate->memberTemplates)` 换成空容器 | 本套件 **5/5 全红**（`InstanceCarriesMemberTemplates` / `OuterBoundInnerKept` / `InstancesDoNotShareBlueprintState` / `DistinctInnerArgsDistinctSymbols` / `EveryCallResolves`） |

**状态**：✅ 已修

---

## B19. static / virtual 的识别位置写反：标准写法被拒、非法写法被收

- severity: medium ｜ status: fixed ｜ related: docs/learn/34, tests/tmpl/test_tmpl_70_member_template_static.cpp, tests/tmpl/test_tmpl_71_error_virtual_member_template.cpp

**现象**（两种顺序，判据正好与语言相反）

```cpp
struct O {
    template <class U> static U f(U x) { return x; }   // ① 标准写法（[temp.pre]：template-head 在最前）
    static template <class U> U g(U x) { return x; }   // ② 非法写法
};
```

| | 写法 ①（合法） | 写法 ②（非法） |
|---|---|---|
| minicc（修前） | `[Parse Error] … at 'static': Expected type name`（rc=1） | 解析放行（`isStatic` 已被吃掉），一路走到调用点 |
| clang++-18 `-std=c++20` | rc=0 | `error: expected member name or ';' after declaration specifiers` |

**性质**：同一处判据的**两个方向都反了** —— 拒收合法程序（①）+ 接受非法程序（②）。
① 与 [B9](#b9-struct-的默认继承级别被当成-private-处理)、[B17](#b17-值位实参要求形态精确相等-拒收合法程序)、
[B18](#b18-类模板里的成员模板在实例类里丢失) 同类；② 属"静默接受"那一类。

**根因**：`static` / `virtual` 的识别写在类体循环里**成员模板分支之前**
（src/parser.cpp 的 1641 起，注释还专门交代了"位置必须在成员模板之前"）——
于是它只能认到**说明符写在 `template` 之前**的顺序。而
[temp.pre] 规定 `template-head` 必须在声明最前，说明符只能写在**形参表之后**：
标准写法在 1666 的分支进门时 `static` 还没出现，吃完 `template <…>` 后代码
**不再前瞻**，直接 `parseMethodDecl`（`parseFunctionDecl` 的第一步是 `parseType()`）
⇒ 撞在 `static` 上 ⇒ `Expected type name`。

**教训**

★ 原注释的理由（"`isStatic`/`isVirtual` 必须先于任何使用它们的成员分支声明"）
说的是**变量声明顺序**，但实现把它与**语法位置**绑在了一起 ——
"先声明"与"先出现"是两件事。对照 clang：`ParseTemplateDeclarationOrSpecialization`
先把整条声明解析完（decl-specifier-seq 天然含 static/virtual），再按结果定 Decl 种类
—— **判据来自声明本身，而不是位置**。

**修法**：在形参表解析完、`parseMethodDecl` 之前**再认一次**，且两种说明符**处置相反**：

```cpp
bool mtIsStatic = isStatic;                     // 前缀位置已吃过 ⇒ 兼容既有写法
if (match(TokenType::KwStatic)) mtIsStatic = true;
if (check(TokenType::KwVirtual) || isVirtual) {
    // 文案与 clang 逐字相同
    error("'virtual' cannot be specified on member function templates");
}
```

| 说明符 | 标准 | 本实现 | 依据 |
|---|---|---|---|
| `static` | 合法 | 收下（`method->isStatic = true`） | [class.static]/2：无隐式 this |
| `virtual` | **非法** | 当场报错 | [temp.mem]/2 末句：member function templates shall not be virtual |

★ virtual 必须拒的理由：虚函数要求"每个动态类型在 vtable 里占一条固定条目"，
而模板实例是**按需产生**的 —— 声明处根本不知道要有几条、更不知道 `U` 有哪些取值。
（本轮**没有**顺手把 `virtual` 也一起"支持"掉，是判据决定的，不是工作量决定的。）

**影响面**：小。① 原先拒收合法程序；② 原先静默接受非法程序。
修复后既有 **112 个集成用例逐字节零漂移**（含 B18 的用例）。

**修复记录**：`src/parser.cpp` 成员模板分支（形参表之后新增 static/virtual 识别）；
同批在该分支补了"**本项目只做成员函数模板**"的代码位置标注（成员类模板 /
成员别名模板 / 静态数据成员模板一律响亮拒收，文案与现状见 docs/learn/34 §3.5）。

**回归**：
· 集成 `tests/tmpl/test_tmpl_70_member_template_static.cpp`
  （普通类 + 类模板（B18 × B19 交叉点）+ 非 static 对照，三者都返回 0）
· 集成 `tests/tmpl/test_tmpl_71_error_virtual_member_template.cpp`
  （rc=1，文案与 clang 逐字相同，报错位置也对上：`27:5`）

**突变负向验证**（逐个拆掉后实跑）：

| 突变 | 红掉的用例 |
|---|---|
| 删掉 `if (match(KwStatic)) mtIsStatic = true;` | `test_tmpl_70` 退回 `[Parse Error] … at 'static'`（rc=1） |
| 删掉 virtual 的 error 分支 | `test_tmpl_71` 不再报错（rc≠1），文案断言失效 |
| 把 `mtIsStatic` 写回 `isStatic` | `test_tmpl_70` 的静态分支失效（同第一条） |

**状态**：✅ 已修

---

## B20. vtable 槽里的符号名与定义点不同源（带参虚函数链接失败 / 次基类假符号）

- severity: high ｜ status: fixed ｜ related: docs/learn/13, docs/learn/17, docs/learn/23, tests/unit/test_vtable_symbols.cpp, tests/mi/test_mi_12_secondary_inherited_slot.cpp, tests/lang/test_basics_03_virtual_with_params.cpp

一句话：**vtable 槽里的名字是"拼"出来的，定义点的名字也是"拼"出来的，两处各拼一遍。**

### 缺陷 a：带参虚函数的槽少了 `_<形参个数>` 后缀

**现象**

```cpp
struct A { virtual int f(int x) { return x; } };
struct B : A { int f(int x) { return x + 1; } };
int main() { B b; A* pa = &b; return pa->f(3) - 4; }
```

| | 结果 |
|---|---|
| clang++-18 `-std=c++20` | rc=0 |
| minicc（修前） | 编译全绿，**链接期** `undefined reference to 'A_f'`（B 那侧 `B_f` 同理） |

汇编证据（修前）：

```text
.globl A_f_1        ← 定义点：带形参 ⇒ 追加 _1
_ZTV1A:
    .quad A_f       ← 槽位：硬拼"类名_方法名"，没有 _1 ⇒ 这个符号没人定义
```

**根因**：同一条命名规则写在两处 —— 定义点 `registerFunction`（Pass 2，写
`decl->mangledName`）与 vtable 条目点 `processClassDecl`（Pass 1，写
`entry.mangledName`，三个落点：主表覆写 / 次表覆写 / 新条目）。后者只会
`decl->name + "_" + methodNameInVTable`。**为什么当时能"跑"**：无参虚函数两侧
算出的都是 `A_f`，规则恰好重合；缺口只在带参虚函数上，而此前没有任何用例写过它。

### 缺陷 b：次基类里【未覆写】的槽被重造出一个假符号

**现象**

```cpp
struct A { virtual int f() { return 1; } };
struct B : A {};                       // 次基类：没覆写 f
struct C { virtual int g() { return 2; } };
struct D : C, B {};
int main() { D d; B* pb = &d; return pb->f() - 1; }
```

| | 结果 |
|---|---|
| clang++-18 `-std=c++20` | rc=0 |
| minicc（修前） | 编译全绿，链接期 `undefined reference to 'B_f'` |

收集次基类条目时名字被按 `次基类名 + "_" + 裸名` **重造**了一遍（原意："这条槽
属于 B，那就该叫 B_f"）。但 B 没覆写 f，槽里原本指向的是**更上游**的 A::f ——
重造等于凭空捏造一个没人定义的符号。**名字是路径（谁提供的实现），不是位置
（这是谁的表）** —— 承 [B13](#b13-两个次基类各有一个同名字段时显示名塌陷且第二条偏移算成-0)~[B15](#b15-祖辈前缀与直接基类撞名时偏移算到错的子对象) 与
[docs/learn/23](learn/23-cv-position-and-type-identity.md) 的同一句教训。

**现场**：这个缺陷一直藏在 `tests/mi/test_mi_04_error.cpp` 里。该文件当时写的是
菱形继承，头注释把 rc=1 解释成"菱形本该被拒收"—— 实测 clang rc=0：非虚继承的
菱形在标准下**合法**（两个 X 子对象，只有成员访问歧义才报错）。rc=1 的真原因就是
这句 `undefined reference to 'Q_f'`。**又一次印证：logdiff 基线会把失败固化成契约**。

**修法**（两处，方向相反）：

1. **判据单点**：抽 `memberMethodSymbolName(owner, name, paramCount, earlierSameNameCount)`
   （src/semantic_analyzer.cpp:1445），`registerFunction` 与 vtable 三个落点
   **共用同一个函数**；
2. **次表名字原样透传**：`VTableEntry secEntry = baseEntry;`，不再重造。
   真覆写由后面的方法循环负责（那时才改指本类实现并配 thunk）。

★ 计数口径有个坑：`earlierSameNameCount` 只数**排在当前方法之前**的同名方法
（Pass 2 注册时扫 `m_functions` 也只看得见前者）。数成"同类同名总数"会让
`struct X { virtual int f(); virtual int f(int); }` 里 f() 得到 `X_f_0`，而定义点是
`X_f` —— 又是一个只在链接期炸的错配。故 vtable 侧按 `decl->methods` 的**下标顺序**数。

**顺带发现（缺陷 c，已修）**：槽位匹配只比**裸名**、不比形参表 ⇒
`class C { virtual int f(); int f(int); };` 里 `f(int)` 会认领 `f()` 的槽，且被
**误标成 virtual**。实测 `C c; return c.f() + c.f(2) - 3;`：clang rc=0，
minicc 编译 rc=0 但**运行返回 255**（静默算错）。

**缺陷 c 的修法**：槽位身份补上**形参类型链**（`VTableEntry::signature`，与符号名共用
`paramTypeChain`）。判据从"裸名"变成"裸名 + 形参类型" —— 即 `[class.virtual]/2` 的覆写判据
（裸名只是查找入口，不是身份的全体）。同轮的 [B22](#b22-同名同个数的成员重载sema-静默取第一个汇编期撞符号)
把同一条判据推进到了另外三处（符号定名 / 调用点选择 / 构造函数选定），详见该条。

**教训**

**这个缺陷证明了"对称不变量"不够用**：认错槽位后"谁在槽里、谁被直接调"整体互换，
`|槽位| + |直接调用| == |定义|` 这类计数照样配平 —— 有判别力的那条是
"经基类指针的虚调用必须退化成 `callq *%rax` 的**间接**调用"（详见 B22 的回归用例 ⑤）。

### 回归与影响面

| 项 | 结果 |
|---|---|
| 既有集成用例漂移 | **恰好 1 个**：`tests/mi/test_mi_04_error.cpp`（缺陷 b 的现场，rc 1→0，已改写为私有继承错误用例）；其余 115 个逐字节不变 |
| 单测 | 262 → 266（新增 `VTableSymbols.*` 4 例） |
| 集成 | 114 → 116（新增 `tests/mi/test_mi_12_secondary_inherited_slot.cpp`、`tests/lang/test_basics_03_virtual_with_params.cpp`） |
| 有意日志漂移 | 无（命名结果对无参虚函数逐字未变） |

**回归**：

- 单测 `tests/unit/test_vtable_symbols.cpp`（`VTableSymbols.*` 4 例，断言**不变量**：
  每条 `.quad <sym>`（`.L*`/纯数字除外）都必须有 `.globl <sym>` 定义 ——
  命名规则随便改都不会误报，但"两处各算一遍"必红）
- 集成 `tests/mi/test_mi_12_secondary_inherited_slot.cpp`（菱形次表透传 + 次基类自身覆写走 thunk）
- 集成 `tests/lang/test_basics_03_virtual_with_params.cpp`（带参虚函数覆写 / 未覆写 / 基类自身三条路）

**突变负向验证**（逐个拆掉后实跑）：

| 突变 | 红掉的用例 |
|---|---|
| 三个 vtable 落点改回 `decl->name + "_" + methodNameInVTable` | `VTableSymbols.ParameterizedVirtualSlotMatchesDefinition`、`...SecondaryInheritedSlotKeepsUpstreamSymbol` |
| 次表条目改回 `baseName + "_" + 裸名` | 只有 `...SecondaryInheritedSlotKeepsUpstreamSymbol` 红（外科级定位） |

**状态**：✅ 缺陷 a、b、c 均已修（c 与 [B22](#b22-同名同个数的成员重载sema-静默取第一个汇编期撞符号) 同轮完成）

---

## B21. 限定名 `S::v` 访问成员数据被拒收

- severity: medium ｜ status: fixed ｜ related: docs/learn/36, tests/lang/test_basics_09_qualified_member.cpp

**现象**（同一个类里两种限定名，一个通一个不通）

```cpp
struct S {
public:
    int v;
    int f() { return 5; }
    int g() { return S::v; }     // ① 限定名的【数据】成员
    int h() { return S::f(); }   // ② 限定名的【函数】成员
};
```

| | ① `S::v` | ② `S::f()` |
|---|---|---|
| minicc | `[ERROR] [Semantic Error] 5:22: Undefined variable 'S::v'`（rc=1） | rc=0 |
| clang++-18 `-std=c++20` | rc=0（`-Xclang -ast-dump` 见下） | rc=0 |

**性质**：**拒收合法程序**。与 [B9](#b9-struct-的默认继承级别被当成-private-处理)、
[B12](#b12-继承来的成员方法查不到)、[B17](#b17-值位实参要求形态精确相等-拒收合法程序)、
[B18](#b18-类模板里的成员模板在实例类里丢失) 同类。（② 能过是**侥幸**：inferCall 有一条
限定名分支认 `类名_方法名` 的 mangled 符号，与"成员访问"无关。）

**clang 的落点（oracle）**：`S::v` 在成员函数体内是**隐式 this 的成员访问** ——
[expr.prim.id.general]/3，AST 里就是一个正牌 MemberExpr：

```text
`-ReturnStmt
    `-MemberExpr  'int' lvalue ->v        ← 注意是 ->v，base 是隐式 this
        `-CXXThisExpr  'S *' implicit this
```

**根因**：`A::B` 在 Parser 就被**拼成一整个字符串**（parser.cpp:2679 的
`while (::) name += "::" + ident`），此后全项目只按"名字串"查表。而三条限定名通路
覆盖的分别是 **函数**（inferCall 的限定名分支）、**类内类型别名**（符号表 `Cls::alias`）、
**嵌套类型名**（resolveType 的 nested-name 分支）—— **数据成员一条都没有**：
inferVar 只认裸字段名（外加"当前类"的类作用域回退），查 `"S::v"` 这个字符串必然落空。

★ 有意思的是**方向正好错开**：本项目唯一从 `NodeKind::Member` 分支过的限定名是
`Cls<int>::value`（Parser 造 `isTypeAccess=true` 的 MemberExpr，供 `foldStaticConst`
折叠静态常量）—— 而它在 clang 里是 **DeclRefExpr**，不是成员表达式。也就是说
"clang 里是 MemberExpr 的形态我们收不到，我们造 MemberExpr 的形态 clang 不是"。

**修法（要修的话）**：inferVar 里先把 `A::B` 按 `::` 切开，若限定者是**当前类**（或落在其基类链上）
则退化为"字段查找 + 隐式 this"，与 `b`（裸名走 m_currentClassName 回退）汇成同一条路；
`self()` 那条 `this->b` 已经能跑，收口点是现成的。
判据要按 [expr.prim.id.general]/3 写：**限定者是不是一个类型** 决定这是不是隐式成员访问
（命名空间限定名仍走符号表）。

**影响面**：小（写法少见，且只是**多写**了一个限定者；去掉 `S::` 就恢复正常）。
但属"拒收合法程序"，与 B12 同族 —— 记在这里是因为**推导分支表的边界**值得留痕。

**回归**：暂**不加**钉住现状的用例 —— 那会把失败固化成契约
（正是 logdiff 教训：基线会把 rc=1 也固化成"契约"）。修完再补正例。

**状态**：✅ 已修（2026-10 那一轮，与 B24/B25/B26/B27 同批）

---

### B21 修法

**改动面**（两半，缺一不可）

| # | 缺口 | 改动点 |
|---|---|---|
| a | 语义层没有通路：`inferVar` 只认裸字段名 | `inferVar` 新增步骤 0：`Q::m` 切开 ⇒限定者是当前类或基类 ⇒ 走**同一条**字段查找路径 |
| b | 语法层压根不认 `obj.Q::m` | `Parser::parsePostfixExpr` 的 `.`/`->` 分支收 `IDENT (:: IDENT)*`，拼成成员名字串（与既有 `A::B` 处置一致） |
| c | ★ **偏移会取错**：按裸名查命中派生类隐藏字段那条 | `resolveQualifiedField`（判据单点）+ 把权威偏移**回填**到 `VarExpr`/`MemberExpr`，CodeGen 优先采用 |

c 是本次修复的**核心判据**：写限定名的唯一用途就是绕过名字隐藏，
而 `findField` 的三级顺序里"自身字段优先"（[class.member.lookup]/3）恰好会
挑中派生类那一条 —— 不改这一处，程序能编译、能运行、**结果错**。

**教训**

同 B10 的符号回填、B13~B15 的 `declaredName`/`viaBase`：**名字是给人看的，位置才是机器要的**。

`resolveQualifiedField(ownerClass, qualifier, member)` 的"归不归 Q 管"三个条件：
① `sourceClass == qualifier`（继承自它；主基类合并时 `sourceClass` 会被补成基类名）
② `sourceClass` 为空且 `qualifier == ownerClass`（本类自己的字段）
③ `qualifier` 是 `sourceClass` 的祖辈（字段声明在 Q，经若干层才带到本类布局）
—— 三条都来自 `computeClassLayout` 扁平化的既有语义，与 B13~B15 是同一张表。

**实测**（`Base::v` 与裸名 `v` 落在**不同**槽位 —— 这条不变量就是 c 的判据）

```text
  [resolve] 'Base::v' → int    (qualified member, offset=0 隐式 this)
  [resolve] 'v' → int    (class field, offset=8)
    [member] Derived.Base::v → int    (qualified member, offset=0)
    [member] Derived.v → int    (offset=8, size=4)
```

```asm
    movl 0(%rax), %eax    # 读取限定名成员 .v（偏移 +0 字节）
    movl 8(%rax), %eax    # 读取字段 .v（偏移 +8 字节）
```

**回归**：`tests/lang/test_basics_09_qualified_member.cpp`（正例，类内 / 类外 /
写目标三种形态 + 隐藏场景，运行 rc=0）+ 单测 `ArgBinding.QualifiedMemberPicksTheBaseSubobject`
（断言"两条偏移必须不同且基类在前"，**突变验证**：去掉回填 ⇒ 该例当场变红）。
文档：docs/learn/36 §4（clang 的 AST 对照见 §4.2）。

**已知边界**（同族未做）：限定名调用**成员函数** `d.Base::f(2)` 仍报 `No member 'Base::f'`
（B21 只管数据成员）；`using Base::f;` 不支持。见 docs/learn/36 §7。

---

## B22. 同名同个数的成员重载：Sema 静默取第一个、汇编期撞符号

- severity: high ｜ status: fixed ｜ related: docs/learn/13, docs/learn/36, tests/unit/test_member_identity.cpp, tests/lang/test_basics_04_member_overload_same_arity.cpp, tests/mi/test_mi_13_virtual_overload_slots.cpp

**现象**（一个类里两个同名、同个数、不同类型的成员方法）

```cpp
struct S { public: int v; };
struct C { public: int f(int x) { return 1; } int f(S s) { return 2; } };
int main() { C c; S s; s.v = 0; return c.f(1) + c.f(s) - 3; }   // clang: rc=0
```

| 情形 | 源码 | minicc | clang++-18 `-std=c++20` |
|---|---|---|---|
| 同名、**个数不同** | `f(int)` + `f(int,int)` | ✅ rc=0 | ✅ rc=0 |
| 同名、**个数相同**、类型不同 | `f(int)` + `f(S)` | ❌ ``minicc_o4.s:43: Error: symbol `C_f_1' is already defined`` ⇒ `[ERROR] 汇编失败 (as 退出码 256)` | ✅ rc=0 |
| 同上，声明顺序对调 | `f(S)` + `f(int)` | ❌ 同上（与顺序无关） | ✅ rc=0 |

**性质**：**拒收合法程序**，且**报错点离根因很远** —— as 只甩一个符号名，不说
"这个类里有两个同名同个数的 `f`"。

**根因**：**"名字 + 参数个数"被当成了身份**，两处各用一次（正是 B10 那类"同一判据写在两条通路上"）。

| # | 位置 | 判据 | 后果 |
|---|---|---|---|
| ① | `findMethodInClass`（`src/semantic_analyzer.cpp:4558`） | `name` 相同 ∧ `parameters.size()` 相同 ⇒ `return method`（**在循环内**） | 同分者**声明序第一个赢**，全程不比较类型 |
| ② | `memberMethodSymbolName`（`src/semantic_analyzer.cpp:1445`） | `sym = owner_name`，后缀**只编码 paramCount**（`earlierSameNameCount` 只决定"要不要加后缀"，不进后缀内容） | 同名同个数 ⇒ **生成同一个符号** `C_f_1` |

调用点（inferCall:3439）传的就是 `argTypes.size()`。于是 ② 保证"两个都定义必然撞名"，
① 保证"若只有一处定义则会静默选错" —— **前者让后者至今没机会暴露**。

**标准视角**：`[class.member.lookup]` 只产出**候选声明集**（不筛类型）；筛类型是
`[overload.resolve]` 的事：候选集 → **可行集**（[overload.best.viable]：个数 + 隐式转换序列）
→ 最优（[overload.icp] 排序）。本项目四条通路的判据粗细度：

| 通路 | 判据 | 粗细 |
|---|---|---|
| 函数模板 | 推导 + 偏序（[temp.func.order]） | 最细 |
| 自由函数（含 ADL） | 候选合并 + 精确匹配裁决 | 中 |
| **成员方法** | **名字 + 个数** | 粗（本条，已修） |
| vtable 槽认领 | **只有裸名** | 最粗（[B20](#b20-vtable-槽里的符号名与定义点不同源带参虚函数链接失败--次基类假符号) 缺陷 c，**已是静默算错**，已修） |

### 修法：一条判据，四处落点

"身份 = 名字 + 形参类型"这条判据在这份实现里有**四个落点**，本轮把它们全部收口：

| # | 落点 | 修前 | 修后 |
|---|---|---|---|
| ① | 汇编符号名 `memberMethodSymbolName`（src/semantic_analyzer.cpp:1445） | 后缀只编码 `paramCount` | 同签名个数有兄弟时再挂 `paramTypeChain`（`C_f_1_int` / `C_f_1_S`），**无兄弟则一字不改** |
| ② | vtable 槽位身份 `processClassDecl` | `entryFuncName == methodNameInVTable`（裸名） | 再与 `entry.signature`（形参类型链）比对 —— B20 缺陷 c |
| ③ | 调用点选择 `findMethodInClass` | 循环内首个命中即 `return` | 收**候选集** → `pickBestByArgs` 三级择优（精确 → 可隐式转换 → 退回首个），并回填 `resolvedCalleeSymbol` |
| ④ | 构造函数选定 `processVarDeclStmt` | **只数个数**，且把刚推出来的实参类型**扔掉** | 同走 `pickBestByArgs`（`[ctor] … ⇒ 选定构造函数符号 …`） |

★ ③④ 共用的 `pickBestByArgs(candidates, argTypes, what)` 是一个**本地静态函数** ——
这正是本项目反复交学费的那条：同一判据写在两条通路上，早晚分叉（[B10](#b10-带参成员方法的调用点符号与定义点不一致)、
[B12](#b12-继承来的成员方法调用报-no-member)、B13~B15、[B20](#b20-vtable-槽里的符号名与定义点不同源带参虚函数链接失败--次基类假符号)）。

★ 符号名为什么**只在必要时**才挂类型链：挂了就动了全部既有产物（logdiff 基线）。
实测"有兄弟才挂"策略下，**既有 116 个集成用例逐字节零漂移**。

★ 对照 clang：`_ZN1C1fEi` / `_ZN1C1fE1S` —— 形参类型**无条件**进 mangling，
且声明是 `Decl*` 句柄（查找/匹配/发射天然同一个东西）。minicc 没有句柄，
只能用"字符串做键"，所以必须靠**收口到同一个函数**来达到同等效果。

### 回归与影响面

| 项 | 结果 |
|---|---|
| 既有集成用例漂移 | **0**（116 个逐字节不变；新增 2 个是全新文件） |
| 单测 | 266 → 272（新增 `MemberIdentity.*` 6 例） |
| 集成 | 116 → 118（新增 `tests/lang/test_basics_04_member_overload_same_arity.cpp`、`tests/mi/test_mi_13_virtual_overload_slots.cpp`） |
| 有意日志漂移 | 无 |

**回归**：

- 集成 `tests/lang/test_basics_04_member_overload_same_arity.cpp`
  （`f(int)`/`f(S)`、三元混排 `g(int,int)`/`g(int,S)`/`g(S,int)`、构造函数 `D(int)`/`D(S)`；运行 rc=0）
- 集成 `tests/mi/test_mi_13_virtual_overload_slots.cpp`
  （`virtual int f()` + 非虚 `int f(int)` 共存、派生覆写后槽数不变、经基类指针虚调用派发正确）
- 单测 `tests/unit/test_member_identity.cpp`（`MemberIdentity.*` 6 例，断言**不变量**而非具体拼法）

**突变负向验证**（逐个拆掉后实跑，`--gtest_filter='MemberIdentity.*'`）：

| 突变 | 红掉的用例 |
|---|---|
| ① 符号定名回退（`needsTypeTag = false`） | 4 / 6 红（`SameNameSameArityGetDistinctSymbols`、`LoneMethodNameUnchanged`…）集成侧：`test_basics_04` 编译 rc=1 |
| ② 槽位匹配回退到裸名（删 `entry.signature == selfSignature`） | 2 / 6 红（`NonVirtualOverloadDoesNotOccupyVTableSlot`、`VirtualCallOnPointerStaysVirtual`）集成侧：`test_mi_13` 编译 rc=0 但**运行 rc=4** |
| ④ 构造函数选定回退到按个数（`ctors.front()`） | 2 / 6 红 |

**教训**

★ **② 的教训（值得单独记）**：第一版单测只断言了"槽里只有一个符号"和"派生类槽数不变"，
**突变验证时全绿** —— 因为认错槽位后"谁在槽里、谁被直接调"会**整体互换**，计数照样配平。
真正有判别力的是**不对称**的那条：经基类指针的虚调用必须是 `callq *%rax` 间接调用
（认错 ⇒ CodeGen 按 `resolvedCalleeSymbol` 精确比对落空 ⇒ 退化成直接调用 ⇒ 运行期不派发）。
**"守恒式"不变量（两边加起来对得上）挡不住互换型缺陷**，要挑守恒式盯不住的方向再钉一条。

**状态**：✅ 已修（①②③④ 四处落点 + B20 缺陷 c）

---

## B23. 浮点：字面量缺失 + 运算按整数发射

- severity: high ｜ status: open ｜ related: docs/learn/23, docs/learn/35, docs/learn/39

**现象**

```cpp
int main() { double d = 1.5; return 0; }                            // 缺陷 a
int main() { double a = 1; double b = 2; double c = a / b;
             if (c > 0) { return 1; } return 0; }                   // 缺陷 b
```

| | minicc | clang++-18 `-std=c++20` |
|---|---|---|
| a. `double d = 1.5;` | `[ERROR] [Parse Error] 1:27 at '5': Expected member name: expected Identifier, got '5'`（rc=1） | rc=0 |
| a'. `double d = 15e2;` | `[ERROR] [Parse Error] 1:27 at 'e2': Expected ';' after variable declaration`（rc=1） | rc=0 |
| b. `a`=1、`b`=2（都 `double`），判 `a / b > 0` | **rc=0**（`1/2` 按整数算 ⇒ 0，判假） | rc=**1**（0.5 > 0） |

**性质**：**两个不同层的缺陷**。a 是**未实现的词法形态**（响亮失败，但报错文案指向
`.` **后面那个数字**，完全看不出"本实现没有浮点字面量"）；b 是**静默算错**
（编译通过、运行给出错的结果 —— 与 [B14](#b14-派生类同名字段的隐藏方向做反了) /
[B20](#b20-vtable-槽里的符号名与定义点不同源带参虚函数链接失败--次基类假符号) 缺陷 c 同类）。

**根因**：

- **a**：Lexer 只有整数那条路（[lex.icon]：进制前缀 / 后缀 / 数字分隔符，见 docs/learn/35），
  [lex.fcon] 的小数点 / 指数 / 后缀一概没有 ⇒ `1.5` 被切成 `1` `.` `5`，
  而 `.` 在 `parsePostfixExpr` 里是**成员访问** ⇒ `Expected member name`。
- **b**：`src/codegen.cpp` 里搜不到任何 SSE 指令（`movsd`/`addsd`/`xmm` 计数为 **0**），
  也搜不到 `isDouble` —— `double` 在 CodeGen 眼里就是一个**8 字节标量**，
  `a / b` 照发 `idivq`（实测 `.s` 第 27 行）⇒ `1/2 = 0`。

**边界（实测）**：`float` 类型根本没有（`unknown type name 'float'`）；但 `double`
**类型**是通的 —— `double g(double x) { return x + 1; }` + `double d = 1;`（int → double
走 [conv.fpint]）编译运行 rc=0。**缺的是字面量与运算**，不是类型。

**修法（要修的话）**：a 在 Lexer 认小数点/指数/`f` 后缀，Parser 给出 `double`（要不要连 `float`
一起补需先定 CodeGen 的浮点宽度分派）；b 要引入 SSE 寄存器与指令（`movsd`/`addsd`/`divsd`、
调用约定里 xmm0 传参）—— 是**一条独立主线**，不是顺手项。

**状态**：⬜ 未修（a、b 都未修）

**回归**

（无）

**教训**

（无）

---

## 小结 字符串兼任 ID 与路径

三条 bug 的最小复现各不相同，根因却是同一句话：**把"显示名"当成了"索引"。**

| 层 | 它该用来做什么 | 用错之后的后果 |
|---|---|---|
| 显示名 `"B.x"` | 打印给人看 | 看不出前缀是第几代留下的 ⇒ **B15** |
| 查找键 | 必须是**声明名**（`declaredName`） | 反拼前缀去配对 ⇒ **B14** |
| 子对象定位 | 必须是**结构信息**（`viaBase` + 下标） | 靠裸名在基类布局里找第一个命中 ⇒ **B13 / B15** |

这与踩坑史 **T1**（`Type::toString()` 不是单射、而实例缓存键吃它 —— 见
[docs/learn/23](learn/23-cv-qualifier-position.md)）是**同一个坑的另一次现形**；
[B20](#b20-vtable-槽里的符号名与定义点不同源带参虚函数链接失败--次基类假符号) 是**第三次**
（vtable 槽里的函数名 = 机器用的键，而定义点与槽位**各拼一遍**）。
可以概括成一条项目级教训：

> **凡是拿"给人看的字符串"当"机器用的键"，早晚出事。**

clang 那边这两种东西从设计上就是分开的：成员是 `FieldDecl*`，
布局按 `getFieldIndex()` 寻址，名字只交给 `LookupResult` 做查找、
且查找结果自带"来自哪个子对象"（歧义因此天然可检出）。

**顺带记录的两条方法论**：

1. **补第三态**（B11）：`if/else` 写"优化决策"时，两态往往是从旧代码继承来的，
   新情形出现时最容易漏 —— 先枚举出**全部**情形再写分支。
2. **判据只许有一处**（B12，承 B10）：同一句"成员叫什么/是不是它"写在两条通路上，
   就会各自演化。本轮的收口是 `findMethodInClass` / `findMethodInHierarchy` 两个原语。

---

## B24. 成员方法的**名字隐藏**：派生类一声明同名方法，基类整族应被隐藏

- severity: medium ｜ status: fixed ｜ related: docs/learn/36, tests/lang/test_basics_07_member_call_arity_error.cpp, tests/mi/test_mi_13_virtual_overload_slots.cpp

**现象**

**来源**：本轮写 `tests/mi/test_mi_13_virtual_overload_slots.cpp` 时被 clang oracle 当场抓住 ——
用例里原本有一句 `d.f(2)`（`d` 是 `Derived`，`f(int)` 只在 `Base` 里），clang 直接拒收：

```text
error: too many arguments to function call, expected 0, have 1; did you mean 'Base::f'?
```

而 minicc 编译 rc=0、运行也"看着对"（调到了 `Base::f(int)`）。

**标准视角**（[class.member.lookup]/3）：派生类作用域里**只要出现过某个名字**，
基类中**所有**同名声明就被隐藏（`using` 声明可以把它们重新拉进来）。
"隐藏"是按**名字**发生的，不是按签名 —— 所以 `Derived::f()` 一个无参函数，
就足以让 `Base::f(int)`、`Base::f(S)` 全部不可见。

**根因**：`findMethodInHierarchy` 的遍历是"本类找不到**这个名字**才往基类走"，
而"找得到"的判据是**裸名**（`findMethodInClass(clsName, name, -1)`）。
于是它把被隐藏的那一族也捞了出来 —— 又是"身份比裸名宽"的老毛病，
只是这次宽的方向是**该藏没藏**（B20 缺陷 c / B22 是**该分没分**）。

**修法**（未做）：隐藏判据要按名字的**作用域**判，而不是按签名 ——
先查派生类里这个名字有没有**任何**声明（`decl->methods` 里 `name` 相等即算），
有 ⇒ 基类整族不参与候选；没有 ⇒ 才往基类走。
注意这与 B22 的"候选集 + 择优"是**两个正交的筛选**：
隐藏先砍掉一整族（按名字），重载决议再从中择优（按类型）。

**影响面**：小（接受非法程序；不产生错误代码，但把 clang 会拒收的程序放行了）。
**回归**：本轮**不加**（不把失败固化成契约，同 B21/B22 的处理）。
`tests/mi/test_mi_13_*.cpp` 的文件头已记下这条边界与"为什么用例绕开了它"。

**状态**：✅ 已修（2026-10 那一轮，与 B21/B25/B26/B27 同批）

---

### B24 修法

**修法**：不改 `findMethodInHierarchy`（它服务的是字段/方法**访问**那条通路，不看实参），
而是在 `inferCall` 的成员查找 BFS 里补上"**查到名字就下结论**"：

```text
对象类 BFS 每一层：
  ① findMethodInClass(cls, name, 个数, &argTypes)  ── 命中 ⇒ 选定，回填符号，返回
  ② 成员模板表（同名）                              ── 推导成功 ⇒ 实例化，返回
  ③ ★ 本类是否为这个名字声明过任何东西？
       · 是 ⇒ 在这里把话说死：个数区外 ⇒ too few/too many；区内 ⇒ no matching member function
              **不往基类走、也不落进普通查找**（隐藏 + 可行性两条规则同时兑现）
       · 否 ⇒ 把基类压进 BFS 队列，继续
```

★ 位置选在 `inferCall` 而不是 `findMethodInHierarchy`，正是因为**隐藏是"按名字"的筛选**：
`findMethodInHierarchy` 按裸名判"本地有没有"本来就是对的（它只回答"有没有这个名字"），
错的是调用点拿到 `nullptr` 之后**没下结论**、继续往别处找。两件事拆开看就通了 ——
`findMethodInHierarchy`（B12 抽出来的原语）继续服务 `inferMember`，成员调用的
"名字可见性 + 可行性"在 `inferCall` 里一次说清。

**与 B25 的关系**：本条与 B25 的 ① 是**同一处代码**（同一个 BFS 里"落空之后去哪儿"），
方向相反（该藏没藏 / 该报没报），故**同一批修**。B25 的修复见下。

**回归**：`tests/lang/test_basics_07_member_call_arity_error.cpp`（rc=1）
+ 单测 `ArgBinding.NameHidingStopsBaseLookup`（含"不声明同名成员时基类的 f 照常可见"的对照组）。
**突变验证**：把"查到名字就下结论"整块停掉 ⇒ 该例与 `NoViableOverloadIsAnError` 两条齐红。

**顺带修出一个被基线固化的非法程序**：本条修好后，
`tests/unit/test_member_identity.cpp` 的 `DerivedOverrideKeepsSlotCount` **当场变红** ——
那个用例里写着 `d.f(2)`（`f(int)` 只在基类），clang 同样 rc=1，minicc 此前 rc=0 所以"通过"。
已改成经基类指针的 `p->f(2)`，并在文件里写下缘由。

**教训**

★ 这是本项目**第三次**踩"基线把失败固化成契约"（前两次：`tests/mi/test_mi_04_error.cpp`、
`tests/pp/test_pp_01_include.cpp`）。**写用例前先用 clang 过一遍"这程序真的合法吗"。**

---

## B25. 成员调用实参个数不匹配无人认领：一条静默接受、一条只剩链接器

- severity: medium ｜ status: fixed ｜ related: docs/learn/36, tests/lang/test_basics_07_member_call_arity_error.cpp

**来源**：读 `inferCall` 的成员分支时问"3562 那一行到底报不报错"。不报 —— 而且
**这条错全项目没有任何一处报**（本条的四个形态全部实测）。

**现象**

```cpp
// a：少传
struct C { int f(int x) { return x; } };   int main() { C c; return c.f(); }
// b：多传（方法无参 —— 唯一能活到运行期的那条）
struct C { int f() { return 1; } };        int main() { C c; return c.f(1, 2); }
// c：多传
struct C { int f(int x) { return x; } };   int main() { C c; return c.f(1, 2); }
// d：名字不存在（对照组，本来就报）
struct C { int f(int x) { return x; } };   int main() { C c; return c.nope(1); }
```

| 情形 | minicc | clang++-18 `-std=c++20` |
|---|---|---|
| a. 少传：`f(int)` 调 `f()` | ❌ rc=1，但错在**链接期**：`[LINK ERROR] … undefined reference to 'C_f'` | `error: too few arguments to function call, single argument 'x' was not specified` |
| b. 多传：`f()` 调 `f(1, 2)` | ⚠️ **rc=0**，产物照跑（实测退出码 1，两个实参被静默丢弃） | `error: too many arguments to function call, expected 0, have 2` |
| c. 多传：`f(int)` 调 `f(1, 2)` | ❌ rc=1，同 a 的 `undefined reference to 'C_f'` | `error: too many arguments to function call, expected single argument 'x', have 2 arguments` |
| d. 名字不存在：`c.nope(1)` | ✅ rc=1，`[Semantic Error] 2:27: No member 'nope' in class 'C'` | `error: no member named 'nope' in 'C'` |

**性质**：与 [B21](#b21-限定名-sv-访问成员数据被拒收) / [B22](#b22-同名同个数的成员重载sema-静默取第一个汇编期撞符号)
（**拒收合法程序**）方向相反 —— 本条是**该报不报**：b 直接**接受非法程序**
（编译、链接、运行全通，只是语义按"多余实参被丢弃"执行），a/c 虽终被拒，
但报错人是**链接器**，`undefined reference to 'C_f'` 一个字都不提"实参个数不对"。

**根因**：**"个数"在两条通路上分别被看 / 不看，落空之后的那条缝没人认领。**

| # | 位置 | 判据 | 后果 |
|---|---|---|---|
| ① | `inferMember`（`src/semantic_analyzer.cpp:4810`，由 inferCall 先调一次） | 名字（`arity = -1`，**不看个数**） | 按名字命中 ⇒ 不报 `No member` |
| ② | `findMethodInClass`（`src/semantic_analyzer.cpp:3562`，传 `argTypes.size()`） | 名字 + **个数** | 个数不符 ⇒ 候选集空 ⇒ `nullptr` |
| ③ | 落空后继续的通路（3604 起 → 3733） | 自由函数 + ADL + 模板 | 日志自己都写着 `[call] non-template 'f' arg count mismatch (0 vs 2), keep searching` —— **"keep searching" 后面没有终点** |
| ④ | 兜底 `inferType(expr.callee)`（3782） | **又回到 ①** | 第二次按名字通过 ⇒ `[call] f() → int (callee type)` ⇒ 全程零报错 |
| ⑤ | CodeGen（`src/codegen.cpp:1819`） | `resolvedCalleeSymbol` 为空 ⇒ 硬拼 `类名_方法名` | 拼出 `C_f` |

⑤ 就是 [B10](#b10-带参成员方法的调用符号拼不出来) / [B20](#b20-vtable-槽里的符号名与定义点不同源带参虚函数链接失败--次基类假符号)
那个硬拼点。于是 b 与 a/c 的分界变成**某个后缀恰好是不是空串**：
`memberMethodSymbolName`（`src/semantic_analyzer.cpp:1540`）只在 `paramCount > 0` 或前面已有同名者时
追加后缀 ⇒ 0 参方法就叫 `C_f`，硬拼**撞对了** ⇒ 静默通过；1 参方法叫 `C_f_1` ⇒ 撞不上 ⇒
链接期炸。**"静默"和"报错"由一条派生字符串的形状决定** —— 还是那句话：拿给人看的串当身份。

**与 [B24](#b24-成员方法的名字隐藏派生类一声明同名方法基类整族应被隐藏) 的关系**：两条的 ① 是**同一处原语**（`findMethodInHierarchy` 按裸名判
"本地有没有"），但方向相反 —— B24 是**该藏没藏**（派生类一个 `f()` 挡不住基类 `f(int)` 整族），
本条是**该报没报**（名字在、个数不对，没人下结论）。修的时候应一并考虑：
先按名字定"可见集"（含隐藏规则），再按**个数 + 类型**做可行性筛选与择优。

**标准视角**：[expr.call]/1 要求实参个数与形参匹配；[overload.best.viable] 的**第一条**
可行性判据就是个数。本项目把"个数"只当**筛选器**（传进 `findMethodInClass` 过滤），
没当**判据**用 —— 筛空了也推不出"无可行候选"这个结论。对照 clang：
`Sema::CheckFunctionCall` 在 BuildResolvedCallExpr 之前就把 too few / too many 报在**调用点**。

**修法（要修的话）**：② 落空且 `objType` 确为类时收口报错，文案对齐 clang
（`too few/too many arguments`），个数判据与 `memberMethodSymbolName` 同源；
③ 那条 `keep searching` 在候选耗尽时也应有个收尾。

**影响面**：中（b 为**静默接受非法程序**；a/c 的报错点落在链接器，指向不到根因）。
**回归**：暂不加（同 [B21](#b21-限定名-sv-访问成员数据被拒收)，不把失败固化成契约）。

**状态**：✅ 已修（2026-10 那一轮，与 B21/B24/B26/B27 同批）

---

### B25 修法

**修法**：把"个数"从**筛选器**提升为**判据** —— 与 B24 同一处代码（`inferCall` 成员查找 BFS
的"查到名字就下结论"分支），按"本类为这个名字声明过哪些参数个数"的区间 `[lo, hi]` 决定文案：

| 实参个数 | 文案 | 标准依据 |
|---|---|---|
| `have < lo`，`lo == 1` | `too few arguments to function call, single argument 'x' was not specified` | [expr.call]/1 |
| `have < lo` | `too few arguments to function call, expected {lo}, have {have}` | 同上 |
| `have > hi`，`hi == 1` | `too many arguments to function call, expected single argument 'x', have {n} arguments` | 同上 |
| `have > hi` | `too many arguments to function call, expected {hi}, have {have}` | 同上 |
| `lo ≤ have ≤ hi` | `no matching member function for call to 'f' in class 'C' (…)` | [overload.match]/1 |

**教训**

★ **两种句式不是装饰**：个数不符与类型不符是两类诊断（clang 的 `CheckFunctionCall`
就分开报），而"个数落在区间内"恰恰说明**个数过滤这一层拦不住**、淘汰它的是类型 ——
报错必须指向真正的判据，否则会把人往错的方向引（本项目 B10/B20/B22 的教训反复如此）。

**实测**（四个形态全部与 clang **逐字相同**，见原表的 a/b/c/d 四行）

```text
e1.cpp:2:30: error: too few arguments to function call, single argument 'x' was not specified
[ERROR] [Semantic Error] 2:29: too few arguments to function call, single argument 'x' was not specified

e2.cpp:2:30: error: too many arguments to function call, expected 0, have 2
[ERROR] [Semantic Error] 2:29: too many arguments to function call, expected 0, have 2
```

d 形态（名字不存在）仍报原有的 `No member 'nope' in class 'C'`（未受影响）。

**回归**：`tests/lang/test_basics_07_member_call_arity_error.cpp`（个数区外，rc=1，
文件头记下"为什么不是笼统的 no matching"）；单测 `ArgBinding.NoViableOverloadIsAnError`
（个数**在**区内、类型不可行那一支）。

**顺带**：`pickBestByArgs` 的旧③"退回声明序首个"一并删掉 —— 见 [B27](#b27-无可行候选时静默退回候选集首个)。

---

## B26. 引用绑定判据只写了模板那一半

- severity: medium ｜ status: fixed ｜ related: docs/learn/23, docs/learn/36, tests/lang/test_basics_05_reference_binding.cpp, tests/lang/test_basics_06_rvalue_ref_binds_lvalue_error.cpp, tests/tmpl/test_tmpl_72_concrete_ref_param.cpp

**来源**：查 `src/template_deduction.cpp:123` 的"参数匹配"分支时反查了一个问题 ——
"非模板调用在哪里做同样的事？"答案：**哪里都没有**。

**现象**（两条方向相反，都是同一个判据缺一半的后果）

```cpp
// a：const T& 拒右值 —— 拒收合法程序
template<class T> int id(const T& x) { return x; }
int main() { return id(5); }               // clang rc=0；minicc rc=1

// b：int&& 收左值 —— 接受非法程序
int take(int&& r) { return r; }
int main() { int a = 1; return take(a); }  // clang rc=1；minicc rc=0
```

| 情形 | minicc（修前） | clang++-18 `-std=c++20` |
|---|---|---|
| a. `const T&` ← 右值（模板） | ❌ rc=1 `cannot bind lvalue reference 'const T&' to rvalue argument 'int'`（`template_deduction.cpp` 的 P=LRef 分支） | ✅ rc=0 |
| a′. `const int&` ← 右值（非模板） | ⚠️ rc=0 —— 非模板路径**根本没检查过**（连 `int&` ← 右值也放行） | ✅ rc=0 |
| b. `int&&` ← 左值（非模板） | ⚠️ **rc=0**，静默绑定 | ❌ rc=1 `cannot bind rvalue reference of type 'int&&' to lvalue of type 'int'` |
| b′. `T&&` ← 左值（模板，转发引用） | ✅ rc=0（S2 已实现的引用折叠） | ✅ rc=0 |

**根因**：**同一个判据写了两处，且只写了一处**。

| 路径 | 判据 |
|---|---|
| 模板实参推导（S2 那一轮的产物） | `deducePair` 的 P=LRef 分支**有**引用绑定检查；P=T&& 分支经引用折叠后左右值皆可 |
| 非模板调用（老路径） | `typeCompatible` / `stripRefConst` 都把**引用剥掉**再比类型 ⇒ `char&` 与 `char` 相等 ⇒ 什么都不挡 |

★ a 的机理值得单独说 —— 它不是"检查太严"，而是**检查看错了位置**，且错因来自**上一轮的改动**：

```text
docs/learn/23 之前：const T& 建成 Const(LRef(T))     → 剥顶层 const ⇒ LRef(T) ⇒ 检查看见 T
docs/learn/23 之后：const T& 建成 LRef(Const(T))     → 剥 const 的分支在【引用之外】不触发
                                                       ⇒ P 就是 LRef(Const(T))，而检查只问
                                                         "P 是左值引用吗？实参是右值吗？"
                                                       ⇒ 拒。const 被"关"在引用【里面】，
                                                         没人去看 referencedType->isConst()
```

`deducePair` 里那句注释（"本项目 parseType 把 `const T&` 建成 `Const(LRef(T))`"）
在 cv 位置修正之后就**已经过时**了 —— 判据没跟着改。

**教训**

**cv 位置一改，所有"按旧形态看类型"的地方都要跟着看一遍**：这正是 B17
（值位实参要求形态精确相等）同一轮埋下的第二颗雷。修法是让判据去问
`referencedType->isConst()`，而不是假设 const 在外面。

**修法**：判据收口到 `include/type.h` / `src/type.cpp` 的
`referenceBindsValueCategory(paramType, argIsLValue, &why)`，两个调用点共用：

| 调用点 | 位置 |
|---|---|
| 模板推导 | `deducePair` 的 P=LRef 分支（替换掉原有的内联检查） |
| 非模板调用 · 择优**前** | `inferCall` 裁决循环：`exactMatch(c.decl) && argsBindTo(...)` —— 绑定是**可行性**的一部分（[overload.viable]），不可绑定的候选先出局 |
| 非模板调用 · 兜底**后** | `checkArgBinding(f)`，三处选定点各调一次（`winner` / `m_functionMap` / 成员查找落空后的普通查找） |

★ 谓词（`argsBindTo`，可空 `why`/`badIdx`）与报错（`checkArgBinding`）**分开**是有意的：
先剔后退两条都对，先选后查会把 `f(5)`（候选 `f(int&)` 与 `f(const int&)` 并存）
拒成"no matching function" —— 而 clang 选 `f(const int&)`。
（这条自由函数场景眼下因 B7 在汇编期就撞符号而测不了，但判据该这么写。）

顺带修**值类别模型**：`*p`（[expr.unary.op]/1）与 `v[i]`（[expr.sub]/1）**也是左值**，
第一版只写了"变量 / 成员访问"，会让 `void g(int&); g(*p);` 被误拒 ——
**半份判据会造出反方向的缺陷**。

**回归**：`tests/lang/test_basics_05_reference_binding.cpp`（正例，运行 rc=0）+
`tests/lang/test_basics_06_rvalue_ref_binds_lvalue_error.cpp`（rc=1，文案含标准推导链）
+ 单测 `ArgBinding.ReferenceBindingFollowsValueCategory`（六条：三种引用形态 × 左右值，
外加 `f(*p)` 与 `f(5)` 两条边界）。**突变验证**：把 `referenceBindsValueCategory`
改成恒真 ⇒ 该例当场变红。

**文档**：docs/learn/36 §1~§2。

---

### B26 追补：A 的引用壳没剥（[expr.type]/1 的债，2026-10 第二天发现）

**来源**：用户读 `template_deduction.cpp:351`（P=LRef 分支）时问"模板是 `<int&>`、
实参也是 `<int&>` 的是不是匹配不上"。实测——**能**（模板形参位），但【具体 P】
**不能**。又是"侥幸活着"掩盖缺陷的现场。

**现象**

```cpp
// P = int&（具体类型，不含模板形参），实参是 int& 变量
template<class T> int f(int& x, T y) { return x + y; }
int main() { int a = 1; int& r = a; return f(r, 2) - 3; }
```

| | minicc（修前） | clang++-18 |
|---|---|---|
| `f(r, 2)`（P=int&，r 是 int&） | ❌ rc=1 `no match for non-dependent parameter: P='int' vs A='int&'` | ✅ rc=0 |
| `g(r, 2)`（P=T& 与 P=T 共享 T） | ⚠️ rc=0 但 `T := int&`（靠"值传递调整 A'=int"+折叠兜住；clang 是 `T := int`） | ✅ rc=0 |

**根因**：**A 是声明类型，不是表达式类型**。[expr.type]/1：表达式的类型
永不可能是引用 —— `int& r` 的表达式 `r` 类型是 `int`，值类别（左值）另由
`argIsLValue` 携带。而 inferCall 喂给 deducePair 的 A 是 `inferType(arg)` 的产物
（声明类型 `int&`）。P=T 形参位有"值传递调整 A'=int"兜底所以侥幸活着；
**具体 P** 走到非依赖比较时 `int` vs `int&` 结构不等，当场死。
`deducePair` 头部注释里其实早就写着这半句（"[expr.type]/1：表达式永远没有
引用类型，故 clang 里 A 到这一步已是裸类型"）—— 写了判据却没写实现。

**修法**：deducePair **入口**剥 A 的引用壳（仅 `!structuralMatch` —— 偏特化匹配的
A 是模板实参，`Box<int&>` 的 `int&` 是有意义信息）：

```cpp
if (!structuralMatch && A->isReference()) {
    A = A->referencedType;
    aStr = A->toString();
}
```

两条路同一判据：P=T 形参位与具体 P 位都从裸 A 推（② 的 `T := int&` 变成
clang 同款 `T := int`）。`structuralMatch` 守卫与 T&& 分支跳过万能引用是
**同一个守卫、同一个理由**。`deducePair` 的 A 形参因此从 `const TypePtr&`
改为**按值** `TypePtr`（入口要改绑，const& 改不了）。

**回归**：`tests/tmpl/test_tmpl_72_concrete_ref_param.cpp`（①具体 P=int& +
②T&/T 共享 T + ③右值引用变量作左值实参，运行 rc=0，clang 同 rc=0）。
**突变验证**：整块删掉剥壳 ⇒ 72 当场 rc=1（logdiff 红出 rc 差异）。
★ 突变验证时**必须 `./build.sh` 重编 minicc** —— `cmake --build unit_tests`
只编单测，logdiff 吃的是 `./minicc`，不重编就永远"零差异"（本次实测踩到）。
**既有用例漂移**：仅 `test_tmpl_52` 的推导 trace 8 行
（`A=int&`/`A=int&&` → `A=int`，语义不变，恰好是 52 自己注释里写的
[expr.type]/1 口径），重刷基线。

---

## B27. 无可行候选时静默退回候选集首个

- severity: high ｜ status: fixed ｜ related: docs/learn/36, tests/lang/test_basics_08_member_overload_no_viable_error.cpp

**来源**：B22 那一轮写的 `pickBestByArgs`（候选集 → 最优）里，第③档是
"退回声明序首个"。当时只是为了不让既有调用点崩（`candidates.front()`），
但它把"**判不了**"与"**判出来没人行**"混成了同一件事。

**现象**

```cpp
struct S { public: int v; };
class C { public: int f(int x) { return x; } int f(S s) { return s.v; } };
int main() { C c; int a = 1; int* p = &a; return c.f(p); }
```

| | minicc（修前） | clang++-18 `-std=c++20` |
|---|---|---|
| `c.f(p)`（`p` 是 `int*`） | ⚠️ **rc=0**，日志写着 `2 个候选，无一与实参相容 ⇒ 退回首个 'C_f_1_int'`，运行调到 `f(int)` | ❌ rc=1 `no matching member function for call to 'f'` |

**性质**：**静默算错**（能编译、能链接、能跑，只是语义错）——比"拒收合法程序"更坏的一类。
★ 注意它**不是** B25 的个数问题：`f(int)` 与 `f(S)` 都是 1 个形参，
个数过滤这一层**拦不住**，能拦住的只有类型可行性。

**根因**：`pickBestByArgs` 三档择优 `① 精确 → ② 可隐式转换 → ③ 退回首个`，
第三档不是"择优"而是**放弃判断**。而 [overload.best.viable] 的语义是
"可行集为空 ⇒ 非良构" —— 没有"退回首个"这一说。

**修法**：③ 改成返回 `nullptr`（+ 一条 `✗ … 无一可行 ⇒ 匹配失败` 的日志），
由调用点报错：

```text
  [overload] ✗ member C.f(1 args): 2 个候选，无一可行 ⇒ 匹配失败
[ERROR] [Semantic Error] 3:53: no matching member function for call to 'f' in class 'C'
        (1 argument(s) given; 实参类型无法按 [overload.viable] 转换)
```

两个例外**必须保留**（否则会把"判不了"误报成"匹配失败"）：
① 实参类型为 `nullptr`（依赖上下文 / SFINAE 探测里，无从判断可行性）⇒ 维持"取声明序首个"；
② 候选只有一个时**不打日志**（绝大多数调用无重载，打出来会把日志淹掉 ——
与 B22"符号名的类型链只在有同名同个数兄弟时才挂"同一条零漂移策略）。

**回归**：`tests/lang/test_basics_08_member_overload_no_viable_error.cpp`（rc=1）
+ 单测 `ArgBinding.NoViableOverloadIsAnError`。**突变验证**：把 ③ 改回
`return candidates.front();` ⇒ 该例当场变红。

**文档**：docs/learn/36 §2.2。

**教训**

（无）
## B28. 偏特化匹配误用调用推导的剥壳规则（structuralMatch 的另一半）

- severity: high ｜ status: fixed ｜ related: docs/learn/19, docs/learn/21, tests/tmpl/test_tmpl_73_partial_spec_ref_args.cpp

**来源**：排查 B26 追补时顺带核对 `matchPattern` 的 structuralMatch 路径，
发现**调用推导的两条"剥壳"规则没有随 structuralMatch 一起关**：
`deducePair` 的引用分支递归时把 A（模板实参，**可以**是引用类型）原样传下去、
裸形参位的"值传递调整"无条件剥顶层引用/const。

**现象**（两个文件，四种错法）

| 程序 | minicc（修前） | clang++-18 `-std=c++20` |
|---|---|---|
| `Duo<T,int&>` 偏特化 + `Duo<int,int&> d;` | ❌ rc=1 `no match for non-dependent parameter: P='int' vs A='int&'` | ✅ rc=0 命中偏特化 |
| `Same<T,T>` 偏特化 + `Same<int&,int&> s;` | ⚠️ rc=0 但 T := int（clang 是 `int&`），体内用 T 即错 | ✅ T := int& |
| `Same<T,T>` 偏特化 + `Same<int&,int> m;` | ⚠️ **rc=0 误命中偏特化**（v()=4），clang 判 T 推导冲突回落主模板（v()=0），两版运行返回值不同 | ✅ 主模板 |

**性质**：三种症状各一个 —— 拒收合法程序、绑定错（潜在算错）、**静默选错特化**。

**根因**：`[expr.type]/1`（表达式没有引用类型）与 [temp.deduct.call] 的值传递调整
都是**调用推导**的上下文假设；而偏特化匹配（[temp.class.spec.match]/2）比对的是
**模板实参表**，实参本来就是类型、可以带引用壳。B26 追补给入口剥壳加了
`!structuralMatch` 守卫、给万能引用分支加了跳过，但**引用分支的递归**与
**裸形参位的调整**两处漏网 —— 同一个参数的三处用法只改了两处。

**修法**（`src/template_deduction.cpp` deducePair，三处一致地以 structuralMatch 为界）：
① 入口：A 的引用壳剥除仅 `!structuralMatch`（B26 追补已做）；
② 引用分支（`P=T&` / `P=T&&`）：`structuralMatch` 下**对称剥壳**，
   递归改传 `A->referencedType`（安全性：matchPattern 的 refKind 预检已保证
   P 是引用 ⇒ A 是同种引用）⇒ 具体引用位 `int&` 配 `int&` 逐层合一命中；
③ 裸形参位：值传递调整整段收进 `if (!structuralMatch)` ⇒ 绑定原样实参类型
   （`Same<T,T>` 配 `<int&,int&>` ⇒ T := int&；配 `<int&,int>` ⇒ 冲突拒候选）。

**回归**：`tests/tmpl/test_tmpl_73_partial_spec_ref_args.cpp`
（5 实例组合，返回 0；与 clang 逐一核对）。**突变验证 2 条**：
MUT-1 对称剥壳回退 ⇒ 运行 235（红）；MUT-2 调整恢复无条件 ⇒ 运行 64（红）。

**日志漂移**：5 个既有文件（34/46/47/52/72）的推导轨迹行 ——
全是"值传递调整 A'=…"消失或绑定行改写（绑定更贴近 clang），
程序输出/退出码/汇编零变化，已重刷基线（124 → 125）。

**文档**：tests/tmpl/test_tmpl_73 文件头（理论点全列）。

**教训**

（无）
## B29. 全局/命名空间内的类型别名存了未实例化模板 id + 裸名（两处都是"没做完"）

- severity: medium ｜ status: fixed ｜ related: docs/learn/24, docs/learn/43, tests/decl/test_decl_03_global_alias.cpp

**来源**：排查"`using IntBox = Box<int>;` 能不能用"时发现的旧缺口 ——
此前唯一被测过的别名形态是**类内**别名（learn/24 的三条查找路径全在类作用域），
全局与命名空间形态从没进过用例。

**现象**（修复前）

| 程序 | minicc（修前） | clang++-18 |
|---|---|---|
| `using IntBox = Box<int>; IntBox b; b.v = 7;` | ❌ rc=1 `No member 'v' in class 'Box'` | ✅ rc=0 |
| `typedef Box<int> IntBox;`（同上用法） | ❌ 同上 | ✅ rc=0 |
| `namespace N { using IntBox = Box<int>; } N::IntBox b;` | ❌ rc=1 `unknown type name 'N::IntBox'` | ✅ rc=0 |

**性质**：拒收合法程序（两个方向）。

**根因**：`processTypeAliasDecl` 两个"没做完"——
① 把 `underlyingType`（**未实例化**的模板 id `Box<int>`，一个名字节点）
原样存进符号表；使用点 `IntBox b;` 拿到的还是蓝图名 "Box"，
字段查找对着蓝图查 ⇒ 报 No member。对照 clang：TypedefNameDecl 的底层类型
在**声明点**就完成决议（Sema::getTypeName / BuildTypedefNameExpr）。
② 注册名是裸 `aliasName`，不带 `m_currentNamespace` 前缀
（类/函数在 processNamespaceDecl 里早就前缀化了，别名漏了）。
使用点带着 `N::IntBox` 限定名来查，裸名必然查无此名。

**修法**：声明点 `resolveType(underlyingType)` —— 模板 id 在此**实例化**、
别名模板在此**解糖**（复用既有路径，一行新机制都没造），之后所有使用点
拿到的都是成品类型；注册名加命名空间前缀，与类/函数同约定。

**回归**：`tests/decl/test_decl_03_global_alias.cpp`
（using/typedef/命名空间前缀三个形态，运行钉 0）。

**教训**

修复前该类组合**没有任何用例覆盖**（别名全测在类内）——修复后
既有用例逐字节零漂移。

**文档**：docs/learn/43 §1。

## B30. 表达式级构造调用 `Box<int>(7)` 无路可走 + 平凡拷贝/类赋值静默算错

- severity: high ｜ status: fixed ｜ related: docs/learn/43, tests/lang/test_basics_11_ctor_expr.cpp, tests/lang/test_basics_12_ctor_expr_error.cpp

**来源**：追问"inferVar 能不能支持 `x = Box<int>;` 这种"时确认的两组缺口：
Parser 一直能建出 `CallExpr(VarExpr(Box)+显式模板实参)`，但语义层
inferCall 四路（成员/自由函数/函数模板/回退）全不接类模板构造 ⇒
落到回退 `inferType(VarExpr(Box))` ⇒ `Undefined variable 'Box'`；
继续探下去发现**相邻的三个伴随 bug**。

**现象**（修复前）

| 程序 | minicc（修前） | clang++-18 |
|---|---|---|
| `Box<int> b = Box<int>(7);` | ❌ rc=1 `Undefined variable 'Box'` | ✅ rc=0 |
| `auto b = Box(9);`（表达式级 CTAD） | ❌ 同上 | ✅ rc=0 |
| `Box<int> b = a;`（同类左值拷贝初始化） | ❌ 链接期 `undefined reference to 'Box_int_Box_int'`（编译期全绿） | ✅ rc=0 |
| `b = Box<int>(9);` / `b = a;`（类实例赋值） | ⚠️ rc=0 但**静默算错**（当标量赋值只写 8 字节、源地址错） | ✅ rc=0 |

**根因**（一条主线 + 三个伴随）：
① 主缺口：inferCall 没有"类模板名 + 显式/推导实参 ⇒ 构造调用"分支。
   修法：在自由函数查找**之前**插入构造分支 —— 命名空间限定名回退、
   显式实参经 resolveType 实例化（NTTP 值位保留）、空实参表走 CTAD
   （语句级 deduceClassTemplateArgs 提炼出共用的 `deduceCtadCore`，
   [dcl.type.class.deduct]：构造函数形参表当推导指引，与函数模板同一套合一），
   构造选定走 pickBestByArgs，结果回填 `isCtorCall/ctorClassType/符号`
   （与 B10/B21/B22 的"符号回填"同一形状 —— mangling 有状态，
   CodeGen 硬拼必错配）。
② 伴随一（**链接期才炸的错配**）：`Box<int> b = a;` 在 Sema 既不是
   构造调用也不是标量初始化，掉到 CodeGen"无初始化式类声明"分支发
   **0 参默认构造**调用 —— 符号只在用户真写了 0 参构造时才存在。
   修法：Sema 判 `isTrivialCopy`（同类左值 + init 限 Var/Member），
   CodeGen 按布局复制；表达式升级分支同点判 `[class.copy.elision]`
   拷贝省略（临时对象永不物化）。
③ 伴随二（**静默算错**）：类实例赋值走了标量 AssignStmt 路径。
   修法：Sema 在 visit(AssignStmt) 回填 `isClassAssign/assignClassType`，
   CodeGen 按对象表示逐字节复制。
④ 伴随三（**克隆丢槽**）：cloneStmt(VarDeclStmt) 漏搬 `ctorArgs` ——
   顶层 `Box<int> b(7);` 正常，模板体内同名写法构造实参**整体丢失**。

**修法**

**★ 实现期踩的两个坑**（写进判据）：
- **析构时机**：visit(CallExpr) 若对构造临时对象无条件发析构，
  `b = Box<int>(9);` 会 memcpy **已析构**的源 —— 用 `m_suppressTempDtor`
  协作：赋值 RHS 语境抑制，memcpy 完成后由 visit(AssignStmt) 补发
  （[class.dtor]/2：临时对象在**全表达式结束**才析构）。
- **内联复制**：自研链接器只内置 malloc/free，[builtin] 阶段注册的
  memcpy 只是**原型** —— 对象复制一律 `rep movsb` 内联展开，没有
  `callq memcpy` 可绑。

**回归**：`tests/lang/test_basics_11_ctor_expr.cpp`（6 检查点：
拷贝省略升级 / auto+构造表达式 / 类赋值 RHS 临时 / CTAD / 平凡拷贝 /
合成拷贝赋值，运行钉 0）+ `tests/lang/test_basics_12_ctor_expr_error.cpp`
（负例：`Box<int> b = Box<double>(7);` ⇒ rc=1
`Type mismatch in 'b': declared 'Box_int', got 'Box_double'`）。

**日志漂移**：既有 140 个集成用例**逐字节零漂移**；基线 140 → 143
（+3 新文件）。

**文档**：docs/learn/43 §2。

**教训**

（无）

<<<<<<< HEAD
---

## B31. 模板-id 试探把 `a<b>(c)` 误判为模板调用（拒收合法程序）

- severity: medium ｜ status: fixed ｜ related: docs/learn/45, docs/learn/46, tests/lang/test_basics_20_template_id_disambiguation.cpp

**来源**：复查 `parsePrimaryExpr` 的 template-id 歧义消解（parser.cpp:2989 起
"试探法三步曲"）时确认。这轮代码审查顺带核对出三个**无问题**项（`>>` 不切 token、
`m_stopAtGreater` 有 save/restore、表达式分支异常即回滚）—— 但判据本身有方向性分歧。

**现象**

```cpp
int main() {
    int a=1, b=2, c=3;
    return a<b>(c);    // clang：比较链 (a<b)>(c) = false → rc=0（仅 -Wparentheses 警告）
}
```

| | minicc | clang++-18 `-std=c++20` |
|---|---|---|
| `return a<b>(c);` | ❌ rc=1（判成模板调用，Sema 报无此模板） | ✅ rc=0 |

**性质**：**拒收合法程序**。minicc 的判据是"`>` 后紧跟 `(` 或 `::` ⇒ 认定 template-id"，
而 clang 把 `a<b>(c)` 解析为比较链 `(a<b)>(c)`（左结合 relational 运算，
`<` 前不是模板名就是比较）。

**根因**：架构性的。**clang 在这一刻会反向查符号表**判"`a` 是不是模板名"
（语法 ⇄ 语义双向反馈）；minicc 则把符号表查询整体推迟到 Sema（语法期零语义，
这是 [stmt.ambig] 两轮修复的设计基石），纯语法启发式拿不到"是不是模板"这个信息，
只能看紧跟的 token 下注。`x<y>(z)` 全变量的写法现实中罕见
（需要三个短名字恰好凑成这个形状），所以一直没炸。

**修法（要修的话）**：Parser 持一份**只读的"模板名登记表"**（类模板/别名模板/函数模板名，
Pass 1 前向收集或 Lexer 期建），消歧时先查表：名字在表中 ⇒ template-id；
不在 ⇒ 比较运算。不动 Sema 分层（登记表只是语法期的消歧 hint，不做决议）。
注意与 docs/learn/39 §"刻意不收 Less/Greater"的判据协同，别把 `>` 收进
`isExprContinuation`（tmpl 21..26 会全线漂移）。

**教训**

**正确的写法**（clang 的做法，也是生产方式）：

1. **消歧 = 只读查询，不是全量语义**。clang 的 Parser 持 `Actions`（Sema 句柄），
   在 `<` 处回调 `isTemplateName(...)` 问一个 bit 的问题，决议权仍在 Sema。
   对 minicc 的轻量版：Parser 持只读模板名登记表，查不到就按比较运算——
   **查询面只到"名字的种类"，不扩大成"方法存在性 lookup"**。
2. **试探失败用回卷，不用异常**。clang 的 tentative parsing 是"存档 token 游标 →
   空跑 → 不对就回卷"，全程零异常——`a < b > c` 是常见形态，不是异常情况。
   且异常版有个真隐患：catch 的是 `std::exception` 全集，试探内部抛出**真 bug**
   （logic_error / 越界）会被一并吞成"不是模板"，错误静默降级为比较运算。

```cpp
// 正确形态示意（取代 try/catch 三步曲）
bool isTemplateId = false;
{
    TokenCheckpoint cp(this);            // RAII：析构即回卷（不止 m_pos，见 B32）
    auto args = parseTemplateArgumentList();
    isTemplateId = m_templateNames.count(baseName)          // ← 只读登记表查询
                && (check(TokenType::LParen) || check(TokenType::ColonColon));
    if (isTemplateId) cp.commit();       // 确认后才释放存档
}                                        // 未 commit ⇒ 全状态回卷
```

**状态**：✅ 已修（ROADMAP 主线 I 第 1 项，docs/learn/46）。
**修法**（与"要修的话"一致）：Parser 构造时前向扫 token 流建只读**模板名登记表**
`m_templateNames`（类/别名/函数模板、推导指引、成员模板、模板模板形参全收；
Sema 内建的 `std::declval` 以种子名单收入 —— 它不经 token 流，漏收会让
test_tmpl_35..39 的 `decltype(std::declval<T>()...)` 当场被消歧成比较链），
消歧第一层闸 = `check(Less) && isKnownTemplateName(name)`；不在表 ⇒ 不试探
直接按比较解析。在表仍走 B32 的试探-回滚（变量可遮蔽模板名，方向保守）。

**回归**

回归红线：`a<b>(c)` rc=0 与 clang 一致（tests/lang/test_basics_20），
既有 150 份日志基线零漂移（`>` 未进 `isExprContinuation`，与 learn/39 判据协同）。
突变验证：摘掉闸 ⇒ 新测试退回链接期 `undefined reference to 'a'`（rc=1），
证明判据有判别力。
★ 附带问题（catch std::exception 全集吞真 bug，主线 I 第 3 项）已随 B32 修复：
  ParseError 专用异常 + 试探处窄捕获，B32 批次落地（docs/learn/45 §3）。

---

## B32. 模板-id 试探回滚只撤 `m_pos`，不撤日志（幽灵 `[parse:targ]` 行）✅ 已修

- severity: low ｜ status: fixed ｜ related: docs/learn/45, docs/learn/39, tests/lang/test_basics_19_tentative_rollback.cpp

**来源**：与 B31 同轮代码审查。B31 是判据错，本条是**机制漏**：试探-回滚的
清理只做了一半。

**现象**

```cpp
class Box { public: int v; };
int main() {
    int a = 1, c = 3;
    a < Box > c;    // 比较链 (a<Box)>c —— Box 语法上是合法类型实参，试探【成功】
    return 0;
}
```

- Parser 走到 parser.cpp:3001 的试探：`parseTemplateArgumentList()` 把 `Box`
  解析成类型实参 **成功**，内部打印 `[parse:targ] template argument list: <1 argument(s)>`
  及逐实参行；
- 随后发现 `>` 后是 `c` 不是 `(`/`::` ⇒ `isTemplateId = false` ⇒
  `m_pos = saved` 回滚，按比较表达式重新解析 —— **语法与语义都对**；
- 但刚才那几行 `[parse:targ]` 日志**撤不回**，留在输出里成幽灵记录
 （最终 AST 与日志对不上）。

**性质**：纯日志瑕疵，不影响编译正确性；但对本项目"**日志即契约**"的基线体系
（logdiff 逐字节比对、单测子串匹配日志原文、docs/learn 直接粘贴日志片段）
是真污染 —— 用户照着日志学解析过程会被误导。

**根因**：回滚只恢复了 `m_pos` 一个状态维度。`parseTemplateArgumentList`
内部到处 `std::cout` 直接打点（698~807 行共 5+ 处），没有"静默模式"概念；
试探期间产生的输出没有任何缓冲/抑制机制。

**修法（要修的话）**：给 Parser 加一个**静默深度计数器**（`m_quietDepth`）：
试探三步曲入口 `++m_quietDepth`、出口 `--m_quietDepth`；
所有 `[parse:targ]` / `[parse]` 打点处包一层 `if (m_quietDepth == 0)`。
（把日志收口成一个小宏/辅助函数，避免 5 处散落判断日后漏改。）
不改 `parseIntLiteral` 的刻意不打日志约定（learn/39）。

**教训**

**正确的写法**（两条生产惯例，缺一不可）：

1. **回滚要"存档点"化，不能只救一个字段**。生产 parser 的试探解析把"存档 +
   回卷"做成 RAII 对象（clang 的 `TentativeParsingAction` / `TPResult`）：
   构造时快照全部可变状态，析构时整体回卷，`commit()` 后才放手。
   新增状态维度时**编译器逼着你**把它加进快照——这就是防止"回滚漏维度"
   （本条）复发的结构保证，而不是靠 Code Review 记性好。

```cpp
class TokenCheckpoint {
    Parser& p;  size_t pos;  bool stopAtGreater;  bool committed = false;
public:
    explicit TokenCheckpoint(Parser& p) : p(p), pos(p.m_pos),
        stopAtGreater(p.m_stopAtGreater) {
        p.m_quietDepth++;                    // 试探期间顺带静默（顺手治本条）
    }
    ~TokenCheckpoint() {
        p.m_quietDepth--;                    // 静默总是成对退出（含异常路径）
        if (!committed) {                    // 未确认 ⇒ 全维度回卷
            p.m_pos = pos;
            p.m_stopAtGreater = stopAtGreater;
        }
    }
    void commit() { committed = true; }      // 确认：保留 token 进度
};
```

2. **日志收口成单一出口**。散落 5+ 处的 `std::cout << std::format(...)` 全收进
   `Parser::logParse(fmt, args...)` 一个辅助函数（项目里符号定名、择优早已
   "判据单点"，日志却一直是散点——同一个原则两处执行力度不同）。
   收口后静默开关、未来加时间戳/日志级别都是改一处。

**状态**：✅ 已修（ROADMAP 主线 I 第 2、3 项一并完成，2026-10-23）

**修法**（按本条「正确的写法」两条生产惯例落地，docs/learn/45）：
① **TokenCheckpoint RAII**（parser.h）：构造 ⇒ 快照 m_pos + m_stopAtGreater +
   日志缓冲入栈；`commit()` ⇒ 试探成立，保留进度、缓冲冲刷（成功路径输出
   【逐字节不变】）；析构未 commit ⇒ 全维度回卷 + 缓冲**整体丢弃** —— 幽灵行
   从机制上不可能再产生。新状态维度进快照是编译期强制（B32 说的"结构保证"）。
   ★ 设计取舍：不是"静默开关"而是"缓冲-冲刷"——成功路径的 [parse:targ] 等
   日志是【真日志】（现有基线靠它们对拍），静默会让成功路径也丢行；
   缓冲只在回卷时丢弃，语义精确对应"这次解析没发生过"。
② **日志收口单一出口**：parser.cpp 全部 87 处 `std::cout` 散点收进
   `Parser::logParse(fmt, args...)`（链式多段输出改为拼 string 后单点）；
   缓冲分支只此一处。`parseIntLiteral` 的"刻意不打日志"约定不变。
③ **附带完成主线 I 第 3 项**（异常退出试探路径）：新增 `ParseError` 专用
   异常类型（is-a runtime_error，顶层出口不变），试探处 `catch (const ParseError&)`
   ——只有"语法错误"这个预期信号被解释为"不是 template-id"；试探内部的
   真 bug（越界/logic_error）不再被吞，穿透到顶层响亮报错。

**★ 修复过程的意外发现**：基线里早已固化 14 行幽灵行而无人察觉 ——
  flow_01（6 行：for 条件 `i < 10` 的 NTTP 试探）、flow_02（2 行：`a < b`）、
  flow_05（1 行）、flow_06（3 行）、basics_10（2 行）。凡有比较运算的
  测试都在默默产生幽灵。**基线会把污染也固化成契约**（第三次印证）。
  另：本条原始复现 `a < Box > c;` 经 clang++-18 实测是【非法程序】
  （"'a' does not name a template but is followed by template arguments"）——
  minicc 宽松放行；等 B31 的模板名登记表落地后再裁这条分歧。

**回归**：`tests/lang/test_basics_19_tentative_rollback.cpp`（合法形态：
  循环条件 NTTP 试探 + 连续比较链；断言在日志——基线不得出现 [parse:targ]/
  [parse:type] 试探残留；运行钉 0，clang 对拍一致）。

**日志漂移**：**有意** —— 5 个文件各删若干幽灵行（共 14 行，纯删除、
  rc/run 逐字节不变），基线 155 → 156（+1 新文件）。其余 150 个文件零漂移。

**文档**：docs/learn/45。

**未做的同类现场**（记下不混入本批）：parseDeclaration（890 起）与类体
  方法/字段判定（1862 起）的"parseType 试探 + m_pos 回滚"会让 [parse:type]
  行**成对重复**（每个类型打印两遍）——同属试探日志问题，但那两处没有
  幽灵只有重复，改动将使全量基线翻倍瘦身，适合单独成批。

---

## B33. static 数据成员整体缺失：Parser 硬性拒收 + 无存储模型

- severity: medium ｜ status: fixed ｜ related: docs/learn/44, tests/lang/test_basics_13_static_member.cpp

**来源**：追问"`Other::s` 这种静态字段怎么访问"时确认 ——
类体内 `static int count;` 直接在 Parser 报
"static data members are not implemented (only static member functions are)"
（**拒收合法程序**），整条 [class.static.data] 没有实现。

**现象**（修复前）

| 程序 | minicc（修前） | clang++-18 |
|---|---|---|
| `struct A { static int count; }; int A::count = 3; A::count` | ❌ rc=1 Parser 硬拒 | ✅ rc=0 |
| `static const int limit = 7;` 类内初始化 + 读取 | ❌ 同上 | ✅ rc=0（编译期常量折叠） |
| 成员函数内裸名 `return count + limit;` | ❌ 同上 | ✅ rc=0 |
| 模板类 `C<int>::cid` | ❌ 同上 | ✅ rc=0 |

**根因**

（无）

**修法（设计）**（不是 bug 修复而是特性落地，但按本仓惯例记此）：
① **存储模型**：static 成员不属于对象（sizeof 不含它）= "挂在类名上的
   全局变量"。类内只是【声明】；非 const 成员的存储只能来自类外定义
   （`int A::count = 3;` ⇒ 全局符号 `A::count`，asmSymbol 净化为
   `A__count`，读写两端同源）。漏定义又 odr-use ⇒ 链接期
   `undefined reference`（与 clang 的 ld 报错同形态，响亮）。
② **类内初始化**（[class.static.data]/3）：const 整型族 / constexpr
   允许类内初始化 ⇒ 编译期常量、**无存储**。★ 判据对 Const 包裹剥一层
   再查家族（`const int` 是 Const(Int)，isInteger() 只看最外层 kind —
   第一版没剥，把合法的 `const int limit = 7` 当场误拒）。
   双登记：staticConsts（isTypeAccess 折叠 `Cls<Args>::value` 的老设施）
   + registerConstVar 编译期常量表（裸名传播 / NTTP 实参 `Buf<A::twice>`）。
③ **读取三路径判据单点**（承 B21/B22 教训）：`A::count`（Parser 粘单名
   → 符号表全名，inferVar 第 0 步走 resolveQualifiedField 第二源）、
   `a.count`（布局落空 → 符号表回退，inferMember 普通分支）、
   `a.A::count`（resolveQualifiedField：布局三分条件循环 → static 第二源
   符号表 BFS 基类链）。第二源在 resolveQualifiedField 一处，两条调用
   路径共用。
④ **查找顺序 = [class.member.lookup]/3 隐藏**：实例字段（扁平布局）
   先于 static 成员 —— 派生类实例字段可合法隐藏基类 static。
⑤ **折叠槽**：类内初始化的 static const 没有存储，MemberExpr 读取必须
   折叠成立即数 —— MemberExpr 加 `staticConstFolded/Value` 回填槽
   （CodeGen 见槽发 `movq $v`），lookupStaticConst 沿基类链查。
⑥ **成员函数内裸名的时序坑**：ReturnStmt::visit 是【先 fold 后 infer】，
   inferVar 第 2.5 步的改名还没发生，裸名 `limit` 在折叠器里查不到。
   修法：constResolver 回调补第 ② 路 —— 直查落空时按当前类 + 基类链
   BFS 补查 "类名::裸名"（与 inferVar 2.5 同一判据形状）。
⑦ **类外定义的校验**（processGlobalVarDecl 头部）：
   ★ 判据收窄 —— 限定者是【类】才走 static 校验；`int Math::g_factor`
   （namespace 成员）同形但走普通全局变量旧路（第一版把命名空间成员
   定义也拦下，decl_01 当场变红）。四条校验：类存在 / 声明过 /
   **equals 同类型**（typeCompatible 会把 long/int 判兼容静默放行，
   探针校准时实测抓到）/ ODR 单定义。
⑧ **模板类**：实例化步骤 5.7 把蓝图 staticDataMembers（含初始化式
   cloneExpr）替换后挂到实例类 —— 实例经 processClassDecl 自然完成
   常量表/符号注册，与 C++ 语义一致（蓝图不过 processClassDecl）。

**回归**：`tests/lang/test_basics_13_static_member.cpp`（正例 13
检查点：三入口同存储读写 / 成员函数裸名 / const 三路径折叠 / 继承 +
实例字段隐藏 / 命名空间类多级限定 / 模板类 / NTTP 实参，运行钉 0，
clang 对拍一致）+ 5 个负例（basics_14..18：out of line 初始化 /
no member named / redefinition / not a class / different type，
文案与 clang++-18 **逐字相同**，全部探针校准）。

**日志漂移**：既有 149 个集成用例**逐字节零漂移**；基线 149 → 155
（+6 新文件）。全量 ctest 346 全绿。

**文档**：docs/learn/44。

**教训**

（无）

---

## B34. const 类对象端到端缺失：成员访问误报 + 掉标量路径 SIGSEGV + 拷贝静默退化 ✅ 已修

- severity: high ｜ status: fixed ｜ related: docs/learn/31, tests/lang/test_basics_21_const_class_object.cpp

**来源**：追问"`const int` / `const ClassA` 语句声明走 parser.cpp 哪个分支"时
顺藤摸到的**一族**缺陷 —— `const Box_int b;` 的声明类型是 Const(Box_int)，
Sema/CodeGen 的类性判定（查类表 / `isClass()`）全部不看穿这层 Const 包壳。

**现象**

**三种错法三种症状**（同一句 `const ClassA a = tmp; return a.v;`）：

| # | 位置 | 症状 | 性质 |
|---|---|---|---|
| ① | Sema `inferMember`（semantic_analyzer.cpp:5660 一带） | 只剥引用不剥 const ⇒ `Cannot access member 'v' on non-class type 'const ClassA'`，rc=1 | **拒收合法程序**（clang rc=0） |
| ② | CodeGen `visit(VarDeclStmt)`（codegen.cpp:1279 一带） | `m_classTypes->find("const Box_int")` 落空 ⇒ const 类对象掉标量路径：无 RAII 构造/析构，读成员被当"变量存指针再解引用" | **miscompile，实测 SIGSEGV（rc=139）** |
| ③ | Sema 平凡拷贝判据 `type->equals(initType)`（semantic_analyzer.cpp:3259 一带） | const 目标与裸源类型 equals 不成立 ⇒ 拷贝初始化静默退化成默认构造，`a.v` 读出 0 | **静默算错**（返回 251 而非 0） |

**根因**

★ ②的 segv 现场被 ①的报错**挡住**了——修掉 ① 才暴露 ②，再修掉 ② 才暴露 ③。
一层 const 包壳，三层各自独立写错的判据。

**修法 —— 判据归一**：类性/类表查询一律剥壳。
- CodeGen 加文件级助手 `stripTopCvRef`（循环剥顶层 const/引用，`const Box& r`
  的 Const(LRef(Box)) / LRef(Const(Box)) 两种 cv 位置都收），应用于
  VarDeclStmt 类表查询、帧尺寸估算两处、成员读/写字段偏移、方法调用
  （虚表查询 + 符号拼接）、下标 set 糖共 7 处落点；
- Sema 加 `bareForClass` 同型 lambda，应用于构造调用升级（拷贝省略）、
  平凡拷贝、直接初始化选构造三处；
- `inferMember` 引用剥除后补 const 剥除。

**教训**

★ 只服务类性判定：标量宽度分派、日志、符号表存的原 type 全部不动 —
— 非 const 程序汇编逐字节不变。const 只管写检查（minicc 未做，ROADMAP
⑥ 已列），故剥壳无语义损失；成员【查找】本就不受对象 cv 影响
（[class.mfct.non-static]）。

**回归**：`tests/lang/test_basics_21_const_class_object.cpp`（const int
语句声明 / const 默认构造 + const 成员函数 / const 源→const 目标平凡拷贝 /
const 带参构造 / const 模板类，运行钉 0，clang 对拍）。
**突变验证**：摘 VarDeclStmt 剥壳 ⇒ rc=139（segv 现场复现），恢复 ⇒ rc=0。

**有意漂移**：test_tmpl_50 的汇编 4 行改善 —— 它此前正落在"偏移未知默认 0"
的 movq 回退路径上（8 字节读 4 字节 int 字段），修复后走正确的字段偏移
movl 路径；rc/运行输出逐字节不变，基线已重刷。这也回答了一个遗留疑问：
回退路径此前并非死代码，const 对象一直在用它。

**日志漂移**：其余 150 个集成用例逐字节零漂移；基线 151 → 152（+1 新文件）。
全量 ctest 346 全绿，build 0 警告。

**文档**：本条。


---

## B35. 语句层前瞻只认固定 token 顺序：限定名模板 id 与全变量比较链双双被拒收

- severity: medium ｜ status: fixed ｜ related: docs/learn/22, docs/learn/47, tests/lang/test_basics_22_stmt_ambig_tentative_parsetype.cpp

**来源**：复盘 parseStatement 2245 前瞻分支（B31/B34 两次会话都路过它）时
确认。该分支是 [stmt.ambig] 的语句层实现：标识符开头既可能是 `MyClass obj;`
（声明）也可能是 `foo(x);` / `a = 3;`（表达式），LL(1) 判不了，用"跳过
`名 [<...>] [::]* [*&]*` 后看下一个是不是标识符"的启发式下注。

**现象**

```cpp
namespace N { template<class T> struct B { T v; }; }
class Box { public: int x; };
int main() {
    N::B<Box> y;          // clang rc=0；minicc "Undefined variable 'N::B'"
    int a=1; int b=2; int c=3;
    a < b > c;            // clang rc=0（仅 -Wunused-comparison）；minicc 误判声明
}
```

| 程序 | minicc（修前） | clang++-18 |
|---|---|---|
| `N::B<Box> x;`（限定名 + 模板 id） | ❌ `Undefined variable 'N::B'`（前瞻的 `<` 跳过只发生在【第一个】标识符后，`::B<Box>` 那段没人跳） | ✅ rc=0 |
| `a < b > c;`（全变量比较链） | ❌ 误判成声明 `a<b> c`，Sema 报错（报错文案自己都承认"本实现的前瞻会把它当成变量声明……试探法近似"） | ✅ rc=0 |

**性质**：两处都是**拒收合法程序**；且根因是架构性的——

**根因**：2245 的"手写 token 跳过人"是 parseType 语法的**残缺复制品**。
同一个类型语法（限定名 / 模板 id / 嵌套模板实参 / typename 前缀）在
两份代码里各写一遍判定：parseType 写全了，跳过人只认了其中一个固定
token 顺序（`名 <...> :: ...`——`名 :: ... <...>` 就漏）。token.h:236
对 isTypeKeyword 的警告（"同一语义在两条路径上判定，改一边要核对另一边"）
在这里以 bug 形式兑现。B32 批次已把"试探-回滚"升级为 TokenCheckpoint
RAII + 日志缓冲，B31 落地了模板名登记表——改写所需的机制都已就位，
缺的只是动手。

**修法（要修的话）**（对照 clang TryAnnotateTypeToken + tentative parse）：

1. **试探 parseType 取代手写跳过人**：TokenCheckpoint 存档 → parseType()
   通用解析（限定名/模板 id/嵌套/typename/const 全收）→ 成功且后跟
   标识符 ⇒ 声明（commit，类型已解析好直接传 parseVarDeclStmt）；
   `;` ⇒ 可能 B16 无声明符声明（再过 isDeclaratorlessDecl）。失败或
   形态不符 ⇒ 析构回卷走表达式，试探日志随缓冲整体丢弃（B32 机制）。
2. **模板名登记表第一层闸**（B31 同款）：试探成功但类型是模板 id 且
   基名不在登记表 ⇒ 全部推翻回卷——否则 `a < b > c;` 会被 parseType
   成功解析成"模板 id a<b> + 声明符 c"继续误判（语法上无懈可击，
   必须问"名字的种类"这一个 bit，这正是 B31 确立的做法在语句层的
   对称应用）。成功路径日志与今天"回滚后正式 parseType"逐字节一致。

**范围**：只收 parseStatement 的 2245 分支；类体方法/字段判定（1862）
与顶层 parseDeclaration（890）两处同族现场（CLAUDE.md B32 批次已登记
"[parse:type] 成对重复"）单独成批。

**状态**：✅ 已修。
**修法**（与"要修的话"一致，两层）：

1. **Parser（parseStatement 标识符前瞻分支重写）**：手写 token 跳过人
   换成「TokenCheckpoint + parseType() 通用解析」（对照 clang
   TryAnnotateTypeToken + tentative parse）——限定名/模板 id/嵌套/
   typename 全收，消掉第二份残缺语法；成功后标识符 ⇒ 声明、';' 且带
   模板 id 语法 ⇒ B16 无声明符声明（等价旧 sawTypeIdSyntax，刻意不收
   裸 `S;`，留表达式路径给 Sema）。失败/推翻 ⇒ 析构回卷，试探日志随
   B32 缓冲整体丢弃。成功路径输出与旧实现"回滚后正式 parseType"
   **逐字节一致**（152 份基线零漂移实证）。
2. **登记表第一层闸（B31 同款，语句层对称应用）**：类型（含嵌套限定者
   链 t→nestedQualifier）里任一模板 id 层的基名不在登记表 ⇒ 全部推翻
   ——否则 `a < b > c;` 会被 parseType 语法无误地解析成"模板 id a<b> +
   声明符 c"继续误判（语法层面无懈可击，必须问"名字的种类"这一个 bit）。
3. **附带 Sema 修复**：resolveType 模板 id 分支补限定名归一 ——
   `N::B<Box>` 剥命名空间前缀取末段查蓝图表（蓝图按书写名登记，
   TemplateDecl 在 processNamespaceDecl 走 default 不被前缀化；
   `N::B<int>` 与 `B<int>` 归一到同一实例 B_int，缓存键不变）。
   ★ 宽方向：`N::B` 真不存在而全局有 B 时会误绑，记边界。

**回归**：`tests/lang/test_basics_22_stmt_ambig_tentative_parsetype.cpp`
（N::B<Box> y; 语句层声明 + a<b>c 比较链 + 普通类/模板 id/引用/平凡拷贝
回归，rc=0 与 clang 对拍）。**突变验证 2 条**：摘语句层登记表闸 ⇒
`a<b>c` 退回误判声明（原文案复现）；摘 Sema 限定名归一 ⇒ `N::B` unknown
复现——逐条实跑变红、恢复转绿。

**日志漂移**：既有 152 份基线**逐字节零漂移**（含所有错误用例——
`a<b>c` 修正没碰任何既有用例的消息）；基线 +1 新文件 → 153。
ctest 346 全绿，build 0 警告。

**遗留同类现场**：类体方法/字段判定（parser.cpp ~1862）与顶层
parseDeclaration（~890）两处试探仍各自手写（CLAUDE.md B32 批次登记的
"[parse:type] 成对重复"），单独成批再收。

**教训**

（无）
## B36. 限定模板名 `N::B<...>` 误绑全局模板 B（Sema 尾剥无作用域裁决）

- severity: high ｜ status: fixed ｜ related: docs/learn/47, tests/lang/test_basics_24_name_registry_negative.cpp, tests/unit/test_name_registry.cpp

**来源**：「parser 阶段实现类似 clang 的语义识别」需求的缺口盘点
（docs/specs / docs/designs/parser-stage-name-recognition，2026-10-23 签字）。
B35 修语句层时给 resolveType 补的限定名归一是**裸尾剥**：取 `::` 末段
查【全局】蓝图表——N::B 真存在时语义正确，真不存在时有洞。

**现象**

```cpp
template<class T> struct B { T w; };   // 全局模板 B（陷阱）
namespace N {}                          // N 里【没有】B
int main() {
    N::B<int> y;      // clang rc=1；minicc（修前）尾剥误绑全局 B，rc=0
    return 0;
}
```

| 程序 | minicc（修前） | clang++-18 |
|---|---|---|
| `N::B<int> y;`（N 无 B，全局有模板 B） | ❌ 误绑全局 B_int，rc=0（方向性错） | ✅ rc=1 `no template named 'B' in namespace 'N'; did you mean simply 'B'?` |
| `N::B<Box> y;`（N 真有模板 B） | ✅ rc=0 | ✅ rc=0 |

**性质**：**静默走错路**——不存在的限定名落到同名全局模板上，报错指向
派生类型或干脆编译通过（依赖实参形态），根因是「名字在哪个作用域当真」
这个判据没有作用域维度。

**根因**：B31 登记表只有「是否模板」一个 bit 且不带作用域；B35 的 Sema
尾剥按 clang 的 QualifiedNameLookup 形状取了「沿作用域链找蓝图」的
**结果**（归一裸名），却没取它的**判据**（作用域匹配）。判据与消费点
分离，宽方向洞留下。

**修法**（docs/designs/parser-stage-name-recognition.design.md 子流程 3.4）：
登记表升级为多 bit + 作用域键（新模块 include/name_registry.h +
src/name_registry.cpp：`NameClassification` 对齐 clang NameClassification
骨架、`classifyQualified(scope,name)` 对齐 Sema::isTemplateName 的
查询形状），Sema 尾剥改 **classifyQualified 精确裁决**：存在
(scope=N, name=B, template 位) 才归一裸名；配不上 ⇒ 报错
`no template named 'B' in namespace 'N'`（与 clang 逐字，did-you-mean
建议不做——spec OOS 5）。Parser 与 Sema 共享同一张 const 表
（spec 拍板 2：判据单点，红线 3）。单测路径 setNameRegistry 未注入时
维持 B35 近似（兼容缺省）。

**回归**：
- `tests/lang/test_basics_23_name_registry.cpp`（正例：裸 S; 判声明 +
  同名跨类声明优先 + typedef 名 + M::B<int> 归一，rc=0 与 clang 对拍）
- `tests/lang/test_basics_24_name_registry_negative.cpp`（本条的复现，
  rc=1 文案逐字对齐）
- `tests/unit/test_name_registry.cpp`（NameRegistry.* 13 例：收集五规则、
  位并集、scope 精确匹配、value 位恒 false、declval 种子）
- **突变验证 3 条逐条实跑变红**：① 摘语句层 type 位判定 ⇒ 正例
  [parse:stmt] 行 3→0；② 裁决回退尾剥 ⇒ 负例报错消失、误绑复活 rc=0；
  ③ struct/class 不置 type 位 ⇒ S; 不再判声明 + 单测 TypeBit 红。

**有意日志漂移**（2 处，已重刷基线 153→155 份）：
- tmpl_54 的 unknown-type-name 括注改写（旧文案「跳过 <...> + 回滚」
  自 B35 起因过时误导排查）。
- 新增 basics_23/24 两份日志入列。

**教训**

**附带修复**：test_expr_parser 的 ExprParserTemplateId 套件自 B31 起
其实**一直未真正通过**——B31 加了登记表第一层闸，而该套件的 snippet
（`return foo<int>(x);`）从未声明过 foo；当时 ctest 跑的是**陈旧
unit_tests 二进制**，绿色是假象（与 logdiff「基线会把失败固化成契约」
同族的第三种面具：陈旧产物掩盖失败）。修复：parseExpr 辅助函数前导
声明 `template<class T> T foo(T);`（同时守住 a<b 未登记 ⇒ 比较链的
负向用例），声明取下标从 [0] 改末位。重建后 362/362 全绿。

---

## B37. 裸类型名声明的可见性无人裁决：非法程序静默通过（判据答错了问题）

- severity: medium ｜ status: fixed ｜ related: docs/learn/48, tests/lang/test_basics_26_bare_type_name_namespace_not_visible_error.cpp, tests/unit/test_name_visibility.cpp

**来源**：用户提问「对于嵌套类呢，感觉有问题，比如 A 里面有个 B，外面在有一个 B」
引出的实测（docs/learn/48 §1）。起点是"嵌套类在 minicc 里压根不支持"，
但顺着查下去发现同一族的问题**今天就能触发**。

**现象**

```cpp
namespace N { struct S { int x; }; }   // S 只在 N 内可见
int main() {
    S;                      // clang rc=1；minicc（修前）rc=0 ★ 非法程序通过
    return 0;
}
```

| 程序 | minicc（修前） | clang++-18 |
|---|---|---|
| 命名空间里的类型，全局裸 `S;` | ❌ rc=0（静默放行） | ✅ rc=1 `use of undeclared identifier 'S'` |
| 类内别名，全局裸 `B;` | ❌ rc=0（静默放行） | ✅ rc=1 `use of undeclared identifier 'B'` |
| 可见的类型裸 `S;`（全局） | ✅ rc=0 | ✅ rc=0（clang 仅 warning） |

**性质**：**静默放行非法程序**（方向性错，与 B31「漏收=方向性错」同族）。

**根因**：**判据答错了问题**。语句层裸名判定用的是名字登记表（NameRegistry）
的 type 位，而 `classify()` 对裸名是"对任意作用域 OR"的宽近似 ⇒ 它回答的是
"**本 TU 某处**声明过这个类型吗"，而 [stmt.ambig] 需要的是"**此处**可见吗"。
两个问题被当成了同一个：
- 登记表 scope 键只到**命名空间**一级，类体内的名字归外层（spec 异常 1 的"宽方向"）；
- 裸名查询跨作用域取并集 ⇒ 命名空间里的类型在全局也"命中"；
- 命中即判声明 ⇒ 产出**无名** `EmptyStmt` ⇒ Sema 侧无任何落点（`visit(EmptyStmt&)`
  此前不存在）⇒ 没有任何环节能再发现"它其实不可见" ⇒ 静默。

**修法**（docs/designs/visible-name-lookup.design.md）：
**可见性裁决下沉到 Sema**，判据单点化：
1. `SemanticAnalyzer::lookupVisibleType(name)` —— 名字可见性判据（单点）：
   当前类作用域 typeAliases → 命名空间链（**逐级外层**）→ 全局；
   `isNameDeclared(name)` —— "这个名字声明过吗"（任意 kind）。
2. Parser 侧只给**候选**：裸名分支产出 `EmptyStmt{typeName="S"}`（名字进 AST），
   **不下"可见"的结论**（Parser 没有符号表）。
3. `SemanticAnalyzer::visit(EmptyStmt&)` 新增落点：命中 ⇒ 合法；声明过但不是类型
   ⇒ 走表达式路径（clang 是 warning expression result unused）；都没声明 ⇒ 报错
   `use of undeclared identifier 'X'`（**与 clang 逐字，含位置**）。
4. `resolveType` 的三段手写兜底与 `visit(ExprStmt)` 的 [stmt.ambig] 折叠
   **一并收敛**到 `lookupVisibleType`（红线 3 判据单点）——原先这三处各写一份
   查找，正是"同一判据写两处必不一致"的第三现场。

**顺带修好的三处**（原先都**拒收合法程序**）：
- `namespace A { struct S{}; namespace B { void f(){ S; } } }` —— 命名空间链
  逐级外层（clang 的 DeclContext 链语义）；
- 类模板**实例**的别名在成员函数体内裸用 —— 走 `m_classDecls[实例类]->typeAliases`
  （实例类的别名只克隆进类声明，**没进符号表**）；
- 类内别名遮蔽同名全局类型 —— 类作用域先查（clang 顺序）。

**行为变更（向 clang 靠拢）**：`int S = 5;`（块作用域变量遮蔽全局的类 S）时，
`S;` 从"声明"改判为"**表达式**"（clang: warning expression result unused）——
变量因此第一次被登记进符号表。旧行为是 minicc 独有的偏差。

**回归**：
- `tests/lang/test_basics_25_bare_type_name_visibility.cpp`（正例：全局 / 命名空间内 /
  类作用域别名三处可见 + 遮蔽走表达式，rc=0 与 clang 对拍）
- `tests/lang/test_basics_26_bare_type_name_namespace_not_visible_error.cpp`
  （rc=1，文案与位置逐字同 clang：`28:5: use of undeclared identifier 'S'`）
- `tests/lang/test_basics_27_bare_type_name_class_alias_not_visible_error.cpp`
  （rc=1，`29:5: use of undeclared identifier 'B'`）
- `tests/unit/test_name_visibility.cpp`（NameVisibility.* 7 例：类作用域优先 /
  命名空间链逐级外层 / 不可见报错 / 遮蔽走表达式）
- **突变验证 4 条**（design §5 清单）：摘命名空间链外层 / 摘类作用域优先 /
  摘第二问 / Parser 不带名字 ⇒ 对应用例实跑变红。

**有意日志漂移**（3 处，已重刷基线 155→158 份）：
- basics_23：多两行 `[sema:stmt] …` + 一行 `[resolve] 'S' → int (kind=Variable, …)`
  （遮蔽语义改判为表达式 ⇒ 变量进了符号表）；
- tmpl_54：unknown type name 的括注改写（旧文案自称"已由解析期名字登记表在语法期
  消歧"，B37 后由语义期裁决）；
- basics_25/26/27 三份新日志入列。

**教训**

**元命题（本次一并改写的东西）**：`docs/constraints/00-全局约束.md` 红线 1
由**手段式**改写为**目标式**——"语义裁决权单一：名字的种类与可见性只由 Sema 的
符号表裁定；Parser 可以通过窄的只读查询接口向 Sema 提问并服从其答复
（clang 的 Actions 模式）；不得自行维护第二份名字/作用域表来下结论"。
原措辞把"构造期前向收集的只读名字投影"这个**现方案**写成了红线，顺带禁掉了
clang 的实际做法（`Parser.h:639` 的 `Sema &Actions;`）。详见 docs/learn/48 §3。

---

## B38. 作用域不是名字身份的一部分：限定名模板绑错、同名模板撞实例、嵌套命名空间丢外层

- severity: high ｜ status: fixed ｜ related: docs/learn/49, docs/learn/48, tests/lang/test_basics_28_namespace_template_scope.cpp, tests/unit/test_scope_qualified_names.cpp

**来源**：Stage 1（B37）收尾后的既定路线 —— 红线 1 改写后的三阶段里的
**Stage 2「登记表退场」**。起点是 B37 查实时记下的三处既有缺口之一
（"命名空间模板按裸名注册 ⇒ `N::B` 绑到全局 B"），本批把它连同它的兄弟一起修掉。

**现象**

```cpp
template<class T> struct B { T v; };                    // 全局模板
namespace N { template<class T> struct B { T v; T w; }; } // 同名，不同布局
namespace M { template<class T> struct B { T v; T z; }; }

int main() {
    N::B<int> n; n.w = 3;      // ★ 修前：绑到全局 B ⇒ "No member 'w' in class 'B_int'"
    M::B<int> m; m.z = 5;      // ★ 修前：与 N::B 撞成同一实例 ⇒ "No member 'z'"
    return 0;
}
```

```cpp
namespace A { namespace B { int f() { return 7; } } }
int main() { return A::B::f(); }   // ★ 修前：注册成 "B::f" ⇒ "Undefined variable 'A::B::f'"
```

| 程序 | minicc（修前） | clang++-18 |
|---|---|---|
| `N::B<int>`（N 里有 B，另有全局 B） | ❌ 绑到**全局** B | ✅ 绑 N 里的 B |
| `N::B<int>` 与 `M::B<int>` | ❌ **撞成同一实例** `B_int` | ✅ 两个实例 |
| `A::B::f()`（两层嵌套） | ❌ 注册名 `B::f` ⇒ 未定义变量 | ✅ rc=0 |
| `N::B<int>`（N 里**没有** B，全局有 B） | ❌ 静默绑全局 B（宽方向） | ✅ rc=1 `no template named 'B' in namespace 'N'` |

**性质**：**静默算错**（前两条：字段错位 / 实例被合并）与**拒收合法程序**（第三条）
并存；第四条是**静默接受非法程序**。

**根因**（同一个）：**作用域没有进到名字的身份里**。minicc 的三处实现各自漏了它：

| 落点 | 修前 | 后果 |
|---|---|---|
| `processNamespaceDecl` 前缀化 | 用 `decl->name`（**只取本层**） | 嵌套命名空间丢外层 |
| 模板注册 | 模板走 `default:` 分支，**不参与前缀化** ⇒ 按裸名注册 | 同名模板撞实例 |
| 限定名模板 id 绑定 | 「剥 :: 末段查全局蓝图」 | 绑错模板 |

**教训**

而 clang 的模型一句话：**作用域是名字身份的一部分** —— 模板声明属于 DeclContext，
`LookupName` 按上下文查；mangling 带上下文（`_ZN1N1BIiE` vs `_Z1BIiE`）；
实例缓存键带上下文。

**修法**（spec：docs/specs/scope-qualified-names.spec.md）
1. **前缀化单点**：`processNamespaceDecl` 改用 `m_currentNamespace`（完整路径），
   命名空间成员（函数 / 类 / 全局变量 / 枚举 / **模板**）共用同一句 —— 判据单点。
2. **模板按作用域注册**：类模板 / 别名模板的蓝图名带 `M::` ⇒ 注册键、实例名
   （`M::B_int`）、缓存键（`M::B<int>`）、mangling（`_Z4M__BIiE`）全部带作用域。
3. **限定名不再剥尾**：`N::B` 按完整限定名查表，查不到就报
   `no template named 'B' in namespace 'N'`（**与 clang 逐字**）—— 不再退到全局 B。
4. **登记表退场一处**：Sema 侧的 `classifyQualified` 消费点删除，
   `SemanticAnalyzer::m_names` 与 `setNameRegistry` 一并去掉 ——
   第二份作用域事实只剩 Parser 侧那一张（过渡态，Stage 3 消灭）。
5. **mangling 净化**：`mangleTemplateInstance` 对名字先过 `symbolSafe`
   （与 `mangleRTTI` / `mangleVTable` 同一份），否则 `'::'` 进符号标签 ⇒ 汇编器报 junk。

**行为变更（向 clang 靠拢）**：`N::B`（N 里没有 B，全局有 B）从"静默绑全局 B"
改成**响亮报错** —— 收窄方向（红线 2「宁宽勿窄」的反面：这次是**由宽改严**，
因为旧行为会静默算错，属方向性错）。

**回归**
- `tests/lang/test_basics_28_namespace_template_scope.cpp`（正例：全局 B /
  `N::B` / `M::B` 三个实例并存且字段各自正确，rc=0 与 clang 对拍）
- `tests/lang/test_basics_29_nested_namespace_qualified_call.cpp`
  （正例：`A::B::f()` 与 `A::B::S`，rc=0 与 clang 对拍）
- `tests/unit/test_scope_qualified_names.cpp`（ScopeQualifiedNames.* 5 例：
  嵌套保完整路径 / 同名模板两实例 / 全局与命名空间模板相异 /
  限定名查不到即报错 / mangling 无非法字符）
- **突变验证 4 条**（逐条实跑变红）：① 前缀化改回本层名 ⇒
  `NestedNamespaceKeepsFullPath` 红；② 模板不前缀化 ⇒ 同名模板用例红（3 例）；
  ③ 限定名改回剥尾归一 ⇒ `QualifiedNameNotFoundIsAnError` 红；
  ④ mangling 不净化 ⇒ `MangledNameHasNoIllegalChars` 红。

**有意日志漂移**（已显式列出并重刷基线）
- basics_22 / basics_23：命名空间模板的蓝图名、实例名、mangled 全部带作用域
  （`B` ⇒ `N::B`、`B_int` ⇒ `N::B_int`、`_Z1BIiE` ⇒ `_Z4N__BIiE`）；
  旧日志行 `[resolve] 限定模板名 'N::B' ⇒ 裸名 'B'` **消失**（不再剥尾）；
- basics_28 / basics_29 两份新日志入列；
- `_unit_cases.txt` / `_unit_summary.txt`：+7 例（5 个新单测 + 2 个新集成的
  子进程包装）。★ 顺带发现：上一版基线里的 **369** 本身就是
  **没重建的 unit_tests 二进制**留下的假象（learn/47 §6 的第三种失败面具
  这回连**基线**也骗了），重建后是 371，再加本批 5 例 = 376。

**遗留（Stage 3）**：解析期"名字是不是模板"这一问仍由登记表回答（过渡态近似）。
clang 的答案是 Sema 的活符号表（`Actions.isTemplateName`）+ 语法试探兜底
（`isTemplateArgumentList`）—— 需要边解析边 `ActOn*` 登记的架构改造。

---

## B39. 成员调用点没把符号名过净化：命名空间里的类，调用其成员方法链接期炸

- severity: medium ｜ status: fixed ｜ related: src/codegen/codegen.cpp, docs/learn/17, docs/BUGS.md B10

**现象**

```cpp
namespace NS {
struct S { int f() { return 7; } };
}
int main() {
    NS::S s;
    return s.f();     // ★ 修前：汇编期 junk '::S_f' after expression
}
```

**性质**：**编译期全绿、汇编期红**（错误停在 `as`，不在 minicc 自己的诊断里）。

**根因**：**同一处语义判断写了两份，且两份不一致** —— 这正是 B10 记的教训。
调用点（CodeGen 生成 `callq` 目标）直接用 `MemberExpr::resolvedCalleeSymbol`
的原文，而定义点（生成 `.globl`）过了 `asmSymbol()` 净化（`::` ⇒ `__`）。
两处不一致 ⇒ 定义是 `NS__S_f`、调用是 `NS::S_f` ⇒ 汇编器拒绝 `::`。
★ 只有**带作用域**的名字会炸：全局类 `S::f` 的符号里没有 `::` ⇒ 恰好一致
⇒ 此前**没有任何用例写过命名空间里的类的成员方法调用**，这条错法一直藏着。

**修法**：调用点也过 `asmSymbol()`（一处净化，判据单点）——
`funcName = asmSymbol(mem->resolvedCalleeSymbol);`。

**回归**：修复后 `s.f()` 返回 7（与 clang 一致）；既有集成用例**逐字节零漂移**
（该路径此前无人走到）。

**教训**：**"恰好一致"不是"一致"** —— 两份实现只要在某个输入子集上碰巧相同，
就会把错法藏起来，直到有人写出那个子集。判据要收口到**一处**，
而不是"两处都写对了"。

## B40. 变量遮蔽在 Parser 层看不见：同名类型被当成可见，裸名语句判错

- severity: medium ｜ status: fixed ｜ related: src/parse/parser.cpp, docs/learn/52, docs/BUGS.md B37

**现象**

```cpp
struct S { int x; };
int main() {
    int S = 5;      // ★ 块作用域的变量遮蔽了全局的类 S
    S;              // ★ 修前：判为【声明】（无声明符声明）；修后：表达式
    if (S != 5) return 1;
    return 0;
}
```

**性质**：**判错但结果侥幸正确**（旧路径把 `S;` 当成无声明符声明，不发射代码，
运行结果与 clang 相同）—— 属"静默走错分支"型缺陷：日志与 AST 形状错，
但因为那条语句没有副作用，行为上看不出来。

**根因**：语句层的消歧判据问的是**名字登记表**的 type 位，而那张表是
**前向扫 token 流**得到的**跨作用域并集** —— 它分不出"这里有个变量遮蔽了同名类型"
（[basic.scope.hiding]）。而 tests/lang/test_basics_23 的注释**早就写着**
"块作用域的变量 S【遮蔽】全局的类 S ⇒ 此处的 `S;` 是【表达式】不是声明"——
★ 实现与用例文档互相矛盾，靠对拍才暴露。

**修法**：判据换成 Sema 的**活作用域表**（Stage 3 批 2+3）：问的是
"这个名字在此处**是不是一个值**"——是值 ⇒ 表达式；不是 ⇒ 声明候选
（可见性仍由 Sema 的 `lookupVisibleType` 裁决）。活表登记了块作用域里的
局部变量 ⇒ 遮蔽可见。

**回归**：tests/lang/test_basics_23（现场②现在没有 `[parse:stmt]` 行、
汇编多一条 `movq` 加载局部变量）+ tests/lang/test_basics_25（现场④同款）；
单测 tests/unit/test_parse_scope.cpp 的 `ParseScopeRetirement.ValueBitDistinguishesShadowingFromTypes`。

**教训**：**用例的注释也是契约** —— 注释说"应该是表达式"而实现说"是声明"，
说明二者有一个在说谎；只跑"结果对不对"看不出问题（两者运行结果相同），
要靠**读用例意图 + 对拍**才抓得到。

## B41. 模板名不是类型名，但仍是可见的名字：`Box;` 被拒收（合法程序被拒）

- severity: medium ｜ status: fixed ｜ related: src/sema/semantic_analyzer.cpp, docs/learn/52

**现象**

```cpp
template<class T> struct Box { T v; };
int main() {
    Box;              // ★ 修前：use of undeclared identifier 'Box'（rc=1）；修后：rc=0
    return 0;
}
```

**性质**：**拒收合法程序**（clang 只给 `-Wmissing-declarations` warning，rc=0）。

**根因**：裸名声明的裁决分三问（`SemanticAnalyzer::visit(EmptyStmt&)`）：
① 是不是**可见的类型**？② 是不是声明过的名字？③ 都不是 ⇒ 响亮拒收。
第一问查的是 `lookupVisibleType` ⇒ 走符号表 ⇒ **模板名不在符号表里**
（模板的蓝图在 `m_classTemplates`）⇒ 第一问答否；
第二问 `isNameDeclared` 也只查符号表 ⇒ 同样答否 ⇒ 落到第三问 ⇒ 报错。
★ 根子是**只问了"是不是类型"，没问"是不是模板名"** —— 而模板名虽然不是类型名，
却是**可见的名字**（[basic.scope.scope]：模板名与类型名不同类，但都能作声明）。

**修法**：加第三问 `isVisibleTemplateName`（判据单点：类模板 / 别名模板蓝图各一张表），
与表达式语句折叠那条路径共用同一句判据（原先那里写的是内联的两张表.count）。

**回归**：tests/lang/test_basics_31（`Box;` 与 `N::B;` 两条，rc=0；
日志含 "[sema:stmt] 'Box' 可见的模板名 ⇒ 判为无声明符声明"）。

**教训**：**"X 不是 Y"不等于"X 什么都不是"** —— 一条判据只问了"是不是类型"，
就把"是模板名"这一类合法输入漏在了门外。判据要覆盖**问题的完整分类**，
而不是覆盖"最常见的那一类"。

## B42. 构造函数符号在 CodeGen 硬拼：限定名类一撞就错配（链接期才炸）

- severity: high ｜ status: fixed ｜ related: src/codegen/codegen.cpp, src/sema/semantic_analyzer.cpp, docs/learn/53, docs/specs/nested-class.spec.md

**现象**

```cpp
namespace N {
struct S { int v; S() { v = 3; } };
}
int main() {
    N::S s;        // ★ 修前：undefined reference to 'N__S_N__S'（rc=1，链接期）
    return s.v;
}
```

**性质**：**编译期全绿、链接期炸** —— 语义判定写在两处且不一致：
定义点（Sema `registerFunction`）给成员方法名追加"参数个数"后缀，
调用点（CodeGen）硬拼 `类名_类名`。限定名类（命名空间 / 嵌套类）两边算出的
符号不一样 ⇒ 链接期 undefined reference。★ 与 B10 / B20 / B22 **同款**——
这是第四次为"名字的规则"付学费。

**根因**：**"该调哪个构造函数"是语义信息，却让 CodeGen 自己按名字猜**。
构造函数的定名是**有状态**的（同名多参追加 `_N` 后缀），
调用点不可能只靠"类名 + 实参个数"重算出来。

**修法**：照 clang `Sema::BuildCXXConstructExpr` —— **Sema 选定并回填**，
CodeGen 只发被选中的符号：
① `VarDeclStmt::ctorSymbol` / `NewExpr::ctorSymbol` 两个回填槽；
② `inferNew` 与 `visit(VarDeclStmt)` 在选定构造函数时写入 `method->mangledName`；
③ CodeGen 两处构造调用点优先用回填值，空则退回旧硬拼（兜底，不是主路径）。

**回归**：tests/lang/test_basics_32（正例含 `A::B b1;` / `new A::B(6)`）；
探针 `namespace N{struct S{S(){};};} N::S s;` 由 rc=1（链接错配）变 rc=0（取值 3）。

**教训**：**"名字的规则"每多一处作用域就多付一次学费** ——
本条与 B10/B20/B22 构成同一个模式：**同一处语义判断写了两份**。
★ 修法不该是"再写一份对得上"（那只是把两份错成一致），
而是**让规则的持有者唯一**（Sema 选，CodeGen 发）。

## B43. 嵌套类完全不支持（合法程序被拒）

- severity: medium ｜ status: fixed ｜ related: src/parse/parser.cpp, src/sema/semantic_analyzer.cpp, docs/learn/53, docs/specs/nested-class.spec.md

**现象**

```cpp
struct A {
    struct B { int v; };     // ★ 修前：Parse Error "Expected type name"（rc=1）
};
int main() { A::B b; return 0; }
```

**性质**：**拒收合法程序**（clang rc=0）。用户的原始提问
（"class A 里面有个 B，然后外面还有个 class B，感觉有问题"）在
Stage 1~3 批 1~3 之后仍未落地。

**根因**：类作用域只实现了"当前类的一张**类型别名**表"
（`ClassDecl::typeAliases`），**类作用域链**本身不存在 ⇒
嵌套类既无处登记、也无处查找。

**修法**（Stage 3 批 4，四处落点）：
① **Parser**：`parseClassDecl` 的成员循环遇 `class`/`struct` ⇒ 递归解析，
收进 `decl->nestedClasses`（不建第二张作用域表 —— 名字进的是当前类作用域）；
② **Sema 登记**：`processClassDecl` 在**字段解析之前**把嵌套类前缀化
（登记名 = 外层类登记名 + `::` + 裸名）并递归处理；
③ **类作用域链**：`lookupVisibleType` 的 ① 分支扩成链
（类自身 → 类内别名 → 嵌套类 → 沿 `ownerClassName` 外溯一级）；
④ **查找顺序**：`resolveType` 改为 **类作用域 → 命名空间链 → 全局**
（原为全局符号表优先 ⇒ 嵌套类 `A::B` 与全局 `B` 并存时，
A 的成员函数里的裸名 `B` 被绑到**全局 B** —— 不报错，只是算错）。

**回归**：tests/lang/test_basics_32（正例 12 个检查点，rc=0，与 clang 逐点一致）
+ tests/lang/test_basics_33（负例 rc=1，文案与 clang 逐字）
+ 单测 tests/unit/test_nested_class.cpp（`NestedClass.*` 7 例）。

**教训**：**"作用域"不是一张表，是一条链** —— 把它当表实现，
每加一层作用域（命名空间 → 类 → 嵌套类）就要重做一遍；
把它当链实现，加一层只是链上多一环。
★ 本条的真正价值不在"支持了嵌套类"，而在**它把三批（Stage 1/2/3）攒下的
作用域基础设施一次收口**：可见性、作用域身份、活作用域链，三者合起来
才让嵌套类成为自然结果，而不是一个孤立的特判。

## B44. 类自身的名字在本类作用域内不可见（自引用指针被拒）

- severity: medium ｜ status: fixed ｜ related: src/sema/semantic_analyzer.cpp, docs/learn/53

**现象**

```cpp
struct A {
    A* p;              // ★ 修前：unknown type name 'A'（rc=1）；修后：rc=0
    int v;
};
int main() { A a; a.v = 1; return 0; }
```

**性质**：**拒收合法程序**（clang rc=0）。自引用指针是链表 / 树的
**极常见**惯用法。

**根因**：[basic.scope.class]/2.1 —— "类名被插入到本类作用域里" ——
minicc 的类作用域查找只问"类里有没有叫这个名字的**类型别名**"
（`typeAliases`），**没问"这个类自己叫什么"**。
★ 判据只覆盖了问题的**一部分**（与 B41 同款根子：只问了"是不是类型"）。

**修法**：`lookupVisibleType` 的类作用域链上，每个位置先问一句
"可见名 == 名字吗"（可见名 = 登记名的最后一段：`N::A::B` 的可见名是 `B`）⇒ 命中即本类。
★ 连带让"嵌套类在自己的作用域里能用裸名指自己"成立。

**回归**：tests/lang/test_basics_32（正例含 `Node { Node* next; };`）
+ 单测 `NestedClass.ClassNameVisibleInOwnScope`。

**教训**：**一条查找判据要覆盖名字在一个作用域里的【全部形态】** ——
类型别名 / 嵌套类 / 类自身 / 枚举项 / static 成员，各是各的形态；
少问一种，落在那种形态上的合法程序就会被拒。

## B45. vtable 槽的符号在两处各算一遍：同一方法两个符号（链接期错配）

- severity: high ｜ status: fixed ｜ related: src/ast/type.h, src/sema/semantic_analyzer.cpp, src/codegen/codegen.cpp, include/ast.h, docs/learn/54, docs/specs/vtable-slot-handle.spec.md

**现象**

```cpp
struct Outer {
    struct Inner { int v; };
    virtual int f(Inner i) { }   // 形参类型 = 嵌套类
    virtual int f(int i) { }     // 同名同个数 ⇒ 符号要挂形参类型链
};
int main() { Outer o; return 0; }
```

★ 修前的产物（探针实测）：

```asm
    .globl Outer_f_1_OuterCCInner      # 定义点（Pass 2，形参类型已解析）
    .quad Outer_f_1_Inner              # vtable 槽（Pass 1，形参类型未解析）
```

⇒ `[LINK ERROR] undefined reference to 'Outer_f_1_Inner'`（rc=1，链接期）。

**性质**：**链接期才炸的错配**（编译期全程绿灯）。clang rc=0（合法程序）。

**根因**：vtable 槽里存的是**符号串**，而那个串在 **Pass 1**（`processClassDecl`）
与 **Pass 2**（`registerFunction`）**各算了一遍**。两处虽共用
`memberMethodSymbolName`（B20 的收拢），但看到的世界不同：

| 时刻 | 形参类型节点 | `toString()` |
|---|---|---|
| Pass 1 | 未解析的 `Class("Inner")` | `Inner` |
| Pass 2 | 已解析的 `ClassType("Outer::Inner")` | `Outer::Inner` |

★ 只有**同名同个数**的重载才会用到形参类型链（B22）⇒ 触发条件窄，
此前无人写过这种用例 ⇒ 一直没暴露。

**修法**：**槽里不再存符号串，改存声明句柄**（`VTableEntry::method`）——
符号由唯一定名点 `registerFunction` 写在 `decl->mangledName` 上，
槽发射时从句柄取（`vtableEntrySymbol()`）。三个填槽落点（新槽 / 主表覆写 /
次表覆写）只写句柄。★ 对照 clang：`VTableComponent::getFunctionDecl()`
返回的正是 `CXXMethodDecl *` —— 槽里本来就该是声明，不是名字。

**回归**：tests/lang/test_basics_34（正例含本现场）
+ 单测 `tests/unit/test_vtable_handle.cpp`（6 例 `VTableHandle.*`，
断言"槽 → 声明 → 符号三者同源"）+ 突变验证 5/5。

**教训**：★ **"两处共用同一个函数"不等于"消除了重复"** —— 两处调用发生在
不同时刻、看到的状态不同，就还是会漂（B10 → B20 → B22 → B45 是同一个坑的
第四次现形）。治本只有一条：**只算一次，别处只引用**。
★ 另一条：**自指式断言测不出这类缺陷** —— 第一版单测把期望值取自
被测的 accessor 本身（"自己验自己"），突变 m5 因此全绿；
期望值必须来自与被测物【无关】的来源（这里取"声明自己的 mangledName"）。

## B46. 析构槽靠符号子串判定：`my_dtor_helper` 被误认成析构（静默少调一次）

- severity: medium ｜ status: fixed ｜ related: src/codegen/codegen.cpp, docs/learn/54

**现象**

```cpp
int g_dtorCalls = 0;
struct Widget {
    virtual int my_dtor_helper() { return 7; }   // ★ 名字含 "dtor" 但不是析构
    virtual ~Widget() { g_dtorCalls = g_dtorCalls + 1; }
};
int main() {
    { Widget w; int r = w.my_dtor_helper(); if (r != 7) return 1; }
    return g_dtorCalls == 1 ? 0 : 2;              // ★ 修前返回 2（析构没跑）
}
```

★ 修前：作用域结束时的析构调用打在**槽 0**（`my_dtor_helper`）而不是槽 1
（析构）⇒ 真正的析构函数**根本没被调用** ⇒ `g_dtorCalls` 恒为 0。
**静默少调一次析构**：程序不报错、不崩，只是资源没释放。

**性质**：**静默算错**（子串匹配的经典误判）。

**根因**：析构槽的判定写成了"符号串里有没有 `dtor` 子串"
（`entry.mangledName.find("dtor") != npos`）。★ 槽里存符号串时，
"这个槽是不是析构"只能靠猜；句柄化之后**看声明名**即可
（`name.starts_with("~")`）⇒ 假阳性从根上消失。
★ 对照 clang：`VTableComponent::isDestructorKind()` 看的是**声明的种类**
（`CXXDestructorDecl`），同样不是看名字。

**修法**：`isDtorSlot(entry)` = `entry.method->name.starts_with("~")`
（`src/codegen/codegen.cpp`），两个消费点（栈对象析构 / `delete`）共用。

**回归**：tests/lang/test_basics_34（第 ② 组：`g_dtorCalls == 1`）
+ 单测 `VTableHandle.VirtualDtorCallTargetsTheDtorSlot`
（断言析构的虚调用打在 vtable[1]，不是 vtable[0]）。

**教训**：★ **"用符号串反推语义"是本项目的头号反模式**（B10/B13~B15/B22/B45
同族）—— 符号串是**给人看的**，语义判定要落在**声明**上。
★ 附带一条：子串匹配的错误方向往往是**静默**的（少调一次函数没人会发现），
⇒ 这类判据必须有"钉住可观测后果"的用例，而不是只断言"没有报错"。

## B47. 类模板里的嵌套类不支持：蓝图不克隆成员（合法程序被拒）

- severity: medium ｜ status: fixed ｜ related: src/sema/template_instantiation.cpp, src/sema/semantic_analyzer.cpp, docs/learn/56

**现象**

```cpp
template <class T> struct Box {
    struct Inner { T v; };      // ★ 嵌套类也是外层类的成员
};
int main() {
    Box<int>::Inner i;          // ★ clang rc=0；minicc 报 no type named 'Inner' in 'Box_int'
    return 0;
}
```

★ 修前：minicc rc=1，`[Semantic Error] no type named 'Inner' in 'Box_int'`；
clang rc=0（`Box<int>::Inner` 是合法类型）。
**性质**：**功能缺失**（合法程序被拒）—— 比"静默算错"好，但仍是缺口。

**根因**：实例化一个类模板时只搬了"顶层四样"
（字段 / 方法 / 类型别名 / static 数据成员），**没有搬嵌套类**。
★ 而嵌套类按 [class.nest]/1 也是外层类的成员，按 [temp.inst]/5
应当随实例化一起克隆 —— 少搬一项，产出的就是一张**缺项的成员表**。
★ 与 learn/26（命名空间递归漏方法）是同款：**DeclContext 树本身就是递归的，
扁平遍历必然漏**（那次漏在 Pass 2/3，这次漏在实例化本身）。

**修法**（三处，缺一不可）：

1. **实例化新增步骤 5.8（克隆嵌套类）**：字段 / 方法 / static 成员 / 类型别名 /
   成员模板全部随形参替换，嵌套类**递归**克隆；产物挂进实例类的 `nestedClasses`
   （对照 clang 的 `TemplateDeclInstantiator::VisitCXXRecordDecl` → `Owner->addDecl`）。
2. **登记名不在克隆处定**：克隆只交**裸名**（`Inner`），前缀化（`Box_int::Inner`）
   仍由 `processClassDecl` 统一做 ⇒ "登记名 = 外层登记名 + :: + 裸名"只有那一份
   （learn/49 的"作用域即身份"；理由见 learn/56 §2.3）。
3. **Pass 2 / Pass 3 必须递归进嵌套类**：`getOrInstantiateClass` 里扁平的
   `for (auto& method : instance->methods)` 改为递归原语 `forEachClassMethod`
   —— 嵌套类的方法**不在**外层类的 `methods` 表里，不递归 ⇒ 不注册、不分析 ⇒
   CodeGen 从没收到那些方法 ⇒ **链接期 undefined reference**。

**回归**：tests/lang/test_basics_35（正例，rc=0，与 clang 对拍）
+ tests/lang/test_basics_36（负例：`Box<int>::Missing` 仍响亮拒收）
+ 单测 `NestedClassInTemplate.*` 6 例 —— 断言的是**性质**而非拼写
（被克隆 / 字段随形参替换 / 两个实例的嵌套类互不干扰 / 方法被注册 /
嵌套套嵌套 / 边界不放大），命名规则随便改都不会误报。

**教训**：★ **"少做一步"与"做错一步"症状完全不同** ——
少做（漏克隆）⇒ 响亮报错（还算幸运）；做错（替换没做、名字算两遍）⇒ **静默算错**。
★ 本批真正的收获是两条可复用判据：
**"成员表"是递归结构，遍历它就必须递归**；
**"只算一次，别处只引用"**（登记名前缀化只有一处，不给"两处各算一遍"留缝）。

---

## B48. 成员赋值：右值求值踩掉对象地址 ⇒ 写字段写到了错的地址（★ 静默算错）

- severity: high ｜ status: fixed ｜ related: src/codegen/codegen.cpp, docs/learn/57

**现象**

```cpp
struct S { int v; };
int main() {
    S s;
    s.v = 3;
    s.v = s.v + 4;      // ★ clang rc=7；修前 minicc SIGSEGV（rc=139）
    return s.v;
}
```

**性质**：**静默算错 / 段错误** —— 二元表达式把右操作数挪进 `%rcx`（常量 4），
于是 `movl %eax, 0(%rcx)` 写到地址 `0x4` ⇒ 段错误；
若 `%rcx` 恰是别的合法地址 ⇒ **静默踩坏别人的内存**（无任何症状）。

**根因**：`visit(AssignStmt)` 的成员分支（`NodeKind::Member`）是这条顺序：

```
emitExpr(mem->object);              // 对象地址 → rax
emit("movq %rax, %rcx");             // 暂存到 %rcx
emitExpr(stmt.value);                // ★ 右值求值 —— 会踩 %rcx
... emit("movl %eax, 0(%rcx)");      // ★ 用 %rcx 里的（已被冲掉的）地址写入
```

★ `%rcx` 是本实现的**临时周转寄存器**（二元表达式拿它放右操作数；
函数调用按约定可踩 caller-saved 寄存器，含 `%rcx`）⇒
"算一次地址"的前提"地址活到写下去"不成立。
⚠ 与"右值求值两次"那个老简化**不是一回事**：那是**求值次数**问题，
这是**寄存器存活期**问题 —— 两者都在同一条语句上，极易混为一谈。

**修法**：对象地址先 `pushq %rcx`，右值求完再 `popq %rcx` 取回：

```cpp
emitExpr(mem->object);
emit("movq %rax, %rcx");        // 对象地址存入 rcx
emit("pushq %rcx");             // ★ 保护对象地址（右值求值会踩 rcx）
emitExpr(stmt.value);
emit("popq %rcx");              // ★ 对象地址回 rcx
```

**回归**：单测 `MemberAssignClobber.*` 3 例（二元值 / 调用值 / 常量值 ——
第三条钉住"保护是无条件加的"，不靠"这个右值恰好不踩 rcx"蒙对）
+ 集成 `tests/lang/test_basics_37_return_class_by_value.cpp` 检查点 ②。

**教训**：★ **"只算一次"不等于"算的值活得久"** —— 寄存器分配之外，
凡是"跨表达式求值保存东西"的地方，都要问一句"这中间 %rcx 会不会被踩"。
★ 本条是**做 sret 特性时撞出来的**：要写"拿到返回的对象再改它的字段"，
就必然走到这条路径 —— 特性与缺陷常在同一条语句上相遇。

---

## B49. 按值返回类对象：初始化式被丢弃 ⇒ 对象退化成默认构造（★ 静默算错）

- severity: high ｜ status: fixed ｜ related: src/codegen/codegen.cpp, src/sema/semantic_analyzer.cpp, include/ast.h, docs/learn/57

**现象**

```cpp
struct S { int v; };
S f() { S s; s.v = 7; return s; }
int main() { S x = f(); return x.v; }   // ★ clang rc=7；修前 minicc rc=0
```

**性质**：**静默算错** —— 编译全绿、链接全绿，运行给出**别的数**（0）。
★ 这是 ROADMAP §0.1 批 0-1 点名的头号缺口（"按值返回 + 拷贝省略"）。

**根因（两层，缺一不可）**：

1. **语义层**：变量声明处理只认三种初始化形态 —— 构造调用升级 / 平凡拷贝 /
   直接初始化。"**调用表达式返回类对象**"这一形态没被认出来 ⇒
   落到"无初始化的类声明"分支 ⇒ **初始化式被丢弃**，对象按默认构造生成。
2. **代码生成层**：即便认出了形态，也没有"返回缓冲"这回事 ——
   **类对象没有标量值表示**（首 8 字节可能是 vptr、可能是字段），
   按值返回它不能用 rax 装值 ⇒ 必须走 sret（调用者给缓冲、被调者拷进去）。
   ★ 本实现早有一条约定"**类值在表达式里 = 地址**"（`emitClassAddress`），
   它与 sret 互为因果：正因为值是地址，返回时才必须给一块缓冲。

**修法（三处，缺一不可）**：

1. **语义层落标志**：`VarDeclStmt::initIsSretCall`（`include/ast.h`）——
   判据是"调用的 `resolvedType` 剥掉 const 后是不是类"，落点唯一。
2. **代码生成走 sret**：
   · 定义点（`emitFunction`）：序言 spill 隐藏的第 0 参数（rdi）⇒
     显式实参整体右移一位（成员方法的 this 再右移一位，走 rsi）；
   · 返回路径（`visit(ReturnStmt)`）：把对象表示**逐字节拷进缓冲**
     （`rep movsb`），rax 回填缓冲地址；★ 拷贝必须在局部对象析构**之前**；
   · 调用点（`visit(CallExpr)`）：备缓冲（拷贝省略时 = 变量槽，
     否则 = 帧临时区）⇒ 装进 rdi；虚调用同款（`emitVirtualCall` 的 sret 形态）；
   · 帧大小要把那块缓冲算进去（`countSretBytes`，与构造临时对象**分开计数**
     ⇒ 现存程序帧大小逐字节不变）。
3. **拷贝省略**（[class.copy.elision]/8）：`S x = f();` 的返回缓冲
   **就是 x 的槽** ⇒ 不清零、不装 vptr、不调构造函数 ——
   返回值本身就是那个完整对象（含它自己的 vptr）。

**回归**：单测 `SretReturn.*` 7 例（定义点 spill / 调用点装 rdi /
**拷贝省略的缓冲 = 变量槽** / 寄存器位移 / 成员 this 让位 /
**返回引用不走 sret** / 帧盖住缓冲）
+ 集成 `tests/lang/test_basics_37_return_class_by_value.cpp`（九个检查点：
自由函数 / 改字段 / 多字段类 / 成员方法 / 虚方法 / 多态类 / 类模板实例 /
嵌套调用 / 返回值直接进表达式），全部与 clang++-18 对拍一致。

**教训**：★ **判据单点在 ABI 层的应用**：调用点与定义点必须读**同一个 `Type`**
（"返回类型是不是类"）⇒ 两侧天然一致，不会失配。
⚠ 两处各判一次必漂 —— 这是 B10 → B20 → B22 → B45 同一个坑的**第五次**现形。
★ **哨兵不能用"偏移 >= 0"**：rbp 偏移全是负数 ⇒ 真槽位会被当成"没指定"
（本批踩了两次：`m_sretTargetSlot` 与 `emitVirtualCall` 的 `sretBuf`）⇒
"没有"与"某个负偏移"必须能区分（用 `INT_MIN` 哨兵）。

---

## B50. 全局类对象：存储只按 8 字节分配 + 类值的地址约定未在全局路径生效（★ 段错误，未修）

- severity: high ｜ status: open ｜ related: src/codegen/codegen.cpp, docs/learn/57 §7

**现象**

```cpp
struct S { int v; };
S g;                       // ★ 全局类对象
int main() { g.v = 11; return g.v; }   // ★ clang rc=11；minicc SIGSEGV
```

按引用返回类对象同样中招：

```cpp
S g;
S& pick() { return g; }
int main() { g.v = 11; return pick().v; }   // ★ clang rc=11；minicc SIGSEGV
```

**性质**：段错误（响亮）—— 但 `g.v = 11` 这么基本的写法就崩，
**影响面比看上去大**。⚠ 若 `%rax` 恰是别的合法地址，就是静默踩内存。

**根因（两处，缺一不可）**：

1. **存储尺寸**：全局变量发射时，除数组外一律 `.quad 0`（8 字节）⇒
   类对象的真实尺寸（`classLayout.totalSize`）没算进去 ⇒ 存储本身就偏小。
2. **地址约定**：本实现约定"**类值在表达式里 = 对象地址**"，
   但它只在**局部**类对象路径（`m_classLocals` ⇒ `leaq`）生效；
   **全局**路径仍走通用量加载（`movq gref(%rip), %rax`）⇒ 拿到的是
   对象首 8 字节（字段值/vptr）而不是地址 ⇒ 随后的 `movl 0(%rax)`
   解引用的是那个值 ⇒ 段错误。

**为什么本批没修**：① 两处根因要一起改（只改地址约定 ⇒ 存储仍然偏小）；
② 改全局存储布局会影响所有带全局变量的程序 ⇒ 需要单独一批 + 重刷基线；
③ 与本批（按值返回）**不是同一条线** —— 它是"类对象作为全局变量"的缺口。

**回归（修后要补）**：全局类对象的字段读写 / 全局类对象按值返回 /
按引用返回类对象 —— 三条都要与 clang 对拍。

**教训**：★ **一条约定要覆盖【所有】载体**：局部类对象按地址、全局类对象按值
⇒ 同一个"类值"有两套表示。约定写在**产生值的地方**（`visit(VarExpr)`）
才不会有第二份。

---

## B51. `模板id::成员;` 语句被无声明符声明分支整个吞掉（★ 静默接受非法程序，未修）

- severity: medium ｜ status: open ｜ related: `src/parse/parser.cpp` B16 分支（~2427-2431）

**来源**：代码审查语句层前瞻的 B16 分支（"`模板id;` ⇒ 无声明符声明"）时确认的判据缺一半——
**裸名路径（B37）已经问过"是值吗"，嵌套名路径没问**。

**复现**

```cpp
template<class T> struct Box { static const int count = 7; using type = T; };
int main() {
    Box<int>::count;      // 静态【数据】成员访问——是表达式语句
    return 0;
}
```

| 程序 | minicc | clang++-18 |
|---|---|---|
| `Box<int>::count;` | rc=0，但 parse 日志显示被当成**类型**解析（`nested name: Box<int>::count` → `final type`）→ B16 分支判为无声明符声明 → **EmptyStmt 整个吞掉**，表达式语句被静默消除 | rc=0（真按表达式处理） |
| `Box<int>::typo;`（拼错成员名） | **rc=0 静默接受非法程序** | ❌ `no member named 'typo' in 'Box<int>'` |

**性质**：`Box<int>::后面跟任何名字;` 都被吞——B16 分支**不验证链尾名字是否真是类型成员**。
静态成员读取无副作用所以第一条"恰好没算错"，第二条（typo）是实锤危害。
非模板限定名 `S::typo;` 同病（落到裸名分支后查的也是末段名，类内 static 成员名若未
进作用域表则 Value 位查不到，同样被吞）。

**根因**：判据只查"语法上有模板 id"（`hasTemplIdSyntax`），缺"链尾成员名是类型还是值"
这另一半。parseType 的 `::` 循环把 `Box<int>::count` 无条件建成嵌套**类型**名
（它无法知道 count 是别名还是静态数据成员），而 B16 分支对这个形态照单全收。

**修法方向**（Stage 3 之后更顺）：嵌套名的链尾同样查 `nameBits(末段) & ScopeBit::Value`
——活作用域表已有 Value 位（`include/name_scope.h`），类体在语句之前解析、
static 成员声明时应已 `declareName`（★ 需顺带确认/补上登记）。挂了值位 ⇒ 不是声明
⇒ 回卷走表达式分支，由 Sema 的限定名静态成员路径（B21/B33）接住。
这与 B37 裸名分支"不是值 ⇒ 才按声明"的判据**对称**——同一份语义判据的两条消费路径
不应一有一无。

**状态**：⬜ 未修

---
## B52. macOS 本机端到端验证被堵：Apple `as` 拒收汇编注释里的全角标点（未修）

- severity: low（只影响 macOS 本机验证，不影响 Linux 主线）｜ status: open ｜ related: `src/driver/main.cpp` 汇编步骤（`as -o …`）

**现象**：macOS 上 `minicc` 借系统 `as`（/usr/bin/as，clang-1700）汇编时，
**任何程序**（含 `int main(){return 0;}`）都在含全角标点（`，` `（` `）`）的
注释行报 `unexpected token / invalid character`，汇编步必败。
ctest 339 个测试全绿掩盖了它——它们全是单元测试，**不经过 `as`**；
minicc 集成测试只在 Linux（logdiff 基线）跑。
⇒ macOS 本机只能 `-S` 看汇编/日志，无法端到端验证。

**修法方向**（三选一）：① CodeGen 注释只留 ASCII（改动会漂 Linux 基线，需重刷）；
② 汇编注释里的非 ASCII 字符做转义/剥离（发射前过滤，不改源码注释）；
③ 驱动改用 `clang -c` 当汇编器（注释语义由 clang 前端消化）。

**状态**：⬜ 未修

---
## B53. 非静态数据成员的类内初始化式（NSDMI）未实现：`int x = 5;` 类内被拒（未修）

- severity: medium（拒收合法程序）｜ status: open ｜ related: `src/parse/parser.cpp` 类体字段分支（~2150），docs/learn/08 ctor 主线

**来源**：代码审查类体循环字段分支（字段文法收窄为 `type name [array] ;`）时确认。
静态数据成员分支有 `= expr`（[class.static.data]/3，const 整型族类内初值，B33），
**非 static 字段没有**——C++11 NSDMI（non-static data member initializer，
[class.mem]）整条缺失。

**复现**

```cpp
struct S { int x = 5; };           // C++11 起标准写法：成员默认值
int main() { S s; return s.x; }
```

| | minicc | clang++-18 |
|---|---|---|
| 上例 | ❌ rc=1 `[Parse Error] 1:18 at '=': Expected ';' after field declaration` | ✅ rc=0，运行 exit=5 |
| `int x, y;`（多声明符）/ `int : 3;`（位域） | ❌ 同一 expect(Semicolon) 一并拒收 | ✅ |

**实际使用极广**：C++11 后给成员设默认值的标准写法。本项目最常引用的对照文件
`clang/include/clang/Parse/Parser.h` 就有 **19 处**（`bool OpenMPDirectiveParsing = false;`
这类）；clang/lib/Parse + llvm/ADT 粗略统计 **1200+ 行**同风格。
未做的原因不是复杂：① ctor 批次（learn/08）聚焦显式初始化列表，之后主线在模板/
消歧，排期没轮到；② ministl 双栖桩刻意避开 NSDMI ⇒ 飞轮没传来痛感（对比
initializer_list / throw 是 ministl 真缺口才排上的）。

**修法方向**（中等批次，三件套，机制全部现成）：
1. Parser：`FieldInfo`/`FieldDecl` 加 `initializer` 槽——`match(Assign) + parseExpression()`
   抄 static 分支（~2141）即可；数组成员需补 `{}` 形态；
2. Sema：ctor 成员初始化序列合成加回退规则——"显式初始化列表提到该成员 ⇒ 覆盖；
   没提到 ⇒ 用 NSDMI"（[class.mem] 语义的另一半），合成默认构造同样走此路；
3. CodeGen：零新机制——初始化列表发射路径照走，只是初值**来源**多一种。
边角需小心：声明序约束、数组/引用成员的 NSDMI 形态。
建议与"多声明符 `int x, y;` + 函数默认实参"合一个 **ctor 增强批**（同属"声明式
默认值"家族），已登记 ROADMAP §0.3 降级区。

**状态**：⬜ 未修

## B54. 全局限定名 `::X` 不支持：Parser 拒收 + 空段也不落全局层

- severity: low ｜ status: open ｜ related: src/parse/parser.cpp, src/sema/parse_scope.cpp:167, docs/ROADMAP.md

**现象**

```cpp
int g = 5;
int main() { return ::g; }        // ★ clang rc=5；minicc 解析期就拒收
```

```
[Parse Error] 2:21 at '::': Unexpected token '::' in expression
```

**性质**：**语法面缺特性**（不是语义错判）—— clang 支持全局限定名，minicc 在
表达式/类型位置遇到 `::` 开头没有分支 ⇒ 直接报错。★ 响亮拒收，但拒的是
**合法程序** ⇒ 归"缺特性"一档（优先级低于静默类）。

**根因（两处，缺一不可）**：

1. **Parser**：表达式与类型的位置没有"全局限定"分支（`::` 开头不是标识符）
   ⇒ 报 `Unexpected token '::' in expression`。
2. **Scope 表**：`ParseScopeTable::classifyQualified` 拆段时空段被
   `if (seg.empty()) continue;` **跳过**，而 `scopePath` 仍停在**当前路径**
   ⇒ 即便 Parser 放行，`::X` 也会被查成"当前作用域里的 X"（不是全局的 X）。
   ★ 该行注释写的是"落到全局层"，**与实现不符** —— 注释写的是**意图**，
   实现是"跳过"（今天靠第 1 条挡在前面，两处都不落地）。

**为什么现在不修**：语法面欠账（不占主线位，见 ROADMAP §0.3）；要修得同时改
两处 —— Parser 放行 `::X` + `classifyQualified` 把空段处理成"路径落到全局层
`""`"，后者是**语义修正**，不是一行能了的事。

**回归（修后要补）**：`::g`（全局变量）/ `::N::S`（全局限定的嵌套名）/
类模板成员函数里写 `::g` —— 三条都要与 clang 对拍。

**教训**：★ **注释也会过期**：`:167` 那句"落到全局层"描述的是**打算怎么做**，
不是**实际做了什么**；今天靠"Parser 先拒收"把它盖住了 ⇒ **看起来能用的分支
其实没落地**。判据单点的反面：**两处各有一半**（一处拦、一处按另一套算），
谁都没实现 ⇒ 只能靠"另一处先炸"才没出事。

---

## B55. 按 path 认层时，同 path 的多层只查最内层（潜伏边界）

- severity: low ｜ status: open ｜ related: src/sema/parse_scope.cpp:197, docs/learn/53

**现象**（活作用域表层，可复现）

```cpp
ParseScopeTable t;
t.openScope(ScopeKind::Function, "f", ScopeBit::Value);  // 函数作用域：path "f::"
t.declare("x", ScopeBit::Value);                          // x 登记在【函数作用域】这一层
t.classifyQualified("f::x");      // ⇒ 命中
t.openScope(ScopeKind::Block, "", 0);                     // 块作用域：path 也是 "f::"
t.classifyQualified("f::x");      // ⇒ 0（同一句，因块作用域开着而翻转）
```

**性质**：**潜伏边界**（今天**不可达**）—— 限定名只用于 namespace/class 的路径；
`f::x` 本身不是合法 C++ 限定名（clang 直接报错），且函数作用域**不持久**
（`isPersistent(Function) = false`）⇒ 持久上下文里没有 `"f::"` ⇒ 文件作用域查
`f::x` 同样是 0。★ 但这条边界一旦被跨过（支持 `f::x` 形态 / 让块作用域参与
路径），就会变成**状态依赖**的真缺陷：同一句源码在块内/块外结果不同。

**根因**：`lookupInPath`（`:197`）按 **path** 认层，`rbegin()` 先碰到**最内层**
同 path 的那层；该层没有这个名字就 `return std::nullopt`（`:201`，不外溢）
⇒ **同 path 的外层那层被整个跳过**。

同 path 的两个来源：① **Function 作用域与其内部的 Block 作用域**（Block 不加
路径成分，沿用父层路径）；② **TemplateParam 作用域与它的父层**（模板形参域对
名字登记透明）。

**为什么现在不修**：不可达 ⇒ 不占主线位；★ 但它与"路径 = 身份"这条设计前提
直接相关 —— 路径要当身份键，就得**唯一标识一层**，而本实现里路径**不唯一**
⇒ 认层必须是"同 path 的各层合并查"，而不是"取最内层"。

**回归（修后要补）**：同 path 两层的名字可见性（函数参数 vs 块内同名）。

**教训**：★ **"路径 = 身份"是有前提的**：前提是路径**唯一**。一旦有作用域
复用父层路径（Block / TemplateParam），"按路径认层"就必须处理"多对一"，
否则同一路径在不同栈状态下指向不同的层 —— ★ 这与 B10→B20→B22→B45 那个坑
同源：**同一个键在两处被当成唯一，实际不是**。

## B56. 成员模板的形参表另写一份 ⇒ 自己的形参在方法体里认不出（依赖限定名直接被拒）

- severity: medium ｜ status: fixed ｜ related: src/parse/parser.cpp:1243, docs/learn/58

**现象**（合法程序被拒，可复现）

```cpp
struct Box { int value; using type = int; };
struct S { template <class T> typename T::type get(T x) { return x.value; } };
int main(){ Box b; b.value = 5; S s; return s.get(b); }
// clang++-18: rc=5（正确返回 b.value）
// 修前 minicc: [Semantic Error] unknown type name 'T::type'
```

**根因**：类成员循环里的成员模板分支**另写了一份**形参表解析（简化版），
与自由路径那份各写一遍。简化版**只 push 形参节点、没同步 `frame.count`** ⇒
`lookupTemplateParam`（`src/parse/parser.cpp:183`）在成员模板的帧里取不到任何形参
⇒ `parseType` 把方法体里的裸 `T` 建成 **Class 节点**而非 **TemplateParam 节点**
⇒ 依赖名分支（`src/parse/parser.cpp:455`，前提是 `lookupTemplateParam` 命中）走不进去。

★ 实测（同一段函数体里）：成员模板**自己**的 `U` 认不出（`base = U (class/tparam)`），
而**外层**类模板的 `T` 认得出（`base = T (template param, scope hit)`）——
因为外层那帧的 `frame.count` 是同步的（自由路径同步了）。
⇒ 这不是"成员模板不支持什么"，而是"**成员模板自己的形参作用域根本没生效**"。

★ 正确性此前是靠**按名兜底**偶然成立的：`substituteType` 的 Case 6
（`src/sema/template_instantiation.cpp:1007`）按名字撞替换表，
`Class("T")` 配 `{T := int}` 也能替换对 ⇒ 自由模板靠作用域认形参、成员模板靠按名撞表，
两条不同的依据走到同一个结果 —— 一旦某个形态只对一条路成立（依赖限定名）就分叉成 bug。

**影响面**：成员模板全体。连带缺失 5 项形参能力（无名形参 / 模板模板形参 / 默认实参 /
默认实参约束 / 帧计数同步）—— 自由路径早就有，成员侧没有。

**修法**（2026-11-10，docs/learn/58）：抽 `Parser::parseTemplateParameterList`
（`src/parse/parser.cpp:1243`）为**唯一实现**，自由路径与成员路径都调它。
形参作用域的"建帧 + 开作用域"留在调用方（必须活到模板体解析结束）。

**修复**：成员模板的形参能力与形态与自由模板完全一致；`typename T::type` 可用；
集成 tests/tmpl/test_tmpl_78_member_template_params.cpp（rc=22，与 clang 一致）
+ 单测 tests/unit/test_member_template_params.cpp（`MemberTemplateParams.*` 6 例，
断言**性质**：形参节点种类 / 依赖限定名的限定者形态 / 四项能力 / 约束检查）。
突变验证 5/5 实跑变红（M5 = 整体退回旧实现 ⇒ 5 例全红）。
★ 4 份既有集成日志**有意漂移**（成员侧现在也打形参表那套日志 + 形态从 Class 节点变
TemplateParam 节点 + `[subst]` 从 Case 6 切到 Case 1）——
**rc、运行输出、汇编三者逐字节不变**；另 165 份逐字节零漂移。

**教训**：★ **"同一规则两处各写一遍"必然漂移**（B10→B20→B22→B45 同一个坑的第六次现形）。
本次的具体形态是**时间差**：成员侧是批 21 的一次性快照，自由侧此后逐批加料 ⇒ 能力差越拉越大。
★ 另一条：**"名字在哪一层登记"要看登记的 API** ——
`declare`（登记进最内层）与 `openScope`（登记进最近的外层**非模板形参**作用域）
是两套落点规则；只盯着调用点上的 `openScope(TemplateParam)` 会算错
（本批评估阶段就误判过一次"值位丢了"，突变验证时被推翻）。

## B57. 函数模板上的模板模板形参推导 + 默认实参填充未实现（自由与成员同款）

- severity: medium ｜ status: open ｜ related: src/sema/template_deduction.cpp:435, docs/learn/58

**现象**（合法程序被拒，可复现）

```cpp
template <class T> struct Holder { T value; };
template <template <class> class C, class T> T wrap(C<T> x) { return x.value; }
int main(){ Holder<int> h; h.value = 9; return wrap(h); }
// clang++-18: rc=9
// minicc: [Semantic Error] no matching function for call to 'wrap'
//          (no template candidate deduces successfully)
```

```cpp
template <class T, class U = int> T f(T x) { return x; }
int main(){ return f(1); }
// clang++-18: rc=1
// minicc: 同上报错
```

**根因**：两条，都是**推导侧**的独立缺口：
① `deducePair` 只认"P 本身就是模板形参"（`isTemplateParamName(P, paramNames)`，
`src/sema/template_deduction.cpp:435`），不认"**模板 id 的基名**是模板模板形参"
（`C<T>` 形态）⇒ `C` 无处可绑；
② 推导收尾阶段"走完实参仍有未绑定形参 ⇒ 失败"，**没有**"取默认实参顶上"这一步
（[temp.arg]/3：推不出来的形参用默认实参）⇒ `U` 无处可推。

**为什么现在不修**：★ 这两条**自由路径与成员路径同款**（实测自由路径同样报错）
⇒ 不是成员侧独有，收口形参表**不会**让它们消失；属**推导引擎**的独立缺口，
要动的是 `template_deduction.cpp`（① 补模板 id 形态的合一；② 收尾阶段补默认实参填充）。
且 ① 与 ROADMAP 主线 K 的 K-4（模板模板形参的形参包/默认实参/嵌套）同族，宜合并做。

**影响面**：函数模板（自由 + 成员）的这两种形态；类模板的模板模板实参匹配
（`checkTemplateArguments`）**不受影响** —— 那条路已经通了（learn/33）。

**回归（修后要补）**：自由与成员各一份正例（rc 与 clang 一致）。

**教训**：★ **解析侧与推导侧是两层** —— 收口解析侧不会自动补上推导侧的能力。
本批把成员侧从"解析期就拒收"推进到"解析期收得下、推导期按既有能力办"，
**与自由路径完全对齐**；剩下的差距是**两侧共有**的，不是这次收口该背的。

---

## B58. const 引用形参与右值引用形参按值传（不跟踪被绑定对象）

- severity: low ｜ status: open ｜ related: docs/learn/59, docs/BUGS.md B6

**现象**

```cpp
int g = 1;
void mut() { g = 5; }
int read(const int& x) { mut(); return x; }   // clang=5，minicc=1
```

**根因**：`const int&` / `T&&` 形参的实参在合法程序里**可能是右值**
（`read(6)`），而右值没有地址 ⇒ 要按地址传，调用方必须先**物化一个临时量**
（自己帧里开一块临时槽，帧估算也得跟着改）。本实现不做这一步 ⇒
按值传（调用点装值，被调点按普通值解释）。

**影响面**：极小 —— 只有"被调方在调用期间让被绑定对象发生变化"才有可观察差异
（被调方拿到的是调用前的副本）。
局部 `const int& r = a;` **不受影响**（本地知道初始化式可寻址 ⇒ 仍是别名，见 B6 的修复记录）。

**修法**（未做）：调用点为不可寻址的引用实参开临时槽（帧估算加一条
"每个这样的调用点 8 字节"，与 `countCtorTempBytes` 同款）⇒ 被调点即可
统一按别名解释所有引用形参。

**回归（修后要补）**：单测一例钉住"const 引用形参能看到被绑定对象在调用期间的变化"。

**教训**

- "引用一定是别名"是对的，但**"能不能当别名"取决于有没有地址** ——
  右值没有地址 ⇒ 要么物化临时量，要么退化成按值（并如实登记为边界）。

