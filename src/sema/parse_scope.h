// =============================================================================
// ParseScopeTable —— 解析期活作用域表（名字的种类 + 可见性）
// =============================================================================
// ① 定位：解析期的作用域链 —— 每层作用域挂自己声明的名字（名字 → 种类位），
//    供 Parser 的消歧点回答"这个名字**在此处**是什么 / 可见吗"。
// ② 源码锚点：clang/include/clang/Sema/Scope.h:Scope（作用域节点 + parent 链）
//    + clang/lib/Sema/SemaDecl.cpp:1574:Sema::PushOnScopeChains（名字入作用域）
//    + clang/lib/Sema/SemaDecl.cpp:912:Sema::ClassifyName（沿链查并分类）。
// ③ 动机：★ 本表是 NameRegistry 的**正规替代**。NameRegistry 前向扫 token 流
//    **反推**名字（猜：typedef 的目标名要"取 ';' 前最后一个标识符的近似"），
//    且 classify() 对裸名"对任意作用域 OR"（不带作用域）⇒ 它回答的是
//    "本 TU 某处声明过吗"，而消歧要问的是"**此处**是什么/可见吗"。
//    clang 的形状：Parser 逐个声明推进作用域（EnterScope/ExitScope +
//    PushOnScopeChains），查询沿 parent 链**内层遮蔽外层**地查 ⇒ "是什么"
//    与"此处可见吗"是**同一个答案**，不需要两张表。
//    名字**先**入作用域、类型**后**补（clang 的 Decl 两态）⇒ 本表只记
//    "名字 + 种类"，类型/偏移/符号仍由 Sema 的语义表管。
// ④ demo: namespace N { struct S{}; }  int main(){ S; }
//       进 N ⇒ 链 [global, N]，N 层记 S→Type；global 层无 S
//       classify("S") 沿链内→外：global 未命中、N 命中 ⇒ 返回 Type
//       ★ 但 S 在 N 里、**全局不可见** ⇒ 可见性由作用域链本身表达：
//         在 global 层查（main 的作用域）⇒ 未命中 ⇒ 不可见
//         （对照 clang：use of undeclared identifier 'S'）
//       对照 NameRegistry：对裸名跨作用域 OR ⇒ 会误判"可见"（B37 的病根）
// 回归：tests/unit/test_parse_scope.cpp（链 / 内层遮蔽 / 限定名查找 / 路径）
// =============================================================================

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

    // ── 声明一个"引入作用域的名字"并进入该作用域 ──
    // 一步做完两件事（clang 里是 PushOnScopeChains + EnterScope 两步）：
    //   ① 把名字登记进**最近的外层非模板形参作用域**（模板形参作用域是透明的：
    //      `template<class T> struct Box` 的名字 Box 属于命名空间，不属于形参域）；
    //   ② 进入新作用域，路径 = 父路径 + name + "::"。
    // 返回新作用域的路径。
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
