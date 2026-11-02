// =============================================================================
// 单测：作用域即身份 —— 模板与命名空间成员的名字带上作用域（B38）
// =============================================================================
// 考察不变量（断言【性质】而非具体拼写，命名规则随便改都不会误报）：
//   · 作用域前缀化单点：命名空间成员（函数/类/全局变量/枚举/模板）的全名
//     = 完整命名空间路径 + "::" + 裸名（嵌套时不丢外层）。
//   · 实例身份 = 作用域 + 模板名 + 实参表：两个命名空间的同名模板 ⇒ 两个
//     互不相同的实例（实例名不同 ⇒ 汇编符号不同 ⇒ 不会撞）。
//   · 限定名查不到就是查不到：报 "no template named 'X' in namespace 'Y'"，
//     不退到全局的同名模板（clang 逐字）。
//   · mangling 里不出现非法字符：'::' 必须被净化（否则汇编器报 junk）。
// ★ 判据突变验证（摘下面任一规则 ⇒ 对应用例实跑变红）：
//     ① 前缀化用 decl->name（本层）而非 m_currentNamespace
//        ⇒ NestedNamespaceKeepsFullPath 变红（注册名丢外层）
//     ② 模板注册不前缀化 ⇒ SameNameTemplatesInDifferentNamespacesStayDistinct
//        变红（撞成同一个实例）
//     ③ 限定名仍走"剥尾归一" ⇒ QualifiedNameNotFoundIsAnError 变红
//        （不报错，默默绑到全局模板）
//     ④ mangling 不净化 ⇒ MangledNameHasNoIllegalChars 变红（出现 ':'）
// =============================================================================

#include "lexer.h"
#include "obs_helpers.h"
#include "parser.h"
#include "semantic_analyzer.h"

#include <gtest/gtest.h>

using namespace minicc;

namespace {

// 源码 → 语义分析。返回日志；分析抛错时把错误文案一并带回（供断言）。
struct Analysis {
    bool        failed = false;
    std::string message;   // 失败时的诊断文案
    std::string log;       // 全阶段日志
};

Analysis analyze(const std::string& src) {
    Analysis out;
    StdoutCapture cap;
    try {
        Lexer lexer(src);
        auto tokens = lexer.tokenizeAll();
        Parser parser(std::move(tokens));
        TranslationUnit unit = parser.parseTranslationUnit();
        SemanticAnalyzer sema;
        sema.analyze(unit);
    } catch (const std::exception& e) {
        out.failed = true;
        out.message = e.what();
    }
    out.log = cap.str();
    return out;
}

// ── 作用域前缀化单点：嵌套命名空间不丢外层 ────────────────────────────────

TEST(ScopeQualifiedNames, NestedNamespaceKeepsFullPath) {
    auto r = analyze("namespace A { namespace B { int f() { return 7; } } }"
                     " int main() { return A::B::f(); }");
    EXPECT_FALSE(r.failed) << r.message;
    // 注册名必须是完整路径 "A::B::f"（旧写法只取本层 ⇒ "B::f"）
    EXPECT_NE(r.log.find("A::B::f"), std::string::npos) << r.log;
}

// ── 实例身份带作用域：两个命名空间的同名模板是两个实例 ─────────────────────

TEST(ScopeQualifiedNames, SameNameTemplatesInDifferentNamespacesStayDistinct) {
    auto r = analyze(
        "namespace N { template<class T> struct B { T v; T w; }; }"
        " namespace M { template<class T> struct B { T v; T z; }; }"
        " int main() { N::B<int> n; M::B<int> m; n.v = 1; m.v = 2; return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    // 两个实例名都出现，且各自带作用域
    EXPECT_NE(r.log.find("'N::B<int>'"), std::string::npos) << r.log;
    EXPECT_NE(r.log.find("'M::B<int>'"), std::string::npos) << r.log;
    // ★ 反向断言：不得出现"剥掉作用域"的裸实例名 'B<int>'
    EXPECT_EQ(r.log.find("'B<int>'"), std::string::npos) << r.log;
}

// ── 全局模板与命名空间模板是两个实例 ───────────────────────────────────────

TEST(ScopeQualifiedNames, GlobalAndNamespacedTemplateAreDistinct) {
    auto r = analyze(
        "template<class T> struct B { T v; };"
        " namespace N { template<class T> struct B { T v; T w; }; }"
        " int main() { B<int> g; N::B<int> n; g.v = 1; n.v = 2; return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    EXPECT_NE(r.log.find("'B<int>'"), std::string::npos) << r.log;
    EXPECT_NE(r.log.find("'N::B<int>'"), std::string::npos) << r.log;
}

// ── 限定名查不到就是查不到（不退到全局的同名模板）─────────────────────────
// clang: error: no template named 'B' in namespace 'N'

TEST(ScopeQualifiedNames, QualifiedNameNotFoundIsAnError) {
    auto r = analyze("template<class T> struct B { T v; };"
                     " namespace N {}"
                     " int main() { N::B<int> x; return 0; }");
    EXPECT_TRUE(r.failed);
    EXPECT_NE(r.message.find("no template named 'B' in namespace 'N'"),
              std::string::npos) << r.message;
}

// ── mangling 里不出现非法字符（'::' 必须被净化）────────────────────────────
// '::' 直接进符号标签 ⇒ 汇编器报 "junk at end of line"。

TEST(ScopeQualifiedNames, MangledNameHasNoIllegalChars) {
    auto r = analyze("namespace N { template<class T> struct B { T v; }; }"
                     " int main() { N::B<int> x; x.v = 1; return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    // 抓 "║ Mangled: <实例名> → <符号>" 那一行，只取【符号】部分断言
    const auto pos = r.log.find("Mangled:");
    ASSERT_NE(pos, std::string::npos) << r.log;
    const auto line = r.log.substr(pos, r.log.find('\n', pos) - pos);
    const auto arrow = line.find("→");
    ASSERT_NE(arrow, std::string::npos) << line;
    const std::string symbol = line.substr(arrow + 1);
    // '::' 必须被净化（否则汇编器报 junk at end of line）
    EXPECT_EQ(symbol.find(':'), std::string::npos) << line;
    EXPECT_NE(symbol.find("N__B"), std::string::npos) << line;
}

} // namespace
