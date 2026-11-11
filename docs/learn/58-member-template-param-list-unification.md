# 58. 成员模板的形参表收口到自由模板那一份（[temp.mem] × [temp.param]，BUGS.md B56）

> 定位：把"成员函数模板的形参表"与"自由函数模板的形参表"**合成一份实现** ——
> 此前两处各写一遍、随批次演进而漂移，成员侧缺了 5 项能力，
> 其中一项让**成员模板自己的形参在方法体里认不出** ⇒ 依赖限定名直接报错。
> 一句话：**形参表是同一个文法、同一条语义规则** —— 模板体住在命名空间里还是类体内，
> 不改变形参表的规则；两处各写一遍，规则就会分叉。
> 关联：docs/learn/34（成员模板）、docs/learn/30（模板形参作用域）、
> docs/learn/26（作用域链）、docs/BUGS.md B56/B57
> 前置：ROADMAP §0.1 批 0-4（"成员模板的形参表解析收口"）

## §0 定位与能力清单

| 能力 | 状态 | 落点 |
|---|---|---|
| 成员模板的形参表**解析**与自由模板走**同一份实现** | ✅ 本批 | `src/parse/parser.cpp:1243` `parseTemplateParameterList` |
| 成员模板**自己**的类型形参在方法体里认得出（TemplateParam 节点） | ✅ 本批 | 帧计数同步（`frame.count`）+ `parseType` 的形参命中分支 |
| 依赖限定名 `typename T::type`（[temp.res]/5）在成员模板里可用 | ✅ 本批 | `src/parse/parser.cpp:455` 依赖名分支 |
| 无名类型形参 `template <class>`（[temp.param]/3） | ✅ 本批 | 共享实现的无名形参分支（合成 `$unnamedN`） |
| 模板模板形参 `template <template <class> class C>`（[temp.param]/4） | ✅ 本批（**解析侧**） | 共享实现的模板模板形参分支 |
| 默认模板实参 `template <class T, class U = int>`（[temp.param]/12） | ✅ 本批（**解析侧**） | 共享实现的默认实参分支 |
| 默认实参约束检查（"后面必须都有默认"）在成员侧生效 | ✅ 本批 | 共享实现的约束检查 |
| 成员模板的**推导**支持模板模板形参 | ⬜ **边界（B57，未修）** | 推导引擎只认"裸形参名"，不认 `C<T>` 形态 —— **自由路径同款** |
| 推导期用默认实参顶上未推导的形参（[temp.arg]/3） | ⬜ **边界（B57，未修）** | 推导收尾阶段没有"未绑定 ⇒ 取默认"这一步 —— **自由路径同款** |
| 成员类模板 / 成员别名模板 / 静态数据成员模板 | ⬜ 边界 | 成员循环只做成员函数模板这一种形态（learn/34 §5） |

## 1 理论背景

### 1.1 标准怎么说

- **[temp.mem]/1**：类内的成员模板是"类作用域里的模板声明"，其**声明形态**与
  命名空间里的模板声明同构 —— 区别只在"名字属于哪个作用域"，不在"形参表怎么写"。
- **[temp.param]**：`template-parameter-list` 的文法与语义对**所有**模板声明一致：
  类型形参（[temp.param]/3 允许无名）、模板模板形参（[temp.param]/4）、
  非类型形参、[temp.param]/12 的默认实参（且"一旦某位有默认，其后每一位都必须有"）。
- **[temp.res]/5**：`typename T::type` 里的 `T` 是**依赖的限定者** ——
  它在实例化之前还不知道是什么类型，因此不能拼成 `"T::type"` 字符串，
  必须建成"依赖名"节点，等实参到位再兑现。
  ★ 这条的前提是：**`T` 得先被认成形参** —— 认不出来，它就只是个普通标识符。

### 1.2 编译原理怎么说

形参表解析是**上下文无关**的一段文法（`template < ... >` 的内部结构），
它的语义动作只有三件：建形参节点、登记名字、把形参放进"模板形参作用域"。
★ 这三件事**与模板体住在哪儿无关** —— 自由模板与成员模板共用同一段 CFG 片段，
只是**进入/退出**时的作用域栈状态不同。

工程上的对应：clang 里 `Parser::ParseTemplateParameters` 是一个**独立函数**
（`clang/lib/Parse/ParseTemplate.cpp`），自由模板与成员模板都调它；
判"这是不是成员模板"发生在**更上层**（`ParseCXXMemberSpecification` 遇 `kw_template`
⇒ 走 `ParseTemplateDeclarationOrSpecialization`，由 `AllowClassTemplate` /
`MemberTemplate` 之类的标志决定后续怎么解析模板体）。

⇒ **"形参表怎么解析"与"模板体是什么"是两个问题** —— clang 把前者做成一个函数，
后者做成一条分派。本实现此前把**前者也分成了两个**（自由一份 + 成员一份），
于是规则分叉。

### 1.3 与本项目既有约定的关系

- **判据单点**（红线 1 的推论）：同一个问题的答案只能有一处。
  "形参表的文法与语义"是**同一个问题** ⇒ 只能有一份实现。
- **"只算一次，别处只引用"**（B10→B20→B22→B45 同一个坑的第五次现形）：
  此前成员侧与自由侧是"各算一遍"，虽然结论常常相同，但**能力清单不同** ⇒ 分叉。
- **作用域即身份**（learn/49）：形参登记在**模板形参作用域**里，
  而模板形参作用域对**名字登记**是透明的（`nameScopeIndex` 跳过它）。

## 2 病灶与改法

### 2.1 病灶：成员侧另写了一份形参表（★ 规则分叉）

改法前，成员模板的形参表在类成员循环里**另写了一份**（简化版），
与自由路径那份（`parseTemplateDecl` 内联）**各写一遍**。两者的能力差：

| 能力 | 自由路径 | 成员路径（改法前） |
|---|---|---|
| 有名类型形参 | ✅ | ✅ |
| **无名类型形参** `template <class>` | ✅（合成 `$unnamedN`） | ❌ 报 `Expected parameter name in member template` |
| **模板模板形参** | ✅（内层表存节点） | ❌ 自造文案 `template template parameters in member templates are not implemented` |
| **默认模板实参** | ✅ | ❌ 报 `Expected '>' to close member template parameter list` |
| **默认实参约束检查** | ✅ | ❌ |
| **`frame.count` 同步** | ✅ | ❌ ★ 最要命 |
| 形参名登记进活作用域 | ✅ | ✅ |

★ 两条路径不是"同时写的"，而是**时间差**：成员侧是批 21（learn/34）的一次性快照，
自由侧此后逐批加料（NTTP、模板模板形参、默认实参、无名形参…）⇒ 能力差越拉越大。

### 2.2 最要命的那一条：`frame.count` 没同步 ⇒ 自己的形参认不出

`TemplateParamFrame`（`src/parse/parser.h`）是模板形参作用域的帧链，
`count` 是"已注册形参个数"；`lookupTemplateParam`（`src/parse/parser.cpp:183`）
按 `count` 上界取形参：

```cpp
for (const TemplateParamFrame* f = m_currentFrame; f; f = f->parent) {
    for (size_t i = 0; i < f->count; i++) { ... }
}
```

成员侧自写的那份**只 push 了形参节点、没同步 `count`** ⇒
`lookupTemplateParam` 在成员模板的帧里**一个形参都取不到** ⇒
`parseType` 把方法体里的裸 `T` 建成 **Class 节点**（"有个叫 T 的类"），
而不是 **TemplateParam 节点**（"这是模板形参"）。

★ 实测（同一段函数体里）：

```
[parse:type] base = U (class/tparam)                ← 成员模板【自己】的 U：认不出
[parse:type] base = T (template param, scope hit)   ← 外层类模板的 T：认得出
```

外层那个认得出，是因为**外层那帧的 `count` 是同步的**（自由路径同步了）。
⇒ 这不是"成员模板不支持什么"，而是"**成员模板自己的形参作用域根本没生效**"。

### 2.3 后果一：依赖限定名直接报错（合法程序被拒）

`typename T::type` 走 `parseType` 的依赖名分支（`src/parse/parser.cpp:455`），
前提是 `lookupTemplateParam(name)` 命中且 `kind == Type`。
成员路径命中不了 ⇒ 走不进那条路 ⇒ `T::type` 被当成普通限定名拼成字符串
⇒ 语义期查不到 ⇒ 报 `unknown type name 'T::type'`。

```cpp
struct Box { int value; using type = int; };
struct S { template <class T> typename T::type get(T x) { return x.value; } };
// clang: rc=5（正确返回 b.value）
// 改法前 minicc: [Semantic Error] unknown type name 'T::type'
```

### 2.4 后果二：正确性靠"按名兜底"偶然成立

`substituteType` 的 **Case 6**（`src/sema/template_instantiation.cpp:1007`）是
一条**按名字**撞替换表的兜底：`Class("T")` 配 `{T := int}` ⇒ int。
成员模板的 `T` 恰好是 Class 节点 ⇒ 靠这条兜底也能替换对。

⇒ 也就是说：**自由模板靠作用域认形参，成员模板靠按名字撞表认形参** ——
两条不同的路走到同一个结果。★ 这就是"两处各算一遍"的味道：
结果对，但**依据不同** ⇒ 一旦某个形态只对一条路成立（依赖限定名就是这种），
就分叉成 bug。

### 2.5 改法：抽一份共享实现（判据单点）

```
Parser::parseTemplateParameterList(TemplateDecl& decl, TemplateParamFrame& frame)
    （src/parse/parser.cpp:1243）
```

- **搬进去的**：形参表本体（类型/无名/模板模板/NTTP 四种形态 + 默认实参）、
  `expect('>')`、默认实参约束检查、`frame.count` 同步、形参名登记进活作用域。
- **留在调用方的**：`TemplateParamFrame` 建帧 + `openScope(TemplateParam)` ——
  它们必须活到**模板体**解析结束（类体/函数体里的形参要在作用域内），
  而本函数只管形参表 ⇒ 留在调用方，两处各留一份、形状同款。
- **调用点**：
  - 自由路径 `parseTemplateDecl`（`:1526` 附近）—— 行为**零变化**；
  - 成员路径（`:2011`）—— 能力与形态**一步对齐**自由模板。

★ **为什么不做"给 `parseTemplateDecl` 加 `isMember` 开关"**：
那会把两套**模板体分派**（类模板/推导指引/别名模板/函数模板 vs 成员函数）
与两套**名字登记规则**塞进同一个函数。判据单点说的是
"**同一个问题的答案只能有一处**"，不是"函数越少越好"：
**形参表是同一个问题**（该合），**模板体形态是两个问题**（不该合）。

## 3 clang 源码对照表

| clang（上游） | 本实现 | 简化了什么 |
|---|---|---|
| `clang/lib/Parse/ParseTemplate.cpp:Parser::ParseTemplateParameters`（形参表本体） | `src/parse/parser.cpp:1243` `parseTemplateParameterList` | 无：这一段**逐条照搬同一套判据**（无名形参合成名、模板模板形参的内层表、默认实参、默认实参约束） |
| `clang/lib/Parse/ParseTemplate.cpp:Parser::ParseTemplateParameter`（单个形参） | 同上（函数体内四个分支） | clang 用 `TrailingObjects` 存形参数组；本实现用 `vector<shared_ptr<TemplateParam>>` |
| `clang/lib/Parse/ParseTemplate.cpp:ParseTemplateParameters` 的 `TemplateScopes.Enter(TemplateParamScope)` | `src/parse/parser.cpp` 的 `TemplateParamFrame` + `openScope(ScopeKind::TemplateParam, …)` | clang 的模板形参作用域是统一 Scope 链上的一**种**；本实现用**帧链**顶替 |
| `clang/lib/Parse/ParseDeclCXX.cpp:Parser::ParseCXXMemberSpecification` 遇 `kw_template` ⇒ `ParseTemplateDeclarationOrSpecialization` | 类成员循环里的 `if (check(TokenType::KwTemplate))` 分支（`:1990` 附近） | clang 先解析**完整声明**再按结果反推 Decl 种类；本实现按"位置"分派（成员体恒为成员函数） |
| `clang/lib/Sema/SemaTemplate.cpp:Sema::ActOnTypeParameter`（形参入作用域） | `declareName(tp.name, paramBits)` + `frame.count` 同步 | clang 挂 `S->AddDecl(Param)`；本实现登记进活作用域表 + 帧计数 |

## 4 关键过程（ASCII 图）

### 4.1 改法前：两条路，两条依据

```
   自由模板                                   成员模板
   ───────                                   ───────
   parseTemplateDecl                         类成员循环
        │                                        │
        ├─ 建帧 + 开模板形参作用域                ├─ 建帧 + 开模板形参作用域
        │                                        │
        ├─ 形参表（内联，四形态 + 默认实参）        ├─ 形参表（另写：只认有名类型形参/NTTP）
        │   └─ frame.count = n  ✅               │   └─ frame.count 没同步  ★
        │                                        │
   parseType("T") → 命中帧 → TemplateParam("T")  parseType("T") → 帧里没有 → Class("T")
        │                                        │
   substituteType: Case 1（按节点）               substituteType: Case 6（按名字撞表）
        │                                        │
        └──── 同一个结果，两条依据 ────────────────┘
```

### 4.2 改法后：一条路

```
        自由模板                                   成员模板
        ───────                                   ───────
   parseTemplateDecl                         类成员循环
        │                                        │
        └──────────────┬─────────────────────────┘
                       ▼
        parseTemplateParameterList(decl, frame)
          ├─ 四种形态（类型 / 无名 / 模板模板 / NTTP）
          ├─ 默认实参 + 默认实参约束检查
          ├─ frame.count 同步  ★
          └─ 形参名登记进活作用域
                       │
        parseType("T") → 命中帧 → TemplateParam("T")   ← ★ 两边同形
```

### 4.3 ★ 本批踩到的坑：我以为"值位丢了"，其实没丢

评估阶段我判过一条"成员模板的名字在类作用域里丢了值位"（因为
`parseFunctionDecl` 登记方法名时模板形参作用域还开着）⇒ 结论是"值位被 closeScope 丢掉"。
**实测推翻**：突变验证时把 `declareName(method->name, ScopeBit::Value | ScopeBit::Template)`
改回 `ScopeBit::Template`，**日志一个字节都没变**。

真因在 `ParseScopeTable::openScope`（`src/sema/parse_scope.cpp:113`）：
函数名是**openScope** 登记的，不是 `declare` 登记的 ⇒ 它落在
`m_stack[nameScopeIndex()]` —— **最近的外层非模板形参作用域**（= 类作用域），
模板形参作用域被**跳过**了 ⇒ 值位一直都在。

★ 教训：**"名字在哪一层登记"要看登记的 API 是什么** ——
`declare`（登记进最内层）与 `openScope`（登记进最近的外层非模板形参作用域）
是两套落点规则；只盯着调用点上的 `openScope(TemplateParam)` 会算错。
（这一条也解释了为什么 `nameScopeIndex` 要跳过模板形参作用域：
模板自己的名字属于外层作用域，形参的名字属于形参域 —— learn/30 的约定。）

## 5 可复现实验命令

```bash
# ① 编译（0 警告）
./build.sh

# ② 单元测试（本批新增 6 例：MemberTemplateParams.*）
cmake --build build-linux --target unit_tests && ./build-linux/unit_tests --gtest_filter='MemberTemplateParams.*'

# ③ 集成测试（新增 1 份：tests/tmpl/test_tmpl_78_member_template_params.cpp）
./minicc tests/tmpl/test_tmpl_78_member_template_params.cpp -o /tmp/t78 && /tmp/t78; echo $?   # 22

# ④ clang 对拍（本批的检查点）
clang++-18 -std=c++20 tests/tmpl/test_tmpl_78_member_template_params.cpp -o /tmp/t78c && /tmp/t78c; echo $?   # 22

# ⑤ 依赖限定名（改法前报 unknown type name）
cat > /tmp/dep.cpp <<'EOF'
struct Box { int value; using type = int; };
struct S { template <class T> typename T::type get(T x) { return x.value; } };
int main(){ Box b; b.value = 5; S s; return s.get(b); }
EOF
./minicc /tmp/dep.cpp -o /tmp/dep && /tmp/dep; echo $?   # 5（与 clang 同）

# ⑥ 全量回归 + 日志基线
ctest --test-dir build-linux && ./logdiff.sh diff

# ⑦ 突变验证（5 条判据逐条实跑变红）
./tools/verify-member-template-mutations.sh
```

### 5.1 突变验证（5 条判据逐条实跑变红，2026-11-10）

脚本：`tools/verify-member-template-mutations.sh`（★ 还原用 cp 备份，不碰 git 工作区）

| # | 突变 | 变红的用例 |
|---|---|---|
| M1 | 不同步 `frame.count`（复现旧实现"自己的形参认不出"） | `OwnTypeParamIsTemplateParamNode`、`DependentQualifiedNameUsesTemplateParam`（2 例） |
| M2 | 去掉无名形参分支 | `UnnamedTypeParamAccepted`（1 例） |
| M3 | 去掉默认实参分支 | `DefaultTemplateArgAccepted`（1 例） |
| M4 | 模板模板形参分支改成报错 | `TemplateTemplateParamAccepted`（1 例） |
| M5 | 成员分支改回"自写简化版"（复现旧实现） | **5 例全红** |

★ M5 是"整体退回旧实现"的复现突变 ⇒ **本批的收口确实是这几条断言的判据来源**。
★ 每条突变在**集成侧**同样被抓（`test_tmpl_78_member_template_params.cpp` 编译期就红，rc=1 ≠ 期望 22）。
★ **单测断言的是"形态"而非"拼写"** ⇒ 变红的是**能力**被摘掉那一条，
而不是日志文本对不上；这正是本套件 6 例里只有 1 例对某条突变变红的原因。

## 6 真实版多做了什么、为什么

| 真实编译器 | 本实现 | 为什么能简化 |
|---|---|---|
| `ParseTemplateParameters` 是独立函数，自由/成员都调 | ★ 本批也做成独立函数（此前只有自由侧内联） | —— |
| clang 先解析完整声明、再按结果反推 Decl 种类（成员类模板/成员别名模板自然支持） | 成员循环按**位置**分派，只做成员函数模板 | 补齐那三种形态要连带做嵌套类模板的实例化、类外定义（learn/34 §5 边界表） |
| clang 的模板形参作用域是统一 Scope 链上的一"种" | 本实现用**帧链**（`TemplateParamFrame`）顶替 | 教学项目不追求作用域结构的完整，只要"形参在模板体里可见、出了模板体不可见" |
| clang 的形参默认实参在**推导期**未匹配时顶上（[temp.arg]/3） | 未做 ⇒ 自由与成员**同款**缺（B57） | 推导引擎的收尾阶段没有这一步；属独立缺口，不在本批收口范围 |
| clang 支持函数模板上的模板模板实参推导 | 未做 ⇒ 自由与成员**同款**缺（B57） | 推导器只认"裸形参名"，不认 `C<T>` 形态 |

## 7 边界与已知取舍

- **B57（未修，两侧同款）**：
  ① 函数模板上的**模板模板形参推导**（`template<template<class> class C, class T> T f(C<T>)`）
  —— 推导器只认"P 本身就是形参"，不认"模板 id 的基名是形参"；
  ② 推导期**未匹配的形参用默认实参顶上**（[temp.arg]/3）——
  `template<class T, class U = int> T f(T x)` 推导失败（U 无处可推）。
  ★ 两条**自由路径与成员路径同款** ⇒ 不是成员侧独有；收口后两侧行为一致。
- **成员类模板 / 成员别名模板 / 静态数据成员模板**仍未做（成员循环只做成员函数模板）。
  它们与本批**正交**：形参表已经共享了，但"模板体是类/别名/数据成员"这条分派还没做。
- **有意漂移**：4 份既有集成日志（成员模板相关）逐字节变化，但
  **rc、运行输出、汇编三者逐字节不变** —— 漂移全是
  ① 成员侧现在也打形参表那套日志（新增行）、
  ② `base = T (class/tparam)` → `(template param, scope hit)`、
  ③ `[subst]` 从 Case 6（按名兜底）切到 Case 1（按节点直替）。
  另 165 份集成日志逐字节零漂移。

## 8 下一批

- **ROADMAP §0.1 批 0-3**：B50（全局类对象：存储尺寸 + 地址约定）—— 段错误，优先于本批。
- **B57**：函数模板上的模板模板形参推导 + 默认实参填充（自由与成员同款）。
- 之后按 §0.2 进主线 K 六项（别名模板特化的非法程序拒收 / 偏特化模式的值位 /
  `template<auto V>` 偏特化 / 模板模板形参的形参包·默认实参·嵌套 /
  类外定义特化成员 / 函数模板显式 NTTP 实参）。
  ★ K-4 与本批同族：**形参表已经只有一份实现**，K-4 只需改自由侧那一处，成员侧自动跟随。

## 9 演进记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-10 | v1 | 首版：成员模板的形参表收口到自由模板那一份（`parseTemplateParameterList`）；连带修 B56（成员模板自己的形参认不出 ⇒ 依赖限定名被拒）+ 补齐成员侧 5 项形参能力；登记 B57（推导侧两处缺口，自由与成员同款） |
