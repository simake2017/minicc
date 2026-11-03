# 技术设计：vtable 槽挂声明句柄（Stage 3 批 5）

状态：已签字（2026-11-02）
规格：docs/specs/vtable-slot-handle.spec.md
关联：docs/learn/54-vtable-slot-handle.md

- 读者：改 vtable 槽位、虚函数分派、符号定名的人
- 审法：槽里放的是**声明句柄**还是**符号串**？答案必须是「句柄」
- 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 首版：Stage 3 批 5 的技术设计 |

## 1. spec → 设计映射表

| spec 条 | 落点 | 形状 |
|---|---|---|
| 3.1 槽里放声明句柄 | `src/ast/type.h` `VTableEntry` | `std::shared_ptr<FunctionDecl> method;`（前置声明） |
| 3.2 符号只有一个来源 | `include/ast.h` `vtableEntrySymbol()` | `entry.method->mangledName` |
| 3.3 覆写 = 换句柄 | `src/sema/semantic_analyzer.cpp` `processClassDecl` 三个落点 | `entry.method = method;` |
| 3.4 析构槽按声明名判定 | `src/codegen/codegen.cpp` `isDtorSlot()` | `name.starts_with("~")` |
| 4-3 槽 → 声明 → 符号同源 | `tests/unit/test_vtable_handle.cpp` | 期望值取**声明自己的** mangledName |
| 4-6 突变验证 | `tools/verify-vtable-handle-mutations.sh` | 5 条判据 |

## 2. 整体流程图

```
        Pass 1（processClassDecl，登记类）              Pass 2（registerFunction，定名）
        ────────────────────────────────              ────────────────────────────────
        类的每个方法                                    类的每个方法
             │                                              │
             ├─ 覆写检测（槽位身份：裸名 + 形参链）           └─ decl->mangledName = 唯一定名
             │      ├─ 命中主表槽 → entry.method = method        （memberMethodSymbolName）
             │      ├─ 命中次表槽 → entry.method = method              │
             │      └─ 新虚函数   → entry.method = method              │
             ▼                                                      ▼
        VTableEntry{ method, baseFunctionName, signature, ... }
             │                              （槽里【没有】符号串了）
             ▼
        CodeGen::emitVTable
             └─ .quad vtableEntrySymbol(entry)   ← 从句柄取，不是另算
```

★ 时序要点：**Pass 1 填的是句柄，Pass 2 定的名** ⇒ 填槽时符号尚未可知，
这正是"槽里不能存符号串"的结构性理由（存了就只能再算一遍）。

## 3. 子流程 + 业务说明

### 3.1 槽的身份与符号，分成两件事

| | 是什么 | 谁产出 | 何时 |
|---|---|---|---|
| **身份** | 这个槽是哪个虚函数的（覆写判据用） | `baseFunctionName` + `signature` | Pass 1 |
| **符号** | 这个槽该填哪个函数地址 | `method->mangledName` | Pass 2 |

★ 身份在 Pass 1 定（因为覆写检测要当场比对），符号在 Pass 2 定（因为要解析形参类型）。
**两者被分开，是本批能成立的前提** —— 若身份也依赖已解析的类型，就得整体推迟到 Pass 2。

### 3.2 依赖方向：type.h ⇄ ast.h 是单向的

```
   ast.h ──include──▶ type.h          （ast.h 需要 Type / VTableEntry 的完整定义）
      │
      └─ 前置声明 struct FunctionDecl;  （type.h 需要 FunctionDecl 的句柄，但只要指针）
```

★ `std::shared_ptr<T>` 允许 `T` 不完整 ⇒ type.h 只前置声明即可。
★ 取符号的桥接函数 `vtableEntrySymbol()` 放在 **ast.h**：那里 `FunctionDecl` 才完整，
`entry.method->mangledName` 才可编译。

### 3.3 三个填槽落点（唯一的写入点）

| 落点 | 触发条件 | 动作 |
|---|---|---|
| 主表覆写 | 槽位身份命中主表某槽 | `entry.method = method`（索引不变） |
| 次表覆写 | 槽位身份命中次表某槽 | `entry.method = method` + `thunkAdjust = -base.offset` |
| 新槽 | 本类新引入的虚函数 | `entry.method = method`（追加槽） |

★ 旧实现在这三处各算一遍符号（`memberMethodSymbolName`）⇒ 三处都可能与定义点漂。
新实现三处都只写句柄 ⇒ **漂的余地从"三处"降到"零"**。

### 3.4 CodeGen 的四个消费点

| 消费点 | 旧写法 | 新写法 |
|---|---|---|
| 主表 `.quad` | `asmSymbol(entry.mangledName)` | `asmSymbol(vtableEntrySymbol(entry))` |
| 次表 `.quad` / thunk 目标 | 同上 | 同上 |
| 虚调用命中判据 | `entry.mangledName == resolvedCalleeSymbol` | `vtableEntrySymbol(entry) == …` |
| 析构槽判定 | `entry.mangledName.find("dtor") != npos` | `isDtorSlot(entry)`（看声明名） |

★ 第四处的旧写法是**子串匹配**：符号 `W_my_dtor_helper` 含 "dtor" ⇒ 误判成析构 ⇒
作用域结束时的析构调用打在槽 0（helper）而不是槽 1（析构）⇒ 真析构没跑（B46）。

### 3.5 覆写后基类自己的表不动

派生类 `B : A` 覆写 `A::f`：

| 表 | 槽 0 的句柄 | 符号 |
|---|---|---|
| A 自己的表 | `A::f` | `A_f` |
| B 的表（继承来的槽） | `B::f` | `B_f` |

★ B 的表是**继承 + 换句柄**的结果；A 的表不受影响（否则 A 自己的对象会调到 B 的实现）。

## 4. 异常流转表

| 情形 | 现象 | 现在的行为 |
|---|---|---|
| 槽没有句柄（理论上不可能） | `vtableEntrySymbol` 返回空串 | 汇编里 `.quad ` 空操作数 ⇒ 汇编器报错（响亮拒收，不静默） |
| 句柄有但符号为空（Pass 2 没跑） | 同上 | 同上（Stage 3 批 5 之前，槽里存的是 Pass 1 算的串，永远非空 ⇒ 掩盖了这个前提） |
| 析构槽判据改成子串匹配 | 误判 | 单测 `VTableHandle.VirtualDtorCallTargetsTheDtorSlot` 变红 |
| 覆写不换句柄 | 动态分派失效 | 单测 `VTableHandle.OverrideSwapsHandleNotAString` 变红 |

## 5. 突变验证清单

| # | 突变 | 应变红的用例 |
|---|---|---|
| m1 | 覆写时主表不换句柄 | `OverrideSwapsHandleNotAString` |
| m2 | 新槽不挂句柄 | `EverySlotHasDeclarationHandle`（其余同源用例一并红） |
| m3 | 槽的符号不从句柄取 | `SlotSymbolIsTheDeclarationSymbol` |
| m4 | 析构槽判据退回子串匹配 | `VirtualDtorCallTargetsTheDtorSlot` |
| m5 | 槽的符号由槽自己拼（丢形参类型链） | `SlotSymbolIsTheDeclarationSymbol` + `QuadOperandComesFromTheHandle` |

★ **m5 第一次没被抓住** —— 因为那条断言的期望值取自 `vtableEntrySymbol(entry)`，
而它正是被突变的那一个 ⇒ "自己验自己"。改成"期望值取**声明自己的** mangledName"
（与 accessor 无关）后抓住。★ 教训：**断言的期望值必须来自与被测物【无关】的来源**，
否则它测的是"自己跟自己一致"（同族教训见 docs/learn/22 的 B22 条）。

## 6. 有意日志漂移

**零漂移**（见 spec §6）：既有 164 份集成日志逐字节不变。

## 7. 裁量区清单

| # | 裁量点 | 选了哪边 | 理由 |
|---|---|---|---|
| 1 | 槽里放句柄 vs 放符号串 + 句柄 | 只放句柄 | 放两份就还得维护两份的一致性（治本不治标） |
| 2 | `vtableEntrySymbol` 放 type.h 还是 ast.h | ast.h | type.h 里 FunctionDecl 不完整，取不到成员 |
| 3 | 析构判据：声明名 vs 符号子串 | 声明名 | 子串匹配有假阳性（`my_dtor_helper`） |
| 4 | 覆写时基类槽是否也换 | 不换 | 基类的表是基类自己的实现 |
| 5 | 符号命名规则动不动 | 不动 | 动了会让 164 份日志全漂；本批只改"谁在什么时候算" |
