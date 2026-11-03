# 56. 类模板里的嵌套类（[class.nest] + [temp.inst]/5，ROADMAP §0.1 批 0-2）

> 定位：让"类模板里的嵌套类"随实例化一起克隆 —— 从**响亮拒收合法程序**变成**与 clang 对拍一致**。
> 一句话：**嵌套类也是外层类模板的成员**；[temp.inst]/5 说实例化一个类模板
> 等价于实例化它的每个成员声明 —— 成员里既然有嵌套类，它就得被克隆。
> 关联：docs/learn/53（嵌套类）、docs/learn/49（作用域即身份）、docs/learn/54（vtable 槽句柄）
> 前置：ROADMAP §0.1 批 0-2（"实例化步骤 5.8：克隆嵌套类"）

## §0 定位与能力清单

| 能力 | 状态 | 落点 |
|---|---|---|
| 类模板实例化时把蓝图的**嵌套类克隆进实例** | ✅ 本批 | `src/sema/template_instantiation.cpp` 实例化步骤 5.8 |
| 嵌套类的**字段类型随外层形参替换**（`T v` → `int v`） | ✅ 本批 | `cloneNestedClass` → `cloneField` |
| 嵌套类的**方法签名与函数体随形参替换** | ✅ 本批 | `cloneNestedClass` → `cloneMethod` |
| 嵌套类**登记名带作用域**（`Box_int::Inner`） | ✅ 本批 | 前缀化仍由 `processClassDecl` 统一做（判据单点） |
| 嵌套类的**方法要注册（Pass 2）+ 分析函数体（Pass 3）** | ✅ 本批 | `forEachClassMethod` 递归（`src/sema/semantic_analyzer.cpp`） |
| **嵌套套嵌套**（`Deep_int::A::B` 三级登记名） | ✅ 本批 | `cloneNestedClass` 自身递归 |
| 不同实例 ⇒ 不同嵌套类（`Box_int::Inner` vs `Box_double::Inner`） | ✅ 本批 | 登记名派生自实例名（learn/49"作用域即身份"） |
| 蓝图里没有的成员名仍然**响亮拒收**（`Box<int>::Missing`） | ✅ 本批 | 边界不放大 |
| 嵌套类**按值返回** | ⬜ 边界 | 属"按值返回 + 拷贝省略"缺口（ROADMAP §0.1 批 0-1，与本批无关） |
| **类外定义**嵌套类成员（`struct Box<int>::Inner { … };`） | ⬜ 边界 | Parser 未支持任何类外成员定义 |
| 嵌套类写在**别名模板**里 | ⬜ 边界 | 别名不解糖成类，没有"实例化"可言（[temp.alias]/1） |
| 真 Itanium mangling（`<nested-name>` 的 `N…E` 编码） | ⬜ 边界 | 沿用教学版规则（`docs/learn/18` 口径不变） |

## 1 理论背景

### 1.1 标准怎么说

- **[class.nest]/1**：嵌套类是其外层类的**成员**，"没有特殊地位"。
  ★ 这句话是本批的全部依据：**成员**就要随模板一起实例化，嵌套类不例外。
- **[temp.inst]/5**：隐式实例化一个类模板特化，等价于隐式实例化它的
  **每个成员声明**（member declarations）。嵌套类声明就是成员声明之一。
- **[temp.subst]**：被替换的形参出现在**类型**位置时，替换进类型本身 ——
  `struct Inner { T v; }` 里的 `T` 在 `Box<int>` 里必须变成 `int`。
  ★ 不替换的后果比"报错"更坏：字段宽度仍按 `T`（未知）算 ⇒ 偏移错、
  读写出错，而**编译期全绿**。
- **[basic.scope.class]/2.1**：嵌套类的名字在其外层类作用域内可见 ⇒
  外层类可以拿嵌套类当字段类型（`Holder<T>::Cell c;`）。

### 1.2 编译原理怎么说

实例化 = **结构化替换 + 成员表递归搬运**。蓝图（模板定义）里有一张成员表：

```
蓝图 Box<T> 的成员表
├── fields      : [v: T]
├── methods     : [get(): T]
└── nestedClasses: [Inner]      ← ★ 它也是"成员表的一项"
```

实例化 = 拿一张**新表**，把每一项**复制一份并把形参替换掉**。
只搬前三项（字段 / 方法 / 别名）而漏掉第四项，就是本批修的病灶：
**产出的是一张缺项的表** —— 而缺项不会自己报错，只会在"用到它"的地方炸。

★ 这一条是本批最值得记的：**"少做一步"和"做错一步"的症状完全不同**。
少做 ⇒ 该有的类型不存在 ⇒ 响亮报错（还算幸运）；
做错（替换没做、名字拼错、符号算错）⇒ **静默算错**（本项目 B10/B13~B15/B22/B45 的同族）。

## 2 病灶与改法

### 2.1 病灶：实例化只搬"顶层四样"

修前的实例化步骤（`instantiateClassTemplate`）只搬四样东西：
字段、方法、类型别名、static 数据成员 —— **没有嵌套类**。
于是 `Box<int>::Inner` 在实例 `Box_int` 里压根不存在这个类型：

```
[Semantic Error] no type named 'Inner' in 'Box_int'
```

★ 这是**响亮拒收**（比静默算错好），但它是**合法程序被拒**
（clang rc=0）⇒ 属"功能缺失"档，必须补。

### 2.2 改法：实例化步骤 5.8

在实例化流程里加一步"克隆嵌套类"，与 clang 的做法同形：

```
蓝图 Box<T>
   │
   ├─ 5.5 克隆成员模板（learn/34）
   ├─ 5.6 挂成员模板到实例
   ├─ 5.7 克隆 static 数据成员（learn/44）
   └─ 5.8 ★克隆嵌套类（本批）
         │
         ├─ 字段：cloneField(f, subst)      ⇒ T → int
         ├─ 方法：cloneMethod(m, subst)      ⇒ 签名 + 函数体一起替换
         ├─ static 成员 / 类型别名 / 成员模板：同样随 subst 替换
         └─ 嵌套类：递归 cloneNestedClass     ⇒ 套嵌套同理
```

克隆产物挂进 `newClass->nestedClasses` —— 即**嵌套类成为实例类的成员**，
与 clang 的 `Owner->addDecl(Record)`（`TemplateDeclInstantiator::VisitCXXRecordDecl`）同形。

### 2.3 判据单点：登记名不在克隆处定

克隆出来的嵌套类，登记名是 `Box_int::Inner`。
★ **这个前缀不在克隆处拼** —— `cloneNestedClass` 只交出**裸名**（`Inner`），
前缀化仍由 `processClassDecl` 统一做（"登记名 = 外层登记名 + :: + 裸名"只有那一份）。

★ 理由与 learn/49 的"作用域即身份"同源，也与 B45 的教训同源：
**"两处共用同一个函数"只治了"写错"，治不了"算两次"** —— 克隆发生在实例化期、
前缀化发生在登记期，两个时刻看到的状态不同就还是会漂。
治本只有一条：**只算一次，别处只引用**。

### 2.4 连带修：Pass 2 / Pass 3 必须递归进嵌套类

克隆出方法还不够 —— 方法还得**注册**（Pass 2）与**分析函数体**（Pass 3）。
修前 `getOrInstantiateClass` 是扁平的 `for (auto& method : instance->methods)`，
而**嵌套类的方法不在外层类的 `methods` 表里** ⇒
不递归 ⇒ CodeGen 从没收到那些方法 ⇒ 链接期 `undefined reference`。

★ 与命名空间递归是同一个坑的同款（learn/26）：**DeclContext 树本身就是递归的**，
扁平遍历必然漏。修法是抽一个递归走查原语 `forEachClassMethod`，
两个消费点（注册 / 分析）共用 —— 承 B10 的教训：**同一判据不许写两份**。

```
        forEachClassMethod(Box_int)
                │
                ├─ methods: [Inner(int), get()]        ← 外层自己的
                └─ nestedClasses: [Inner]
                        └─ methods: [Inner(int), get()] ← ★ 嵌套类的（原来漏了）
```

## 3 clang 源码对照表

| clang 源码 | 本实现位置 | 简化了什么 |
|---|---|---|
| `clang/lib/Sema/SemaTemplateInstantiate.cpp:Sema::InstantiateClass` —— 成员循环 `for (auto *Member : Pattern->decls())` → `Instantiator.Visit(Member)` | `src/sema/template_instantiation.cpp:TemplateInstantiator::instantiate`（步骤 5.8） | clang 用**一个成员循环**统一处理字段/方法/嵌套记录；本实现按成员种类**分步骤**（字段/方法/别名/static/嵌套类各一步）⇒ 克隆逻辑分散，但每步的替换语义看得清 |
| `clang/lib/Sema/SemaTemplateInstantiateDecl.cpp:TemplateDeclInstantiator::VisitCXXRecordDecl` —— `Owner->addDecl(Record)`（嵌套类挂到实例化后的外层类） | 同上，`newClass->nestedClasses.push_back(clonedNc)` | 同形：嵌套类成为实例类的成员 |
| 同上 —— `Record->setInstantiationOfMemberClass(D, …)`（记下"我是那个成员类的实例化"） | 登记名派生自实例名（`Box_int::Inner`） | clang 靠 `MemberSpecializationInfo` 记录身份；本项目靠**登记名**（learn/49：作用域即身份） |
| `clang/lib/Sema/SemaTemplateInstantiate.cpp:Sema::InstantiateClass` —— `if (Member->getDeclContext() != Pattern) continue;`（语义上下文不同则不实例化） | 本实现暂不做此判据 | clang 区分"词法上下文在外层、语义上下文在别处"的成员（如 `template<int i> class A { class B *g; };` 里的 `B`）；本项目暂无此场景 ⇒ 留作边界 |
| `clang/lib/Sema/SemaDecl.cpp:Sema::PushDeclContext` —— 嵌套类的 DeclContext 指向外层类 ⇒ 遍历 DeclContext 树自然覆盖 | `src/sema/semantic_analyzer.cpp` 的 `forEachClassMethod` 显式递归 `nestedClasses` | clang 靠**上下文树**天然递归；本项目是**自己的 AST 树**，得自己写递归（这就是为什么这步会漏） |

★ 对照 clang 最重要的一条经验：
**clang 里根本没有"克隆嵌套类"这个专门步骤** —— 它只有一个成员循环，
循环体对每个成员调 `Visit`，而 `Visit` 遇到 `CXXRecordDecl` 就递归进
`VisitCXXRecordDecl`。**"嵌套类要跟着实例化"在 clang 是成员循环的自然结果，
不是一条特判** —— 与 learn/53 的"作用域不是表、是一条链"是同一条道理。

## 4 关键过程（ASCII 图）

```
Box<int> 实例化
   │
   │  蓝图 template<class T> struct Box { struct Inner { T v; }; };
   ▼
┌──────────────────────────────────────────────────────────────┐
│ 步骤 5.8  克隆嵌套类                                          │
│   for (nc : blueprint->nestedClasses)                         │
│       cloned = cloneNestedClass(nc, subst)                    │
│         ├─ fields   : cloneField  → T ⇒ int                  │
│         ├─ methods  : cloneMethod → 签名 + 函数体             │
│         └─ nested   : 递归                                       │
│       newClass->nestedClasses.push_back(cloned)   ← 裸名 "Inner"│
└──────────────────────────────────────────────────────────────┘
   │
   ▼  登记（processClassDecl 统一前缀化 —— 判据单点）
[register] class 'Box_int::Inner'
   │
   ├─ Pass 2  注册方法：forEachClassMethod(Box_int)
   │            └─ nestedClasses: [Inner] ⇒ Inner::Inner / Inner::get
   └─ Pass 3  分析函数体：同上递归
   │
   ▼
[clone:nested]   field 'v' : T → int
[register] Box_int::Inner::get() → int   [mangled: Box_int::Inner_get]
```

★ 实测日志（`tests/lang/test_basics_35_nested_class_in_template.cpp`）：

```
  [instantiate:class] ★ on-demand instantiation: Box<int>
  ║ ── Nested Class Substitution ──
    [clone:nested]   field 'v' : T → int
  ║   nested class 'Inner' → 克隆（字段/方法随形参替换）
  [register] class 'Box_int::Inner'
  [register] Box_int::Inner::Inner(int x) → void    [mangled: Box_int::Inner_Inner_1]
  [register] Box_int::Inner::get() → int            [mangled: Box_int::Inner_get]
```

## 5 可复现实验命令

```bash
# ① 编译（0 警告）
cmake -S . -B build-linux -DCMAKE_CXX_COMPILER=clang++-18 \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache
cmake --build build-linux -j6

# ② 单元测试（403 例；本批新增 6 例 NestedClassInTemplate.*）
ctest --test-dir build-linux -R NestedClassInTemplate --output-on-failure

# ③ 集成测试（167 份日志逐字节比对；本批新增 2 份）
./logdiff.sh diff

# ④ clang 对拍：本批的两个用例
clang++-18 -std=c++20 tests/lang/test_basics_35_nested_class_in_template.cpp -o /tmp/t35 && /tmp/t35; echo "rc=$?"   # ⇒ rc=0
clang++-18 -std=c++20 tests/lang/test_basics_36_nested_class_in_template_error.cpp -o /tmp/t36; echo "rc=$?"              # ⇒ rc=1
#   clang 文案：no type named 'Missing' in 'Box<int>'
#   minicc 文案：no type named 'Missing' in 'Box_int'   ← 差在【拼写】（实例名 vs 模板 id）
#   ★ 诊断的**类型名**必须是实例名 —— 查的是实例的成员表，不是蓝图的
```

### 5.1 突变验证（3 条判据逐条实跑，2026-11-03）

判据的"承重"要靠**实测**，不靠声称 —— 逐条改坏源码、重编、跑单测、看红几条：

| 突变 | 改法 | 实测变红 | 与单测头声称是否一致 |
|---|---|---|---|
| ① 不克隆嵌套类 | 步骤 5.8 的循环换成空表（`decltype(...){}`） | **5 例**（261~265） | ✗ 头里原写"4 例" ⇒ **已按实测改正** |
| ② 克隆但**不替换**字段类型 | `cloneField(f, subst)` ⇒ `f` | **2 例**（262、264） | ✗ 头里原写"1 例" ⇒ 已改正（264 顺带红：方法体读到的字段类型也跟着错） |
| ③ Pass 2/3 **不递归**进嵌套类 | `forEachClassMethod` ⇒ 只遍历 `instance->methods` | **1 例**（264） | ✓ 一致 |

★ 值得记的一条：① 的实测条数（5）比声称（4）多一条 —— 差的那条正是
**"边界不放大"**（266 `Box<int>::Missing`）**仍然绿**：
克隆没了，但"找不到成员"的报错路径本来就不依赖克隆 ⇒ **判据不放大**是它的设计意图，
不是它没被覆盖。★ 这类"实测比声称多/少一条"的差，往往正是**边界用例**的位置。

## 6 真实版多做了什么、为什么

| 真实版（clang） | 本实现 | 为什么这样简化 |
|---|---|---|
| 一个成员循环统一 `Visit` 每个成员 | 按成员种类**分五步**克隆（字段/方法/别名/static/嵌套类） | 本项目的 AST 是**自己的树**（不是 clang 的 DeclContext 树）⇒ 分步写更直白；但代价就是**多一步就多一处会漏**（本批漏的就是第 6 步） |
| `VisitCXXRecordDecl` 递归处理嵌套类，含 attrs / 访问权限 / mangling number / previous decl 链 | 只搬字段 / 方法 / static / 别名 / 成员模板 / 嵌套类 | 教学版不实现完整 Decl 语义；这些差异不影响"嵌套类能用" |
| `setInstantiationOfMemberClass` 记录"成员类实例化"关系 | 靠**登记名**（`Box_int::Inner`）承担身份 | learn/49 的"作用域即身份"：名字即身份，本项目没有 `MemberSpecializationInfo` 那套元信息 |
| `Member->getDeclContext() != Pattern` 的语义上下文判据 | 未做 | 本项目暂无"词法在外层、语义在别处"的成员场景 ⇒ 留边界（§7） |
| 嵌套类的 mangling 用 Itanium `<nested-name>`（`N…E`） | 教学版 `Box_int::Inner` 扁平拼接 | 沿用 learn/18 的教学版口径；符号唯一性由登记名保证 |

★ **本批真正的收获不是"支持了嵌套类"**，而是两条可复用的判据：

1. **"成员表"是递归结构，遍历它就必须递归** ——
   命名空间（learn/26）、嵌套类（本批）两次都是同一句话：
   *DeclContext 树本身就是递归的，扁平遍历必然漏*。
2. **"只算一次，别处只引用"** ——
   登记名的前缀化只有 `processClassDecl` 一处（§2.3），
   克隆处只交裸名 ⇒ 不给"两处各算一遍"留缝（learn/49 / B45 同源）。

## 7 边界与已知取舍

| 边界 | 现状 | 影响 |
|---|---|---|
| 嵌套类**按值返回**（`Box<int>::Inner f();`） | 不支持（初始化式被丢弃 ⇒ 静默算错） | 属 ROADMAP §0.1 批 0-1（按值返回 + 拷贝省略），**与本批无关**（普通类同样中招） |
| **类外定义**嵌套类成员 | Parser 未支持任何类外成员定义 | `struct Box<int>::Inner { … };` 解析期拒 |
| 嵌套类写在**别名模板**里 | 不支持 | 别名不解糖成类（[temp.alias]/1），没有"实例化"可言 |
| 语义上下文判据（clang 的 `getDeclContext() != Pattern`） | 未做 | 暂无对应场景；将来支持"模板内声明的全局类"时需补 |
| 真 Itanium mangling 的 `<nested-name>` 编码 | 未做 | 沿用教学版扁平拼接；符号唯一性由登记名保证 |
| 诊断文案 `Box_int` vs `Box<int>` | minicc 用实例名 | 查的是**实例**的成员表 ⇒ 报实例名才对；clang 报模板 id 是另一回事（clang 查的是模板的名字表） |

## 8 下一批

ROADMAP §0.1 批 0 剩 **0-1（按值返回 + 拷贝省略，★ 静默算错）**；
之后按 §0.2 进 **主线 K** 六项（别名模板特化的非法程序拒收 → 偏特化模式的值位 →
`template<auto V>` 偏特化 → 模板模板形参的形参包/默认实参/嵌套 → 类外定义特化成员 →
函数模板显式 NTTP 实参）。

## 9 演进记录

| 版本 | 日期 | 变更 |
|---|---|---|
| v1 | 2026-11-03 | 首版：实例化步骤 5.8（克隆嵌套类）+ `forEachClassMethod` 递归；BUGS.md B47；ROADMAP §0.1 批 0-2 收口 |
