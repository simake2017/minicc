# 47. parser 阶段名字识别：从模板名登记表到多 bit 分类 + 作用域裁决

> 需求原文：「parser 阶段实现类似 clang 的语义识别」。
> 流程产物：`docs/specs/parser-stage-name-recognition.spec.md`（已签字）、
> `docs/designs/parser-stage-name-recognition.design.md`。
> 代码落点：`include/name_registry.h` + `src/name_registry.cpp`（新模块）、
> `src/parser.cpp` 三消费点、`src/semantic_analyzer.cpp` 一消费点 + 接线。
> 台账：BUGS.md B36。

## 1. 理论背景

### 1.1 [stmt.ambig]：为什么解析期需要"名字种类"这一个 bit

C++ 语句 `S;` 有两种合法解释：S 是类型名 ⇒ 无声明符的声明（clang 给
`-Wdeclaration-does-not-declare-anything` warning）；S 是对象 ⇒ 表达式
语句（`-Wunused-value`）。语法本身分不出来——[stmt.ambig]/3 的规定是
**「能当声明就当声明」**，即类型名优先。要实现这条规则，解析器必须
回答"**S 是不是一个类型名**"——这就是解析期语义识别：不是全量符号表
查询，而是**名字分类**（name classification）问题。

clang 的做法（对照表见 §3）：`Sema::ClassifyName`
（clang/lib/Sema/SemaDecl.cpp:912）在语句歧义点被调用，沿 Scope /
DeclContext 链查**活符号表**，返回名字的分类（类型/命名空间/模板/值…）。
Parser 通过 Actions 句柄发起查询——决议权始终在 Sema，Parser 不做语义
判断，这就是"语法期零语义"分层的真实含义：**不是没有语义信息，而是
语义信息只以「查询一个 bit」的方式穿透进来**。

### 1.2 minicc 的红线与投影机制

minicc 的分层红线（B16/B21 确立，spec 否决记录 1）：**Parser 不持
Sema 活句柄**。原因有二：

1. Sema 的三趟扫描（注册→决议→走查）在**解析完成后**才开始，活符号表
   回答不了单趟解析的问题（先使用后声明在解析期不可见，但合法）；
2. 消歧判据一旦变成活语义查询，「宁宽勿窄」的验收口径（误收=响亮拒收、
   漏收=方向性错）就失效了。

所以 minicc 用**构造期只读投影**代替活查询：解析开始前，前向扫一遍
token 流，把本 TU 声明过的名字按**种类**登记成一张只读表（B31 首个
实现只有「是否模板」一个 bit；本主题扩展为多位 + 作用域）。它是对
未来 Sema 符号表的**投影**——只有「名字是哪类」的信息，没有类型实体、
重载集、偏移。查询面只到几个 bit，不打破分层。

### 1.3 作用域：从"末段近似"到精确裁决

B35 给 Sema 补限定名归一时取了 clang QualifiedNameLookup 的**结果**
（`N::B<int>` 归一裸名 B，与 `B<int>` 同实例同缓存键），但判据是
**裸尾剥**：取 `::` 末段查全局蓝图表。N::B 真存在时语义正确；真不存在
而全局有同名模板 B 时**误绑**（B36，方向性错）。修法是把判据补齐：
登记表键从裸名升级为 (scope, name)，Sema 经 `classifyQualified(scope,
name)` 做**作用域精确裁决**——配不上即报错
`no template named 'B' in namespace 'N'`（与 clang 逐字）。

## 2. 设计：四位分类 + 两个查询入口 + 四个消费点

```text
                 ┌─────────── NameRegistry（写一次，只读）──────────┐
 收集(3.1)       │  键 = (scope, name) → 位并集                      │
  · template 头  │  type / namespace / template / value(恒 false 占位)│
  · struct/class │                                                    │
  · enum/typedef │  classify(name)        classifyQualified(scope,n)  │
  · using X = T  │  Parser 侧：全名/      Sema 侧：scope 精确         │
  · namespace    │  末段近似（宽）        （唯一裁决依据）             │
  · declval 种子  └───────────────┬────────────────────┬─────────────┘
                                 │                    │
        Parser 三消费点          │                    │  Sema 一消费点
        ① 表达式模板闸            │                    │  ④ 限定模板名
        ② 语句层模板 id 链闸      │                    │     N::B 裁决
        ③ 语句层裸名 S; 判声明    │                    │     （修 B36 误绑）
```

**关键判据**（spec 1.2 的签名规则）：任何消费点**只允许依赖单个位的
命中与否**；「不在表里」= 未知 = 按宁宽勿窄处置（语句层不判声明、
表达式层不做模板试探），禁止反向推断。这是「查不到≠不是」的直觉陷阱
的防线——全 false 的默认对象表达「未知」，不表达「非类型」。

**同名跨类**：C++ 里类型与普通名字在不同命名空间（`int S; struct S;`
合法并存），分类是**位并集**不是单选枚举；裸 `S;` 只看 type 位 ⇒
判声明（声明优先，不读对象）。

**template 头后不置 type 位**：`template<class T> struct Box` 的 Box
只有 template 位——模板名不是类型名，`Box;` 保持非类型解释。收集器
记下 template 头名字的 token 下标，规则 2/4 跳过（第一版漏了别名
模板，被「`Vec;` 对别名模板名应保持非类型」的边界测试抓出）。

**作用域栈**：`namespace X {` 推栈（frame 记进入深度），花括号深度
回落即弹栈；类体/函数体只改深度不入栈——类内名字归属外层命名空间
（局部类误登记为全局是宽方向，试探/报错兜底）。

## 3. clang 源码对照表

| clang（~/cppproject/llvm-project/） | 本实现 | 简化了什么 |
|---|---|---|
| `Sema::ClassifyName`（SemaDecl.cpp:912，声明于 Sema.h:3862） | `NameRegistry::classify` | clang 查活符号表（Scope/DeclContext 链）+ 重载/模板特化决议，返回带 Decl 指针的完整分类；本实现查构造期投影，只回答「哪类」 |
| `Sema::isTemplateName`（SemaTemplate.cpp:179） | `NameRegistry::isTemplateName` / `classifyQualified` | 无 TemplateNameKind 细分（模板/特化/重载集） |
| `Parser::TentativeParsingAction`（Parse/Parser.h:8479） | `TokenCheckpoint`（learn/45） | 已对齐：RAII + 全维度快照 + 日志缓冲 |
| `Parser::TryAnnotateTypeToken`（Parse/Parser.cpp） | B35 语句层试探（parseStatement） | 已对齐：tentative parseType；本主题加 type 位消费点 |
| clang 无 collect（查活符号表，无需前向收集） | `NameRegistry::collect` | 红线 1 的直接产物：用空间（一张表）换分层 |

## 4. 可复现实验

```bash
# ① 裸名声明判定（type 位）
cat > /tmp/t47a.cpp <<'EOF'
struct S { int x; };
int main() { S; return 0; }
EOF
./minicc /tmp/t47a.cpp -o /tmp/t47a | grep "parse:stmt"
#   [parse:stmt] 'S' 判为声明（type 位）      ← 声明解释（EmptyStmt）
clang++-18 -std=c++20 -fsyntax-only /tmp/t47a.cpp   # rc=0（仅 warning）

# ② 误绑负例（B36 复现）
cat > /tmp/t47b.cpp <<'EOF'
template<class T> struct B { T w; };
namespace N {}
int main() { N::B<int> y; return 0; }
EOF
./minicc /tmp/t47b.cpp | grep ERROR
#   no template named 'B' in namespace 'N'
clang++-18 -std=c++20 -fsyntax-only /tmp/t47b.cpp   # 同款报错 + did-you-mean 附注

# ③ 全量验证
ctest --test-dir build-linux -j6     # 362/362
./logdiff.sh diff                    # 零差异（155 份基线）
```

## 5. 验收与有意漂移

- **362/362 单测全绿**（346 存量 + 13 NameRegistry.* + 2 集成包装 +
  3 修复的陈旧假象用例），build.sh 0 警告。
- **有意日志漂移 2 处**（已重刷基线 153→155 份）：tmpl_54 的
  unknown-type-name 括注改写（旧「跳过 <...> + 回滚」文案自 B35 起因
  过时误导排查）；新增 basics_23/24 两份日志。
- **突变验证 3 条**逐条实跑变红/恢复转绿（见 BUGS.md B36 回归小节）。

## 6. 教训两则

1. **陈旧二进制是第三种失败面具**。B31 引入登记表第一层闸时，
   test_expr_parser 的 template-id 套件其实当场就红了（snippet 从未
   声明 foo），但当次 ctest 跑的是**没重建的 unit_tests**，绿色是
   假象——直到今天重建才暴露。前两种面具（负向全绿掩盖拒收合法程序、
   logdiff 基线固化失败）都有记录，这第三种同属一族：**跑测试前确认
   被测二进制真的是刚编出来的**。
2. **查不到 ≠ 不是**。消费点只依赖「位命中」做正向判定，「全 false」
   只表达未知——spec 把它写成签名规则，因为「用否定查询做正向推断」
   是这类消歧最常见的错法（B27「退回声明序首个」同族）。
