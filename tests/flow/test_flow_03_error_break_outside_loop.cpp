// =============================================================================
// test_flow_03_error_break_outside_loop.cpp —— 主线 C 负例：break 不在循环里
// =============================================================================
// 考察理论点（[stmt.jump]/[stmt.break]）：break 只能出现在循环（或 switch）
// 体内。语法层不判 —— Sema 的循环上下文栈（m_loopDepth==0）裁决。
// 期望报错文案与 clang 逐字相同：
//   'break' statement not in loop or switch statement
//
// 预期行为：rc=1（Semantic Error），链接产物不存在
// =============================================================================

int main() {
    if (1 > 0) {
        break;              // if 体内 ≠ 循环体内 —— clang 同样拒绝
    }
    return 0;
}
