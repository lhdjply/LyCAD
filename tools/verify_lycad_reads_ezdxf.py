#!/usr/bin/env python3
"""
反向交叉校验：由 ezdxf 写出 DXF，交给 LyCAD 读取。

与 verify_dxf_with_ezdxf.py 方向相反——
前者验证「LyCAD 写出的文件能被独立实现读懂」，
本脚本验证「独立实现写出的文件能被 LyCAD 读懂」。

刻意覆盖 LyCAD 内部并非原生建模、需要做语义转换的内容：
  · 块定义 + INSERT 引用（LyCAD 在导入时按插入点/比例/旋转展开）
  · 嵌套 INSERT（块中引用块）
  · 旧式 POLYLINE/VERTEX（非 LWPOLYLINE）
  · 文字 / 填充 / 尺寸标注（已建模，验证内容与测量值正确还原）
  · MTEXT：不建模但会**降级**为单行文字导入（计入 TEXT，警告中如实告知）
  · 非 ASCII 图层名与文字
"""
import sys
import re
import math
import subprocess
from pathlib import Path

import ezdxf

FAILURES = []
CHECKS = 0

LYCAD_SAMPLE = None
WORK = None


def check(condition, message):
    global CHECKS
    CHECKS += 1
    if not condition:
        FAILURES.append(message)
        print(f"  [失败] {message}")
    return condition


def close(a, b, tol=1e-6):
    return abs(a - b) <= tol


def build_reference_dxf(path: Path):
    """构造一份刻意「刁难」读取器的 DXF"""
    doc = ezdxf.new("R2000", setup=True)
    doc.layers.add("轮廓", color=1)
    doc.layers.add("中文图层", color=5, linetype="DASHED")
    msp = doc.modelspace()

    # 基本图元
    msp.add_line((0, 0), (100, 0), dxfattribs={"layer": "轮廓"})
    msp.add_circle((50, 50), 20, dxfattribs={"layer": "轮廓", "color": 3})
    msp.add_arc((50, 50), 30, 45, 200, dxfattribs={"layer": "轮廓"})
    msp.add_point((10, 10), dxfattribs={"layer": "中文图层"})

    # LWPOLYLINE，两个顶点带凸度
    pline = msp.add_lwpolyline(
        [(0, 80, 0.0, 0.0, 0.5), (40, 80, 0.0, 0.0, -0.25), (40, 100, 0.0, 0.0, 0.0)],
        format="xyseb",
        dxfattribs={"layer": "中文图层"},
    )
    pline.closed = True

    # 旧式 POLYLINE / VERTEX（R12 风格），验证兼容路径
    # POLYLINE 的闭合由 70 组码 bit 0 表示。
    # 注意 ezdxf 的 Polyline2d 不接受 flags 属性直传，
    # 也不响应 .closed 赋值，必须调用 close(True) 才会写回该标志。
    old_poly = msp.add_polyline2d(
        [(200, 0), (240, 0), (240, 30)],
        dxfattribs={"layer": "轮廓"},
    )
    old_poly.close(True)

    # 块定义 + INSERT，含缩放与旋转
    block = doc.blocks.new("螺孔")
    block.add_circle((0, 0), 3)
    block.add_line((-4, 0), (4, 0))

    msp.add_blockref("螺孔", (10, 40), dxfattribs={"xscale": 2.0, "yscale": 2.0})
    msp.add_blockref("螺孔", (30, 40), dxfattribs={"rotation": 30.0, "layer": "轮廓"})

    # 嵌套块：外层块引用内层块
    outer = doc.blocks.new("螺孔组")
    outer.add_blockref("螺孔", (0, 0))
    outer.add_blockref("螺孔", (20, 0))
    msp.add_blockref("螺孔组", (300, 0), dxfattribs={"xscale": 0.5, "yscale": 0.5})

    # 文字：已建模，验证内容与属性还原
    msp.add_text("测试文字", dxfattribs={"height": 5, "layer": "中文图层"}).set_placement((0, 200))

    # 填充：已建模（单环多段线边界 + 图案）
    hatch = msp.add_hatch(color=2)
    hatch.set_pattern_fill("ANSI31", scale=2.0)
    hatch.paths.add_polyline_path([(0, 250), (50, 250), (50, 300), (0, 300)])

    # 尺寸标注：对齐 + 半径（render() 生成几何块，LyCAD 读取时忽略该块）
    dim1 = msp.add_aligned_dim(p1=(200, 0), p2=(300, 0), distance=-25,
                               dxfattribs={"layer": "轮廓"})
    dim1.render()
    angle = math.radians(60.0)
    dim2 = msp.add_radius_dim(center=(50, 50),
                              mpoint=(50 + 20 * math.cos(angle),
                                      50 + 20 * math.sin(angle)),
                              dxfattribs={"layer": "轮廓"})
    dim2.render()

    # 仍未建模的实体：应被跳过并报数，而不是让整次导入失败
    msp.add_mtext("仍未建模的多行文字", dxfattribs={"layer": "中文图层"})

    doc.saveas(str(path))
    return doc


def run_inspect(path: Path):
    proc = subprocess.run(
        [str(LYCAD_SAMPLE), "--inspect", str(path)],
        capture_output=True, text=True, timeout=120,
    )
    return proc.stdout + proc.stderr


def parse_summary(output: str):
    summary = {"entities": [], "layers": {}, "warnings": [], "ok": "结果: 成功" in output}
    for line in output.splitlines():
        line = line.strip()
        if line.startswith("raw|"):
            fields = dict(re.findall(r"(\w+)=([^|]+)", line))
            summary["entities"].append(fields)
        elif line.startswith("- ") and "color=" in line:
            m = re.match(r"- (\S+)\s+color=(-?\d+)\s+linetype=(\S+)\s+visible=(\d+)", line)
            if m:
                summary["layers"][m.group(1)] = {
                    "color": int(m.group(2)),
                    "linetype": m.group(3),
                    "visible": int(m.group(4)),
                }
        elif line.startswith("· "):
            summary["warnings"].append(line[2:])
    return summary


def main():
    global LYCAD_SAMPLE, WORK

    if len(sys.argv) < 3:
        print("用法: verify_lycad_reads_ezdxf.py <lycad_sample_generator 可执行文件> <工作目录>")
        return 2

    LYCAD_SAMPLE = sys.argv[1]
    WORK = Path(sys.argv[2])
    WORK.mkdir(parents=True, exist_ok=True)

    dxf_path = WORK / "from_ezdxf.dxf"
    reference = build_reference_dxf(dxf_path)
    print(f"已由 ezdxf {ezdxf.__version__} 生成 {dxf_path.name}")

    output = run_inspect(dxf_path)
    summary = parse_summary(output)

    print("\n=== LyCAD 读取结果 ===")
    for line in output.splitlines():
        if line.strip().startswith("raw|"):
            print("  " + line.strip())

    print("\n=== 校验 ===")
    check(summary["ok"], "LyCAD 未能成功读取 ezdxf 生成的文件")

    entities = summary["entities"]
    by_type = {}
    for entity in entities:
        by_type.setdefault(entity["type"], []).append(entity)

    print(f"  图元统计: { {k: len(v) for k, v in by_type.items()} }")

    # ---- 从参考文档推导期望值，避免写死数字 ----
    #
    # LyCAD 在导入时把 INSERT 展开为独立图元（块内 0 层图元继承 INSERT 图层），
    # 未建模的实体（MTEXT 等）被跳过。期望值按同一规则从源文档算出，
    # 再与 LyCAD 的读取结果逐项比对。
    SUPPORTED = {"LINE", "CIRCLE", "ARC", "POINT", "LWPOLYLINE", "POLYLINE",
                 "TEXT", "HATCH", "DIMENSION"}
    # LyCAD 内部把 LWPOLYLINE 与旧式 POLYLINE 统一表示为多段线
    NORMALIZE = {"POLYLINE": "LWPOLYLINE"}
    # 不建模、但导入时会降级为等价图元的类型（降级后计入目标类型，
    # 且警告中仍必须如实告知降级发生）
    DEGRADE = {"MTEXT": "TEXT"}

    expected_types = {}
    expected_layers = {}
    unsupported_types = set()

    def account(dxftype, layer):
        if dxftype in DEGRADE:
            key = DEGRADE[dxftype]
            expected_types[key] = expected_types.get(key, 0) + 1
            expected_layers[layer] = expected_layers.get(layer, 0) + 1
            unsupported_types.add(dxftype)
        elif dxftype in SUPPORTED:
            key = NORMALIZE.get(dxftype, dxftype)
            expected_types[key] = expected_types.get(key, 0) + 1
            expected_layers[layer] = expected_layers.get(layer, 0) + 1
        else:
            unsupported_types.add(dxftype)

    def walk(entities_iter, inherited_layer):
        for ent in entities_iter:
            dxftype = ent.dxftype()
            layer = ent.dxf.layer
            # 块内位于 0 层的图元继承 INSERT 的图层
            if inherited_layer and layer == "0":
                layer = inherited_layer

            if dxftype == "INSERT":
                block = reference.blocks.get(ent.dxf.name)
                if block is None:
                    continue
                walk(block, layer)
            elif dxftype in ("ATTDEF", "ATTRIB", "SEQEND", "VERTEX"):
                continue
            else:
                account(dxftype, layer)

    walk(reference.modelspace(), None)

    print(f"  期望（按源文档推导）: 类型={expected_types}")
    print(f"                        图层={expected_layers}")
    print(f"  未建模实体: {sorted(unsupported_types)}")

    for dxftype, count in expected_types.items():
        actual = len(by_type.get(dxftype, []))
        check(actual == count, f"{dxftype} 数量应为 {count}，实际 {actual}")

    actual_layers = {}
    for entity in entities:
        actual_layers[entity["layer"]] = actual_layers.get(entity["layer"], 0) + 1
    for layer, count in expected_layers.items():
        actual = actual_layers.get(layer, 0)
        check(actual == count, f"图层「{layer}」上应有 {count} 个图元，实际 {actual}")

    # 未支持实体必须被报数，不能静默消失
    joined_warnings = " ".join(summary["warnings"])
    for dxftype in unsupported_types:
        check(dxftype in joined_warnings,
              f"未报告跳过 {dxftype}：{summary['warnings']}")
    check("INSERT" in joined_warnings or "块引用" in joined_warnings,
          f"未报告块引用展开：{summary['warnings']}")

    # 中文图层名必须正确解码
    check("轮廓" in summary["layers"], f"中文图层「轮廓」丢失: {list(summary['layers'])}")
    check("中文图层" in summary["layers"], f"中文图层「中文图层」丢失: {list(summary['layers'])}")
    check("0" in summary["layers"], "缺少系统图层 0")

    # 图层属性
    if "轮廓" in summary["layers"]:
        check(summary["layers"]["轮廓"]["color"] == 1,
              f"轮廓层颜色应为 1，实际 {summary['layers']['轮廓']['color']}")
    if "中文图层" in summary["layers"]:
        check(summary["layers"]["中文图层"]["linetype"].upper() == "DASHED",
              f"中文图层线型应为 DASHED，实际 {summary['layers']['中文图层']['linetype']}")

    # 显式颜色索引必须保留
    colored = [e for e in by_type.get("CIRCLE", []) if e.get("color") == "3"]
    check(len(colored) == 1, f"显式颜色索引 3 应有 1 个圆，实际 {len(colored)}")

    # 块展开的几何正确性：半径 20 的顶层圆与按比例缩放后的块内圆
    lengths = sorted(round(float(e["len"]), 4) for e in by_type.get("CIRCLE", []))
    expected_lengths = sorted(round(2 * math.pi * r, 4) for r in (20.0, 6.0, 3.0, 1.5, 1.5))
    check(all(any(close(a, b, 1e-3) for b in lengths) for a in expected_lengths),
          f"圆的周长集合不符: {lengths}，期望约 {expected_lengths}")

    # 闭合的旧式 POLYLINE 周长必须包含收尾段：40 + 30 + 50 = 120
    poly_lengths = sorted(round(float(e["len"]), 4) for e in by_type.get("LWPOLYLINE", []))
    check(any(close(v, 120.0, 1e-3) for v in poly_lengths),
          f"闭合 POLYLINE 周长应为 120（含收尾段），实际 {poly_lengths}")

    # ---- 文字：内容必须被正确解码 ----
    text_contents = [e.get("text", "") for e in by_type.get("TEXT", [])]
    check("测试文字" in text_contents, f"文字内容「测试文字」未被正确还原: {text_contents}")

    # ---- 填充：作为独立实体导入 ----
    check(len(by_type.get("HATCH", [])) == 1,
          f"应导入 1 个填充实体，实际 {len(by_type.get('HATCH', []))}")

    # ---- 尺寸标注：测量值必须与源几何一致（显示文本含 R/⌀ 前缀，提取数值） ----
    def numeric(text):
        digits = re.sub(r"[^0-9.]", "", text)
        return float(digits) if digits else 0.0

    dim_values = sorted(round(numeric(e.get("dim", "")), 4)
                        for e in by_type.get("DIMENSION", []))
    check(any(close(v, 100.0, 1e-3) for v in dim_values),
          f"对齐标注测量值应为 100，实际 {dim_values}")
    check(any(close(v, 20.0, 1e-3) for v in dim_values),
          f"半径标注测量值应为 20，实际 {dim_values}")

    print(f"\n{'=' * 60}")
    if FAILURES:
        print(f"结论：{len(FAILURES)} 项失败 / 共 {CHECKS} 项检查")
        for failure in FAILURES:
            print(f"  · {failure}")
        return 1
    print(f"结论：全部 {CHECKS} 项检查通过——LyCAD 能正确读取 ezdxf 写出的文件")
    return 0


if __name__ == "__main__":
    sys.exit(main())
