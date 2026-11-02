// =============================================================================
// 测试（正例）：命名空间里的模板 —— 作用域是名字身份的一部分（B38）
// =============================================================================
// 理论点：
//   · [basic.lookup.qual] 限定名查找：模板声明属于它所在的 DeclContext，
//     `N::B` 查的就是 N 里的 B；查不到就查不到，**不退到全局的 B**
//     （clang: error: no template named 'B' in namespace 'N'）。
//   · [temp.inst] 实例化：实例的身份 = 模板 + 实参表。模板名带作用域 ⇒
//     `N::B<int>` 与 `M::B<int>` 与 `::B<int>` 是三个不同的实例，
//     各自的字段布局、方法符号都不同（clang 的 mangling 同理：
//     _ZN1N1BIiE vs _ZN1M1BIiE vs _Z1BIiE）。
//   · ★ 修复前的症状（docs/BUGS.md B38）：minicc 的模板按【裸名】注册，
//     限定名走「剥 :: 末段查全局蓝图」⇒ ① `N::B<int>` 绑到全局 B（绑错模板）；
//     ② 两个命名空间的同名模板撞成同一个实例（`No member 'w'`）。
//   · 判据单点（红线 3）：作用域前缀化只有一处（processNamespaceDecl），
//     注册键 / 实例名 / 缓存键 / mangling 全部由它派生，不各写一份。
// 预期行为：rc=0，运行返回 0；日志含三个【互不相同】的实例名
//   [instantiate:name] 'B<int>'      → 汇编符号前缀 'B_int'
//   [instantiate:name] 'N::B<int>'   → 汇编符号前缀 'N::B_int'
//   [instantiate:name] 'M::B<int>'   → 汇编符号前缀 'M::B_int'
// 对照 clang：clang++-18 -std=c++20 同样 rc=0、返回 0。
//   （clang 的实例名是 _ZN1N1BIiE 这种真 mangling；本实现用可读名 +
//    asmSymbol 净化（'::'→'__'）⇒ 汇编符号 N__B_int。编码形态不同，
//    唯一性一致 —— 见 docs/BUGS.md B7。）
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_28_namespace_template_scope) { minicc_gtest_body(); }
#endif

// 现场①：全局模板 B（1 个字段）
template<class T>
struct B { T v; };

// 现场②：命名空间 N 里的同名模板 B（2 个字段，名字也不同 ⇒ 绑错必炸）
namespace N {
template<class T>
struct B { T v; T w; };
}   // namespace N

// 现场③：命名空间 M 里的同名模板 B（2 个字段，第三个个名字又不同）
namespace M {
template<class T>
struct B { T v; T z; };
}   // namespace M

int main() {
    // 三个【不同的实体】：全局 B / N::B / M::B
    B<int> g;
    g.v = 1;

    N::B<int> n;
    n.v = 2;
    n.w = 3;          // ★ 绑错到全局 B ⇒ "No member 'w' in class 'B_int'"

    M::B<int> m;
    m.v = 4;
    m.z = 5;          // ★ 与 N::B 撞实例 ⇒ "No member 'z' in class 'N::B_int'"

    if (g.v != 1) return 1;
    if (n.v != 2) return 3;
    if (n.w != 3) return 4;
    if (m.v != 4) return 5;
    if (m.z != 5) return 6;

    return 0;
}
