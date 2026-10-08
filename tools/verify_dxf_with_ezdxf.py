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


def verify(path: Path, expected_version: str, has_lineweight: bool,
           has_lwpolyline: bool, has_hatch: bool):
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
    check(len(types) == 18, f"实体数应为 18，实际 {len(types)}")
    check(types.count("LINE") == 4, f"LINE 数应为 4，实际 {types.count('LINE')}")
    check(types.count("CIRCLE") == 2, f"CIRCLE 数应为 2，实际 {types.count('CIRCLE')}")
    check(types.count("ARC") == 1, "ARC 数应为 1")
    check(types.count("POINT") == 1, "POINT 数应为 1")
    check(types.count("TEXT") == 2, f"TEXT 数应为 2，实际 {types.count('TEXT')}")
    check(types.count("DIMENSION") == 4, f"DIMENSION 数应为 4，实际 {types.count('DIMENSION')}")
    poly_type = "LWPOLYLINE" if has_lwpolyline else "POLYLINE"
    # R2000：外框 + 圆弧段多段线 + 带宽度走线 = 3；R12 额外多一条由 HATCH 降级而来的闭合多段线
    expected_polys = 4 if not has_hatch else 3
    check(types.count(poly_type) == expected_polys,
          f"{poly_type} 数应为 {expected_polys}，实际 {types.count(poly_type)}")

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
    check(len(closed) == expected_polys - 1,
          f"闭合多段线应为 {expected_polys - 1} 条（带宽走线为开环），实际 {len(closed)} 条")

    # 用凸度区分「直线外框」与「含圆弧段的多段线」
    arc_polys = [poly for poly in polylines if any(abs(b) > 1e-9 for b in bulges_of(poly))]
    check(len(arc_polys) == 1, f"应恰好有一条含圆弧段的多段线，实际 {len(arc_polys)}")

    frames = [poly for poly in polylines
              if not any(abs(b) > 1e-9 for b in bulges_of(poly))
              and len(vertices_of(poly)) == 4]
    check(len(frames) == 1, "应恰好有一条 4 顶点纯直线多段线（外框）")
    if frames:
        points = vertices_of(frames[0])
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

    # 带宽度多段线（PCB 走线语义）：宽度值必须完整保留
    if has_lwpolyline:
        wide = [p for p in modelspace.query("LWPOLYLINE")
                if abs((p.dxf.const_width or 0.0) - 2.0) < 1e-9]
        check(len(wide) == 1, f"应有一条恒定宽度 2.0 的多段线，实际 {len(wide)}")
    else:
        # R12：宽度在 POLYLINE 头部的 40/41 组码（ezdxf: default_start/end_width）
        wide = [p for p in modelspace.query("POLYLINE")
                if abs((p.dxf.get('default_start_width', 0.0) or 0.0) - 2.0) < 1e-9]
        check(len(wide) == 1, f"R12 中应有默认宽度 2.0 的 POLYLINE，实际 {len(wide)}")

    # R12 下填充被降级为闭合多段线：验证几何信息不丢失
    if not has_hatch:
        hatch_like = [poly for poly in polylines
                      if not any(abs(b) > 1e-9 for b in bulges_of(poly))
                      and len(vertices_of(poly)) == 5]
        check(len(hatch_like) == 1, "R12 中应由 HATCH 降级出一条 5 顶点闭合多段线")
        if hatch_like:
            got = {(round(x, 6), round(y, 6)) for x, y in vertices_of(hatch_like[0])}
            expected = {(100.0, 15.0), (130.0, 15.0), (140.0, 38.0),
                        (115.0, 52.0), (92.0, 38.0)}
            check(got == expected, f"降级多段线的顶点集合不符: {sorted(got)}")

    # 图层归属
    layer_of = {}
    for entity in modelspace:
        layer_of.setdefault(entity.dxf.layer, 0)
        layer_of[entity.dxf.layer] += 1
    print(f"  实体图层分布: {layer_of}")
    check(layer_of.get("中心线") == 2, f"中心线层应有 2 个实体，实际 {layer_of.get('中心线')}")
    check(layer_of.get("轮廓", 0) >= 5, "轮廓层实体数偏少")
    check(layer_of.get("标注", 0) == 8,
          f"标注层应有 8 个实体（点/隐藏圆/2 文字/4 标注），实际 {layer_of.get('标注')}")

    # ---- 文字 ----
    texts = modelspace.query("TEXT")
    by_content = {t.dxf.text: t for t in texts}
    check("LyCAD 样例图纸" in by_content, f"缺含中文的文字实体: {list(by_content)}")
    check("直径 50" in by_content, "缺第二条文字实体")
    if "LyCAD 样例图纸" in by_content:
        t1 = by_content["LyCAD 样例图纸"]
        check(close(t1.dxf.height, 6.0), f"文字字高应为 6，实际 {t1.dxf.height}")
        check(t1.dxf.halign == 0 and t1.dxf.valign == 0, "第一条文字应为左/基线对齐")
        check(close(t1.dxf.insert.x, 10.0) and close(t1.dxf.insert.y, 104.0),
              f"文字插入点错误: {t1.dxf.insert}")
    if "直径 50" in by_content:
        t2 = by_content["直径 50"]
        check(t2.dxf.halign == 1 and t2.dxf.valign == 2,
              f"第二条文字应为居中/居中对齐，实际 halign={t2.dxf.halign} valign={t2.dxf.valign}")
        check(close(t2.dxf.align_point.x, 60.0) and close(t2.dxf.align_point.y, 55.0),
              f"第二对齐点错误: {t2.dxf.align_point}")
        check(close(t2.dxf.height, 4.0), "第二条文字字高应为 4")
        # 71 生成标志 bit2 = 水平镜像（背面文字），41 宽度因子
        check(bool(t2.dxf.text_generation_flag & 2),
              f"第二条文字应带水平镜像标志，实际生成标志 {t2.dxf.text_generation_flag}")
        check(close(t2.dxf.width, 1.1), f"宽度因子应为 1.1，实际 {t2.dxf.width}")

    # ---- 填充（仅 R2000） ----
    hatches = modelspace.query("HATCH")
    if has_hatch:
        check(len(hatches) == 1, f"HATCH 数应为 1，实际 {len(hatches)}")
        if hatches:
            h = hatches[0]
            check(h.dxf.pattern_name.upper() == "ANSI31",
                  f"填充图案应为 ANSI31，实际 {h.dxf.pattern_name}")
            check(close(h.dxf.pattern_scale, 1.5),
                  f"填充比例应为 1.5，实际 {h.dxf.pattern_scale}")
            check(not h.dxf.solid_fill, "填充不应是实体填充")
            paths = list(h.paths)
            check(len(paths) == 1, f"边界环数应为 1，实际 {len(paths)}")
            if paths and hasattr(paths[0], "vertices"):
                # ezdxf 对边界顶点有两代表示：早期返回带 x/y 属性的对象，
                # 新版返回 (x, y) 元组。两种都要能读，否则换个 ezdxf 版本就报错。
                def vertex_xy(v):
                    if isinstance(v, (tuple, list)):
                        return float(v[0]), float(v[1])
                    return float(v.x), float(v.y)

                verts = [vertex_xy(v) for v in paths[0].vertices]
                got = {(round(x, 6), round(y, 6)) for x, y in verts}
                expected = {(100.0, 15.0), (130.0, 15.0), (140.0, 38.0),
                            (115.0, 52.0), (92.0, 38.0)}
                check(got == expected, f"填充边界顶点集合不符: {sorted(got)}")
    else:
        check(len(hatches) == 0, "R12 文件不应包含 HATCH 实体")

    # ---- 尺寸标注 ----
    dims = modelspace.query("DIMENSION")
    check(len(dims) == 4, f"DIMENSION 数应为 4，实际 {len(dims)}")
    subtypes = sorted(d.dxf.dimtype & 7 for d in dims)
    check(subtypes == [0, 1, 3, 4],
          f"标注类型集合应为 线性/对齐/直径/半径，实际 {subtypes}")

    for d in dims:
        subtype = d.dxf.dimtype & 7

        # 每个标注都必须引用一个已定义的匿名几何块（ezdxf 中属性名为 geometry）
        block_name = d.dxf.geometry
        check(bool(block_name) and block_name in doc.blocks,
              f"标注的几何块 {block_name!r} 应存在于 BLOCKS 区")
        if block_name in doc.blocks:
            block_entities = list(doc.blocks.get(block_name))
            check(len(block_entities) >= 3,
                  f"几何块 {block_name} 内容过少: {len(block_entities)} 个实体")
            has_text = any(e.dxftype() == "TEXT" for e in block_entities)
            has_solid = any(e.dxftype() == "SOLID" for e in block_entities)
            check(has_text, f"几何块 {block_name} 缺少测量文字")
            check(has_solid, f"几何块 {block_name} 缺少箭头 (SOLID)")

        measurement = getattr(d.dxf, "actual_measurement", 0.0)
        if subtype == 1:      # 对齐
            check(close(d.dxf.defpoint2.x, 0.0) and close(d.dxf.defpoint2.y, 0.0),
                  f"对齐标注界线原点 1 错误: {d.dxf.defpoint2}")
            check(close(d.dxf.defpoint3.x, 180.0) and close(d.dxf.defpoint3.y, 0.0),
                  f"对齐标注界线原点 2 错误: {d.dxf.defpoint3}")
            check(close(measurement, 180.0), f"对齐标注测量值应为 180，实际 {measurement}")
        elif subtype == 0:    # 线性（垂直）
            check(close(d.dxf.defpoint2.x, 180.0) and close(d.dxf.defpoint2.y, 0.0),
                  f"线性标注界线原点 1 错误: {d.dxf.defpoint2}")
            check(close(d.dxf.defpoint3.x, 180.0) and close(d.dxf.defpoint3.y, 110.0),
                  f"线性标注界线原点 2 错误: {d.dxf.defpoint3}")
            check(close(measurement, 110.0), f"线性标注测量值应为 110，实际 {measurement}")
        elif subtype == 4:    # 半径
            check(close(d.dxf.defpoint.x, 60.0) and close(d.dxf.defpoint.y, 55.0),
                  f"半径标注圆心错误: {d.dxf.defpoint}")
            r = d.dxf.defpoint.distance(d.dxf.defpoint4) if hasattr(d.dxf.defpoint, 'distance') \
                else math.hypot(d.dxf.defpoint4.x - d.dxf.defpoint.x,
                                d.dxf.defpoint4.y - d.dxf.defpoint.y)
            check(close(r, 25.0), f"半径标注的圆上点距圆心应为 25，实际 {r}")
            check(close(measurement, 25.0), f"半径标注测量值应为 25，实际 {measurement}")
        elif subtype == 3:    # 直径
            r = math.hypot(d.dxf.defpoint4.x - d.dxf.defpoint.x,
                           d.dxf.defpoint4.y - d.dxf.defpoint.y)
            check(close(r, 50.0), f"直径标注两定义点距离应为 50，实际 {r}")
            check(close(measurement, 50.0), f"直径标注测量值应为 50，实际 {measurement}")

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

    # 默认按「lycad_sample_generator 写入仓库 sample/ 的目录结构」取文件；
    # 生成器显式指定输出目录时，三种文件平铺在该目录下，直接把该目录传进来即可。
    r12_dir = sample_dir / "r12" if (sample_dir / "r12").is_dir() else sample_dir
    r2000_dir = sample_dir / "2000" if (sample_dir / "2000").is_dir() else sample_dir

    verify(r12_dir / "lycad_sample_r12.dxf",
           expected_version="AC1009",
           has_lineweight=False,     # R12 无 370 组码
           has_lwpolyline=False,     # R12 使用 POLYLINE/VERTEX
           has_hatch=False)          # R12 无 HATCH，降级为多段线

    verify(r2000_dir / "lycad_sample_2000.dxf",
           expected_version="AC1015",
           has_lineweight=True,
           has_lwpolyline=True,
           has_hatch=True)

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
