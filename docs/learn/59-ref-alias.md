# 59. 引用的别名语义（[dcl.ref]/1 + [dcl.init.ref]，BUGS.md B6）

> 定位：让"引用"从**静默算错**（`int& r = a; r = 30;` 改的是 r 自己那份拷贝，
> a 全程不变）变成**与 clang 逐字对拍一致**。
> 一句话：**引用不是对象，是别名** —— 它的槽里放的是被绑定对象的**地址**，
> 读走间接加载、写走间接存储；引用形参则由调用点把实参的**地址**传过去。
> 关联：docs/learn/23（const 位置）、docs/learn/53（作用域即身份）、
> docs/BUGS.md B6（本体）/ B51（引用返回，边界）
> 前置：ROADMAP §0（当前优先级 = 执行序）

## §0 定位与能力清单

| 能力 | 状态 | 落点 |
|---|---|---|
| 局部引用变量写穿被绑定对象（`r = 30` 改的是 a） | ✅ 本批 | `visit(VarDeclStmt)` 路径①-ref + `visit(AssignStmt)` |
| 局部引用变量读的是被绑定对象的**当前值** | ✅ 本批 | `visit(VarExpr)` → `emitRefValue` |
| 引用形参写穿到调用方的对象（`void f(int& x)`） | ✅ 本批 | 序言登记别名槽 + 调用点传地址 |
| 引用形参再传给另一个引用形参 | ✅ 本批 | 调用点 `emitAddressOf` 走别名路径 |
| 引用绑到成员 / 全局 / 另一个引用 | ✅ 本批 | `emitAddressOf` 三条取址分支 |
| 类类型的引用（`S& r = s;`） | ✅ 本批 | 类值即地址（`emitRefValue` 的类分支） |
| 虚方法 / 成员方法 / 模板函数的引用形参 | ✅ 本批 | 判据与普通函数同源，只换传参通道 |
| const 局部引用（`const int& r = a;`）仍是别名 | ✅ 本批 | 局部侧按"初始化式可寻址"判 |
| **同名变量在不同块里各有一种形态** | ✅ 本批 | 引用表改成**作用域链**（`m_refScopes`） |
| const 引用形参（`void f(const int& x)`） | ⬜ **边界（B58）** | 实参可能是右值 ⇒ 本实现按值传 |
| 右值引用形参（`void f(T&& x)`） | ⬜ 边界 | 同上；右值没有地址可取 |
| 按引用返回（`S& f(){return g;}`） | ⬜ **边界（B51）** | 与 B50 同根因（全局取址），未修 |
| 引用成员（`struct S { int& r; };`） | ⬜ 边界 | 本实现没有引用成员的概念 |
| 真 Itanium ABI 的引用传参分类 | ⬜ 边界 | 教学版口径：可写左值引用才算别名 |

## 1 理论背景

### 1.1 标准怎么说

- **[dcl.ref]/1**："A reference is an alias for an object and is not itself an object."
  引用**是别名**，它自己**不是对象** ⇒ 它不占存储、不参与析构、没有自己的值类别。
  ⇒ 代码生成上，引用变量只需要一个**指针大小的槽**（存被绑定对象的地址）。
- **[dcl.init.ref]**：引用用**初始化式**绑定。被绑定对象有两种来源：
  · 实参/初始化式是**左值** ⇒ 直接绑定到那个对象（别名）；
  · 实参/初始化式是**右值** ⇒ 先**物化一个临时量**，再绑定到那个临时
    （[dcl.init.ref]/5.4.2 的 `SK_BindReferenceToTemporary`）。
- **[expr.prim.id]**：引用变量**表达式**的类型是**被引用对象的类型**
  （引用在表达式里被剥掉）⇒ `int y = r;` 里 y 是 int，不是 int&。

### 1.2 编译原理怎么说

- **别名 = 间接寻址**：把"名字 → 存储位置"的映射多走一层。
  标量变量：名字 → 槽 → **值**；引用变量：名字 → 槽 → **地址** → 值。
  于是读引用是"两次寻址"，写引用是"到地址里写"。
- **左值 / 可寻址（addressable）**：只有能算出地址的表达式才能当别名的绑定对象。
  变量、成员、解引用、下标都可以；字面量、临时对象、函数返回值不行
  （⇒ 编译器必须先物化临时量，才有地址可取）。
- **形参传递的对称性**：调用方按**地址**传，被调方就必须按**地址**解释那个槽 ——
  两侧必须**同口径**，否则被调方会把一个普通整数当地址解引用。
  ⇒ "这个形参是不是引用"这件事，两端必须读**同一个**类型、同一条判据。

### 1.3 与本项目既有约定的关系

- 本实现早有一条约定：**类对象在表达式里的值 = 它的地址**
  （`emitClassAddress`；局部类对象 `visit(VarExpr)` 也照此发射 `leaq`）。
  ⇒ 引用到类对象时，引用槽里那个地址**就是**表达式的值，**不该再解引用**
  （解引用会把 vptr 当值取出来）—— 这就是 `emitRefValue` 里那个类分支。
- 本实现还有一条约定：**可寻址性由节点形态判定**（`isAddressableExpr`）。
  ⇒ 引用绑定的"有没有地址"直接复用它，不另造一套可寻址判据。

## 2 病灶与改法

### 2.1 病灶（一）：引用变量被当成"值的副本"

```cpp
int main() { int a = 5; int& r = a; r = 30; return a; }   // clang=30，minicc=5
```

旧实现里 `int& r = a;` 与 `int r = a;` 生成的代码**一模一样**：
声明时把 a 的**值**拷进 r 的槽，之后 `r = 30` 写回 r 自己那份 ⇒ a 全程不变。
⇒ 编译全绿、运行算错 ⇒ **静默算错**（BUGS.md B6 记的高危）。

### 2.2 病灶（二）：★ BUGS.md 里"函数形参天生正确"这句话是**错的**

B6 原文写："现有用例里引用只出现在两处恰好都正确的位置 —— ① **函数形参**
（`void f(T& x)`，走调用方传地址，天生正确）"。**实测不是**：

```cpp
void bump(int& x) { x = x + 1; }
int main() { int a = 5; bump(a); return a; }   // clang=6，minicc=5
```

旧实现里调用点传的是**值**（`emitExpr(arg)` 把 a 的值装进 rdi），
被调方把形参槽当普通 int 用 ⇒ callee 的写回不到调用方 ⇒ **同款静默算错**。
★ 教训：**"文档里写着这条路是对的"不等于它真的对** —— 缺陷台账里的
"为何一直没被发现"那段，本身可能就是错的；本批是**实测推翻**的。

### 2.3 改法：判据单点（`isAliasBinding`）

**判据**：这个绑定是不是**别名**？—— 【**有没有被绑定对象的地址**】。

```cpp
static bool isAliasBinding(TypePtr type, bool operandHasAddress) {
    if (!type || !type->isReference()) return false;
    if (isWritableLValueRefType(type)) return true;   // 实参必是左值 ⇒ 必有地址
    return operandHasAddress;                         // 其余引用：看绑定对象有没有地址
}
```

两个调用点问的是**同一个**问题，但"地址从哪来"不同：

| 调用点 | `operandHasAddress` 传什么 | 依据 |
|---|---|---|
| 局部引用变量 | 初始化式**可寻址**吗（实情） | 本地就能答 |
| 引用形参（调用点 + 被调点） | 恒 `false` | 实参**可能是右值**（右值没有地址）⇒ 形参侧无从得知 |

★ 形参侧的"恒 false"不是偷懒，而是**语言的保证**：
**可写左值引用（`T&`）的实参在合法程序里必然是左值** ⇒ 必有地址 ⇒
调用点可以放心按地址传，被调点可以放心按别名解释 —— **两侧同源**。

⇒ 于是：`T&`（可写）走别名通道；`const T&` / `T&&` 按值传（**边界，见 §7**）。

### 2.4 ★ 改法之二：引用表必须是**作用域链**，不是一张平表

第一版实现把别名登记进一张平表 `m_refLocals`（名字 → 别名信息），
结果**自测用例当场崩了**（SIGSEGV）：

```cpp
{ int a = 1; int& r = a; int& s = r; s = 7; }   // 块①：s 是【引用】
{ S s; s.v = 1; }                                // 块②：s 是【类对象】
```

平表里 `m_refLocals["s"]` 存的是**块①那个引用**，块②里查名字先命中它 ⇒
`s.v = 1` 拿到的"对象地址"其实是块①里 `r` 持有的地址 ⇒ 写坏内存。

⇒ 修法：**引用表改成作用域链**（`m_refScopes`：层 0 = 函数作用域，
每进一层 `BlockStmt` 压一层、出块弹一层），查找由内向外扫。
★ 这正是 docs/learn/53 那条教训的第二次现形：**"作用域"不是一张表，是一条链** ——
把它当表实现，每加一种"形态"就要重做一遍遮蔽规则。

> ⚠ 边界（**未修**）：`m_localVars` / `m_classLocals` 仍是平表，
> 所以"标量 / 类对象"之间的同名遮蔽仍按"最后声明者胜"处理
> （这是既有行为，本批未动，也未引入回归）。

### 2.5 改法之三：调用点怎么知道"这个形参是不是引用"

调用点要知道被调函数的**形参类型**，才能决定传地址还是传值。
本实现的取法是**按 Sema 已定好的符号名查声明**（`m_funcBySymbol`）：

- 键 = `FunctionDecl::mangledName`（**Sema 的裁决结果**，CodeGen 不重算）；
- 值 = 那个 `FunctionDecl`（**形参类型就在它身上**）。

★ 与 docs/learn/54（vtable 槽挂声明句柄）同款思路：**别处只引用，别处不算** ——
CodeGen 只把"已定的身份"翻译成"形参是不是引用"，不自己重新裁决身份。

★ 兜底：查不到声明（被调函数没有定义 / 是 libc 内建）⇒ 一律按值传
⇒ 与被调点的解释一致（没有定义就没有被调点）⇒ 不会口径漂移。

## 3 clang 源码对照表

| clang 位置 | 本实现位置 | 简化了什么 |
|---|---|---|
| `clang/lib/CodeGen/CGExpr.cpp:3502` `EmitLoadOfReferenceLValue`（引用声明 ⇒ 取被绑定对象的地址） | `visit(VarDeclStmt)` 路径①-ref + `m_refScopes` | 本实现只在**槽**里放地址，不建 alloca（本来也没有 alloca） |
| `clang/lib/CodeGen/CGExpr.cpp:3620` `EmitDeclRefLValue` | `visit(VarExpr)` → `emitRefValue` | clang 对引用类型直接返回被绑定对象的 LValue；本实现拆成"取地址 + 间接加载"两条指令 |
| `clang/lib/CodeGen/CGExpr.cpp:700` `EmitReferenceBindingToExpr`（实参绑引用 ⇒ 取该左值的**指针**） | `emitAddressOf` + `emitArgument` | clang 交回一个 `llvm::Value*`；本实现交回 rax |
| `clang/lib/CodeGen/CGCall.cpp:5235` `EmitCallArg`（左值实参 ⇒ `args.add(EmitReferenceBindingToExpr(E), type)`） | `visit(CallExpr)` 四个调用点 | clang 的 `CallArgList` 统一管理；本实现按寄存器逐个弹 |
| `clang/lib/Sema/SemaInit.cpp:3954` `SK_BindReference` / `SK_BindReferenceToTemporary`（右值实参 ⇒ 物化临时） | `isAliasBinding(type, false)` 的 `false` 分支 | ★ 本实现**不物化临时**，const/右值引用形参改走按值通道（边界，见 §7） |
| `clang/lib/Sema/SemaLookup.cpp` 的作用域链查找（内层遮蔽外层） | `findRefLocal`（`m_refScopes` 由内向外扫） | clang 在 Sema 侧就定了可见性；本实现在 CodeGen 侧保留一份链 |

## 4 关键过程（ASCII 图）

### 4.1 改法前：引用 = 值的副本

```
int a = 5; int& r = a; r = 30;
  movq $5, %rax
  movq %rax, -16(%rbp)      # a 的槽 = 5
  movq -16(%rbp), %rax      # ★ 拷值：r 的槽 = 5（不是 &a）
  movq %rax, -24(%rbp)      # r 的槽 = 5
  ...
  movq $30, %rax
  movq %rax, -24(%rbp)      # ★ 写 r 自己的槽 ⇒ a 全程不变 ⇒ 静默算错
```

### 4.2 改法后：引用 = 别名

```
int a = 5; int& r = a; r = 30;
  movq $5, %rax
  movq %rax, -16(%rbp)      # a 的槽 = 5
  leaq -16(%rbp), %rax      # ★ 取 a 的地址
  movq %rax, -24(%rbp)      # r 的槽 = &a（别名）
  ...
  movq -24(%rbp), %rcx      # ★ 取引用持有的地址
  movq %rax, (%rcx)         # ★ 写被绑定对象 ⇒ a = 30 ✓
```

### 4.3 两侧同源（判据单点）

```
调用点                                  被调点
──────                                  ──────
读被调声明的形参类型                      读自己的形参类型
  ↓ isAliasBinding(t, false)               ↓ isAliasBinding(t, false)
  true  ⇒ 传地址（leaq 槽 / 取引用持有的地址）  true  ⇒ 槽按别名解释（间接读 / 间接写）
  false ⇒ 传值                              false ⇒ 槽按普通值解释
       ↑ 同一个 Type 对象（mangledName → FunctionDecl）⇒ 不会一边传地址一边当值用
```

### 4.4 ★ 作用域链：同名遮蔽

```
块① { int& s = a;  s = 1; }     作用域链：[函数层] → [块①层{s→别名}] → 弹层
块② { S s;         s.v = 2; }   作用域链：[函数层] → [块②层{s→类对象}]
                                   ↑ 查"s"：块②层没有 ⇒ 块①层已弹 ⇒ 函数层也没有
                                   ⇒ 名字解析到【类对象】✓（平表会串到块①的别名 ✗）
```

## 5 可复现实验命令

```bash
# ① 编译（0 警告）
JOBS=6 ./build.sh

# ② 单元测试（本批新增 7 例：RefAlias.*）
cmake --build build-linux -j6 --target unit_tests
./build-linux/unit_tests --gtest_filter='RefAlias.*'

# ③ 集成测试（新增 1 份：tests/lang/test_basics_38_ref_alias.cpp）
./minicc tests/lang/test_basics_38_ref_alias.cpp -o /tmp/tb38 && /tmp/tb38; echo "rc=$?"

# ④ clang 对拍（本批的检查点）
clang++-18 -std=c++20 tests/lang/test_basics_38_ref_alias.cpp -o /tmp/tb38c && /tmp/tb38c; echo "rc=$?"

# ⑤ 改法前的症状（引用被当值的副本）
cat > /tmp/b6.cpp <<'EOF'
int main() { int a = 5; int& r = a; r = 30; return a; }
EOF
./minicc /tmp/b6.cpp -o /tmp/b6 && /tmp/b6; echo "minicc rc=$?（改法后 = 30）"
clang++-18 -std=c++20 /tmp/b6.cpp -o /tmp/b6c && /tmp/b6c; echo "clang rc=$?"

# ⑥ 全量回归 + 日志基线
ctest --test-dir build-linux --output-on-failure
./logdiff.sh diff

# ⑦ 突变验证（6 条判据逐条实跑变红）
./tools/verify-ref-alias-mutations.sh
```

### 5.1 突变验证（6 条判据逐条实跑变红，2026-11-11）

| 突变 | 摘掉的判据 | 被谁抓红 |
|---|---|---|
| m1 | 局部引用变量绑定时**取地址** ⇒ 改成拷值 | `RefAlias.T1` / `T6`；集成侧 SIGSEGV |
| m2 | 引用形参**登记为别名** ⇒ 改成不登记 | `RefAlias.T4`；集成侧 rc=3 |
| m3 | 调用点**按地址传**引用形参 ⇒ 改成按值传 | `RefAlias.T4`；集成侧 SIGSEGV |
| m4 | 引用赋值**写被绑定对象** ⇒ 改成写引用自己的槽 | `RefAlias.T3` / `T7`；集成侧 rc=1 |
| m5 | 作用域链**出块弹层** ⇒ 改成不弹（平表语义） | `RefAlias.T7`；集成侧 SIGSEGV |
| m6 | const 引用形参**按值传** ⇒ 改成按地址传 | `RefAlias.T5`；集成侧编译期报错 |

★ 每条突变在**集成侧**同样被抓（不止单测）—— 这是本项目的惯例：
单测抓"性质"，集成抓"症状"。

## 6 真实版多做了什么、为什么

| 真实 C++ | 本实现 | 为什么 |
|---|---|---|
| 引用变量是别名（槽里放地址） | ✅ 一样 | 这是 [dcl.ref]/1 的定义 |
| const 引用形参仍是别名 | ❌ 按值传 | 实参可能是右值 ⇒ 要传地址必须先**物化临时量**（调用方帧里要开一块临时槽 + 帧估算要跟着改）。本批不做 ⇒ 登记为边界 B58 |
| 右值引用形参是别名 | ❌ 按值传 | 同上；且右值引用在 minicc 里只用于模板推导 |
| 引用绑右值时物化临时量 | ⚠ 退化为按值（槽里放值本身） | 对 `const T& r = 5;` 而言，临时量的修改无人观测得到 ⇒ 可观察行为一致 |
| 引用形参的 ABI（Itanium §3.2.4）按引用类型分类 | ⚠ 只分"可写左值引用 / 其它" | 教学版口径：**两侧同口径**比"完全对齐 ABI"更要紧（口径漂移会段错误） |
| 引用返回 `S& f()` | ⬜ 未做 | 与 B50（全局取址）同根因，另开一批 |

## 7 边界与已知取舍

1. **const 引用形参 / 右值引用形参按值传**（新登记 **B58**）：
   `void f(const int& x)` 的实参在合法程序里**可能是右值**（`f(5)`），
   而右值没有地址 ⇒ 要按地址传就得在调用方物化临时量（帧要开临时槽）。
   ⇒ 本实现按值传。**可观察差异**：被调方若在调用期间让被引用对象发生变化
   （例如 callee 调了另一个函数改了那个对象），读到的值会不同 ——
   极罕见，但确实存在。
   ★ 局部 `const int& r = a;` **不受影响**（本地知道初始化式可寻址 ⇒ 仍是别名）。
2. **引用返回未做**（B51，与 B50 同根因）。
3. **`m_localVars` / `m_classLocals` 仍是平表**：同名遮蔽在"标量 vs 类对象"之间
   仍按"最后声明者胜"处理（既有行为，本批未动，也未引入回归）。
4. **引用成员（`struct S { int& r; };`）**未支持。
5. 引用变量的槽**占 8 字节**（clang 那里引用变量在 IR 里也是指针大小 ✓）。

## 8 下一批

- ROADMAP §0.2 主线 K 六项（别名模板特化的非法程序拒收 / 偏特化模式的值位 /
  `template<auto V>` 偏特化 / 模板模板形参的形参包·默认实参·嵌套 /
  类外定义特化成员 / 函数模板显式 NTTP 实参）。
- ★ 本批遗留：**B51 引用返回**（与 B50 全局类对象一起做，两个根因要一起改：
  存储尺寸 + 全局取址）。

## 9 演进记录

| 日期 | 变化 |
|---|---|
| 2026-11-11 | 首版：局部引用变量 + 引用形参的别名语义；判据 `isAliasBinding`；引用表改作用域链；单测 7 例 + 集成 1 份 + 突变 6 条 |
