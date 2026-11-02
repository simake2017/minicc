# 03 显式模板实参与 template-id 解析（S3）

> 子阶段 S3：`mix<int>(1, 2)` —— 显式实参占模板参数表前缀，其余继续推导。

## §0 定位与能力清单

推导不是万能的：形参里没出现 `T`（如 `template<class T> T gen();`）时推不出来，
调用者有时也想直接钉死具体类型（`mix<int>(1, 2)`）。这时要能**显式给实参**。
但显式实参带来两个新问题：① 显式实参与推导出来的实参怎么拼接（显式在前、推导补后）；
② `<` 在表达式里**既是"小于"又是"实参表开头"**，必须消歧。
本文讲的就是这两件事——S3 是把 S2 的闭环补上最后一块的那一环。

| 能力 | 一句话 | 状态 |
|---|---|---|
| [temp.explicit] 显式实参前缀规则 | 显式实参从左到右填模板参数表，只能省略**尾部**可推导的参数 | 已实现 |
| [temp.arg.explicit] 显式 + 推导的拼接 | 显式填前缀、推导补后缀，合成完整实参表 | 已实现 |
| [temp.names] template-id 消歧（结构判据） | 名字 `<` 类型列表 `>` 后紧跟 `(` 才算 template-id，否则回滚交还比较表达式 | 已实现 |
| [temp.names] 消歧（查表 + 试探） | 消歧前先问"这个名字是不是模板名"，不在表连试探都不做 | 已实现（learn/46，B31） |
| 显式实参的形态校验 | 显式给的形态与该位形参 kind 不符时报错 | 已实现（learn/18 的 tagged 实参校验） |
| 语境消解（问符号表） | clang 的 `Actions::ClassifyName` 问 Sema"这个名字是什么" | 部分（learn/47 引入多 bit 分类，learn/48 把可见性裁决权下沉 Sema） |
| `template` 消歧关键字 | `f.template g<int>(x)` 显式要求此处按模板解析 | 未做 |
| 显式实参的默认值 | `template<class T, class U = int>` 中 `U` 缺省时的填充 | 未做 |
| 显式实参的转换 | 显式给的实参到形参的隐式转换（`Buf<4>` 的 4 → unsigned） | 未做（learn/18 明确记为简化） |

## ① 理论背景

- **前缀规则**（[temp.explicit]/[temp.deduct]）：显式实参从左到右填充模板参数表，
  只能省略尾部可推导的参数；`mix<int>` ⇒ T 给定、U 推导。
- **template-id 歧义**：`<` 在表达式里既是"小于"又是"模板实参表开头"。
  C++ 的解法要求名字已被声明为模板（语境消解）；本项目 Parser 无符号信息，
  用**结构判据**：`名字 '<' 类型列表 '>'` 之后紧跟 `'('` 才算 template-id，
  否则回滚交还比较表达式。这是 clang `ParseImplicitTemplateId` 的简化版。

## ② 设计决策

- 显式实参挂在 `VarExpr::explicitTemplateArgs`（template-id 出现在 `(` 之前，
  CallExpr 还不存在）；sema 在 `inferCall` 取用。
- 解析失败回滚用 `m_pos = saved` + try/catch（parseType 报错可恢复）——
  与类成员"方法 vs 字段"的前瞻同款机制，保持 Parser 单一风格。
- 推导引擎侧零成本：显式实参只是预填 subst 表的前缀。

## ③ clang 对照

| 本实现 | clang | 简化 |
|---|---|---|
| parsePrimaryExpr 的前瞻分支 | `ParseTemplate.cpp → ParseImplicitTemplateId` + Sema ActOnExplicitTemplateArgumentList | 无语境消解（不查名字是否模板）、不支持 `template` 消歧关键字 |
| 前缀填充 subst | `DeduceTemplateArguments` 开头对 ExplicitTemplateArguments 的循环 | 一致 |

## ④ 实验

```bash
./build-linux/minicc tests/test_tmpl_17_explicit_args.cpp -o /tmp/t17.s
gcc /tmp/t17.s -o /tmp/t17 && /tmp/t17; echo $?   # 0
# 日志关注：[parse] template-id: mix<...>、"显式给定 / 推导补全"分界
```

## ⑤ 过程图

```
mix<int>(1, 2)
  parse: VarExpr{ name=mix, explicitTemplateArgs=[int] }
         └─ 前瞻: '<' int '>' 后是 '(' → template-id ✓（不回滚）
  sema:  subst 预填 {T := int}（显式）
         P/A 对 b(U) vs 实参 int → U := int（推导）
         ⇒ <T=int, U=int>
```

## ⑥ 真实版多做了什么、为什么

★ 素材来源：本文 §③ 对照的「简化」列 + `docs/BUGS.md` B31/B32/B35/B36/B37
+ CLAUDE.md「名字可见性三阶段」。**未查证的写「（本项目未查证）」**。

| clang 真实版多做的 | 为什么可以砍 |
|---|---|
| `ParseImplicitTemplateId` + `Sema::ActOnExplicitTemplateArgumentList` 的**语境消解**（问符号表"这个名字是不是模板"） | Parser 不持 Sema 活句柄（红线 1 的取舍，见 learn/48），早期只能用**结构判据**：`名字 < 类型列表 > 紧跟 (`。这条路后来被 B31/B32/B35/B36 追打四轮才补上"只读查表 + 试探"（learn/45 / learn/46） |
| `template` 消歧关键字（`p->template f<int>(x)`） | 罕见语法，且只在"成员名恰好与模板名同名"时才需要；教学编译器先不管 |
| 显式实参的**默认实参填充** | 模板形参表里没有"默认值"这个槽位（形参只有名字 + 可选非类型类型）；要支持得先扩形参表示 |
| 显式实参的**隐式转换** | 本项目要求形态精确匹配（learn/18 记为已知简化）；转换判定与 [temp.deduct] 的可表示性判据是同一件事，但两条路的判据还没收口 |
| 显式实参的**生存期/存储期**语义 | 教学编译器不做 ODR 与存储期分析 |
| 错误恢复（消歧失败后按比较表达式继续） | 本项目用"存档 + 回滚"实现（`m_pos = saved` + try/catch），后来升级为 `TokenCheckpoint` RAII 存档点（B32，learn/45）——**回滚机制换了，消歧判据没换** |

## 演进记录

| 日期 | 改了什么 | 为什么 |
|---|---|---|
| 2026-11-02 | 按四段式文档标准补齐骨架：能力清单 / 真实版多做了什么 / 演进记录 | 规范见 docs/constraints/06 |
| 2026-08-10 | S3 落地：`VarExpr::explicitTemplateArgs` 承载显式实参，sema 在 `inferCall` 取用 | template-id 出现在 `(` 之前，CallExpr 还不存在 |
| 2026-09-16 | 模板机制主线收官：显式实参的形态校验随 tagged 实参一起落地 | `Buf<int>` 与 `Buf<4>` 需要同一套 kind 判定（learn/18） |
| 2026-09-21 | 试探-回滚升级为 RAII 存档点（TokenCheckpoint，B32） | 旧回滚只撤 `m_pos` 不撤日志 ⇒ 基线里凭空多出 14 行幽灵行（learn/45） |
| 2026-10-23 | 模板名登记表（learn/46，B31）：消歧第一层闸"是不是模板名" | `a<b>(c)` 全变量比较链被误判成模板调用 ⇒ 拒收合法程序 |
| 2026-11-02 | 名字可见性下沉 Sema（B37/B38，learn/48 / learn/49） | 限定模板名 `N::B<...>` 曾绑到全局 `B`（B36）——作用域是名字身份的一部分 |
