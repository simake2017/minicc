# 57. 按值返回类对象 + 拷贝省略（[dcl.init]/17.6.1 + [class.copy.elision]/8，ROADMAP §0.1 批 0-1）

> 定位：让"按值返回类对象"从**静默算错**（返回值被丢弃，对象退化成默认构造）
> 变成**与 clang 逐字对拍一致**。
> 一句话：**类对象没有"标量值"表示** —— 按值返回它不能用 rax 装值，
> 只能给一块缓冲（sret），让被调者把对象拷进去；而拷贝省略让那块缓冲
> **就是目标变量自己的槽**，于是"返回值落进变量"一步完成。
> 关联：docs/learn/42（数组）、docs/learn/13（vtable）、docs/BUGS.md B48/B49/B50
> 前置：ROADMAP §0.1 批 0-1（"按值返回 + 拷贝省略"）

## §0 定位与能力清单

| 能力 | 状态 | 落点 |
|---|---|---|
| 自由函数按值返回类对象（`S x = f();`） | ✅ 本批 | `visit(CallExpr)` sret 路径 + `visit(VarDeclStmt)` 拷贝省略 |
| **拷贝省略**：返回缓冲就是变量槽（不产生第二个对象） | ✅ 本批 | `m_sretTargetSlot`（单次取用即清零） |
| 成员方法按值返回类对象（this 让到 rsi） | ✅ 本批 | `visit(CallExpr)` 成员分支 + `emitFunction` 形参右移 |
| **虚方法**按值返回类对象（多态派发 + sret 共存） | ✅ 本批 | `emitVirtualCall` 的 sret 形态 |
| 类模板实例按值返回（`Box<int> b = f();`） | ✅ 本批 | 判据只看"返回类型是不是类"，与模板无关 |
| 多态类按值返回（vptr 随对象表示一起拷走） | ✅ 本批 | 返回缓冲拷的是**整个对象表示**（`rep movsb`） |
| 返回"另一个函数返回的对象"（两层缓冲） | ✅ 本批 | 内层缓冲落帧临时区（`countSretBytes` 已预留） |
| 返回引用（`S& f();`）**不**走 sret | ✅ 本批 | 判据只剥 const、不剥引用（`bareClassType`） |
| 成员赋值的对象地址活过右值求值 | ✅ 本批 | B48：`pushq %rcx` / `popq %rcx` |
| **全局类对象**的字段访问（`g.v = 11`） | ⬜ **边界（B50，未修）** | 全局只按 8 字节分配存储 + 类值的地址约定未在全局路径生效 ⇒ 段错误 |
| 按引用返回类对象（`S& f(){return g;}`） | ⬜ **边界（B50，未修）** | 同上根因 |
| 拷贝构造 / 移动构造的用户定义版本 | ⬜ 边界 | 本实现没有拷贝构造概念；拷贝一律"逐字节复制" |
| `std::move` / 右值引用全套 | ⬜ 边界 | ROADMAP §0.4 明写"暂不开新战线" |
| 真 Itanium sret 编码（`TYPE_INFO`/`MEMORY` 分类） | ⬜ 边界 | 教学版口径：凡是类就按 sret 走（见 §7） |

## 1 理论背景

### 1.1 标准怎么说

- **[dcl.init]/17.6.1**：`T x = f();` 里 f 的返回值是 **prvalue**；
  它用来初始化 x。若 prvalue 的类型与 x 相同，**不要求**有可访问的拷贝构造
  —— 这次初始化就是**直接构造** x。
- **[class.copy.elision]/8**（"copy elision"）：允许（并鼓励）省略拷贝，
  即使省略会导致 [class.copy.ctor] 的选择有歧义。⇒ clang 默认就省略：
  `S x = f();` 里的临时对象**就是** x，不生成第二个对象。
- **[dcl.fct]/12**（返回类型）：函数的返回类型不得是数组、不得是函数、
  不得是（cv 限定的）void ⇒ 类类型返回是合法的，而且**按值**返回。

### 1.2 编译原理怎么说

- **返回值的存储**：标量返回值装 rax；**类对象没有标量表示**
  （首 8 字节可能是 vptr、可能是字段，不是"值"）⇒ 只能给一块内存。
- **sret（struct return）**：调用者分配缓冲，把它的地址作为**隐藏的第 0 参数**
  传给被调者；被调者把结果写进去；返回值 rax = 缓冲地址
  （Itanium C++ ABI §3.2.3 "Passing and Returning Aggregates"）。
- **寄存器位移**：隐藏参数占据 rdi ⇒ 显式实参整体右移一位（rsi 起）；
  成员函数的 this 排在显式实参之前 ⇒ 再右移一位（this 走 rsi、实参从 rdx 起）。
- **拷贝省略的存储含义**：既然 prvalue 直接materialize成 x，
  那块"返回缓冲"**就不必另找** —— 让它**等于 x 的槽**即可。
  ⇒ 本实现据此把 `S x = f();` 编译成"调用直接写进 x"，中间零拷贝。

### 1.3 与本项目既有约定的关系

本实现早有一条约定：**类对象在表达式里的值 = 它的地址**
（`emitClassAddress`；局部类对象 `visit(VarExpr)` 也照此发射 `leaq`）。
⇒ 按值返回类对象只是这条约定的**另一面**：既然"值 = 地址"，
返回时就不能靠 rax 传值，必须传一块缓冲的地址 —— **两处约定互为因果**。

## 2 病灶与改法

### 2.1 病灶一：初始化式被丢弃（★ 静默算错）

```cpp
struct S { int v; };
S f() { S s; s.v = 7; return s; }
int main() { S x = f(); return x.v; }   // clang: 7    修前 minicc: 0
```

Sema 的变量声明处理只认三种初始化形态：构造调用升级 / 平凡拷贝 / 直接初始化。
"**调用表达式返回类对象**"这一形态**没被认出来** ⇒ 落到"无初始化的类声明"
分支 ⇒ 初始化式被丢弃，对象按默认构造生成 ⇒ `x.v` 恒 0。
★ 症状最坏的地方在于**它不报错**：编译全绿、运行给出别的数。

### 2.2 病灶二：即便认出了形态，也没有"返回缓冲"这回事

认出来之后要真跑起来，还需要三件事：

1. **定义点**：序言把隐藏的第 0 参数（rdi）spill 到栈槽；
   `return <类对象>` 把对象表示**逐字节拷进那块缓冲**，rax 回填缓冲地址。
2. **调用点**：准备一块缓冲（拷贝省略时 = 目标变量的槽，否则 = 帧临时区），
   把它的地址装进 rdi；显式实参整体右移；成员方法的 this 让到 rsi；
   虚调用同款。
3. **帧要够大**：那块缓冲在帧临时区里，帧大小必须把它算进去
   （否则它落在 rsp 之下 ⇒ 被 `pushq` 暂存与 `callq` 返回地址踩掉）。

### 2.3 改法：判据单点 + 三处同源

★ **判据单点**（本批唯一的新判据）：**"这次调用按值返回类"只看返回类型是不是类**。
它在**三个地方**被读，必须**同源**：

| 落点 | 读什么 | 干什么 |
|---|---|---|
| `src/sema/semantic_analyzer.cpp`（变量声明处理） | 调用表达式的 `resolvedType` | 落 `initIsSretCall` 标志（**唯一来源**） |
| `src/codegen/codegen.cpp` `visit(CallExpr)` | 同一个 `resolvedType` | 备缓冲、装 rdi、实参右移 |
| `src/codegen/codegen.cpp` `emitFunction` | 函数的 `returnType` | spill 隐藏指针、形参与 this 右移 |

★ 为什么必须同源：调用点与定义点**读的是同一个 `Type` 对象**
⇒ 一侧判"是类"、另一侧也必判"是类" ⇒ 不会失配。
⚠ 两处各判一次必漂 —— 这是 B10 → B20 → B22 → B45 同一个坑的**第五次**现形。

★ **剥 const 但不剥引用**：判据写成 `bareClassType()`（只剥顶层 const）。
`const S f();` 仍是按值返回（const 只是限定）；`S& f();` **不是**
（返回引用不需要缓冲，误判会把引用语义改写成"拷贝一份"）。

### 2.4 拷贝省略：缓冲就是变量槽

`visit(VarDeclStmt)` 的类对象分支里，`initIsSretCall` 为真时：

```
缓冲 = 变量的槽（m_currentStackOffset 刚分配的那个）
⇒ 不清零、不装 vptr、不调构造函数 —— 返回值本身就是那个完整对象
   （含它自己的 vptr，随对象表示一起被拷贝过来）
```

⚠ **不清零是有意的**：返回值马上就把整个对象写进来，
先清零再覆盖纯属多余（但也不出错；这里是为了让"返回值即对象"更直白）。

★ **取用即清零**：`m_sretTargetSlot` 只给**最外层那一次**调用用 ——
取走就置回哨兵。否则 `S x = f(g());` 里 g 的返回值会写进 x 的槽（串味）。

### 2.5 连带修：成员赋值的对象地址活不过右值求值（B48）

做这个特性时必然要写"拿到返回的对象再改它的字段"，于是撞上：

```cpp
S s; s.v = 3; s.v = s.v + 4;    // clang: 7    修前 minicc: SIGSEGV
```

★ 根因在**成员赋值**这条老路径（与 sret 无关，但被本批的用例逼出来）：
`visit(AssignStmt)` 的成员分支先算对象地址装进 `%rcx`，**再求右值** ——
而右值求值会踩 `%rcx`（二元表达式把右操作数挪进 `%rcx`；函数调用按约定可踩
caller-saved 寄存器）⇒ 地址被冲掉 ⇒ `movl %eax, 0(%rcx)` 写到了一个
**不是该对象**的地址（rcx 恰是常量 4 ⇒ 段错误；恰是别的合法地址 ⇒ 静默踩内存）。

★ 修法：对象地址先 `pushq %rcx`，右值求完再 `popq %rcx` 回来。
⚠ 这是**寄存器存活期**问题，与"右值求值两次"那个老简化**不是一回事**
（那是求值次数，这是存活期）—— 两者都在同一条语句上，容易混。
★ **保护是无条件的**（右值是常量也保护）：判据若写成"右值含调用才保护"，
就等于用**节点种类**去猜"会不会踩 `%rcx`" —— 那是新的一处半份判据
（本批教训的第 N 次同款）。单测第三例专门钉住"常量右值也保护"。
★ **漂移范围**：这处保护是**汇编行**的插入（每条成员赋值 +2 行），
连带 `.text` 尺寸与 `main @` 地址位移 ⇒ **43 份既有集成日志漂移**，
但**语义日志一行未动、rc 与运行输出逐字节不变**（漂移全部落在汇编正文里）。

## 3 clang 源码对照表

| clang 源码（clang-18） | 本实现位置 | 简化了什么 |
|---|---|---|
| `clang/lib/CodeGen/CGCall.cpp:CodeGenFunction::EmitReturn` | `src/codegen/codegen.cpp` `visit(ReturnStmt)` | clang 按 `ABIArgInfo::isIndirect()` 决定"返回值放 rax"还是"写进 sret 缓冲"；本实现只认"返回类型是不是类" |
| `clang/lib/Target/X86/X86ABIInfo.cpp:X86_64ABIInfo::classifyReturnType` | `src/codegen/codegen.cpp` `bareClassType` / `returnsClassByValue` | clang 按 C++ 记录类型 + Triviality 分类（还要看是不是 trivially-copyable）；本实现一律按 sret |
| `clang/lib/CodeGen/CGCall.cpp:CodeGenFunction::EmitCallArg` | `src/codegen/codegen.cpp` `visit(CallExpr)` 的寄存器装载 | clang 做完整的 ABI 分类（含间接传参、内存传参）；本实现只处理"隐藏缓冲占 rdi ⇒ 实参右移一位" |
| `clang/lib/Sema/SemaInit.cpp:Sema::BuildCXXConstructExpr`（拷贝省略） | `src/sema/semantic_analyzer.cpp` 变量声明处理 | clang 在 AST 阶段就把 prvalue 直接建成 x 的构造；本实现落一个 `initIsSretCall` 标志，CodeGen 照办 |
| `clang/lib/CodeGen/CGExpr.cpp:CodeGenFunction::EmitMemberExpr` | `src/codegen/codegen.cpp` `visit(MemberExpr)` / `visit(AssignStmt)` 成员分支 | clang 用寄存器分配保证地址存活；本实现靠 push/pop 一对周转 |

★ 一句话对照：**clang 用"ABI 分类 + 寄存器分配"两套机制解决的问题，
本实现用"判据单点 + 手工寄存器周转"两处约定解决** ——
可观测行为一致（退出码逐字对拍），内部结构不可比（这是教学取舍，不是缺陷）。

## 4 关键过程（ASCII 图）

### 4.1 `S x = f();` 的完整时序（拷贝省略路径）

```
  编译期                            运行期
  ──────                            ──────
  Sema:  S x = f();
         └─ f() 的 resolvedType 是类 S
            ⇒ initIsSretCall = true        （判据单点的唯一落点）

  CodeGen(main):
    x 的槽 = -16(%rbp)                （m_currentStackOffset 分配）
    m_sretTargetSlot = -16            （★ 缓冲 = 变量槽 ⇒ 拷贝省略）
    ↓ emitExpr(f())
    visit(CallExpr):
      sretBuf = m_sretTargetSlot = -16      （取用即清零）
      leaq -16(%rbp), %rdi           rdi = 返回缓冲（隐藏第 0 参数）
      callq f
      leaq -16(%rbp), %rax           表达式的值 = 缓冲地址
    ↓ 回到 f
    emitFunction(f):
      movq %rdi, -8(%rbp)            spill 隐藏返回缓冲指针
      ... 构造局部 s、s.v = 7 ...
      return s:
        leaq -24(%rbp), %rax         源对象地址
        movq %rax, %rsi
        movq -8(%rbp), %rdi          目标 = 调用者的缓冲
        movq $8, %rcx
        rep movsb                    ★ 逐字节拷进缓冲
        movq -8(%rbp), %rax          rax = 缓冲地址（sret 约定）
        [析构局部 s]                  ★ 拷贝在前、析构在后
    ↓ 回到 main
    x.v == 7                          ✔ 与 clang 一致
```

### 4.2 寄存器位移（sret ⇒ 显式实参右移一位）

```
  普通调用（无 sret）              sret 调用
  ─────────────────               ─────────────────
  rdi = arg0                       rdi = 返回缓冲（隐藏）
  rsi = arg1                       rsi = arg0
  rdx = arg2                       rdx = arg1
  ...                              ...
  r9  = arg5                       r9  = arg4   （上限 5 个显式实参）

  成员方法再右移一位：
  rdi = this                       rdi = 返回缓冲
  rsi = arg0                       rsi = this
  rdx = arg1                       rdx = arg0
```

### 4.3 ★ 本批踩到的坑：哨兵不能用"偏移 >= 0"

```
  rbp 偏移【全是负数】（-8 / -16 / -24 …）
  ⇒ "槽位 >= 0 才算有" 这个判据【恒假】
  ⇒ 真实槽位被当成"没指定" ⇒ 缓冲另分配一块 ⇒ 返回值没进变量 ⇒ 静默算错
  ⇒ 修法：哨兵用 kNoSretTarget = INT_MIN（"没有"与"某个负偏移"必须能区分）
```

## 5 可复现实验命令

```bash
# ① 编译（0 警告）
cmake --build build-linux -j6

# ② 单元测试（本批新增 10 例：SretReturn.* 7 + MemberAssignClobber.* 3）
./build-linux/unit_tests --gtest_filter='SretReturn.*:MemberAssignClobber.*'

# ③ 集成测试（新增 1 份：tests/lang/test_basics_37_return_class_by_value.cpp）
ctest --test-dir build-linux

# ④ clang 对拍（本批的九个检查点）
cat > /tmp/sret.cpp <<'EOF'
struct S { int v; };
S f() { S s; s.v = 7; return s; }
int main() { S x = f(); return x.v; }
EOF
clang++-18 -std=c++20 -o /tmp/sret.clang /tmp/sret.cpp && /tmp/sret.clang; echo "clang rc=$?"   # 7
./minicc /tmp/sret.cpp -o /tmp/sret.mini && /tmp/sret.mini;       echo "minicc rc=$?"     # 7（修前是 0）

# ⑤ 突变验证（6 条判据逐条实跑变红）
./tools/verify-sret-mutations.sh
```

### 5.1 突变验证（6 条判据逐条实跑，2026-11-03）

脚本：`tools/verify-sret-mutations.sh`（★ 还原用 cp 备份，不碰 git 工作区）

| 突变 | 摘掉什么 | 变红的用例 |
|---|---|---|
| m1 | 定义点不 spill 隐藏返回缓冲指针 | `SretReturn.T1` / `T5` + 集成侧 SIGSEGV |
| m2 | 返回路径不把对象拷进缓冲 | `SretReturn.T1` + 集成侧 rc=1 |
| m3 | 调用点不把缓冲装进 rdi | `SretReturn.T2` + 集成侧 SIGSEGV |
| m4 | 拷贝省略退回临时区 | `SretReturn.T3` / `T7` + 集成侧 rc=1 |
| m5 | sret 判据连引用一起剥 | `SretReturn.T6`（集成侧不敏感 —— 它没有引用返回用例） |
| m6 | 成员赋值不保护对象地址 | `MemberAssignClobber.T1` / `T2` / `T3` + 集成侧 SIGSEGV |

★ **每条突变都要"实跑变红"才算数** —— 判据写错方向（恒真）时，
突变脚本会当场暴露（learn/50 §判据③ 的教训）。

## 6 真实版多做了什么、为什么

| 真实编译器 | 本实现 | 为什么能简化 |
|---|---|---|
| ABI 分类：`classifyReturnType` 按 C++ 记录类型 + trivially-copyable 决定 sret 还是 rax | "返回类型是不是类" ⇒ 一律 sret | 教学项目不追求 ABI 正确性，只追求**语义正确 + 可观测** |
| 寄存器分配器保证跨调用的地址存活 | 手工 `pushq`/`popq` 周转 | 本实现只有 rax 一个结果寄存器，其余靠栈周转（既有约定） |
| AST 阶段就把 prvalue 建进 x 的构造（真拷贝省略） | 落一个 `initIsSretCall` 标志，CodeGen 照办 | Sema 不该碰存储细节；标志是**跨阶段的契约**，两边各判一次必漂 |
| 拷贝构造 / 移动构造的用户定义版本 | 一律"逐字节复制" | 本语言没有拷贝构造的概念（learn/08） |
| 真实 sret 还要处理 `MEMORY` 类（太大 ⇒ 走内存间接返回） | 不区分大小 | 类都很小，缓冲固定在调用者栈上 |

★ **"拷贝省略"在本实现里其实是"缓冲复用"** —— 语义效果与 clang 相同
（不产生第二个对象、不调用拷贝构造），实现路径不同（clang 在 AST 阶段合并，
本实现复用同一块存储）。

## 7 边界与已知取舍

1. **全局类对象的字段访问仍会段错误**（BUGS.md B50，未修）：
   `S g; g.v = 11;` ⇒ minicc 崩、clang rc=11。根因有两处：
   ① 全局变量发射只按 8 字节分配存储（`.quad 0`），类对象的真实尺寸没算；
   ② "类值 = 地址"的约定只在**局部**类对象路径（`m_classLocals`）生效，
   全局路径仍按标量加载首 8 字节 ⇒ 拿到的是字段值而不是地址 ⇒ 解引用即崩。
   ⇒ 已登记为 ROADMAP §0.1 的下一条（0-3），本批不动（两处根因要一起改，
   且会牵动全局存储布局）。
2. **按引用返回类对象**同样中招（根因同上）⇒ 也归 B50。
3. **凡是类就 sret**：真实 ABI 会按 trivially-copyable 与大小分级；
   本实现一律按 sret ⇒ 对"小类"正确，对"含虚表的大类"也正确（多态类的
   vptr 随对象表示一起拷走），但**不保证与 clang 的 ABI 兼容**（不追求）。
4. **实参上限**：sret 占用一个寄存器 ⇒ 显式实参上限从 6 降到 5
   （成员方法从 5 降到 4）。超过上限的实参**被静默丢弃**（既有行为，未改）。
5. **拷贝省略只认"变量初始化"这一种形态**：`S x = f();` ✔；
   `f(g())` 这种嵌套调用走临时区（多一次拷贝，但语义正确）。
6. **返回类型与声明类型不匹配时报的是自造文案**
   （`Type mismatch in 'x': declared 'S', got 'T'`），
   clang 是 `cannot initialize 'S' with an rvalue of type 'T'` ——
   属"诊断不对（clang 也拒，只是文案不同）"那一档，本批未对齐。

## 8 下一批

- **ROADMAP §0.1 批 0-3**：B50（全局类对象：存储尺寸 + 地址约定）。
- 之后按 §0.2 进主线 K 六项（别名模板特化的非法程序拒收 / 偏特化模式的值位 /
  `template<auto V>` 偏特化 / 模板模板形参的形参包·默认实参·嵌套 /
  类外定义特化成员 / 函数模板显式 NTTP 实参）。

## 9 演进记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-03 | v1 | 首版：按值返回 + 拷贝省略（sret 形态）+ 连带修 B48（成员赋值踩 %rcx）+ 登记 B50（全局类对象，未修） |
