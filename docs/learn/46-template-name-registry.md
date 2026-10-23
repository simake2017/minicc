# 46. 模板名登记表：`<` 消歧的"问 Sema 一个 bit"轻量版

> 主线 I 第 1 项（BUGS.md B31）。前置：learn/22（[stmt.ambig] 语句版）、
> learn/39（NTTP 实参的 `>` 停止规则）、learn/45（B32 试探-回滚 RAII）。

## 1. 问题：`a<b>(c)` 是模板调用还是比较链？

C++ 里 `IDENT '<'` 有两大类语法解释（[stmt.ambig] 的表达式版）：

| 写法 | 解释 |
|---|---|
| `foo<int>(x)` | template-id + 调用（函数模板显式实参） |
| `Cls<int>::value` | template-id + 类型限定静态成员访问 |
| `a<b>(c)` | 比较链：`(a<b)>(c)`，左结合 relational（clang rc=0，仅 -Wparentheses 警告） |
| `a<b<c>d` | 比较链：`((a<b)<c)>d` |

minicc 旧消歧（B32 之前的"三步曲"，B32 已把机制换成 RAII 但**判据没动**）：

```
IDENT '<' ⇒ 空跑 parseTemplateArgumentList()
    成功 && '>' 后跟 '(' 或 '::'  ⇒ template-id
    否则                          ⇒ 回卷，按比较运算解析
```

`a<b>(c)` 全变量时这套判据**双双满足**：`b` 被 parseType 建成 `Class("b")`
节点（语法期不查符号表——learn/22 ⑦ 的"决议推迟到 Sema"基石反过来坑了这里），
"实参表"空跑成功，`>` 后跟 `(` ⇒ 判成模板调用。Sema 阶段找不到模板 `a` 报错
——**拒收合法程序**。变异实测：摘掉判据后链接期报
`undefined reference to 'a'`（把变量 a 当函数模板实例发了 callq）。

## 2. clang 的做法：消歧 = 只读查询，不是全量语义

clang 在 `<` 处**反向查符号表**判"`a` 是不是模板名"（`Parser::isTemplateName`
→ `Sema::isTemplateName`，Parser 持 `Actions` 句柄回调）：

| clang 位置 | 职责 |
|---|---|
| `clang/lib/Parse/Parser.cpp` `isTemplateName(...)` | 语法点发起查询 |
| `clang/lib/Sema/SemaTemplate.cpp` `Sema::isTemplateName(...)` | 符号表决议，回答一个 bit |
| `TentativeParsingAction`（ParseRAII.h） | 存档/回滚，零异常 |

要点：**查询面只到"名字的种类"，不扩大成"方法存在性 lookup"**——决议权仍在
Sema，Parser 不因此变成语义分析器。minicc 的 [stmt.ambig] 修复（B16/B21/
655369a）确立了"语法期零语义"分层，不能因为一个消歧判据就把符号表整个搬进
Parser——需要的是**同一个信息的一个只读投影**。

## 3. 设计：前向扫描登记表

### 3.1 为什么前向收集、而不是边解析边登记

minicc 的 Sema 三遍扫描支持**先使用后声明**（类成员调后面才定义的方法、
模板体内用外层实参等）。若解析到声明才登记，文本上"调用点在声明前"的
合法写法（在模板体/类体等 Sema 多遍可解的范围内）会查不到表。故在
**Parser 构造函数**里对 token 流做一次纯词法前向扫描，消歧时只读查询。

### 3.2 收集规则（src/parser.cpp `collectTemplateNames`）

```
遇 KwTemplate ⇒ 跳过配对的 '<...>'（'>>' 不切 token，逐 Greater 计数）
  后继 = struct/class  ⇒ 下一个 Identifier 是模板名（含偏/全特化的头）
  后继 = using         ⇒ 下一个 Identifier 是别名模板名
  其它（函数模板/推导指引/成员模板）
                     ⇒ 形参表后【第一个"Identifier + '('"】的名字
                       （ret name(params)；Name(params) -> ...；static 说明符跳过）
```

- **不跳过已处理区间**：形参表内部可嵌套 template 头（模板模板形参
  `template <class> class C`），内层名字 `C` 要照常登记——`C<int> m;`
  成员声明靠它消歧。
- **种子名单**：`declval`。它是 Sema 内建（主线 H，inferCall ⓪），不经
  token 流。漏收的真实现场：`is_range<T, void_t<decltype(std::declval<T>()
  .begin())>>`（test_tmpl_35..39）被消歧成比较链，**五个文件当场 rc=1**。
  `std::void_t` 只在类型位置按名识别（resolveType），不走本消歧，不收。
- **不打点**：构造期一行日志会漂移全部基线（150+ 份）。

### 3.3 查询规则（`isKnownTemplateName`）

```
全名命中                      ⇒ 是
否则取 '::' 后最后一段查裸名   ⇒ 是（N::foo 的近似）
否则                          ⇒ 否
```

消歧点（parsePrimaryExpr 变量引用分支）：

```cpp
if (check(TokenType::Less) && isKnownTemplateName(name)) {
    // B32 的 TokenCheckpoint 试探-回滚：在表 ≠ 一定是（变量可遮蔽模板名），
    // 空跑实参表 + '>' 后跟 '('/'::' 仍要成立；试不出来回退比较。
}
// 不在表：连试探都不做，'< 直接交给比较表达式解析
```

### 3.4 方向性：宁可误判进试探，不可漏判成比较

| 方向 | 后果 | 可接受性 |
|---|---|---|
| 表里有、实际是比较（变量遮蔽模板名） | 试探 → Sema 报错 | ✅ 响亮拒收 |
| 表里无、实际是 template-id | 比较链 → 下游莫名报错/错类型 | ❌ 方向性错误 |

所以收集规则**宁宽勿窄**（函数模板分支"第一个 Ident+LParen"会误收同形变量
声明如 `MyPtr(T)`？——那正是推导指引的文法，误收无害：真遇到 `MyPtr < T`
比较时试探会失败回退）。限定名取末段查询会把不同命名空间的同名非模板函数
误收进来——同为宽方向，无害。

### 3.5 与 learn/39 判据的协同

NTTP 字面量 fast path 的 `isExprContinuation` **刻意不收** Less/Greater
（`Buf<4>` 的收尾 `>` 与大于号同 token）。本登记表是**另一条独立闸**：
只作用于"表达式里 IDENT 后紧跟 `<`"的消歧点，不碰字面量 fast path，
两处判据不交叉——tmpl 21..26 逐字节零漂移与此直接相关。

## 4. clang 源码对照表

| clang | 本实现 | 简化了什么 |
|---|---|---|
| `Sema::isTemplateName`（符号表决议） | `Parser::isKnownTemplateName`（set 查询） | clang 查全作用域链+using 指令；minicc 用裸名集合近似，无作用域 |
| `Parser::isTemplateName`（歧义点回调） | parsePrimaryExpr 里 `check(Less) && isKnownTemplateName(name)` | 同款"问一个 bit"形状 |
| `TentativeParsingAction` | B32 `TokenCheckpoint` | 见 learn/45 |
| 内建声明进预处理符号表（`Sema::Initialize`） | declval 种子名单硬编码 | clang 的 builtin 也是名单制，规模不同 |

## 5. 可复现实验

```bash
# 正例（B31 验收）：全变量比较链 + 真模板调用并存
cat > /tmp/b31.cpp <<'EOF'
template<class T> T identity(T x) { return x; }
int main() {
    int a=1; int b=2; int c=3;   // minicc 多声明符未实现，拆三条
    int r = a < b > (c);        // (a<b)>(c)：修复前误判模板调用 rc=1
    return r + identity<int>(6);
}
EOF
./minicc /tmp/b31.cpp -o /tmp/b31 && /tmp/b31; echo "rc=$?"   # 期望 6
clang++-18 -std=c++20 /tmp/b31.cpp -o /tmp/b31c && /tmp/b31c; echo "clang rc=$?"

# 回归：全量 ctest + 日志基线零漂移
ctest --test-dir build-linux -j6 | tail -1
./logdiff.sh diff | tail -1

# 突变验证（摘掉登记表闸 ⇒ 新测试退回链接期 undefined reference to 'a'）
sed -i 's/if (check(TokenType::Less) && isKnownTemplateName(name)) {/if (check(TokenType::Less)) {/' src/parser.cpp
./build.sh >/dev/null && ./minicc tests/lang/test_basics_20_template_id_disambiguation.cpp -o /tmp/mut 2>&1 | tail -3
```

## 6. 关键过程 ASCII 图

```
旧判据（B31 前）：
  a < b > (c)
  └┬┘      parsePrimaryExpr 见 IDENT '<'
   │ 空跑实参表：b → Class("b") 节点（语法期不查表！）✓
   │ '>' 后跟 '(' ✓
   └─⇒ template-id 调用 a<int???>  ──Sema──▶ 无此模板，rc=1 ✗

新判据（登记表闸 + B32 试探）：
  a < b > (c)
  └┬┘ isKnownTemplateName("a")?  查表 ✗（a 是变量，从未声明为模板）
   └─⇒ 不试探，'< 交还比较解析 ⇒ ((a<b)>(c))，rc=0 ✓

  identity<int>(6)
  └─┬─┘ isKnownTemplateName("identity")?  查表 ✓（构造时已前向收集）
    └─⇒ TokenCheckpoint 试探：实参表 ✓ '>' 后跟 '(' ✓ ⇒ commit，真模板调用 ✓
```

## 7. 边界与已知近似

| 边界 | 处置 |
|---|---|
| 变量/函数遮蔽模板名（`int Box; Box < x > (y);`） | 表里有 ⇒ 进试探 ⇒ 试探按后缀判成模板-id ⇒ Sema 报错。**响亮拒收**，可接受（3.4 宽方向） |
| 同名非模板函数跨命名空间（`N::foo` vs 模板 `M::foo`） | 末段查询误收，同宽方向 |
| `new X<...>()`（new 表达式分支） | 未加闸：非模板名时实参表解析失败走原报错路径。后续可加同闸收敛 |
| 表驱动的 `>` 停止规则 | **不做**——与 learn/39 的刻意判据冲突，tmpl 21..26 会漂移 |
| constexpr if / requires 等新式歧义 | 本语言未实现，N/A |
