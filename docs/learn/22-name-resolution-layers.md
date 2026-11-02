# 22 · 名字在哪一层当真 —— 从解析前瞻到符号命名

> 主题：**同一个名字，在编译管线的不同层扮演完全不同的角色。**
> 把层搞混，就会写出"查不到就当没事"的静默洞，或者"裁决对了但符号撞了"的怪错。
> 本篇是 ③⑤⑦⑩ 四个 bug（外加 `test_tmpl_44/45` 两处守卫）的合订本，
> 也是「Parser 为什么不能决定用哪个模板」这个问题的完整答案。

---

## §0 定位与能力清单

同一个名字在编译管线的四层里扮演**完全不同的角色**：解析层它只是一串字符、
语义层它必须能查到声明、裁决层根本不看名字、符号层它就是键。把层搞混，就会写出
"查不到就当没事"的**静默洞**，或者"裁决对了但符号撞了"的怪错。本篇给出一张分层表，
并把四个方向各一个的真 bug（③⑤⑦⑩）按层归位。

| 能力 | 一句话 | 状态 |
|---|---|---|
| L1 解析层只判**形态**（[stmt.ambig]、[dcl.type]） | 标识符开头的语句用"存档 → 试探 → 回滚"判断是声明还是表达式 | 已实现 |
| L1 声明符后缀 `*` / `&` / `&&` 一并跳过 | `S& r = a;` 不再被误判成表达式语句 | 已实现 |
| L1 `const` 计入类型关键字表 | 语句层 `const int x = 1;` 可解析 | 已实现 |
| L2 查不到类型名必须**报错** | `Undeclared q;` 不再 rc=0 静默通过 | 已实现 |
| L3 裁决层**结构化**择优 | 偏特化按合一 + 引用结构检查匹配，不看名字 | 已实现 |
| L4 实例名清洗保持**单射** | `Box<int*>` → `Box_intP`、`Box<int&>` → `Box_intR`，不再撞符号 | 已实现 |
| L4 撞名守卫 | 残余非单射在实例化入口拦一道，报"实例名撞车"而不是留给 `as` | 已实现 |
| [stmt.ambig] 的**最终裁决**（Sema 回头重解析） | `a < b > c;` 的完整歧义裁决 | 未做（本实现无 Token 注解机制；现状是 L2 兜底**响亮报错**，不静默） |
| `int const x` 后置 const | 与 cv 位置同源 | 未做 |
| ⑧ 一元 `*` 解引用 | 表达式层取 pointee | 已实现（learn/31 补齐；本文成文时列为边界） |
| ⑨ cv 限定在引用/指针里的**位置** | `const int&` 应建成 `LValueRef(Const(Int))` | 已实现（learn/23 修正；本文成文时列为边界） |
| ⑪ `const int*` 与 `&y` 的类型不兼容诊断 | 两侧打印一模一样却不相等 | 已实现（learn/23 修正） |
| `static_cast<T&&>` | — | 未做（见 ROADMAP） |

> ★ §九「已知边界」是**成文时的快照**，本表按**当前**状态填写，两者的差异在演进记录里逐条列出。

## 一、理论背景

一句话代码 `Box<int*> b;` 会穿过四层，每层对「`Box`」的理解都不同：

| 层 | 位置 | 名字是什么 | 在这一层"当真"的依据 | 标准条款 |
|---|---|---|---|---|
| L1 解析 | `parser.cpp:1250` | **没有任何含义**，只是一串字符 | 只判形态：后面跟不跟 `<`、`*`、`&`、标识符 | [stmt.ambig]、[dcl.type] |
| L2 语义 | `semantic_analyzer.cpp:565` | 必须**能查到声明** | 符号表 / 类模板注册表 | [basic.scope]、[temp.names] |
| L3 裁决 | `[spec:select]` | **不看名字**，看结构 | 合一算法 + 偏序 dominance | [temp.class.spec.match]、[temp.class.order] |
| L4 符号 | `template_instantiation.cpp:265` | **名字就是键**，必须单射 | 字符串清洗 | [basic.link]（外部链接名唯一） |

**四条各管各的。** L1 不能越权替 L3 做决定（Parser 没有符号表）；L4 又不能偷懒
（名字撞了就是两条实例抢一个符号）。

---

## 二、L1：解析层 —— 前瞻只判形态

### 问题

`parseStatement` 遇到标识符开头时，要在两条路里选一条：

```
Box<int> b;      →  变量声明
foo(1);          →  表达式语句
```

LL(1) 一个 Token 分不开，于是用「试探法」：存档 → 往前跳过"类型模样"的东西 →
看下一个是不是标识符 → 回滚后重新正式解析（`parser.cpp:1250-1297`）。

这段的关键性质：**它是一个探针，读到的东西全部丢弃**。真正记录实参是回滚之后的
`parseType()`（第二遍）。所以"前瞻里没记录任何类型"不是 bug —— 它是设计。

```
        m_pos = savedPos ─┐
                          │  ① 探针：只数括号
   Box < int * > b ;      │     丢弃全部 token
   ↑                       │
   └── parseType() ② ──────┘  真正建 AST：templateArgs = [int*]
```

### ③ 探针只跳 `*`，不跳 `&`

```cpp
// 修复前
while (match(TokenType::Star)) {}
```

于是 `S& r = a;`、`Box<int>& r = b;` 的探针停在 `&` 上，发现下一个不是
标识符，判成表达式 → `Expected ';' after expression`。

内建类型（`int& r = a;`）不走这条前瞻（它命中 `isTypeKeyword()` 分支），
所以洞只在**标识符开头的类型**上暴露 —— 这种"两条路径判定不一致"是漏点的常见来源。

修法：`*` 与 `&`/`&&` 是**声明符**的一部分（[dcl.decl]），一起跳。
clang 对应 `ParseDeclarator` 的指针/引用算子循环。

### ⑤ `const` 不是类型关键字

`isTypeKeyword()`（`token.h:233`）只列了 `int/double/bool/void/auto/decltype`，
漏了 `const`。于是语句层 `const int x = 1;` 掉进表达式分支，
报出误导性的 `Unexpected token 'const' in expression`。

顶层没暴露：`parseTopLevelDecl` 用的是"试探性 `parseType()` + 回滚"，
压根不看 `isTypeKeyword()` —— 又一处路径不一致。

`const` 是 type-specifier（[dcl.type]：type-specifier-seq 可以是 cv-qualifier +
类型），进这张表是它该待的地方。

### ④ 已知边界：`a < b > c;`

按 [stmt.ambig]，声明/表达式歧义**在语义分析后裁决**："能当声明就当声明"。
`a` 若不是模板，`a<b>c` 就不是合法声明，按表达式解析。

本实现做不了：判断 `a` 是不是模板需要符号表，而 L1 没有（也不该有）。
更麻烦的是前向引用：

```cpp
void f() { Box<int> b; }              // 用到在前
template <typename T> struct Box{};   // 声明在后
```

clang 允许 —— 它靠 `TryAnnotateTypeToken` 先猜、猜错再回来重解析。
本实现一旦"查不到就不算声明"，这种写法立刻崩。

**现状**：`a < b > c;` 仍按声明解析，但 L2 的兜底校验（见下）会让它**响亮报错**
而不是静默生成错误代码。修复前它是 rc=0 悄悄编过的。

---

## 三、L2：语义层 —— 查不到就必须报

### ⑦ 符号表查不到就静默放行

`resolveType` 里对 `Class` 节点的处理是"查到就换成注册版，查不到就算了"：

```cpp
Symbol* sym = m_symbolTable.lookup(type->name);
if (sym && sym->kind == SymbolKind::Type && sym->type && sym->type != type)
    return sym->type;
// ← 修复前：这里直接 fall through，返回那个空布局的 Class 节点
```

后果（全部 rc=0 静默通过）：

```cpp
Undeclared q;        // 未声明的类型名
int a = 1; a q;      // 拿变量名当类型名
```

危害不止"少报一个错"：空布局会让后续字段访问拿到垃圾偏移，错误推迟到运行期。

它还是 ④「能静默编译」的原因 —— 补上这道校验后，④ 至少会响亮地报
`unknown type name 'a'`，并且带一句 [stmt.ambig] 的提示。

clang 对应诊断：`error: unknown type name 'Undeclared'`。

---

## 四、L3：裁决层 —— 结构化，不看名字

**这一层的答案是：不只看名字，根本不用名字。**

拿 `Box<T*>` / `Box<T&>` 两条偏特化选 `Box<int*>` 时的实况日志：

```
[spec:select] ★ selecting class template 'Box' for <int*>
[spec:select]   ├─ ② 候选：'Box<T*>' 匹配成功
[spec:select]   ├─ ② 候选不匹配：reference structure mismatch:
                 pattern 'T&' requires an lvalue reference but argument 'int*' is not a reference
[spec:select]   │  唯一候选 → 直接选中 'Box<T*>'
```

`T&` 被**结构性地**拒掉（引用结构检查），不是"名字对不上"。这是合一算法
在 `templateArgs` 上跑。实例缓存同理，键是 `"Box<int*>"`（实参的 `toString` 拼接），
也是无损的。

```
候选集          模式        实参       合一结果
Box<T*>    →    [T*]        [int*]    T := int      ✓
Box<T&>    →    [T&]        [int*]    结构不符      ✗（引用结构检查）
Box<T**>   →    [T**]       [int*]    结构不符      ✗
```

---

## 五、L4：符号层 —— 名字是键，必须单射

### ⑩ `Box<int*>` 与 `Box<int&>` 撞符号

实例名同时充当**汇编符号前缀**（方法符号 = `<实例名>_<方法名>`）。
清洗规则原来把 `*` `&` `-` `+` `,` `<` `>` 一把抓全换成 `_`：

```
修复前：  Box<int*>  → Box_int_
          Box<int&>  → Box_int_        ← 撞了
          as: Error: symbol `Box_int__dtor' is already defined
```

最刺眼的地方在于：**L3 判得完全正确**（两条各走各的偏特化、各自实例化），
错的只有 L4 的命名。报错还来自 `as`，指不到根因。

修法：一字符对一字母，恢复单射。

| 字符 | 替换 | 含义 | 例 |
|---|---|---|---|
| `*` | `P` | Pointer | `Box<int*>` → `Box_intP` |
| `&` | `R` | Reference | `Box<int&>` → `Box_intR` |
| `-` | `N` | Negative | `Buf<-3>` → `Buf_N3` |
| `,` `<` `>` | `_` | 结构符 | `Map<int,double>` → `Map_int_double` |

残余的非单射（`_` 既是结构符又是分隔符，如 `Map<int,double>` 与
`Map<int_double>`）由**撞名守卫**兜底 —— `m_instanceNameOwner`
（清洗名 → 无损键）在实例化入口拦一道，冲突时抛
`[Instantiate Error] 实例名撞车`，把根因说清楚，而不是留给 `as`。

---

## 六、clang 源码对照

| 本实现 | clang 位置 | 简化了什么 |
|---|---|---|
| `parseStatement` 的存档/回滚试探（`parser.cpp:1250`） | `Parser::isDeclarationSpecifier` + `TryAnnotateTypeToken`（`Parser.cpp`） | clang 用 Token 注解而不是回滚重解析；[stmt.ambig] 的最终裁决靠 Sema 回头重解析，本实现未做 |
| `parseType` 的 `*`/`&` 后缀循环 | `Parser::ParseDeclarator` | 同一层，本实现不支持括号声明符、数组声明符 |
| `isTypeKeyword()` | `tok::isAnyIdentifier` + 类型说明符表 | clang 的表大得多（含 `constexpr`、`volatile`、`signed`…） |
| `resolveType` 的符号表兜底 | `Sema::getTypeName` / `LookupTypeName` | clang 还有 ADL、依赖名两阶段查找 |
| `[spec:select]` 三路择优 | `Sema::InstantiateClassTemplateSpecialization` + `isMoreSpecializedThan` | 见 docs/learn/21 |
| 实例名清洗 + 撞名守卫 | 不存在（clang 全用 Itanium mangling 做符号名） | 本实现为**教学可读性**保留人读名，因此必须自己保证单射 |

## 六 之补 真实版多做了什么、为什么

（把 §六 对照表第三列"简化了什么"与 §九 已知边界合并成一张两列表。）

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| Parser 用 **Token 注解**（`TryAnnotateTypeToken` / `isDeclarationSpecifier`）决定"这是不是声明"，而不是"存档 → 试探 → 回滚"重解析 | 本实现没有 Token 注解机制；试探法在教学上更直白（探针读到的东西**全部丢弃**，真正建 AST 的是回滚后的第二遍 `parseType()`） |
| [stmt.ambig] 的最终裁决由 **Sema 回头重解析**完成 | 没有 Token 注解就没有"回头"的锚点 —— 这条要等模板名登记表（B31）与作用域裁决（B37/B38）落地才可能补；当前靠 L2 兜底**响亮报错**代替静默生成错误代码 |
| `parseType` 的声明符还支持**括号声明符**与**数组声明符** | 本实现的 `*`/`&` 后缀循环只覆盖指针与引用；数组是独立的一条主线（后来由 learn/42 单独补齐） |
| 类型说明符表大得多（含 `constexpr`、`volatile`、`signed`…） | `isTypeKeyword()` 只列本项目**已实现**的类型关键字 —— 表大不带来教学收益，反而让"哪些还没做"变得不可见 |
| `Sema::getTypeName` / `LookupTypeName` 还带 **ADL** 与**依赖名的两阶段查找** | 二者后来由 learn/26（ADL + 限定名）与 learn/25（依赖类型名）分别补齐，见本文演进记录 |
| 符号名**完全交给 Itanium mangling** | 本实现为**教学可读性**保留人读实例名（`Box_intP`），代价是必须自己保证单射：字符一对一字母 + 撞名守卫（`m_instanceNameOwner`） |
| clang 的实例类型**带着实参表**（是一个 Decl），不需要另造缓存键 | minicc 的实例类型是**改名后的 Type**（`Box_intP`），可读串不是单射 ⇒ 缓存键必须另取无损形式（`Box<int*>`） |

---

## 七、可复现实验

```bash
cd build-linux && cmake --build . -j8

# ③ 模板 id + 引用声明（修复前 Parse Error）
cat > /tmp/a.cpp <<'EOF'
template <typename T> struct Box { T v; };
int main() { Box<int> b; Box<int>& r = b; return r.v; }
EOF
./minicc /tmp/a.cpp -o /tmp/a && /tmp/a; echo $?

# ⑤ 语句层 const（修复前 Parse Error）
#   注意：这里【不能】写成 `const int* p = &x;` —— 那会撞上 ⑨
#   （cv 限定套在指针外面），见文末已知边界。
cat > /tmp/b.cpp <<'EOF'
int main() { const int x = 4; return x; }
EOF
./minicc /tmp/b.cpp -o /tmp/b && /tmp/b; echo $?

# ⑦ 未声明类型名（修复前 rc=0 静默通过）
echo 'int main(){ Undeclared q; return 0; }' > /tmp/c.cpp
./minicc /tmp/c.cpp -o /tmp/c; echo $?        # 现应报 unknown type name

# ⑩ 指针版 / 引用版共存（修复前 as 报 duplicate symbol）
./minicc ../tests/tmpl/test_tmpl_46_ptr_vs_ref_instance.cpp -o /tmp/d && /tmp/d; echo $?   # → 125
./minicc ../tests/tmpl/test_tmpl_46_ptr_vs_ref_instance.cpp -S -o /dev/null 2>&1 | grep 'instantiate:name'
```

预期（最后一条）。前四行来自本程序，后六行来自 main.cpp Phase 4 那个
**固定的演示 pass**（`main.cpp:674`，用 6 种实参展示实例化引擎，与源程序无关）：

```
[instantiate:name] 'Box<int*>'  → 汇编符号前缀 'Box_intP'
[instantiate:name] 'Box<int&>'  → 汇编符号前缀 'Box_intR'
[instantiate:name] 'Box<int**>' → 汇编符号前缀 'Box_intPP'
[instantiate:name] 'Box<int*&>' → 汇编符号前缀 'Box_intPR'
--- 以下是 Phase 4 演示 pass ---
[instantiate:name] 'Box<int>'          → 'Box_int'
[instantiate:name] 'Box<double>'       → 'Box_double'
[instantiate:name] 'Box<int*>'         → 'Box_intP'
[instantiate:name] 'Box<int&>'         → 'Box_intR'
[instantiate:name] 'Box<int&&>'        → 'Box_intRR'
[instantiate:name] 'Box<const int&>'   → 'Box_constintR'
```

---

## 八、一句话总结每一层

```
L1 解析：名字没有含义      →  只判形态，判错了也要让 L2 有机会喊
L2 语义：名字必须能查到    →  查不到 = 硬错误，绝不能静默放行
L3 裁决：名字不参与        →  合一 + 偏序，纯结构
L4 符号：名字就是键        →  必须单射，撞了就是两条实例抢一个符号
```

---

## 九、已知边界（未做）

| # | 现象 | 为什么没做 |
|---|---|---|
| ④ | `a < b > c;` 按声明解析 | [stmt.ambig] 需解析期符号表 + 失败重解析；本实现无 Token 注解机制。现状：L2 兜底会响亮报错，不会静默 |
| ⑥ | `int const x` 后置 const 不解析 | 与 ⑨ 同源（cv 限定在类型里的位置），需一并处理 |
| ⑧ | 一元 `*` 解引用未实现 | `parseUnaryExpr` 只认 `-` `!` `&`；缺 `UnaryOp::Deref` + Sema 取 pointee + CodeGen 发 load |
| ⑨ | `const int&` 建成 `const (int&)`、`const int*` 建成 `const (int*)` | **cv 限定被套在引用/指针外面**，是 ill-formed 类型（[dcl.ref]/1 禁止 cv 引用）。正确结构是 `LValueRef(Const(Int))`。根因：`parseType` 的 Step 4 在后缀循环之后才应用 const，应提前到 Step 2 之后。影响面广（推导、偏序、匹配），单独立项 |
| ⑪ | `const int y; const int* p = &y;` → `Type mismatch in 'p': declared 'const int*', got 'const int*'` | ⑨ 的第二副面孔 —— **两侧打印一模一样却不相等**，诊断完全无法自解。根因同 ⑨：`p` 是 `Const(Pointer(Int))`（const 指针），`&y` 是 `Pointer(Const(Int))`（指向 const 的指针）。注意 `int y; const int* p = &y;` 反而是通过的（`typeCompatible` 会剥顶层 const），所以这个洞只在**被指对象本身也是 const** 时露头。⑤ 修复把语句层 `const` 放行之后，本洞从"解析期就报错"变成"语义期报错"，暴露面变大 |
| — | `static_cast<T&&>` 未实现 | 见 ROADMAP |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-09-16 | 模板机制主线收官（commit 5d9b78d）：③⑤⑦⑩ 四个修复落地，本文把四个 bug 合订成"名字在哪一层当真" | 四处错法是同一句话的四个方向：查不到就当没事 / 声明符后缀漏跳 / cv 关键字漏列 / 显示名当键 |
| 2026-09-16 | 学习文档 18..29 一次性成文（commit 582887c），§六 对照表与 §九 已知边界同期写定 | 一处一 bug 分散在多处，看不出"层次"这条主线 |
| 2026-09-16 | 命名空间下的三种查找（限定名 / 作用域内非限定名 / **ADL**）补齐（learn/26） | §六 对照表里"clang 还有 ADL"那条被填上 |
| 2026-09-22 | ⑨ cv 限定位置修正（learn/23）与 ⑧ 一元 `*` 解引用（learn/31，commit e0ff653） | 本文 §九 把它们记为边界；后来各自单独立项修掉 —— cv 位置一改，所有"按旧形态看类型"的地方都要重看一遍 |
| 2026-10-23 | 模板名登记表消歧：`a<b>(c)` 不再被误判成模板调用（B31，commit 908bbde） | 旧的"实参表空跑成功 + `>` 后跟 `(`"判据会**拒收合法程序** |
| 2026-10-27 | 语句层 [stmt.ambig] 前瞻重写为「TokenCheckpoint + `parseType()` 试探」（B35，commit 25622a4） | 旧前瞻是 parseType 语法的**残缺复制品**；改后至少能响亮报错而不是静默 |
| 2026-10-28 | parser 阶段名字识别：多 bit 分类 + 作用域裁决，对齐 clang `ClassifyName` 骨架（commit 83b4891） | 名字的**种类**要在解析期可问，但**可见性**由 Sema 裁决 |
| 2026-11-02 | 名字可见性裁决下沉 Sema + 作用域即身份（B37/B38，commit 4f1fb49） | 见 learn/48 / learn/49；本文 L2"查不到就报错"扩成了"查得到但**在这里**看不见也不算数" |

