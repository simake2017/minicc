#!/bin/bash
# =============================================================================
# check-comment-style.sh —— 源码注释的「四段式」合规护栏
# =============================================================================
# 用途：把「注释标准」从一次性人工检查变成【可重复执行的断言】——
#       否则每次改代码都要靠肉眼回想"这段注释该几行、该不该带锚点"，
#       而注释恰好是最容易被"顺手多写两行"破坏的地方。
#
# 依据：~/wiki/common/docs/design-standard/comment-standard.md
#       本批实施计划：docs/specs/comment-standard-conformance.spec.md
#
# 判据（三条，两条硬失败 + 一条统计）：
#   ① 体内注释块 ≤ 25 行（复杂算法放宽到 30，且该块须写明放宽理由）；
#      结构性文件头豁免（标准 §6）。
#   ② 源码里不出现私人笔记标记（标准「禁止：私人笔记」）。
#   ③ 统计 clang 锚点的「带文件路径」比例（标准硬约束 7.1 的可 grep 形式）。
#
# 用法：
#   ./check-comment-style.sh          跑检查（有违规即退出码 1）
#   ./check-comment-style.sh --report 只出统计、不判失败
# =============================================================================

set -u
cd "$(dirname "$0")"

REPORT_ONLY=0
[ "${1:-}" = "--report" ] && REPORT_ONLY=1

# ── 判据阈值（突变验证：改这里就能让脚本变红/变绿）────────────────────────
MAX_BLOCK=25            # 普通注释块的行数上限
MAX_BLOCK_ALGO=30       # 复杂算法放宽后的上限
ALGO_MARK="放宽"        # 放宽时该块必须出现的说明字样
FILE_HEADER_LINES=60    # 文件前 N 行内的注释块视为【结构性文件头】（豁免）
PRIVATE_MARKS="mrd|wangyang|TODO|FIXME|XXX|HACK"   # 私人笔记标记黑名单
EXCUSE_MARK="⚠"          # 豁免标记：块内出现它 ⇒ 该块允许留"未补路径"的裸锚点

python3 - "$MAX_BLOCK" "$MAX_BLOCK_ALGO" "$ALGO_MARK" "$FILE_HEADER_LINES" "$PRIVATE_MARKS" "$REPORT_ONLY" "$EXCUSE_MARK" <<'PYEOF'
import os, re, sys

MAX_BLOCK       = int(sys.argv[1])
MAX_BLOCK_ALGO  = int(sys.argv[2])
ALGO_MARK       = sys.argv[3]
FILE_HEADER     = int(sys.argv[4])
PRIVATE_MARKS  = re.compile(r'\b(' + sys.argv[5] + r')\b')
REPORT_ONLY     = int(sys.argv[6])
EXCUSE_MARK     = sys.argv[7]

# 裸锚点：行里出现 "对照 clang：Xxx::yyy" 形态的 clang 符号名。
#   只认【紧跟在 "clang" 冒号之后】的符号 —— 散文里的 "clang 的 Xxx::yyy"
#   不算（那是比较陈述，不是源码锚点）。
BARE_ANCHOR = re.compile(
    r'(?:对照|对应|等价)[^/]{0,4}clang[：:]\s*(?:clang::)?'
    r'[A-Z][A-Za-z0-9_]*::[A-Za-z0-9_~]+')

# 只查产品源码 —— tests/ 的注释是「用例说明」而非「实现注释」，
# 标准不要求它们四段式（且改它们会动 logdiff 基线）。
# ★ 扫描范围【显式列出】src/ 与 include/ 两个目录，而不是"排除若干目录"——
#   后者会在新出现的构建目录（.cache/、cmake-build-debug/、_deps/）里
#   把第三方源码（gmock/gtest 的注释动辄 80 行）一起判红。
SCAN_ROOTS = ('src', 'include')

def source_files():
    for root_dir in SCAN_ROOTS:
        for root, dirs, files in os.walk(root_dir):
            for f in sorted(files):
                if f.endswith(('.cpp', '.h')):
                    yield os.path.join(root, f)

def comment_blocks(lines):
    """把连续的 // 注释行合成块；返回 (起始行号, 结束行号, 行列表)。"""
    blocks, cur = [], []
    for i, l in enumerate(lines, 1):
        if l.strip().startswith('//'):
            cur.append((i, l))
        else:
            if cur:
                blocks.append(cur); cur = []
    if cur:
        blocks.append(cur)
    return blocks

overlong, private, bare_anchors = [], [], []
total_blocks, exempt_blocks = 0, 0
anchor_total, anchor_with_path = 0, 0

for path in source_files():
    try:
        lines = open(path, encoding='utf-8').read().split('\n')
    except (OSError, UnicodeDecodeError):
        continue

    for blk in comment_blocks(lines):
        total_blocks += 1
        start, end = blk[0][0], blk[-1][0]
        n = end - start + 1
        text = '\n'.join(l for _, l in blk)

        # ① 结构性文件头豁免：块起于文件前 FILE_HEADER_LINES 行 ⇒ 豁免
        if start <= FILE_HEADER:
            exempt_blocks += 1
            continue

        limit = MAX_BLOCK
        if ALGO_MARK in text:          # 自称复杂算法 ⇒ 允许放宽
            # ★ 取 max 而不是直接赋值：否则把 MAX_BLOCK 调大来【放松】判据时，
            #   放宽分支仍会把它按回 MAX_BLOCK_ALGO ⇒ 放松无效（突变验证会露馅）。
            limit = max(MAX_BLOCK_ALGO, MAX_BLOCK)
        if n > limit:
            overlong.append((path, start, end, n, limit))

        # ④ 裸锚点：写出 clang 符号名却没给文件路径，且整块无 ⚠ 豁免登记
        if EXCUSE_MARK not in text:
            for ln, l in blk:
                if BARE_ANCHOR.search(l):
                    bare_anchors.append((path, ln, l.strip()[:90]))

    for i, l in enumerate(lines, 1):
        s = l.strip()
        if not s.startswith('//'):
            continue
        # ② 私人笔记标记
        if PRIVATE_MARKS.search(s):
            private.append((path, i, s[:90]))
        # ③ 锚点统计：提到 clang 锚点的行里，带文件路径的比例
        if re.search(r'(对照 ?clang|源码锚点|clang/lib|clang/include)', s):
            anchor_total += 1
            if re.search(r'clang/[A-Za-z0-9_/]+\.(?:cpp|h)', s):
                anchor_with_path += 1

print("=" * 70)
print("  源码注释合规检查（四段式标准，docs/specs/comment-standard-conformance.spec.md）")
print("=" * 70)
print(f"  扫描注释块      : {total_blocks}（其中结构性文件头豁免 {exempt_blocks}）")
print(f"  判据①块长      : 上限 {MAX_BLOCK} 行"
      f"（复杂算法放宽至 {MAX_BLOCK_ALGO}，须含「{ALGO_MARK}」说明）")
print(f"  判据②私人笔记  : 黑名单 {PRIVATE_MARKS.pattern}")
print(f"  判据③锚点路径  : {anchor_with_path}/{anchor_total} "
      f"= {100.0 * anchor_with_path / anchor_total if anchor_total else 0:.1f}%")

if bare_anchors:
    print()
    print(f"  ❌ 判据④ 裸锚点 {len(bare_anchors)} 处（clang 符号名未给文件路径、"
          f"且块内无「{EXCUSE_MARK}」豁免登记）：")
    for p, i, s in bare_anchors:
        print(f"     {p}:{i}  {s}")

if overlong:
    print()
    print(f"  ❌ 判据① 超长注释块 {len(overlong)} 处：")
    for p, s, e, n, lim in overlong:
        print(f"     {p}:{s}-{e}  {n} 行（上限 {lim}）")

if private:
    print()
    print(f"  ❌ 判据② 私人笔记标记 {len(private)} 处：")
    for p, i, s in private:
        print(f"     {p}:{i}  {s}")

if REPORT_ONLY:
    print()
    print("  ℹ️  --report 模式：不判失败")
    sys.exit(0)

if overlong or private or bare_anchors:
    print()
    print("  ❌ 注释标准未达标（修法见 docs/learn/50 §4）")
    sys.exit(1)

print()
print("  ✅ 注释标准全部达标")
sys.exit(0)
PYEOF
