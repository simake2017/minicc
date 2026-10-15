// =============================================================================
// ministl map 测试 —— 红黑树有序映射
// =============================================================================
// 策略：手工用例钉住每条规则（中序有序 / 不覆盖插入 / operator[] 语义 /
// 三种删除形态 / 自定义比较器 / 哨兵边界），再用【随机操作序列与 std::map
// 交叉核对】当 oracle —— 上千步插入/删除/查找后两者必须完全一致。
// 这同时就是红黑不变量的检验：任何一次旋转/染色写错，交叉核对必然失配。
// =============================================================================

#include <ministl/map.h>

#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using ministl::map;
using ministl::pair;

// 把 ministl map 的内容拍平成 vector，便于和 std::map 直接比对
template <typename K, typename V>
static std::vector<std::pair<K, V>> dump(const map<K, V>& m) {
    std::vector<std::pair<K, V>> out;
    for (const auto& kv : m) out.emplace_back(kv.first, kv.second);
    return out;
}

template <typename K, typename V>
static std::vector<std::pair<K, V>> dump(const std::map<K, V>& m) {
    return std::vector<std::pair<K, V>>(m.begin(), m.end());
}

TEST(Map, InsertAndIterateInOrder) {
    map<int, int> m;
    // 故意按非序插入（含逆序与交错），中序遍历必须升序
    for (int k : {5, 3, 8, 1, 4, 7, 9, 2, 6}) m.insert({k, k * 10});
    ASSERT_EQ(m.size(), 9u);
    int expect = 1;
    for (const auto& kv : m) {
        EXPECT_EQ(kv.first, expect);
        EXPECT_EQ(kv.second, expect * 10);
        ++expect;
    }
}

TEST(Map, InsertDoesNotOverwrite) {
    map<std::string, int> m;
    auto r1 = m.insert({"a", 1});
    EXPECT_TRUE(r1.second);
    auto r2 = m.insert({"a", 999});   // 键已存在 ⇒ 不动旧值
    EXPECT_FALSE(r2.second);
    EXPECT_EQ(r2.first->second, 1);
    EXPECT_EQ(m.at("a"), 1);
    EXPECT_EQ(m.size(), 1u);
}

TEST(Map, OperatorBracketInsertsDefault) {
    map<std::string, int> m;
    m["x"] = 42;                       // 不存在 ⇒ 默认构造 0 再赋 42
    EXPECT_EQ(m.at("x"), 42);
    EXPECT_EQ(m["y"], 0);              // 读不存在键 ⇒ 插入默认值
    EXPECT_EQ(m.size(), 2u);
}

TEST(Map, FindAndCount) {
    map<int, int> m;
    for (int i = 0; i < 100; ++i) m.insert({i, i * i});
    EXPECT_EQ(m.find(50)->second, 2500);
    EXPECT_EQ(m.find(-1), m.end());
    EXPECT_EQ(m.find(100), m.end());
}

// 删除的三种结构形态：叶子 / 单子 / 双子（后继顶替），各钉一个
TEST(Map, EraseLeaf) {
    map<int, int> m{{1, 1}, {2, 2}, {3, 3}};
    EXPECT_EQ(m.erase(1), 1u);
    EXPECT_EQ(dump(m), (std::vector<std::pair<int, int>>{{2, 2}, {3, 3}}));
}

TEST(Map, EraseOneChild) {
    map<int, int> m{{1, 1}, {2, 2}, {3, 3}};
    EXPECT_EQ(m.erase(1), 1u);
    EXPECT_EQ(m.erase(2), 1u);
    EXPECT_EQ(dump(m), (std::vector<std::pair<int, int>>{{3, 3}}));
}

TEST(Map, EraseTwoChildren) {
    map<int, int> m;
    for (int i = 1; i <= 9; ++i) m.insert({i, i * 100});
    EXPECT_EQ(m.erase(5), 1u);         // 根有两子 ⇒ 走后继顶替路径
    ASSERT_EQ(m.size(), 8u);
    int expect = 1;
    for (const auto& kv : m) {
        if (expect == 5) ++expect;
        EXPECT_EQ(kv.first, expect);
        EXPECT_EQ(kv.second, expect * 100);
        ++expect;
    }
}

TEST(Map, EraseViaIteratorReturnsNext) {
    map<int, int> m{{1, 1}, {2, 2}, {3, 3}};
    auto it = m.find(2);
    auto next = m.erase(it);
    ASSERT_NE(next, m.end());
    EXPECT_EQ(next->first, 3);
    EXPECT_EQ(m.size(), 2u);
}

TEST(Map, EraseMinAndMax) {
    map<int, int> m;
    for (int i = 0; i < 20; ++i) m.insert({i, i});
    EXPECT_EQ(m.erase(0), 1u);         // 最小：leftmost 缓存必须更新
    EXPECT_EQ(m.erase(19), 1u);        // 最大
    EXPECT_EQ(m.begin()->first, 1);
    auto it = m.end();
    --it;                              // --end() 走 tree_prev
    EXPECT_EQ(it->first, 18);
}

TEST(Map, EraseAllSequentially) {
    map<int, int> m;
    for (int i = 0; i < 64; ++i) m.insert({i, i});
    for (int i = 0; i < 64; ++i) ASSERT_EQ(m.erase(i), 1u);
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.begin(), m.end());
}

TEST(Map, CustomComparatorDescending) {
    map<int, int, ministl::greater<int>> m;
    for (int k : {4, 1, 9, 7, 3}) m.insert({k, k});
    const int want[] = {9, 7, 4, 3, 1};   // 按 greater 的"升序" = 数值降序
    int i = 0;
    for (const auto& kv : m) {
        ASSERT_LT(i, 5);
        EXPECT_EQ(kv.first, want[i]);
        ++i;
    }
    EXPECT_EQ(i, 5);
}

TEST(Map, StringKeys) {
    map<std::string, int> m;
    m["banana"] = 2;
    m["apple"]  = 1;
    m["cherry"] = 3;
    auto it = m.begin();               // 字典序
    EXPECT_EQ(it->first, "apple"); ++it;
    EXPECT_EQ(it->first, "banana"); ++it;
    EXPECT_EQ(it->first, "cherry");
}

TEST(Map, InitListCopyAssignClear) {
    map<int, int> a{{1, 10}, {2, 20}};
    map<int, int> b = a;               // 拷贝：逐键重建一棵树
    EXPECT_EQ(dump(a), dump(b));
    b[3] = 30;
    EXPECT_EQ(a.size(), 2u);           // 深拷贝，互不影响
    EXPECT_EQ(b.size(), 3u);
    map<int, int> c;
    c = a;                             // 拷贝赋值
    EXPECT_EQ(dump(c), dump(a));
    c.clear();
    EXPECT_TRUE(c.empty());
    EXPECT_EQ(c.begin(), c.end());
}

TEST(Map, AtThrowsOnMissing) {
    map<int, int> m;
    EXPECT_THROW(m.at(7), std::out_of_range);
    m[7] = 1;
    EXPECT_NO_THROW(m.at(7));
}

// ── oracle 交叉核对：随机操作序列与 std::map 逐步比对 ──────────────────────

TEST(Map, CrossCheckAgainstStdMap) {
    std::mt19937 rng(42);              // 固定种子，失败可复现
    std::uniform_int_distribution<int> key_dist(0, 499);
    std::uniform_int_distribution<int> op_dist(0, 99);   // <55 插, <75 删, 其余查
    std::uniform_int_distribution<int> val_dist(1, 10000);

    map<int, int> mine;
    std::map<int, int> oracle;
    for (int step = 0; step < 5000; ++step) {
        int key = key_dist(rng);
        int op  = op_dist(rng);
        if (op < 55) {
            int v = val_dist(rng);
            auto rm = mine.insert({key, v});
            auto ro = oracle.insert({key, v});
            EXPECT_EQ(rm.second, ro.second) << "step " << step;
        } else if (op < 75) {
            EXPECT_EQ(mine.erase(key), oracle.erase(key)) << "step " << step;
        } else {
            auto im = mine.find(key);
            auto io = oracle.find(key);
            EXPECT_EQ(im == mine.end(), io == oracle.end()) << "step " << step;
            if (im != mine.end()) EXPECT_EQ(im->second, io->second) << "step " << step;
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
TEST(Map, DescendingInsertionKeepsBalance) {
    map<int, int> m;
    for (int i = 999; i >= 0; --i) m.insert({i, i});
    EXPECT_EQ(m.size(), 1000u);
    EXPECT_EQ(m.begin()->first, 0);
    auto it = m.end(); --it;
    EXPECT_EQ(it->first, 999);
    for (int i = 0; i < 1000; ++i) ASSERT_EQ(m.find(i)->second, i);
}
