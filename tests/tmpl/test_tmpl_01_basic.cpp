// =============================================================================
// 测试：基础单参数模板 (Basic Single-Parameter Template)
// =============================================================================
// 最简单的模板场景：一个模板参数 T，字段和方法都使用 T。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]     : "template <typename T> Box stored as blueprint"
//   Phase 3 [Sema]       : "template <typename T> Box (blueprint stored, not analyzed)"
//   Phase 4 [Instantiation]:
//     Substitution map: { 'T' → 'int' }
//     field 'value'  : T → int
//     method 'get'   : return T → int
//     method 'set'   : param v:T → int
//     Mangled: Box_int → _Z3BoxIiE
// =============================================================================


// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_tmpl_01_basic) { minicc_gtest_body(); }
#endif
template<typename T>
class Box {
public:
    T value;

    T get() {
        return value;
    }

    void set(T v) {
        value = v;
    }
};

int add(int a, int b) {
    return a + b;
}

int main() {
    auto result = add(1, 2);
    return result;
}