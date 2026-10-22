# 43 — 全局/命名空间别名（注册即实例化）与表达式级构造调用

> 起点：两个提问 —— "inferVar 能支持 `class a = Box<int>;` 吗"（能，但见 §2.1），
> "inferVar 是不是解析不出 `x = Box<int>(7)` 这种"（**对，真缺口**）。
> 测试：`tests/decl/test_decl_03_global_alias.cpp`、
> `tests/lang/test_basics_11_ctor_expr.cpp`（+ 负例 `_12`）。
> BUGS.md：B29（别名）、B30（构造表达式 + 平凡拷贝/类赋值）。

## 1. 别名的声明点决议（B29）

### 1.1 理论背景：[dcl.typedef] —— 别名不是新类型

`using IntBox = Box<int>;` / `typedef Box<int> IntBox;` 的语义是：
给**既有类型**再登记一个名字。关键推论：

- **决议发生在声明点**，不是使用点。clang 里 TypedefNameDecl 持有的是
  已经决议完成的 QualType —— `Box<int>` 在 `using` 那一行就已经是
  一个 RecordType（实例），不是 TemplateSpecializationType 名字节点。
- **别名不产生新实例**：`IntBox` 与 `Box<int>` 是同一个类型的两个名字，
  `sizeof` 相等、互相赋值不需要任何转换。
- 命名空间成员的名字遵循统一的前缀化约定：`namespace N { using X = ...; }`
  注册的是 `N::X`，使用点 `N::X v;` 带限定名来查。

### 1.2 缺陷：两个"没做完"

修复前 `processTypeAliasDecl`：

```cpp
Symbol sym;
sym.name = decl->aliasName;                    // ① 裸名，不带命名空间前缀
sym.type = decl->underlyingType;               // ② 未决议的模板 id 名字节点
m_symbolTable.globalScope()->define(decl->aliasName, sym);
```

| 错法 | 症状 |
|---|---|
| ② 存未决议类型 | `IntBox b; b.v = 7;` ⇒ `No member 'v' in class 'Box'`（对着**蓝图**查字段，rc=1） |
| ① 裸名注册 | `N::IntBox b;` ⇒ `unknown type name 'N::IntBox'`（rc=1） |

两个都是**拒收合法程序**。此前没炸只是因为别名全测在**类内**
（learn/24 的三条查找路径都在类作用域），全局/命名空间形态零覆盖。

### 1.3 修法：注册即决议

```cpp
TypePtr resolved = resolveType(decl->underlyingType);   // 声明点实例化/解糖
std::string fullName = m_currentNamespace.empty()
    ? decl->aliasName : m_currentNamespace + "::" + decl->aliasName;
// sym.type = resolved; sym.name = fullName; …
```

`resolveType` 是既有机制（类模板分支实例化、别名模板分支解糖），
**一行新机制都没造** —— 缺的只是"在声明点调用它"。

```
using IntBox = Box<int>;
        │
        ▼  processTypeAliasDecl（声明点）
   resolveType(Box<int>) ──► [instantiate:class] Box_int
        │
        ▼  符号表登记
   IntBox ──► Class("Box_int")     ← 使用点拿到的就是成品
```

## 2. 表达式级构造调用（B30）

### 2.1 理论背景

- **[expr.type.conv]/1**：`Box<int>(7)` 是显式类型转换（函数式写法），
  语义 = 以 7 为实参构造一个 `Box_int` 临时对象，表达式结果
  是该对象（左值）。注意：`class a = Box<int>;`（用户最初问的写法）
  **不是**这个 —— 那是把类型当值用，clang 同样拒；真正的缺口是
  `Box<int>(7)` 带实参表的形态。
- **[dcl.type.class.deduct]（CTAD）**：`Box(9)` 由构造实参反推类模板形参。
  与函数模板推导是**同一套合一算法的反向使用**：模式来自构造函数形参表
  （隐式推导指引），实参是调用实参表。
- **[class.copy.elision]**：拷贝初始化 `Box<int> b = Box<int>(7);` 允许
  （且 clang 默认）省略临时对象、直接构造进目标槽。
- **[class.copy]/6 与 [class.copy.assign]**：用户没写拷贝构造/赋值时，
  编译器**合成**的语义就是逐字节复制对象表示。
- **[class.dtor]/2**：临时对象在**全表达式结束**时析构 —
  `b = Box<int>(9);` 的临时对象必须 memcpy 进 b **之后**才析构。

### 2.2 缺陷链：一条路不通，顺出三个伴随 bug

```
Parser:  CallExpr(callee=VarExpr("Box")+explicitTemplateArgs, args=[7])
            │  （语法层一直没问题）
            ▼
inferCall 四路全不接类模板构造：
  成员调用? 否   自由函数? 查无   函数模板? Box 不是函数模板   回退 inferType(VarExpr(Box))
            ▼
  Undefined variable 'Box'（rc=1）   ← 主缺口
```

继续探相邻场景，发现同一片区的三个伴随 bug：

| 场景 | 修前行为 | 性质 |
|---|---|---|
| `Box<int> b = a;`（同类左值） | CodeGen 掉"无初始化式类声明"分支，发 **0 参默认构造**调用 | 链接期 `undefined reference`（编译期全绿） |
| `b = a;` / `b = Box<int>(9);` | 走标量 AssignStmt，只写 8 字节、源地址错 | **rc=0 静默算错** |
| 模板体内 `Box<int> b(7);` | cloneStmt(VarDeclStmt) 漏搬 `ctorArgs` | 顶层正常、体内构造实参丢失 |

### 2.3 修法：判据单点 + 符号回填（同一形状的第四次出现）

**① inferCall 新增构造分支**（放在自由函数查找**之前**）：

```
模板名 Box（可能带 N:: 前缀回退）
   │  显式实参表非空 ──► resolveType 逐位实例化（NTTP 值位原样保留）
   │  实参表为空     ──► CTAD：deduceCtadCore（与语句级共用）
   ▼
pickBestByArgs 选定构造函数（B22 的择优原语）
   ▼
回填 isCtorCall / ctorClassType / callee=构造符号（mangling 有状态，
CodeGen 硬拼 `类名_方法名` 必错配 —— B10/B20/B22 的"符号回填"同一形状）
```

语句级与表达式级 CTAD 共用 `deduceCtadCore(t, argTypes, dbgName)`：
`deduceClassTemplateArgs`（`Box<int> b(9);` 那条路）与构造分支
只是实参表的来源不同，推导本体只有一份。

**② Sema 两个新判据**（visit(VarDeclStmt)，都放在 auto/类型检查
if/else **之外** —— auto 变量反推完类型后同样要走）：

- 构造调用升级（拷贝省略）：init 是 `isCtorCall` 的 Call ⇒
  类型一致性检查（`Box<int> b = Box<double>(7);` ⇒ Type mismatch，rc=1）
  ⇒ `ctorArgs/ctorSymbol` 回填、initializer 清空（[ctor] ★ 日志）；
- 平凡拷贝：同类左值（init 限 Var/Member）⇒ `isTrivialCopy`。

visit(AssignStmt) 回填 `isClassAssign/assignClassType`。

**③ CodeGen**：构造临时对象走临时对象区（allocTempSlot，与 new 同区，
countCtorTempBytes 数帧）；`isTrivialCopy` 与 `isClassAssign` 按布局
**内联 `rep movsb`** 复制 —— ★ 自研链接器只内置 malloc/free，
`callq memcpy` 没有定义可绑（[builtin] 阶段注册的只是原型）。

**④ 析构时机协作**：visit(CallExpr) 若无条件析构构造临时对象，
`b = Box<int>(9);` 会 memcpy **已析构**的源。`m_suppressTempDtor`
计数器：赋值 RHS 语境 +1 抑制，visit(AssignStmt) memcpy 后补发析构。

### 2.4 clang 源码对照表

| clang（~/cppproject/llvm-project） | 本实现 | 简化了什么 |
|---|---|---|
| `Sema::BuildTypedefNameExpr` / `Sema::getTypeName`（声明点决议底层类型） | `processTypeAliasDecl` 的 `resolveType` | 无两阶段查找/依赖型别名 |
| `Sema::DeduceClassTemplateFromInit`（CTAD 统一入口） | `deduceCtadCore`（语句级/表达式级共用） | 无聚合初始化推导、无推导指引的隐式生成全族 |
| `Sema::BuildCXXFunctionalCastExpr`（函数式转换表达式） | inferCall 构造分支 + isCtorCall 回填 | 无转换函数查找（无用户自定义转换） |
| `CodeGenFunction::EmitAggregateCopy`（聚合拷贝） | `rep movsb` 内联 | 无 copy ctor 调用语义（只认平凡拷贝） |
| `CodeGenFunction::EmitDestroy`（全表达式结束析构） | `m_suppressTempDtor` + 赋值后补发 | 无 EH clean-up 栈 |

## 3. 可复现实验

```bash
./build.sh && ./build-linux/minicc tests/lang/test_basics_11_ctor_expr.cpp -o /tmp/p11 && /tmp/p11; echo "run=$?"
# 期望 run=0（六个检查点全中）
./build-linux/minicc tests/lang/test_basics_12_ctor_expr_error.cpp -o /tmp/p12 2>&1 | grep "Type mismatch"
# 期望：Type mismatch in 'b': declared 'Box_int', got 'Box_double'
./build-linux/minicc tests/decl/test_decl_03_global_alias.cpp -o /tmp/pd3 && /tmp/pd3; echo "run=$?"
# 期望 run=0
```

Sema 侧关键日志（e2 探针 `auto b = Box<int>(7);`）：

```
[ctor-expr] ★ Box_int(1 args) ⇒ 选定构造函数符号 Box_int_Box_int_1    [临时对象构造]
[auto] ★ b : auto ⟹ Box_int   ← auto 被永久替换!
[ctor] ★ b : Box_int(1 个实参) ⇒ 升级直接初始化（拷贝省略），构造符号 Box_int_Box_int_1
```

## 4. 边界与未做

- 构造表达式做**函数实参** `f(Box<int>(7))`：形参是类类型按值传递时，
  调用约定（谁分配实参槽）还没定义 —— 未做。
- 用户自定义拷贝构造/拷贝赋值（`Box(const Box&)`）：判据只看"无用户
  拷贝构造"，写了就还不认（会掉进旧路径）—— 未做。
- `Box<int> b = f();`（函数返回同类对象）：平凡拷贝判据 init 限
  Var/Member，Call 形态未接 —— 未做（需函数返回类值的调用约定）。
- 别名：别名模板在**全局**位置的解糖已通（resolveType 复用），
  但别名指向**依赖类型**（`using X = T::type;` 在模板体内）未做。
- 多态类的平凡拷贝会连 vptr 一起复制（按对象表示逐字节）——
  这是正确的（合成拷贝构造就是 memcpy 语义），但没有自定义
  拷贝构造时"切片"语义未验证。
