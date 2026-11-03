#!/usr/bin/env bash
# 突变验证：摘掉 5 条判据 ⇒ 对应单测/集成用例必须变红（红线 ⑦）
#
# 本批的判据（Stage 3 批 5：vtable 槽 → 声明句柄）：
#   m1 覆写时主表【不换句柄】      ⇒ 槽仍指向基类的声明（动态分派失效）
#   m2 新槽【不挂句柄】            ⇒ 槽里没有声明（符号无处可取）
#   m3 槽的符号【不从句柄取】      ⇒ 槽与声明脱钩（两处各算一遍的老毛病）
#   m4 析构槽判据【退回子串匹配】  ⇒ 名字含 "dtor" 的普通虚函数被误认成析构
#   m5 槽的符号【由槽自己拼】      ⇒ 丢掉形参类型链 ⇒ .quad 命中不到 .globl
#
# 用法：./tools/verify-vtable-handle-mutations.sh
#
# ★ 还原用 cp 备份（项目铁律：严禁 git checkout 未提交文件，B34 批次教训）。
set -u
cd "$(dirname "$0")/.."

SEMA=src/sema/semantic_analyzer.cpp
CODEGEN=src/codegen/codegen.cpp
AST=include/ast.h
BAK_SEMA=/tmp/vtable_handle_mut_sema.cpp
BAK_CODEGEN=/tmp/vtable_handle_mut_codegen.cpp
BAK_AST=/tmp/vtable_handle_mut_ast.h
cp "$SEMA" "$BAK_SEMA"
cp "$CODEGEN" "$BAK_CODEGEN"
cp "$AST" "$BAK_AST"

restore() {
  cp "$BAK_SEMA" "$SEMA"
  cp "$BAK_CODEGEN" "$CODEGEN"
  cp "$BAK_AST" "$AST"
}
trap restore EXIT

# 单测：VTableHandle.* —— 期望"有红"（即至少一条失败）
run_unit() {
  local label="$1"; shift
  cmake --build build-linux -j6 >/dev/null 2>&1
  local out
  out=$(ctest --test-dir build-linux -R "VTableHandle\..*" 2>&1)
  local failed
  failed=$(echo "$out" | grep -oE "[0-9]+ tests failed" | head -1)
  echo "── $label"
  echo "$out" | grep -E "tests passed|tests failed" | head -2
  if echo "$out" | grep -q "0 tests failed"; then
    echo "   ★ 突变未被捕获（应变红却绿了）"
  else
    echo "   ✓ 突变被捕获（变红）"
  fi
}

# 集成用例：编译 + 运行，期望 run-rc == 0（被突变破坏时 rc != 0）
run_integration() {
  local label="$1"; shift
  cmake --build build-linux -j6 >/dev/null 2>&1
  local tmp=/tmp/vtable_handle_mut_it
  mkdir -p "$tmp"
  local out
  out=$(./build-linux/minicc tests/lang/test_basics_34_vtable_slot_handle.cpp \
          -o "$tmp/a.out" 2>&1)
  local crc=$?
  local rrc="n/a"
  if [ $crc -eq 0 ]; then
    "$tmp/a.out" >/dev/null 2>&1
    rrc=$?
  fi
  echo "── $label"
  echo "   编译 rc=$crc  运行 rc=$rrc"
  if [ "$crc" != "0" ] || [ "$rrc" != "0" ]; then
    echo "   ✓ 突变被捕获（变红）"
    echo "$out" | grep -E "undefined|LINK ERROR" | head -3
  else
    echo "   ★ 突变未被捕获（应变红却绿了）"
  fi
}

mutate() {
  python3 - "$1" <<'PY'
import sys
which = sys.argv[1]

sema = "src/sema/semantic_analyzer.cpp"
codegen = "src/codegen/codegen.cpp"
ast = "include/ast.h"

if which == "m1":       # 覆写时主表不换句柄
    p = sema
    old = "entry.method = method;   // 覆写 ⇒ 槽位改指本类的实现"
    new = "// MUTATION m1: 覆写时不换句柄（主表）"
elif which == "m2":     # 新槽不挂句柄
    p = sema
    old = "entry.method = method;       // 本类新引入的虚函数：新占一个槽位（符号由句柄给出）"
    new = "// MUTATION m2: 新槽不挂句柄"
elif which == "m3":     # 槽的符号不从句柄取
    p = ast
    old = "return entry.method ? entry.method->mangledName : std::string();"
    new = "return entry.baseFunctionName;  // MUTATION m3: 不从句柄取"
elif which == "m4":     # 析构判据退回子串匹配
    p = codegen
    old = 'return entry.method && entry.method->name.starts_with("~");'
    new = 'return entry.method && entry.method->mangledName.find("dtor") != std::string::npos;  // MUTATION m4'
elif which == "m5":     # 槽的符号由槽自己拼（丢形参类型链）
    p = ast
    old = "return entry.method ? entry.method->mangledName : std::string();"
    new = 'return entry.method ? entry.method->ownerClassName + "_" + entry.method->name : std::string();  // MUTATION m5'
else:
    raise SystemExit("unknown mutation " + which)

s = open(p).read()
if s.count(old) != 1:
    raise SystemExit("mutation %s: anchor count = %d (expected 1)" % (which, s.count(old)))
open(p, "w").write(s.replace(old, new))
PY
}

echo "════ 突变验证：vtable 槽 → 声明句柄（5 条判据）════"
for m in m1 m2 m3 m4 m5; do
  restore
  mutate "$m"
  case "$m" in
    m1) run_unit "① 覆写时主表不换句柄" ;;
    m2) run_unit "② 新槽不挂句柄" ;;
    m3) run_unit "③ 槽的符号不从句柄取" ;;
    m4) run_unit "④ 析构槽判据退回子串匹配" ;;
    m5) run_unit "⑤ 槽的符号由槽自己拼"; run_integration "⑤（集成侧）槽的符号由槽自己拼" ;;
  esac
done
restore
echo "════ 已还原源文件 ════"
