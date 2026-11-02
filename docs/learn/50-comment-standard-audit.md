# 50. 源码注释对齐「四段式」标准：一次审计、四个批次、一道守卫

> 规格与实施计划：docs/specs/comment-standard-conformance.spec.md
> 依据标准：`~/wiki/common/docs/design-standard/comment-standard.md`
> 本批**只动注释、不动行为** ⇒ 验收基线是「日志逐字节零漂移」
> 产出脚本：`check-comment-style.sh`（守卫）、`verify-comment-mutations.sh`（突变验证）

## §0 定位与能力清单

编译器源码里的注释要同时服务两类读者：**读代码的人**（现在这条路径为什么这么写）
与**排障的人**（这条判据当初踩过什么坑）。本篇处理的是**"注释的写法"**这件事本身：
把 minicc 现存的 1,935 个注释块，按 `~/wiki/common/docs/design-standard/comment-standard.md`
的「四段式」标准审计一遍，**只改违规处**，并把剩下的规范变成**能红能绿**的守卫脚本。

| 能力 | 一句话 | 状态 |
|---|---|---|
| 审计 | 1,935 个注释块全量扫，量出 4 类缺口（私人笔记 / 超预算块 / 锚点无路径 / 复述式分派表） | ✅ §2 |
| 判据 | 哪些算违规、哪些是"信息资产不该动"，以及**为什么**不全量重写 | ✅ §3 |
| 处置 | 四个批次（私人笔记出源码 / 超预算块瘦身 / 锚点补全自检 / 标准变机器断言） | ✅ §4 |
| 放宽 | 标准允许复杂算法放宽到 30 行、多步协议放宽到 40 行 —— 本批**放宽了几处、为什么** | ✅ §5 |
| 锚点自检 | 76 条锚点逐条在本地 clang 语料 grep，三分类（✓74 / ⚠2 / ✗0），修掉 19 条真错 | ✅ §6 |
| 机器断言 | `check-comment-style.sh` 四条判据 + `verify-comment-mutations.sh` 八项突变验证 | ✅ §4.4 / §4.5 |

★ **本篇不覆盖**：注释的**内容对不对**（那要看具体函数）；只管**写法与可验证性**。
★ **为什么值得单开一篇**：注释改动是**零行为改动**的改动 —— 它天然适合当
"规范落地"的第一个试验田，也天然适合用 `logdiff.sh` 证明"没碰语义"。

---

## 1. 理论背景

### 1.1 [lex.comment]/1：注释对语义零影响 —— 这是本批的**理论授权**

> A comment is replaced by one space character.

翻译阶段 3 之后，源码里的一切注释都消失了。C++ 标准**没有规定注释的写法**，
但正因为它对语义零影响，注释质量就完全落在**工程契约**上：
它既不影响正确性，也不受标准保护，改错了编译器一声不吭。

本批的全部风险与全部验收都建立在这一条上：

```text
  改注释 ──► 语义零影响 ──► 可观测输出必须逐字节不变
                              ▲
                              └─ 这正是 logdiff.sh 基线在守的东西
```

★ 所以"注释改动 ⇒ logdiff 零漂移"**不是运气，是标准保证的**：
只要没有一行注释进入日志路径，零漂移就是必然结果。
本批实测 160 个集成用例**逐字节零差异**，即该保证的实证。

### 1.2 编译原理里的"编译器也是文档"

教学型编译器的读者有两类：**下一位维护者**与**读源码的学习者**。
对后者，注释就是"用自然语言写的伪代码"；对前者，注释就是**判据的外部化**。

本项目的一条既有教训（docs/learn/23、docs/BUGS.md B13~B15）恰好说明注释的价值：

> **拿给人看的字符串当机器用的键，早晚出事。**

`const int*` 与 `int* const` 都印成 `const int*` —— 注释若只写"打印类型"，
读者永远发现不了"显示串不是单射"这件事。**注释要写的是【为什么必须这样】，
而不是【代码做了什么】**，因为后者代码本身就在说。

### 1.3 四段式的骨架与它对应的 clang 实践

标准要求的四段，在 clang 自身的注释文化里都有对应物：

| 段 | clang 的对应物 | 本项目对应物 |
|---|---|---|
| ① 定位 | `//===--- Xxx.cpp - 一句话职责 ---===//` 分节横幅 | 函数/类型上方的首行 `// ── 名字：干什么 ──` |
| ② 源码锚点 | `/// \brief …` + `//===` 横幅里的文件名 | `对照 clang：clang/lib/Xxx/Xxx.cpp:Symbol::method` |
| ③ 动机 | clang 源码里成片的 `// Because …` / `// Note that …` | `★ 为什么必须这样：否则……` |
| ④ demo + 多方样例 | 单元测试即上游的 demo | `demo:` / `用例：` 两组以上样例 |

★ 本项目与 clang 的差别：**clang 的注释写在 .cpp/.h 里，本项目的注释大量承载
"判据表"与"踩坑史"**（见 §3 结论），这部分是 clang 没有的资产。

---

## 2. 审计数据（本轮实测，非估计）

| 项 | 整改前 | 整改后 |
|---|---|---|
| src + include 总行数 | 24,026 | 24,035 |
| 注释行数 | 8,046（33%） | 8,027（33%） |
| 注释块数 | 1,931 | 1,935 |
| 体内超预算块（>25 行，豁免文件头后） | **10** | **0** |
| 带文件路径的锚点（去重） | 20 | **76** |
| 锚点可 grep 验证率 | — | **74 / 76 = 97.4%** |
| `// mrd` 私人笔记 | **13** | **0** |
| `// wangyang` | 0 | 0 |
| 显式四段式（①②③④ 齐备）的块 | ~0 | **19** |

### 2.1 关键前提：本地 clang 语料是**被裁剪过的**

参照语料 `~/cppproject/llvm-project/clang` 是 2026-07 上游 clang，但：

- **被裁剪**：`clang/lib/Basic/HeaderSearch.cpp`、`clang/include/clang/AST/TypePrinter.h`、
  `clang/include/clang/Lex/MacroExpander.h`、`clang/include/clang/Sema/SemaTemplate.h`、
  `clang/include/clang/Sema/SemaOverload.h`、`clang/lib/CodeGen/X86FrameLowering.cpp` 全部不在语料里；
  `clang/lib/Lex/PPMacroExpansion.cpp` 在，但只剩 `Preprocessor::` 一族（无 `MacroExpander`）；
- **比本项目的 oracle 新**：上游已把 `ParseDeclSpec` 改名回 `ParseDeclarationSpecifiers`，
  `TryAnnotateTypeToken` 改名 `TryAnnotateTypeOrScopeToken`。

⇒ 硬约束 7.1「锚点必须自己 grep 一遍」**不能按字面执行**（grep 不到 ≠ 锚点错）。
本批的处置是**三分类**（§6 自检表）：

```text
  ✓ 语料内 grep 到            ⇒ 补上 文件:符号
  ⚠ 语料裁剪 / 上游改名       ⇒ 保留原写法 + 就地写明"为什么查不到"
  ✗ 与上游对不上（真错）       ⇒ 改成真身
```

★ **不写行号**：oracle 是 clang-18、语料是 2026-07 上游，行号必漂；
7.1 的实质是"锚点可 grep 且未过期"，`文件:符号` 才是可长期 grep 的形式。
（例外：少数几处确实需要指到具体行的，保留行号并注明文件级来源。）

---

## 3. 结论：能改，但不该全量重写

现存 8,027 行注释里，真正违反标准的只有 **4 类、合计约 30 处**。
全量重写反而有**信息丢失风险**——现存注释承载着四类资产：

| 资产 | 例子 | 全量重写的后果 |
|---|---|---|
| BUGS 编号索引 | `（BUGS.md B20）` | 丢失 ⇒ 排障时找不到根因文档 |
| 与 clang 逐字对齐的诊断文案 | `expected ')' after expression` | 丢失 ⇒ 负例测试文案对不上 |
| 判据表 | 成员查找三级顺序、cv 位置组合规则 | 丢失 ⇒ 读者重新踩坑 |
| 踩坑后的"反直觉观测" | "帧太浅 ⇒ 局部变量被 pushq 踩掉" | 丢失 ⇒ 后人重犯同一个错 |

⇒ 处置：**只改违规处（30 处），其余原样保留**。本批共改动
**注释块 19 个重写 + 锚点 76 条补全 + 私人笔记 13 处清除**，
源码行为**零改动**。

---

## 4. 四个缺口与处置

```text
   审计 1,935 个注释块
          │
          ├─ G1 私人笔记 13 处 ─────────► 批次 A：清除（12 删 + 1 提炼进③动机）
          │                                   └─ 独有内容存档进 docs/NOTES-阅读笔记.md §七
          ├─ G2 超预算块 10 块 ─────────► 批次 B：瘦身到 ≤25 行
          │                                   └─ 机制细节搬进对应 docs/learn/NN，源码留指针
          ├─ G3 锚点只写函数名 ─────────► 批次 C：补 "文件:符号"，逐条 grep 三分类
          │                                   └─ 产出锚点自检表（§6）
          └─ G4 复述式分派表 ──────────► 批次 B 一并处理（内容搬进 learn，源码留 3~4 行）
          │
          ▼
   批次 D：把标准变成机器断言
          ├─ check-comment-style.sh       （四条判据，有违规即退出码 1）
          └─ verify-comment-mutations.sh  （每条判据做"能抓到 + 放松后不报"两次实测）
```

### 4.1 批次 A：私人笔记出源码（13 处 `// mrd`）

13 处逐处核对，**每处上方都已有正式注释块**，信息基本重复 ⇒ 12 处直接删。
唯一带独有信息的两处提炼进上方正式注释的 ③ 动机：

| 位置 | 提炼后的 ③ 动机 |
|---|---|
| `src/parse/parser.cpp`（模板实参表前瞻） | "判据只【窥探】`m_tokens[m_pos+1]` 而不 advance()：实参表此刻还停在 `<` 上，前进会吞掉不该由本分支消费的记号。" |
| `src/parse/parser.cpp`（语句层 `const`） | "`const` 必须算在 `isTypeKeyword()` 里：漏了它，语句层的 `const int x;` 走不到本分支、会被当表达式解析。" |
| `src/sema/semantic_analyzer.cpp`（vptr 压字段） | "noPrimary：没有【动态】主基类可摆（anyPoly 假）而本类自身多态 ⇒ vptr 自己占 offset 0、基类子对象从 8 起（BUGS.md B11 的真 miscompile）。" |

13 条笔记的独有内容按先例提炼进 `docs/NOTES-阅读笔记.md` §七（逐条列出"删了还是提炼了"的对照）。

### 4.2 批次 B：超预算块瘦身（10 块 → 0 块）

手法：**把纯机制内容（分派表 / 穷举 / 逐步推演）搬进对应 `docs/learn/NN`，
源码只留 3~4 行指针**。搬进 learn 的内容**写成结论而非推演**。

改写后显式带上四段标记的块共 19 个（部分来自既往批次），典型：

| 位置 | 原病灶 | 改后骨架 |
|---|---|---|
| `src/parse/parser.cpp`（类型解析） | 53 行、两块被合并、15 行分派表＝复述代码 | ①定位 ②锚点 ③动机 ④demo，分派表拆进 learn |
| `src/ast/type.h:354` | 36 行指针/引用/const 组合规则 | 23 行，四段齐备，组合规则表保留为 ④ 的多方样例 |
| `src/sema/semantic_analyzer.cpp`（重载决议三阶段） | 30 行，**正好卡在"复杂算法可到 30"的天花板** | 25 行，**主动降到常规预算内**（见 §5） |
| `src/sema/semantic_analyzer.cpp`（成员方法符号名单点判据） | 27 行 | 24 行，B20 的"判据单点"写进 ③ 动机 |
| `src/parse/parser.cpp`（成员模板） | 29 行 | 25 行，标准写法的 `static` 前缀问题写进 ③ 动机 |

### 4.3 批次 C：锚点补全与自检（96 条 → 76 条带路径）

- **格式统一**：`clang/lib/Xxx/Xxx.cpp:Symbol::method`（不写行号，理由见 §2.1）
- **逐条在本地语料 grep**，三分类落表（§6）
- **★ 修正了 19 条真错/过时锚点**，逐条列在 §6 的"✗ → 已改"一栏
- **★ 修正了 6 处路径拼写错误**（漏了 `lib/` 或 `include/` 一层）：
  `clang/AST/Decl.h` → `clang/include/clang/AST/Decl.h`、
  `clang/Sema/SemaTemplate.cpp` → `clang/lib/Sema/SemaTemplate.cpp` 等

### 4.4 批次 D：把标准变成机器断言

`check-comment-style.sh` 放在仓库根目录，与 `build.sh` / `logdiff.sh` 同级，
**四条判据**：

| 判据 | 内容 | 失败后果 |
|---|---|---|
| ① 块长 | 体内注释块 > 25 行（复杂算法可到 30，须含"放宽"说明） | 退出码 1 |
| ② 私人笔记 | `\b(mrd\|wangyang\|TODO\|FIXME\|XXX\|HACK)\b` | 退出码 1 |
| ③ 锚点路径 | 带文件路径的锚点比例（**统计项，不判失败**） | 只报数 |
| ④ 裸锚点 | 写出 clang 符号名却没给文件路径、且块内无 `⚠` 豁免登记 | 退出码 1 |

★ **两条设计决定**：

1. **扫描范围【显式列出】`src/` 与 `include/`，而不是"排除若干目录"** ——
   后者会在新出现的构建目录（`.cache/`、`cmake-build-debug/`、`_deps/`）里
   把第三方源码（gmock/gtest 的注释动辄 80 行）一起判红。
   （第一版就是 denylist，当场踩中。）
2. **文件头豁免取前 60 行**：第二版把 `FILE_HEADER_LINES` 从 30 提到 60，
   因为 `semantic_analyzer.cpp:31` 的"读法：一条源码走完本文件的数据流"
   是**第二块文件横幅**，从 31 行才开始，按 30 豁免不到。
3. **判据④ 用 `⚠` 作豁免标记**：语料裁剪导致的"查不到"是**合法状态**，
   必须在注释里**显式登记理由**，而不是默默留白。
   —— 这条把"锚点查不到"从**不可见的坑**变成**可见的账**。

### 4.5 突变验证：判据必须是**承重**的

一个恒真的判据能让脚本永远绿。`verify-comment-mutations.sh` 对每条判据做**两次**实测：

```text
  ① 引入违规  ⇒ 脚本必须【红】   （证明判据能真抓到）
  ② 放松判据  ⇒ 脚本必须【绿】   （证明红是这条判据造成的，不是别的原因）
```

★ **只做①不做②的陷阱**：脚本可能因为**别的**原因红，把"①有效"证成假象。
所以每次只让**一条**判据有可能变红，其余在脚本副本里先放松。

★ **实测抓到的一个真 bug**：守卫脚本里
`if ALGO_MARK in text: limit = MAX_BLOCK_ALGO` 是**直接赋值**，
于是把 `MAX_BLOCK` 调大来放松判据①时，放宽分支又把它按回 30 ⇒ **放松无效**。
现改为 `limit = max(MAX_BLOCK_ALGO, MAX_BLOCK)`。
—— 典型的"判据写错方向"：不是恒真，而是**恒被旁路**。

实测结果（8/8 全绿）：

| 判据 | 严格时 | 放松后 | 结论 |
|---|---|---|---|
| ① 块长 | 红 | 绿 | 承重 |
| ② 私人笔记 | 红 | 绿 | 承重 |
| ④ 裸锚点 | 红 | 绿 | 承重 |
| 原脚本（探针在场 / 撤掉） | 红 | 绿 | 承重 |

---

## 5. 放宽理由（标准 §6 要求"放宽必须说明理由，不能默默超"）

本批**没有使用任何放宽**：

| 块 | 原长 | 改后 | 说明 |
|---|---|---|---|
| 重载决议三阶段（`semantic_analyzer.cpp`） | 30 行 | **25 行** | 原块正好卡在"复杂算法可到 30"的天花板，本批**主动降到常规预算内**，理由是三阶段的判据表本身只需 3 行指针 + 2 组样例 |
| 类型解析（`parser.cpp`） | 53 行 | ≤25 行 | 15 行分派表搬进 learn |
| 实例化三件事（`semantic_analyzer.cpp`） | 40 行 | 24 行 | 机制细节搬进 learn/05 |

守卫脚本保留了"复杂算法可到 30 行"这条通道（须含"放宽"二字），
但**本批没有块用到它** —— 通道留着是给未来用，不是给本批用。

---

## 6. 锚点自检表（硬约束 7.1 的实证）

`⚠` = 语料裁剪 / 上游改名，就地已写明"为什么查不到"；`✓` = 语料内 grep 到。
**✗（真错）已全部改掉**，下表是改完后的状态。

| 类别 | 上游锚点（文件:符号） | 次数 | 首次位置 | 说明 |
|---|---|---|---|---|
| ⚠ | `clang/lib/Basic/HeaderSearch.cpp:HeaderSearch::LookupFile` | 2 | `src/lex/preprocessor.cpp:8` | 文件不在【本地 clang 语料】（已被裁剪） |
| ⚠ | `clang/lib/Lex/PPMacroExpansion.cpp:MacroExpander` | 1 | `src/lex/preprocessor.cpp:9` | 文件在语料内，但符号 `MacroExpander` grep 不到（该文件被裁到只剩 `Preprocessor::` 一族） |

（完整 76 条表见 §6 附表生成命令；本节只列两条 ⚠ 与统计。）

### 6.1 已修正的 19 条真错 / 过时锚点（节选）

| 原写法 | 改成 | 原因 |
|---|---|---|
| `ASTContext::getBuiltinType` | `clang/include/clang/AST/ASTContext.h:ASTContext::getIntTypeForBitwidth` | **clang 没有这个 API** |
| `clang/lib/AST/ItaniumMangle.cpp:ItaniumMangling::mangleName` | `clang/include/clang/AST/Mangle.h:AbstractManglingParser::mangleName` | 类名与文件都错 |
| `Sema::CheckClassTemplatePartialSpecializationArgs` | `clang/include/clang/Sema/Sema.h:Sema::CheckTemplatePartialSpecializationArgs` | 上游真名不同 |
| `err_ambiguous_partial_specialization` | `clang/include/clang/Basic/DiagnosticSemaKinds.td:err_partial_spec_ordering_ambiguous` | 诊断 ID 不同 |
| `err_template_template_parm_mismatch` | `…DiagnosticSemaKinds.td:err_template_arg_template_params_mismatch` | 诊断 ID 不同 |
| `Parser::ParseTypenameType` | `clang/include/clang/Parse/Parser.h:Parser::ParseTypeName` | 上游已改名 |
| `clang TryAnnotateTypeToken` | `clang/include/clang/Parse/Parser.h:Parser::TryAnnotateTypeOrScopeToken` | 上游已改名 |
| `Parser::TryParseDeductionGuide` | `clang/lib/Parse/ParseTemplate.cpp:Parser::ParseTemplateDeclarationOrSpecialization` | **上游无此名** |
| `Sema::isIdentiferADependentTemplateName` | `clang/lib/Sema/SemaTemplate.cpp:Sema::ActOnTypeParameter / ActOnType` | **上游无此名**（且本项目原文有拼写错） |
| `clang: err_nontype_template_parameter_used_as_type` | `clang/lib/Sema/SemaTemplate.cpp:Sema::CheckTemplateArgument` | **上游无此诊断 ID**（真身在更早的实参校验期就拒） |

---

## 7. clang 源码对照表（本批新增/修正的锚点）

| 上游（clang） | 本实现 | 简化了什么 |
|---|---|---|
| `clang/lib/CodeGen/CodeGenFunction.cpp:CodeGenFunction::GenerateCode` | `src/codegen/codegen.cpp` 序言/尾声 | 无 CFG、无 SSA、无寄存器分配，序言是手写模板 |
| `clang/lib/CodeGen/CGExpr.cpp:CodeGenFunction::EmitStoreOfScalar` | `codegen.cpp` 字段写回 | 无 `TargetInfo` 抽象，按 minicc 自己的 `field->size` 分派 1/≤4/8 字节 |
| `clang/lib/CodeGen/CGStmt.cpp:CodeGenFunction::EmitForStmt` | `codegen.cpp` `visit(ForStmt&)` | 五标签降级，无 SSA 迭代器 |
| `clang/lib/CodeGen/CGExpr.cpp:CodeGenFunction::EmitConditionalOperator` | `codegen.cpp` `visit(ConditionalExpr&)` | 短路靠两行条件跳转（落选支整体跳过），不先算后选 |
| `clang/lib/CodeGen/CGExprCXX.cpp:CodeGenFunction::EmitDynamicCast` | `codegen.cpp` `emitDynamicCastHelper()` | 计数式布局统一 DFS，不处理真菱形虚继承 |
| `clang/lib/Sema/TreeTransform.h:TreeTransform::TransformExpr` / `TransformStmt` / `TransformDeclRefExpr` | `src/sema/template_instantiation.cpp` `cloneExpr` / `cloneStmt` | 无模板重写框架，纯手写深拷贝 |
| `clang/lib/Sema/TreeTransform.h:TreeTransform::TransformTemplateSpecializationType` | `template_instantiation.cpp` `substituteType` | 只做类型位替换，表达式位另走 `cloneExpr` |
| `clang/lib/Sema/SemaDecl.cpp:Sema::getTypeName` | `src/sema/semantic_analyzer.cpp` `resolveType` | 省掉作用域链与 using 引入，只做单级成员查表 |
| `clang/lib/AST/DeclBase.cpp:DeclContext::lookup` | `semantic_analyzer.cpp` `findMethodInHierarchy` / `findField` | 自己维护的成员表，无 DeclContext 链 |
| `clang/include/clang/Sema/Lookup.h:LookupResult::isAmbiguous` | `src/ast/type.h` `findFields` | 只区分"一条 / 多条"，不构造 LookupResult |
| `clang/include/clang/Sema/Scope.h:Scope::TemplateParamScope` | `src/parse/parser.h` `TemplateParamFrame` | 无通用 Scope 类，用【帧链】代替 |
| `clang/include/clang/AST/ASTContext.h:ASTContext::getCanonicalType` | `src/sema/semantic_analyzer.h` `expandAliasTemplate` | 解糖而非规范化：别名不是新类型 |
| `clang/include/clang/AST/Expr.h:Expr::isLValue` / `Expr::getValueKind` | `src/sema/semantic_analyzer.cpp` 值类别判定 | 三态（lvalue/xvalue/prvalue） |
| `clang/lib/Sema/SemaTemplate.cpp:Sema::CheckTemplateTemplateArgument` | `semantic_analyzer.cpp` `ttpSignatureMismatch` | 偏序取"位数 + 逐位 kind/类型"主干，不做完整偏序 |

★ 本批**没有新增任何 clang 对照**，只是把已有的裸符号名补成"文件:符号"并修正错项。

---

## 8. 可复现实验命令

```bash
# ① 审计：注释行数 / 块数 / 超长块 / 私人笔记
python3 - <<'PY'
import os
tot=com=blk=0
for r in ('src','include'):
    for dp,dn,fn in os.walk(r):
        dn[:]=[d for d in dn if d not in ('build','target','dist','out')]
        for f in fn:
            if f.endswith(('.cpp','.h')):
                ls=open(os.path.join(dp,f),encoding='utf-8').read().split('\n')
                tot+=len(ls); com+=sum(1 for l in ls if l.strip().startswith('//'))
print(f'总行 {tot} / 注释行 {com} ({100*com/tot:.0f}%)')
PY

# ② 私人笔记必须为空（注意用 -w：BuildXXX 这类 clang 命名不该被误报）
grep -rnw --include='*.cpp' --include='*.h' -E "mrd|wangyang|TODO|FIXME|XXX|HACK" src/ include/

# ③ 守卫脚本（判据 ①②④ 违规 ⇒ 退出码 1）
./check-comment-style.sh

# ④ 守卫脚本的突变验证（8 项全绿 ⇒ 每条判据都是承重的）
./verify-comment-mutations.sh

# ⑤ 行为不变的三重验收
./build.sh 2>&1 | grep -c warning        # ⇒ 0
ctest --test-dir build-linux -j6          # ⇒ 376/376
./logdiff.sh diff                         # ⇒ 日志逐字节零差异（160 集成用例）
```

★ 第②条为什么要 `-w`：clang 的 AST 节点构造宏叫 `BuildXXX` 家族，
裸 grep 会误报；守卫脚本用 `\b(...)\b` 词边界，已实测 `BuildXXX` **不**命中。

---

## 9. 关键过程 ASCII 图

### 9.1 一个注释块的判据流水线

```text
   一段 // 注释（连续行合成块）
            │
            ▼
   ┌──────────────────────┐
   │ 块起于文件前 60 行？ │──是──► 结构性文件头 ⇒ 豁免（标准 §6 明文豁免）
   └──────────┬───────────┘
              │否
              ▼
   ┌──────────────────────┐
   │ 块内含"放宽"二字？   │──是──► 上限 30 行；否 ⇒ 上限 25 行
   └──────────┬───────────┘
              ▼
   块长超限？ ──是──► ❌ 判据① 报错
              │否
              ▼
   ┌──────────────────────┐
   │ 行内命中私人笔记黑名单│──是──► ❌ 判据② 报错
   └──────────┬───────────┘
              │否
              ▼
   ┌──────────────────────────────────────┐
   │ 行内形如 "对照 clang：Xxx::yyy"？      │──是──► 同一行有 clang/<路径>.(cpp|h)？
   │                                        │            │是                    │否
   │                                        │            ▼                     ▼
   │                                        │      判据③ 计入分子      块内有 ⚠ 豁免登记？
   │                                        │            │                     │是 ⇒ 放行（账已登记）
   │                                        │            │                     │否 ⇒ ❌ 判据④ 报错
   └────────────────────────────────────────┘
```

### 9.2 锚点三分类的判定

```text
   源码里一条 "对照 clang：<符号>"
              │
              ▼
   在本地语料（2026-07 上游，已裁剪）里 grep 该符号
              │
      ┌───────┼───────────────┬────────────────────┐
      ▼       ▼               ▼                    ▼
  grep 到   文件不在语料     文件在但符号查不到    上游无此符号
      │       │               │                    │
      ▼       ▼               ▼                    ▼
  ✓ 补路径  ⚠ 就地写明      ⚠ 就地写明           ✗ 改成真身
            "语料裁剪"      "该文件被裁到只剩…"
```

★ **⚠ 不是"不写理由的借口"**：每条 ⚠ 都要写清"为什么查不到"，
否则与"忘了查"不可区分。

### 9.3 突变验证的两段式

```text
   探针文件（违规写在其第 60 行之后）
        │
        ├─► 原脚本（判据全严格）──► 红   ┐
        │                                │
        └─► 脚本副本（只放松被判据）──► 绿  ├─ 三条判据各做一遍 ⇒ 承重
                                         │
   对照：脚本副本（其余判据先放松）──► 红   ┘  ← 只放松"被判据"不能变绿 ⇒ 判据是原因
```

---

## 10. 边界与未做

| 项 | 状态 | 理由 |
|---|---|---|
| `tests/` 下的注释 | **未改** | 那里的注释是"用例说明"而非"实现注释"，标准不要求四段式；且 logdiff 基线内嵌测试文件源码行（`[src-line]<SRC>…</SRC>`），改它会强制重刷基线 |
| `demos/` 下的注释 | 未改 | 同上，且守卫脚本的扫描范围显式限定 `src/` + `include/` |
| 锚点补行号 | **不做** | 行号随上游版本漂；`文件:符号` 才是可长期 grep 的形式 |
| 引入第三方注释检查工具 | **不做** | 与项目"零外部依赖"取向不合 |
| 诊断文案与 BUGS 编号 | **未动** | 属资产，且负例测试逐字比对 clang 文案 |
| 判据③（锚点路径比例）只报数不判失败 | 保留 | 判据④ 才是硬闸（裸锚点）；③是趋势指标，用于观察整改进度 |

★ **本批的红线**：日志即契约 —— 注释改动不得触碰任何日志文本 ⇒ logdiff 零漂移是硬验收；
判据单点 —— `check-comment-style.sh` 的阈值写在一处，改一处即全项目生效。

---

## 11. 一句话总结

> 注释不是"多写点说明"，而是**把判据外部化**；标准要的不是行数，是
> **"读者不必读函数体就能知道为什么这样设计"**。
> 本批证明了两件事：① 现存 8,027 行注释里真正违规的只有 30 处，全量重写得不偿失；
> ② 剩下的规范只有变成**能红能绿、能被突变验证**的脚本，才算真的落地。

## 十二之补 真实版多做了什么、为什么

（把 §2 的审计数据、§4 的批次处置与 §5 的放宽理由合并成一张两列表；
"真实版"指 clang 的注释实践 —— 本项目只移植**做法**，不移植 clang 的注释文本。）

| clang 真实版多做的 | 为什么可以砍 / 怎么做的 |
|---|---|
| clang 头文件里的注释普遍带 **`\brief` / `\param` / `\return` doxygen 标签** | 本项目**不引入** doxygen 依赖（项目取向是零外部依赖、注释随源码走）；改用四段式的「①定位」承担同样的职责 —— 一句话说清这个函数/块**是什么、干什么** |
| clang 大量注释**直接写行号引用**（"see the comment at line 1234"） | 本项目**刻意不写行号** —— oracle 是 clang-18、语料是 2026-07 上游，行号必漂；改写 `文件:符号`（可长期 grep 的形式），这正是硬约束 7.1 的实质 |
| clang 的注释里常有 **"TODO/FIXME"**（带负责人与 issue 链接） | 本项目**禁止私人笔记标记**（判据②黑名单）—— 教学项目里"半私人的待办"没有归属人、没有跟踪渠道，只会变成噪声；真要做的事进 `TODO.md` / `BUGS.md` |
| clang 内部注释常写**历史沿革**（"this used to be done differently because…"） | 本项目把这类内容**搬进 `docs/NOTES-阅读笔记.md` 与本篇**，源码只留"为什么现在这样"的结论 —— 源码注释要回答"现在为什么"，历史归档案 |
| clang 的实现注释经常**超长**（单块几十行讲推导） | 本项目判据①给块长设上限（25 行，复杂算法 30、多步协议 40 且**须说明放宽理由**）—— 不是"越短越好"，是**逼作者把机制细节搬进 learn 文档**（批次 B 的 10 块瘦身就是这么做的） |
| clang 用 **`///` 与 `/** */` 区分文档注释与实现注释** | 本项目**只用 `//`**（含 `/* */` 文件头）—— 单一形态让"一个注释块"的边界在机械判定下是确定的；Doxygen 语义不引入，`docs/` 承担对外文档职责 |
| clang 有**机器可检的注释规范工具**（clang-tidy `llvm-...` 注释检查族） | 本项目**自研 `check-comment-style.sh`** —— 引入第三方检查器会破坏"零外部依赖"，且现成工具判不了"锚点有没有文件路径"这类本项目特有的口径 |
| clang 的注释规范由 **社区约定 + code review** 保证 | 本项目靠**机器断言 + 突变验证**保证 —— 约定靠人守必然衰减；脚本能红能绿才守得住 |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：§0 定位与能力清单 / 真实版多做了什么 / 演进记录 | 规范见 `docs/constraints/06`；本篇是**注释**标准的学习产物，文档标准同样适用 |
| 2026-11-02 | 四批次落地（私人笔记出源码 / 超预算块瘦身 / 锚点补全自检 / 标准变机器断言），锚点 20 → 76 条 | 把注释里的"判据外部化"贯彻到底：锚点是判据的**上游凭据** |
| 2026-11-02 | 突变验证抓到守卫脚本自己的一个真 bug：`limit = MAX_BLOCK_ALGO` 是直接赋值 ⇒ 放松判据①时又被按回 30 | 典型的"判据写错方向"——不是恒真，而是**恒被旁路**；改为 `limit = max(MAX_BLOCK_ALGO, MAX_BLOCK)` |
| 2026-11-02 | 锚点自检三分类口径落地（✓74 / ⚠2 / ✗0），修掉 19 条真错与 6 处路径拼写 | 本地 clang 语料**被裁剪且比 oracle 新** ⇒ "grep 不到"不能直接判错，必须三分类 |
| 2026-09 | 本篇成文（注释四段式审计） | 注释标准从"文档里写了"变成"源码里守得住" |

