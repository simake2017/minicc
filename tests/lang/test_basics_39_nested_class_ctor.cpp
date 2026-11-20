// =============================================================================
// 测试：嵌套类的构造/析构身份（ROADMAP §0.1 批 0-6；BUGS.md B60）
// =============================================================================
// 理论点：
//   · [class.ctor]/1：构造函数的名字是【注入类名】（injected-class-name）——
//     在类内就是【裸名】。`Outer::Inner` 的构造函数叫 `Inner`，限定前缀
//     `Outer::` 是【作用域身份】（learn/49），不进函数名。
//     ⇒ 符号是 "Outer::Inner_Inner"，不是 "Outer::Inner_Outer::Inner"。
//   · [class.base.init]：初始化列表的每一条都要【选定一个构造函数】
//     （对照 clang: Sema::BuildCXXConstructExpr 选完把构造声明交给
//     CXXConstructExpr，CodeGen 只读声明、从不按名字重算）。选谁 + 符号怎么拼
//     是一处语义决定 ⇒ 调用点不能按 "类名_类名_N" 硬拼（BUGS.md B42 的老坑）。
//   · [basic.lookup.unqual]/6：成员的名字只在类作用域里可见 —— 类里的函数名
//     不是全局名字，不能泄漏成"全局可见"。
// 预期行为：rc=0（每个检查点用不同的返回码定位失败的那一处）。
// 对照 clang：clang++-18 -std=c++20 同样 rc=0。
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_basics_39_nested_class_ctor) { minicc_gtest_body(); }
#endif

// ── 嵌套类：一个自带构造函数，一个靠编译器合成 ──
struct Outer {
    struct Inner {
        int v;
        Inner() { v = 5; }
    };
    struct Plain {
        int w;
    };
};

// ── 嵌套类作字段 + 初始化列表（本批修复的核心路径）──
struct Holder {
    struct Part {
        int v;
        Part(int x) { v = x; }
        int read() { return v; }
    };
    Part m;
    Holder() : m(9) {}
};

// ── 命名空间的类作基类 + 初始化列表（基类名是限定名）──
namespace N {
struct Base {
    int v;
    Base(int x) { v = x; }
};
}

struct DerivedFromN : N::Base {
    DerivedFromN() : Base(7) {}
};

// ── 成员类模板（实例名带限定前缀）──
struct MemOuter {
    template <class T>
    struct Mem {
        T v;
    };
};

// ── 类模板里的嵌套类（实例名 Box_int::Inner）──
template <class T>
struct Box {
    struct Inner {
        T v;
    };
};

int main() {
    // ① 嵌套类自带的构造函数（用户写的名字天然是裸名）
    {
        Outer::Inner i;
        if (i.v != 5) return 1;
    }

    // ② 嵌套类的【合成】构造函数：名字同样必须是裸名，否则对象构造落空
    {
        // ★ 变量名不叫 p：`m_classLocals`（栈上类对象表）是【平表】，块退出不清理，
        //   同名的类对象在后面的块里会拿到前一个块的栈偏移（B61 边界，与本批无关）。
        Outer::Plain plain;
        plain.w = 3;
        if (plain.w != 3) return 2;
    }

    // ③ 嵌套类作字段 + 初始化列表：构造符号由 Sema 选定回填
    //    （修前 CodeGen 硬拼 "Holder::Part_Holder::Part_1" ⇒ as 报 junk）
    {
        Holder h;
        if (h.m.v != 9) return 3;
    }

    // ④ 命名空间类作基类 + 初始化列表：基类名是限定名
    {
        DerivedFromN d;
        if (d.v != 7) return 4;
    }

    // ⑤ 成员类模板的实例：类名 "MemOuter::Mem_int"，构造/析构名字取裸名
    {
        MemOuter::Mem<int> m;
        m.v = 6;
        if (m.v != 6) return 5;
    }

    // ⑥ 类模板里的嵌套类：类名 "Box_int::Inner"
    {
        Box<int>::Inner i;
        i.v = 4;
        if (i.v != 4) return 6;
    }

    // ⑦ new 表达式同样走 Sema 回填的构造符号（与 ③ 不同路径）
    {
        //    读值走成员方法（`p->v` 这种【指针取字段】是另一条既有缺口，B50 同族，
        //    与本批无关 —— 不在这里放大）。
        Holder::Part* p = new Holder::Part(11);
        if (p->read() != 11) return 7;
    }

    return 0;
}
