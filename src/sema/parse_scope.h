// 解析期名字分类表：活作用域链支持遮蔽，持久上下文支持限定名查找。
// 参考 clang/Sema/Scope.h 与 SemaDecl.cpp: ClassifyName。
// 例：块内 int T 遮蔽外层类型 T；离开 namespace N 后仍可查询 N::T。

#ifndef MINICC_PARSE_SCOPE_H
#define MINICC_PARSE_SCOPE_H

#include "name_scope.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace minicc {

// ═══ 解析期活作用域表 ═══════════════════════════════════════════════════════
class ParseScopeTable {
public:
    // 名字条目：种类位 + （非空时）它引入的作用域路径。
    // scopePath 非空 ⇔ 该名字是一个【作用域名字】（namespace / 类）——
    // 限定名查找靠它把 "N::B" 的限定者 N 落到对应那层作用域。
    struct NameInfo {
        unsigned    bits = 0;
        std::string scopePath;
    };

    struct Scope {
        ScopeKind kind = ScopeKind::Global;
        std::string path;                                  // 完整路径（"N::A::"）
        std::unordered_map<std::string, NameInfo> names;   // 裸名 → 条目
    };

    ParseScopeTable() { m_stack.push_back(Scope{}); }   // 预置全局作用域

    // ── 作用域进出（对照 clang Parser::EnterScope/ExitScope → Actions.CurScope）──
    // enter：压入一层新作用域。Namespace/Class 两类把 name 追加进路径；
    //        Function/TemplateParam/Block 三类沿用父层路径（不引入路径成分）。
    void enter(ScopeKind kind, const std::string& name);
    // leave：弹出当前层。Namespace/Class 两类把本层登记的名字并入【持久上下文】
    //        （clang 的 DeclContext 是持久的，Scope 节点才是临时的 ——
    //        离开 N 之后 `N::B` 仍要能查得到）。
    void leave();

    // ── 登记名字（对照 clang Sema::PushOnScopeChains）──
    // 登记进**当前**（最内层）作用域。同名再登记 ⇒ 位并集（一个名字可以同时
    // 是类型与模板）。
    void declare(const std::string& name, unsigned bits);

    // 登记引入作用域的名字并进入该作用域；模板形参层对登记位置透明。
    // 例：template<class T> struct Box 的 Box 登记到外层，而 T 留在形参层。
    std::string openScope(ScopeKind kind, const std::string& name, unsigned bits);

    // ── 查询 ──
    // 分类：沿作用域链内→外，**首命中即返回**（clang LookupParsedName 的
    // "内层遮蔽外层"）。0 = 未声明。
    unsigned classify(const std::string& name) const;

    // 限定名分类："N::B" —— 限定者逐段定位到那层作用域后，在那层查（**不外溢**
    // 到全局，对应 B38「由宽改严」：N 里没有 B 就是没有，不退到全局 B）。
    // 支持多段限定名："N::A::B"。
    unsigned classifyQualified(const std::string& qualified) const;

    // 声明过吗（任意种类）—— 对应 clang 的 "NotFound" 分支。
    bool isDeclared(const std::string& name) const;

    // 当前作用域的完整路径（"N::A::"；全局为空串）。
    const std::string& currentPath() const { return m_stack.back().path; }
    ScopeKind currentKind() const { return m_stack.back().kind; }
    std::size_t depth() const { return m_stack.size(); }
    // 某条路径是否存在（限定名查找的前置检查）
    bool hasScopePath(const std::string& path) const;

private:
    // 在指定路径的那层作用域里查一个名字（活作用域优先，持久上下文兜底）。
    std::optional<NameInfo> lookupInPath(const std::string& path,
                                         const std::string& name) const;
    // 最近的外层"非模板形参作用域"下标（模板形参域对名字登记是透明的）。
    std::size_t nameScopeIndex() const;
    // 该作用域是否需要持久化（离开后仍要能被限定名查到）。
    static bool isPersistent(ScopeKind kind, ScopeKind parentKind);

    std::vector<Scope> m_stack;
    // 持久上下文：路径 → 该作用域登记过的名字（跨"离开作用域"存活）。
    std::unordered_map<std::string, std::unordered_map<std::string, NameInfo>>
        m_contexts;
};

} // namespace minicc

#endif // MINICC_PARSE_SCOPE_H
