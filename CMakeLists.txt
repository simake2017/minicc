cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
project(MiniCppCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# macOS: 确保使用正确的 SDK 和 C++ 标准库路径
if(APPLE)
#    execute_process(COMMAND xcrun --show-sdk-path
#        OUTPUT_VARIABLE SDK_PATH OUTPUT_STRIP_TRAILING_WHITESPACE)
#    set(CMAKE_OSX_SYSROOT ${SDK_PATH})
#
#    # 显式添加 C++ 标准库头文件路径
#    set(CXX_STDLIB_PATH "${SDK_PATH}/usr/include/c++/v1")

    set(CXX_STDLIB_PATH "/opt/homebrew/opt/llvm/include/c++/v1")
    if(EXISTS ${CXX_STDLIB_PATH})
        include_directories(SYSTEM ${CXX_STDLIB_PATH})

        link_directories("/opt/homebrew/opt/llvm/lib/c++")
#        set(CMAKE_BUILD_RPATH "/opt/homebrew/opt/llvm/lib/c++")
    endif()

endif()

# Linux + Clang: 系统 libstdc++ (GCC 10) 无 <format>，改用 libc++
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
endif()

# ─── 源文件 ────────────────────────────────────────────
set(SOURCES
    src/preprocessor.cpp
    src/sfinae.cpp
    src/constant_folder.cpp
    src/lexer.cpp
    src/parser.cpp
    src/type.cpp
    src/semantic_analyzer.cpp
    src/template_instantiation.cpp
    src/template_deduction.cpp
    src/codegen.cpp
    src/linker.cpp
    src/main.cpp
)

add_executable(minicc ${SOURCES})
target_include_directories(minicc PRIVATE include) ## 添加include文件件

# 编译选项
target_compile_options(minicc PRIVATE -Wall -Wextra -pedantic)

# ─── 单元测试（GoogleTest，CLion gutter 可点击单测）─────────────────────
# CLion 用法：Reload CMake 后 tests/unit/*.cpp 里每个 TEST() 左侧出现绿色三角，
# 点击即单跑该用例；底部 Test 面板树状查看全部结果。
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_executable(unit_tests
    tests/unit/test_preprocessor.cpp
    tests/unit/test_template_deduction.cpp
    tests/unit/test_decl_and_ctor.cpp
    tests/unit/test_expr_parser.cpp
    tests/unit/test_expressions.cpp
    tests/unit/test_ast_nodes.cpp
    tests/unit/test_decltype_sfinae.cpp
    tests/unit/test_rtti_layout.cpp
    tests/unit/test_typeinfo_suite.cpp
    tests/unit/test_codegen_frame.cpp
    tests/unit/test_codegen_field_write.cpp
    tests/unit/test_ast_visitor.cpp
    tests/unit/test_symbol_consistency.cpp
    tests/unit/test_vtable_symbols.cpp
    tests/unit/test_member_identity.cpp
    tests/unit/test_layout_lookup.cpp
    tests/unit/test_ttp_signature.cpp
    tests/unit/test_nttp_type_domain.cpp
    tests/unit/test_declaratorless_decl.cpp
    tests/unit/test_member_template_in_class_template.cpp
    tests/unit/test_arg_binding.cpp
    src/preprocessor.cpp
    src/sfinae.cpp
    src/constant_folder.cpp
    src/lexer.cpp
    src/parser.cpp
    src/semantic_analyzer.cpp
    src/codegen.cpp
    src/template_deduction.cpp
    src/template_instantiation.cpp
    src/type.cpp
    src/linker.cpp
)
target_include_directories(unit_tests PRIVATE include)
target_compile_options(unit_tests PRIVATE -Wall -Wextra)
target_link_libraries(unit_tests GTest::gtest GTest::gtest_main)
# -rdynamic：把弱符号（vtable/typeinfo）导出进动态符号表，
# 供 test_rtti_layout.cpp 用 dladdr 把地址解析成符号名打印内存图形；
# 老 glibc 的 dladdr 在独立 libdl 里，需显式链接
target_link_options(unit_tests PRIVATE -rdynamic)
target_link_libraries(unit_tests dl)

include(GoogleTest)
gtest_discover_tests(unit_tests)

# ─── 集成测试的 gtest 包装（调试入口：CLion 单用例断点）───────────────────
# 动机：tests/*/*.cpp 都以 int main() 交付，由 logdiff.sh 统一跑；想在 CLion 里
# 【单步进某一个测试的程序语义】时，没有 per-test 的调试配置可点。
# 原理（不改任何测试文件）：
#   每个正例 .cpp 生成一个包装 TU：文件体原样 #include 进具名 namespace（全部符号
#   转内部链接，不同测试里的同名自由函数/类互不冲突），-Dmain=<符号> 把 main 改名
#   成普通函数，同 TU 尾部落一个 TEST(Integration, <文件名>) 当注册器；
#   所有包装 TU 链接进 integration_gtest（gtest_main 提供真正的 main）。
#   CLion 识别 gtest 目标后，Test 面板列出每个用例 → 单独 Run/Debug。
# ★ 职责边界：返回值校验归 logdiff（基线比对 run-rc），这里只保证"能跑、可断点"，
#   所以 TEST 体不断言返回值（有些正例本就返回非 0，如 test_tmpl_61 返回 7）。
# ★ 错误用例（*_error.cpp）是【非法程序】（本就被 clang/minicc 拒绝），
#   主机编译器编不过，一律不进这个二进制 —— 它们只由 logdiff 管。
# ★ 调试【编译器本身】不用这个目标：直接建 minicc 的运行配置，
#   Program arguments 填 tests/xxx.cpp -o /tmp/xxx，断点打在 src/*.cpp 即可。
file(GLOB_RECURSE ITEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
list(FILTER ITEST_SOURCES EXCLUDE REGEX "/tests/unit/")
# 错误用例（*_error*，本就是【非法程序】，被 clang/minicc 双双拒绝）不进这个
# 二进制 —— 主机编译器编不过，它们只由 logdiff 管。
list(FILTER ITEST_SOURCES EXCLUDE REGEX "_error")
# 命名没带 _error、但基线 rc=1 的【预期编译失败】用例（以 .logbaseline 的 rc 为准）：
set(ITEST_EXCLUDES
    tests/mi/test_mi_03_dynamic_cast.cpp      # 跨转型 B*→A* 未实现，预期 rc=1
    tests/mi/test_mi_11_ambiguous_member.cpp  # [class.member.lookup]/8 歧义报错用例
    tests/stl/test_stl_06_builtins.cpp        # 内建边界探测，预期 rc=1
    tests/tmpl/test_tmpl_42_order_ambiguous.cpp # [temp.class.order] 歧义，预期 rc=1
    tests/stl/test_stl_02_subscript.cpp       # minicc 的 v[i] 下标糖（无 operator[]），主机编译器不认
    tests/stl/test_stl_04_map.cpp             # 同上：m[k] 下标糖
    tests/tmpl/test_tmpl_15_deduce_forms.cpp  # return x（左值）按 T&& 返回：minicc 宽松放行，
                                              # clang 拒（rvalue ref 不能绑左值）——主机编译器编不过
)
foreach(rel ${ITEST_EXCLUDES})
    list(REMOVE_ITEM ITEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${rel})
endforeach()
# tests/search-path/ 是 -I 搜索路径特性的【具材目录】（probe_angle.cpp 用 <angled>
# include，必须配 -I 才合法），不是独立测试，整目录排除。
list(FILTER ITEST_SOURCES EXCLUDE REGEX "/tests/search-path/")

# 正例：每测试一个 gtest 可执行目标。文件里的 #ifdef MINICC_GTEST_TEST 块
# 此时生效（main 改名 + 同文件 TEST()）—— CLion 编辑器中绿色三角直接落在
# tests/ 的源文件上，点 Debug 即单步该文件。
# ★ EXCLUDE_FROM_ALL：点哪个才编哪个，不进 ctest/logdiff 账本，也不拖慢
#   ./build.sh 全量构建（94 个小目标的链接开销只在点击时支付）。
# ★ 每测试一个目标而非拼大二进制：不同文件里的同名自由函数（如 twice(int)）
#   拼一起会 multiple definition（实测踩到）。
foreach(src ${ITEST_SOURCES})
    get_filename_component(base ${src} NAME_WE)
    add_executable(itest_${base} ${src})
    set_target_properties(itest_${base} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_compile_definitions(itest_${base} PRIVATE MINICC_GTEST_TEST)
    target_compile_options(itest_${base} PRIVATE -Wno-everything)
    target_include_directories(itest_${base} PRIVATE tests)
    target_link_libraries(itest_${base} GTest::gtest GTest::gtest_main)
endforeach()
list(LENGTH ITEST_SOURCES ITEST_N)
message(STATUS "集成测试 gtest 包装：in-process ${ITEST_N} 个 → 每测试一个 itest_<名> 目标")

# ── 剩余用例：子进程包装（MiniccSubprocess 套件）──────────────────────────
# in-process 二进制之外的每一个 main 入口用例（错误用例 + minicc 方言用例，
# 主机编译器编不过）都包成【子进程用例】：TEST 体调 minicc 编译该文件、
# 核对退出码，rc=0 再运行产物核对 run-rc —— 期望值从 .logbaseline 解析
# （与 logdiff 同一 oracle）。这样 CLion Test 面板 = tests/ 全部 main 用例，
# 一个不缺。机理与调试语义见 tests/itest_subprocess_helper.h 头注释。
file(GLOB_RECURSE ITEST_ALL CONFIGURE_DEPENDS tests/*.cpp)
list(FILTER ITEST_ALL EXCLUDE REGEX "/tests/unit/")
list(FILTER ITEST_ALL EXCLUDE REGEX "/tests/search-path/")
set(ITEST_WRAPPERS "")
set(ITEST_SUB_N 0)
foreach(src ${ITEST_ALL})
    if(src IN_LIST ITEST_SOURCES)
        continue()
    endif()
    get_filename_component(base ${src} NAME_WE)
    # 基线名规则与 logdiff.sh 一致：tests/tmpl/x.cpp → tmpl__x.log
    file(RELATIVE_PATH blrel ${CMAKE_CURRENT_SOURCE_DIR} ${src})
    string(REGEX REPLACE "^tests/" "" blname ${blrel})
    string(REPLACE "/" "__" blname ${blname})
    string(REGEX REPLACE "\\.cpp$" "" blname ${blname})
    set(blog ${CMAKE_CURRENT_SOURCE_DIR}/.logbaseline/${blname}.log)
    if(NOT EXISTS ${blog})
        message(WARNING "  ${base}：缺 .logbaseline 日志，跳过子进程包装（先跑 ./logdiff.sh save）")
        continue()
    endif()
    file(READ ${blog} bcontent)
    string(REGEX MATCH "=== rc=([0-9]+)" _unused ${bcontent})
    set(crc ${CMAKE_MATCH_1})
    set(hasrun 0)
    set(rrc 0)
    string(REGEX MATCH "=== run-rc=([0-9]+)" _unused ${bcontent})
    if(CMAKE_MATCH_1)
        set(hasrun 1)
        set(rrc ${CMAKE_MATCH_1})
    endif()
    math(EXPR ITEST_SUB_N "${ITEST_SUB_N} + 1")
    set(wrapper ${CMAKE_CURRENT_BINARY_DIR}/itest_wrappers/sub_${base}.cpp)
    file(WRITE ${wrapper}
        "// 本文件由 CMake 生成，勿手改。子进程包装的 minicc 用例（主机编译器编不过的那批）。\n"
        "#include \"${CMAKE_CURRENT_SOURCE_DIR}/tests/itest_subprocess_helper.h\"\n"
        "TEST(MiniccSubprocess, ${base}) { itest_subprocess::runCase(\"${base}\"); }\n")
    set(ITEST_WRAPPERS ${ITEST_WRAPPERS} ${wrapper})
    set_source_files_properties(${wrapper} PROPERTIES COMPILE_DEFINITIONS
        "ITEST_SRC=\"${src}\";ITEST_EXPECT_COMPILE_RC=${crc};ITEST_HAS_RUN=${hasrun};ITEST_EXPECT_RUN_RC=${rrc}")
endforeach()
message(STATUS "集成测试 gtest 包装：subprocess ${ITEST_SUB_N} 个用例")

add_executable(integration_gtest ${ITEST_WRAPPERS})
target_include_directories(integration_gtest PRIVATE include tests)
target_link_libraries(integration_gtest GTest::gtest GTest::gtest_main)
# 子进程用例要以子进程方式启动本项目的 minicc：
add_dependencies(integration_gtest minicc)
target_compile_definitions(integration_gtest PRIVATE
    "MINICC_BIN_PATH=\"$<TARGET_FILE:minicc>\"")
gtest_discover_tests(integration_gtest)

# ─── ministl 测试 ───────────────────────────────────────────────────────────
# ministl 本身是可以独立配置的 CMake 项目，这里 add_subdirectory 进来，
# 这样 CLion 打开 mycompiler 时能一并看到它的 5 个测试目标、
# 每个 TEST() 左侧出现绿色三角，不必再另开一个窗口。
# googletest 用的是上面同一份 FetchContent 声明（同 URL、同 tag），
# 会被识别为已 populate 的依赖直接复用，不会重复下载或重复构建。
add_subdirectory(ministl)

