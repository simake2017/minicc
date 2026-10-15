// =============================================================================
// test_pp_05_minicc_macro.cpp —— P0 正例：内建静态宏 __MINICC__
// =============================================================================
// 考察理论点（[cpp.predefined] 的 minicc 扩展）：
//   1. __MINICC__ 由预处理器构造时注入宏表（静态宏，值不随展开位置变化），
//      与 __LINE__/__FILE__ 那对"就地生成的动态宏"实现路径不同
//   2. #ifdef 走 m_macros.count；#if __MINICC__ >= 1 走 evalConstantExpr
//      第②步的宏展开后再求值 —— 两条路径都要测到
//   3. defined(__MINICC__) 运算符（[cpp.cond]：defined 先于宏展开处理）
//   4. 双栖：宿主编译器（clang/gcc）不定义 __MINICC__ ⇒ 走 #else 分支，
//      同一份源码两边都能编译（ministl 桩头分支开关就靠它）
//
// 预期行为：minicc 下走 #ifdef 真分支，return __MINICC__ - 1 = 0 ⇒ rc=0；
//          返回值钉死 __MINICC__ 的值（若实现把值改成 2，此处立刻非零）。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_pp_05_minicc_macro) { minicc_gtest_body(); }
#endif

#if !defined(__MINICC__)          // 宿主分支：clang/gcc 上未定义 ⇒ 也要能编过
int main() { return 0; }
#else
#  if !defined(__MINICC__)        // defined() 与 #ifdef 结论必须一致
#    error "inconsistent: #ifdef says defined, defined() says not"
#  endif
#  if __MINICC__ < 1              // 值参与常量表达式求值（先展开成 1 再比）
#    error "__MINICC__ should be >= 1"
#  endif
int main() {
    return __MINICC__ - 1;        // 1 - 1 = 0 ⇒ rc=0；返回值钉死宏的值
}
#endif
