// =============================================================================
// 单测：名字可见性判据（lookupVisibleType / isNameDeclared —— B37 单点判据）
// =============================================================================
// 考察理论点（对应 docs/BUGS.md B37、docs/learn/48）：
//   1. [basic.lookup.unqual]：非限定名字查找沿作用域链上溯 —— 当前类作用域先于
//      命名空间链，命名空间链逐级外层（namespace A { namespace B { … } } 里 B 内
//      能看到 A 的类型），全局在最后。
//   2. 可见性是【语义】问题：只有 Sema 的符号表答得了。Parser 侧的名字登记表
//      （NameRegistry）是"本 TU 某处声明过"的跨作用域并集，拿它当"可见"就会
//      静默放行非法程序 —— `namespace N { struct S{}; } int main(){ S; }`
//      里 S 在 N 里、全局不可见（clang rc=1）。
//   3. 同名遮蔽：块作用域的变量 S 遮蔽全局的类 S ⇒ 此处 `S;` 是【表达式】不是
//      声明（clang：expression result unused，rc=0）——[basic.scope.scope]。
//
// 断言的是【不变量】而不是症状值：
//   · 可见 ⇒ 分析通过，且日志出现可见性裁决行；
//   · 不可见 ⇒ 分析抛错，文案与 clang 逐字相同（use of undeclared identifier 'X'）。
//
// ★ 判据突变验证（摘下面任一处置 ⇒ 对应用例实跑变红）：
//     ① lookupVisibleType 的命名空间链逐级外层 ⇒ NamespaceChainOuterLevel 变红
//     ② lookupVisibleType 的类作用域优先 ⇒ ClassScopeWinsOverNamespace 变红
//     ③ visit(EmptyStmt) 的第二问（声明过但不是类型 ⇒ 表达式路径）
//        ⇒ ShadowedByLocalVariable 变红
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='NameVisibility.*'
// =============================================================================

#include <gtest/gtest.h>

#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "obs_helpers.h"

#include <memory>
#include <stdexcept>
#include <string>

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

// ── 可见：全局的类型名裸声明 ────────────────────────────────────────────────

TEST(NameVisibility, GlobalTypeIsVisible) {
    auto r = analyze("struct S { int x; }; int main() { S; return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    // 可见性裁决走到了 Sema（而不是停在 Parser）
    EXPECT_NE(r.log.find("[sema:stmt] 'S' 可见的类型名"), std::string::npos);
}

// ── 可见：命名空间内的类型名，在【该命名空间内】裸用 ────────────────────────

TEST(NameVisibility, NamespaceTypeVisibleInsideItsNamespace) {
    auto r = analyze("namespace N { struct S { int x; }; int f() { S; return 0; } }"
                     " int main() { N::f(); return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    EXPECT_NE(r.log.find("[sema:stmt] 'S' 可见的类型名"), std::string::npos);
}

// ── 可见：命名空间链逐级外层（内层命名空间能看到外层的类型）──────────────
// 这是 clang 的 DeclContext 链语义；只查一层时会拒收合法程序。

TEST(NameVisibility, NamespaceChainOuterLevel) {
    // ★ f() 不被调用也无妨：Sema 的 Pass 3 会走查【所有】函数体，裁决照样发生
    //   （不去调用它是因为"限定名调用命名空间成员函数"是另一处缺口，与本判据无关）。
    auto r = analyze("namespace A { struct S { int x; }; namespace B { int f() { S; return 0; } } }"
                     " int main() { return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    EXPECT_NE(r.log.find("[sema:stmt] 'S' 可见的类型名"), std::string::npos);
}

// ── 可见：类作用域的类型别名，在类成员函数体内裸用 ──────────────────────────

TEST(NameVisibility, ClassScopeAliasVisibleInsideClass) {
    auto r = analyze("struct A { using B = int; int f() { B; return 0; } };"
                     " int main() { A a; return a.f(); }");
    EXPECT_FALSE(r.failed) << r.message;
    EXPECT_NE(r.log.find("[sema:stmt] 'B' 可见的类型名"), std::string::npos);
}

// ── 不可见：命名空间里的类型，在【全局】裸用 ───────────────────────────────
// clang: error: use of undeclared identifier 'S'

TEST(NameVisibility, NamespaceTypeNotVisibleAtGlobalScope) {
    auto r = analyze("namespace N { struct S { int x; }; } int main() { S; return 0; }");
    EXPECT_TRUE(r.failed);
    EXPECT_NE(r.message.find("use of undeclared identifier 'S'"), std::string::npos)
        << r.message;
}

// ── 不可见：类作用域的别名，在【全局】裸用 ─────────────────────────────────
// clang: error: use of undeclared identifier 'B'

TEST(NameVisibility, ClassScopeAliasNotVisibleAtGlobalScope) {
    auto r = analyze("struct A { using B = int; }; int main() { B; return 0; }");
    EXPECT_TRUE(r.failed);
    EXPECT_NE(r.message.find("use of undeclared identifier 'B'"), std::string::npos)
        << r.message;
}

// ── 遮蔽：块作用域的变量遮蔽同名的全局类型 ⇒ 走表达式路径（不是声明）────────
// clang: warning: expression result unused（rc=0）；minicc 无 warning，但不得报错。

TEST(NameVisibility, ShadowedByLocalVariable) {
    auto r = analyze("struct S { int x; }; int main() { int S = 5; S; return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    // 走了表达式路径（变量被登记进符号表），而不是被判成声明
    EXPECT_EQ(r.log.find("[sema:stmt] 'S' 可见的类型名"), std::string::npos);
    EXPECT_NE(r.log.find("'S' → int"), std::string::npos);
}

} // namespace
