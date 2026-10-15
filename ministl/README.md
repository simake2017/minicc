# ministl —— 一个用来读懂标准库的迷你 STL

## 这是什么

一个从零手写的 C++20 容器 / 智能指针 / ranges 库，约 5000 行，只依赖编译器。
它不是 std 的替代品，是一次**逆向工程**：把标准库那些看起来理所当然的设计，
还原成"如果不这么做会出什么问题"。

它和同仓库的 `minicc` 编译器构成一个飞轮：

```text
ministl 用宿主编译器（GCC/Clang）写 → 跑通测试 → 成为 minicc 的验证用例
                                        ↓
                          minicc 能编译它 → 说明 minicc 支持了这些语言特性
```

**现状（2026-10）**：飞轮第二段尚未闭合。用 `./minicc` 编译 `map.h` 探针，
第一堵墙是系统头依赖链：`<cstddef>` → `<initializer_list>` / `<stdexcept>` →
`<type_traits>`（ministl 的 traits 是有意转发 `std::` 的）。即使垫桩过预处理，
`initializer_list` 文法、`throw`、`<utility>` 转发等还是 parser/语义层的真缺口。
路线图（第一步 ✅ 已完成）：minicc 预定义 `__MINICC__`（[cpp.predefined] 扩展，
docs/learn/37）→ 由 ministl 侧按 `__MINICC__` 分支提供自供桩头，逐层闭合。

测试用 GoogleTest，与仓库里 `tests/unit/` 同一套约定（`FetchContent` 拉 v1.14.0，
`gtest_discover_tests` 注册）。代价是 minicc 暂时吃不下这些测试——它连 GTest 的头文件
都过不了，飞轮的第二段要等它追上来才能闭合。这个取舍是明确选的：
**先用上 IDE 的单测体验**，而不是为一个远期目标牺牲每天的开发效率。

## 目录

| 文件 | 内容 |
|---|---|
| `type_traits.h` | 转发 `std::` 的 traits + 自研 `is_memmovable` |
| `utility.h` | move / forward / move_if_noexcept / pair / index_sequence / less 等仿函数 |
| `iterator.h` | 五个迭代器标签、`iterator_adaptor`(CRTP)、reverse/back/insert 迭代器 |
| `memory.h` | allocator、`uninitialized_*`、unique_ptr、shared_ptr、weak_ptr、make_shared |
| `detail/rb_tree.h` | 红黑树核心（哨兵节点 + CLRS 旋转/插入/删除修复，移植 libc++ `__tree`） |
| `vector.h` | 三指针 + EBO 的动态数组 |
| `list.h` | 哨兵节点的双向链表 |
| `unordered_map.h` | 链地址法哈希表 |
| `map.h` | 有序映射（`rb_tree` 适配器：pair 取 `.first` 当键） |
| `set.h` | 有序唯一键集合（`rb_tree` 适配器：键即值，只读迭代器） |
| `hash.h` | MurmurHash3 混合器 + FNV-1a + 各类型特化 |
| `algorithm.h` | copy/find/sort/lower_bound/is_sorted 等 |
| `ranges.h` | 惰性视图与管道组合 |
| `function.h` | 类型擦除（手写 vtable + SBO） |

## 几个关键设计决策

### EBO：为什么 `sizeof(unique_ptr) == 8` 而不是 16

删除器如果是空类（`default_delete`），把它作为**成员**会让对象多占 1 字节，
再经对齐填充直接翻倍成 16。标准做法是让 unique_ptr **私有继承**空删除器——
空基类优化（EBO）把它压成 0 字节。`vector` 里的 allocator 同理，
这就是 `sizeof(vector<int>) == 24`（恰好三个指针）而不是 32 的原因。

测试里有 `CHECK_EQ(sizeof(up), sizeof(void*))` 直接钉死这个结论。

### 三指针布局与倍增扩容

`begin_ / end_ / cap_` 三个指针，而不是"指针 + 大小"。
扩容时容量翻倍，把 n 次 push_back 的总代价摊还成 O(1)。
扩容搬元素时用 `uninitialized_move_if_noexcept`：移动构造是 `noexcept` 就移动，
否则退回拷贝（因为移动到一半抛异常，源对象已经残了，无法回滚）。

测试用一个 `Tracker` 同时数 ctor/move/copy，断言扩容期间 `copy == 0`。

### shared_ptr 的控制块双计数协议

控制块里有 `strong_` 和 `weak_` 两个计数，且 `weak_` 初始为 **1**（不是 0）——
这个 1 代表"强引用组自己还占着一个弱引用名额"。释放顺序必须是：

```text
strong 归零 → dispose() 销毁对象 → 释放 strong 自己占的那个 weak
weak   归零 → 才真正 delete 控制块
```

`weak_ptr::lock()` 不能"先查 expired 再 ++"，那是竞态；
必须 CAS 循环把 strong 从非零值加上去。这是 `weak_ptr` 唯一有难度的地方。

### take_view 的配额券：一个真实的 bug

第一版 `take_view::end()` 是"从 begin 走 n 步"。它有两个问题：
`end()` 变成 O(n)，而且**会重复触发上游谓词**——上游是 `filter_view` 时，
算一次 end() 就要把前 n 个元素重新过滤一遍。实测 filter 被调了 14 次，
而容器里只有 10 个元素。谓词只要有副作用（计数、日志），结果就直接错了。

修法是给迭代器配一张"还能产出几个"的配额券（对应标准库的
`counted_iterator` + `default_sentinel`）。修完 filter 只被调 5 次，
第 6 个元素之后全程未被访问——这才是 take 该有的短路。

细节在于 `operator++`：`--remaining_` 和 `++it_` 必须**分两个 if**。
因为 range-for 收尾是"先 ++ 再比较"，取满 n 个之后还会多调一次 ++，
若那次仍推进上游，就白拉一个元素，把短路收益又抵消掉。

### 视图的持有策略：左值引用，右值搬走

```cpp
template <typename R>
using view_storage_t =
    conditional_t<is_lvalue_reference_v<R>, R, remove_reference_t<R>>;
```

`v | transform(f)` 里的 `v` 是左值，视图只存引用，不拷贝；
`vector<int>{...} | transform(f)` 是右值，视图必须把它**搬进自己内部**，
否则临时对象析构后视图悬空。这一行 `conditional_t` 就是全部奥妙。

### `operator|` 为什么必须放在 `views` 命名空间里

`operator|` 靠 ADL 查找。ADL 只检查实参类型**最内层**的外围命名空间，
**不会向上递归**。闭包类型都定义在 `ministl::ranges::views`，
所以 `operator|` 放在外层 `ministl::ranges` 里是找不到的——这是第一版编译失败的原因。

### map：哨兵节点与删除修正的"重新选边"

红黑树不用 `nullptr` 当 end()，而是放一个哨兵节点 `end_`：
`end_.right` 存根、`end_.parent` 缓存最左节点（即 `begin()`）、
`end_.left` 缓存最右节点。好处是 `begin()/end()` 全是 O(1)，
且最左节点上溯时天然停在哨兵上，`++` 到头自然等于 `end()`，
不需要任何"根的特殊情况"分支。这与 libc++ `__tree` 的布局一致。

删除修正（黑节点被移除后的双重黑消解）最容易写错的点是**情形 2 的 fall-through**：
兄弟染红、双重黑上推给父节点之后，若父节点仍是黑，下一轮处理的"兄弟"在
**另一侧**（方向可能翻转）。旧实现顺着往下走进了为旧方向写的情形 3/4，
树当场损坏——5000 随机操作对拍时表现为"父链断在黑高错乱的节点上"。
修法是照 libc++ 的结构：循环头**每次重新选边**（`w = is_left_child(x) ? ... : ...`），
情形 2 结尾无条件 `continue`，情形 3/4 收敛在 `else` 里以 `break` 结束。

### `detail/rb_tree.h`：一棵树，两个适配器

libc++ 的分层是 `__tree`（裸树 + 哨兵）→ `map`/`set`/`multiset`（适配器）。
照抄这个分层后，新增一个有序容器 = 写键萃取器 + 转发接口。树核心只认三样：

```text
rb_tree<Key, T, KeyOf, Compare>
              │     │      └─ 键比较器（树内所有比较都发生在 Key 域）
              │     └─ 存储类型：map 存 pair<const K,V>，set 存 K
              └─ 键萃取：KeyOf::get(const T&) → const Key&
```

map 的 `first_of_value::get` 取 `.first`，set 的 `identity_key::get` 原样返回——
`set.h` 的全部树相关代码就这一行。两个适配器差异只剩接口语义：
`map` 有 `operator[]`（键不存在先默认构造填进去）与 `at`（越界抛异常），
`set` 的迭代器是只读的（改键会破坏排序不变量，`iterator` 与
`const_iterator` 同为 const 版）。

键萃取用**静态成员函数**而非 `operator()`——minicc 尚未支持重载调用运算符，
这是双栖（宿主可编译 / minicc 可解释）约束下的取舍。迭代器的
可变→只读转换用 C++20 `requires` 约束（const 性挂在 pointee 上，
`remove_const` 必须先剥指针——直接比 `Ptr` 类型会撞
`remove_const_t<const int*> == const int*` 这个经典坑）。

## 构建与测试

```bash
# 配置 + 构建 + 跑全部（googletest 由 FetchContent 自动拉取）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`gtest_discover_tests` 把每个 `TEST()` 注册成独立的 ctest 用例，所以可以按套件筛：

```bash
ctest --test-dir build -R Ranges --output-on-failure   # 只跑 Ranges.*
./build/test_ranges --gtest_filter='Ranges.TakeShortCircuits'   # 单跑一个
```

在 CLion 里：Reload CMake 后每个 `TEST()` 左侧出现绿色三角，点击即单跑该用例；
底部 Test 面板树状查看全部 89 个用例的通过/失败。

验证内存正确性（建议每次改动后都跑）：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan -j && ctest --test-dir build-asan
```

当前状态：7 个测试文件、89 个用例、337 处断言，GCC 10 / Clang 18 双编译器通过，
ASan + UBSan 无告警。

`map.h` 与 `set.h` 的交叉核对用例（`Map.CrossCheckAgainstStdMap` /
`Set.CrossCheckAgainstStdSet`）各用 mt19937 生成 5000 个随机插入/删除/查找
操作，与 `std::map` / `std::set` 逐键对拍——任何一次旋转/染色写错，
对拍必然失配。删除修正的 fall-through bug（情形 2 后方向翻转走错边）
就是它对拍抓出来的（详见下文「map：哨兵节点与删除修正」一节）。

## 尚未实现

- `string`（SSO）、`optional`、`variant`、`span`、`string_view`
- 分配器传播（`allocator_traits::propagate_on_container_move_assignment`）
- `vector` 的 `insert` 未做"就地扩容 + 位移"的优化，当前走的是重建路径
- ranges 只做了单向视图，没有 `join` / `split` / `zip` / 自定义 range 适配
- `unordered_map` 未实现 `node_handle`（extract/merge）
