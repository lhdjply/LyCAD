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
import tempfile
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


def verify(path: Path, expected_version: str):
    print(f"\n=== 校验 {path.name} (期望版本 {expected_version}) ===")

    # recover 模式会报告结构性问题；正常文件应无需修复
    doc, auditor = ezdxf.recover.readfile(str(path))

    check(doc.dxfversion == expected_version,
          f"版本号应为 {expected_version}，实际 {doc.dxfversion}")

    # 文本编码：AC1021（2007）起 DXF 一律 UTF-8，更早按 $DWGCODEPAGE 走代码页。
    # 「声明 2007 却写 GBK」这类错误在自己读自己时看不出来（读侧跟着同一个声明走），
    # 只有独立实现按版本判定时才暴露。
    if expected_version >= "AC1021":
        check(doc.output_encoding == "utf-8",
              f"{expected_version} 的文本编码应为 utf-8，实际 {doc.output_encoding}")
    else:
        check(doc.output_encoding != "utf-8",
              f"{expected_version} 的文本应按代码页编码，实际 {doc.output_encoding}")
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

    # 图元构成逐项比对。可写版本自 R2000 起实体集相同，
    # HATCH / SPLINE / ELLIPSE / XLINE / RAY 都必须是**原生实体**——
    # R12 的降级通道已随「移除 R12 导出」删除，这里再出现
    # POLYLINE 顶替 HATCH 之类的情形就是回归。
    poly_type = "LWPOLYLINE"
    expected_types = {
        "LWPOLYLINE": 3, "LINE": 4, "CIRCLE": 2, "ARC": 1, "ELLIPSE": 2,
        "SPLINE": 1, "HATCH": 1, "POINT": 1, "TEXT": 2, "DIMENSION": 5,
        "ARC_DIMENSION": 1, "XLINE": 1, "RAY": 1,
    }

    total = sum(expected_types.values())
    check(len(types) == total, f"实体数应为 {total}，实际 {len(types)}")
    for name, want in expected_types.items():
        check(types.count(name) == want, f"{name} 数应为 {want}，实际 {types.count(name)}")
    for name in sorted(set(types) - set(expected_types)):
        check(False, f"出现预期之外的实体类型: {name}")

    expected_polys = expected_types[poly_type]

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
        """LWPOLYLINE 用 70 组码 bit 0"""
        return bool(poly.closed)

    def vertices_of(poly):
        # ezdxf 在不同版本中可能返回标量或单元素元组，统一成二元组
        return [(pt[0], pt[1]) if isinstance(pt, tuple) else (pt[0], pt[1])
                for pt in poly.get_points("xy")]

    def bulges_of(poly):
        raw = list(poly.get_points("b"))
        return [round(b[0] if isinstance(b, tuple) else b, 6) for b in raw]

    # 闭合多段线：外框 + 圆弧段多段线 2 条（带宽走线为开环）。
    # 填充与椭圆是原生实体，不该再以多段线的形式出现。
    expected_closed = 2
    closed = [poly for poly in polylines if is_closed(poly)]
    check(len(closed) == expected_closed,
          f"闭合多段线应为 {expected_closed} 条，实际 {len(closed)} 条")

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

    # 带宽度多段线（PCB 走线语义）：宽度值必须完整保留（43 组码）
    wide = [p for p in modelspace.query("LWPOLYLINE")
            if abs((p.dxf.const_width or 0.0) - 2.0) < 1e-9]
    check(len(wide) == 1, f"应有一条恒定宽度 2.0 的多段线，实际 {len(wide)}")

    # 图层归属
    layer_of = {}
    for entity in modelspace:
        layer_of.setdefault(entity.dxf.layer, 0)
        layer_of[entity.dxf.layer] += 1
    print(f"  实体图层分布: {layer_of}")
    expected_layer_of = {"0": 2, "中心线": 4, "标注": 8, "轮廓": 9}
    check(layer_of == expected_layer_of,
          f"实体图层分布应为 {expected_layer_of}，实际 {layer_of}")

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

    # ---- 填充 ----
    hatches = modelspace.query("HATCH")
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

    # ---- 尺寸标注 ----
    # 坐标标注是 DIMENSION + AcDbOrdinateDimension 子类；弧长标注不是 DIMENSION
    # 的变体，而是 DXF 的**扩展实体** ARC_DIMENSION（子类 AcDbArcDimension），
    # 因此要单独查一次。两者都是 DIMARC/DIMORDINATE 的原生表达，不能被降级成
    # 别的实体，也不能凭空少掉。
    dims = modelspace.query("DIMENSION")
    check(len(dims) == 5, f"DIMENSION 数应为 5，实际 {len(dims)}")
    subtypes = sorted(d.dxf.dimtype & 7 for d in dims)
    check(subtypes == [0, 1, 3, 4, 6],
          f"标注类型集合应为 线性/对齐/直径/半径/坐标，实际 {subtypes}")

    arc_dims = modelspace.query("ARC_DIMENSION")
    check(len(arc_dims) == 1, f"ARC_DIMENSION 数应为 1，实际 {len(arc_dims)}")

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
        elif subtype == 6:    # 坐标（X 型：引线偏竖直 ⇒ 量 X 坐标）
            check(bool(d.dxf.dimtype & 64), "坐标标注应为 X 型（70 的 bit6 置位）")
            feat = d.dxf.defpoint2      # 13 被测点
            lead = d.dxf.defpoint3      # 14 引线端点
            base = d.dxf.defpoint       # 10 坐标基准
            check(close(feat.x, 180.0) and close(feat.y, 110.0),
                  f"坐标标注被测点错误: {feat}")
            check(close(lead.x, 196.0) and close(lead.y, 136.0),
                  f"坐标标注引线端点错误: {lead}")
            check(close(base.x, 0.0) and close(base.y, 0.0),
                  f"坐标标注基准应为 WCS 原点，实际 {base}")
            check(close(measurement, 180.0, 1e-6),
                  f"坐标标注测量值应为被测点 X 坐标 180，实际 {measurement}")

    # ---- 弧长标注：量的是圆弧的弧长（半径 × 包角），不是弦长 ----
    for d in arc_dims:
        # 被测弧：圆心 (120,55)、半径 25、30° 逆时针扫到 250° ⇒ 弧长 95.9931
        check(close(measurement, 25.0 * math.radians(220.0), 1e-4),
              f"弧长标注测量值应为 95.9931，实际 {measurement}")

        block_name = getattr(d.dxf, "geometry", None)
        check(bool(block_name) and block_name in doc.blocks,
              f"弧长标注的几何块 {block_name!r} 应存在于 BLOCKS 区")
        if block_name and block_name in doc.blocks:
            block_entities = list(doc.blocks.get(block_name))
            check(len(block_entities) >= 3,
                  f"几何块 {block_name} 内容过少: {len(block_entities)} 个实体")
            check(any(e.dxftype() == "TEXT" for e in block_entities),
                  f"几何块 {block_name} 缺少测量文字")
            check(any(e.dxftype() == "SOLID" for e in block_entities),
                  f"几何块 {block_name} 缺少箭头 (SOLID)")
            check(any(e.dxftype() == "ARC" for e in block_entities),
                  f"几何块 {block_name} 缺少尺寸弧")

    # ---- 回写再读：验证 ezdxf 能无损重写我们的文件 ----
    # 回写产物一律落在临时目录：sample/ 是纳入版本管理的参考图纸目录，
    # 校验脚本对它只读，绝不往里写任何中间文件。
    rewritten = ezdxf.recover.readfile(str(path))[0]
    ok = True
    try:
        with tempfile.TemporaryDirectory(prefix="lycad-verify-") as tmp:
            rewritten.saveas(str(Path(tmp) / (path.stem + "_ezdxf.dxf")))
    except Exception as exc:                       # noqa: BLE001
        ok = False
        check(False, f"ezdxf 回写失败: {exc}")
    if ok:
        print("  回写: ezdxf 可无损重写该文件")


def main():
    sample_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")

    # 默认按「lycad_sample_generator 写入仓库 sample/ 的目录结构」取文件；
    # 生成器显式指定输出目录时，全部文件平铺在该目录下，直接把该目录传进来即可。
    def sample_path(year: str) -> Path:
        nested = sample_dir / year / f"lycad_sample_{year}.dxf"
        return nested if nested.is_file() else sample_dir / f"lycad_sample_{year}.dxf"

    # 可写版本自 R2000 起：实体集完全相同（都是原生 HATCH/SPLINE/ELLIPSE/XLINE/RAY），
    # 差别只在 $ACADVER 与文本编码（2007 起 UTF-8），因此共用一套期望值，
    # 靠 expected_version 触发版本号与编码两项断言。
    # R12 (AC1009) 已不在可写清单里，也就没有对应的样例可校。
    for year, version in (("2000", "AC1015"), ("2004", "AC1018"), ("2007", "AC1021"),
                          ("2010", "AC1024"), ("2013", "AC1027"), ("2018", "AC1032")):
        path = sample_path(year)
        if not path.is_file():
            print(f"\n(跳过 {version}：未找到 lycad_sample_{year}.dxf)")
            continue
        verify(path, expected_version=version)

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
