// =============================================================================
// tests/unit/test_member_assign_clobber.cpp —— 成员赋值：对象地址必须活过右值求值
// =============================================================================
// 考察理论点：字段访问的消除（编译器后端的"寻址折叠"）
//   · `obj.field = value` 降级成 `[objAddr + fieldOffset] = value` ——
//     对象地址算一次、字段偏移编译期定死（对照 clang 的
//     clang/lib/CodeGen/CGExpr.cpp:CodeGenFunction::EmitMemberExpr
//     与 clang/lib/CodeGen/TargetInfo.cpp:TargetInfo::getStructLayout）。
//   · ★ 但"算一次"有个前提：**算出来的地址得活到写下去那一刻**。
//     本实现用 %rcx 暂存对象地址，而右值求值会踩 %rcx
//     （二元表达式把右操作数挪进 %rcx；函数调用按约定可踩 caller-saved 寄存器）
//     ⇒ 右值求值夹在"算地址"与"写字段"之间时，地址被冲掉，
//     写到了一个【不是该对象】的地址。
//   · 现场：`s.v = s.v + 4;` ⇒ 二元表达式把 %rcx 改成 4（常量）⇒
//     `movl %eax, 0(%rcx)` 写到地址 0x4 ⇒ 段错误；
//     若 %rcx 恰是别的合法地址 ⇒ 静默踩坏别人的内存（更坏，无症状）。
//   · 修法：对象地址先存栈上，右值求完再取回（push/pop 一对）。
//     ★ 与"右值求值两次"的老简化无关：那次简化是【求值次数】问题，
//       这次是【寄存器存活】问题 —— 两者都在同一条语句上，别混为一谈。
//
// 断言的是【不变量】：写字段之前，对象地址必须被【重新装载】，
// 不能是右值求值留下的残值。
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='MemberAssignClobber.*'
// =============================================================================

#include <gtest/gtest.h>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "obs_helpers.h"

#include <string>
#include <vector>

using namespace minicc;

namespace {

// 编译一段源码，返回生成的汇编（静默吞掉各阶段日志）。
std::string compileQuiet(const std::string& src) {
    StdoutCapture cap;
    Lexer lexer(src);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    TranslationUnit unit = parser.parseTranslationUnit();
    SemanticAnalyzer sema;
    sema.analyze(unit);
    CodeGen cg;
    return cg.generate(unit, sema.getClassTypes(), sema.getFunctions());
}

// 一个函数的汇编片段：从 `<label>:` 那一行起，到下一个 `.globl` 止。
std::string funcBody(const std::string& asmCode, const std::string& label) {
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : asmCode) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) lines.push_back(cur);
    }
    auto strip = [](const std::string& s) {
        auto b = s.find_first_not_of(" \t");
        return b == std::string::npos ? std::string() : s.substr(b);
    };
    size_t begin = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (strip(lines[i]).rfind(label + ":", 0) == 0) { begin = i; break; }
    }
    if (begin == lines.size()) return "";
    std::string body;
    for (size_t i = begin; i < lines.size(); ++i) {
        if (i > begin && strip(lines[i]).rfind(".globl", 0) == 0) break;
        body += lines[i] + "\n";
    }
    return body;
}

} // namespace

// =============================================================================
// ① 右值是二元表达式（会踩 %rcx）⇒ 对象地址必须重新装载
// =============================================================================
TEST(MemberAssignClobber, T1_BinaryValueKeepsObjectAddress) {
    std::string asmCode = compileQuiet(R"(
struct S { int v; };
int main() { S s; s.v = 3; s.v = s.v + 4; return s.v; }
)");
    std::string body = funcBody(asmCode, "main");
    ASSERT_FALSE(body.empty()) << "找不到函数 main";
    size_t load = body.find("对象地址存入 rcx");
    ASSERT_NE(load, std::string::npos) << "成员赋值没有把对象地址装进 rcx";
    size_t store = body.find("写入字段 .v", load);
    ASSERT_NE(store, std::string::npos) << "成员赋值没有写字段";
    std::string between = body.substr(load, store - load);
    EXPECT_NE(between.find("popq %rcx"), std::string::npos)
        << "右值求值之后没有把对象地址取回 %rcx ⇒ 写字段会写到错的地址"
        << "（二元表达式把右操作数挪进了 %rcx）";
}

// =============================================================================
// ② 右值是函数调用（调用按约定可踩 caller-saved 寄存器，含 %rcx）⇒ 同一条不变量
// =============================================================================
TEST(MemberAssignClobber, T2_CallValueKeepsObjectAddress) {
    std::string asmCode = compileQuiet(R"(
struct S { int v; };
int get() { return 7; }
int main() { S s; s.v = 3; s.v = get(); return s.v; }
)");
    std::string body = funcBody(asmCode, "main");
    ASSERT_FALSE(body.empty()) << "找不到函数 main";
    size_t load = body.find("对象地址存入 rcx");
    ASSERT_NE(load, std::string::npos) << "成员赋值没有把对象地址装进 rcx";
    size_t store = body.find("写入字段 .v", load);
    ASSERT_NE(store, std::string::npos) << "成员赋值没有写字段";
    std::string between = body.substr(load, store - load);
    EXPECT_NE(between.find("popq %rcx"), std::string::npos)
        << "右值是函数调用时没有把对象地址取回 %rcx ⇒ 写字段会写到错的地址";
}

// =============================================================================
// ③ 右值是常量（不踩任何寄存器）⇒ 保护仍在，但不影响正确性
//   —— 这条钉住"保护是无条件加的"：不靠"这个右值恰好不踩 rcx"蒙对。
// =============================================================================
TEST(MemberAssignClobber, T3_ConstantValueAlsoProtected) {
    std::string asmCode = compileQuiet(R"(
struct S { int v; };
int main() { S s; s.v = 3; s.v = 9; return s.v; }
)");
    std::string body = funcBody(asmCode, "main");
    ASSERT_FALSE(body.empty()) << "找不到函数 main";
    size_t load = body.find("对象地址存入 rcx");
    ASSERT_NE(load, std::string::npos) << "成员赋值没有把对象地址装进 rcx";
    size_t store = body.find("写入字段 .v", load);
    ASSERT_NE(store, std::string::npos) << "成员赋值没有写字段";
    std::string between = body.substr(load, store - load);
    EXPECT_NE(between.find("popq %rcx"), std::string::npos)
        << "对象地址的保护应当无条件生效（不靠右值恰好不踩 %rcx）";
}
