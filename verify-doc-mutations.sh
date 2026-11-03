#!/usr/bin/env bash
# =============================================================================
# verify-doc-mutations.sh —— check-doc-standard.sh 的突变验证
# =============================================================================
# 【为什么要有这个脚本】
#   守卫脚本自己也会骗人：一个恒真的判据能让脚本永远绿。
#   本脚本对每条判据做【两次】实测，把"能抓到"和"判据是承重的"分开证：
#     ① 引入违规  ⇒ 脚本必须【红】（证明判据不是摆设，能真抓到）
#     ② 放松判据  ⇒ 脚本必须【绿】（证明红是这条判据造成的，不是别的原因）
#   只做①不做②，会把"脚本恰好因为别的原因红了"误当成"判据有效"。
#
# ★【隔离】三层，每层各管一件事：
#   第一层 探针模式（CHECK_DOC_PROBE）：只判探针目录里的文档。
#           —— 不隔离的话，真实文档那几百处违规会淹没结果，"红"分不清是谁造成的。
#   第二层 判据放松（CHECK_DOC_RELAX）：一次只放松【待验证的那一条】。
#           —— 不隔离的话，"放松 X 后仍红"不能证明 X 是原因（可能是 Y 还在报）。
#   第三层 一条违规一��探针：每个探针只缺【一项】，其余判据全绿。
#           —— 不隔离的话，"红"可能来自另一个判据。
#
# ★【判据① 与判据③ 的分支覆盖】
#   ①头部 有两个子判据：缺三行头部 / 缺「变更记录」表 —— 各自单独测。
#   ③元数据 有两个来源：frontmatter 缺必需键 / 顶部没有 frontmatter / BUGS 条目缺三字段。
#   ④占位符 有两个来源：xx_ 标识符 / jsonc 块里的 <type> —— 各自单独测。
#
# 【探针机制】
#   探针放在仓库内的 .doc-mutant-probe/（必须【在仓库内】——判据⑤要用 git 基线
#   比对行数，路径必须能被 git 解析成仓库相对路径），跑完即删（trap 保证）。
#   判据⑤ 的基线用【临时 git 索引】（GIT_INDEX_FILE）写入：把一份 N 行的内容
#   登记进临时索引，再把工作区文件截短 ⇒ 基线 N 行、当前 M<N 行 ⇒ 必须报红。
#   ★ 用临时索引而不是真索引：验证脚本绝不能改动被验证对象的仓库状态。
#
# 【用法】
#   ./verify-doc-mutations.sh          # 跑全部判据的突变验证
# =============================================================================

set -u
cd "$(dirname "$0")"

PROBE_DIR=".doc-mutant-probe"
TMPDIR_MUT="$(mktemp -d)"
MUTANT_IDX="$TMPDIR_MUT/index"

# ★ 临时 git 索引：只用于给探针造"基线行数"，绝不影响仓库真索引。
export GIT_INDEX_FILE="$MUTANT_IDX"
git read-tree HEAD 2>/dev/null || true

cleanup() {
    unset GIT_INDEX_FILE
    # ★ 只删本脚本【自己造出来】的两个路径，不碰任何别的目录：
    #   $PROBE_DIR   —— 仓库内固定名 .doc-mutant-probe（脚本自己 mkdir 的）
    #   $TMPDIR_MUT  —— mktemp -d 造出来的 /tmp/tmp.XXXXXX（形态匹配才删）
    if [ -d "$PROBE_DIR" ]; then
        rm -rf -- "$PROBE_DIR"
    fi
    case "$TMPDIR_MUT" in
        /tmp/tmp.*)
            if [ -d "$TMPDIR_MUT" ]; then
                rm -rf -- "$TMPDIR_MUT"
            fi
            ;;
    esac
}
trap cleanup EXIT

PASS=0
FAIL=0

# ── 通用工具 ────────────────────────────────────────────────────────────────

# 一份【除目标缺陷外全部合规】的技术文档骨架。
# $1 = 额外插入的内容（放在正文里）
tech_skeleton() {
    cat <<'EOF'
# 探针技术文档

读者：突变验证探针
审法：突变验证探针
名词版本：v0

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| v0 | 2026-11-02 | 探针初始版本 |

## 正文

EOF
    printf '%s\n' "$1"
    cat <<'EOF'

## Out of Scope

探针专用，不表达任何真实设计意图。

## 观测点

探针专用，不表达任何真实设计意图。
EOF
}

learn_skeleton() {
    cat <<'EOF'
# 探针学习文档

## 0 定位与能力清单

探针专用，不表达任何真实设计意图。

## 真实版多做了什么、为什么

探针专用，不表达任何真实设计意图。

## 演进记录

| 版本 | 日期 | 变更 |
|---|---|---|
| v0 | 2026-11-02 | 探针初始版本 |
EOF
}

reset_probe() {
    rm -rf "$PROBE_DIR"
    mkdir -p "$PROBE_DIR/tech" "$PROBE_DIR/learn" "$PROBE_DIR/post"
}

report() {
    local name="$1" step="$2" want="$3" got="$4" detail="$5"
    local mark
    if [ "$want" = "$got" ]; then
        mark="✅"; PASS=$((PASS + 1))
    else
        mark="❌"; FAIL=$((FAIL + 1))
    fi
    printf "  %s %-26s %-14s 期望=%s 实得=%s  %s\n" \
           "$mark" "$name" "$step" "$want" "$got" "$detail"
}

# $1 = 放松的判据（逗号分隔的英文代号；空串 = 全部严格）
run_guard() {
    CHECK_DOC_PROBE="$PROBE_DIR" CHECK_DOC_RELAX="$1" \
        ./check-doc-standard.sh > "$TMPDIR_MUT/guard.log" 2>&1
    if [ $? -eq 0 ]; then echo 0; else echo 1; fi
}

# 只看命中的判据名（写进日志便于排查）
hit() {
    grep -o '\[①头部\]\|\[②骨架节\]\|\[③元数据\]\|\[④占位符\]\|\[⑤资产\]' \
        "$TMPDIR_MUT/guard.log" | sort -u | tr '\n' ' '
}

echo "======================================================================"
echo "  文档守卫脚本的突变验证（check-doc-standard.sh）"
echo "======================================================================"

# ── 判据① 头部 ─────────────────────────────────────────────────────────────
echo ""
echo "  ▸ 判据① 头部（缺三行头部）"
reset_probe
{ echo "# 探针技术文档"; echo ""; echo "## 正文"; echo ""; echo "探针。"; echo "";
  echo "## Out of Scope"; echo ""; echo "探针。"; echo "";
  echo "## 观测点"; echo ""; echo "探针。"; } > "$PROBE_DIR/tech/h1.md"
got=$(run_guard "")
report "① 缺三行头部" "①严格时" "1" "$got" "（红：命中 $(hit)）"
got=$(run_guard "header")
report "① 缺三行头部" "①放松后" "0" "$got" "（绿：命中 $(hit)）"

echo ""
echo "  ▸ 判据① 头部（缺「变更记录」表）"
reset_probe
tech_skeleton "" | grep -v '^## 变更记录$' | grep -v '^| 版本 | 日期 | 变更 |$' \
    | grep -v '^|---|---|---|$' | grep -v '^| v0 | 2026-11-02 | 探针初始版本 |$' \
    > "$PROBE_DIR/tech/h2.md"
got=$(run_guard "")
report "① 缺变更记录" "①严格时" "1" "$got" "（红：命中 $(hit)）"
got=$(run_guard "header")
report "① 缺变更记录" "①放松后" "0" "$got" "（绿：命中 $(hit)）"

# ── 判据② 骨架节（技术文档分支）────────────────────────────────────────────
echo ""
echo "  ▸ 判据② 骨架节 · 技术文档（缺 Out of Scope）"
reset_probe
tech_skeleton "" | grep -v '^## Out of Scope$' \
    | sed '/^## Out of Scope$/,+2d' > "$PROBE_DIR/tech/s1.md"
got=$(run_guard "")
report "② 技术文档缺 Out of Scope" "②严格时" "1" "$got" "（红：命中 $(hit)）"
got=$(run_guard "skeleton")
report "② 技术文档缺 Out of Scope" "②放松后" "0" "$got" "（绿：命中 $(hit)）"

# ── 判据② 骨架节（learn 文档分支）──────────────────────────────────────────
echo ""
echo "  ▸ 判据② 骨架节 · learn 文档（缺「能力清单」）"
reset_probe
learn_skeleton | sed '/^## 0 定位与能力清单$/,+2d' > "$PROBE_DIR/learn/01-l1.md"
got=$(run_guard "")
report "② learn 缺能力清单" "②严格时" "1" "$got" "（红：命中 $(hit)）"
got=$(run_guard "skeleton")
report "② learn 缺能力清单" "②放松后" "0" "$got" "（绿：命中 $(hit)）"

# ── 判据③ 元数据（frontmatter 缺必需键）────────────────────────────────────
echo ""
echo "  ▸ 判据③ 元数据（frontmatter 缺必需键）"
reset_probe
cat > "$PROBE_DIR/post/p1.md" <<'EOF'
---
title: 探针复盘
date: 2026-11-02
category: probe
tags: probe
type: probe
status: open
---

# 探针复盘

探针专用，不表达任何真实设计意图。
EOF
got=$(run_guard "")
report "③ frontmatter 缺键" "③严格时" "1" "$got" "（红：命中 $(hit)）"
got=$(run_guard "meta")
report "③ frontmatter 缺键" "③放松后" "0" "$got" "（绿：命中 $(hit)）"

echo ""
echo "  ▸ 判据③ 元数据（顶部没有 frontmatter）"
reset_probe
cat > "$PROBE_DIR/post/p2.md" <<'EOF'
# 探针复盘

探针专用，不表达任何真实设计意图。
EOF
got=$(run_guard "")
report "③ 无 frontmatter" "③严格时" "1" "$got" "（红：命中 $(hit)）"
got=$(run_guard "meta")
report "③ 无 frontmatter" "③放松后" "0" "$got" "（绿：命中 $(hit)）"

echo ""
echo "  ▸ 判据③ 元数据（BUGS 条目缺三字段）"
reset_probe
cat > "$PROBE_DIR/bugs.md" <<'EOF'
# 探针缺陷台账

## B1 探针条目

探针专用，不表达任何真实设计意图。
EOF
got=$(run_guard "")
report "③ BUGS 条目缺字段" "③严格时" "1" "$got" "（红：命中 $(hit)）"
got=$(run_guard "meta")
report "③ BUGS 条目缺字段" "③放松后" "0" "$got" "（绿：命中 $(hit)）"

# ── 判据④ 占位符 ───────────────────────────────────────────────────────────
echo ""
echo "  ▸ 判据④ 占位符（xx_ 标识符）"
reset_probe
tech_skeleton "这里有一个没写完的洞：xx_placeholder" > "$PROBE_DIR/tech/p1.md"
got=$(run_guard "")
report "④ xx_ 标识符" "④严格时" "1" "$got" "（红：命中 $(hit)）"
got=$(run_guard "placeholder")
report "④ xx_ 标识符" "④放松后" "0" "$got" "（绿：命中 $(hit)）"

echo ""
echo "  ▸ 判据④ 占位符（jsonc 块里的 <type>）"
reset_probe
{
    tech_skeleton ""
    echo '```jsonc'
    echo '{ "kind": "<type>", "args": [..._] }'
    echo '```'
} > "$PROBE_DIR/tech/p2.md"
got=$(run_guard "")
report "④ jsonc 块占位符" "④严格时" "1" "$got" "（红：命中 $(hit)）"
got=$(run_guard "placeholder")
report "④ jsonc 块占位符" "④放松后" "0" "$got" "（绿：命中 $(hit)）"

# ── 判据⑤ 资产 ─────────────────────────────────────────────────────────────
echo ""
echo "  ▸ 判据⑤ 资产（行数少于 git 基线）"
reset_probe
{
    tech_skeleton ""
    for i in $(seq 1 40); do echo "填充行 $i"; done
} > "$PROBE_DIR/tech/a1.md"
# 把 40+ 行的版本登记进【临时索引】当基线，再把工作区截短
blob=$(git hash-object -w "$PROBE_DIR/tech/a1.md")
git update-index --add --cacheinfo "100644,$blob,$PROBE_DIR/tech/a1.md"
{ tech_skeleton ""; } > "$PROBE_DIR/tech/a1.md"
got=$(run_guard "")
report "⑤ 行数低于基线" "⑤严格时" "1" "$got" "（红：命中 $(hit)）"
got=$(run_guard "asset")
report "⑤ 行数低于基线" "⑤放松后" "0" "$got" "（绿：命中 $(hit)）"

# ── 判据⑥ 引用可达 ─────────────────────────────────────────────────────────
echo ""
echo "  ▸ 判据⑥ 引用可达（引用了不存在的源码文件）"
reset_probe
tech_skeleton "见 src/zz_does_not_exist.cpp:42 这一处的实现" > "$PROBE_DIR/tech/c1.md"
got=$(run_guard "")
report "⑥ 引用不存在的文件" "⑥严格时" "1" "$got" "（红：命中 $(hit)）"
got=$(run_guard "cite")
report "⑥ 引用不存在的文件" "⑥放松后" "0" "$got" "（绿：命中 $(hit)）"

echo ""
echo "  ▸ 判据⑥ 引用可达（行号越界）"
reset_probe
tech_skeleton "见 src/parse/parser.cpp:999999 这一处的实现" > "$PROBE_DIR/tech/c2.md"
got=$(run_guard "")
report "⑥ 行号越界" "⑥严格时" "1" "$got" "（红：命中 $(hit)）"
got=$(run_guard "cite")
report "⑥ 行号越界" "⑥放松后" "0" "$got" "（绿：命中 $(hit)）"

# ── 收官：全放松必须绿；不放松必须红 ───────────────────────────────────────
echo ""
echo "  ▸ 收官：判据全部放松 / 全部严格"
reset_probe
tech_skeleton "这里有一个没写完的洞：xx_placeholder" > "$PROBE_DIR/tech/z1.md"
got=$(run_guard "header,skeleton,meta,placeholder,asset,cite")
report "收官" "全放松" "0" "$got" "（绿：六条判据全关 ⇒ 不可能再报）"
got=$(run_guard "")
report "收官" "全严格" "1" "$got" "（红：探针违规必须被抓到）"

echo ""
echo "======================================================================"
echo "  突变验证结果：通过 $PASS 项，失败 $FAIL 项"
echo "======================================================================"

if [ "$FAIL" -ne 0 ]; then
    echo "  ❌ 存在未通过项 —— 说明有判据是【摆设】（恒真 / 抓不到 / 抓错了原因）"
    exit 1
fi

echo "  ✅ 全部判据均为承重判据（能抓到违规，且放松后不再报）"
exit 0
