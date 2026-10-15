// =============================================================================
// test_flow_04_error_continue_outside_loop.cpp —— 主线 C 负例：continue 不在循环里
// =============================================================================
// 考察理论点（[stmt.jump]/[stmt.cont]）：continue 只能出现在循环体内。
// 期望报错文案与 clang 逐字相同：
//   'continue' statement not in loop statement
//
// 预期行为：rc=1（Semantic Error）
// =============================================================================

int main() {
    continue;
    return 0;
}
