// =============================================================================
// 测试：switch 多路分支（主线 C 二批，[stmt.switch]）
// =============================================================================
// 预期行为：
//   比较链降级：条件值存栈顶，逐条 cmp-je 分发到 case 标号；case 标号
//   按体内语句顺序顺序放置 —— 不穿 break 的 case 自然贯穿到下一条
//   （C 的 fall-through）；无匹配落 default，无 default 直落出口；
//   break 在 switch 内合法（break 守卫的 "or switch" 至此名副其实）。
//   全部检查点通过返回 0，任一失败返回该检查点编号。
//
// 考察理论点：
//   · [stmt.switch]/1 的值匹配语义 + fall-through 是【默认】（贯穿不是 bug）。
//   · break 的上下文栈：switch 与循环同栈共管，内层循环的 break 落内层出口。
//   · 对照 clang：LowerSwitch 的两种形态（跳表 / 比较链），教学版恒用比较链。
// =============================================================================

int main() {
    int x = 2;
    int r = 0;

    // 1. 命中 case 2：贯穿 case 3 直到 default（无 break 一路落下）
    switch (x) {
        case 1: r = r + 1;
        case 2: r = r + 10;
        case 3: r = r + 100;
        default: r = r + 1000;
    }
    if (r != 1110) return 1;

    // 2. break 截断贯穿：case 2 命中即出，r 只加 10
    r = 0;
    switch (x) {
        case 1: r = r + 1; break;
        case 2: r = r + 10; break;
        case 3: r = r + 100; break;
        default: r = r + 1000;
    }
    if (r != 10) return 2;

    // 3. 无匹配走 default
    switch (9) {
        case 1: r = 100; break;
        default: r = 3;
    }
    if (r != 3) return 3;

    // 4. 无匹配且无 default：直落出口，r 不变
    switch (9) {
        case 1: r = 100;
        case 2: r = 200;
    }
    if (r != 3) return 4;

    // 5. 内层循环的 break 不受 switch 干扰：switch 里嵌 while，break 落循环出口
    int count = 0;
    switch (1) {
        case 1: {
            int i = 0;
            while (i < 10) {
                i = i + 1;
                if (i == 3) break;   // 落 while 出口，不是 switch 出口
                count = count + 1;
            }
            // 穿出 while 后继续 switch 体
            count = count + 100;
            break;                    // 这条才落 switch 出口
        }
        default: count = 999;
    }
    if (count != 102) return 5;       // (1,2 两个完整轮) + 100

    // 6. 条件为 0 落 default（case 值 0 与"无匹配"判据同路径）
    switch (0) {
        case 0: r = 42; break;
        default: r = 0;
    }
    if (r != 42) return 6;

    return 0;
}
