// =============================================================================
// tests/unit/test_member_template_params.cpp —— 成员模板的形参表与自由模板同构
// =============================================================================
// 考察理论点：[temp.mem] × [temp.param]
//   成员函数模板 `template<class T> T add(T x)` 的【形参表】与自由函数模板的
//   形参表是**同一个文法、同一条语义规则**（[temp.param]）—— 模板体在哪儿
//   （命名空间里还是类体内）不改变形参表的规则。
//   ★ 本实现此前把成员侧的形参表**另写了一份简化版**，两处随批次演进而漂移：
//     成员侧缺【无名形参 / 模板模板形参 / 默认实参 / 默认实参约束 / 帧计数同步】，
//     其中"帧计数同步"（TemplateParamFrame::count）缺了最要命 ——
//     成员模板**自己**的形参在方法体里认不出（建成 Class 节点而非 TemplateParam
//     节点）⇒ 依赖限定名 `typename T::type` 直接报 "unknown type name"。
//   本套件钉住"收口后"的性质：两处走**同一份**实现 ⇒ 能力与形态完全一致。
//
// 断言的是【性质】而不是拼写：
//   ① 形态 —— 成员模板自己的类型形参在签名里必须是【模板形参节点】
//      （不是"名字碰巧叫 T"的 Class 节点）；
//   ② 依赖限定名 —— `typename T::type` 的限定者必须是模板形参节点
//      （这正是 ① 的直接后果，也是旧实现报错的那条路）；
//   ③ 能力对齐 —— 无名形参 / 模板模板形参 / 默认模板实参，三样成员侧都能收；
//   ④ 约束对齐 —— 默认实参"后面必须都有默认"这条检查，成员侧同样生效；
//   （成员模板的名字在类作用域里的"值位|模板位"由两处共同凑齐：
//     值位来自 parseFunctionDecl 的 openScope（登记进最近的外层非模板形参作用域），
//     模板位来自 closeScope 之后那一句 declareName —— 本套件不重复钉它，
//     因为那一位不落在本次收口的判据上。）
//
// 突变负向验证（脚本：tools/verify-member-template-mutations.sh，5 条判据逐条实跑变红）：
//   · 成员分支的 parseTemplateParameterList 调用换回"自写一份" ⇒ 5 例全红；
//   · 不同步 frame.count ⇒ OwnTypeParamIsTemplateParamNode + DependentQualifiedNameUsesTemplateParam 红；
//   · 去掉无名形参分支 ⇒ UnnamedTypeParamAccepted 红；
//   · 去掉默认实参分支 ⇒ DefaultTemplateArgAccepted 红；
//   · 模板模板形参分支改成报错 ⇒ TemplateTemplateParamAccepted 红。
//
// 运行：
//   cmake --build build-linux --target unit_tests
//   && ./build-linux/unit_tests --gtest_filter='MemberTemplateParams.*'
// =============================================================================

#include <gtest/gtest.h>

#include "ast.h"
#include "lexer.h"
#include "obs_helpers.h"
#include "parser.h"

#include <string>
#include <vector>

using namespace minicc;

namespace {

// 解析一份源码，返回 TU；解析失败（抛异常）时返回 nullptr。
// ★ 只跑 Parser —— 本套件钉的是**解析期**的形态（形参节点种类 / 形参表能力 /
//   名字登记位），语义期与代码生成不在本套件的射程内。
TranslationUnit parseOrNull(const std::string& src, bool* threw = nullptr) {
    StdoutCapture cap;   // 吞掉全阶段中文日志（本套件只看结构）
    try {
        Lexer lexer(src);
        Parser parser(lexer.tokenizeAll());
        TranslationUnit unit = parser.parseTranslationUnit();
        if (threw) *threw = false;
        return unit;
    } catch (const std::exception&) {
        if (threw) *threw = true;
        return TranslationUnit{};
    }
}

// 在 TU 里找第一个名字匹配的类
const ClassDecl* findClass(const TranslationUnit& unit, const std::string& name) {
    for (const auto& d : unit.declarations) {
        auto cls = std::dynamic_pointer_cast<ClassDecl>(d);
        if (cls && cls->name == name) return cls.get();
    }
    return nullptr;
}

// 在 TU 里找第一个名字匹配的类的某个成员模板
const TemplateDecl* findMemberTemplate(const TranslationUnit& unit,
                                       const std::string& clsName,
                                       const std::string& tmplName) {
    const ClassDecl* cls = findClass(unit, clsName);
    if (!cls) return nullptr;
    for (const auto& mt : cls->memberTemplates) {
        if (mt->templateName() == tmplName) return mt.get();
    }
    return nullptr;
}

}  // namespace

// ── ① 形态：成员模板自己的类型形参必须是【模板形参节点】──────────────────────
// 旧实现（成员侧自写一份形参表、帧计数没同步）：T 被建成 Class 节点 ⇒ 此断言红。
TEST(MemberTemplateParams, OwnTypeParamIsTemplateParamNode) {
    const char* src = R"(
struct Acc {
    template <class T> T add(T x) { return x; }
};
)";
    bool threw = false;
    TranslationUnit unit = parseOrNull(src, &threw);
    ASSERT_FALSE(threw);

    const TemplateDecl* mt = findMemberTemplate(unit, "Acc", "add");
    ASSERT_NE(mt, nullptr);
    ASSERT_NE(mt->funcTemplate, nullptr);
    ASSERT_NE(mt->funcTemplate->returnType, nullptr);
    // ★ 断言"种类"而不是"名字"：名字叫 T 的 Class 节点不算数。
    EXPECT_TRUE(mt->funcTemplate->returnType->isTemplateParam());
    ASSERT_EQ(mt->funcTemplate->parameters.size(), 1u);
    ASSERT_NE(mt->funcTemplate->parameters[0].type, nullptr);
    EXPECT_TRUE(mt->funcTemplate->parameters[0].type->isTemplateParam());
}

// ── ② 依赖限定名：限定者必须是模板形参节点（① 的直接后果）──────────────────
// [temp.res]/5：`typename T::type` 里 T 是依赖的限定者，不能拼成 "T::type" 字符串。
// 旧实现：lookupTemplateParam 查不到成员模板自己的形参 ⇒ 走不进依赖名分支
//         ⇒ 报 "unknown type name 'T::type'"（合法程序被拒）。
TEST(MemberTemplateParams, DependentQualifiedNameUsesTemplateParam) {
    const char* src = R"(
struct Box { int value; using type = int; };
struct S {
    template <class T> typename T::type get(T x) { return x.value; }
};
)";
    bool threw = false;
    TranslationUnit unit = parseOrNull(src, &threw);
    ASSERT_FALSE(threw);

    const TemplateDecl* mt = findMemberTemplate(unit, "S", "get");
    ASSERT_NE(mt, nullptr);
    ASSERT_NE(mt->funcTemplate, nullptr);
    ASSERT_NE(mt->funcTemplate->returnType, nullptr);
    const TypePtr& ret = mt->funcTemplate->returnType;
    // 形态：Class("type") + nestedQualifier = TemplateParam("T")
    ASSERT_TRUE(ret->isClass());
    ASSERT_NE(ret->nestedQualifier, nullptr);
    EXPECT_TRUE(ret->nestedQualifier->isTemplateParam());
}

// ── ③ 能力对齐之一：无名类型形参（[temp.param]/3）────────────────────────────
// `template <class> void f(int x);` —— clang 接受（第二个形参只用来承接默认值）。
TEST(MemberTemplateParams, UnnamedTypeParamAccepted) {
    const char* src = R"(
struct S {
    template <class> void f(int x);
};
)";
    bool threw = false;
    TranslationUnit unit = parseOrNull(src, &threw);
    ASSERT_FALSE(threw);

    const TemplateDecl* mt = findMemberTemplate(unit, "S", "f");
    ASSERT_NE(mt, nullptr);
    ASSERT_EQ(mt->templateParams.size(), 1u);
    EXPECT_TRUE(mt->templateParams[0]->isUnnamed);
    EXPECT_EQ(mt->templateParams[0]->kind, TemplateParamKind::Type);
}

// ── ③ 能力对齐之二：模板模板形参（[temp.param]/4）───────────────────────────
// `template <template <class> class C, class T> T f(C<T> x)` —— clang 接受。
TEST(MemberTemplateParams, TemplateTemplateParamAccepted) {
    const char* src = R"(
struct S {
    template <template <class> class C, class T> T f(C<T> x) { return x; }
};
)";
    bool threw = false;
    TranslationUnit unit = parseOrNull(src, &threw);
    ASSERT_FALSE(threw);

    const TemplateDecl* mt = findMemberTemplate(unit, "S", "f");
    ASSERT_NE(mt, nullptr);
    ASSERT_EQ(mt->templateParams.size(), 2u);
    const TemplateParamPtr& c = mt->templateParams[0];
    EXPECT_EQ(c->kind, TemplateParamKind::Template);
    // 内层表是【节点】（实参位要逐位签名匹配，得知道每位期望什么 kind）
    ASSERT_EQ(c->innerParams.size(), 1u);
    EXPECT_EQ(c->innerParams[0]->kind, TemplateParamKind::Type);
    EXPECT_EQ(mt->templateParams[1]->kind, TemplateParamKind::Type);
}

// ── ③ 能力对齐之三：默认模板实参（[temp.param]/12）──────────────────────────
TEST(MemberTemplateParams, DefaultTemplateArgAccepted) {
    const char* src = R"(
struct S {
    template <class T, class U = int> T f(T x) { return x; }
};
)";
    bool threw = false;
    TranslationUnit unit = parseOrNull(src, &threw);
    ASSERT_FALSE(threw);

    const TemplateDecl* mt = findMemberTemplate(unit, "S", "f");
    ASSERT_NE(mt, nullptr);
    ASSERT_EQ(mt->templateParams.size(), 2u);
    EXPECT_FALSE(mt->templateParams[0]->hasDefault);
    EXPECT_TRUE(mt->templateParams[1]->hasDefault);
}

// ── ④ 约束对齐：默认实参"后面必须都有默认"（[temp.param]/12）────────────────
// 这条检查此前只在自由路径有；收口后成员侧同样生效。
TEST(MemberTemplateParams, DefaultArgGapRejected) {
    const char* src = R"(
struct S {
    template <class T = int, class U> T f(T x) { return x; }
};
)";
    bool threw = false;
    parseOrNull(src, &threw);
    EXPECT_TRUE(threw);
}
