// =============================================================================
// 测试（正例）：裸类型名声明 `S;` 的可见性裁决（B37）
// =============================================================================
// 理论点：
//   · [stmt.ambig]/3「能当声明就当声明」：裸 `S;` 若 S 是【此处可见的类型名】
//     ⇒ 判为无声明符的声明（合法，clang 只给 -Wmissing-declarations warning）。
//   · [basic.lookup.unqual]：非限定名字沿作用域链上溯 —— 当前类作用域先于
//     命名空间链，命名空间链逐级外层（namespace A { namespace B { … } } 里
//     B 内能看到 A 的类型），全局在最后。
//   · ★ 可见性由【Sema 的符号表】裁决（判据单点，红线 3）：Parser 侧的名字
//     登记表（NameRegistry）只是"本 TU 某处声明过这个类型"的跨作用域并集，
//     拿它当"可见"会静默放行非法程序（本条正例的三处可见性都由 Sema 回答）。
//   · [basic.scope.scope] 遮蔽：块作用域的变量 S 遮蔽全局的类 S ⇒ 此处 `S;`
//     是【表达式】不是声明（clang：warning expression result unused，rc=0）。
// 预期行为：rc=0，运行返回 0；日志含三处可见性裁决行
//   [sema:stmt] 'S' 可见的类型名 ⇒ 判为无声明符声明
// 对照 clang：clang++-18 -std=c++20 同样 rc=0、返回 0。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_25_bare_type_name_visibility) { minicc_gtest_body(); }
#endif

// 现场①：全局的类型名 —— 可见
struct GlobalS { int x; };

// 现场②：命名空间里的类型 —— 在【该命名空间内】可见，在全局不可见
namespace N {
struct InnerS { int x; };
int f() { InnerS s; s.x = 1; InnerS; return s.x; }
}   // namespace N

// 现场③：类作用域的类型别名 —— 在【类成员函数体内】可见
struct Holder {
    using Alias = int;
    int g() { Alias; return 1; }
};

// 现场④：块作用域的变量遮蔽同名的全局类型 ⇒ `S;` 是表达式（clang 同）
int main() {
    GlobalS;              // ★ 可见的类型名 ⇒ 无声明符声明
    N::f();               // 命名空间内的裸用（函数体内的 InnerS; 可见）
    Holder h;             // 类作用域别名的裸用（在 Holder::g 体内）
    h.g();
    int GlobalS = 5;      // ★ 变量遮蔽同名类型 ⇒ 下面的 GlobalS; 是表达式
    GlobalS;
    if (GlobalS != 5) return 1;
    return 0;
}
