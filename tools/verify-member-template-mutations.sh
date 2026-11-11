#!/usr/bin/env bash
# 突变验证：摘掉本批的判据 ⇒ 对应单测/集成用例必须变红（红线 ⑦）
#
# 本批的判据（ROADMAP §0.1 批 0-4：成员模板的形参表收口到自由模板那一份；BUGS.md B56）：
#   m1 不同步 frame.count              ⇒ 成员模板自己的形参在方法体里认不出
#                                        （建成 Class 节点而非 TemplateParam 节点）
#   m2 去掉无名形参分支                ⇒ `template<class>` 在成员侧被拒
#   m3 去掉默认实参分支                ⇒ `template<class T = int>` 在成员侧被拒
#   m4 模板模板形参分支改成报错        ⇒ `template<template<class> class C>` 在成员侧被拒
#   m5 成员分支改回"自写一份"         ⇒ 整条收口作废（复现旧实现 ⇒ 5 例全红）
#
# 用法：./tools/verify-member-template-mutations.sh
#
# ★ 还原用 cp 备份（项目铁律：严禁 git checkout 未提交文件，B34 批次教训）。
set -u
cd "$(dirname "$0")/.."

PARSER=src/parse/parser.cpp
BAK_PARSER=/tmp/member_tmpl_mut_parser.cpp
cp "$PARSER" "$BAK_PARSER"

restore() {
  cp "$BAK_PARSER" "$PARSER"
}
trap restore EXIT

# 单测：期望"有红"（即至少一条失败）
run_unit() {
  local desc="$1"; local filter="$2"
  cmake --build build-linux -j6 --target unit_tests > /tmp/member_tmpl_mut_build.log 2>&1
  if [ $? -ne 0 ]; then
    echo "   ⚠️  构建失败（突变引入了编译错误）—— 视为已捕获"
    grep -E "error" /tmp/member_tmpl_mut_build.log | head -3
    return 0
  fi
  local out
  out=$(./build-linux/unit_tests --gtest_filter="$filter" 2>&1)
  if echo "$out" | grep -qE "FAILED|\[  FAILED  \]"; then
    echo "   ✅ $desc ⇒ 对应单测变红（已捕获）"
    echo "$out" | grep -E "^\[  FAILED  \]" | head -6 | sed 's/^/        /'
  else
    echo "   ★ 突变未被捕获（应变红却绿了）: $desc"
    echo "$out" | tail -3 | sed 's/^/        /'
  fi
}

# 集成用例：编译该文件并跑，看退出码是否不再是期望值
run_integration() {
  local desc="$1"; local src="$2"; local expect_rc="$3"
  cmake --build build-linux -j6 --target minicc > /tmp/member_tmpl_mut_minicc.log 2>&1
  if [ $? -ne 0 ]; then
    echo "   ⚠️  构建失败（突变引入了编译错误）—— 视为已捕获"
    return 0
  fi
  ./build-linux/minicc "$src" -o /tmp/member_tmpl_mut_it.bin > /tmp/member_tmpl_mut_it.log 2>&1
  local crc=$?
  if [ $crc -ne 0 ]; then
    echo "   ✅ $desc ⇒ 编译期就红了（rc=$crc，已捕获）"
    return 0
  fi
  /tmp/member_tmpl_mut_it.bin > /dev/null 2>&1
  local rrc=$?
  if [ "$rrc" != "$expect_rc" ]; then
    echo "   ✅ $desc ⇒ 运行退出码 $rrc ≠ 期望 $expect_rc（已捕获）"
  else
    echo "   ★ 突变未被捕获（运行退出码仍是 $expect_rc）: $desc"
  fi
}

mutate() {
  python3 - "$1" <<'PY'
import sys
which = sys.argv[1]
p = "src/parse/parser.cpp"
s = open(p, encoding="utf-8").read()

if which == "m1":       # 不同步 frame.count（复现"自己的形参认不出"）
    old = """            frame.count = decl.templateParams.size();   // ← 可见范围同步扩张"""
    new = """            // MUTATION m1: 不同步 frame.count（形参在模板体里认不出）"""
elif which == "m2":     # 去掉无名形参分支
    old = """                    param.name = "$unnamed" + std::to_string(unnamedSeq++);
                    param.isUnnamed = true;"""
    new = """                    error("unnamed template parameters are not implemented");
                    // MUTATION m2: 去掉无名形参分支"""
elif which == "m3":     # 去掉默认实参分支
    old = """            if (match(TokenType::Assign)) {
                if (check(TokenType::IntLiteral)) {"""
    new = """            if (false) {   // MUTATION m3: 去掉默认实参分支
                if (check(TokenType::IntLiteral)) {"""
elif which == "m4":     # 模板模板形参分支改成报错
    old = """            else if (check(TokenType::KwTemplate)) {
                // ── 模板模板参数 [temp.param]/4 ──"""
    new = """            else if (check(TokenType::KwTemplate)) {
                // MUTATION m4: 模板模板形参分支改成报错
                error("template template parameters are not implemented");
                // ── 模板模板参数 [temp.param]/4 ──"""
elif which == "m5":     # 成员分支改回"自写一份"（复现旧实现）
    old = """            parseTemplateParameterList(*memTmpl, memFrame);"""
    new = """            // MUTATION m5: 成员分支改回"自写一份"的简化形参表（复现旧实现）
            if (!check(TokenType::Greater)) {
                do {
                    auto tp = std::make_shared<TemplateParam>();
                    tp->location = current().location;
                    if (check(TokenType::KwTypename) || check(TokenType::KwClass)) {
                        advance();
                        tp->kind = TemplateParamKind::Type;
                        tp->name = expect(TokenType::Identifier,
                            "Expected parameter name in member template").text;
                    }
                    else {
                        error("template template parameters in member templates "
                              "are not implemented");
                    }
                    memTmpl->templateParams.push_back(tp);
                    memTmpl->typeParams.push_back(tp->name);
                } while (match(TokenType::Comma));
            }
            expect(TokenType::Greater,
                   "Expected '>' to close member template parameter list");"""
else:
    raise SystemExit("unknown mutation " + which)

if s.count(old) != 1:
    raise SystemExit("mutation %s: anchor count = %d (expected 1)" % (which, s.count(old)))
open(p, "w", encoding="utf-8").write(s.replace(old, new))
PY
}

echo "════ 突变验证：成员模板的形参表收口（5 条判据）════"
for m in m1 m2 m3 m4 m5; do
  restore
  mutate "$m"
  case "$m" in
    m1) run_unit "m1 不同步 frame.count" 'MemberTemplateParams.*' ;;
    m2) run_unit "m2 去掉无名形参分支" 'MemberTemplateParams.*' ;;
    m3) run_unit "m3 去掉默认实参分支" 'MemberTemplateParams.*' ;;
    m4) run_unit "m4 模板模板形参分支改成报错" 'MemberTemplateParams.*' ;;
    m5) run_unit "m5 成员分支改回自写一份" 'MemberTemplateParams.*' ;;
  esac
  # 集成侧：成员模板的四种形参形态任一失守都会让退出码 ≠ 22
  run_integration "$m（集成侧）" tests/tmpl/test_tmpl_78_member_template_params.cpp 22
done
restore
cmake --build build-linux -j6 --target minicc > /dev/null 2>&1
cmake --build build-linux -j6 --target unit_tests > /dev/null 2>&1
echo "════ 已还原源文件 ════"
