#pragma once
// =============================================================================
// ministl/set.h —— 有序唯一键容器
// =============================================================================
// 学习要点：
//   1. 【set 是 map 的退化版】键即值。同一棵红黑树（detail/rb_tree.h），
//      适配器只改两样：值类型从 pair<const Key,T> 退成 Key；
//      键萃取从"取 .first"退成"取自身"。这是 libc++ __tree 分层的
//      直接体现——加一个新有序容器的工作量 = 写萃取器 + 转发接口。
//   2. 【迭代器是只读的】set 的 iterator 解引用出来是 const Key&。
//      键的位置由树的排序不变量决定，允许改键等于允许破坏排序——
//      所以 set 的 iterator 与 const_iterator 实际上是同一种东西。
//   3. 【没有 operator[] / at】set 没有"键 → 值"的映射语义，
//      查不到就是查不到（find 返回 end()），不存在"默认构造一个填进去"。
// =============================================================================

#include "detail/rb_tree.h"

#include <initializer_list>

namespace ministl {

template <typename Key, typename Compare = less<Key>,
          typename Alloc = allocator<Key>>
class set {
public:
    using key_type        = Key;
    using value_type      = Key;
    using key_compare     = Compare;
    using allocator_type  = Alloc;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference       = const value_type&;   // ★ 只读：改键会破坏排序不变量
    using const_reference = const value_type&;
    using pointer         = const value_type*;
    using const_pointer   = const value_type*;
    // iterator 与 const_iterator 同为只读迭代器（std::set 同款设计）
    using iterator        = detail::rb_iterator<value_type, const value_type&, const value_type*>;
    using const_iterator  = detail::rb_iterator<value_type, const value_type&, const value_type*>;

private:
    // 键萃取：键即值，原样返回（静态成员函数而非 operator()，minicc 友好）
    struct identity_key {
        static const Key& get(const value_type& v) { return v; }
    };

    using tree_type = detail::rb_tree<Key, value_type, identity_key, Compare>;
    tree_type tree_;

public:
    // ── 构造 / 析构 ─────────────────────────────────────────────────────────
    set() : tree_() {}
    explicit set(const Compare& comp) : tree_(comp) {}

    set(std::initializer_list<value_type> il) : tree_() {
        for (const auto& v : il) insert(v);
    }

    set(const set& other) : tree_(other.key_comp()) {
        for (const auto& v : other) insert(v);
    }

    ~set() = default;

    set& operator=(const set& other) {
        if (this != &other) {
            clear();
            for (const auto& v : other) insert(v);
        }
        return *this;
    }

    // ── 容量 ────────────────────────────────────────────────────────────────
    bool      empty() const { return tree_.empty(); }
    size_type size()  const { return tree_.size(); }

    // ── 迭代 ────────────────────────────────────────────────────────────────
    iterator       begin()        { return tree_.begin(); }
    const_iterator begin()  const { return tree_.begin(); }
    const_iterator cbegin() const { return tree_.cbegin(); }
    iterator       end()          { return tree_.end(); }
    const_iterator end()    const { return tree_.end(); }
    const_iterator cend()   const { return tree_.cend(); }

    // ── 修改 ────────────────────────────────────────────────────────────────
    // 返回 {指向该值的迭代器, 是否新插入}。值已存在 ⇒ 不动（set 唯一性语义）
    pair<iterator, bool> insert(const value_type& v) { return tree_.insert_unique(v); }
    pair<iterator, bool> insert(value_type&& v)       { return tree_.insert_unique(ministl::move(v)); }

    // set 的 iterator 与 const_iterator 同类型，只留一个 erase（收 const_iterator，
    // 与 std::set 的 erase(const_iterator) 对齐）
    size_type erase(const Key& k) { return tree_.erase_key(k); }
    iterator  erase(const_iterator it) { return tree_.erase(it); }

    void clear() { tree_.clear(); }

    // ── 查找 ────────────────────────────────────────────────────────────────
    iterator       find(const Key& k)       { return tree_.find(k); }
    const_iterator find(const Key& k) const { return tree_.find(k); }
    size_type      count(const Key& k) const { return find(k) == end() ? 0 : 1; }

    key_compare key_comp() const { return tree_.key_comp(); }

    void swap(set& o) { tree_.swap(o.tree_); }
};

// 非成员 swap，让 using ministl::swap; 能捞到它（与 utility.h 的约定一致）
template <typename K, typename C, typename A>
void swap(set<K, C, A>& a, set<K, C, A>& b) {
    a.swap(b);
}

}  // namespace ministl
