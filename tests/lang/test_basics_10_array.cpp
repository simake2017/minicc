// =============================================================================
// tests/lang/test_basics_10_array.cpp —— 原生数组与下标寻址（ROADMAP 主线 E）
// =============================================================================
// 验证原生数组类型（TypeKind::Array）与数组-指针退化（decay）机制：
//
//   ① [dcl.array]      原生数组声明与初始化（局部栈数组、全局 .data 数组）
//   ② [expr.sub]        原生数组/指针下标寻址：a[i] 读写、base + i * sizeof(T)
//   ③ [conv.arrayptr]  数组退化为指针（Array-to-pointer decay）：数组传参给 int*
//   ④ 类成员数组        结构体成员数组与下标访问（隐式 this 字段与显式成员）
//   ⑤ 常量表达式大小    常量折叠器计算数组大小：int c[2 + 3];
//
// 观测约定：本项目不链接 libc，没有 printf，故用【退出码】汇总：
//     bad 累加"哪一项不符预期"，run-rc = 0 表示全项通过，
//     非 0 时其值就是失败项的编号（1..5），便于定位。
//
// 期望（clang++-18 -std=c++20 与本编译器必须一致）：
//     编译 rc = 0，运行 rc = 0
//
// 语义 oracle：
//     clang++-18 -std=c++20 tests/lang/test_basics_10_array.cpp -o /tmp/arr && /tmp/arr; echo $?
// =============================================================================

int g_arr[4];

struct Vector3 {
    int coords[3];
    int getX() { return coords[0]; }
    void setX(int val) { coords[0] = val; }
};

int sum_elements(int* p, int n) {
    int total = 0;
    int i = 0;
    while (i < n) {
        total = total + p[i];
        i = i + 1;
    }
    return total;
}

int main() {
    int bad = 0;

    // ── ① 局部栈数组声明与读写 ──
    int a[5];
    a[0] = 10;
    a[1] = 20;
    a[2] = 30;
    a[3] = 40;
    a[4] = 50;
    if (a[0] + a[1] + a[2] + a[3] + a[4] != 150) {
        bad = bad + 1;
    }

    // ── ② 全局数组读写 ──
    g_arr[0] = 1;
    g_arr[1] = 2;
    g_arr[2] = 3;
    g_arr[3] = 4;
    if (g_arr[0] + g_arr[1] + g_arr[2] + g_arr[3] != 10) {
        bad = bad + 2;
    }

    // ── ③ 数组退化为指针传参 ──
    int total_a = sum_elements(a, 5);
    if (total_a != 150) {
        bad = bad + 4;
    }
    int total_g = sum_elements(g_arr, 4);
    if (total_g != 10) {
        bad = bad + 8;
    }

    // ── ④ 结构体/类中的数组成员 ──
    Vector3 v;
    v.coords[0] = 100;
    v.coords[1] = 200;
    v.coords[2] = 300;
    if (v.coords[0] + v.coords[1] + v.coords[2] != 600) {
        bad = bad + 16;
    }
    v.setX(105);
    if (v.getX() != 105) {
        bad = bad + 32;
    }

    // ── ⑤ 常量表达式数组大小 ──
    int c[2 + 3];
    c[4] = 99;
    if (c[4] != 99) {
        bad = bad + 64;
    }

    return bad;
}

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_10_array) { minicc_gtest_body(); }
#endif
