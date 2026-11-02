# 08 ministl —— 配套标准库（双栖容器库）

- 读者：想看"用 minicc 能写什么"的开发者
- 审法：双栖头文件必须同时过宿主编译器与 minicc；不得在 -I 目录造叫 cstddef 的文件
- 名词版本：v1

## 变更记录

| 日期 | 版本 | 改了哪条 |
|---|---|---|
| 2026-11-02 | v1 | 按 tech-doc 骨架补齐：诊断类别 / Out of Scope / 观测点 |

## §1 管什么（定位）

| 项 | 内容 |
|---|---|
| 属性 | 不属于编译器本体，是"用 minicc 能写什么"的验收场 |
| 输入 → 输出 | 头文件库：同一份源码，宿主编译器（clang++-18）与 minicc 都能编 |
| 对照 | libc++（map/set 的红黑树全移植 `__tree` 算法） |
| 权威文档 | ministl/README.md |

## §2 关键判据（详细设计）

- **双栖**：`#ifdef __MINICC__` 分支——minicc 侧走自供桩头（引号 include 完整
  路径，**不在 -I 目录造叫 `cstddef` 的文件**，会连 clang 一起劫持），宿主侧走
  系统标准库。宏 `__MINICC__` 由预处理器内建注入（业务 01）。
- **分层抽取**（对齐 libc++ `__tree`）：树核心 `detail/rb_tree.h` 只认存储类型 T、
  键域 Key、键萃取 KeyOf；map/set 是薄适配器。**删除修正只存在一份**——那种
  "5000 步随机操作才炸一次"的代码存在两份就是两个 bug 温床。
- **双栖约束下的取舍**：KeyOf 是**静态成员函数**而非 operator()（minicc 不
  支持重载调用运算符）；set 的迭代器只读（改键破坏排序不变量，std 同款）。
- **可解释性**：每个容器都能从 minicc 的模板/类/继承机制逐层解释回去——这是它
  存在于学习项目里的根本理由。
- **覆盖率**：vector/list/智能指针/function/algorithm/ranges/map/set；
  缺 string；`initializer_list` 文法与 `throw` 是 parser 层真缺口（README 路线图）。

## §3 诊断类别（异常处理）

**本模块不产生诊断文案**——ministl 是**纯头文件库**，没有自己的编译器前端；
它的错误全部由宿主编译器（clang++-18）或 minicc 自己报出，归属各自模块卡：

| # | 触发情形 | 诊断文案 | 可重试 |
|---|---|---|---|
| 1 | 用 minicc 编译 ministl 源码时，名字解析失败 | 归属 03 解析（`[Parse Error]`）与 04 语义（`[Semantic Error]`）——见那两份模块卡的 §3 | 否 |
| 2 | 用 minicc 编译 ministl 源码时，模板实参/特化不匹配 | 归属 05 模板域的 §3（`[Semantic Error]`） | 否 |
| 3 | 用宿主编译器编译 ministl 时报的任何错 | 归属 clang++-18，本项目不复制其诊断文案 | 否 |

★ 本项目**不复制 clang 的诊断文案**到 ministl 文档里——那会造成"两处真相"。

## §4 Out of Scope（本模块不做的事）

- **缺 `std::string`**：容器清单里没有 string（ministl/README.md 的路线图登记）。
- **`initializer_list` 文法与 `throw` 是 parser 层真缺口**：这两条是 minicc 侧的语法缺口（不是 ministl 的缺陷），挡住了标准库容器的通用写法——ministl README「现状」一节已登记路线图。
- **不做 `operator()` 重载调用**：minicc 不支持重载调用运算符——这是**双栖约束下的取舍**：rb_tree 的键萃取 `KeyOf` 因此只能是**静态成员函数**（`static const Key& get(const T&)`）而不是 `operator()`。
- **set 的迭代器只读**：`iterator` 与 `const_iterator` 同型（改键会破坏排序不变量，std 同款设计）。
- **不做 `std::string_view` / `std::span` / `std::format` / `std::ranges` 的替代品**（ranges 有，format 没有）。
- **不在 `-I` 目录造叫 `cstddef` 的桩头**：桩头用引号 include 完整路径——否则会连 clang 一起劫持（宿主编译器也会去 `-I` 目录找同名头）。

## §5 观测点（日志键）

**本模块不打日志**——ministl 是头文件库，没有自己的运行时输出。

★ 与它相关的日志来自**编译它的那个编译器**：

| 时机 | 打的键 | 归属 |
|---|---|---|
| 双栖开关的求值（`#ifdef __MINICC__`） | `[pp]` / `[sema:targ]` | 01 预处理 / 04 语义 |
| 宿主侧 gtest 的断言输出 | （gtest 自己的输出，不属于本项目日志体系） | 宿主工具链 |

## §6 代码地图（落点）

| 文件 | 职责 |
|---|---|
| ministl/include/ministl/*.h | 全部头文件库（map.h / set.h / detail/rb_tree.h / …） |
| ministl/include/ministl.h | 汇总头 |
| ministl/tests/ | gtest 测试（宿主编译器跑） |

## §7 测试入口

- ministl/tests/：map 16 例 + set 13 例，均含 **5000 步随机操作与 std 对拍**
  （mt19937 插入/删除/operator[]，逐键比对 + 红黑不变量校验），ASan+UBSan 全绿
- 主项目 tests/stl/：minicc 侧的编译+运行验证

## §8 深入读

| 文档 | 内容 |
|---|---|
| ministl/README.md | 容器清单、双栖机制、路线图 |
| docs/learn/37 | __MINICC__ 内建宏（双栖开关的地基） |
