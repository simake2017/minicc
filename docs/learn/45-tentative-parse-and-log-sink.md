# 45 — 试探解析的生产方式：TokenCheckpoint 存档点 + 日志单一出口

> 对应 BUGS.md **B32**（ROADMAP 主线 I 第 2 项，顺带完成第 3 项）。
> 动机一句话：试探-回滚的清理旧实现只做了一半 —— 只撤 `m_pos`，
> 试探期打的日志撤不回，成幽灵行；而基线曾把 14 行这样的幽灵
> 固化进 5 个文件没人察觉。

## 1. 理论背景：tentative parsing 是生产 parser 的基本功

C++ 语法有大量"看后面才知道是什么"的歧义点（[stmt.ambig] 整节都在讲这个），
生产 parser 的标准答案是**试探解析（tentative parsing）**：

```
存档（checkpoint） ⇒ 按假设 A 空跑一段 ⇒ 看结果
    成立   ⇒ commit：保留进度
    不成立 ⇒ 回卷（rollback）：全部状态恢复到存档点，按假设 B 重新解析
```

clang 把它做成 RAII：`TentativeParsingAction`（clang ParseTentative.cpp）
构造时 `TentativeParsingAction PA(Actions, true)` 存下 token 游标与注解状态，
析构自动回卷，`Commit()` 之后不再回卷。**新增状态维度时，编译器逼着你**
把它加进快照——这是防"回滚漏维度"的**结构保证**，不是靠 Code Review 记性。

minicc 此前有三个手写版（saved 变量 + `m_pos = saved`），本批只改了
产生幽灵行的那一处（template-id 消歧）；另两处（parseDeclaration 的
函数/变量前瞻、类体方法/字段前瞻）不产生幽灵只产生**重复**（`parseType`
打两遍），记为后续批次。

## 2. 缺陷与修法

### 2.1 幽灵行现场（B32 复现）

```
源码        for (int i = 0; i < 10; i = i + 1) ...
                              └┬┘
试探        parsePrimaryExpr(i) 见 '<' ⇒ 空跑实参表
                [parse:targ] ★ non-type argument (NTTP): integer literal 10   ← 打了
                expect '>' 撞上 ';' ⇒ 抛异常
            catch ⇒ isTemplateId = false
            m_pos = saved            ← 只撤了游标
            重解析    i < 10 是比较运算，语法语义都对
日志        [parse:targ] 那行撤不回 —— 与最终 AST 对不上（幽灵）
```

对"日志即契约"的基线体系（logdiff 逐字节比对、单测子串匹配、
docs 直接粘贴日志片段）这是真污染：用户照着日志学解析过程会被误导。

### 2.2 修法①：TokenCheckpoint = 存档 + 回卷 + **日志缓冲**

```
class TokenCheckpoint {
    Parser& p;  size_t pos;  bool stopAtGreater;  bool committed = false;
public:
    TokenCheckpoint(Parser& p) : p(p), pos(p.m_pos),
        stopAtGreater(p.m_stopAtGreater) {
        p.m_logBuffers.emplace_back();       // ← 试探期日志进缓冲（不只是静默！）
    }
    ~TokenCheckpoint() {
        std::string buf = std::move(p.m_logBuffers.back());
        p.m_logBuffers.pop_back();
        if (committed) {                     // 成立：日志是真的，冲刷
            if (p.m_logBuffers.empty()) std::cout << buf;
            else p.m_logBuffers.back() += buf;   // 嵌套试探：并入外层缓冲
        }                                    // 未成立：连 token 进度一起废弃
        if (!committed) { p.m_pos = pos; p.m_stopAtGreater = stopAtGreater; }
    }
    void commit() { committed = true; }
};
```

★ **设计取舍：缓冲-冲刷，而不是静默开关**。台账原提案是 `m_quietDepth`
试探期静默——但成功路径的 `[parse:targ]` 等日志是【真日志】（既有基线
和单测子串匹配全靠它们），静默会让成功路径也丢行、全量基线漂移。
缓冲的语义精确对应"这次解析没发生过"：commit ⇒ 日志落盘（输出逐字节
不变），回卷 ⇒ 连同语法进度一起废弃。既有 150 个基线文件因此**零漂移**。

★ 缓冲还带来一个白捡的正确性：旧代码里 `parseTemplateArgumentList`
**内部**的 `m_stopAtGreater = true/false`（实参表 > 收尾规则）在异常
路径上**没人恢复**——裸 `m_pos = saved` 不救它。TokenCheckpoint 的快照
把它修对了（`a < b;` 异常后 m_stopAtGreater 不再泄漏为 true）。

### 2.3 修法②：日志单一出口 logParse

收口前 parser.cpp 有 **87 处** `std::cout` 散点直写——符号定名、择优
早已"判据单点"，日志却一直是散点；同一个原则两处执行力度不同。
全部收进：

```cpp
template<typename... Args>
void logParse(std::string_view fmt, Args&&... args) {
    std::string line = std::vformat(fmt, std::make_format_args(args...));
    if (m_logBuffers.empty()) std::cout << line;   // 非试探路径：直出，逐字节不变
    else m_logBuffers.back() += line;              // 试探路径：进缓冲
}
```

链式多段输出（如实参表摘要 `cout << head << ", " << item << tail`）
改为拼 `std::string` 后单点调用——保证整条摘要要么全进缓冲要么全出。
`parseIntLiteral` 的"刻意不打日志"约定（learn/39：它被每条整数表达式调用）
**不破**：它根本不调用 logParse。

### 2.4 修法③：ParseError —— 异常只用于真错误（主线 I 第 3 项）

旧试探代码 `catch (const std::exception&)` —— 这意味着试探内部抛出的
**任何**异常（包括越界、logic_error 这类真 bug）都会被吞掉，静默降级成
"不是模板，按比较运算继续"。错误被静默降级是最坏的一类失败。

```cpp
class ParseError : public std::runtime_error { ... };   // panic 模式唯一的预期异常
throw ParseError(...)              // errorAt：语法错误的专用类型
catch (const ParseError&)          // 试探处：只有"语法错误"才是"不是 template-id"的信号
                                   // 真 bug 穿透到顶层，响亮报错
```

顶层驱动仍按 `std::exception` 捕获（is-a runtime_error），报错出口不变；
字面量溢出那处 `runtime_error` 也一并归入 ParseError。

## 3. clang 源码对照表

| clang（llvm-project/） | 本实现 | 简化 |
|---|---|---|
| `TentativeParsingAction`（ParseTentative.cpp） | `TokenCheckpoint`（parser.h） | clang 快照 token 游标 + 注解栈 + 翻译单元状态；本实现只快照 m_pos + m_stopAtGreater + 日志缓冲 |
| `Parser::ParseImplicitTemplateId` / `AnnotateTemplateIdToken` | 试探实参表 + 跟 token 判定（B31 后加登记表查询） | clang 会把 template-id 注解成专用 token；本实现留在 AST 层 |
| `Sema::isTemplateName` 反向查询 | ⬜ B31 未做（Parser 只读模板名登记表） | — |
| diag 系统（DiagnosticsEngine） | logParse 单出口 + 缓冲 | clang 诊断可走 river 抑制；本实现只需"这次解析没发生过" |

## 4. 可复现实验

```bash
# ① 新用例：合法形态（循环 NTTP 试探 + 连续比较链），运行钉 0
./minicc tests/lang/test_basics_19_tentative_rollback.cpp -o /tmp/t19 && /tmp/t19; echo "run=$?"
clang++-18 -std=c++20 tests/lang/test_basics_19_tentative_rollback.cpp -o /tmp/t19c && /tmp/t19c; echo "clang run=$?"

# ② 幽灵行绝迹断言（应输出 0 —— 全部匹配只可能是 [pp] 源码回声）
./minicc tests/lang/test_basics_19_tentative_rollback.cpp -o /tmp/t19 2>&1 \
  | grep -cE "parse:targ|base = y|final type = y"

# ③ 全量回归：ctest 346 全绿；既有 150 文件零漂移（5 文件有意删 14 行幽灵）
ctest --test-dir build-linux -j6 | tail -1
./logdiff.sh diff
```

## 5. 关键过程 ASCII 图：`i < 10;` 的试探（修复前后）

```
修前    ┌ checkpoint 不存在
        │ parsePrimaryExpr(i)──见 '<'──┐
        │   [parse:targ] NTTP: 10      │ 空跑实参表，打印日志
        │   expect '>' 撞 ';' ⇒ 抛异常  │
        │ catch ⇒ isTemplateId=false   │
        │ m_pos = saved                │ 只撤游标 ★日志留下★；且若异常发生在
        │                              │ parseTemplateArgumentList 内部
        │                              │ m_stopAtGreater=true 的窗口里，
        │                              │ 该布尔位无人恢复（裸回滚不救它）
        │ 重解析：i < 10 比较 ✓         │
修后    ┌ TokenCheckpoint cp(*this)     │ 快照 + 日志进缓冲
        │ parsePrimaryExpr(i)──见 '<'──┐│
        │   logParse(...) ⇒ 进缓冲      ││ 外面看不见
        │   expect '>' 撞 ';' ⇒ ParseError
        │ catch (ParseError&) ⇒ false  │
        │ 出作用域：cp 未 commit        │
        │   缓冲丢弃 + m_pos/m_stopAtGreater 回卷  ← "这次解析没发生过"
        │ 重解析：i < 10 比较 ✓         │ 日志与 AST 一致
```

## 6. 边界与未做

| 项 | 现状 |
|---|---|
| 另两处试探（parseDeclaration 890 / 类体 1862） | 只产生【重复】不产幽灵（parseType 打两遍）：等单独批次瘦身，届时全量基线 [parse:type] 行数近半 |
| B32 原始复现 `a < Box > c;` | clang 实测**非法**（"does not name a template"）；minicc 宽松放行——B31 登记表落地后再裁 |
| 嵌套试探 | 缓冲栈天然支持（内层 commit 并入外层缓冲），无额外代码 |
| m_pendingConstexpr 等前缀状态 | 不在试探快照内：试探期 parseType 吃到 constexpr 会置位并泄漏——当前文法下试探入口只能是表达式，parseType 走不到 constexpr 前缀，属理论缺口；若将来试探范围扩大须并入快照 |
| B31（消歧判据本身） | ⬜ 未修（主线 I 第 1 项）：`a<b>(c)` 全变量比较链仍被误判为模板调用，见 BUGS.md B31 |
