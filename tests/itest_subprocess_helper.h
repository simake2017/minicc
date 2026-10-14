// =============================================================================
// itest_subprocess_helper.h —— 集成测试【子进程包装】的公共 helper
// =============================================================================
// 只被 build-linux/itest_wrappers/ 里 CMake 生成的包装 TU 包含（勿手写 include）。
//
// 适用对象：主机编译器（clang++-18）编不过、因而进不了 in-process 二进制
// integration_gtest 的那批用例 ——
//   ① 错误用例（*_error* 及基线 rc=1 的）：本就是【非法程序】，clang/minicc
//      双双拒绝，没有"程序语义"可步进；
//   ② minicc 方言用例：v[i]/m[k] 下标糖（无 operator[]）、tmpl_15 的左值按
//      T&& 返回等，只有 minicc 放行。
//
// 机制：TEST 体以子进程调 minicc 编译该文件并核对退出码；rc=0 的再运行产物
// 核对 run-rc。期望值由 CMake  configure 时从 .logbaseline/<...>.log 解析进来
// （宏 ITEST_SRC / ITEST_EXPECT_COMPILE_RC / ITEST_HAS_RUN / ITEST_EXPECT_RUN_RC），
// 与 logdiff 是【同一个 oracle】，不会各说各话。
//
// ★ 调试语义：这批用例在 CLion 里点 Run = 单条复现 + 失败时回看编译日志；
//   点 Debug 步进到的只是本 helper 的包装层（真身是子进程，gdb 跟不进去）。
//   要断进【编译器】，请用 minicc 的运行配置（Program arguments 填用例路径）；
//   要断进【程序语义】的只有 in-process 那 94 个用例（integration_gtest）。
// =============================================================================
#pragma once

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace itest_subprocess {

inline std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// 一条子进程用例：编译（核 rc）→ 可选运行产物（核 run-rc）。
// 输出落在 /tmp/itest_subprocess_<name>_<pid>/（不清理，交给系统 tmp 策略）。
inline void runCase(const char* name) {
    const std::string dir =
        std::string("/tmp/itest_subprocess_") + name + "_" + std::to_string(getpid());
    const std::string exe = dir + "/a.out";
    const std::string log = dir + "/compile.log";

    if (std::system(("mkdir -p " + dir).c_str()) != 0) {
        FAIL() << name << "：无法创建临时目录 " << dir;
        return;
    }

    const std::string cmd = std::string("\"") + MINICC_BIN_PATH + "\" \"" + ITEST_SRC +
                            "\" -o " + exe + " > " + log + " 2>&1";
    const int status = std::system(cmd.c_str());
    ASSERT_TRUE(WIFEXITED(status)) << name << "：minicc 异常终止（信号）";
    ASSERT_EQ(ITEST_EXPECT_COMPILE_RC, WEXITSTATUS(status))
        << name << "：minicc 编译退出码不符（期望 " << ITEST_EXPECT_COMPILE_RC
        << "）\n------ minicc 编译日志 ------\n"
        << slurp(log);

    if (!ITEST_HAS_RUN) return;
    const int rstatus = std::system((exe + " > /dev/null 2>&1").c_str());
    ASSERT_TRUE(WIFEXITED(rstatus)) << name << "：程序异常终止（信号）";
    EXPECT_EQ(ITEST_EXPECT_RUN_RC, WEXITSTATUS(rstatus))
        << name << "：程序运行退出码不符（期望 " << ITEST_EXPECT_RUN_RC << "）";
}

}  // namespace itest_subprocess
