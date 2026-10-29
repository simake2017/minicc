# 08 ministl —— 配套标准库（双栖容器库）

## 定位

| 项 | 内容 |
|---|---|
| 属性 | 不属于编译器本体，是"用 minicc 能写什么"的验收场 |
| 输入 → 输出 | 头文件库：同一份源码，宿主编译器（clang++-18）与 minicc 都能编 |
| 对照 | libc++（map/set 的红黑树全移植 `__tree` 算法） |
| 权威文档 | ministl/README.md |

## 核心机制与判据

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

## 代码地图

| 文件 | 职责 |
|---|---|
| ministl/include/ministl/*.h | 全部头文件库（map.h / set.h / detail/rb_tree.h / …） |
| ministl/include/ministl.h | 汇总头 |
| ministl/tests/ | gtest 测试（宿主编译器跑） |

## 测试入口

- ministl/tests/：map 16 例 + set 13 例，均含 **5000 步随机操作与 std 对拍**
  （mt19937 插入/删除/operator[]，逐键比对 + 红黑不变量校验），ASan+UBSan 全绿
- 主项目 tests/stl/：minicc 侧的编译+运行验证

## 学习文档索引

| 文档 | 内容 |
|---|---|
| ministl/README.md | 容器清单、双栖机制、路线图 |
| docs/learn/37 | __MINICC__ 内建宏（双栖开关的地基） |
