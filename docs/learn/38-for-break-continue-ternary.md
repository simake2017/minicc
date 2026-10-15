# 38. for / break / continue / 三元 ?: —— 主线 C 首批

> 主题：结构化控制流的降级（CFG 视角）与条件表达式的短路语义
> 理论点：[stmt.iter]/[stmt.for]、[stmt.jump]/[stmt.break]/[stmt.cont]、[expr.cond]；
> 语法层"赋值是语句"的边界与 for 头的局部放宽
> clang 对照：`ParseForStatement`（ParseStmt.cpp）、`CodeGenFunction::EmitForStmt`
> （clang/lib/CodeGen/CGStmt.cpp）、`ParseConditionalExpression`（ParseExpr.cpp）

## 1. 理论背景：结构化控制 = goto 的糖

高级语言的控制流在汇编层只有两样东西：**条件跳转**（je/jne）与**无条件跳转**（jmp）。
for 循环是「 init 恰好一次 + 条件测试 + 体 + 增量 + 回边 」的组合糖：

```text
for (int i = 0; i < n; i = i + 1) { body }
        │
        ▼  降级（本实现 codegen.cpp visit(ForStmt)，五标签）
for_init:   初始化（只执行一次）
for_cond:   求条件 → testq → je for_end        ←┐
for_body:   body                                │
for_incr:   增量   ←── continue 落点           │ 回边
            jmp for_cond ──────────────────────┘
for_end:    ←── break 落点
```

两个容易错死的细节：

1. **continue 落在 incr，不是 cond**。`for(;;)` 里若 continue 直落条件式，
   增量被跳过就是死循环。while 没有独立增量步，它的 continue 落点就是条件式
   —— 两个循环入栈时约定的落点不同，栈里存一对 {break 出口, continue 落点}。
2. **break/continue 只作用最近一层**。嵌套循环下这是纯粹的「栈顶语义」：
   生成期一个 `m_loopStack` 解决，无需任何静态分析。

## 2. 语义层的循环上下文栈（Sema）

语法层【不判】break/continue 合不合法（文法里它们在任何语句位置都能出现），
合法性是语义问题：Sema 维护 `m_loopDepth`（进 for/while 体 +1、出 -1），
栈空时遇到 break/continue 即报错，文案逐字沿用 clang：

```text
'break' statement not in loop or switch statement
'continue' statement not in loop statement
```

（break 的文案带 "or switch" —— minicc 还没有 switch，但 oracle 文本以 clang
为准，将来做 switch 时基线不再漂。）

作用域注意点：C++ 里 for-init 声明的名字**贯穿整个循环**（cond、incr、body
都可见），所以 `enterScope("for-init")` 包住 init→cond→incr→body 全程，
而不是只包 init。

## 3. 语法边界：赋值是语句，for 头局部放宽

本语言沿用早期设计：**赋值是语句不是表达式**（AssignStmt），`a = b = c`、
`while ((x = next()))` 这类写法本来就不支持。但 for 头的
「`for (i = 0; i < n; i = i + 1)`」是最经典的循环形态，第三格（乃至第一格）
如果只能写表达式，`i = i + 1` 直接 Parse Error —— 这个边界会把最普通的
教学示例拒之门外。

处置：for 头的 init / incr 两格**局部放宽**为「表达式或赋值」——解析出
表达式后若紧跟 `=`，就地在 for 头内构一个 AssignStmt（这两格的 AST 槽
因此是 StmtPtr 而非 ExprPtr）。`a = b = c` 之类的链式赋值仍是语句语法，
不受影响。

## 4. 三元表达式 ?:（[expr.cond]）

优先级上，?: 夹在 || 与赋值之间，**右结合**（`a?b:c?d:e` = `a?b:(c?d:e)`）。
实现：parseExpression 的最顶层包一层——

```cpp
ExprPtr Parser::parseExpression() {
    ExprPtr cond = parseOrExpr();
    if (!check(TokenType::Question)) return cond;
    advance();
    ExprPtr thenExpr = parseExpression();   // 中支是全表达式
    expect(TokenType::Colon, ...);
    ExprPtr elseExpr = parseExpression();   // 右结合挂在这一支
    return std::make_shared<ConditionalExpr>(cond, thenExpr, elseExpr);
}
```

**短路语义**（[expr.cond]/1 的 "only one of the second and third expressions
is evaluated"）靠代码生成的两行跳转实现，不是靠算完再选：

```text
  <cond> → testq → je cond_else
  <then 支求值，结果在 rax> → jmp cond_end   ← 条件为假时这段【整体跳过】
cond_else: <else 支求值>
cond_end:
```

test_flow_02 用两个全局副作用计数器钉死它：条件为真时 else 支零调用、
为假时 then 支零调用。

类型规则教学级简化：两支必须同型（[expr.cond]/3 的合成类型规则不做，
int/double 混排等主线 D 再说）；结果类型 = 分支共同类型。

## 5. clang 源码对照表

| clang（llvm-project/） | 本实现 | 简化了什么 |
|---|---|---|
| `ParseForStatement`：init-statement 复用声明/表达式两条既有通路，cond/incr 可空 | `Parser::parseForStmt` 同构；差别在赋值 —— clang 里赋值天生是表达式，本语言是语句，for 头局部放宽 | 无范围-based for；无 for-init 里的属性/对齐说明符 |
| `CodeGenFunction::EmitForStmt`：EmitStmt(init) → EmitCondition → body → EmitStmt(increment) → 回边；break/continue 经 BreakContinueStack 查目标块 | `CodeGen::visit(ForStmt)` 五标签；`m_loopStack` 存的直接是标签字符串而非 BasicBlock* | 无优化（clang 会在 -O1+ 把循环旋转、规范化），教学版只发结构正确的跳转 |
| `ParseConditionalExpression`：cond → '?' → expr → ':' → assignment-expr（右结合由产生式保证）；glvalue 合并规则 | `Parser::parseExpression` 顶层包一层，中/右两支都收全表达式 | 无 glvalue/xvalue 合并；无 braced-init-list 第三支 |
| `Sema::ActOnBreak/ActOnContinue`：查 CurContext 链上有没有循环/switch | `m_loopDepth` 计数器 | 无 switch；continue 目标不检查（恒为最近循环） |

## 6. 可复现实验

```bash
cat > /tmp/f.cpp <<'EOF'
int main() {
    int s = 0;
    for (int i = 1; i <= 100; i = i + 1) s = s + i;
    int m = s > 5000 ? s : 0 - s;
    return m - 5050;
}
EOF
./minicc /tmp/f.cpp -o /tmp/f && /tmp/f; echo "rc=$?"   # 0

# AST 观测
./minicc /tmp/f.cpp --dump-ast -o /dev/null 2>&1 | grep -A2 "ForStmt\|ConditionalExpr"

# 错误用例（文案与 clang 逐字相同）
echo 'int main() { break; }' > /tmp/b.cpp
./minicc /tmp/b.cpp -o /dev/null 2>&1 | grep "Semantic Error"
```

预期日志特征：

```text
  [for] init:
  [for] condition:
  [for] condition type: bool
  [for] increment:
  [cond] int ? int : int → int
  [break] loop-depth=1 ✓
```

## 7. 测试

| 文件 | 性质 | 内容 |
|---|---|---|
| tests/flow/test_flow_01_for_break_continue.cpp | 正例（7 个检查点，失败各返回不同码） | 声明式/表达式式/空 init、continue 落增量步（`for(;;)` 不死循环）、break 出循环、嵌套循环只作用最近一层、while 同样入循环栈 |
| tests/flow/test_flow_02_ternary.cpp | 正例 | 基本选值、**短路副作用钉扎**（全局计数器）、优先级（\|\| 低于 ?:）、右结合嵌套、作实参、模板函数体内的 ?:（cloneExpr Conditional 分支） |
| tests/flow/test_flow_03_error_break_outside_loop.cpp | 负例 | if 体内 break ⇒ `'break' statement not in loop or switch statement` |
| tests/flow/test_flow_04_error_continue_outside_loop.cpp | 负例 | 顶层 continue ⇒ `'continue' statement not in loop statement` |

全量：ctest 340 → **342**（+2 子进程用例）；集成日志基线 126 → **130** 份
（+4 个 flow 文件），既有日志**逐字节零漂移**（for/break/continue/?: 的新日志
只出现在新测试里——旧测试一个 for 都没用过，天然零干扰）。

## 8. 边界与后续（主线 C 剩余）

- `switch`（含 case/default、跳转表 vs if 链决策）——break 文案里的
  "or switch" 已提前留位
- `do-while`（先体后判，continue 落条件式）
- 逗号表达式（for 头第三格 `i++, j++` 的正规形态）
- 区间 for（range-based for，等 ministl 迭代器协议稳定后再说）
- 赋值表达式化（`a = b = c`、while 条件里赋值）——语法级决定，暂不做
