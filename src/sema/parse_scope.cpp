// =============================================================================
// ParseScopeTable 实现（作用域链 + 名字分类）
// =============================================================================
// ① 定位：本文件实现"解析期活作用域表"的全部机制 —— 进出作用域、登记名字、
//    沿链分类（裸名与限定名两种查询）。
// ② 源码锚点：clang/include/clang/Sema/Scope.h:Scope（作用域节点 + parent 链）
//    + clang/lib/Sema/SemaDecl.cpp:1574:Sema::PushOnScopeChains（名字入作用域）
//    + clang/lib/Sema/SemaDecl.cpp:912:Sema::ClassifyName（沿链查并分类）。
// ③ 动机：★ 三条判据各自只写一处（判据单点）：
//    ① "内层遮蔽外层"——classify 沿链自内向外、首命中即返回；
//    ② "限定名不外溢"——classifyQualified 在限定者落到的那层里查，查不到即无；
//    ③ "作用域名字可持久"——离开 namespace/class 后其名字仍能被限定名查到
//       （clang 的 DeclContext 持久、Scope 节点临时）。
//    ★ 第③条与第①②条不是一回事：可见性（①②）看**活链**，限定名（③）看**持久
//      上下文** —— 一个 namespace 里的类型在离开该 namespace 后依然能用
//      `N::T` 指到（这正是限定名与可见性的区别）。
// ④ demo: namespace N { struct S{}; }
//       enter(Namespace,"N") ⇒ 链 [global, "N::"]，global 层记 N→{Namespace,"N::"}
//       leave() ⇒ N 层的名字并入 m_contexts["N::"]
//       classifyQualified("N::S")：N 的 scopePath="N::" ⇒ 在 "N::" 层查 S
//       ⇒ 未命中（该层没有 S，只有 N 自己）⇒ 返回 0（不外溢到全局）
//       classify("S") 在 global 层查 ⇒ 未命中 ⇒ 0（S 不可见）
// 回归：tests/unit/test_parse_scope.cpp
// =============================================================================

#include "parse_scope.h"

#include <algorithm>

namespace minicc {

// ─────────────────────────────────────────────────────────────────────────────
// 作用域进出
// ─────────────────────────────────────────────────────────────────────────────
// 进入新作用域：路径按种类决定是否追加成分。
//   Namespace/Class/Function ⇒ 追加 name + "::"（限定名的路径成分）
//   TemplateParam/Block ⇒ 沿用父层路径（不引入路径成分）
// ★ Function 也加成分：函数体里的**局部类**需要与命名空间里的同名类区分
//   （`void f(){ struct L{}; }` 的 L 落在 "f::L::" 而不是与全局 L 同路径）。
//   代价：同名重载的两个函数共用同一路径 —— 已知边界（局部类场景），
//   见 docs/learn/51 §5。
void ParseScopeTable::enter(ScopeKind kind, const std::string& name) {
    Scope s;
    s.kind = kind;
    s.path = m_stack.back().path;
    if (kind == ScopeKind::Namespace || kind == ScopeKind::Class ||
        kind == ScopeKind::Function) {
        s.path += name + "::";
    }
    // 命名空间/类可被多次打开（namespace N { } N { }）：把上次登记的名字带进来
    // （clang 的 DeclContext 是累积的）。
    if (kind == ScopeKind::Namespace || kind == ScopeKind::Class) {
        auto it = m_contexts.find(s.path);
        if (it != m_contexts.end()) s.names = it->second;
    }
    m_stack.push_back(std::move(s));
}

// 离开当前作用域：命名空间/类两类的名字并入持久上下文。
// ★ 函数/块/模板形参三类【不持久化】—— 局部声明不该被限定名查到
//   （`f::x` 不是 C++ 里的合法限定名）。
void ParseScopeTable::leave() {
    if (m_stack.size() <= 1) return;   // 全局作用域不可弹出
    Scope s = std::move(m_stack.back());
    m_stack.pop_back();
    if (!isPersistent(s.kind, m_stack.back().kind)) return;
    // 键即作用域路径：离开时把本层的名字并进该路径的持久上下文。
    // ★ 用 operator[] 取槽：路径没出现过时它就地建一张空表 —— 这是对的，
    //   刚被判为持久的作用域（命名空间/类）确实存在于该路径上。
    auto& ctx = m_contexts[s.path];
    for (const auto& [name, info] : s.names) {
        auto it = ctx.find(name);
        if (it == ctx.end()) {
            ctx.emplace(name, info);
        } else {
            it->second.bits |= info.bits;   // 位并集：一个名字可同时是多种
        }
    }
}

// 名字所属的作用域：最近的外层"非模板形参作用域"。
// ★ 模板形参作用域对【名字登记】是透明的 —— `template<class T> struct Box`
//   的名字 Box 属于命名空间 N（不属���形参域 T），clang 同理：
//   模板的名字挂在外层 DeclContext，形参挂在自己的 TemplateParameterList。
std::size_t ParseScopeTable::nameScopeIndex() const {
    for (std::size_t i = m_stack.size(); i-- > 0;) {
        if (m_stack[i].kind != ScopeKind::TemplateParam) return i;
    }
    return 0;
}

// 登记名字到当前（最内层）作用域。同名再登记 ⇒ 位并集。
void ParseScopeTable::declare(const std::string& name, unsigned bits) {
    Scope& cur = m_stack.back();
    auto it = cur.names.find(name);
    if (it == cur.names.end()) {
        NameInfo info;
        info.bits = bits;
        cur.names.emplace(name, std::move(info));
    } else {
        it->second.bits |= bits;
    }
}

// 声明"引入作用域的名字"并进入该作用域（clang 的 PushOnScopeChains + EnterScope）。
std::string ParseScopeTable::openScope(ScopeKind kind, const std::string& name,
                                       unsigned bits) {
    // 模板形参作用域与块作用域：既不引入路径成分、也不登记名字 ——
    // 它们只是"一层可见范围"，本身不产生新名字。
    if (kind == ScopeKind::TemplateParam || kind == ScopeKind::Block) {
        enter(kind, name);
        return m_stack.back().path;
    }
    const std::string path = m_stack.back().path + name + "::";
    // 名字登记进最近的外层非模板形参作用域
    Scope& owner = m_stack[nameScopeIndex()];
    auto it = owner.names.find(name);
    if (it == owner.names.end()) {
        NameInfo info;
        info.bits = bits;
        info.scopePath = path;
        owner.names.emplace(name, std::move(info));
    } else {
        it->second.bits |= bits;
        it->second.scopePath = path;   // 路径以最后一次为准（同名重开 ⇒ 同路径）
    }
    enter(kind, name);
    return m_stack.back().path;
}

// ─────────────────────────────────────────────────────────────────────────────
// 查询
// ─────────────────────────────────────────────────────────────────────────────
// 沿链自内向外，首命中即返回 —— 内层遮蔽外层（[basic.lookup.unqual] 的形状）。
unsigned ParseScopeTable::classify(const std::string& name) const {
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        auto n = it->names.find(name);
        if (n != it->names.end()) return n->second.bits;
    }
    return 0;
}

// 限定名分类："N::B" ⇒ 逐段解析限定者，最后一段在限定者所在的那层里查。
// ★ 不外溢：N 里没有 B 就是没有，不退到全局 B（B38「由宽改严」的判据）。
unsigned ParseScopeTable::classifyQualified(const std::string& qualified) const {
    // 拆段：'N::A::B' ⇒ ["N","A","B"]
    std::vector<std::string> segs;
    std::string cur;
    for (char c : qualified) {
        if (c == ':') {
            segs.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    segs.push_back(cur);
    if (segs.size() < 2) {
        // 不是限定名（无 '::'）⇒ 退化成裸名查询
        return classify(qualified);
    }

    // 逐段解析限定者。
    // ★ 第一段走【无限定查找】（沿活链自内向外，首命中即用）—— "N::B" 里的 N
    //   是在【当前位置】可见的限定者；后续各段则限定在上一段落到的那层里查
    //   （"N::C::m" 的 C 只能在 N 里找，不能去全局找）。
    std::string scopePath = m_stack.back().path;
    for (std::size_t i = 0; i + 1 < segs.size(); i++) {
        const std::string& seg = segs[i];
        if (seg.empty()) continue;   // "::B"（全局限定）⇒ 落到全局层
        std::string nextPath;
        if (i == 0) {
            // 第一段：在活链里自内向外找 seg（内层遮蔽外层）。
            for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
                auto n = it->names.find(seg);
                if (n != it->names.end()) {
                    nextPath = n->second.scopePath;
                    break;   // 内层遮蔽外层：即使这层没有 scopePath，也不再往外找
                }
            }
        } else {
            // 后续段：在上一段落到的那层里查（不外溢）。
            auto info = lookupInPath(scopePath, seg);
            if (info) nextPath = info->scopePath;
        }
        if (nextPath.empty()) return 0;   // 限定者不可见 / 不是作用域名 ⇒ 整体不可见
        scopePath = nextPath;
    }

    // 最后一段：在限定者所在的那层里查（不外溢）
    const std::string& last = segs.back();
    auto info = lookupInPath(scopePath, last);
    return info ? info->bits : 0;
}

// 在指定路径的那层作用域里查名字：活作用域优先（新鲜），持久上下文兜底。
std::optional<ParseScopeTable::NameInfo>
ParseScopeTable::lookupInPath(const std::string& path,
                              const std::string& name) const {
    // ★ path 是【精确键】不是前缀：查 "N::M::" 只命中那一层，不命中外层的 "N::"。
    //   活链自内向外扫，第一个 path 命中的帧就是该限定名的归属层。
    for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it) {
        if (it->path != path) continue;
        auto n = it->names.find(name);
        if (n != it->names.end()) return n->second;
        return std::nullopt;   // 找到这一层但没有该名 ⇒ 不外溢
    }
    auto c = m_contexts.find(path);
    if (c != m_contexts.end()) {
        auto n = c->second.find(name);
        if (n != c->second.end()) return n->second;
    }
    return std::nullopt;
}

bool ParseScopeTable::isDeclared(const std::string& name) const {
    return classify(name) != 0;
}

bool ParseScopeTable::hasScopePath(const std::string& path) const {
    for (const auto& s : m_stack) {
        if (s.path == path) return true;
    }
    return m_contexts.count(path) > 0;
}

// 该作用域是否需要持久化（离开后仍要能被限定名查到）。
// ★ 判据：命名空间恒持久；类仅当其【名字所属作用域】不是函数/块时持久
//   —— 函数/块里的类是局部类（local class），C++ 里没有 `f::Local` 这种限定名。
bool ParseScopeTable::isPersistent(ScopeKind kind, ScopeKind parentKind) {
    if (kind == ScopeKind::Namespace) return true;
    if (kind != ScopeKind::Class) return false;
    return parentKind != ScopeKind::Function && parentKind != ScopeKind::Block;
}

} // namespace minicc
