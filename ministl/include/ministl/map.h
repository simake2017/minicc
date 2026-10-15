#pragma once
// =============================================================================
// ministl/map.h —— 有序关联容器（键 → 值的映射）
// =============================================================================
// 学习要点：
//   1. 【map 只是一层皮】真正的红黑树在 detail/rb_tree.h（与 set 共用，
//      分层对齐 libc++ 的 __tree → map/set）。本文件只做三件事：
//        ① 定值类型 pair<const Key, T>（键不可变，迭代器改键会破坏有序性）
//        ② 告诉树"键从哪取"（first_of_value::get 取 .first）
//        ③ 加 map 专属的接口：operator[] 的下标语义、at 的越界检查
//   2. 【operator[] 的下标语义】键不存在 ⇒ 先默认构造一个 mapped_type
//      插进去，再返回它的引用。所以 operator[] 永远成功，且会【改树】——
//      这就是它不能是 const 成员的原因，也是 "m[k]++ 对不存在的 k 先填 0"
//      的来源。
//   3. 【insert 不覆盖】键已存在 ⇒ 返回 {现有迭代器, false}，旧值不动
//      （与 std::map 一致；想覆盖用 insert_or_assign 或干脆 operator[]）。
//
// v1 边界（有意不做，见 README 设计决策）：
//   - 没有 emplace / piecewise_construct（需要变参完美转发到 pair 内部）
//   - 没有 lower_bound/upper_bound/equal_range（iterator 之外的范围接口）
//   - 比较器直接按成员存储，没用 EBO（unique_ptr 里讲过这个技巧，不重复）
//   - Alloc 模板参数只作接口兼容，分配固定走 allocator<node>（与 list 同款）
// =============================================================================

#include "detail/rb_tree.h"

#include <initializer_list>
#include <stdexcept>

namespace ministl {

template <typename Key, typename T, typename Compare = less<Key>,
          typename Alloc = allocator<pair<const Key, T>>>
class map {
public:
    using key_type        = Key;
    using mapped_type     = T;
    using value_type      = pair<const Key, T>;
    using key_compare     = Compare;
    using allocator_type  = Alloc;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference       = value_type&;
    using const_reference = const value_type&;
    using pointer         = value_type*;
    using const_pointer   = const value_type*;
    using iterator        = detail::rb_iterator<value_type, value_type&, value_type*>;
    using const_iterator  = detail::rb_iterator<value_type, const value_type&, const value_type*>;

private:
    // 键萃取：树核心只认键，pair 的键在 .first。
    // 静态成员函数而非 operator() —— minicc 尚未支持重载调用运算符。
    struct first_of_value {
        static const Key& get(const value_type& v) { return v.first; }
    };

    using tree_type = detail::rb_tree<Key, value_type, first_of_value, Compare>;
    tree_type tree_;

public:
    // ── 构造 / 析构 ─────────────────────────────────────────────────────────
    map() : tree_() {}
    explicit map(const Compare& comp) : tree_(comp) {}

    map(std::initializer_list<value_type> il) : tree_() {
        for (const auto& kv : il) insert(kv);
    }

    map(const map& other) : tree_(other.key_comp()) {
        for (const auto& kv : other) insert(kv);
    }

    ~map() = default;

    map& operator=(const map& other) {
        if (this != &other) {
            clear();
            for (const auto& kv : other) insert(kv);
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
    // 返回 {指向该键的迭代器, 是否新插入}。键已存在 ⇒ 不动旧值（与 std::map 一致）
    pair<iterator, bool> insert(const value_type& v) { return tree_.insert_unique(v); }
    pair<iterator, bool> insert(value_type&& v)       { return tree_.insert_unique(ministl::move(v)); }

    // 下标语义：键不存在 ⇒ 默认构造 mapped_type 插入，返回其引用
    T& operator[](const Key& k) {
        return insert(value_type(k, T())).first->second;
    }

    size_type erase(const Key& k) { return tree_.erase_key(k); }
    iterator  erase(iterator it)  { return tree_.erase(it); }

    void clear() { tree_.clear(); }

    // ── 查找 ────────────────────────────────────────────────────────────────
    iterator       find(const Key& k)       { return tree_.find(k); }
    const_iterator find(const Key& k) const { return tree_.find(k); }

    T& at(const Key& k) {
        iterator it = find(k);
        if (it == end()) throw std::out_of_range("ministl::map::at: key not found");
        return it->second;
    }
    const T& at(const Key& k) const {
        const_iterator it = find(k);
        if (it == end()) throw std::out_of_range("ministl::map::at: key not found");
        return it->second;
    }

    key_compare key_comp() const { return tree_.key_comp(); }

    void swap(map& o) { tree_.swap(o.tree_); }
};

// 非成员 swap，让 using ministl::swap; 能捞到它（与 utility.h 的约定一致）
template <typename K, typename T, typename C, typename A>
void swap(map<K, T, C, A>& a, map<K, T, C, A>& b) {
    a.swap(b);
}

}  // namespace ministl
