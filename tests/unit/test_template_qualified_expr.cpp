// 模板限定表达式：[basic.lookup.qual]、[expr.prim.id.qual]。
// Box<int>::Inner::value 可用于任意表达式；Box<int>::make(x) 不需要对象。
#include "lexer.h"
#include "parser.h"
#include "semantic_analyzer.h"
#include "codegen.h"
#include "obs_helpers.h"
#include <gtest/gtest.h>

using namespace minicc;

namespace {
std::string compileQualified(const std::string& source) {
    Lexer lexer(source);
    Parser parser(lexer.tokenizeAll());
    auto unit = parser.parseTranslationUnit();
    SemanticAnalyzer sema;
    sema.analyze(unit);
    CodeGen codegen;
    return codegen.generate(unit, sema.getClassTypes(), sema.getFunctions());
}
}

TEST(TemplateQualifiedExpr, NestedConstantsInReturnAndArithmetic) {
    StdoutCapture capture;
    EXPECT_NO_THROW(compileQualified(
        "template<class T> struct Box { struct Inner {"
        " static const int b=3; struct Deep { static const int c=9; }; }; };"
        "int main() { return Box<int>::Inner::b + Box<int>::Inner::Deep::c; }"));
}

TEST(TemplateQualifiedExpr, StaticCallUsesFirstArgumentRegister) {
    StdoutCapture capture;
    auto assembly = compileQualified(
        "template<class T> struct Box { static int add(int x) {return x+4;} };"
        "int main() { return Box<int>::add(2); }");
    EXPECT_NE(assembly.find("callq Box_int_add"), std::string::npos);
    EXPECT_EQ(assembly.find("暂存 this 指针"), std::string::npos);
    EXPECT_NE(assembly.find("popq %rdi"), std::string::npos);
}

TEST(TemplateQualifiedExpr, ObjectStaticCallUsesFirstArgumentRegister) {
    StdoutCapture capture;
    auto assembly = compileQualified(
        "template<class T> struct Box { static int add(int x) {return x+4;} };"
        "int main() { Box<int> box; return box.add(2); }");
    EXPECT_NE(assembly.find("callq Box_int_add"), std::string::npos);
    EXPECT_EQ(assembly.find("暂存 this 指针"), std::string::npos);
    EXPECT_NE(assembly.find("popq %rdi"), std::string::npos);
}

TEST(TemplateQualifiedExpr, NestedStaticCall) {
    StdoutCapture capture;
    EXPECT_NO_THROW(compileQualified(
        "template<class T> struct Box { struct Inner {static int f(){return 4;}}; };"
        "int main(){int x=Box<int>::Inner::f(); return x;}"));
}

TEST(TemplateQualifiedExpr, RejectsValueAsQualifier) {
    StdoutCapture capture;
    try {
        compileQualified("template<class T> struct Box {static int a;};"
                         "int main(){return Box<int>::a::b;}");
        FAIL() << "accepted a value as a qualifier";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("is not a class, namespace, or enumeration"),
                  std::string::npos) << e.what();
    }
}

TEST(TemplateQualifiedExpr, RejectsNonStaticCallWithoutObject) {
    StdoutCapture capture;
    try {
        compileQualified("template<class T> struct Box {int f(){return 4;}};"
                         "int main(){return Box<int>::f();}");
        FAIL() << "accepted a non-static call without an object";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("non-static member function"),
                  std::string::npos) << e.what();
    }
}
