#pragma once
// =============================================================================
// ministl/detail/rb_tree.h —— 红黑树核心（map / set 共用的底层）
// =============================================================================
// 学习要点：
//   1. 【为什么抽这一层】libc++ 的分层是 __tree（裸树 + 哨兵）→ map/set/multiset
//      （适配器：决定"值长什么样、键从哪取"）。同一棵树不用写两遍——删除修正
//      那种"5000 步随机操作才炸一次"的代码，存在两份就是两个 bug 温床。
//   2. 【树核心不知道"键"的形状】它只认三样：存储类型 T、键类型 Key、
//      以及 KeyOf 策略（static const Key& get(const T&) —— 从值里取键）。
//      map 传 pair<const K,V> 取 .first；set 传 K 取自身。比较永远发生在
//      Key 域（Compare 是键比较器）。
//   3. 【红黑不变量】① 每个节点红或黑 ② 根黑 ③ 红节点的孩子必黑（无连续红）
//      ④ 任意节点到其所有叶子的路径含相同数目的黑节点（黑高相同）。
//      ⇒ 最长路径 ≤ 2 × 最短路径 ⇒ 树高 = O(log n)。
//   4. 【染色 vs 旋转】插入/删除修复的第一手段是【重新染色】（O(1) 改标志位），
//      只有染色解决不了才【旋转】（改 3~5 个指针）。所以红黑树的常数很小。
//      这也是它与 AVL 的本质区别：AVL 用旋转换严格平衡，红黑树用染色换少旋转。
//   5. 【哨兵 end 节点】root 存在 end_.left，end_ 自己充当 end() 迭代器，
//      end_.parent 缓存整树最小节点（--end() 与 begin() 都靠它）。
//      从此「空树」「root 无父」都不再特判 —— 与 list 的哨兵节点同一思想。
//   6. 【erase 是这里唯一的难点】删除有两子的节点时，实际摘除的是【中序后继】，
//      把它接到 z 的位置上并继承 z 的颜色；值由适配器层搬（树核心只管结构）。
//      真正需要修复的只有「摘除了黑色节点」这一种情况 —— 且修复的是
//      x = 被摘节点的（可能为空的）那个孩子，它带着「双重黑」的隐含属性。
//
// 算法来源：旋转/插入修复/删除修复均移植自 libc++ 的 __tree
// （~/cppproject/llvm-project/libcxx/include/__tree，CLRS 第 13 章的迭代版），
// 按本项目规则只移植算法、不整段拷贝。
//
// minicc 友好性：KeyOf 用【静态成员函数】而非 operator()（minicc 尚不支持
// 重载调用运算符，类内 static 成员函数已支持）。
// =============================================================================

#include "../memory.h"   // allocator
#include "../utility.h"  // pair / move / forward / swap / less

#include <cstddef>
#include <concepts>
#include <type_traits>

namespace ministl {
namespace detail {

// ─────────────────────────────────────────────────────────────────────────────
//  一、节点
// ─────────────────────────────────────────────────────────────────────────────

// 基类不含数据 —— 哨兵 end 节点不需要存 T 就能构造（同 list 的哨兵思想）
struct rb_node_base {
    rb_node_base* parent = nullptr;
    rb_node_base* left   = nullptr;
    rb_node_base* right  = nullptr;
    bool is_black        = true;   // 新节点按红插入（哨兵默认黑）
};

template <typename T>
struct rb_node : rb_node_base {
    T value;

    explicit rb_node(const T& v) : value(v) {}
    explicit rb_node(T&& v) : value(ministl::move(v)) {}
};

// ── 遍历原语（libc++ __tree_min / __tree_next 的移植）──────────────────────
inline bool is_left_child(rb_node_base* x) {
    return x->parent != nullptr && x == x->parent->left;
}
inline bool is_right_child(rb_node_base* x) {
    return x->parent != nullptr && x == x->parent->right;
}

// tree_min/tree_max：最左/最右子孙，O(h)
inline rb_node_base* tree_min(rb_node_base* x) {
    while (x->left != nullptr) x = x->left;
    return x;
}
inline rb_node_base* tree_max(rb_node_base* x) {
    while (x->right != nullptr) x = x->right;
    return x;
}

// 中序后继：右子树非空 ⇒ 右子树最小；否则向上爬直到「从左子树上来」。
// end 哨兵的 parent 指向整树最小节点，因此 --end() 也走同一条路。
inline rb_node_base* tree_next(rb_node_base* x) {
    if (x->right != nullptr) return tree_min(x->right);
    while (!is_left_child(x)) x = x->parent;
    return x->parent;
}
inline rb_node_base* tree_prev(rb_node_base* x) {
    if (x->left != nullptr) return tree_max(x->left);
    while (is_right_child(x)) x = x->parent;
    return x->parent;
}

// ── 旋转（CLRS 13.2）─────────────────────────────────────────────────────────
// 左旋：y = x->right 上提，x 成为 y 的左孩子，y 原来的左子树 β 挂给 x 当右子树。
// 中序序列 (α x β y γ) 旋转前后不变 —— 这就是 BST 性质在旋转下保持的原因。
inline void tree_left_rotate(rb_node_base*& root, rb_node_base* x) {
    rb_node_base* y = x->right;
    x->right        = y->left;
    if (x->right != nullptr) x->right->parent = x;
    y->parent = x->parent;
    if (x == root) {
        root = y;
    } else if (is_left_child(x)) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left   = x;
    x->parent = y;
}

inline void tree_right_rotate(rb_node_base*& root, rb_node_base* y) {
    rb_node_base* x = y->left;
    y->left         = x->right;
    if (y->left != nullptr) y->left->parent = y;
    x->parent = y->parent;
    if (y == root) {
        root = x;
    } else if (is_right_child(y)) {
        y->parent->right = x;
    } else {
        y->parent->left = x;
    }
    x->right  = y;
    y->parent = x;
}

// ── 插入修复（CLRS 13.3 / libc++ __tree_balance_after_insert）────────────────
// 前提：x 刚作为【红】叶子挂上。只有「父节点也是红」才违反不变量 ③。
// 分两种大情况（父节点是左孩子/右孩子），每种里再分「叔红⇒染色上推」
// 与「叔黑⇒旋转+染色」两小情况。左旋右旋版本对称。
inline void tree_balance_after_insert(rb_node_base*& root, rb_node_base* x) {
    x->is_black = (x == root);          // 根必须是黑的
    while (x != root && !x->parent->is_black) {
        if (is_left_child(x->parent)) {                       // 父是左孩子
            rb_node_base* uncle = x->parent->parent->right;
            if (uncle != nullptr && !uncle->is_black) {       // 叔红 ⇒ 染色上推
                x                = x->parent;
                x->is_black      = true;
                x                = x->parent;                 // 祖父
                x->is_black      = (x == root);
                uncle->is_black  = true;
            } else {                                          // 叔黑 ⇒ 旋转
                if (!is_left_child(x)) {                      // 内侧 ⇒ 先转成外侧
                    x = x->parent;
                    tree_left_rotate(root, x);
                }
                x                = x->parent;                 // 父
                x->is_black      = true;
                x                = x->parent;                 // 祖父
                x->is_black      = false;
                tree_right_rotate(root, x);
                break;
            }
        } else {                                              // 父是右孩子（对称）
            rb_node_base* uncle = x->parent->parent->left;
            if (uncle != nullptr && !uncle->is_black) {
                x                = x->parent;
                x->is_black      = true;
                x                = x->parent;
                x->is_black      = (x == root);
                uncle->is_black  = true;
            } else {
                if (is_left_child(x)) {
                    x = x->parent;
                    tree_right_rotate(root, x);
                }
                x                = x->parent;
                x->is_black      = true;
                x                = x->parent;
                x->is_black      = false;
                tree_left_rotate(root, x);
                break;
            }
        }
    }
}

// ── 删除（libc++ __tree_remove 的移植）───────────────────────────────────────
// 摘除 z（不析构/不释放 —— 由调用者负责）。删除有两子的节点时实际摘除的是
// 中序后继 y 并把它接到 z 的位置继承 z 的颜色；只有「摘了黑色」才需要修复，
// 且修复对象是 y 的那个（可能为空的）孩子 x —— 它继承了「双重黑」。
inline void tree_remove(rb_node_base*& root, rb_node_base* z) {
    // y = 实际被摘除的节点（最多一个孩子）；z 有两子时 y 是中序后继
    rb_node_base* y =
        (z->left == nullptr || z->right == nullptr) ? z : tree_next(z);
    // x = y 的（可能为空的）唯一孩子，将接替 y 的位置
    rb_node_base* x = y->left != nullptr ? y->left : y->right;
    // w = x 的叔节点（将成为 x 的兄弟），修复时围着它转
    rb_node_base* w = nullptr;

    if (x != nullptr) x->parent = y->parent;
    if (is_left_child(y)) {
        y->parent->left = x;
        if (y != root) w = y->parent->right;
        else           root = x;                 // w 不存在
    } else {
        y->parent->right = x;
        w = y->parent->left;                     // 右孩子不可能是 root
    }

    bool removed_black = y->is_black;

    // y != z 时把 y 接到 z 的位置上，继承 z 的颜色（值由调用方搬）
    if (y != z) {
        y->parent = z->parent;
        if (is_left_child(z)) y->parent->left = y;
        else                  y->parent->right = y;
        y->left            = z->left;
        y->left->parent    = y;
        y->right           = z->right;
        if (y->right != nullptr) y->right->parent = y;
        y->is_black        = z->is_black;
        if (root == z) root = y;
    }

    if (removed_black && root != nullptr) {
        // x 带着 y 传下来的「隐含黑」。x 为红 ⇒ 染黑即吸收；x 为 root 同理；
        // 否则 x 是「双重黑」的洞，沿兄弟 w 做四种情形的修复（CLRS 13.4）。
        if (x != nullptr) {
            x->is_black = true;
        } else {
            // 双重黑的洞沿兄弟 w 上移修复（CLRS 13.4 四种情形）。
            // ★ 结构必须忠实于 libc++ __tree_remove：情形 2 末尾无条件 continue，
            //   让循环头按 w 的新位置重新选边 —— 曾把它改成"fall through"，
            //   方向翻转时会落进错误的一侧执行 3/4 情形（5000 步随机删除必炸）。
            while (true) {
                if (!is_left_child(w)) {                     // x 是左孩子（w 为右兄弟）
                    if (!w->is_black) {                      // 情形1：兄弟红
                        w->is_black         = true;
                        w->parent->is_black = false;
                        tree_left_rotate(root, w->parent);
                        if (root == w->left) root = w;
                        w = w->left->right;                  // 兄弟仍是黑的，非空
                    }
                    if ((w->left == nullptr || w->left->is_black) &&
                        (w->right == nullptr || w->right->is_black)) {
                        // 情形2：两侄黑 ⇒ 兄弟染红、双重黑上推给父
                        w->is_black = false;
                        x = w->parent;
                        if (x == root || !x->is_black) {     // 父红或根：染黑收工
                            x->is_black = true;
                            break;
                        }
                        w = is_left_child(x) ? x->parent->right : x->parent->left;
                        continue;                            // ★ 重新选边，不许 fall through
                    } else {                                 // 情形3/4：有红侄
                        if (w->right == nullptr || w->right->is_black) {
                            // 情形3：近侄（左）红、远侄黑 ⇒ 近侄上旋成新兄弟
                            w->left->is_black = true;
                            w->is_black       = false;
                            tree_right_rotate(root, w);
                            w = w->parent;
                        }
                        // 情形4：远侄红 ⇒ 兄弟继承父色、父与远侄染黑、父上旋
                        w->is_black         = w->parent->is_black;
                        w->parent->is_black = true;
                        w->right->is_black  = true;
                        tree_left_rotate(root, w->parent);
                        break;
                    }
                } else {                                     // x 是右孩子（镜像）
                    if (!w->is_black) {
                        w->is_black         = true;
                        w->parent->is_black = false;
                        tree_right_rotate(root, w->parent);
                        if (root == w->right) root = w;
                        w = w->right->left;
                    }
                    if ((w->right == nullptr || w->right->is_black) &&
                        (w->left == nullptr || w->left->is_black)) {
                        w->is_black = false;
                        x = w->parent;
                        if (x == root || !x->is_black) {
                            x->is_black = true;
                            break;
                        }
                        w = is_right_child(x) ? x->parent->left : x->parent->right;
                        continue;
                    } else {
                        if (w->left == nullptr || w->left->is_black) {
                            w->right->is_black = true;
                            w->is_black        = false;
                            tree_left_rotate(root, w);
                            w = w->parent;
                        }
                        w->is_black         = w->parent->is_black;
                        w->parent->is_black = true;
                        w->left->is_black   = true;
                        tree_right_rotate(root, w->parent);
                        break;
                    }
                }
            }
        }
    }
}

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  二、迭代器
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

// Ref/Ptr 区分 const 与 non-const（与 list_iterator 同一份代码两种实例）
template <typename T, typename Ref, typename Ptr>
class rb_iterator {
public:
    rb_node_base* node = nullptr;

    rb_iterator() = default;
    explicit rb_iterator(rb_node_base* n) : node(n) {}
    // const_iterator 从 const 树上构造：指针存的是非 const（tree_next 要爬父链），
    // 读写权限由 Ref/Ptr 模板参数把守（与 std 迭代器的实现策略一致）
    explicit rb_iterator(const rb_node_base* n)
        : node(const_cast<rb_node_base*>(n)) {}

    template <typename R2, typename P2>
    bool operator==(const rb_iterator<T, R2, P2>& o) const { return node == o.node; }
    template <typename R2, typename P2>
    bool operator!=(const rb_iterator<T, R2, P2>& o) const { return node != o.node; }

    // 可变 → 只读 的转换（std 迭代器的经典设计：iterator 隐式升格为
    // const_iterator，反向不行）。C++20 requires 约束：目标所指对象是源所指
    // 对象的 const 版本 —— const 性挂在 pointee 上，remove_const 必须先剥指针。
    template <typename R2, typename P2>
        requires(std::is_const_v<std::remove_pointer_t<Ptr>> &&
                 std::same_as<std::remove_const_t<std::remove_pointer_t<Ptr>>,
                              std::remove_pointer_t<P2>>)
    rb_iterator(const rb_iterator<T, R2, P2>& o) : node(o.node) {}

    Ref operator*() const { return static_cast<rb_node<T>*>(node)->value; }
    Ptr operator->() const { return &static_cast<rb_node<T>*>(node)->value; }

    rb_iterator& operator++() { node = tree_next(node); return *this; }
    rb_iterator& operator--() { node = tree_prev(node); return *this; }
    rb_iterator operator++(int) { rb_iterator tmp(*this); ++(*this); return tmp; }
    rb_iterator operator--(int) { rb_iterator tmp(*this); --(*this); return tmp; }
};

}  // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  三、树本体（map / set 的共用底层）
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

// KeyOf 策略：从存储类型 T 取键。约定为【静态成员函数】
//   static const Key& get(const T&)
// map 传 pair 取 .first，set 传 K 取自身。不用 operator() —— minicc 尚未
// 支持重载调用运算符；静态成员函数两个编译器都吃得下。
//
// Compare 是【键】比较器：树内所有比较都发生在 Key 域。
template <typename Key, typename T, typename KeyOf, typename Compare>
class rb_tree {
public:
    using key_type        = Key;
    using value_type      = T;
    using key_compare     = Compare;
    using size_type       = std::size_t;
    using iterator        = detail::rb_iterator<T, T&, T*>;
    using const_iterator  = detail::rb_iterator<T, const T&, const T*>;

private:
    using node_base  = detail::rb_node_base;
    using node       = detail::rb_node<T>;
    using node_alloc = allocator<node>;   // 实际分配 node，不是裸 T（与 list 同理）

    // 哨兵：end_.left 是 root；end_.parent 缓存整树最小节点（--end() 用）
    node_base end_;
    size_type size_ = 0;
    Compare comp_;
    node_alloc alloc_;

    node_base*& root() { return end_.left; }
    node_base*  root() const { return end_.left; }   // const 查找路径只读访问
    static node* as_node(node_base* p) { return static_cast<node*>(p); }

    node* new_node(const value_type& v) {
        node* p = alloc_.allocate(1);
        alloc_.construct(p, v);
        return p;
    }
    node* new_node(value_type&& v) {
        node* p = alloc_.allocate(1);
        alloc_.construct(p, ministl::move(v));
        return p;
    }
    void delete_node(node* p) {
        alloc_.destroy(p);
        alloc_.deallocate(p, 1);
    }

    void init_end() { end_.parent = &end_; }   // 空树：最小节点指向哨兵自己

    node_base* leftmost() const { return end_.parent; }

    void destroy_all(node_base* p) {
        if (p == nullptr) return;
        destroy_all(p->left);
        destroy_all(p->right);
        delete_node(as_node(p));
    }

public:
    rb_tree() : comp_(), alloc_() { init_end(); }
    explicit rb_tree(const Compare& comp) : comp_(comp), alloc_() { init_end(); }
    ~rb_tree() { clear(); }

    rb_tree(const rb_tree&)            = delete;   // 拷贝由适配器逐元素 insert 实现
    rb_tree& operator=(const rb_tree&) = delete;

    // ── 容量 ────────────────────────────────────────────────────────────────
    bool      empty() const { return size_ == 0; }
    size_type size()  const { return size_; }

    // 比较器访问（适配器实现 key_comp() 用）
    Compare key_comp() const { return comp_; }

    // ── 迭代 ────────────────────────────────────────────────────────────────
    iterator       begin()        { return iterator(leftmost()); }
    const_iterator begin()  const { return const_iterator(leftmost()); }
    const_iterator cbegin() const { return const_iterator(leftmost()); }
    // end() 就是哨兵本身 —— 对哨兵做 tree_next 的行为由 tree_prev 的约定保证
    iterator       end()          { return iterator(&end_); }
    const_iterator end()    const { return const_iterator(&end_); }
    const_iterator cend()   const { return const_iterator(&end_); }

    // ── 修改 ────────────────────────────────────────────────────────────────
    // 返回 {指向相等键的迭代器, 是否新插入}。键已存在 ⇒ 不动旧值（unique 语义）
    template <typename V>
    pair<iterator, bool> insert_unique(V&& v) {
        node_base* parent = &end_;
        node_base* cur    = root();
        bool go_left         = true;            // 空树时把 root 当作 end 的左孩子挂
        while (cur != nullptr) {
            parent  = cur;
            if (comp_(KeyOf::get(as_node(cur)->value), KeyOf::get(v))) {
                go_left = false;
                cur     = cur->right;
            } else if (comp_(KeyOf::get(v), KeyOf::get(as_node(cur)->value))) {
                go_left = true;
                cur     = cur->left;
            } else {
                return {iterator(cur), false};  // 键已存在，不覆盖
            }
        }
        node* z        = new_node(ministl::forward<V>(v));
        z->parent      = parent;
        z->left = z->right = nullptr;
        z->is_black    = false;                  // 新节点按红插入
        if (go_left) {
            parent->left = z;
            if (parent == &end_) root() = z;     // 挂到哨兵上 ⇒ 它就是新 root
        } else {
            parent->right = z;
        }
        tree_balance_after_insert(root(), z);
        end_.parent = root() != nullptr ? detail::tree_min(root()) : &end_;  // 最小可能变了，重算
        ++size_;
        return {iterator(z), true};
    }

    // 按迭代器删（两种 const 性都给：std::set::erase 收 const_iterator）
    iterator erase(iterator it) { return erase_impl(it.node); }
    iterator erase(const_iterator it) { return erase_impl(it.node); }

    // 按键删，返回删了几个（unique 树非 0 即 1）
    size_type erase_key(const Key& k) {
        iterator it = find(k);
        if (it == end()) return 0;
        erase(it);
        return 1;
    }

    void clear() {
        destroy_all(root());
        root()      = nullptr;
        end_.parent = &end_;
        size_       = 0;
    }

    // ── 查找 ────────────────────────────────────────────────────────────────
    iterator find(const Key& k) {
        node_base* cur = root();
        while (cur != nullptr) {
            if (comp_(KeyOf::get(as_node(cur)->value), k))      cur = cur->right;
            else if (comp_(k, KeyOf::get(as_node(cur)->value))) cur = cur->left;
            else                                                return iterator(cur);
        }
        return end();
    }
    const_iterator find(const Key& k) const {
        node_base* cur = root();
        while (cur != nullptr) {
            if (comp_(KeyOf::get(as_node(cur)->value), k))      cur = cur->right;
            else if (comp_(k, KeyOf::get(as_node(cur)->value))) cur = cur->left;
            else                                                return const_iterator(cur);
        }
        return end();
    }

    // 交换：root/leftmost 引用各自的哨兵，不能直接换整个哨兵，
    // 否则两棵树的 root 会指到对方的哨兵上 —— 这是有哨兵容器交换的坑
    void swap(rb_tree& o) {
        if (this == &o) return;
        node_base* r1 = root();
        node_base* r2 = o.root();
        root()      = r2;
        o.root()    = r1;
        if (root() != nullptr)   root()->parent   = &end_;
        if (o.root() != nullptr) o.root()->parent = &o.end_;
        end_.parent   = root()   == nullptr ? &end_   : detail::tree_min(root());
        o.end_.parent = o.root() == nullptr ? &o.end_ : detail::tree_min(o.root());
        using ministl::swap;
        swap(size_, o.size_);
        swap(comp_, o.comp_);
    }

private:
    iterator erase_impl(node_base* z) {
        iterator next(as_node(z));
        ++next;
        tree_remove(root(), z);
        if (z == leftmost()) end_.parent = next.node;   // 最小被删 ⇒ 次小顶上
        delete_node(as_node(z));
        --size_;
        return next;
    }
};

}  // namespace detail
}  // namespace ministl
