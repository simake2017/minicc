// =============================================================================
// ministl set 测试 —— 红黑树有序唯一键集合
// =============================================================================
// 策略：与 test_map 同款——手工用例钉住每条规则（中序有序 / 重复插入被忽略 /
// 三种删除形态 / 只读迭代器 / 自定义比较器 / 哨兵边界），再用【随机操作序列
// 与 std::set 交叉核对】当 oracle。树核心（detail/rb_tree.h）与 map 共用，
// map 的 5000 步对拍已钉死其语义，这里钉的是 set 适配器层：
// 键萃取（identity）、只读迭代器、insert/erase/find 的转发正确性。
// =============================================================================

#include <ministl/set.h>

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using ministl::set;

// 把 ministl set 的内容拍平成 vector，便于和 std::set 直接比对
// （手写循环：rb_iterator 不是 std 输入迭代器，走不了 vector 的区间构造）
template <typename K>
static std::vector<K> dump(const set<K>& s) {
    std::vector<K> out;
    for (const K& v : s) out.push_back(v);
    return out;
}

template <typename K>
static std::vector<K> dump(const std::set<K>& s) {
    return std::vector<K>(s.begin(), s.end());
}

TEST(Set, InsertAndIterateInOrder) {
    set<int> s;
    for (int k : {5, 3, 7, 1, 4, 6, 8}) s.insert(k);
    EXPECT_EQ(dump(s), std::vector<int>({1, 3, 4, 5, 6, 7, 8}));
    EXPECT_EQ(s.size(), 7u);
    EXPECT_EQ(*s.begin(), 1);
    auto it = s.end();
    --it;
    EXPECT_EQ(*it, 8);
}

TEST(Set, InsertDuplicateIgnored) {
    set<int> s;
    auto r1 = s.insert(42);
    EXPECT_TRUE(r1.second);
    auto r2 = s.insert(42);          // 重复：不插入，指向已存在的那个
    EXPECT_FALSE(r2.second);
    EXPECT_EQ(r1.first, r2.first);
    EXPECT_EQ(s.size(), 1u);
    EXPECT_EQ(s.count(42), 1u);
    EXPECT_EQ(s.count(43), 0u);
}

TEST(Set, EraseLeaf) {
    set<int> s;
    for (int k : {10, 5, 15}) s.insert(k);
    EXPECT_EQ(s.erase(5), 1u);
    EXPECT_EQ(dump(s), std::vector<int>({10, 15}));
    EXPECT_EQ(s.erase(5), 0u);       // 已不存在
}

TEST(Set, EraseOneChild) {
    set<int> s;
    for (int k : {10, 5, 15, 12}) s.insert(k);
    EXPECT_EQ(s.erase(15), 1u);      // 15 只有左孩子 12
    EXPECT_EQ(dump(s), std::vector<int>({5, 10, 12}));
}

TEST(Set, EraseTwoChildren) {
    set<int> s;
    for (int k : {20, 10, 30, 5, 15, 25, 35}) s.insert(k);
    EXPECT_EQ(s.erase(10), 1u);      // 10 有两子：中序后继 15 顶上
    EXPECT_EQ(dump(s), std::vector<int>({5, 15, 20, 25, 30, 35}));
}

TEST(Set, EraseViaIteratorReturnsNext) {
    set<int> s;
    for (int k : {1, 2, 3, 4, 5}) s.insert(k);
    auto it = s.find(3);
    auto nx = s.erase(it);           // 返回值 = 被删元素的下一个
    EXPECT_EQ(*nx, 4);
    EXPECT_EQ(dump(s), std::vector<int>({1, 2, 4, 5}));
}

TEST(Set, EraseMinAndMax) {
    set<int> s;
    for (int k : {3, 1, 2}) s.insert(k);
    s.erase(1);                      // 最小：begin 必须移交给 2（end_.parent 缓存）
    EXPECT_EQ(*s.begin(), 2);
    s.erase(3);                      // 最大
    EXPECT_EQ(dump(s), std::vector<int>({2}));
}

TEST(Set, EraseAllSequentially) {
    set<int> s;
    for (int i = 0; i < 100; ++i) s.insert(i);
    for (int i = 0; i < 100; ++i) {  // 从小到大删：最左缓存每次都要重算
        ASSERT_EQ(s.erase(i), 1u);
        ASSERT_EQ(s.size(), 99u - static_cast<unsigned>(i));
    }
    EXPECT_TRUE(s.empty());
}

TEST(Set, CustomComparatorDescending) {
    set<int, ministl::greater<int>> s;
    for (int k : {1, 9, 4, 7, 3}) s.insert(k);
    const int want[] = {9, 7, 4, 3, 1};
    int i = 0;
    for (int v : s) EXPECT_EQ(v, want[i++]);
}

TEST(Set, StringKeys) {
    set<std::string> s;
    s.insert("pear");
    s.insert("apple");
    s.insert("fig");
    auto d = dump(s);
    ASSERT_EQ(d.size(), 3u);
    EXPECT_EQ(d[0], "apple");
    EXPECT_EQ(d[1], "fig");
    EXPECT_EQ(d[2], "pear");
}

TEST(Set, InitListCopyAssignClear) {
    set<int> a = {3, 1, 2};
    EXPECT_EQ(dump(a), std::vector<int>({1, 2, 3}));
    set<int> b(a);                   // 拷贝：逐元素重新插入（深拷贝）
    EXPECT_EQ(dump(b), dump(a));
    set<int> c;
    c = a;                           // 拷贝赋值
    EXPECT_EQ(dump(c), dump(a));
    c.clear();
    EXPECT_TRUE(c.empty());
    EXPECT_EQ(a.size(), 3u);         // a 不受影响
}

// mt19937 固定种子，失败可复现。上千步插入/删除/查找后必须与 std::set 一致——
// 树核心的红黑不变量由 test_map 的同款对拍钉死，这里钉适配器转发层
TEST(Set, CrossCheckAgainstStdSet) {
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> key_dist(0, 499);
    std::uniform_int_distribution<int> op_dist(0, 99);   // <55 插, <75 删, 其余查

    set<int> mine;
    std::set<int> oracle;
    for (int step = 0; step < 5000; ++step) {
        int key = key_dist(rng);
        int op  = op_dist(rng);
        if (op < 55) {
            auto rm = mine.insert(key);
            auto ro = oracle.insert(key);
            EXPECT_EQ(rm.second, ro.second) << "step " << step;
        } else if (op < 75) {
            EXPECT_EQ(mine.erase(key), oracle.erase(key)) << "step " << step;
        } else {
            EXPECT_EQ(mine.count(key), oracle.count(key)) << "step " << step;
        }
        ASSERT_EQ(mine.size(), oracle.size()) << "step " << step;
    }
    EXPECT_EQ(dump(mine), dump(oracle));

    // 前半键全删后再核对一次（批量触发删除修复路径）
    for (int k = 0; k < 250; ++k) {
        mine.erase(k);
        oracle.erase(k);
    }
    EXPECT_EQ(dump(mine), dump(oracle));
    EXPECT_EQ(mine.size(), oracle.size());
}

// 逆序插入是最坏情况之一（BST 退化成链的输入顺序），单独钉住
TEST(Set, DescendingInsertionKeepsBalance) {
    set<int> s;
    for (int i = 999; i >= 0; --i) s.insert(i);
    EXPECT_EQ(s.size(), 1000u);
    EXPECT_EQ(*s.begin(), 0);
    auto it = s.end();
    --it;
    EXPECT_EQ(*it, 999);
    for (int i = 0; i < 1000; ++i) ASSERT_EQ(s.count(i), 1u);
}
