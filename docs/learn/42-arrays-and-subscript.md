# 42 — 原生数组与下标寻址（主线 E 首批）

> 主线：ROADMAP 主线 E 首批。前接：39（常量折叠）、40（constexpr）、41（switch / do-while / 逗号）。
> 测试：`tests/lang/test_basics_10_array.cpp`。

## §0 定位与能力清单

数组是 C++ 里第一个"**类型本身携带大小**"的类型 —— `T[N]` 的类型身份里
含着 N。minicc 把它做成 `TypeKind::Array`，于是同一套类型系统能同时回答
"占几格"、"布局里怎么放"、"模板实参里怎么匹配"。下标运算则是**寻址**
而不是取值：结果必须是左值，否则 `v[i] = x` 写不进去。

| 能力 | 一句话 | 状态 |
|---|---|---|
| 数组类型 `TypeKind::Array` | 持 elementType / arraySize / arraySizeExpr | 已实现 |
| 元素与大小的精确比对（`equals`） | 逐项比对元素类型与数组大小 | 已实现 |
| 内存尺寸 `sizeof(T[N]) == sizeof(T) * N` | `sizeInBytes()` 从元素尺寸递归求得 | 已实现 |
| 声明符后缀 `[N]` 的提取 | 局部变量 / 全局变量 / 类成员字段三个声明点共用 | 已实现 |
| 数组长度须为正整数常量表达式 | 经 `ConstantFolder::evaluateIntegral` 求值 | 已实现 |
| 数组向指针的隐式退化 | 局部 `leaq -off(%rbp)`、全局 `leaq sym(%rip)`、成员 `leaq off(%rax)` | 已实现 |
| 原生下标寻址 | `leaq (%rcx, %rax, scale), %rcx`，scale = 1 / 2 / 4 / 8 | 已实现 |
| 下标结果是左值（可写） | `IndexExpr` 读 + `AssignStmt` 写两条路 | 已实现 |
| 局部数组的栈分配与零初始化 | 栈上预留连续空间并清零 | 已实现 |
| 全局数组的 `.data` 发射 | `.zero <size>` | 已实现 |
| 容器类下标糖（`v[i]` → `v.at(i)` / `v.set(i,x)`） | 与原生数组**双轨制**判据，按 `isRaw` 分流 | 已实现 |
| `T[N]` 作模板实参 / 偏特化模式 | 需在 `TypeKind::Array` 上做模板模式匹配 | 未做（属 ROADMAP 主线 E 整条） |
| `sizeof` 运算符 | 折叠表与语法层都还没有 | 未做 |
| 数组作函数形参（退化为指针） | 未做 | 未做 |
| 运行时边界检查 | 教学版不做（sanitizer 范畴） | 未做 |

---

## 1. 理论背景

### 1.1 数组类型模型（[dcl.array]）

C++ 中的数组类型由元素类型与元素数量共同决定：

- **类型复合**：`T[N]` 是以 `T` 为元素类型、长度为 `N` 的连续对象序列；
- **常量大小要求**：数组长度 `N` 必须是大于零的整型常量表达式（[expr.const]）；
- **内存尺寸**：`sizeof(T[N]) == sizeof(T) * N`；
- **多处声明点**：局部变量（栈上预留连续空间）、全局变量（`.data` 或 `.bss` 节连续字节）、类成员字段（结构体布局内连续偏移）。

### 1.2 数组向指针的隐式退化（[conv.arrayptr] Array-to-pointer Decay）

在 C/C++ 语义模型中，数组名极少作为"整个聚合体值"参与运算：

- **退化规则**：在大多数表达式语境下（除了 `sizeof`、取地址运算符 `&` 以及特定引用绑定），数组类型的左值或右值会自动转换为指向其首元素的指针（`T[N] → T*`）；
- **求值语义**：数组退化后产出的值不是内存内容，而是**内存地址**（如局部数组使用 `leaq -offset(%rbp), %rax`，全局数组使用 `leaq sym(%rip), %rax`，类成员数组使用 `leaq offset(%rax), %rax`）。

### 1.3 原生下标运算寻址（[expr.sub]）

标准定义 `E1[E2]` 等价于 `*((E1) + (E2))`：

- **寻址公式**：目标元素地址 = `base_address + index * sizeof(element)`；
- **值类别**：下标运算的结果是一个**左值**（lvalue），既可以作为右值读取，也可以作为赋值表达式的左侧目标进行写入；
- **标量宽度适配**：按元素宽度分派读写指令（1B `movb`/`movzbq`、2B `movw`/`movzwq`、4B `movl`、8B `movq`）。

### 1.4 对照 clang

| clang (llvm-project) | 本实现 | 简化与设计取舍 |
|---|---|---|
| `ASTContext::getConstantArrayType` | `Type::makeArray(elem, size)`（`TypeKind::Array`） | 统一表示已知大小的原生数组 |
| `Sema::ActOnArrayDeclarator` 计算常量折叠 | `SemanticAnalyzer::resolveType` 调用 `ConstantFolder::evaluateIntegral` | 复用常量折叠器求得正整数数组大小 |
| `DefaultFunctionArrayLvalueConversion` 发射 ArrayToPointerDecay 节点 | CodeGen 在 `VarExpr`、`MemberExpr` 遇数组类型统一发射 `leaq` 取首地址 | 零 AST 冗余节点，代码生成器直接兑现退化语义 |
| `CGBuilder.CreateGEP`（GetElementPtr 指令） | `leaq (%rcx, %rax, scale), %rcx`（scale=1,2,4,8） | 直接利用 x86-64 SIB（Scale-Index-Base）寻址模式 |

## 2. 架构与设计决策

### 2.1 语法解析：声明符后缀提取

在 C++ 文法中，数组声明符 `[N]` 出现在标识符**之后**（如 `int a[5];`），而不是类型前缀：

```
                ┌────────────────────────┐
                │ parseType()            │ ── baseType = int
                └───────────┬────────────┘
                            │
                            ▼
                ┌────────────────────────┐
                │ expect(Identifier)     │ ── 消费变量名 'a'
                └───────────┬────────────┘
                            │
                            ▼
                ┌────────────────────────┐
                │ parseArrayDeclarator   │ ── 检测到 '['，解析表达式，闭合 ']'
                │ Suffix(baseType)       │ ── 包装为 Type::makeArray(base, expr)
                └────────────────────────┘
```

该后缀解析统一挂接到三个声明入口：
1. `parseVarDeclStmt`：局部变量声明 `int a[5];`；
2. `parseGlobalVarDecl`：全局变量声明 `int g_arr[3];`；
3. `parseClassDecl`：类成员字段声明 `struct Box { int data[4]; };`。

### 2.2 下标运算与容器类下标糖的解耦

minicc 原生包含类似 STL 容器的 `v[i]` 下标语法糖（自动脱糖为 `v.at(i)` 与 `v.set(i, val)`）。原生数组接入后形成**双轨制**判据：

```
                           IndexExpr 下标运算
                                   │
                    ┌──────────────┴──────────────┐
                    ▼                             ▼
        原生数组或指针 (isRaw)              类对象 (Container)
     (isArray() || isPointer())                    │
                    │                    脱糖为方法调用:
         计算首地址 + 下标加权偏移       读: callq Class_at
         按元素宽度做 movl/movq 读写     写: callq Class_set
```

### 2.3 数组退化关键：类成员与隐式 this

在类成员方法内访问数组成员 `data[i]` 时，`data` 表现为裸标识符 `VarExpr`：

```
      普通标量字段读取:          数组字段退化读取:
      movq -8(%rbp), %rax       movq -8(%rbp), %rax
      movl 0(%rax), %eax        leaq 0(%rax), %rax
      (取值: 将内容加载到 rax)   (取地址: 将 data 首地址加载到 rax)
```

若将数组字段误用 `movl` 读出，则读出的是数组前 4 个字节的内容（通常为 0），随后将 0 作为基址进行下标寻址必将触发段错误（SIGSEGV）。因此在 `VarExpr` 与 `MemberExpr` 处理字段时，必须严格区分类/数组与标量类型。

## 3. 可复现实验

### 3.1 编译与执行原生数组测试

```bash
# 1. 运行 minicc 编译生成独立可执行文件
./build-linux/minicc tests/lang/test_basics_10_array.cpp -o /tmp/arr_test

# 2. 运行生成的文件并检查返回值（0 代表全项通过）
/tmp/arr_test
echo "Exit code: $?"
```

### 3.2 观察生成的 x86-64 数组寻址汇编

```bash
./build-linux/minicc tests/lang/test_basics_10_array.cpp -S -o /tmp/arr.s
grep -A 15 "raw array/pointer" /tmp/arr.s
```

输出示例（展示 SIB 寻址与加权计算）：

```gas
    # raw array/pointer subscript a[i]
    leaq -32(%rbp), %rax    # 局部数组 a decay 为首元素地址
    pushq %rax              # 暂存基址
    movq $2, %rax           # 整数字面量载入 rax (下标 i=2)
    popq %rcx               # 弹出基址到 rcx
    leaq (%rcx, %rax, 4), %rcx # 目标地址 = base + i * 4
    movl (%rcx), %eax       # 按 4 字节读出
```

---

## 4. 真实版多做了什么、为什么

素材来自 §1.4 对照表的「简化与设计取舍」列。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `ASTContext::getConstantArrayType` 支持**未知大小数组**（`int a[] = {1,2,3};` 由初始化式反推大小） | 本实现要求声明处就带 `[N]`；推导大小要多一套"先建未知大小类型、后回填"的机制，与"类型自带大小"的模型相悖 |
| `Sema::ActOnArrayDeclarator` 的**完整常量求值** | 本实现复用 `ConstantFolder` 求正整数；能求的形态已覆盖教学用例 |
| `DefaultFunctionArrayLvalueConversion` 发射独立的 **ArrayToPointerDecay** AST 节点 | 本实现**零 AST 冗余节点**：CodeGen 在 `VarExpr` / `MemberExpr` 遇数组类型直接发 `leaq` 兑现退化语义 |
| `CGBuilder::CreateGEP` 抽象出的 GetElementPtr 指令（可做向量化的地址计算） | 本实现直接用 x86-64 SIB 寻址模式 `leaq (%rcx,%rax,scale)`，少一层指针算术的 IR |
| clang 支持**多维数组**与数组的**运行时边界检查** | 数组类型模型能递归求尺寸（§1.1），但多维形态**本项目未验证**；边界检查属 sanitizer 范畴，教学版不做 |
| clang 的 `Type` 系统里有独立的 `ConstantArrayType` / `VariableArrayType` / `IncompleteArrayType` 三态 | minicc 只有一个 `Array`（大小必填），教学用得上"大小未知数组"的场合很少 |

---

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-10-15 | 落地主线 E 首批：原生数组类型（`TypeKind::Array`）+ 声明符后缀提取 + 数组向指针退化 + SIB 下标寻址（读与写两条路） | 主线 E 首批（ROADMAP）；CodeGen 一行未改 —— 全程按元素宽度分派，新增标量类型的代码生成成本为 0（git 5af6604） |
