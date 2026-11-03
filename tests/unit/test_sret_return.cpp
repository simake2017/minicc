// =============================================================================
// tests/unit/test_sret_return.cpp —— 按值返回类对象：sret 形态与拷贝省略（白盒不变量）
// =============================================================================
// 考察理论点：按值返回【类对象】的存储与调用约定（System V AMD64 ABI / Itanium ABI）
//   · 类对象没有"标量值"表示 ⇒ 按值返回不能走 rax 装值，只能走 sret：
//     调用者给一块缓冲（隐藏的第 0 参数，rdi），被调者把对象表示拷进去，
//     返回值 rax = 缓冲地址（Itanium ABI §3.2.3 "Passing and Returning
//     Aggregates"；对照 clang 的 clang/lib/CodeGen/CGCall.cpp:
//     CodeGenFunction::EmitReturn 与 X86_64ABIInfo::classifyReturnType）。
//   · 拷贝省略（[class.copy.elision]/8）：`S x = f();` 里 prvalue 直接
//     materialize 成 x ⇒ 返回缓冲【就是 x 的槽】，不产生第二个对象。
//   · ★ 判据单点："这次调用按值返回类"只看【返回类型是不是类】——
//     调用点与定义点读同一个 Type ⇒ 两侧天然一致。
//     ★ 这里的"一致"是本文件全部断言的根：判据一旦两边不同源，
//       调用点不传缓冲 / 被调点等着缓冲 ⇒ 静默算错或段错误。
//
// 断言的是【不变量】而不是症状：
//   ① 定义点：序言必须 spill 隐藏返回缓冲指针，返回路径必须逐字节拷进该缓冲；
//   ② 调用点：必须把一块缓冲地址装进 rdi，且表达式的值 = 那块缓冲；
//   ③ 拷贝省略：那块缓冲的偏移必须【等于】目标变量的槽偏移（不是临时区的另一块）；
//   ④ 寄存器位移：sret 时显式实参从 rsi 起、成员函数的 this 让到 rsi；
//   ⑤ 返回引用【不是】按值返回 ⇒ 不许走 sret（判据只剥 const、不剥引用）；
//   ⑥ 帧要盖住那块缓冲（否则缓冲落在 rsp 之下 ⇒ 被 pushq / callq 踩掉）。
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='SretReturn.*'
// =============================================================================

#include <gtest/gtest.h>

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "obs_helpers.h"

#include <algorithm>
#include <regex>
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
// ★ 不能只按子串找结束标记：符号导出行缩进 8 空格、标签行 4 空格，
//   混用会一路吞到文件尾（把别的函数的最深偏移算进来）。
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

// 帧大小 N（序言里的 `subq $N, %rsp`）
int frameSizeOf(const std::string& body) {
    std::smatch m;
    std::regex re(R"(subq \$(\d+), %rsp)");
    if (std::regex_search(body, m, re)) return std::stoi(m[1]);
    return -1;
}

// 该函数体里最深（绝对值最大）的 %rbp 负偏移
int deepestLocalOffset(const std::string& body) {
    std::regex re(R"(-(\d+)\(%rbp\))");
    int deepest = 0;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re);
         it != std::sregex_iterator(); ++it) {
        deepest = std::max(deepest, std::stoi((*it)[1].str()));
    }
    return deepest;
}

// ★ sret 路径的标记：序言里那行"保存隐藏返回缓冲指针"（语义标记，不是排版细节）。
bool hasHiddenPointerSpill(const std::string& body) {
    return body.find("隐藏返回缓冲指针") != std::string::npos;
}

// 该函数体里所有 `leaq -N(%rbp), %rdi` 的偏移（调用点装返回缓冲用的那种）
std::vector<int> bufferOffsets(const std::string& body) {
    std::vector<int> out;
    std::regex re(R"(leaq -(\d+)\(%rbp\), %rdi)");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re);
         it != std::sregex_iterator(); ++it) {
        out.push_back(std::stoi((*it)[1].str()));
    }
    return out;
}

// 取某个变量的槽偏移：形如 `leaq -N(%rbp), %rax ... &name` 的那行
int varSlotOf(const std::string& body, const std::string& varName) {
    // ★ 名字单独拼（不塞进 raw string）：raw string 里出现 `)"` 会提前闭合，
    //   把后面的拼接一起吞进去 ⇒ 变量名被当成字面量（正则不匹配）。
    std::regex re(R"(leaq -(\d+)\(%rbp\), %rax[^\n]*&)" + varName + R"(\b)");
    std::smatch m;
    if (std::regex_search(body, m, re)) return std::stoi(m[1].str());
    return -1;
}

} // namespace

// =============================================================================
// ① 定义点：按值返回类的函数必须 spill 隐藏返回缓冲指针，并把对象拷进去
//   —— 反面：把对象的地址（而不是拷贝）返回 ⇒ 调用方拿到的是 callee 的
//   栈帧地址，函数一返回就是悬垂指针（静默算错）。
// =============================================================================
TEST(SretReturn, T1_DefinitionSpillsHiddenPointerAndCopies) {
    std::string asmCode = compileQuiet(R"(
struct S { int v; };
S make() { S s; s.v = 7; return s; }
int main() { S x = make(); return x.v; }
)");
    std::string body = funcBody(asmCode, "make");
    ASSERT_FALSE(body.empty()) << "找不到函数 make";
    EXPECT_TRUE(hasHiddenPointerSpill(body))
        << "按值返回类的函数没有 spill 隐藏返回缓冲指针 ⇒ 调用方无处放对象";
    EXPECT_NE(body.find("rep movsb"), std::string::npos)
        << "返回路径没有把对象逐字节拷进返回缓冲 ⇒ 返回的是 callee 帧内的地址";
}

// =============================================================================
// ② 调用点：必须把缓冲地址装进 rdi（隐藏第 0 参数），且表达式的值 = 那块缓冲
// =============================================================================
TEST(SretReturn, T2_CallSitePassesBufferInRdi) {
    std::string asmCode = compileQuiet(R"(
struct S { int v; };
S make() { S s; s.v = 7; return s; }
int main() { S x = make(); return x.v; }
)");
    std::string body = funcBody(asmCode, "main");
    ASSERT_FALSE(body.empty()) << "找不到函数 main";
    std::vector<int> bufs = bufferOffsets(body);
    ASSERT_FALSE(bufs.empty())
        << "调用点没有 `leaq -N(%rbp), %rdi` ⇒ 隐藏返回缓冲参数没传";
    EXPECT_NE(body.find("# rdi = 返回缓冲"), std::string::npos)
        << "调用点没有把返回缓冲装进 rdi";
}

// =============================================================================
// ③ 拷贝省略：那块缓冲必须【就是】目标变量的槽（不是临时区的另一块）
//   —— 若不是同一块，返回值会留在临时区，x 拿到的仍是零 ⇒ 静默算错。
// =============================================================================
TEST(SretReturn, T3_CopyElisionBufferIsTheVariableSlot) {
    std::string asmCode = compileQuiet(R"(
struct S { int v; };
S make() { S s; s.v = 7; return s; }
int main() { S x = make(); return x.v; }
)");
    std::string body = funcBody(asmCode, "main");
    ASSERT_FALSE(body.empty()) << "找不到函数 main";
    int varSlot = varSlotOf(body, "x");
    ASSERT_GE(varSlot, 0) << "找不到变量 x 的槽（leaq ... &x）";
    std::vector<int> bufs = bufferOffsets(body);
    ASSERT_FALSE(bufs.empty()) << "调用点没有返回缓冲";
    EXPECT_EQ(bufs.front(), varSlot)
        << "返回缓冲在 -" << bufs.front() << "，而变量 x 在 -" << varSlot
        << " ⇒ 拷贝省略没发生（返回值没写进 x 的槽）";
}

// =============================================================================
// ④ 寄存器位移：sret 时显式实参从 rsi 起（rdi 归缓冲指针）
//   —— 定义点与调用点必须同口径，否则形参整体错位一格 ⇒ 静默算错。
// =============================================================================
TEST(SretReturn, T4_SretShiftsExplicitArgsToRsi) {
    std::string asmCode = compileQuiet(R"(
struct S { int v; };
S make(int k) { S s; s.v = k; return s; }
int main() { S x = make(7); return x.v; }
)");
    std::string body = funcBody(asmCode, "make");
    ASSERT_FALSE(body.empty()) << "找不到函数 make";
    // 形参 k 的 spill 必须取 rsi（不是 rdi）
    EXPECT_NE(body.find("movq %rsi,"), std::string::npos)
        << "sret 函数的形参没有从 rsi 取 ⇒ 与调用点的实参寄存器口径不一致";
    // 调用点：实参装 rsi（缓冲占 rdi）
    std::string mainBody = funcBody(asmCode, "main");
    EXPECT_NE(mainBody.find("popq %rsi"), std::string::npos)
        << "调用点没有把实参装进 rsi";
}

// =============================================================================
// ⑤ 成员函数的 this 在 sret 时让到 rsi（rdi 归缓冲指针）
// =============================================================================
TEST(SretReturn, T5_MemberThisMovesToRsiUnderSret) {
    std::string asmCode = compileQuiet(R"(
struct S { int v; };
struct Holder { S make() { S s; s.v = 7; return s; } };
int main() { Holder h; S x = h.make(); return x.v; }
)");
    std::string body = funcBody(asmCode, "Holder_make");
    ASSERT_FALSE(body.empty()) << "找不到函数 Holder_make";
    EXPECT_TRUE(hasHiddenPointerSpill(body)) << "成员方法按值返回类却没有 sret 形态";
    // this 的 spill 必须取 rsi
    EXPECT_NE(body.find("movq %rsi,"), std::string::npos)
        << "sret 成员方法的 this 没有让到 rsi";
}

// =============================================================================
// ⑥ 返回引用【不是】按值返回 ⇒ 不许走 sret（判据只剥 const、不剥引用）
//   —— 误判成 sret ⇒ 调用方白传一块缓冲、被调方往里拷 ⇒ 引用语义被改写。
// =============================================================================
TEST(SretReturn, T6_ReferenceReturnIsNotSret) {
    std::string asmCode = compileQuiet(R"(
struct S { int v; };
S g;
S& pick() { return g; }
int main() { return pick().v; }
)");
    std::string body = funcBody(asmCode, "pick");
    ASSERT_FALSE(body.empty()) << "找不到函数 pick";
    EXPECT_FALSE(hasHiddenPointerSpill(body))
        << "返回引用的函数被当成了按值返回类 ⇒ 引用语义被改写";
    EXPECT_EQ(body.find("rep movsb"), std::string::npos)
        << "返回引用的函数不该有对象拷贝";
}

// =============================================================================
// ⑦ 帧要盖住那块返回缓冲（缓冲落在 rsp 之下 ⇒ 被 pushq / callq 踩掉）
// =============================================================================
TEST(SretReturn, T7_FrameCoversTheReturnBuffer) {
    std::string asmCode = compileQuiet(R"(
struct S { int v; };
S make() { S s; s.v = 7; return s; }
int main() {
    S x = make();
    int a = 1; int b = 2; int c = 3; int d = 4;
    int e = 5; int f = 6; int g = 7; int h = 8;
    return x.v + h;
}
)");
    for (const char* label : {"make", "main"}) {
        std::string body = funcBody(asmCode, label);
        ASSERT_FALSE(body.empty()) << "找不到函数 " << label;
        int frame = frameSizeOf(body);
        int deepest = deepestLocalOffset(body);
        ASSERT_GE(frame, 0) << label << " 没有 subq $N, %rsp 序言";
        EXPECT_GE(frame, deepest)
            << "函数 " << label << "：帧 " << frame << " 字节，却访问到 -"
            << deepest << "(%rbp) —— 局部/缓冲落在 rsp 之下";
    }
}
