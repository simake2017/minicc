// =============================================================================
// test_flow_01_for_break_continue.cpp —— 主线 C 正例：for / break / continue
// =============================================================================
// 考察理论点（[stmt.iter]/[stmt.for]、[stmt.jump]/[stmt.break]/[stmt.cont]）：
//   1. for 的降级形状：init 一次 → cond 测试 → body → incr → 回边（五标签）
//   2. continue 落在【增量步】而不是条件式 —— `for(;;)` 里 continue 必须
//      先跑 incr 再判 cond，否则会死循环
//   3. break/continue 只作用于【最近一层】循环（嵌套时的标签栈语义）
//   4. for-init 的声明作用域贯穿 cond/incr/body（`int i` 在三个位置都可见）
//   5. 表达式式 init（`i = 1`）与空 init（for(;;)）
//
// 预期行为：rc=0，run-rc=0（每个检查点失败返回不同码，便于定位）
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_flow_01_for_break_continue) { minicc_gtest_body(); }
#endif

int main() {
    // ① 基本 for + 声明式 init：1..100 求和
    int sum = 0;
    for (int i = 1; i <= 100; i = i + 1) sum = sum + i;
    if (sum != 5050) return 1;

    // ② continue 跳过本轮、continue 落增量步：0..9 求和但跳过 3
    int n = 0;
    for (int i = 0; i < 10; i = i + 1) {
        if (i == 3) continue;
        n = n + i;
    }
    if (n != 42) return 2;              // 0+1+2+4+5+6+7+8+9 = 42

    // ③ break 出循环
    int b = 0;
    for (int i = 0; i < 100; i = i + 1) {
        if (i == 7) break;
        b = b + 1;
    }
    if (b != 7) return 3;

    // ④ 表达式式 init + 空 incr（增量写在体尾）
    int k = 0;
    for (k = 5; k < 8;) k = k + 1;
    if (k != 8) return 4;

    // ⑤ for(;;) 死循环靠 break 出口
    int d = 0;
    for (;;) {
        d = d + 1;
        if (d >= 7) break;
    }
    if (d != 7) return 5;

    // ⑥ 嵌套循环：break/continue 只作用最近一层
    //    外层 i=0..4，内层 j=0..9 但 j==2 continue、j==5 break
    //    ⇒ 每轮内层累加 0+1+3+4 = 8，五轮共 40
    int total = 0;
    for (int i = 0; i < 5; i = i + 1) {
        for (int j = 0; j < 10; j = j + 1) {
            if (j == 2) continue;
            if (j == 5) break;
            total = total + j;
        }
    }
    if (total != 40) return 6;

    // ⑦ while 里的 break/continue（循环上下文栈对 while 同样生效）
    int w = 0;
    while (w < 100) {
        w = w + 1;
        if (w == 4) continue;
        if (w > 6) break;
    }
    if (w != 7) return 7;

    return 0;
}
