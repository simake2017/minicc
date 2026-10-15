// =============================================================================
// 测试：switch 负向用例（case 位置/重复/非常量，文案与 clang 逐字同）
// =============================================================================
// 预期行为：
//   本文件测的是【非 switch 体内写 case】—— 语义分析当场报错，rc=1，
//   文案与 clang++-18 -std=c++20 逐字相同：
//     error: 'case' statement not in switch statement
//
// 考察理论点：
//   · [stmt.switch]/1：case 标号只在 selection-statement 的 switch 内有意义。
//   · 伴生负向（已有自动化单测覆盖）：
//     - 重复 case 值 ⇒ duplicate case value 'N'
//     - 非常量 case 值 ⇒ case value is not a constant expression
//     - switch 内写 continue ⇒ 'continue' statement not in loop statement
// =============================================================================

int main() {
    case 1: ;
    return 0;
}
