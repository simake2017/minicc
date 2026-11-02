# 41 — switch / do-while / 逗号表达式（主线 C 二批）

> 主线：ROADMAP 主线 C 二批。前接：38（for / break / continue / 三元）。
> 测试：`tests/flow/test_flow_05_switch.cpp`、
> `tests/flow/test_flow_06_do_while_and_comma.cpp`、
> `tests/flow/test_flow_07_error_case_outside_switch.cpp`（负向）。

## §0 定位与能力清单

switch 在汇编层是"一串比较 + 跳转"，do-while 是"标签次序换个摆法"，
逗号表达式是"括号里的第一个操作数被丢掉"。三者的共同点是**语义都依赖
上下文**：break 能不能用取决于外层有没有循环或 switch，case 标号能不能写
取决于外层是不是 switch，逗号要不要当运算符取决于当前是不是括号分组。

| 能力 | 一句话 | 状态 |
|---|---|---|
| switch 的比较链降级 | 条件压栈，逐条 cmp-je，无匹配落 default 或出口 | 已实现 |
| case 标号的常量性 | 非常量值报错（文案与 clang 逐字同） | 已实现 |
| case 值重复拒绝 | `m_caseValueSets` 栈保证，同值报 duplicate | 已实现 |
| default 分支 | 无匹配落 default 或出口 | 已实现 |
| fall-through 是默认语义 | 不穿 break 的 case 自然贯穿到下一条 | 已实现 |
| break 在 switch 内合法 | `m_switchDepth` 与 `m_loopDepth` 任一成立即可 | 已实现 |
| continue 仍须在循环内 | switch 内写 continue 报 `'continue' statement not in loop statement` | 已实现 |
| do-while（体先于条件） | 与 while 的唯一差别是标签次序 | 已实现 |
| 逗号表达式 | 只在 `parsePrimaryExpr` 的括号分组分支开放 | 已实现 |
| 逗号表达式左操作数为纯字面量时消除 | `[fold] (1, 2) → 2`，与恒定三元同款死分支消除 | 已实现 |
| 跳表（jump table）下沉 | 教学实现恒用比较链 | 未做 |
| 嵌套块内的 case 穿透分发 | 未做 | 未做 |
| 全语言逗号运算符 | 仅括号内开放（其余位置的 `,` 仍是分隔符） | 部分 |
| 区间 for | 未做 | 未做 |

---

## 1. 理论背景

### 1.1 switch 的本质与 C++ 规则

[stmt.switch]：值匹配的多路选择语句。

- **fall-through 是默认语义**：不穿 `break` 的 case 标号自然贯穿到
  下一条 case 的代码。贯穿不是 bug，是 C 的有意设计（Duff's device 等技巧的基础）；
- **case 标号的常量性**：case 值必须是核心常量表达式（[expr.const]），
  非常量值报错（clang 文案逐字同）；
- **重复值拒绝**：同一 switch 内两个 case 值相等为 ill-formed；
- **break 的上下文共享**：`break` 在循环**或 switch** 内合法；
  但 `continue` **只能在循环内** —— switch 内写 continue 是
  clang 的 `'continue' statement not in loop statement`。

### 1.2 对照 clang

| clang | 本实现 | 简化了什么 |
|---|---|---|
| `SelectionDAG` / `LowerSwitch`：两种下沉（密值跳表 / 稀疏比较链） | 比较链降级（条件压栈，逐条 cmp-je，无匹配落 default 或出口） | 教学实现恒用比较链，避免 jump table 的重定位复杂度 |
| `ParseStatement` 首 Token 分派（tok::kw_switch / tok::kw_case / tok::kw_do） | 同构：Token 流按首关键字分派 | —— |
| `ActOnCaseStmt`：检查 CurContext 是不是 SwitchScope，检查常量性与重复 | `m_caseValueSets` 栈（进 switch 压栈、出弹栈），插不进报 duplicate | 同构 |
| `BinaryOperator(BO_Comma)`（全语言逗号表达式） | 仅在【括号内】开放的 `CommaExpr` | 调用实参表/初始化列表的 `,` 仍为分隔符（避免上下文推导，见 §2.4） |

## 2. 设计决策

### 2.1 switch 的比较链降级

```
                  ┌───────────────────────┐
                  │ emitExpr(condition)   │
                  │ pushq %rax            │ ── 条件值暂存栈顶
                  └───────────┬───────────┘
                              │
             ┌────────────────┴────────────────┐
             ▼                                 ▼
   cmpq $1, (%rsp); je case1         cmpq $2, (%rsp); je case2 ...
             │                                 │
             └────────────────┬────────────────┘
                              ▼ 无匹配
                     jmp default / endLabel
                              │
  case1: <body>  ─────────────┼─► 自然贯穿（无 break）
  case2: <body>; jmp endLabel ─┼─► break 跳出口
  default: <body>             │
                              ▼
  endLabel: addq $8, %rsp ────┴─► 释放暂存槽
```

**为什么栈顶暂存**：比较链要反复取用条件值。用寄存器会与体内表达式
求值撞车（rax/rcx 随时被冲掉）；栈顶最干净，出 switch 时一条
`addq $8, %rsp` 回收。

### 2.2 break / continue 守卫的对称性

```
               m_loopDepth > 0    m_switchDepth > 0
break 合法？         ✓                  ✓       （两者任一成立即可）
continue 合法？      ✓                  ✗       （必须在循环内）
```

docs/learn/38 里为 break 留的预留文案
`'break' statement not in loop or switch statement` 至此**名副其实**。

### 2.3 do-while：体先于条件执行

与 while 的唯一 codegen 差别是**标签次序**：

```
while:    condLabel: 测试; je endLabel; bodyLabel: 体; jmp condLabel; endLabel:
do-while: bodyLabel: 体; condLabel: 测试; jne bodyLabel; endLabel:
```

continue 落点两者**相同**（都落 condLabel，与 for 的 incrLabel 不同）。

### 2.4 逗号表达式只在括号内开放

`f(a, b)` 的 `,` 是实参分隔符，`int a = 1, b = 2;` 的 `,` 是声明符分隔符。
若在全语言顶层开放逗号为二元运算符，每处 `,` 都得看语法上下文消歧。
本实现照 [expr.comma] 最常见的惯用形态（`(touch(5), 42)`）：只在
`parsePrimaryExpr` 的括号分组分支开放 `match(TokenType::Comma)`，
其他位置完全不受影响（边界诚实登记）。

左操作数纯字面量（无副作用）时在 ConstantFolder 被消除
（`[fold] (1, 2) → 2`，与恒定三元同款死分支消除）。

## 3. 可复现实验

```bash
# switch 贯穿 / break / default
./minicc tests/flow/test_flow_05_switch.cpp -o /tmp/f5 && /tmp/f5; echo $?

# do-while + 逗号表达式
./minicc tests/flow/test_flow_06_do_while_and_comma.cpp -o /tmp/f6 && /tmp/f6; echo $?

# 负向（与 clang++-18 -std=c++20 逐字同）
./minicc tests/flow/test_flow_07_error_case_outside_switch.cpp     # rc=1

# 三绿
./build.sh && (cd build-linux && ctest) && ./logdiff.sh diff
```

## 4. 边界与有意漂移

- **有意日志漂移 1 处**：`flow__01` 的 `[break]` 日志追加了
  `switch-depth=0`（如实反映新守卫，已重刷基线）。
- **未做**：跳表（jump table）下沉（密集标号优化）、嵌套块内的 case
  穿透分发、区间 for（主线 C 余量，依赖主线 E 数组）。

---

## 5. 真实版多做了什么、为什么

素材来自 §1.2 对照表的「简化了什么」列与 §4 的边界清单。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `SelectionDAG` / `LowerSwitch` 的**密值跳表**下沉 | 跳表需要常量池与重定位，minicc 的自研链接器没有重定位池；恒用比较链，行为正确即可 |
| `ActOnCaseStmt` 的完整合法性检查（case 是否在 switch 作用域内等） | 本实现用 `m_caseValueSets` 栈做"常量性 + 重复"两条；作用域检查靠进出 switch 的压栈 / 弹栈隐含完成 |
| clang 的 case 值接受**任意整型常量表达式** | 本实现只接受可常量求值的字面量族；完整常量表达式求值是主线 D 的范围（learn/39 / learn/40） |
| `BinaryOperator(BO_Comma)` 是**全语言**逗号运算符 | 全局开放要每处 `,` 做语法上下文消歧（实参表分隔符 / 声明符分隔符）；只在括号分组开放是"够用且不引歧义" |
| clang 的 `,` 在 for 头第三格天然可用（`i++, j++`） | 本语言 for 头第三格收的是"表达式或赋值"，逗号表达式仍需括号 |
| clang 的 switch 条件是**整型提升后**的表达式 | 本实现直接比较条件压栈的值；提升规则属类型系统范围 |
| clang 的 `case` 值在同一个 switch 内按**声明顺序**做区间分析 | 本实现逐条 cmp-je，无区间归并 |

---

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-10-15 | 落地主线 C 二批：AST 五节点（SwitchStmt / CaseStmt / DefaultStmt / DoWhileStmt / CommaExpr）+ `m_switchDepth` + `m_caseValueSets` 栈 | learn/38 预留的 break 文案"or switch"至此名副其实；case 值必须常量、不得重复是 [stmt.switch] 的强制义务（git 5af6604） |
