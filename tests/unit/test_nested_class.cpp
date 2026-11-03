// =============================================================================
// 单测：嵌套类（[class.nest]，Stage 3 批 4）
// =============================================================================
// 考察不变量（断言【性质】而非具体拼写，命名规则随便改都不会误报）：
//   · 嵌套类的登记名带作用域：外层类 A 里的 B 登记成 "A::B" ⇒ 类表键 /
//     符号表键 / mangling / 汇编符号全带作用域。
//   · 同名共存不串味：A::B 与全局 B 是【两个类型、两个符号、两个 vtable】——
//     嵌套类的名字绝不能被"归一"成裸名（否则两个类的字段/方法互相覆盖）。
//   · 类作用域优先于全局：A 的成员函数里的裸名 B 必须解析成 A::B，
//     不是全局 B（clang 同款；此前全局符号表先命中 ⇒ 静默绑错，不报错只算错）。
//   · 类自身的名字在本类作用域内可见（[basic.scope.class]/2.1）：
//     `struct A { A* p; };` 是合法程序（自引用指针），此前被拒收。
//   · 嵌套类看得见外层类的 static 成员（嵌套类是外层类的成员）。
//   · 限定名调用按登记名查表：`B::sm()` 的限定者按可见性解析后改写成登记名
//     （可见名 "B" ≠ 登记名 "A::B"）。
//   · 查不到就是查不到：`A::C`（C 不存在）报 "no type named 'C' in 'A'"，
//     不退到全局的同名类型。
// ★ 判据突变验证（摘下面任一规则 ⇒ 对应用例实跑变红）：
//     ① 嵌套类不前缀化（登记成裸名 "B"）⇒ NestedAndGlobalStayDistinct 变红
//     ② 类作用域链不查嵌套类（①b 分支去掉）⇒ UnqualifiedNameResolvesToNested 变红
//     ③ 类自身的名字不可见（①0 分支去掉）⇒ ClassNameVisibleInOwnScope 变红
//     ④ 限定名调用不按登记名改写 ⇒ QualifiedCallUsesRegisteredName 变红
//     ⑤ 缺成员时不报错（限定名兜底去掉）⇒ MissingNestedMemberIsAnError 变红
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

// 取出日志里 "mangled: <符号>" 那一串（用于断言符号互不相同）
std::string mangledOf(const std::string& log, const std::string& funcFullName) {
    const auto pos = log.find("[register] " + funcFullName + "(");
    if (pos == std::string::npos) return "";
    const auto m = log.find("[mangled: ", pos);
    if (m == std::string::npos) return "";
    const auto open = m + 10;
    const auto close = log.find(']', open);
    return log.substr(open, close - open);
}

// ── 嵌套类的登记名带作用域 ────────────────────────────────────────────────

TEST(NestedClass, RegisteredUnderQualifiedName) {
    auto r = analyze("struct A { struct B { int v; }; }; int main() { return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    // 嵌套类的登记名必须是限定名 "A::B"
    EXPECT_NE(r.log.find("[register] class 'A::B'"), std::string::npos) << r.log;
}

// ── 同名共存：A::B 与全局 B 是两个实体 ───────────────────────────────────
// ★ 反向断言：不得出现"归一成裸名"的登记（否则两个类互相覆盖）。

TEST(NestedClass, NestedAndGlobalStayDistinct) {
    auto r = analyze("struct A { struct B { int v; int get() { return v; } }; };"
                     " struct B { int w; int get() { return w; } };"
                     " int main() { return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    EXPECT_NE(r.log.find("[register] class 'A::B'"), std::string::npos) << r.log;
    EXPECT_NE(r.log.find("[register] class 'B'"), std::string::npos) << r.log;
    // 两个 get 的发射符号必须不同（否则汇编期撞符号）
    const std::string nested = mangledOf(r.log, "A::B::get");
    const std::string global = mangledOf(r.log, "B::get");
    EXPECT_FALSE(nested.empty()) << r.log;
    EXPECT_FALSE(global.empty()) << r.log;
    EXPECT_NE(nested, global) << r.log;
}

// ── 类作用域优先于全局：裸名 B 在 A 里指 A::B ─────────────────────────────
// clang: A 的成员函数里 `B b;` 的 B 是 A::B。

TEST(NestedClass, UnqualifiedNameResolvesToNested) {
    auto r = analyze("struct A { struct B { int v; }; int use() { B b; return b.v; } };"
                     " struct B { int w; };"
                     " int main() { return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    EXPECT_NE(r.log.find("[sema:nested] B 在类 'A' 作用域内"), std::string::npos) << r.log;
}

// ── 类自身的名字在本类作用域内可见（[basic.scope.class]/2.1）──────────────
// ★ 自引用指针是极常见的惯用法，此前被当成"未知类型名"拒收（合法程序被拒）。

TEST(NestedClass, ClassNameVisibleInOwnScope) {
    auto r = analyze("struct A { A* p; int v; }; int main() { A a; a.v = 1; return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    EXPECT_EQ(r.log.find("unknown type name 'A'"), std::string::npos) << r.log;
}

// ── 嵌套类看得见外层类的 static 成员 ─────────────────────────────────────

TEST(NestedClass, NestedSeesEnclosingStaticMember) {
    auto r = analyze("struct Outer { static int sval; struct Inner {"
                     " int get() { return sval; } }; };"
                     " int Outer::sval = 10;"
                     " int main() { Outer::Inner i; return i.get(); }");
    EXPECT_FALSE(r.failed) << r.message;
    EXPECT_EQ(r.log.find("use of undeclared identifier 'sval'"), std::string::npos)
        << r.log;
}

// ── 限定名调用按登记名查表（可见名 ≠ 登记名）──────────────────────────────

TEST(NestedClass, QualifiedCallUsesRegisteredName) {
    auto r = analyze("struct A { struct B { static int sm() { return 5; } };"
                     " int use() { return B::sm(); } };"
                     " int main() { return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    // 调用点必须把可见名 "B::sm" 改写成登记名 "A::B::sm"
    EXPECT_NE(r.log.find("[call] 限定名 'B::sm' ⇒ 登记名 'A::B::sm'"), std::string::npos)
        << r.log;
}

// ── 查不到就是查不到（不退到全局的同名类型）──────────────────────────────
// clang: error: no type named 'C' in 'A'

TEST(NestedClass, MissingNestedMemberIsAnError) {
    auto r = analyze("struct A { struct B { int v; }; };"
                     " int main() { A::C c; return 0; }");
    EXPECT_TRUE(r.failed);
    EXPECT_NE(r.message.find("no type named 'C' in 'A'"), std::string::npos)
        << r.message;
}

} // namespace
