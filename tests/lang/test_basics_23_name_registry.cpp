// =============================================================================
// 测试：parser 阶段名字识别（作用域裁决 + 遮蔽）
// =============================================================================
// ★ 文件名里的 "name_registry" 是历史名：本文件原测的是已退场的「名字登记表」
//   （前向扫 token 流猜出来的跨作用域并集），Stage 3 批 2 起测的是它的接班 ——
//   Sema 的活作用域表（ParseScopeTable）。文件名保留是为了不打散既有文档引用。
// 理论点：
//   · [stmt.ambig]/3「能当声明就当声明」的另一半：B35 只覆盖了「带模板 id
//     语法」的无声明符声明（Box<int>;）；裸 `S;` 当时刻意留给表达式路径
//     （parser.cpp 注释自承"Token 前瞻分不出来"）。名字登记表从模板位一位
//     扩展为 type/namespace/template 三位并集后，`S;` 问 type 位一个 bit
//     即可裁决——这是 clang Sema::ClassifyName（SemaDecl.cpp:912）骨架的
//     minicc 投影版（docs/specs/parser-stage-name-recognition.spec.md）。
//   · 声明优先于对象：[basic.scope.scope] 类型与普通名字在不同命名空间，
//     `int S; struct S;` 合法并存 —— ★ B37 起，可见性由 Sema 的符号表裁决：
//     块作用域的变量 S 遮蔽全局的类 S ⇒ 该处 `S;` 是【表达式】不是声明
//     （clang: expression result unused，rc=0），与 clang 同。
//   · 限定名作用域裁决：B35 的 Sema 尾剥「取 :: 末段查全局蓝图表」在
//     N::B 真存在时语义正确（归一同一实例），但 N::B 不存在时会误绑
//     全局模板 B（方向性错）。改经登记表 classifyQualified（scope 精确）
//     裁决：配不上 ⇒ 报错「no template named 'B' in namespace 'N'」。
// 预期行为：rc=0，运行返回 0；日志含
//   [parse:stmt] 'S' 判为声明（type 位）      （现场①：全局类型名）
//   [parse:stmt] 'MyInt' 判为声明（type 位）  （现场③：typedef 名）
//   [resolve] 限定模板名 'M::B' ⇒ 裸名 'B'     （正例归一照常）
// ★ 现场②（块作用域的变量 S 遮蔽全局的类 S）**不再**出现 [parse:stmt] 行 ——
//   Stage 3 批 2 起，Parser 判"是不是声明"问的是活作用域表里的【值位】：
//   变量可见 ⇒ `S;` 是表达式（clang 同：expression result unused）。
//   旧实现问的是"名字登记表里有没有 type 位"（跨作用域并集）⇒ 看不见遮蔽。
// 对照 clang：clang++-18 -std=c++20 同样 rc=0（裸 S; 仅 -Wdeclaration-
// does-not-declare-anything warning）。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_23_name_registry) { minicc_gtest_body(); }
#endif

struct S { int x; };       // type 位：struct 声明

typedef int MyInt;         // type 位：typedef（取 ';' 前末标识符）

namespace M {               // namespace 位 + 作用域栈
template<class T>
struct B { T v; };         // template 位（scope=M；template 头后不置 type 位）
}   // namespace M

int main() {
    // ★ 现场①：裸 `S;` —— type 位命中 ⇒ 声明解释（EmptyStmt），
    //   不再走表达式路径（旧：VarExpr(S) 撞符号表碰巧放行，AST 形状错）
    S;

    // ★ 现场②：同名跨类 —— 块作用域的变量 S 【遮蔽】全局的类 S，
    //   故此处的 `S;` 是【表达式】不是声明（[basic.scope.scope] 遮蔽；
    //   clang: warning expression result unused，rc=0）。
    //   ★ B37 起与 clang 一致：可见性由 Sema 的符号表裁决，遮蔽生效；
    //   B37 之前这里被判成声明（变量根本没被登记进符号表）。
    int S = 5;
    S;
    if (S != 5) return 1;

    // ★ 现场③：typedef 名同理
    MyInt;

    // ★ 现场④：限定名模板 id 正例 —— 作用域链命中，归一照常（B35 路径），
    //   运行值钉 7
    M::B<int> y;
    y.v = 7;
    if (y.v != 7) return 2;

    return 0;
}
