// =============================================================================
// 测试：偏特化匹配里的引用类型位 —— 模板实参可以是引用，调用推导的剥壳规则不适用
// =============================================================================
// 预期行为（全部与 clang++-18 -std=c++20 逐一核对，运行返回值 0）：
//   Duo<int, int&>   命中偏特化 Duo<T, int&>   ⇒ v() = 1
//   Duo<int, int&&>  命中偏特化 Duo<T, int&&>  ⇒ v() = 2
//   Duo<int, int>    无偏特化命中，回落主模板  ⇒ v() = 0
//   Same<int&, int&> 命中偏特化 Same<T, T>
//                    （T := int&，实参位保留引用）⇒ v() = 4
//   Same<int&, int>  偏特化推导冲突（T := int& 与 T := int），
//                    clang 判不命中、回落主模板  ⇒ v() = 0
//
// 考察理论点：
//   [temp.class.spec.match]/2：偏特化匹配是模板实参表的【结构等价】，
//   模板实参（template-argument）本来就是类型，可以是引用类型 ——
//   调用推导里的两条"剥壳"规则在这里都是错的：
//   ① [expr.type]/1（表达式没有引用类型）不成立：实参位不是表达式。
//      具体引用位（Duo<T, int&> 的 int&）配 int& 实参，旧实现递归时拿 A=int&
//      原样比"非依赖恒等"，比成 int vs int& 误拒（拒收合法程序）；
//      修法：structuralMatch 下引用分支【对称剥壳】，两侧同时退一层。
//   ② [temp.deduct.call] 的值传递调整（剥顶层引用/const）不成立：
//      Same<T,T> 配 Same<int&,int&>，旧实现逐位剥壳后 T := int（clang 是 int&）；
//      更糟的是 Same<T,T> 配 Same<int&,int>，剥壳后两位"一致 ✓"
//      误命中偏特化（静默选错特化，clang 判推导冲突回落主模板）；
//      修法：structuralMatch 下裸形参位绑定【原样】的实参类型。
//   ★ 两条规则的共同点：都是把【调用推导】的上下文假设带进了【偏特化匹配】。
//     structuralMatch 参数（matchPattern 传入）就是这两套语境的分界线。
//   回归红线：test_tmpl_52（引用折叠）的推导轨迹须在 zero-drift 意义下等价。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_tmpl_73_partial_spec_ref_args) { minicc_gtest_body(); }
#endif
template <class T, class U> struct Duo { int v() { return 0; } };
template <class T> struct Duo<T, int&>  { int v() { return 1; } };
template <class T> struct Duo<T, int&&> { int v() { return 2; } };

template <class T, class U> struct Same { int v() { return 0; } };
template <class T> struct Same<T, T>   { int v() { return 4; } };

int main() {
    Duo<int, int&>   a;   // 具体引用位（int&）配 int& 实参 ⇒ 命中
    Duo<int, int&&>  b;   // 具体引用位（int&&）配 int&& 实参 ⇒ 命中
    Duo<int, int>    c;   // 无命中 ⇒ 主模板
    Same<int&, int&> s;   // T := int& ⇒ 命中
    Same<int&, int>  m;   // T 推导冲突 ⇒ 主模板

    return a.v() + b.v() * 10 + c.v() * 100 + s.v() * 1000 + m.v() * 10000 - 4021;
}
