# 26 · 命名空间下的名字查找：限定名、作用域内非限定名、ADL

> 一句话：`get(s)` 里的 `get` 可能**根本不在**普通查找找得到的地方 ——
> ADL 让编译器去问"实参类型是从哪个命名空间来的"。
> 而且它**不是兜底**，是把候选**补进同一个候选集**再一起裁决；
> 实现成"先到先得"会静默调错函数（能编译、能链接、结果错）。

---

## §0 定位与能力清单

`get(s)` 里的 `get` 可能**根本不在**普通查找找得到的地方 —— ADL 让编译器去问
"实参类型是从哪个命名空间来的"（[basic.lookup.argdep]）。它存在的理由是**运算符重载**：
`a + b` 会被改写成 `operator+(a, b)` 的查找，若只做普通查找，用户为自定义类型写的
`N::operator+` 永远找不到。而它**不是兜底** —— 是把候选**补进同一个候选集**再一起裁决，
实现成"先到先得"会静默调错函数（能编译、能链接、结果错）。

| 能力 | 一句话 | 状态 |
|---|---|---|
| ① 限定名查找（[basic.lookup.qual]） | `N::S` / `N::get(s)` 只在 N 里找，不进全局 | 已实现 |
| ② 命名空间内**非限定**查找（[basic.scope.namespace]） | 本命名空间 → 外层 → 全局（`resolveType` 的命名空间作用域回退） | 已实现 |
| ③ **ADL**（[basic.lookup.argdep]） | 非限定调用时把关联命名空间里的同名函数**补进候选集** | 已实现 |
| ADL 的**关联集合** | 实参类型是 `N::S` ⇒ 关联命名空间 `{N}` | 部分（只做"类名前缀"；基类与模板实参的关联集合未做） |
| ADL 与普通查找**同场竞争** | `measure(s)` 里 `N::measure(S)` 与全局 `measure(int)` 一起裁决 | 已实现 |
| 候选池过滤（只收普通自由函数） | 成员函数与函数模板实例各走各的路径 | 已实现 |
| 嵌套命名空间的**名字前缀化** | `A::B::f` 的注册名是完整路径 | 已实现 |
| 转换序列 rank（[over.ics.rank]） | promotion / conversion 的排序 | 未做（只区分"精确 / 不精确"两档） |
| 歧义诊断 | 两个候选同样精确时报 `ambiguous` | 未做（取注册顺序靠前者） |
| 嵌套命名空间的**作用域链** | 只链到最近一层 | 未做 |
| `using` 引入 | `using N::get;` / `using namespace N;` | 未做 |
| 内联命名空间 / 匿名命名空间 | — | 未做 |

> ★ §5「边界」是**成文时的快照**，本表按**当前**状态填写，差异在演进记录里逐条列出。

## 1. 理论背景

### 1.1 三种查找，三条路（[basic.lookup]）

```cpp
namespace N {
    struct S { int v; };
    int get(S s) { return s.v; }
}
int measure(int x) { return x + 100; }
int main() {
    N::S s;               // ① 限定名查找
    N::get(s);            // ① 限定名查找
    get(s);               // ③ ADL
    measure(s);           // ③ ADL —— 与全局 measure(int) 同场竞争
}
```

| 写法 | 走哪条规则 | 在哪找 |
|---|---|---|
| `N::S` / `N::get(s)` | [basic.lookup.qual] 限定名查找 | **只在** N 里找，不进全局 |
| 命名空间内的 `S`（`int get(S s)`） | [basic.scope.namespace] 非限定查找 | 本命名空间 → 外层 → 全局 |
| `get(s)`（N 内没有 get） | [basic.lookup.argdep] ADL | 普通查找的**并集** + 实参类型的关联命名空间 |

### 1.2 ADL 到底"依赖"什么

"实参依赖查找"（Argument-Dependent Lookup，旧称 Koenig lookup）：
非限定函数调用时，除了普通查找，还要去**实参类型的关联命名空间**里找同名函数。
关联集合的完整定义在 [basic.lookup.argdep]/2，包含：

```
实参类型是  N::S          ⇒ 关联命名空间 {N}
实参类型是  N::S*         ⇒ 同上（指针/引用/数组/函数指针层层剥开）
实参类型是  类 C : public B ⇒ C 的关联集合 ∪ B 的关联集合
实参类型是  C<T>          ⇒ C 的 ∪ T 的
```

本实现覆盖第一条（类名自身的前缀）—— 因为 minicc 把命名空间成员的名字
**前缀化**成 `N::S` 登记，从类名反推关联命名空间是免费的。

**为什么语言需要这条规则**：运算符重载。`a + b` 会被改写成
`operator+(a, b)` 的查找，若只做普通查找，用户为自定义类型定义的
`N::operator+` 永远找不到（除非写在全局）。这条规则就是为了让
"类型和它的运算符住在同一个命名空间"这种自然写法成立。

### 1.3 最容易写错的一点：ADL 不是兜底

标准的表述是：非限定查找得到集合 S1，ADL 得到集合 S2，
**候选集是 S1 ∪ S2**，然后一起做重载决议（[over.match]）。

```
把 ADL 实现成"普通查找失败才用" ⇒
    measure(s) 普通查找命中 measure(int)（按名字+个数命中）⇒ 直接返回
    ⇒ 静默调用 measure(int)：S 到 int 根本没有转换，却一路编译链接通过
    ⇒ 结果是错的，而且没有任何诊断
```

本实现因此先合并候选、再裁决（`inferCall` 的候选池）：

| 步骤 | 判据 | 说明 |
|---|---|---|
| ① 收集 | 普通查找（裸名）+ ADL（`N::名字`） | 两批都进 `pool` |
| ② 过滤 | 只收"普通自由函数" | 成员函数与模板实例不在此列（见 §3.2） |
| ③ 裁决 | 形参类型**逐位精确相等**者优先 | [over.match.best] 的教学精简版 |
| ④ 无精确匹配 | 退回既有的"按个数命中"行为 | 保持向后兼容 |

真正的重载决议要算隐式转换序列的 rank（[over.ics.rank]），
本实现只区分"精确 / 不精确"两档 —— 足够覆盖 ADL 的经典场景。

---

## 2. 数据流（ASCII 图）

```
源码  namespace N { struct S{}; int measure(S); }   int measure(int);
      N::S s;   int c = measure(s);

inferCall(measure, [s])
   │
   ├─ P1 成员方法查找（callee 不是 MemberExpr）—— 跳过
   │
   ├─ 合并候选（本阶段新增）
   │    ├─ 普通查找：m_functions 里 name == "measure" 的自由函数
   │    │              ⇒ { int measure(int) }
   │    └─ ADL：实参类型 N::S ⇒ 关联命名空间 {N}
   │             ⇒ 在 m_functions 里找 name == "N::measure"
   │             ⇒ { int N::measure(N::S) }
   │
   ├─ 裁决：逐位精确匹配
   │    measure(int)      vs 实参 (N::S)  ⇒ int ≠ N::S   ✗
   │    N::measure(N::S)  vs 实参 (N::S)  ⇒ 相等        ✓ ← 胜出
   │
   ├─ 名字原地改写：callee->name = "N::measure"
   │     （CodeGen 按名字发射 callq，再经 asmSymbol 净化成 N__measure）
   ▼
返回 int
```

---

## 3. 实现改动清单

| 文件 | 改动 | 作用 |
|---|---|---|
| `src/semantic_analyzer.cpp` | ① `inferCall` 的普通函数路径重写为"合并候选 + 精确匹配裁决"（原 P2 + 新增 ADL） | ADL 与重载同场竞争 |
| `src/semantic_analyzer.cpp` | ② `resolveType` 新增**命名空间作用域回退**（查不到时按 `当前命名空间::名字` 再查） | 命名空间内非限定类型名 |
| `include/semantic_analyzer.h` | 新增 `m_currentNamespace` | 记住"现在身处哪个命名空间" |
| `src/semantic_analyzer.cpp` | ③ `processNamespaceDecl` 进入/退出时设置该状态 | Pass 1 期间生效 |
| `src/semantic_analyzer.cpp` | ④ **Pass 2 / Pass 3 递归进命名空间** | 见 §3.1 |
| `src/codegen.cpp` | ⑤ 符号拼装点统一过 `asmSymbol`（栈对象构造、析构调用、vtable 条目、thunk、RTTI 名字串标签） | 见 §3.3 |
| `src/template_instantiation.cpp` | ⑥ `mangleRTTI` / `mangleVTable` 内的类名先净化再算长度 | 命名空间类的 RTTI/vtable 符号合法且自洽 |

### 3.1 扁平遍历漏掉命名空间（Pass 2/3 的洞）

```
单元顶层 declarations: [ NamespaceDecl N ]        ← 类/函数都藏在里面
Pass 1: processDecl 递归进命名空间 ✓
Pass 2: for (decl : unit.declarations)            ← 只看到 NamespaceDecl
        既不是 FunctionDecl 也不是 ClassDecl ⇒ 什么也没注册
Pass 3: 同上 ⇒ 函数体从未分析
```

症状很有迷惑性：Sema 侧一切正常（类布局有、符号表有、类型解析全对），
但 CodeGen 从没收到那些方法 ⇒ 合成构造/析构不发射 ⇒
链接期报 `undefined reference to 'N__S_N__S'`。

顺带修掉了它的**镜像 bug**：`processNamespaceDecl` 在 Pass 1 里顺手
`registerFunction`（当初正是为了绕过扁平遍历），Pass 2 一递归就重复注册，
汇编器报 `symbol 'N__get' is already defined`。
**只改名不注册**是 Pass 1 对命名空间内函数应有的行为。

对照 clang：`DeclContext` 树本身就是递归结构，不存在"顶层遍历"这回事。

### 3.2 候选池为什么必须过滤（一个真实的回归）

第一版实现直接扫 `m_functions` 收集同名候选，结果 `tests/tmpl/test_tmpl_17_explicit_args.cpp`
从"能跑"变成"链接失败：undefined reference to 'mix'"。原因：

```
m_functions 里混着三类东西，name 字段分别是：
  普通自由函数      name = "mix"          mangledName = "mix"
  类成员函数        name = "get"          mangledName = "Box_int_get"
  函数模板实例      name = "mix"          mangledName = "_Z3mixIiiE"   ← 保留模板名！
```

`mix<int>(3, 4)` 的第二次调用时，第一次调用产生的实例 `mix<int,int>` 已在表里，
它的形参类型是具体的 `(int, int)` ⇒ 被"精确匹配"抢先命中 ⇒
调用的却是**尚未按调用点重写名字**的实例，`callq mix` 自然链接不到。

修法：候选池只收 `ownerClassName.empty() && mangledName == name` 的自由函数。
模板实例与成员函数各走各的路径（S5 模板路径 / P3 类作用域分支）。

### 3.3 符号净化：源码名 ≠ 汇编标签

gas 的标签只允许 `[A-Za-z0-9_.$]`，而源码层的名字里有 `::`。
codegen 早有 `asmSymbol()` 做净化，但**只用在部分拼装点**上，
命名空间类的构造/析构调用（`N::S_N::S`）与 vtable 条目仍是裸拼，
汇编期直接 `Error: junk '::S_N::S' after expression`。

本阶段把剩下的拼装点全部收口到 `asmSymbol`，并让
`mangleRTTI` / `mangleVTable` 在内部先净化再算长度（`_ZTV3N_S`）——
产生端与引用端是同一个函数，天然一致。

> ⚠️ 净化不是单射：`N::S` 与 `N_S` 会撞名。教学实现接受这一点，
> 真编译器用 Itanium ABI 的 `_ZN1N1SE`（长度前缀 + 嵌套编码）保证可逆。

---

## 4. 可复现实验

```bash
# ① 端到端（期望 40；clang++-18 同样 40）
./minicc tests/decl/test_decl_02_adl_and_qualified_lookup.cpp -o /tmp/t02 && /tmp/t02; echo $?

# ② 看三条查找路径各自的日志
./minicc tests/decl/test_decl_02_adl_and_qualified_lookup.cpp -S -o /dev/null 2>&1 \
  | grep -E "\[call\]|在命名空间"

# ③ 反例：把 ADL 实现成"兜底"就会错在这里（本文件第 ④ 检查点）
#    c = measure(s)：正确 7；若普通查找先返回，会得到 107。

# ④ 全量回归
ctest --test-dir build-linux                                   # 154/154
```

---

## 5. 边界

| 缺口 | 说明 |
|---|---|
| 关联集合不完整 | 只做"类名前缀"，未含基类与模板实参的关联命名空间 |
| 转换序列 rank | 只区分"精确 / 不精确"，没有 [over.ics.rank] 的 promotion/conversion 排序 |
| 歧义诊断 | 两个候选同样精确时不报 ambiguous，取注册顺序靠前者 |
| 嵌套命名空间 | `A::B::f` 的**名字前缀化**与 ADL 前缀推导已通，但作用域链只有最近一层 |
| `using` 引入 | `using N::get;` / `using namespace N;` 未实现 |
| 内联命名空间 / 匿名命名空间 | 未实现 |

## 6. 真实版多做了什么、为什么

（把 §1 / §3 里对 clang 的对照与 §5 的边界表合并成一张两列表。）

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `DeclContext` 是**递归树**，命名空间天然被遍历到 | 本项目的声明列表是**扁平**的 ⇒ Pass 2 / Pass 3 必须显式递归进命名空间；顺带修掉"Pass 1 顺手注册 ⇒ 重复定义"的镜像 bug |
| 关联集合包含**基类**与**模板实参**的命名空间（[basic.lookup.argdep]/2 全表） | 本实现只做"类名自身的前缀" —— 因为 minicc 把命名空间成员的名字**前缀化**成 `N::S` 登记，从类名反推关联命名空间是免费的 |
| 重载决议算完整的**隐式转换序列 rank**（[over.ics.rank]） | 只区分"精确 / 不精确"两档 —— 足够覆盖 ADL 的经典场景（精确匹配 + 兜底命中） |
| 候选同样精确时报 **`ambiguous`** | 本实现取注册顺序靠前者 —— 属于**静默选错**方向，但本文档只覆盖 ADL 与候选合并；歧义诊断留待重载决议那条线（见 learn/36） |
| 符号名用 **Itanium ABI 的长度前缀 + 嵌套编码**（`_ZN1N1SE`）保证可逆 | `asmSymbol()` 的净化**不是单射**（`N::S` 与 `N_S` 撞名）—— 教学实现接受这一点，换来的是人读名 |
| `using` 引入声明 / `using namespace` | 未实现 —— 属另一条独立的查找路径 |
| 内联命名空间 / 匿名命名空间 | 未实现 |
| 嵌套命名空间有完整**作用域链** | 本实现只链到最近一层；嵌套的完整链后来由 learn/49（作用域即身份）补上 |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-09-16 | 模板机制主线收官（commit 5d9b78d）：`inferCall` 重写为「合并候选 + 精确匹配裁决」，ADL 补进**同一候选集** | ADL 若做成"兜底"会**静默调错函数** —— 能编译、能链接、结果错 |
| 2026-09-16 | `resolveType` 新增命名空间作用域回退 + `m_currentNamespace` 状态 | 命名空间内的非限定类型名原先查不到 |
| 2026-09-16 | Pass 2 / Pass 3 递归进命名空间（原来只看到 `NamespaceDecl` 本身） | 症状迷惑：Sema 全对、CodeGen 没收到方法 ⇒ 链接期 `undefined reference to 'N__S_N__S'` |
| 2026-09-16 | 顺带修掉镜像 bug：Pass 1 顺手 `registerFunction` ⇒ Pass 2 递归后重复注册（`symbol 'N__get' is already defined`） | **只改名不注册**才是 Pass 1 对命名空间内函数应有的行为 |
| 2026-09-16 | 候选池只收"普通自由函数"（`ownerClassName.empty() && mangledName == name`） | 函数模板实例的 `name` 保留模板名，会被精确匹配抢走 ⇒ `test_tmpl_17` 链接失败 |
| 2026-09-16 | 符号拼装点统一过 `asmSymbol`；`mangleRTTI` / `mangleVTable` 先净化再算长度 | 产生端与引用端同函数 ⇒ 天然一致；否则汇编期报 `junk '::S_N::S' after expression` |
| 2026-09-16 | 学习文档 18..29 一次性成文（commit 582887c） | — |
| 2026-11-02 | 作用域即身份（B38，commit 4f1fb49）：命名空间的**前缀化**改用完整路径，嵌套命名空间的完整链打通 | §5 里"嵌套命名空间作用域链只有最近一层"那条被填上；限定名不再剥尾，查不到即报 `no template named 'B' in namespace 'N'` |

