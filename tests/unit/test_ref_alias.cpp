// =============================================================================
// tests/unit/test_ref_alias.cpp —— 引用的别名语义（白盒不变量）
// =============================================================================
// 考察理论点：[dcl.ref]/1 —— "A reference is an alias for an object and is not
//   itself an object"（引用是别名，不占存储、不参与析构、没有自己的值）。
//   ⇒ 代码生成上，引用变量的槽里放的是【被绑定对象的地址】：
//     读 = 从该地址间接加载；写 = 往该地址间接存储。
//   ⇒ 引用形参的实参在调用点必须传【地址】，被调点按别名解释那个槽。
//
// ★ 判据单点（docs/BUGS.md B6 的修法）："这个绑定是不是别名"只有一条判据
//   （isAliasBinding）：【有没有被绑定对象的地址】——
//     · 局部引用变量：初始化式可寻址 ⇒ 有地址 ⇒ 别名；
//     · 引用形参：实参在合法程序里必然是左值吗？只有"可写左值引用"能保证
//       （const T& / T&& 的实参可能是右值，右值没有地址可传）⇒ 只有它走别名通道。
//   调用点与被调点读同一份类型、同一条判据 ⇒ 不会"一边传地址一边当值用"。
//   ★ 本文件的每条断言都在钉这个"两侧同源"：只看单侧会漏掉口径漂移。
//
// 断言的是【不变量】而不是症状：
//   ① 引用变量的槽里放的是地址（leaq + movq），不是值的拷贝；
//   ② 读引用走间接加载（movq 槽,%rcx ; movq (%rcx),%rax）；
//   ③ 写引用走间接存储（movq 槽,%rcx ; movq %rax,(%rcx)）；
//   ④ 引用形参：调用点传地址 + 被调点按别名读（两侧同时成立才算口径一致）；
//   ⑤ const 引用形参按值传（边界：实参可能是右值，右值没有地址）；
//   ⑥ 引用绑到成员 / 全局 / 另一个引用时，取的仍是"被绑定对象的地址"；
//   ⑦ 作用域链：同名变量在内层块重新声明 ⇒ 内层用内层的、外层用外层的。
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='RefAlias.*'
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

// 某个名字的槽偏移：形如 `leaq -N(%rbp), %rax ... &name` 的那行。
// ★ 名字单独拼（不塞进 raw string）：raw string 里出现 `)"` 会提前闭合。
int varSlotOf(const std::string& body, const std::string& varName) {
    std::regex re(R"(leaq -(\d+)\(%rbp\), %rax[^\n]*&)" + varName + R"(\b)");
    std::smatch m;
    if (std::regex_search(body, m, re)) return std::stoi(m[1].str());
    return -1;
}

// 某个名字的槽偏移：形如 `movq %rax, -N(%rbp) ... 存储到局部变量 name` 的那行。
// ★ 用途：变量【只被写、没被取址】时（按值传实参），varSlotOf 找不到它，
//   改从它的初始化式那次存储反查槽偏移。
int varStoreSlotOf(const std::string& body, const std::string& varName) {
    std::regex re(R"(movq %rax, -(\d+)\(%rbp\)[^\n]*存储到局部变量 )" + varName + R"(\b)");
    std::smatch m;
    if (std::regex_search(body, m, re)) return std::stoi(m[1].str());
    return -1;
}

// 引用读的特征形态：movq 槽,%rcx 后跟 movq (%rcx),%rax（间接加载）
bool hasIndirectLoad(const std::string& body) {
    return body.find("movq (%rcx), %rax") != std::string::npos;
}

// 引用写的特征形态：movq 槽,%rcx 后跟 movq %rax,(%rcx)（间接存储）
bool hasIndirectStore(const std::string& body) {
    return body.find("movq %rax, (%rcx)") != std::string::npos;
}

// "把引用槽里的地址取出来"这一步：movq -N(%rbp), %rcx
bool hasRefSlotLoad(const std::string& body) {
    std::regex re(R"(movq -(\d+)\(%rbp\), %rcx)");
    return std::regex_search(body, re);
}

// 子串出现次数（用来数"几个类对象走了同一条路径"）
size_t countOccurrences(const std::string& text, const std::string& sub) {
    size_t n = 0, pos = 0;
    while ((pos = text.find(sub, pos)) != std::string::npos) {
        ++n;
        pos += sub.size();
    }
    return n;
}

} // namespace

// =============================================================================
// ① 引用变量的槽里放的是【地址】，不是值的拷贝
//   —— 反面：把 a 的值拷进 r 的槽 ⇒ `r = 30` 改的是 r 自己那份，a 不变
//   （BUGS.md B6 的老症状：静默算错，编译全绿、运行算错）。
// =============================================================================
TEST(RefAlias, T1_LocalRefSlotHoldsAddressNotCopy) {
    std::string asmCode = compileQuiet(R"(
int main() {
    int a = 5;
    int& r = a;
    r = 30;
    return 0;
}
)");
    std::string body = funcBody(asmCode, "main");
    ASSERT_FALSE(body.empty()) << "找不到函数 main";
    int aSlot = varSlotOf(body, "a");
    ASSERT_GE(aSlot, 0) << "找不到变量 a 的槽（leaq ... &a）";
    // 绑定那一步必须是"取 a 的地址"（leaq a 的槽）而不是"加载 a 的值"
    // （movq a 的槽,%rax）—— 后者就是把值拷进 r 的槽。
    EXPECT_NE(body.find("leaq -" + std::to_string(aSlot) + "(%rbp), %rax"), std::string::npos)
        << "引用 r 的绑定没有取 a 的地址 ⇒ r 槽里放的是值的拷贝（别名语义丢失）";
    // 本用例里 a 的值【从头到尾没被读过】⇒ 出现一次"加载 a 的槽"就是拷贝
    EXPECT_EQ(body.find("movq -" + std::to_string(aSlot) + "(%rbp), %rax"), std::string::npos)
        << "引用 r 的绑定加载了 a 的值 ⇒ 那是拷贝不是别名";
}

// =============================================================================
// ② 读引用走间接加载：movq 槽,%rcx ; movq (%rcx),%rax
//   —— 反面：直接加载 r 的槽 ⇒ 读到的是"地址"这个数而不是被绑定对象的值。
// =============================================================================
TEST(RefAlias, T2_ReadingRefLoadsIndirectly) {
    std::string asmCode = compileQuiet(R"(
int main() {
    int a = 5;
    int& r = a;
    int y = r;
    return y;
}
)");
    std::string body = funcBody(asmCode, "main");
    ASSERT_FALSE(body.empty()) << "找不到函数 main";
    EXPECT_TRUE(hasRefSlotLoad(body))
        << "读引用时没有先取引用槽里的地址";
    EXPECT_TRUE(hasIndirectLoad(body))
        << "读引用没有走间接加载（movq (%rcx),%rax）⇒ 读到的是地址不是值";
}

// =============================================================================
// ③ 写引用走间接存储：movq 槽,%rcx ; movq %rax,(%rcx)
//   —— 反面：直接写 r 的槽 ⇒ 被绑定对象全程不受影响 ⇒ 静默算错。
// =============================================================================
TEST(RefAlias, T3_WritingRefStoresIndirectly) {
    std::string asmCode = compileQuiet(R"(
int main() {
    int a = 5;
    int& r = a;
    r = 30;
    return a;
}
)");
    std::string body = funcBody(asmCode, "main");
    ASSERT_FALSE(body.empty()) << "找不到函数 main";
    EXPECT_TRUE(hasRefSlotLoad(body))
        << "写引用时没有先取引用槽里的地址";
    EXPECT_TRUE(hasIndirectStore(body))
        << "写引用没有走间接存储（movq %rax,(%rcx)）⇒ 写的是引用自己的槽";
}

// =============================================================================
// ④ 引用形参：调用点传【地址】+ 被调点按【别名】读 —— 两侧同时成立才算口径一致。
//   ★ 只查一侧会漏掉口径漂移：调用点按值传、被调点按别名读 ⇒ 被调点把一个
//     普通整数当地址解引用 ⇒ 段错误或写坏内存。
// =============================================================================
TEST(RefAlias, T4_RefParamCallSiteAndCalleeAgree) {
    std::string asmCode = compileQuiet(R"(
void bump(int& x) { x = x + 1; }
int main() { int a = 5; bump(a); return a; }
)");
    std::string callee = funcBody(asmCode, "bump");
    std::string caller = funcBody(asmCode, "main");
    ASSERT_FALSE(callee.empty()) << "找不到函数 bump";
    ASSERT_FALSE(caller.empty()) << "找不到函数 main";
    // 被调点：形参 x 是别名 ⇒ 读它走间接加载
    EXPECT_TRUE(hasRefSlotLoad(callee))
        << "被调方没有把引用形参当别名处理（没取引用槽里的地址）";
    EXPECT_TRUE(hasIndirectLoad(callee))
        << "被调方读引用形参没有走间接加载";
    // 调用点：实参取的是地址（leaq 局部槽），不是值
    int aSlot = varSlotOf(caller, "a");
    ASSERT_GE(aSlot, 0) << "找不到变量 a 的槽";
    EXPECT_NE(caller.find("leaq -" + std::to_string(aSlot) + "(%rbp), %rax"), std::string::npos)
        << "调用点没有把 a 的地址传给引用形参 ⇒ 两侧口径不一致";
}

// =============================================================================
// ⑤ const 引用形参按【值】传（边界：实参可能是右值，右值没有地址可取）
//   —— 这条是"边界钉住"：哪天改成按地址传，调用点就必须同步物化临时，
//     否则 `f(5)` 会把字面量的值当地址传 ⇒ 段错误。
// =============================================================================
TEST(RefAlias, T5_ConstRefParamPassedByValue) {
    std::string asmCode = compileQuiet(R"(
int readConst(const int& x) { return x; }
int main() { int a = 5; return readConst(a); }
)");
    std::string caller = funcBody(asmCode, "main");
    ASSERT_FALSE(caller.empty()) << "找不到函数 main";
    int aSlot = varStoreSlotOf(caller, "a");
    ASSERT_GE(aSlot, 0) << "找不到变量 a 的槽";
    // 调用点必须是"加载 a 的值"再传，而不是"取 a 的地址"
    EXPECT_NE(caller.find("movq -" + std::to_string(aSlot) + "(%rbp), %rax"), std::string::npos)
        << "const 引用形参的实参没有按值传";
    EXPECT_EQ(caller.find("leaq -" + std::to_string(aSlot) + "(%rbp), %rax"), std::string::npos)
        << "const 引用形参的实参被按地址传了 —— 实参可能是右值（没有地址）";
}

// =============================================================================
// ⑥ 引用绑到成员 / 全局 / 另一个引用时，取的仍是"被绑定对象的地址"
//   —— 三种绑定对象三种取址方式，但结果都是"一个地址"。
// =============================================================================
TEST(RefAlias, T6_RefBoundToMemberGlobalAndOtherRef) {
    std::string asmCode = compileQuiet(R"(
struct S { int v; };
int g = 1;
int main() {
    S s;
    s.v = 0;
    int& m = s.v;      // 绑到成员
    int& gg = g;       // 绑到全局
    int& r = m;        // 绑到另一个引用
    return r;
}
)");
    std::string body = funcBody(asmCode, "main");
    ASSERT_FALSE(body.empty()) << "找不到函数 main";
    // 绑到成员：对象地址 + 字段偏移（偏移 0 时不加）
    EXPECT_NE(body.find("取栈对象地址 &s"), std::string::npos)
        << "引用绑到成员时没有取对象的地址";
    // 绑到全局：全局标签地址
    EXPECT_NE(body.find("leaq g(%rip)"), std::string::npos)
        << "引用绑到全局时没有取全局标签的地址";
    // 绑到另一个引用：取的是那个引用【持有的地址】
    EXPECT_NE(body.find("持有的地址就是 &被绑定对象"), std::string::npos)
        << "引用绑到另一个引用时没有沿用被绑定对象的地址";
}

// =============================================================================
// ⑦ 作用域链：同名变量在内层块重新声明 ⇒ 内层用内层的、出块后外层恢复。
//   —— 平表（一张 name→形态 的表）会让"内层的别名"盖住"外层的类对象"
//     ⇒ 外层块里 `s.v = 1` 取到内层别名的地址 ⇒ 写坏内存（实测 SIGSEGV）。
// =============================================================================
TEST(RefAlias, T7_SameNameInNestedScopeResolvesToInnermost) {
    std::string asmCode = compileQuiet(R"(
struct S { int v; };
int main() {
    int a = 0;
    {
        S s;             // 外层：s 是类对象
        s.v = 1;
    }
    {
        int& s = a;      // 内层：s 是引用（遮蔽外层同名）
        s = 2;           // 写的是 a，不是外层的类对象
    }
    {
        S s;             // 第三层：s 又是类对象
        s.v = 3;
    }
    return 0;
}
)");
    std::string body = funcBody(asmCode, "main");
    ASSERT_FALSE(body.empty()) << "找不到函数 main";
    // 两个类对象的成员访问都必须走"取栈对象地址"路径 ——
    // 若名字解析串到中间那个引用上，第三个类对象的成员写就会落到别处。
    EXPECT_EQ(countOccurrences(body, "取栈对象地址 &s"), 2u)
        << "两个类对象没有都走类对象取址路径 ⇒ 名字解析串到了别的声明上";
    // 内层块的引用写：走间接存储（写被绑定对象 a）
    EXPECT_TRUE(hasIndirectStore(body))
        << "内层块的引用 s 没有走间接存储";
}
