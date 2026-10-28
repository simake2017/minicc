// =============================================================================
// 测试：parser 阶段名字识别（名字登记表多 bit 分类 + 作用域裁决）
// =============================================================================
// 理论点：
//   · [stmt.ambig]/3「能当声明就当声明」的另一半：B35 只覆盖了「带模板 id
//     语法」的无声明符声明（Box<int>;）；裸 `S;` 当时刻意留给表达式路径
//     （parser.cpp 注释自承"Token 前瞻分不出来"）。名字登记表从模板位一位
//     扩展为 type/namespace/template 三位并集后，`S;` 问 type 位一个 bit
//     即可裁决——这是 clang Sema::ClassifyName（SemaDecl.cpp:912）骨架的
//     minicc 投影版（docs/specs/parser-stage-name-recognition.spec.md）。
//   · 声明优先于对象：[basic.scope.scope] 类型与普通名字在不同命名空间，
//     `int S; struct S;` 合法并存，`S;` 判声明（不读对象），与 clang 同。
//   · 限定名作用域裁决：B35 的 Sema 尾剥「取 :: 末段查全局蓝图表」在
//     N::B 真存在时语义正确（归一同一实例），但 N::B 不存在时会误绑
//     全局模板 B（方向性错）。改经登记表 classifyQualified（scope 精确）
//     裁决：配不上 ⇒ 报错「no template named 'B' in namespace 'N'」。
// 预期行为：rc=0，运行返回 0；日志含一行
//   [parse:stmt] 'S' 判为声明（type 位）     （两处裸名声明）
//   [resolve] 限定模板名 'M::B' ⇒ 裸名 'B'   （正例归一照常）
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

    // ★ 现场②：同名跨类 —— [stmt.ambig] 声明优先：S; 仍判声明，
    //   表达式位置（return S）照常读对象 5
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
