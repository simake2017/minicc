// =============================================================================
// test_flow_02_ternary.cpp —— 主线 C 正例：条件（三元）表达式 ?:
// =============================================================================
// 考察理论点（[expr.cond]）：
//   1. 短路语义：落选的那一支【不求值】——副作用（全局计数、调用）不触发
//   2. 优先级：?: 比 || 低（a || b ? x : y = (a||b) ? x : y），
//      比赋值/逗号高；右结合：a?b:c?d:e = a?b:(c?d:e)
//   3. 嵌套与作为实参/初始化式出现
//   4. 模板函数体内出现 ?: ⇒ cloneExpr 的 Conditional 分支被实例化路径走到
//
// 预期行为：rc=0，run-rc=0（短路计数器钉住"未选支零副作用"）
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_flow_02_ternary) { minicc_gtest_body(); }
#endif

int g_then_calls = 0;    // 短路钉扎：被调几次 = 该支被求值几次
int g_else_calls = 0;

int then_side() { g_then_calls = g_then_calls + 1; return 11; }
int else_side() { g_else_calls = g_else_calls + 1; return 22; }

// 模板函数体内用 ?: —— 实例化走 cloneExpr(NodeKind::Conditional) 分支
template <typename T>
T clamp_nonzero(T v) {
    return v > 0 ? v : 0 - v;            // 简易 abs（T := int）
}

int main() {
    // ① 基本选值
    int a = 5;
    int b = 9;
    int m = a > b ? a : b;
    if (m != 9) return 1;

    // ② 短路：条件为真 ⇒ else 支【零调用】；条件为假 ⇒ then 支零调用
    int x = 1 > 0 ? then_side() : else_side();
    if (x != 11 || g_then_calls != 1 || g_else_calls != 0) return 2;
    int y = 1 < 0 ? then_side() : else_side();
    if (y != 22 || g_then_calls != 1 || g_else_calls != 1) return 3;

    // ③ 优先级：|| 低于 ?: —— (a > 0 || b > 0) ? 100 : 200
    int p = a > 0 || b > 0 ? 100 : 200;
    if (p != 100) return 4;

    // ④ 右结合嵌套：v<0 ? -1 : (v==0 ? 0 : 1) —— 三层符号函数
    int s1 = -3 < 0 ? -1 : -3 == 0 ? 0 : 1;
    int s2 = 0 < 0 ? -1 : 0 == 0 ? 0 : 1;
    int s3 = 7 < 0 ? -1 : 7 == 0 ? 0 : 1;
    if (s1 != -1 || s2 != 0 || s3 != 1) return 5;

    // ⑤ 作为实参与初始化式
    int arr0 = (a < b ? 3 : 4) + (a > b ? 30 : 40);
    if (arr0 != 43) return 6;

    // ⑥ 模板实例化路径上的 ?:
    if (clamp_nonzero<int>(-5) != 5) return 7;
    if (clamp_nonzero<int>(6) != 6) return 8;

    return 0;
}
