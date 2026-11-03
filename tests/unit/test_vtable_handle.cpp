// =============================================================================
// tests/unit/test_vtable_handle.cpp —— vtable 槽挂的是【声明句柄】而非符号串
// =============================================================================
// 考察理论点：
//   · 对照 clang：vtable 的槽里放的是 **CXXMethodDecl 句柄**
//     （clang/lib/CodeGen/VTables.cpp:VTableComponent ⇒ GlobalDecl），
//     符号名由同一个 MangleContext 产出 ⇒ 结构上不可能出现
//     "槽里的名字"与"定义的名字"不一致。
//   · 本实现此前在两处各算一遍符号（Pass 1 算槽、Pass 2 算定义）。
//     两处虽共用 memberMethodSymbolName，却跑在不同时刻、看到不同状态的
//     形参类型（Pass 1 未解析 / Pass 2 已解析）⇒ 同一方法两个符号
//     ⇒ .quad 命中不到 .globl ⇒ 链接期 undefined reference（docs/BUGS.md B45）。
//   · 本套件钉的是【不变量】而不是具体名字：
//     ① 每个槽都有句柄，句柄上的符号非空；
//     ② 槽的符号 **就是** 句柄上的符号（同一个来源，不是另算一遍）；
//     ③ 汇编里那条 .quad 的操作数 == 句柄上的符号（端到端同源）；
//     ④ 覆写时槽换的是【句柄】（指向派生类的声明），基类自己的表不动；
//     ⑤ 析构槽由声明名判定（"~X"），不靠符号串里的 "dtor" 子串。
//     命名规则随便改（后缀、Itanium 化、换分隔符），只要"槽 → 声明 → 符号"
//     三者仍同源，本测试就有意义。
//
// 与 tests/unit/test_vtable_symbols.cpp 互补：那条管"每个 .quad 都有 .globl"
//   （链接期可解析性），本条管"那条 .quad 是从声明句柄来的"（同源性）。
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='VTableHandle.*'
// =============================================================================

#include <gtest/gtest.h>

#include "ast.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "obs_helpers.h"

#include <string>
#include <unordered_map>
#include <vector>

using namespace minicc;

namespace {

// 源码 → (汇编, 类表)。语义日志全部吞掉。
struct Compiled {
    std::string asmCode;
    std::unordered_map<std::string, TypePtr> classTypes;
};

Compiled compileQuiet(const std::string& src) {
    StdoutCapture cap;
    Lexer lexer(src);
    auto tokens = lexer.tokenizeAll();
    Parser parser(std::move(tokens));
    TranslationUnit unit = parser.parseTranslationUnit();
    SemanticAnalyzer sema;
    sema.analyze(unit);
    CodeGen cg;
    Compiled out;
    out.classTypes = sema.getClassTypes();
    out.asmCode = cg.generate(unit, sema.getClassTypes(), sema.getFunctions());
    return out;
}

// 该类的全部 vtable 槽（主表）
std::vector<const VTableEntry*> slotsOf(const Compiled& c, const std::string& cls) {
    std::vector<const VTableEntry*> out;
    auto it = c.classTypes.find(cls);
    if (it == c.classTypes.end()) return out;
    for (const auto& e : it->second->classLayout.vtableEntries) out.push_back(&e);
    return out;
}

}  // namespace

// ── ① 每个槽都有声明句柄，且句柄上的符号非空 ─────────────────────────────────
TEST(VTableHandle, EverySlotHasDeclarationHandle) {
    const char* src = R"(
class A { public: virtual int f() { return 1; } virtual int g(int k) { return k; } };
class B : public A { public: int f() { return 2; } };
int main() { B b; A* p = &b; return p->f() - 2; }
)";
    Compiled c = compileQuiet(src);
    for (const char* cls : {"A", "B"}) {
        auto slots = slotsOf(c, cls);
        ASSERT_FALSE(slots.empty()) << "类 " << cls << " 没有 vtable 槽 —— 用例前提不成立";
        for (const auto* e : slots) {
            ASSERT_NE(e->method, nullptr)
                << "类 " << cls << " 的槽没有挂声明句柄 —— 槽里不该再存第二份符号串";
            EXPECT_FALSE(e->method->mangledName.empty())
                << "类 " << cls << " 的槽所挂声明没有符号（Pass 2 未给它定名？）";
        }
    }
}

// ── ② 槽的符号【就是】句柄上的符号（同一来源，不是另算一遍）──────────────────
// ★ 用例挑的是【带形参】的方法：符号带 "_<形参个数>" 后缀 ⇒ 若 accessor 改成
//   "自己拼一个"（类名_方法名），两者立刻不等 ⇒ 本条立刻红。
TEST(VTableHandle, SlotSymbolIsTheDeclarationSymbol) {
    const char* src = R"(
class A { public: virtual int f(int k) { return k; } };
int main() { A a; return a.f(1) - 1; }
)";
    Compiled c = compileQuiet(src);
    auto slots = slotsOf(c, "A");
    ASSERT_EQ(slots.size(), 1u);
    ASSERT_NE(slots[0]->method, nullptr);
    EXPECT_EQ(vtableEntrySymbol(*slots[0]), slots[0]->method->mangledName)
        << "槽的符号与它挂的声明上的符号不是同一个 —— 槽里仍有第二份符号的残影";
}

// ── ③ 端到端同源：汇编里那条 .quad 的操作数 == 句柄上的符号 ─────────────────
// ★ 这一条是 B45 的直接见证：旧实现里槽的符号是 Pass 1 另算的
//   （未解析的形参类型）⇒ 与句柄（Pass 2 已解析）上的符号不同 ⇒
//   .quad 指向一个没人定义的符号 ⇒ 链接期 undefined reference。
TEST(VTableHandle, QuadOperandComesFromTheHandle) {
    const char* src = R"(
class Outer {
public:
    struct Inner { int v; };
    virtual int f(Inner i) { return i.v; }   // 形参类型 = 嵌套类
    virtual int f(int i) { return i; }       // 同名同个数 ⇒ 符号要挂形参类型链
};
int main() { Outer o; Outer::Inner in; in.v = 3; return o.f(in) - 3; }
)";
    Compiled c = compileQuiet(src);
    auto slots = slotsOf(c, "Outer");
    ASSERT_EQ(slots.size(), 2u);
    for (const auto* e : slots) {
        ASSERT_NE(e->method, nullptr) << "槽没有声明句柄";
        // ★ 期望值取【声明自己的】mangledName，**不经过** vtableEntrySymbol ——
        //   否则这条断言是"自己验自己"：accessor 被改成什么，期望值就跟着变，
        //   突变照样全绿（教训：守恒式/自指式不变量挡不住互换型缺陷，见 B22）。
        const std::string declSym = e->method->mangledName;
        ASSERT_FALSE(declSym.empty()) << "声明上没有符号";
        EXPECT_NE(c.asmCode.find(".quad " + declSym), std::string::npos)
            << "汇编里没有 .quad " << declSym
            << " —— 槽发射用的不是句柄给出的符号（两处各算一遍 ⇒ 链接期错配）";
    }
}

// ── ④ 覆写时换的是【句柄】：派生类的槽指向派生类的声明 ───────────────────────
TEST(VTableHandle, OverrideSwapsHandleNotAString) {
    const char* src = R"(
class A { public: virtual int f() { return 1; } };
class B : public A { public: int f() { return 2; } };
int main() { B b; A* p = &b; return p->f() - 2; }
)";
    Compiled c = compileQuiet(src);
    auto aSlots = slotsOf(c, "A");
    auto bSlots = slotsOf(c, "B");
    ASSERT_EQ(aSlots.size(), 1u);
    ASSERT_EQ(bSlots.size(), 1u);
    EXPECT_EQ(aSlots[0]->method->ownerClassName, "A")
        << "基类自己的表不该被派生类改写";
    EXPECT_EQ(bSlots[0]->method->ownerClassName, "B")
        << "派生类的槽没指向派生类的声明 —— 覆写时改的是别的东西";
}

// ── ⑤ 析构槽由声明名判定，不靠符号串里的 "dtor" 子串 ─────────────────────────
TEST(VTableHandle, DtorSlotIsDecidedByDeclarationName) {
    const char* src = R"(
class W {
public:
    virtual int my_dtor_helper() { return 7; }   // 名字含 "dtor" 但不是析构
    virtual ~W() {}
};
int main() { W w; return w.my_dtor_helper() - 7; }
)";
    Compiled c = compileQuiet(src);
    auto slots = slotsOf(c, "W");
    ASSERT_EQ(slots.size(), 2u);
    int dtorSlots = 0;
    int helperSlots = 0;
    for (const auto* e : slots) {
        if (e->method->name.starts_with("~")) ++dtorSlots;
        if (e->method->name == "my_dtor_helper") ++helperSlots;
    }
    EXPECT_EQ(dtorSlots, 1) << "应当恰好有一个析构槽（判据：声明名以 ~ 开头）";
    EXPECT_EQ(helperSlots, 1) << "my_dtor_helper 的槽不见了";
    // ★ 子串启发式（符号串里找 "dtor"）会把 my_dtor_helper 也算成析构 ——
    //   那样作用域结束时会拿它当析构调用 ⇒ 真正的析构根本没跑。
    for (const auto* e : slots) {
        if (e->method->name != "my_dtor_helper") continue;
        EXPECT_FALSE(e->method->name.starts_with("~"))
            << "my_dtor_helper 不是析构，却被当成析构槽了";
    }
}

// ── ⑥ 析构的虚调用必须打在【析构槽】上 ───────────────────────────────────────
// ★ 这一条直接盯住 codegen 的判据：作用域结束时的析构调用落在哪个槽。
//   旧的子串启发式会把 my_dtor_helper（槽 0）当成析构 ⇒ 虚调用打在槽 0
//   ⇒ 真正的析构（槽 1）根本没跑（静默少调一次，用户看不见）。
//   判据读法：CodeGen::emitVirtualCall 会把槽号写进注释 "VIRTUAL CALL: W::dtor (vtable[N])"。
TEST(VTableHandle, VirtualDtorCallTargetsTheDtorSlot) {
    const char* src = R"(
class W {
public:
    virtual int my_dtor_helper() { return 7; }   // 槽 0 —— 不是析构
    virtual ~W() {}                              // 槽 1 —— 真正的析构
};
int main() { W w; return w.my_dtor_helper() - 7; }
)";
    Compiled c = compileQuiet(src);
    EXPECT_NE(c.asmCode.find("VIRTUAL CALL: W::dtor (vtable[1])"), std::string::npos)
        << "析构的虚调用没打在析构槽上 —— 槽位判据退回了子串匹配（BUGS.md B45）";
    EXPECT_EQ(c.asmCode.find("VIRTUAL CALL: W::dtor (vtable[0])"), std::string::npos)
        << "析构的虚调用打到了 my_dtor_helper 那个槽上 —— 真正的析构不会被调用";
}
