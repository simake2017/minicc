// =============================================================================
// 单测：类模板里的嵌套类（实例化步骤 5.8 —— 克隆嵌套类）
// =============================================================================
// 考察不变量（断言【性质】而非具体拼写，命名规则随便改都不会误报）：
//   · 嵌套类是【外层类模板的成员】⇒ 必须随实例化一起克隆（[temp.inst]/5）：
//     实例 Box_int 里要有一个登记名 "Box_int::Inner" 的嵌套类。
//   · 嵌套类的字段随【外层形参】替换：`struct Inner { T v; }` 在 Box<int> 里
//     必须是 `int v`（不替换 ⇒ 字段 0 字节、偏移错、"能编译"但全是错的）。
//   · 不同实例 ⇒ 不同嵌套类：Box_int::Inner 与 Box_double::Inner 是两个类型、
//     两套符号（"作用域即身份"：learn/49）。
//   · 嵌套类的方法也要走 Pass 2/Pass 3（注册 + 分析函数体）—— 不走 ⇒
////     CodeGen 从没收到那些方法 ⇒ 链接期 undefined reference。
//   · 嵌套套嵌套：Deep_int::A::B 三级登记名同样成立。
//   · 边界不放大：蓝图里没有的成员名仍然响亮拒收（`Box<int>::Missing`）。
// ★ 判据突变验证（摘下面任一规则 ⇒ 对应用例实跑变红；★ 括号内是**实测**条数，
//   2026-11-03 逐条实跑复核过 —— 判据的"承重"要靠实测，不靠声称）：
//     ① 不克隆嵌套类（步骤 5.8 的循环换成空表）⇒ **5** 个用例变红
//        （261~265 全红；266 仍绿 —— 边界那条本就不依赖克隆）
//     ② 克隆但【不替换】字段类型 ⇒ **2** 个用例变红
//        （NestedFieldTypeIsSubstituted + NestedClassMethodsAreRegistered：
//         后者顺带红是因为方法体里读到的字段类型也跟着错了）
//     ③ 不递归进嵌套类的方法表（Pass 2/3 退回只遍历 instance->methods）⇒
//        **1** 个用例变红（NestedClassMethodsAreRegistered）
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

// ── ① 嵌套类随实例化一起克隆 ──────────────────────────────────────────────

TEST(NestedClassInTemplate, NestedClassIsClonedIntoInstance) {
    auto r = analyze("template <class T> struct Box { struct Inner { T v; }; };"
                     " int main() { Box<int>::Inner i; return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    // 实例类里要有一个登记名带作用域的嵌套类
    EXPECT_NE(r.log.find("[register] class 'Box_int::Inner'"), std::string::npos)
        << r.log;
}

// ── ② 嵌套类的字段随外层形参替换 ──────────────────────────────────────────

TEST(NestedClassInTemplate, NestedFieldTypeIsSubstituted) {
    auto r = analyze("template <class T> struct Box { struct Inner { T v; }; };"
                     " int main() { Box<int>::Inner i; return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    // ★ 断言的是【替换发生了】，不是"最终类型是什么"：
    //   克隆日志形如 "field 'v' : T → int" —— 左边是蓝图形态、右边是替换结果。
    EXPECT_NE(r.log.find("field 'v' : T → int"), std::string::npos) << r.log;
}

// ── ③ 不同实例 ⇒ 不同嵌套类 ───────────────────────────────────────────────

TEST(NestedClassInTemplate, TwoInstancesGetDistinctNestedClasses) {
    auto r = analyze("template <class T> struct Box { struct Inner { T v; }; };"
                     " int main() { Box<int>::Inner i; Box<double>::Inner d;"
                     " return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    EXPECT_NE(r.log.find("[register] class 'Box_int::Inner'"), std::string::npos)
        << r.log;
    EXPECT_NE(r.log.find("[register] class 'Box_double::Inner'"), std::string::npos)
        << r.log;
}

// ── ④ 嵌套类的方法也要注册（Pass 2）── ────────────────────────────────────

TEST(NestedClassInTemplate, NestedClassMethodsAreRegistered) {
    auto r = analyze("template <class T> struct Box {"
                     "   struct Inner { T v; T get() { return v; } }; };"
                     " int main() { Box<int>::Inner i; return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    // 方法的登记名带【嵌套类的登记名】⇒ 符号与别的类的方法不会撞
    EXPECT_NE(r.log.find("[register] Box_int::Inner::get()"), std::string::npos)
        << r.log;
}

// ── ⑤ 嵌套套嵌套 ──────────────────────────────────────────────────────────

TEST(NestedClassInTemplate, NestedInNestedIsCloned) {
    auto r = analyze("template <class T> struct Deep {"
                     "   struct A { struct B { T v; }; }; };"
                     " int main() { Deep<int>::A::B b; return 0; }");
    EXPECT_FALSE(r.failed) << r.message;
    EXPECT_NE(r.log.find("[register] class 'Deep_int::A::B'"), std::string::npos)
        << r.log;
}

// ── ⑥ 边界不放大：蓝图里没有的成员名仍然拒收 ──────────────────────────────

TEST(NestedClassInTemplate, MissingNestedMemberIsAnError) {
    auto r = analyze("template <class T> struct Box { struct Inner { T v; }; };"
                     " int main() { Box<int>::Missing m; return 0; }");
    EXPECT_TRUE(r.failed);
    EXPECT_NE(r.message.find("no type named 'Missing' in 'Box_int'"), std::string::npos)
        << r.message;
}

}  // namespace
