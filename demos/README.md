# demos —— 可运行的编译器示例

> 与 `tests/` 的分工：**tests 钉住行为**（带诊断码、是回归红线，改动前先看它），
> **demos 给人看**（短小，每个只演示一个理论点，配可观测的日志线索）。

> - 读者：想知道"某个模板/继承/控制流特性在 minicc 里长什么样"的人；
>   想给 minicc 加一个演示例子的人
> - 审法：每个 demo 的头部注释写清四件事 —— **演示什么 / 理论点（带标准章节号） /
>   看什么日志 / 预期退出码**；跑 `./demos/run.sh` 全绿即通过
> - 名词版本：v1（demo 清单与下表随特性增删同步）

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 补头部三行与变更记录（按 `docs/constraints/06` 技术文档骨架）；补 Out of Scope 与观测点；正文未改写 |

## 怎么跑

```bash
./demos/run.sh              # 跑全部
./demos/run.sh tmpl         # 只跑某个子目录
./demos/run.sh -v tmpl/01   # 失败时显示完整编译日志
```

每个 demo 的约定：**编译成功且运行退出码 = 0**。
（非 0 表示该 demo 内部的断言失败，具体码值的含义写在 demo 源码里。）

单个 demo 也可以手工跑，顺便看编译日志：

```bash
./build-linux/minicc demos/tmpl/01_deduction.cpp -S -o /tmp/demo01.s
```

## 索引

| demo | 演示什么 | 理论点 | 看什么日志 |
|---|---|---|---|
| `core/01_control_flow` | if / while 降级成什么形状 | 控制流 → 基本块 + 回边 | 生成的 `.s` 里的 `while_begin_N` / `jmp` 回边 |
| `core/02_preprocessor` | 宏替换与条件编译 | 预处理是纯文本阶段 | `-E` 看展开结果；`[pp]` 日志看宏调用点 |
| `core/03_infer_branches` | 表达式定型 15 条分支各喂一个数据 | [expr] 语法制导翻译（综合属性） | `[infer]` / `[resolve]` / `[member]` / `[index]` 逐支对应 |
| `core/04_decl_kinds` | 顶层声明 7 种形态各喂一个 | 声明分派（Decl::getKind + dyn_cast） | `[register] …` / `[namespace] enter …` 逐支对应 |
| `oop/01_vtable` | 同一调用点、运行期决定函数体 | [class.virtual] + Itanium vtable | `.s` 里的 `movq (%rax),%rax` 三部曲 |
| `oop/02_dynamic_cast` | 运行期沿继承链找目标类型 | [expr.dynamic.cast] | `callq __minicc_dynamic_cast` |
| `oop/03_multiple_inheritance` | 一个对象、多个基类子对象与指针调整 | Itanium ABI §2.4（thunk） | `.s` 里的 `thunk` 与 offset-to-top |
| `tmpl/01_deduction` | 从调用实参反推出 T | [temp.deduct.call] | `[deduction] P=T A=int ⇒ T := int` |
| `tmpl/02_reference_collapsing` | 同一个 `T&&` 折叠出三种类型 | [dcl.ref]/6 | `[subst] ★ Reference collapsing: int&&& → int&` |
| `tmpl/03_specialization` | 主模板 / 偏特化 / 全特化三路择优 | [temp.class.spec] | `[spec:select] ① ② ③` 三步裁决 |
| `tmpl/04_partial_order` | 两个函数模板都能匹配时选更特殊的 | [temp.func.order] | `[overload] partial ordering check...` |
| `tmpl/05_sfinae_void_t` | 用 void_t 探测成员是否存在 | [temp.deduct]/8 | `[sfinae] void_t<2 个实参> —— 逐个替换 + 求值探测` |
| `tmpl/06_nttp` | 把值当模板参数 | [temp.param]/4 | `[instantiate:name] 'Size<4>' → 'Size_4'` |
| `tmpl/07_alias_and_ctad` | 别名模板解糖 + CTAD 推导 | [temp.alias] / [dcl.type.class.deduct] | `[ctad] ▶ m MyPtr(int*)` |

每个 demo 的源码头部都写清了：**演示什么 / 理论点 / 看什么日志 / 预期退出码**。

## 自己加一个

1. 建文件 `demos/<分类>/NN_<名字>.cpp`
2. 头部注释写四件事：演示什么、理论点（带标准章节号）、看什么日志、预期退出码
3. 用它自己的退出码做断言：`if (实际 != 期望) return <诊断码>;`
4. 跑 `./demos/run.sh` —— 它会自动发现新文件

> 写 demo 时请**实际跑一遍**。minicc 仍有一些语法边界（如函数形参不能省名、
> `if` 条件里不能直接放模板 id），这些边界会随时补进这里的 demo 注释中。


---

## Out of Scope（这里不负责的事）

| 不负责 | 原因 / 去哪看 |
|---|---|
| **回归测试** | demos 是"给人看"的短例子，**不做回归红线**；行为由 `tests/` 钉住（`logdiff.sh` 逐字节比对，见 `docs/learn/29` 与 `docs/BUGS.md`） |
| **每个语言特性的完整讲解** | demo 只演示**一个**理论点；要系统学请看 `docs/learn/NN-*.md`（每篇点名标准章节 + clang 源码对照） |
| **覆盖全部特性** | demos 是**精选**而非**全集** —— 覆盖面看 `README.md` 的"支持特性"表与 `docs/business/**` |
| **第三方库的使用示范** | 配套标准库是 `ministl/`（独立 gtest + ASan 构建），不是 demo |

## 观测点（怎么自己验一遍）

★ demo 的观测手段是**编译日志 + 运行退出码**：

| 想看什么 | 命令 | 关注什么 |
|---|---|---|
| 跑全部 demo | `./demos/run.sh` | 每个 demo 的退出码必须为 0；任一非 0 即该 demo 内部断言失败 |
| 只跑一个分类 | `./demos/run.sh tmpl` | 同上 |
| 失败时看完整编译日志 | `./demos/run.sh -v tmpl/01` | minicc 的**全阶段中文日志**（阶段 0~6 横幅 + 逐条推导） |
| 看某个 demo 的源码声明 | `head -20 demos/tmpl/01_deduction.cpp` | 头部四件事：演示什么 / 理论点 / 看什么日志 / 预期退出码 |
| 对照 clang 的行为 | `clang++-18 -std=c++20 demos/tmpl/01_deduction.cpp -o /tmp/d1 && /tmp/d1` | 退出码与关键输出应与 minicc 一致（负例则是两边都非 0） |
