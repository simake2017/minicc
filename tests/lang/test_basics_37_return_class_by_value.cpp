// =============================================================================
// 测试：按值返回类对象 + 拷贝省略（ROADMAP §0.1 批 0-1；BUGS.md B49）
// =============================================================================
// 理论点：
//   · [dcl.init]/17.6.1（prvalue 初始化对象）：`S x = f();` 里 f 的返回值是
//     prvalue，它【直接materialize】成 x —— 中间不产生第二个对象，也就不需要
//     拷贝构造（[class.copy.elision]/8：允许省略，且 clang 默认就这么做）。
//   · 返回的存储怎么来？按值返回【类对象】在 System V AMD64 ABI 里用 sret
//     形态：调用者给一块缓冲（隐藏的第 0 参数，走 rdi），被调者把对象拷进去，
//     返回值 rax = 缓冲地址（对照 clang 的
//     clang/lib/CodeGen/CGCall.cpp:CodeGenFunction::EmitReturn / the Itanium ABI
//     §3.2.3 "Passing and Returning Aggregates"）。
//   · ★ 拷贝省略让这块缓冲【就是 x 自己的槽】⇒ 调用直接把返回值写进 x，
//     不需要"先写进临时区、再拷进 x"两段。
// 预期行为：rc=0（本文件每个检查点用不同的返回码定位失败的那一处）。
// 对照 clang：clang++-18 -std=c++20 同样 rc=0。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_37_return_class_by_value) { minicc_gtest_body(); }
#endif

// ── 被返回的普通类 ──
struct S {
    int v;
};

// ── 多字段类：返回缓冲的大小必须按【真实布局】算（不是一个槽）──
struct Triple {
    int x;
    int y;
    int z;
};

// ── 多态类：vptr 随对象表示一起被拷贝（返回缓冲拷的是"整个对象"）──
struct Poly {
    virtual int kind() { return 1; }
    virtual ~Poly() {}
    int data;
};

struct Base {
    virtual S make(int k) { S s; s.v = k; return s; }
    virtual ~Base() {}
};

struct Derived : Base {
    S make(int k) { S s; s.v = k + 100; return s; }
};

struct Holder {
    S make(int k) { S s; s.v = k; return s; }
};

template <class T>
struct Box {
    T v;
};

// ── 各种返回类对象的函数 ──
S makeS(int k) {
    S s;
    s.v = k;
    return s;
}

Triple makeTriple(int a, int b, int c) {
    Triple t;
    t.x = a; t.y = b; t.z = c;
    return t;
}

Poly makePoly() {
    Poly p;
    p.data = 6;
    return p;
}

Box<int> makeBox() {
    Box<int> b;
    b.v = 9;
    return b;
}

// 返回"另一个函数返回的对象"：两层 sret 缓冲（内层缓冲在帧临时区）
S wrap() {
    S s = makeS(10);
    return s;
}

int main() {
    // ① 自由函数按值返回 + 拷贝省略
    S a = makeS(7);
    if (a.v != 7) return 1;

    // ② 返回对象的字段可改（走成员赋值路径：★ 旧实现在这里踩坏 %rcx）
    a.v = a.v + 1;
    if (a.v != 8) return 2;

    // ③ 多字段类：缓冲按真实尺寸（12 字节 ⇒ 16）拷贝
    Triple t = makeTriple(1, 2, 3);
    if (t.x != 1) return 3;
    if (t.y != 2) return 4;
    if (t.z != 3) return 5;

    // ④ 成员方法按值返回（this 让到 rsi，实参从 rdx 起）
    Holder h;
    S b = h.make(4);
    if (b.v != 4) return 6;

    // ⑤ 虚方法按值返回：多态派发 + sret 共存（this 在 rsi、缓冲在 rdi）
    Base* p = new Derived();
    S c = p->make(5);
    if (c.v != 105) return 7;

    // ⑥ 多态类按值返回：vptr 随对象表示一起拷过去
    Poly q = makePoly();
    if (q.data != 6) return 8;
    if (q.kind() != 1) return 9;

    // ⑦ 类模板实例按值返回
    Box<int> bi = makeBox();
    if (bi.v != 9) return 10;

    // ⑧ 返回"另一个函数返回的对象"（两层缓冲）
    S d = wrap();
    if (d.v != 10) return 11;

    // ⑨ 返回值可以进表达式（不必立刻初始化）
    if (makeS(3).v != 3) return 12;

    return 0;
}
