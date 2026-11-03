// =============================================================================
// 测试（正例）：模板名本身是【可见的名字】—— `Box;` 是无声明符的声明，不是表达式
// =============================================================================
// 理论点：
//   · [basic.scope.scope]：模板名与类型名【不同类】—— `Box` 是模板名不是类型名；
//     但它仍然是一个可见的名字，能出现在声明的位置。
//   · [stmt.ambig]/3：语句以标识符开头时，"能当声明就当声明" ⇒ `Box;` 是
//     【无声明符的声明】（clang：declaration does not declare anything，只给
//     -Wmissing-declarations 警告，rc=0）。
//   · ★ 本实现此前把模板名挡在门外：Parser 问的是"这个名字是不是类型"，
//     模板名不是类型 ⇒ 走表达式路径 ⇒ 报 "Undefined variable 'Box'" ⇒
//     【拒收合法程序】。Stage 3 批 2 起，Sema 的裸名声明裁决多问一句
//     "它是可见的模板名吗"（isVisibleTemplateName，判据单点）。
//   · ★ 限定名同理：`N::B;`（B 是命名空间里的类模板）也是合法声明。
// 预期行为：rc=0，运行返回 0；日志含两行
//   [sema:stmt] 'Box' 可见的模板名 ⇒ 判为无声明符声明
//   [sema:stmt] 'N::B' 可见的模板名 ⇒ 判为无声明符声明
// 对照 clang：clang++-18 -std=c++20 同样 rc=0、返回 0（只给两条 warning）。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_31_template_name_declaratorless) { minicc_gtest_body(); }
#endif

template<class T> struct Box { T v; };      // 全局的类模板名

namespace N {                                // 命名空间里的类模板名
template<class T> struct B { T v; };
}

int main() {
    Box;                 // ★ 可见的模板名 ⇒ 无声明符声明（clang：只给警告）
    N::B;                // ★ 限定名同理

    Box<int> b;          // ★ 模板 id 声明照常走（回归）
    b.v = 7;
    if (b.v != 7) return 1;

    N::B<int> nb;        // ★ 命名空间里的模板 id（回归）
    nb.v = 9;
    if (nb.v != 9) return 2;

    return 0;
}
