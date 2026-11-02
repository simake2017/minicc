# 17. 多继承布局：primary base 判定、size/align 分离与嵌套子对象

> 对应 C++ 标准：[class.mi]（多继承对象模型）、[class.mem]/2（成员必须 complete）、
> [basic.align]（对齐要求）、[class.virtual]/2（动态分派）、Itanium C++ ABI §2.4
> （non-virtual base allocation）与 §2.9（RTTI/typeinfo）。
> 参照源码：`clang/lib/AST/RecordLayoutBuilder.cpp`（ItaniumRecordLayoutBuilder）。

## §0 定位与能力清单

多继承下最容易踩的坑有三个：**① primary base 是谁**（是第一个**多态**基类，
不是声明序第一个）；**② size 与 align 是两个独立维度**（拿 size 当 align 会算出
非 2 幂的错位布局）；**③ vtable 槽里放的是符号名**，不是"函数本身"——
次基类没覆写时，槽里该指的是**更上游**那个基类的实现，而不是"次基类名 + 裸名"
拼出来的假符号。本文讲这三件事的判据，以及踩过的五个连环 bug。

| 能力 | 一句话 | 状态 |
|---|---|---|
| Itanium ABI 的 primary base | 第一个**多态**基类占 offset 0，与派生类共享主虚表 | 已实现 |
| 全非多态时的"视作 primary" | 取声明序第一个基类 | 已实现 |
| 非虚基类子对象摆放 | 其余子对象从 primary 尾部依次摆放 | 已实现 |
| size 与 align **分离** | `alignOf()`：Class 递归取成员 max（封顶 8）、标量 `min(size,8)` | 已实现 |
| [class.mem]/2 成员类型必须 complete | 字段注册前 `resolveType(field.type)`，占位类型换成注册版 | 已实现 |
| 嵌套类字段 = 内联子对象 | `addr(o) + offset(f) + offset(a)` 的逐层地址折叠 | 已实现 |
| vtable 槽放**符号名** + 判据单点 | `memberMethodSymbolName` 一处产出、定义点与槽位点两处使用 | 已实现 |
| 次表条目**原样透传** | `VTableEntry secEntry = baseEntry;`——名字是**路径**不是位置 | 已实现 |
| 槽位身份 = 裸名 + 形参类型链 | `VTableEntry::signature`（[class.virtual]/2 的覆写判据） | 已实现 |
| 次表覆写项的 **thunk** 跳板 | `this` 归顶后再调本类实现（简化 mangling：`类名_函数名_thunk偏移`） | 已实现 |
| 字段查找三级 | 全限定名 / 自身字段优先（隐藏）/ 继承 | 已实现 |
| 歧义诊断 | `Member 'x' is ambiguous ...`（[class.member.lookup]/8） | 已实现 |
| 虚继承 / 虚基类 | vbptr、vbtable、VTT | 未做 |
| 空基类优化（EBO） | | 未做 |
| 位域（bitfield） | | 未做 |
| tail padding 复用 | | 未做 |
| 16 字节 SSE 对齐 | | 未做（对齐封顶 8） |
| 子对象按 **nvalign** 对齐 | clang 按 nvalign 对齐；本项目一律 8 字节 | 未做 |
| `ASTRecordLayout` 的不可变缓存对象 | clang 的布局是**不可变**对象；本项目是两段式（工作清单 → 整体覆盖） | 未做 |
| 递归按需构建布局 | clang 的 `ASTContext::getASTRecordLayout` 按需递归 + 缓存 | 未做（靠声明序：基类必须先定义） |

## 1. 理论背景

### 1.1 primary base：谁占 offset 0

Itanium ABI 规定：派生类的对象布局中，**第一个多态基类**（有 vtable 的）
作为 primary base，子对象放在 offset 0，与派生类**共享主虚表**（primary base
optimization）。关键点是"**第一个多态的**"，不是"声明的第一个"：

```text
class D : public A(非多态), public P(多态);

声明序：A 在前 —— 但 primary 是 P！
正确布局：  [P 子对象@0 (含 _vptr)] [A 子对象@P尾部] [D 自身字段]
错误布局：  A@0 且 P@0 → 重叠，A.x 压在 _vptr 上，写 x 即毁虚表指针
```

全部基类都非多态时，无 _vptr 之争，约定取声明序第一个当"视作 primary"。

### 1.2 size 与 align 是两个独立维度

clang 摆放每个成员时从 `Context.getTypeInfoInChars(T)` 一次取回
`TI.Width`（大小）与 `TI.Align`（对齐）两个**独立**的值
（`RecordLayoutBuilder.cpp:1850 LayoutField`）：

- 标量：查 TargetInfo ABI 表（int→4、double→8、bool→1、指针→8）
- 类类型：align = 成员 align 的递归最大值（`UpdateAlignment`，:2213），
  **与 size 无关**——`Five{bool×5}` size=5、align=1；`Five{int,int}` size=8、align=4

拿 size 当 align 会推出 `alignTo(4,5)=5` 这种非 2 幂的错位布局。

### 1.3 成员类型必须 complete

[class.mem]/2：类成员不能是不完整类型。clang 里成员布局信息永远来自
`ASTContext::getASTRecordLayout`（按需递归构建 + 缓存），**不存在占位类型**。
教学编译器若让 Parser 的占位 `Class("Five")`（空布局，totalSize=0）漏进布局
计算，字段会占 0 字节 → 后续字段重叠 → malloc 分配不足 → 越界写。

### 1.4 嵌套类字段 = 内联子对象

`o->f.a` 中 `f` 不是指针，是内联在父对象里的子对象。访问 = 地址折叠：
`addr(o) + offset(f) + offset(a)`。任何把子对象头 8 字节当指针解引用
（movq）的代码生成都是错的。

### 1.5 槽位里放的是【符号名】，不是"函数本身"

vtable 是**数据**：槽位在目标文件里就是一条重定位（`.quad <sym>`），链接器只认
裸字符串。Itanium ABI 里这张表由每个 TU 各自发射，链接器再用一组规则
（vague linkage / 主 vtable）去重 —— 所以"槽里的名字"与"函数定义的名字"
必须是同一个字符串，这个是**硬约束**，与语言层的类型检查无关。

对照 clang：`CodeGenModule::EmitVTable` 填槽时拿到的是刚发射过的函数
（`GlobalDecl` 句柄），名字来自同一个 `MangleContext` ⇒ 结构上不可能错配。
minicc 没有句柄、只能各自拼字符串 ⇒ 判据必须收口（见 docs/learn/13 §13.4.3）。

**次表（secondary）的语义**因此要格外小心：次基类子对象在派生类里**换了位置**，
但槽里该放谁的名字，取决于**谁提供了这个实现**，而不是**这是谁的表**。

**槽位身份不只是名字（B20 缺陷 c / B22，已修）**：`vtable` 的"这是谁的槽"
= （裸名 + **形参类型**），不是裸名 —— 这正是 [class.virtual]/2 的覆写判据。
`class C { virtual int f(); int f(int); };` 里，非虚的 `f(int)` 会认领 `f()` 的槽
并被误标成 `virtual`，于是 `c.f(2)` 走虚调用跳进无参的 `f()`（clang rc=0、
minicc 编译 rc=0 但**运行返回 255** —— 静默算错，汇编里一点异常都看不出来）。
修法是 `VTableEntry::signature` 存形参类型链，匹配时一并比对（见 §2 Bug 4）。

★ 这个缺陷还留下一条**方法论**：只断言"守恒式"不变量（谁占槽 + 谁被直接调用 = 总定义数）
**挡不住它** —— 认错槽位后两者整体互换，计数照样配平（突变验证时单测全绿）。
有判别力的是**不对称**的那条：经基类指针的虚调用**必须**是 `callq *%rax` 间接调用。
见 tests/unit/test_member_identity.cpp 的 `VirtualCallOnPointerStaysVirtual`。

## 2. 踩坑史（四个连环 bug，全部实测复现后修复）

### Bug 1：primary 判定两处标准打架

- `processClassDecl` 基类循环：非多态分支"边扫边放"先占 offset 0，
  primary 分支又写死 offset 0 → `D : A(非多态), P(多态)` 两子对象重叠
- `computeClassLayout` 用 `bi==0`（声明序）选 primary，与上面的
  "第一个多态基类"标准不一致

**修复**：循环内不再计算 offset（删除 currentOffset），循环后统一
`[relocate]`：primary 恒占 0，其余从 primary 尾部依次 `alignTo(place,8)`。
对照 clang `DeterminePrimaryBase`（:852）/ `LayoutNonVirtualBases`（:1017）。

### Bug 2：全非多态时 [relocate] 的 place 停在 0

`place` 计算被 `if (anyPoly)` 守卫 → `D : A, B`（都非多态）时 B 被放到
offset 0，与 A 重叠（构造 B 覆盖 A.x，实测返回 70 而非 60）。

**修复**：去掉守卫，place 恒从 primary（含"视作 primary"的首个非多态基类）
尾部起算。

### Bug 3：size/align 混用 + 占位类型 + 子对象当指针（嵌套字段三连坑）

```text
class Five { bool b1..b5; int a; };
class Outer { int tag; Five f; int tail; };

修复前：+0 tag(4) +4 f(0 bytes!) +4 tail(4)  size=8   ← f 与 tail 重叠
clang：  0 tag    4 f(占4..15)    16 tail     size=20
```

三处修复：

1. **Sema**：字段注册前 `field.type = resolveType(field.type)`，
   占位类型换成注册版 → size 从 0 变真值
2. **Sema**：新增 `alignOf()`（Class 递归取成员 max，封顶 8；标量 min(size,8)），
   `computeClassLayout` 用它替代 `min(sizeInBytes(),8)`
3. **CodeGen**：
   - `emitMember` 遇类类型字段改发 `leaq offset(%rax),%rax`（取子对象地址）
   - ctor 初始化列表遇类类型字段改发"调整 this + callq 嵌套构造函数"
     （`f(7,9)` 原来是把实参裸 movq 进子对象头部）
   - 标量字段初始化按宽度选 movb/movl/movq（原来一律 movq，4 字节 int
     字段会踩坏相邻字段）

### Bug 4：次表槽里被"重造"出来的假符号（BUGS.md B20 缺陷 b）

- 收集次基类条目时，名字被按 `次基类名 + "_" + 裸名` **重造**了一遍
  （原意："这条槽属于次基类 Q，那就该叫 Q_f"）
- 但基类自己**没覆写**该虚函数时，槽里原本指向的是**更上游**基类的实现

```text
class X { virtual int f(); };       // 实现处：.globl X_f
class P : public X {};              // primary：条目原样拷贝 ⇒ X_f ✓
class Q : public X {};              // secondary：条目被重造成 Q_f ✗（没人定义 Q_f）
class Diamond : public P, public Q {};

修复前：Diamond 次表[0] = .quad Q_f   ⇒ [LINK ERROR] undefined reference to 'Q_f'
修复后：Diamond 次表[0] = .quad X_f   ⇒ ✓
```

**教训**：名字是**路径**（谁提供的实现），不是**位置**（这是谁的表）。
同一句话在本项目已经出现过三次：B13~B15（把显示名当索引）、docs/learn/23
（`toString()` 不是单射却被当缓存键）、B20（这里）。
**修法**：次表条目**原样透传**（`VTableEntry secEntry = baseEntry;`）；
真正的覆写发生在后面的方法循环里 —— 那时才把槽改指本类实现并配 thunk。

**现场**：这个 bug 一直藏在 `tests/mi/test_mi_04_error.cpp` 里。那个文件当时写的是
菱形继承，期望"重复基类被拒收"，实测 clang rc=0（非虚继承的菱形在标准下**合法**，
只有成员访问歧义才报错）——rc=1 的真原因是这条 `undefined reference to 'Q_f'`。
修好 B20 后该文件改为真正的错误用例（私有继承），菱形正例搬到
`tests/mi/test_mi_12_secondary_inherited_slot.cpp`。
**又一次印证**：logdiff 基线会把**失败**也固化成契约，修完必须回头看 rc。

### Bug 5：槽位身份与符号定名的同一条判据（B20 缺陷 c + B22，已修）

上面 Bug 4 暴露出"槽位匹配只比裸名"⇒ 非虚的 `f(int)` 认领虚的 `f()` 的槽、并被误标
`virtual`。实测 `C c; return c.f() + c.f(2) - 3;`：clang rc=0，minicc 编译 rc=0 但
**运行返回 255**（静默算错）。

把这条往外推一步就是 B22 的另三处现场 —— **同一句"这是哪个函数"总共写在四个地方**：

| # | 落点 | 修前 | 修后 |
|---|---|---|---|
| ① | 汇编符号名 `memberMethodSymbolName` | 后缀只编码形参**个数** ⇒ 同签名个数的重载都叫 `C_f_1`，as 报 `symbol is already defined` | 有兄弟时再挂**形参类型链**（`C_f_1_int` / `C_f_1_S`） |
| ② | vtable 槽位身份 `processClassDecl` | 只比裸名 | 再比 `entry.signature` |
| ③ | 成员调用选定 `findMethodInClass` | 循环内首个同名同个数命中即返回 | 收候选集 → `pickBestByArgs` 择优 |
| ④ | 构造函数选定 `processVarDeclStmt` | **只数个数**，且把刚推出来的实参类型扔掉 | 同走 `pickBestByArgs` |

③④ 共用同一个本地静态函数 —— 承 B10/B12 那条铁律：**同一判据不许写两份**。
回归：`tests/lang/test_basics_04_*`、`tests/mi/test_mi_13_*`、
单测 `tests/unit/test_member_identity.cpp`（6 例，三条突变逐条实跑变红）；
既有 116 个集成用例**逐字节零漂移**（符号名只在"有兄弟"时才挂类型链）。

## 3. clang 源码对照表

| clang（RecordLayoutBuilder.cpp） | minicc 对应位置 | 简化了什么 |
|---|---|---|
| `DeterminePrimaryBase` :852 | `processClassDecl` [relocate] 块（semantic_analyzer.cpp:673 起） | 无虚继承/无空基类优化 |
| `LayoutNonVirtualBases` :1017 / `LayoutBase` :1198 | 同上，`sub.offset = alignTo(place,8)` | 子对象一律 8B 对齐（clang 按 nvalign） |
| `LayoutField` :1850（TI.Width/TI.Align 分离） | `computeClassLayout` 字段放置循环 + `alignOf()` | 无位域/无 tail padding 复用 |
| `UpdateAlignment` :2213（成员 align 递归 max） | `alignOf()` Class 分支 | 封顶 8，无 16B SSE 对齐 |
| `FinishLayout` :2135（nvsize/一次成型） | `computeClassLayout` 尾部 + :891 回填 | 两段式（decl->fields 工作清单 → 整体覆盖），clang 是不可变 ASTRecordLayout |
| ASTContext::getASTRecordLayout（complete 保证） | 字段注册处 `resolveType(field.type)` | 无递归按需构建，靠声明序（基类必须先定义） |
| CGRecordLayoutBuilder 链式访问 GEP 折叠 | `emitMember` 逐层 leaq | 保留每一跳便于观察 |
| `CodeGenModule::EmitVTable`（槽放刚发射的 GlobalDecl） | `processClassDecl` 的条目循环 + `codegen.cpp` 次表段发射 | 无句柄，槽里是**拼出来的字符串**（故判据必须单点，见 §2 Bug 4）|
| `ItaniumMangleContext::mangleName` | `memberMethodSymbolName()`（semantic_analyzer.cpp:1445） | 简化规则：`类名_方法名[_<形参个数>]`，不编码形参类型 |

## 4. 可复现实验

```bash
# oracle：看 clang 的真实布局
clang++-18 -Xclang -fdump-record-layouts -c tests/mi/test_mi_07_nested_class_field.cpp -o /dev/null

# minicc：观察 [relocate]/布局日志 + 运行验证
./build-linux/minicc tests/mi/test_mi_06_nonpoly_only.cpp -o /tmp/m6 && /tmp/m6; echo $?   # 60
./build-linux/minicc tests/mi/test_mi_07_nested_class_field.cpp -o /tmp/m7 && /tmp/m7; echo $? # 9

# Bug 4（B20 缺陷 b）：次表槽该指谁 —— 看汇编里的 .quad 那一行
#   注：-S 且不给 -o 时，汇编落在【源文件旁边的同名 .s】（tests/mi/*.s 即由此而来）
./minicc tests/mi/test_mi_12_secondary_inherited_slot.cpp -S > /dev/null
grep -n "secondary\[" tests/mi/test_mi_12_secondary_inherited_slot.s   # X_f / X_g_1（不是 Q_f）

# Bug 4 的两个 oracle 对照（clang 全 rc=0）
clang++-18 -std=c++20 tests/mi/test_mi_12_secondary_inherited_slot.cpp -o /tmp/c12 && /tmp/c12; echo $?
clang++-18 -std=c++20 tests/lang/test_basics_03_virtual_with_params.cpp -o /tmp/c03 && /tmp/c03; echo $?

# 回归红线
for i in 01 02 03 04 05 06 07 08 09 10; do
  ./build-linux/minicc tests/tmpl/test_tmpl_${i}_*.cpp -o /tmp/t && /tmp/t; echo "tmpl_$i=$?"
done
```

## 5. 关键过程 ASCII 图

```text
D : A(非多态), P(多态) —— [relocate] 摆放过程：

bases 收集（循环内，不算 offset）:      [A: non-poly] [P: primary]
                                              │
[relocate] 第一步：找 primary ────────────────┘
   第一个 hasVTable 的基类 = P → P.offset := 0, place := P.totalSize(16)
[relocate] 第二步：其余子对象依次摆放
   A.offset := alignTo(16,8) = 16, place := 16 + 4 = 20
[computeClassLayout] 第三步：字段落位
   _vptr@0  p@8(复用 P 内偏移)  A.x@16(=base.offset+bf.offset)
   d@24(自身字段, alignTo(20,8)=24)  → totalSize = 32

Outer{int tag; Five f; int tail}（Five{bool×5,int a}）:

   resolveType 后 f.type → 注册版 Five（size=12, alignOf=4）
   tag:  alignTo(0,4)=0   → +0,  place=4
   f:    alignTo(4,4)=4   → +4,  place=4+12=16     ← align 取 4（成员 int a）
   tail: alignTo(16,4)=16 → +16, place=20
   totalSize = alignTo(20,4) = 20                   ← 与 clang 完全一致

o->f.a 的代码生成（逐层地址折叠）:
   movq -16(%rbp), %rax     # o
   leaq 4(%rax), %rax       # &o->f   ← 类类型字段：取地址不是取值
   movl 8(%rax), %eax       # f.a     ← Five 内 a 的偏移
```

## 6. 真实版多做了什么、为什么

★ 素材来源：本文 §3 clang 源码对照表的「简化了什么」列 + §2 的五个 bug
+ `docs/BUGS.md` B11~B15/B20/B22 + CLAUDE.md「未做」清单。**未查证的写「（本项目未查证）」**。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| **虚继承**：vbptr、vbtable、VTT（虚表构造器），共享子对象推到尾部靠 vbptr 间接寻址 | 未做：本项目无虚继承支持。代价是"菱形 + 虚基类"这套最复杂的布局完全缺席（learn/11 §4.6 只做了 clang 侧的实测对照） |
| **空基类优化**（EBO）：空基类可与派生类同址 | 未做：多占几个字节，但换不来概念复杂度 |
| **位域**（bitfield）的打包与跨单元分配 | 未做：位域的布局规则是 C++ 里最琐碎的一块，与"primary base 判定"这条主线正交 |
| **tail padding 复用**：派生类可以复用基类尾部空隙放字段 | 未做：布局更松，但规则更少 |
| **nvalign** 对齐（子对象按其自身的自然对齐而非一律 8） | 本项目一律 8 字节对齐：规则少一条，但布局与 clang 可能不同（体积略大） |
| **16 字节 SSE 对齐**（`__m128` 等向量的对齐要求） | 未做：没有向量类型 |
| `ASTRecordLayout` **不可变**的布局对象 + `ASTContext::getASTRecordLayout` 的**递归按需构建 + 缓存** | 本项目是两段式（`decl->fields` 工作清单 → 整体覆盖），且靠**声明序**保证基类先定义（不递归、不缓存）。代价：前向引用的基类排不出 |
| `CGRecordLayoutBuilder` 的**链式访问 GEP 折叠**（一次寻址算完 `o->f.a`） | 本项目**保留每一跳**（逐层 `leaq`）——多两条指令，但"哪一跳算错了"一眼可见（这正是 Bug 3 能被当场抓住的原因） |
| `CodeGenModule::EmitVTable` 填槽时拿到**刚发射过的函数句柄**（`GlobalDecl`） | 本项目没有句柄，槽里是**各自拼出来的字符串** ⇒ 判据必须收口到一处（`memberMethodSymbolName`），否则定义点与槽位点漂一点就是链接期 `undefined reference`（Bug 4 / B20） |
| `ItaniumMangleContext::mangleName` 完整编码形参类型 | 本项目简化为 `类名_方法名[_<形参个数>]`，**只在"有兄弟"时**才再挂形参类型链——挂了就动了全部既有产物，logdiff 基线逐字节比对是硬约束（Bug 5 / B22） |
| 派生类到基类的**引用绑定** `Base& b = d;` | 未做：CLAUDE.md 已把它列进"未做"清单——跨转型 `B*`→`A*` 也未做 |
| 限定名调用成员函数 `d.Base::f(2)` | 未做：B21 只管**数据**成员（`obj.Base::v`），成员**函数**的限定调用未做 |
| `using Base::f;` 把被隐藏的名字拉回来 | 未做 |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-09-09 | 主线 G 落地：primary base 判定 + 次表 + thunk（ROADMAP 主线 G 验收点） | 多继承布局是 `dynamic_cast`（learn/11）与名字修饰（learn/13）的共同地基 |
| 2026-09-24 | 多继承成员/布局**五条连环 bug** 修复（B11~B15） | ① vptr 压字段是真 miscompile ② 继承来的成员方法查不到 ③~⑤ 同一根因：把**显示名当成了索引** |
| 2026-09-24 | 修好 `tests/mi/test_mi_04_error.cpp`（原以为是菱形继承被拒，实际真因是次表假符号） | **又一次印证**：logdiff 基线会把**失败**也固化成契约——修完必须回头看 rc，不能只看"零差异" |
| 2026-10-08 | vtable 槽里的符号名与定义点不同源（B20 缺陷 a/b/c） | 带参虚函数链接失败 + 次基类假符号 + 非虚 `f(int)` 认领虚 `f()` 的槽 |
| 2026-10-09 | 成员方法身份收口为「名字 + 形参类型」（B22） | "这是哪个函数"总共写在四个地方（符号名 / 槽位 / 成员调用 / 构造函数选定）——同一判据不许写两份 |
