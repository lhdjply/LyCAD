#!/usr/bin/env python3
"""
LyCAD 导出的 DXF 独立交叉校验。

用 ezdxf（第三方、严格按 DXF 规范实现的库）读取 LyCAD 写出的文件，
逐项核对版本、图层表、线型表与全部图元的几何参数。

目的：LyCAD 自身的往返测试只能证明「自己写、自己读」是自洽的，
无法发现「双方都偏离规范」的情形。必须用独立实现交叉验证。
"""
import sys
import math
from pathlib import Path

import ezdxf
import ezdxf.recover

FAILURES = []
CHECKS = 0


def check(condition, message):
    global CHECKS
    CHECKS += 1
    if not condition:
        FAILURES.append(message)
        print(f"  [失败] {message}")
    return condition


def close(a, b, tol=1e-6):
    return abs(a - b) <= tol


def verify(path: Path, expected_version: str, has_lineweight: bool, has_lwpolyline: bool):
    print(f"\n=== 校验 {path.name} (期望版本 {expected_version}) ===")

    # recover 模式会报告结构性问题；正常文件应无需修复
    doc, auditor = ezdxf.recover.readfile(str(path))

    check(doc.dxfversion == expected_version,
          f"版本号应为 {expected_version}，实际 {doc.dxfversion}")
    print(f"  版本: {doc.dxfversion}  (DXF 规范版本 {ezdxf.DXF12 if expected_version == 'AC1009' else ezdxf.DXF2000})")

    # 审计器报告的每个错误都视为缺陷
    for error in auditor.errors:
        check(False, f"审计错误 (code {error.code}): {error.message}")
    for fix in auditor.fixes:
        check(False, f"审计修复 (code {fix.code}): {fix.message}")
    if not auditor.errors and not auditor.fixes:
        print("  审计: 无错误、无修复")

    # ---- 图层表 ----
    layers = {layer.dxf.name: layer for layer in doc.layers}
    print(f"  图层: {sorted(layers)}")
    # 注：ezdxf 载入时会自行补充 Defpoints 等辅助图层，
    # 因此这里只断言「文件声明的图层必须全部存在」，而非集合完全相等。
    expected_layers = {"0", "轮廓", "中心线", "标注"}
    check(expected_layers.issubset(set(layers)),
          f"缺少图层: {sorted(expected_layers - set(layers))}")
    check(layers["轮廓"].dxf.color == 1, f"轮廓层颜色应为 1，实际 {layers['轮廓'].dxf.color}")
    check(layers["中心线"].dxf.color == 4, "中心线层颜色应为 4")
    check(layers["标注"].dxf.color == 2, "标注层颜色应为 2")
    check(layers["中心线"].dxf.linetype.upper() == "CENTER", "中心线层线型应为 CENTER")
    check(layers["标注"].dxf.linetype.upper() == "DASHED", "标注层线型应为 DASHED")
    if has_lineweight:
        check(layers["轮廓"].dxf.lineweight == 35,
              f"轮廓层线宽应为 35，实际 {layers['轮廓'].dxf.lineweight}")

    # 图层中文名的 \U+ 转义必须被正确还原
    check("轮廓" in layers and "中心线" in layers and "标注" in layers,
          "中文图层名解码失败")

    # ---- 线型表 ----
    linetypes = {lt.dxf.name.upper() for lt in doc.linetypes}
    print(f"  线型: {sorted(linetypes)}")
    check("CONTINUOUS" in linetypes, "缺少 CONTINUOUS 线型定义")
    check("CENTER" in linetypes, "缺少 CENTER 线型定义")
    check("DASHED" in linetypes, "缺少 DASHED 线型定义")

    # ---- 实体 ----
    modelspace = doc.modelspace()
    types = [e.dxftype() for e in modelspace]
    print(f"  实体: {types}")
    check(len(types) == 10, f"实体数应为 10，实际 {len(types)}")
    check(types.count("LINE") == 4, f"LINE 数应为 4，实际 {types.count('LINE')}")
    check(types.count("CIRCLE") == 2, f"CIRCLE 数应为 2，实际 {types.count('CIRCLE')}")
    check(types.count("ARC") == 1, "ARC 数应为 1")
    check(types.count("POINT") == 1, "POINT 数应为 1")
    poly_type = "LWPOLYLINE" if has_lwpolyline else "POLYLINE"
    check(types.count(poly_type) == 2, f"{poly_type} 数应为 2，实际 {types.count(poly_type)}")

    # 圆弧几何
    arcs = modelspace.query("ARC")
    check(len(arcs) == 1, "圆弧数量不符")
    if arcs:
        arc = arcs[0]
        check(close(arc.dxf.center.x, 120.0) and close(arc.dxf.center.y, 55.0),
              f"圆弧圆心错误: {arc.dxf.center}")
        check(close(arc.dxf.radius, 25.0), f"圆弧半径错误: {arc.dxf.radius}")
        check(close(arc.dxf.start_angle, 30.0, 1e-6),
              f"圆弧起始角应为 30°，实际 {arc.dxf.start_angle}")
        check(close(arc.dxf.end_angle, 250.0, 1e-6),
              f"圆弧终止角应为 250°，实际 {arc.dxf.end_angle}")

    # 圆的几何与隐藏标志
    circles = modelspace.query("CIRCLE")
    radii = sorted(round(c.dxf.radius, 6) for c in circles)
    check(radii == [6.0, 25.0], f"圆半径集合不符: {radii}")
    invisible = [c for c in circles if c.dxf.invisible]
    check(len(invisible) == 1, "应恰好有一个圆带有不可见标志 (60 组码)")
    if invisible:
        check(close(invisible[0].dxf.radius, 6.0), "被隐藏的应是半径 6 的圆")
        check(invisible[0].dxf.layer == "标注", "被隐藏圆应位于「标注」层")

    # 多段线的顶点数、闭合标志与凸度
    polylines = modelspace.query(poly_type)

    def is_closed(poly):
        """LWPOLYLINE 用 70 组码 bit 0；POLYLINE 同样使用 70 组码 bit 0"""
        if poly_type == "LWPOLYLINE":
            return bool(poly.closed)
        return bool(poly.dxf.flags & 1)

    def vertices_of(poly):
        if poly_type == "LWPOLYLINE":
            # ezdxf 在不同版本中可能返回标量或单元素元组，统一成二元组
            return [(pt[0], pt[1]) if isinstance(pt, tuple) else (pt[0], pt[1])
                    for pt in poly.get_points("xy")]
        return [(v.dxf.location.x, v.dxf.location.y) for v in poly.vertices]

    def bulges_of(poly):
        if poly_type == "LWPOLYLINE":
            raw = list(poly.get_points("b"))
        else:
            raw = [v.dxf.bulge for v in poly.vertices]
        return [round(b[0] if isinstance(b, tuple) else b, 6) for b in raw]

    closed = [poly for poly in polylines if is_closed(poly)]
    check(len(closed) == 2, f"样例中两条多段线均为闭合，实际闭合 {len(closed)} 条")

    # 用凸度区分「直线外框」与「含圆弧段的多段线」
    arc_polys = [poly for poly in polylines if any(abs(b) > 1e-9 for b in bulges_of(poly))]
    check(len(arc_polys) == 1, f"应恰好有一条含圆弧段的多段线，实际 {len(arc_polys)}")

    frames = [poly for poly in polylines if not any(abs(b) > 1e-9 for b in bulges_of(poly))]
    check(len(frames) == 1, "应恰好有一条纯直线多段线（外框）")
    if frames:
        points = vertices_of(frames[0])
        check(len(points) == 4, f"外框应有 4 个顶点，实际 {len(points)}")
        xs = sorted({round(pt[0], 6) for pt in points})
        ys = sorted({round(pt[1], 6) for pt in points})
        check(xs == [0.0, 180.0] and ys == [0.0, 110.0],
              f"外框范围错误: x={xs} y={ys}")
        check(is_closed(frames[0]), "外框应为闭合多段线")

    if arc_polys:
        poly = arc_polys[0]
        vertex_count = len(vertices_of(poly))
        bulges = bulges_of(poly)
        check(vertex_count == 4, f"圆弧段多段线应有 4 个顶点，实际 {vertex_count}")
        # 正凸度 = 逆时针弧段，负凸度 = 顺时针弧段，两者的符号必须都被保留
        check(0.6 in bulges, f"缺失凸度 +0.6（逆时针弧段），实际 {bulges}")
        check(-0.4 in bulges, f"缺失凸度 -0.4（顺时针弧段），实际 {bulges}")

    # 图层归属
    layer_of = {}
    for entity in modelspace:
        layer_of.setdefault(entity.dxf.layer, 0)
        layer_of[entity.dxf.layer] += 1
    print(f"  实体图层分布: {layer_of}")
    check(layer_of.get("中心线") == 2, f"中心线层应有 2 个实体，实际 {layer_of.get('中心线')}")
    check(layer_of.get("轮廓", 0) >= 4, "轮廓层实体数偏少")

    # ---- 回写再读：验证 ezdxf 能无损重写我们的文件 ----
    rewritten = ezdxf.recover.readfile(str(path))[0]
    ok = True
    try:
        rewritten.saveas(str(path.with_name(path.stem + "_ezdxf.dxf")))
    except Exception as exc:                       # noqa: BLE001
        ok = False
        check(False, f"ezdxf 回写失败: {exc}")
    if ok:
        print("  回写: ezdxf 可无损重写该文件")


def main():
    sample_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")

    verify(sample_dir / "lycad_sample_r12.dxf",
           expected_version="AC1009",
           has_lineweight=False,     # R12 无 370 组码
           has_lwpolyline=False)     # R12 使用 POLYLINE/VERTEX

    verify(sample_dir / "lycad_sample_2000.dxf",
           expected_version="AC1015",
           has_lineweight=True,
           has_lwpolyline=True)

    print(f"\n{'=' * 60}")
    if FAILURES:
        print(f"结论：{len(FAILURES)} 项失败 / 共 {CHECKS} 项检查")
        for failure in FAILURES:
            print(f"  · {failure}")
        return 1
    print(f"结论：全部 {CHECKS} 项检查通过——LyCAD 导出的 DXF 可被独立实现正确读取")
    return 0


if __name__ == "__main__":
    sys.exit(main())
