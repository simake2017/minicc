// =============================================================================
// 测试：class 关键字代替 typename (class keyword instead of typename)
// =============================================================================
// C++ 标准中 template<class T> 和 template<typename T> 完全等价。
// minicc 同时支持两种写法。
//
// 编译过程中的关键日志：
//   Phase 2 [Parser]:
//     "template <class T> Wrapper stored as blueprint"
//     [parse:template] class T
//     ★ type parameter registered: 'T'
//     注意这里打印的是 "class T" 而不是 "typename T"
//
//   Phase 4 [Instantiation]:
//     替换过程与 typename 完全一致——class 和 typename 只是语法差异
// =============================================================================

// ── GoogleTest 入口（仅 CLion 单用例 Run/Debug；minicc 编译时本块被条件编译抹掉）──
#ifdef MINICC_GTEST_TEST
#include <gtest/gtest.h>
#define main minicc_gtest_body
static int minicc_gtest_body();
TEST(Integration, test_tmpl_05_class_keyword) { minicc_gtest_body(); }
#endif
template<class T>
class Wrapper {
public:
    T data;
    int size;

    T getData() {
        return data;
    }

    void setData(T d) {
        data = d;
    }

    int getSize() {
        return size;
    }
};

int main() {
    auto x = 100;
    return 0;
}
