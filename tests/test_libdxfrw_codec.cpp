// =============================================================================
//  LyCAD - tests/test_libdxfrw_codec.cpp
//  libdxfrw-modify 文本解码器的回归测试。
//
//  为什么要有这个文件
//    third-party/libdxfrw-modify 是 libdxfrw 0.6.3 的永久修改版（不再跟随上游），
//    其中解码器的核心不变量是：**解码器只取决于当前文件的版本，与同进程中
//    此前处理过什么文件无关**。上游原实现用函数内 static 的 min_ver 记录
//    「本进程见过的最低版本」来选解码器，实测后果是：先打开一个 2004 版 DWG，
//    再打开一个 2018 版 DWG，后者 173 个样本里有 59 个的文本被按老代码页解码，
//    汉字几乎全丢。
//
//    这类错误不崩溃、不报错，只会让界面上的中文变成乱码，所以必须有测试盯住。
//    这里做白盒测试：直接引用修改版的内部头文件，验证这条不变量。
//
//  与 test_libdxfrw 的分工
//    那个测试走的是 LyCAD 的公开接口，验的是适配层把数据搬得对不对；
//    这个测试只验修改版自身的这条不变量，不涉及 LyCAD 的任何类型。
// =============================================================================
#include <QtTest>

#include "drw_base.h"      // DRW::Version 枚举（AC1009 / AC1015 / AC1021 …）
#include "drw_textcodec.h"

#include <string>

namespace {

/// std::string 里的字节原样取出来比对，避免经过本地编码转换
QByteArray bytes(const std::string& s)
{
    return QByteArray(s.data(), static_cast<int>(s.size()));
}

/// 一个 2007 及以后版本的文件：文本按 UTF-16（DWG）或 UTF-8（DXF）存放
const std::string kChinese = "中文测试 ABC";

/// 「中文」的 UTF-16LE 字节，即 2007+ DWG 里文字的真实存放形态
const std::string kUtf16Chinese = std::string("\x2D\x4E\x87\x65", 4);

/// 2000 版 DWG 里「中文」两个字的 GBK 字节（这是老版本文件的真实形态）
const std::string kGbkChinese = std::string("\xD6\xD0\xCE\xC4", 4);

} // namespace

class TestLibDxfRwCodec : public QObject
{
    Q_OBJECT

private slots:
    void decoderDoesNotDependOnFilesReadEarlier();
    void modernDxfTextIsUtf8Verbatim();
    void legacyCodepageStillDecodesChinese();
};

/// 解码器的行为只取决于「当前文件」，与同进程中此前读过什么无关。
///
/// 这是 libdxfrw-modify 必须守住的不变量。若把解码器改回「按进程内见过的
/// 最低版本」选择，下面的断言会失败——那正是这个测试存在的意义。
void TestLibDxfRwCodec::decoderDoesNotDependOnFilesReadEarlier()
{
    // 基准：一个只处理 2007+ 文件的进程
    DRW_TextCodec fresh;
    fresh.setVersion(DRW::AC1021, false);

    // 先确认这个基准本身是有意义的（不是「两边都返回空串」那种假通过）
    const std::string freshDecoded = fresh.toUtf8(kUtf16Chinese);
    QCOMPARE(bytes(freshDecoded), bytes(std::string("中文")));

    // 同一个进程里先处理过一个 2000 版文件，再处理 2007+ 的文件
    DRW_TextCodec afterLegacy;
    afterLegacy.setVersion(DRW::AC1015, false);
    afterLegacy.setVersion(DRW::AC1021, false);

    QVERIFY2(bytes(afterLegacy.toUtf8(kUtf16Chinese)) == bytes(freshDecoded),
             "先前读过 2000 版文件，改变了 2007+ 文件的文本解码结果");

    // 反过来：先处理 2007+ 再处理 2000 版，不能影响 2000 版的解码
    DRW_TextCodec afterModern;
    afterModern.setVersion(DRW::AC1021, false);
    afterModern.setVersion(DRW::AC1015, false);
    afterModern.setCodePage("ANSI_936", false);

    DRW_TextCodec onlyLegacy;
    onlyLegacy.setVersion(DRW::AC1015, false);
    onlyLegacy.setCodePage("ANSI_936", false);

    QCOMPARE(bytes(afterModern.toUtf8(kGbkChinese)), bytes(onlyLegacy.toUtf8(kGbkChinese)));
}

/// 2007 及以后的 DXF 里文本就是 UTF-8，写出与读回都应当原样不动。
void TestLibDxfRwCodec::modernDxfTextIsUtf8Verbatim()
{
    DRW_TextCodec codec;
    codec.setVersion(DRW::AC1021, true);
    QCOMPARE(bytes(codec.fromUtf8(kChinese)), bytes(kChinese));
    QCOMPARE(bytes(codec.toUtf8(kChinese)), bytes(kChinese));

    // 前面先碰过一个老版本文件，这条性质同样不能变
    codec.setVersion(DRW::AC1015, true);
    codec.setVersion(DRW::AC1021, true);
    QCOMPARE(bytes(codec.fromUtf8(kChinese)), bytes(kChinese));
    QCOMPARE(bytes(codec.toUtf8(kChinese)), bytes(kChinese));
}

/// 老版本 DWG 的中文存放在代码页里（简中为 GBK），按 $DWGCODEPAGE 解出。
/// 这条与补丁无关，但它是老图纸能读对的前提，顺手锁住。
void TestLibDxfRwCodec::legacyCodepageStillDecodesChinese()
{
    DRW_TextCodec codec;
    codec.setVersion(DRW::AC1015, false);
    codec.setCodePage("ANSI_936", false);
    QCOMPARE(bytes(codec.toUtf8(kGbkChinese)), bytes(std::string("中文")));

    // 写出方向：中文应当变成 GBK 双字节
    QCOMPARE(bytes(codec.fromUtf8(std::string("中文"))), bytes(kGbkChinese));
}

QTEST_MAIN(TestLibDxfRwCodec)
#include "test_libdxfrw_codec.moc"
