# 54. vtable 槽挂声明句柄（[class.virtual]，Stage 3 批 5）

> 定位：把 vtable 槽里的**符号串**换成**声明句柄** —— 从此"槽指向哪个函数"
> 由一份声明说了算，符号名只有一个来源（唯一定名点）。
> 关联：docs/specs/vtable-slot-handle.spec.md ＋ docs/designs/vtable-slot-handle.design.md
> 前置：docs/learn/13（多继承布局）/ 17（vtable 符号）/ 53（嵌套类）

## §0 定位与能力清单

| 能力 | 状态 | 落点 |
|---|---|---|
| vtable 槽存**声明句柄**（`shared_ptr<FunctionDecl>`） | ✅ 本批 | `src/ast/type.h` `VTableEntry` |
| 槽的符号由句柄给出（唯一定名点 = `registerFunction`） | ✅ 本批 | `include/ast.h` `vtableEntrySymbol()` |
| 三个填槽落点只换句柄、**不算符号** | ✅ 本批 | `processClassDecl`（主表覆写 / 次表覆写 / 新槽） |
| 派生类覆写 = 换句柄（基类自己的表不动） | ✅ 本批 | 同上 |
| 析构槽按**声明名**判定（`~X`），不靠符号子串 | ✅ 本批 | `src/codegen/codegen.cpp` `isDtorSlot()` |
| CodeGen 四个消费点统一走同一个取符号入口 | ✅ 本批 | `emitVTable` / 次表 / 虚调用命中 / 析构判定 |
| 布局 dump 与 Sema 打印统一走取符号入口 | ✅ 本批 | `dumpHierarchy` / `dumpLayout` / `main.cpp` |
| 嵌套类作虚函数重载的形参类型 | ✅ 本批 | 修前链接期 undefined reference（BUGS.md B45） |
| 名字含 `dtor` 的普通虚函数不被误认成析构 | ✅ 本批 | 修前静默少调一次析构（BUGS.md B46） |
| Itanium ABI 真 mangling | ⬜ 边界 | 沿用教学版规则（`docs/learn/18` 口径不变） |
| 虚函数模板 / `virtual` 成员模板 | ⬜ 边界 | `docs/learn/34` §3.5 已登记 |

## §1 理论背景

### 1.1 标准怎么说

- **[class.virtual]**：虚函数在**虚表**里占一个槽；派生类的实现**占同一个槽**
  （不是新增槽）⇒ 槽位身份 = 方法身份（方法名 + 形参表，见 [class.virtual]/2）。
- **[class.ctor]/[class.dtor]**：析构函数也可以是虚的，占一个槽；
  槽里放的是"哪个类的析构"由**声明**决定，不该由名字的形状猜。
- 标准不规定 vtable 的**内存布局**（那是 ABI 的事），但规定了**行为**：
  虚调用必须调到"最派生类的那份实现"。

### 1.2 编译原理怎么说

vtable 是一个**静态表**（数据），每项是一个函数指针。表格里放什么，是
**中间表示的设计选择**：

| 方案 | 表格里放什么 | 后果 |
|---|---|---|
| 放**名字** | `"A_f_1"` | 名字得在别处再解析一次 ⇒ 两处可能不一致 |
| 放**地址** | `&A::f` | 编译期就绑定 ⇒ 无法做"同一方法多形态" |
| ★ 放**声明句柄** | `CXXMethodDecl*` | 名字在**发射时**由唯一的 mangler 产出 ⇒ 不一致无从产生 |

★ clang 选第三种。本项目此前选的是第一种（教学简化：先拼字符串再发射），
代价就是 B20 / B22 / B45 这条"同一个坑的三次现形"。

### 1.3 clang 的三个机制（逐条对照）

1. **槽里放声明**：`clang/include/clang/AST/VTableBuilder.h:VTableComponent`
   —— `getFunctionDecl()` 返回 `const CXXMethodDecl *`，
   `getGlobalDecl()` 把它转成 `GlobalDecl` 供发射用。
   ★ 槽里**没有**第二份符号名。
2. **发射时才取符号**：`clang/lib/CodeGen/CGVTables.cpp:CodeGenVTables::addVTableComponent`
   —— 对 `CK_FunctionPointer` 取 `GlobalDecl GD = component.getGlobalDecl(...)`
   再转成 IR 常量 ⇒ 名字由 `MangleContext` 唯一产出。
3. **析构槽按声明种类判定**：`VTableComponent::isDestructorKind()`
   —— 看**声明的种类**（`CXXDestructorDecl`），不是看名字里有没有 "dtor"。

## §2 设计决策

### 2.1 为什么槽里不能存符号串

旧形状（Pass 1 填槽时算一遍、Pass 2 定名时算一遍）：

```
Pass 1（processClassDecl）          Pass 2（registerFunction）
  selfSymbol = memberMethodSymbolName(...)
      ↑ 此刻形参类型【未解析】               ↑ 此刻形参类型【已解析】
  entry.mangledName = selfSymbol     decl->mangledName = memberMethodSymbolName(...)
```

★ 两处**共用同一个函数**，仍然会漂 —— 因为它们看到的世界不同：

| 时刻 | 形参类型节点 | toString() |
|---|---|---|
| Pass 1 | 未解析的 `Class("Inner")` | `Inner` |
| Pass 2 | 已解析的 `ClassType("Outer::Inner")` | `Outer::Inner` |

⇒ 同一方法两个符号 ⇒ `.quad Outer_f_1_Inner` 指向没人定义的符号 ⇒
**链接期 undefined reference**（B45 的现场）。

★ 结论：**"共用函数"只治了"写错"，治不了"算两次"**。
治本只有一条路：**只算一次，别处只引用** —— 这就是句柄化。

### 2.2 为什么槽的身份与符号要分成两件事

| | 身份 | 符号 |
|---|---|---|
| 内容 | 裸名 + 形参类型链 | 声明上的 `mangledName` |
| 何时定 | Pass 1（覆写检测要当场比对） | Pass 2（要解析形参类型） |
| 谁产出 | `processClassDecl` | `registerFunction`（唯一定名点） |

★ 若身份也依赖已解析的类型，就得整体推迟到 Pass 2 ⇒ 覆写检测（Pass 1 的活）
就做不了。分开之后，两件事各在自己的时刻定，互不依赖。

### 2.3 为什么取符号的桥接函数放在 ast.h 而不是 type.h

`type.h` 只能**前置声明** `FunctionDecl`（ast.h 定义它并 include type.h ⇒
依赖单向）。`shared_ptr<T>` 允许 `T` 不完整 ⇒ 槽里挂句柄没问题；
但 `entry.method->mangledName` 要**完整类型** ⇒ 桥接函数放 ast.h。

```cpp
// include/ast.h（此处 FunctionDecl 完整）
inline std::string vtableEntrySymbol(const VTableEntry& entry) {
    return entry.method ? entry.method->mangledName : std::string();
}
```

### 2.4 为什么析构槽改看声明名

旧判据：`entry.mangledName.find("dtor") != npos`（**子串匹配**）。
新判据：`entry.method->name.starts_with("~")`。

★ 子串匹配的假阳性很具体：虚拟方法 `my_dtor_helper` 的符号是
`W_my_dtor_helper` ⇒ 含 "dtor" ⇒ 被当成析构 ⇒ 作用域结束时的析构调用
打在**槽 0**（helper）而不是槽 1（析构）⇒ **真正的析构根本没跑**
（静默少调一次，用户看不见）。★ 这正是 clang 用 `isDestructorKind()`
（看声明种类）而不是"看名字"的原因。

## §3 clang 源码对照表

| clang 位置（符号） | 本实现位置 | 简化了什么 |
|---|---|---|
| `clang/include/clang/AST/VTableBuilder.h:VTableComponent`（`getFunctionDecl` / `getGlobalDecl`） | `src/ast/type.h:VTableEntry::method` ＋ `include/ast.h:vtableEntrySymbol()` | clang 存 `CXXMethodDecl*` 裸指针（AST 生命周期由 Context 管）；本实现存 `shared_ptr`（教学工程的自保） |
| `clang/lib/CodeGen/CGVTables.cpp:CodeGenVTables::addVTableComponent` | `src/codegen/codegen.cpp:CodeGen::emitVTable` | clang 转成 IR 常量（`llvm::Constant*`）；本实现直接写 `.quad <符号>` |
| `clang/include/clang/AST/VTableBuilder.h:VTableComponent::isDestructorKind` | `src/codegen/codegen.cpp:isDtorSlot` | clang 看声明种类；本实现看声明名（`~X`） |
| `clang/lib/CodeGen/CGVTables.cpp:CodeGenVTables::maybeEmitThunk` | `src/codegen/codegen.cpp:emitThunk` | clang 生成 Itanium 规范的 thunk；本项目用"类名_方法名_thunk偏移"的简化标签 |
| `clang/lib/CodeGen/ItaniumCXXABI.cpp:ItaniumCXXABI::canSpeculativelyEmitVTable` | `src/sema/semantic_analyzer.cpp:injectVTableAndRTTI` | clang 的"投机发射"含 typeinfo 精确类型；本项目只有 `_ZTI<类名>` |
| `clang/lib/AST/ItaniumMangle.cpp:CXXNameMangler::mangleName` | `src/sema/semantic_analyzer.cpp:memberMethodSymbolName` | clang 是可逆的 ABI 编码；本项目是"名字 + 形参类型链"这条土规则 |

## §4 关键过程（ASCII 图）

### 4.1 槽的一生

```
   类的每个方法 (Pass 1)
        │
        ├─ 槽位身份 = 裸名 + 形参类型链        ← 不含符号
        │
        ├─ 覆写？ ──是──▶ 命中主表槽 → entry.method = 本类的方法
        │              命中次表槽 → entry.method = 本类的方法 (+thunkAdjust)
        │
        └─ 否（新虚函数）→ 新槽 entry.method = 本类的方法
                                   │
                                   ▼
                        VTableEntry{ method, baseFunctionName, signature }
                                   │
        Pass 2：registerFunction ──▶ decl->mangledName（唯一定名点）
                                   │
                                   ▼
        CodeGen：.quad vtableEntrySymbol(entry)   ← 从句柄取，不再算
```

### 4.2 覆写前后的两张表

```
   class A { virtual int f() {...} };      class B : A { int f() {...} };

   A 的表：  [槽0] ──▶ A::f   (符号 A_f)      ← A 自己的实现
   B 的表：  [槽0] ──▶ B::f   (符号 B_f)      ← 继承来的槽，换了句柄
```

★ B 的表是"继承 + 换句柄"；**A 的表不动**（否则 A 的对象会调到 B 的实现）。

### 4.3 修前 / 修后的产物对比

```
修前（Pass 1 算槽、Pass 2 算定义）：
    .globl Outer_f_1_OuterCCInner      ← 定义点
    .quad Outer_f_1_Inner              ← 槽（未解析的类型）⇒ 链接期 undefined reference

修后（槽只有句柄）：
    .globl Outer_f_1_OuterCCInner      ← 定义点（唯一定名点）
    .quad Outer_f_1_OuterCCInner       ← 槽（从句柄取）⇒ 逐字相同
```

## §5 可复现实验命令

```bash
# ① 编译（0 警告）
cmake --build build-linux -j6

# ② 单元测试（396 例；本批新增 6 例 VTableHandle.*）
ctest --test-dir build-linux

# ③ 集成日志基线（165 份逐字节比对；本批新增 1 份）
./logdiff.sh diff

# ④ 突变验证（5 条判据逐条实跑变红）
./tools/verify-vtable-handle-mutations.sh

# ⑤ clang 对拍：本批的两组探针
cat > /tmp/p1.cpp <<'EOF'
struct Outer {
    struct Inner { int v; };
    virtual int f(Inner i) { }
    virtual int f(int i) { }
};
int main() { Outer o; return 0; }
EOF
./minicc /tmp/p1.cpp -S -o /tmp/p1.s && grep -E "globl|quad" /tmp/p1.s   # 槽与定义逐字相同
clang++-18 -std=c++20 /tmp/p1.cpp -o /tmp/p1c && /tmp/p1c                   # rc=0
```

★ 修前跑第 ⑤ 条：minicc 在链接期报 `undefined reference to 'Outer_f_1_Inner'`。

## §6 真实版多做了什么、为什么

| 真 clang | 本项目 | 为什么这么取舍 |
|---|---|---|
| 槽里存 `CXXMethodDecl*`（AST 层） | 槽里存 `shared_ptr<FunctionDecl>` | 本项目没有 clang 的 `ASTContext` 管 AST 生命周期 ⇒ 用 `shared_ptr` 自保；**形状一致**（都是声明句柄） |
| 名字由 `ItaniumMangleContext` 唯一产出 | 名字由 `memberMethodSymbolName` 唯一产出（`registerFunction` 调用点唯一） | 教学版不做真 mangling（`docs/learn/18` 口径） |
| 析构槽看声明种类（`CXXDestructorDecl`） | 析构槽看声明名（`~X`） | 本项目的 `FunctionDecl` 没有"种类"字段（析构靠名字前缀区分）⇒ 用名字，但**判据落在声明上**而不是符号串上 |
| vtable 组件含 `CK_VCallOffset` / `CK_VBaseOffset` / thunk 组 | 只有函数指针项 | 多继承的 vcall/vbase 偏移是 Itanium ABI 的进阶部分，本项目只做到"次表 + thunkAdjust"（`docs/learn/17` 口径） |
| 槽的身份由 `VTableLayoutBuilder` 按**方法签名**比对 | 槽的身份 = 裸名 + 形参类型链 | clang 比的是 `MethodDecl` 的完整签名（类型 + const + noexcept）；本项目用类型链近似（教学简化，B22 的已知边界） |

★ **本批真正多做的**：不是"多支持了一个特性"，而是**把一笔"两处各算一遍"的债还掉了**。
收益有两条：① B45 的链接期错配**从根上消失**（不是加个补丁堵住某个case）；
② 今后任何"新增一个虚函数形态"都不必再担心"槽里的名字和定义点的名字对不上"。

## §7 边界与已知取舍

| # | 边界 | 表现 | 登记在 |
|---|---|---|---|
| 1 | 槽的符号来自 `decl->mangledName` ⇒ **Pass 2 必须跑过** | 理论上槽的符号为空 ⇒ 汇编器报错（响亮拒收） | — |
| 2 | 槽位身份仍用**形参类型链**近似签名 | `f(int)` 与 `f(S)` 的区分靠类型链，不是真签名 | B22 的已知边界 |
| 3 | 析构槽按**名字前缀**判定 | 没有 clang 的"析构声明种类" | 本批的取舍（见 §6） |
| 4 | 次表槽的 `thunkAdjust` 仍是简化标签（`类名_方法名_thunk偏移`） | 非 Itanium 真 thunk | `docs/learn/17` |
| 5 | 句柄里不含**模板实例**的方法（实例化路径另有克隆） | 模板类的虚表槽指向实例方法 | 实例化步骤 5.x（`docs/learn/34` §1.5） |

## §8 下一批

- **7-5 实例化步骤 5.8：克隆嵌套类**（类模板里的嵌套类）——
  批 4 的边界 1；做法与本批同源：让实例类持有蓝图的**声明**，而不是重算一遍。
- **7-6 虚表槽的 `CK_VCallOffset` / `CK_VBaseOffset`**（多继承的虚基类）—— 教学版暂不需要。
- **主线 I 行 7-7**：把 `docs/learn/13` 里"槽"的描述与本批对齐（槽 = 句柄）。

## 演进记录

| 日期 | 批次 | 做了什么 | 留下什么 |
|---|---|---|---|
| 2026-09-09 | 主线 G | 虚表布局（槽序 / thunk / RTTI）落地 | 槽里存的是**符号串**（两处各算一遍的老形状） |
| 2026-10-2x | B20 | vtable 三落点与定义点共用 `memberMethodSymbolName` | 仍是"算两遍"，只是函数换了个地方 |
| 2026-10-2x | B22 | 槽位身份加形参类型链（`f(int)` vs `f(S)`） | 类型链吃 `toString()` ⇒ 未解析/已解析两种形态 |
| 2026-11-02 | **批 5** | **槽挂声明句柄**（本批） | 符号唯一定名点；B45 / B46 修掉 |
