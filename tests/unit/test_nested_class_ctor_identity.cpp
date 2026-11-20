// =============================================================================
// 单测：嵌套类的构造/析构身份 + 初始化列表的构造目标（BUGS.md B60、learn/62）
// =============================================================================
// 考察不变量（断言【性质】而非具体拼写，命名规则随便改都不会误报）：
//   · 构造函数的名字是【本类的裸名】（[class.ctor]/1 的 injected-class-name）：
//     用户写的 ctor 名来自源码 token（天然裸名）⇒ 编译器合成的那个必须同口径，
//     否则同一个类会有两种符号形态（"Outer::Mem_int_Outer::Mem_int" vs
//     "Outer::Inner_Inner"）。
//   · 析构函数同款（名字 = "~" + 裸名），符号 = 登记名 + "_dtor"。
//   · 显式构造只算一次：判据认错就会再合成一个无参 ctor ⇒ 同一个类两个 ctor。
//   · 初始化列表的构造目标由 Sema 选定并回填（[class.base.init]）——
//     调用点按名字重算就是 B42/B60 那个坑；基类与字段两条路径都要回填。
//   · 成员的名字只在类作用域里可见（[basic.lookup.unqual]/6）：
//     `namespace N { struct S {...}; } int main(){ S; }` 必须拒收。
//   · `类名::成员名` 这个键空间是【静态成员】的判据不动：成员函数不能占进去
//     （占了会让 `d.g()` 被误判成 static member）。
// ★ 判据突变验证（摘掉任一条 ⇒ 对应用例实跑变红；括号内是【实测】结果）：
//     ① 合成 ctor 名改回 decl->name ⇒ ①②④⑤ 红（符号变成登记名重复形态）
//     ② 合成 dtor 名改回 "~" + decl->name ⇒ ① 红
//     ③ Sema 不回填 init.ctorSymbol ⇒ ④⑤ 红（日志里没有 [ctor-init] 行）
//     ④ 成员函数以限定名登记进符号表 ⇒ ⑧ 红（静态分支误命中）
//     ⑤ 成员函数以裸名登记进全局作用域 ⇒ ⑦ 红（成员名泄漏成全局可见）
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

// 子串出现次数（用于"只合成一个构造"这类性质断言）。
size_t countOf(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

}  // namespace

// ── ① 合成的构造/析构取【裸名】，不是登记名 ───────────────────────────────

TEST(NestedCtorIdentity, SynthesizedCtorAndDtorUseBareName) {
    auto r = analyze("struct Outer { struct Inner { int v; }; };"
                     " int main() { Outer::Inner i; i.v = 1; return i.v; }");
    ASSERT_FALSE(r.failed) << r.message;
    // 类成员列表里的形态：裸名
    EXPECT_NE(r.log.find("method: Inner() "), std::string::npos) << r.log;
    EXPECT_NE(r.log.find("method: ~Inner() "), std::string::npos) << r.log;
    // 登记名重复形态（"Outer::Inner()"）恰恰是缺陷形态 —— 不许出现
    EXPECT_EQ(r.log.find("Outer::Inner::Outer::Inner("), std::string::npos) << r.log;
}

// ── ② 命名空间类的合成构造：符号 = 登记名 + "_" + 裸名 ────────────────────

TEST(NestedCtorIdentity, SynthesizedCtorSymbolUsesBareMethodName) {
    auto r = analyze("namespace N { struct S { int v; }; }"
                     " int main() { N::S s; s.v = 2; return s.v; }");
    ASSERT_FALSE(r.failed) << r.message;
    EXPECT_NE(r.log.find("[register] N::S::S() → void"), std::string::npos) << r.log;
    EXPECT_NE(r.log.find("[mangled: N::S_S]"), std::string::npos) << r.log;
    EXPECT_EQ(r.log.find("[mangled: N::S_N::S]"), std::string::npos) << r.log;
}

// ── ③ 显式构造只被识别一次（判据认错 ⇒ 会再合成一个）─────────────────────

TEST(NestedCtorIdentity, ExplicitCtorIsNotDuplicatedBySynthesis) {
    auto r = analyze("struct Outer { struct Inner { int v; Inner() { v = 5; } }; };"
                     " int main() { Outer::Inner i; return i.v; }");
    ASSERT_FALSE(r.failed) << r.message;
    // 一个类只该有一个无参 ctor 的登记行
    EXPECT_EQ(countOf(r.log, "[register] Outer::Inner::Inner()"), 1u) << r.log;
}

// ── ④ 初始化列表的字段构造符号由 Sema 回填 ────────────────────────────────

TEST(NestedCtorIdentity, FieldInitializerSymbolIsBackfilled) {
    auto r = analyze("struct Holder {"
                     "   struct Part { int v; Part(int x) { v = x; } };"
                     "   Part m;"
                     "   Holder() : m(9) {}"
                     " };"
                     " int main() { Holder h; return 0; }");
    ASSERT_FALSE(r.failed) << r.message;
    // 回填值与定义点同一个符号（裸名口径）：不是 "Holder::Part_Holder::Part_1"
    EXPECT_NE(r.log.find("⇒ 选定构造函数符号 Holder::Part_Part_1"),
              std::string::npos) << r.log;
}

// ── ⑤ 基类构造同样被认出来（基类名可能是限定名）───────────────────────────

TEST(NestedCtorIdentity, BaseInitializerIsRecognized) {
    auto r = analyze("namespace N { struct Base { int v; Base(int x) { v = x; } }; }"
                     " struct D : N::Base { D() : Base(7) {} };"
                     " int main() { D d; return d.v; }");
    ASSERT_FALSE(r.failed) << r.message;
    EXPECT_NE(r.log.find("基类 Base(1 个实参)"), std::string::npos) << r.log;
    EXPECT_NE(r.log.find("⇒ 选定构造函数符号 N::Base_Base_1"),
              std::string::npos) << r.log;
}

// ── ⑥ 边界不放大：目标类没有匹配的构造函数 ⇒ 响亮拒收 ────────────────────

TEST(NestedCtorIdentity, MissingTargetCtorIsAnError) {
    auto r = analyze("struct Holder {"
                     "   struct Part { int v; };"
                     "   Part m;"
                     "   Holder() : m(9) {}"
                     " };"
                     " int main() { Holder h; return 0; }");
    EXPECT_TRUE(r.failed);
    EXPECT_NE(r.message.find("no matching constructor for initialization of 'm'"),
              std::string::npos) << r.message;
}

// ── ⑦ 成员名不泄漏成全局可见名（[basic.lookup.unqual]/6）─────────────────

TEST(NestedCtorIdentity, MemberNamesDoNotLeakIntoGlobalScope) {
    auto r = analyze("namespace N { struct S { int x; }; }"
                     " int main() { S; return 0; }");
    EXPECT_TRUE(r.failed);
    EXPECT_NE(r.message.find("use of undeclared identifier 'S'"),
              std::string::npos) << r.message;
}

// ── ⑧ 成员函数不占用"类名::成员名"这个静态成员键空间 ──────────────────────

TEST(NestedCtorIdentity, MemberFunctionIsNotMistakenForStaticMember) {
    auto r = analyze("struct A { int g() { return 5; } };"
                     " struct D : A { };"
                     " int main() { D d; return d.g(); }");
    ASSERT_FALSE(r.failed) << r.message;
    EXPECT_NE(r.log.find("[member] D.g() → int"), std::string::npos) << r.log;
    EXPECT_EQ(r.log.find("(static member"), std::string::npos) << r.log;
}
