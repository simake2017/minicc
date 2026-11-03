# 规格：vtable 槽挂声明句柄（Stage 3 批 5）

状态：已签字（2026-11-02）
签字：2026-11-02（用户拍板"可以，按照你的建议去做"——ROADMAP 主线 I 行 7-4）
来源：docs/ROADMAP.md **7-4**（vtable 槽 → `FuncDeclPtr` 句柄）＋ docs/BUGS.md **B45** / **B46**

> 阶段：**Stage 3 批 5**（承批 4 的嵌套类 —— docs/learn/53）
> 关联缺陷：BUGS.md **B45**（vtable 槽的符号在两处各算一遍 ⇒ 链接期错配）/ **B46**（析构槽靠符号子串判定 ⇒ 误判）
> 关联学习产物：docs/learn/54-vtable-slot-handle.md
> 关联上游：docs/learn/13（多继承布局）、docs/learn/17（vtable 符号）、docs/BUGS.md B20 / B22

- 读者：改 vtable 槽位、虚函数分派、符号定名的人
- 审法：槽里放的是**声明句柄**还是**符号串**？答案必须是「句柄」——
  槽里若还有第二份符号，就还留着"两处各算一遍"的老毛病
- 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 首版：Stage 3 批 5 的需求 / 范围 / 判据 / 验收 / 观测点 |

## 1. 需求与动机

ROADMAP 行 7-4 写着一笔"主线 G 的第一笔债"：

> vtable 槽 → `FuncDeclPtr` 句柄。B20/B22 的根因（槽里放的是**字符串符号**，
> 规则在多处各写一遍）⇒ 虚表槽存**声明句柄**而非符号串 ⇒ 符号名由唯一的
> 定名函数产出；CodeGen 只发句柄给出的符号。

★ B20 与 B22 已经把"两处各写一遍"收拢成"两处共用**同一个函数**"
（`memberMethodSymbolName`），但**共用函数不等于消除了重复**：两处调用发生在
**不同时刻**，看到的状态不同。探针实测（clang++-18 -std=c++20 对拍）：

```cpp
struct Outer {
    struct Inner { int v; };
    virtual int f(Inner i) { }   // 形参类型 = 嵌套类
    virtual int f(int i) { }     // 同名同个数 ⇒ 符号要挂形参类型链
};
```

minicc 修前的产物（`.logbaseline` 与探针实测一致）：

```asm
    .globl Outer_f_1_OuterCCInner      # 定义点（Pass 2，形参类型已解析）
    .quad Outer_f_1_Inner              # vtable 槽（Pass 1，形参类型未解析）
```

⇒ `[LINK ERROR] undefined reference to 'Outer_f_1_Inner'`（链接期才炸）。

★ 机理：同名同个数的重载要靠**形参类型链**区分（B22），而类型链吃的是
`param->type->toString()`：

| 时刻 | 形参类型节点的形态 | toString() |
|---|---|---|
| Pass 1（`processClassDecl` 填槽） | 未解析的 `Class("Inner")` | `Inner` |
| Pass 2（`registerFunction` 定名） | 已解析的 `ClassType("Outer::Inner")` | `Outer::Inner` |

⇒ 同一方法算出两个符号。**这不是"写错了"，是"算了两次"的必然后果。**

★ 附带修（B46）：析构槽的判定原先是"符号串里有没有 `dtor` 子串" ⇒ 叫
`my_dtor_helper` 的普通虚函数会被误认成析构 ⇒ 作用域结束时的析构调用打在
错的槽上 ⇒ 真正的析构根本没跑（静默少调一次）。

## 2. 范围

### 2.1 In Scope

- `VTableEntry` 挂**声明句柄**（`std::shared_ptr<FunctionDecl>`），删掉符号串字段；
- 槽的符号由句柄给出（唯一定名点仍是 `registerFunction`）；
- 三个填槽落点（新槽 / 主表覆写 / 次表覆写）只换**句柄**，不算符号；
- 派生类覆写 = 换句柄（基类自己的表不动）；
- 析构槽的判定改为"声明名以 `~` 开头"；
- CodeGen 的四个消费点（主表 / 次表 / 虚调用命中 / 析构判定）统一走同一个取符号入口。

### 2.2 Out of Scope

- **不动符号命名规则本身**（`<类名>_<方法名>` + `_N` 后缀 + 形参类型链）——
  本批只改"谁在什么时候算"，不改"算成什么"，以保证既有产物逐字节不变；
- 不做 Itanium ABI 的真 mangling（`docs/learn/18` 口径不变）；
- 不动 vtable 的**布局**（槽序、thunk、offset-to-top、RTTI）—— 那是 `docs/learn/17` 的地盘；
- 不做 `virtual` 成员模板（`docs/learn/34` §3.5 的边界）；
- 不引入 clang 的 `VTableComponent`/`VTableBuilder` 那套完整机制（本批只对齐"槽里放句柄"这一条）。

## 3. 规格

### 3.1 槽里放声明句柄，不放符号串

`VTableEntry` 的形状：

```cpp
struct VTableEntry {
    std::shared_ptr<FunctionDecl> method;   // 槽指向的声明（唯一定名点在这里）
    uint32_t    index   = 0;
    bool        isOverridden = false;
    std::string baseFunctionName;            // 槽位身份：裸名（析构记作 "dtor"）
    std::string signature;                   // 槽位身份：形参类型链
    int         thunkAdjust = 0;
};
```

★ `type.h` 只**前置声明** `FunctionDecl`（ast.h 定义它且 include type.h ⇒ 依赖单向）；
取符号的桥接函数 `vtableEntrySymbol(entry)` 放在 **ast.h**（那里 FunctionDecl 才是完整类型）。

### 3.2 符号只有一个来源

- 唯一定名点：`SemanticAnalyzer::registerFunction`（Pass 2）写 `decl->mangledName`；
- 槽的符号：`vtableEntrySymbol(entry)` = `entry.method->mangledName`；
- ★ **任何地方都不许再算一遍符号**：填槽点只写句柄，CodeGen 只读句柄。

### 3.3 覆写 = 换句柄

| 落点 | 旧做法 | 新做法 |
|---|---|---|
| 主表槽被覆写 | `entry.mangledName = selfSymbol` | `entry.method = method` |
| 次表槽被覆写 | `entry.mangledName = selfSymbol` | `entry.method = method` |
| 本类新虚函数 | `entry.mangledName = selfSymbol` | `entry.method = method` |

★ 槽位**身份**（`baseFunctionName` + `signature`）不变 —— 覆写判据按
[class.virtual]/2 仍是"同签名"，与符号无关。

### 3.4 析构槽按声明名判定

`isDtorSlot(entry)` = `entry.method->name.starts_with("~")`。
★ 不用"符号串里找 `dtor` 子串"：子串匹配会把 `my_dtor_helper` 误判成析构。

## 4. 验收

| # | 验收项 | 判据 |
|---|---|---|
| 1 | 探针程序可链接可运行 | `struct Outer { struct Inner{int v;}; virtual int f(Inner); virtual int f(int); };` rc=0 且运行 rc=0（修前：**链接期 undefined reference**） |
| 2 | 既有集成用例逐字节零漂移 | `./logdiff.sh diff` ⇒ 164 份旧日志全等 + 新增 1 份 |
| 3 | 槽 → 声明 → 符号三者同源 | 单测 `VTableHandle.QuadOperandComesFromTheHandle`：汇编里的 `.quad` 操作数 == **声明自己的** `mangledName` |
| 4 | 覆写换的是句柄 | 单测 `VTableHandle.OverrideSwapsHandleNotAString`：派生类的槽挂派生类的声明，基类的表不动 |
| 5 | 析构槽不误判 | 单测 `VTableHandle.VirtualDtorCallTargetsTheDtorSlot`：析构的虚调用打在**析构槽**（vtable[1]）上，不是 `my_dtor_helper` 那个槽（vtable[0]） |
| 6 | 突变验证 | 5 条判据逐条实跑变红（`tools/verify-vtable-handle-mutations.sh`） |
| 7 | 与 clang 对拍 | 集成用例 `tests/lang/test_basics_34_vtable_slot_handle.cpp` 两编译器均 rc=0、运行 rc=0 |

## 5. 与 Stage 3 的关系

Stage 3 的主线是"名字的可见性与作用域由 Sema 裁决，Parser 不持第二份事实"。
本批是同一条主线的**虚函数侧**：

| | 名字事实的持有者 | 本批之前的坏形状 | 本批之后 |
|---|---|---|---|
| 普通名字 | Sema 符号表 | Parser 自建登记表（已退场） | 单点 |
| **虚函数槽** | Sema 的 vtable 布局 | **槽里存一份符号串 + 定义点存一份** | **槽里存声明句柄** |

⇒ 一句话：**槽不再持有"名字的第二份副本"，只持有"指向名字来源的句柄"。**

## 6. 有意日志漂移（红线 4：显式列出）

★ **零漂移**：本批不产生任何日志漂移。

- 164 份既有集成日志**逐字节不变**（`./logdiff.sh diff` 实测）；
- 槽的符号在旧路径下打印的是 Pass 1 算的串，新路径打印的是 Pass 2 算的串 ——
  两者对既有全部用例**本来就相等**（否则旧路径早就链接失败了），
  故打印结果不变；
- 新增 1 份集成日志（`lang__test_basics_34_vtable_slot_handle.log`）与 6 个单测用例。

## 7. 观测点

| 观测点 | 在哪看 | 看什么 |
|---|---|---|
| 槽的符号 | 编译日志 `vtable[N]: <符号>` | 槽的符号与同一次编译里 `[mangled: …]` 的**定义点**同名 |
| 定义点符号 | 编译日志 `[register] … [mangled: X]` | 唯一定名点的产物 |
| 汇编同源 | `minicc x.cpp -S -o x.s` | `.quad <符号>` 与 `.globl <符号>` 逐字相同 |
| 虚调用落在哪个槽 | 汇编注释 `VIRTUAL CALL: C::m (vtable[N])` | N 就是语义期定好的槽号 |
| 析构槽判定 | 汇编注释 `VIRTUAL CALL: W::dtor (vtable[N])` | N 指向**析构**那个槽（不是名字含 dtor 的那个） |

## 附：实施记录

- 落点：`src/ast/type.h`（`VTableEntry`）、`include/ast.h`（`vtableEntrySymbol`）、
  `src/sema/semantic_analyzer.cpp`（三个填槽落点 + `vtableSlotName` 助手）、
  `src/codegen/codegen.cpp`（四个消费点 + `isDtorSlot`）、`src/driver/main.cpp`（布局 dump）；
- 测试：`tests/unit/test_vtable_handle.cpp`（6 例）＋ `tests/lang/test_basics_34_vtable_slot_handle.cpp`；
- 突变验证：`tools/verify-vtable-handle-mutations.sh`（5 条判据）。
