# 42 — 原生数组与下标寻址（主线 E 首批）

> 主线：ROADMAP 主线 E 首批。前接：39（常量折叠）、40（constexpr）、41（switch / do-while / 逗号）。
> 测试：`tests/lang/test_basics_10_array.cpp`。

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
