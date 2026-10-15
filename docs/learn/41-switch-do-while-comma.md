# 41 — switch / do-while / 逗号表达式（主线 C 二批）

> 主线：ROADMAP 主线 C 二批。前接：38（for / break / continue / 三元）。
> 测试：`tests/flow/test_flow_05_switch.cpp`、
> `tests/flow/test_flow_06_do_while_and_comma.cpp`、
> `tests/flow/test_flow_07_error_case_outside_switch.cpp`（负向）。

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
