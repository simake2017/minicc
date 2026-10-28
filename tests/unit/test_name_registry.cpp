// =============================================================================
// 单测：名字登记表（NameRegistry）——parser 阶段名字识别的判定依据
// =============================================================================
// 考察不变量（不钉具体容器/日志，命名规则随便改都不会误报）：
//   · 收集：五种声明形状各置对应分类位；template 头后的 struct/class/using
//     名【不】置 type 位（`Box;` 对类模板名保持非类型解释）；
//     namespace 别名收 namespace 位；作用域栈按花括号深度配对。
//   · 查询：未命中返回默认对象（四位全 false）——spec 异常 1 的
//     「禁止反向推断」靠这个默认形状表达。
//   · classifyQualified：scope 精确匹配（全局 B 配不上 scope=N ——
//     basics_24 误绑负例的判据级单测）。
//   · value 位恒 false（拍板 1：占位，不参与任何判定）。
// ★ 判据突变验证（摘下面任一规则 ⇒ 对应用例实跑变红）：
//     ① parser.cpp 语句层 type 位判定 ⇒ basics_23 正例无 [parse:stmt] 行
//     ② semantic_analyzer.cpp 裁决回退尾剥 ⇒ basics_24 负例 rc 变 0
//     ③ name_registry.cpp struct/class type 位规则 ⇒ 本文件 Collects.TypeBit 变红
// =============================================================================

#include "name_registry.h"
#include "lexer.h"
#include "obs_helpers.h"

#include <gtest/gtest.h>

using namespace minicc;

namespace {

// 源码 → 登记表（collect 全程零日志；Lexer 的日志被吞掉保险起见）
NameRegistry collectFrom(const std::string& src) {
    StdoutCapture cap;
    Lexer lexer(src);
    auto tokens = lexer.tokenizeAll();
    NameRegistry reg;
    reg.collect(tokens);
    return reg;
}

// ── 收集 ─────────────────────────────────────────────────────────────────────

TEST(NameRegistryCollects, TypeBit) {
    auto reg = collectFrom(R"(
        struct S1 { int x; };
        class C1 { public: int y; };
        enum E1 { A };
        enum class E2 { B };
        typedef unsigned long U1;
        using A1 = int;
    )");
    EXPECT_TRUE(reg.classify("S1").isType);
    EXPECT_TRUE(reg.classify("C1").isType);
    EXPECT_TRUE(reg.classify("E1").isType);
    EXPECT_TRUE(reg.classify("E2").isType);
    EXPECT_TRUE(reg.classify("U1").isType);   // typedef：';' 前末标识符
    EXPECT_TRUE(reg.classify("A1").isType);   // using X = ...（Ident + '=' 形）
}

TEST(NameRegistryCollects, TemplateHeadDoesNotSetTypeBit) {
    // 类模板名只有 template 位：`Box;` 应保持【非类型】解释
    // （模板名不是类型名，[basic.scope.scope] 同名词不同类）
    auto reg = collectFrom("template<class T> struct Box { T v; };");
    EXPECT_TRUE(reg.classify("Box").isTemplate);
    EXPECT_FALSE(reg.classify("Box").isType);
}

TEST(NameRegistryCollects, AliasTemplateNotTypeBit) {
    auto reg = collectFrom(
        "template<class T> struct Box { T v; };"
        "template<class T> using Vec = Box<T>;");
    EXPECT_TRUE(reg.classify("Vec").isTemplate);
    EXPECT_FALSE(reg.classify("Vec").isType);
}

TEST(NameRegistryCollects, NamespaceBitAndScopeStack) {
    auto reg = collectFrom(R"(
        namespace N1 {
            struct S1 {};
            namespace N2 {
                struct S2 {};
            }
            struct S3 {};   // N2 已弹出：归 N1，不是 N1::N2
        }
        namespace Alias = N1;
        struct S4 {};       // 回到全局
    )");
    EXPECT_TRUE(reg.classifyQualified("N1", "S1").isType);
    EXPECT_TRUE(reg.classifyQualified("N1::N2", "S2").isType);
    EXPECT_TRUE(reg.classifyQualified("N1", "S3").isType);
    EXPECT_FALSE(reg.classifyQualified("N1::N2", "S3").isType);  // 弹栈判据
    EXPECT_TRUE(reg.classifyQualified("", "S4").isType);
    EXPECT_TRUE(reg.classify("Alias").isNamespace);              // 命名空间别名
}

TEST(NameRegistryCollects, ClassBodyNamesBelongToOuterScope) {
    // 类体不入作用域栈：类内 typedef 归外层（宽方向近似，spec 异常 1 注）
    auto reg = collectFrom(R"(
        namespace N { struct Outer { typedef int Inner; }; }
    )");
    EXPECT_TRUE(reg.classifyQualified("N", "Inner").isType);
}

// ── 查询语义 ─────────────────────────────────────────────────────────────────

TEST(NameRegistryQuery, MissReturnsAllFalse) {
    auto reg = collectFrom("struct S {}; namespace N {}");
    const auto c = reg.classify("Qux");
    EXPECT_FALSE(c.isType);
    EXPECT_FALSE(c.isNamespace);
    EXPECT_FALSE(c.isTemplate);
    EXPECT_FALSE(c.isValue);   // 占位位恒 false（拍板 1）
}

TEST(NameRegistryQuery, TailApproximationForParserSide) {
    // Parser 侧（classify）：限定名全名/末段近似（B31 语义原样升级）
    auto reg = collectFrom("namespace N { template<class T> struct B { T v; }; }");
    EXPECT_TRUE(reg.classify("N::B").isTemplate);
    EXPECT_TRUE(reg.classifyQualified("N", "B").isTemplate);
}

TEST(NameRegistryQuery, GlobalEntryDoesNotMatchNamespacedScope) {
    // ★ basics_24 误绑负例的判据：全局模板 B 配不上 scope=N
    auto reg = collectFrom(R"(
        template<class T> struct B { T w; };
        namespace N {}
    )");
    EXPECT_TRUE(reg.classifyQualified("", "B").isTemplate);   // 全局确有 B
    const auto c = reg.classifyQualified("N", "B");
    EXPECT_FALSE(c.isTemplate);                               // 但 N 里没有
    EXPECT_FALSE(c.isType);
}

TEST(NameRegistryQuery, SameNameUnionAcrossScopes) {
    // 位并集：不同作用域同名互不覆盖（OR 累加）
    auto reg = collectFrom(R"(
        namespace P1 { struct Same {}; }
        namespace P2 { class Same {}; }
    )");
    const auto c = reg.classify("Same");   // Parser 侧裸名：跨作用域 OR
    EXPECT_TRUE(c.isType);
    EXPECT_TRUE(reg.classifyQualified("P1", "Same").isType);
    EXPECT_TRUE(reg.classifyQualified("P2", "Same").isType);
}

TEST(NameRegistryQuery, ValueBitAlwaysFalse) {
    auto reg = collectFrom(R"(
        struct S {}; namespace N {} template<class T> struct B {};
    )");
    EXPECT_FALSE(reg.classify("S").isValue);
    EXPECT_FALSE(reg.classify("N").isValue);
    EXPECT_FALSE(reg.classify("B").isValue);
}

TEST(NameRegistryQuery, DeclvalSeed) {
    NameRegistry reg;   // 空表也应有种子
    EXPECT_TRUE(reg.isTemplateName("declval"));
    EXPECT_TRUE(reg.isTemplateName("std::declval"));   // 末段命中
}

} // namespace
