// =============================================================================
//  LyCAD - interact/CoordinateInput.cpp
// =============================================================================
#include "interact/CoordinateInput.h"

#include "core/Format.h"

#include <QRegularExpression>

#include <cmath>
#include <stack>

namespace lycad {

namespace {

/// 递归下降的四则运算求值器：支持 + - * / 、一元负号与括号
class ExpressionParser {
public:
    explicit ExpressionParser(const QString& text)
        : m_text(text)
    {
    }

    bool parse(double& out)
    {
        m_pos = 0;
        skipSpaces();
        if (!parseExpression(out)) return false;
        skipSpaces();
        return m_pos >= m_text.size();          // 必须完全消费，拒绝 "1+2abc"
    }

private:
    void skipSpaces()
    {
        while (m_pos < m_text.size() && m_text.at(m_pos).isSpace()) ++m_pos;
    }

    bool peek(QChar& c) const
    {
        if (m_pos >= m_text.size()) return false;
        c = m_text.at(m_pos);
        return true;
    }

    bool parseExpression(double& out)
    {
        if (!parseTerm(out)) return false;
        while (true) {
            skipSpaces();
            QChar c;
            if (!peek(c) || (c != QLatin1Char('+') && c != QLatin1Char('-'))) break;
            ++m_pos;
            double rhs = 0.0;
            if (!parseTerm(rhs)) return false;
            out = (c == QLatin1Char('+')) ? out + rhs : out - rhs;
        }
        return true;
    }

    bool parseTerm(double& out)
    {
        if (!parseFactor(out)) return false;
        while (true) {
            skipSpaces();
            QChar c;
            if (!peek(c) || (c != QLatin1Char('*') && c != QLatin1Char('/'))) break;
            ++m_pos;
            double rhs = 0.0;
            if (!parseFactor(rhs)) return false;
            if (c == QLatin1Char('/')) {
                if (std::abs(rhs) <= kEpsilon) return false;   // 除零
                out /= rhs;
            } else {
                out *= rhs;
            }
        }
        return true;
    }

    bool parseFactor(double& out)
    {
        skipSpaces();
        QChar c;
        if (!peek(c)) return false;

        if (c == QLatin1Char('-')) {
            ++m_pos;
            if (!parseFactor(out)) return false;
            out = -out;
            return true;
        }
        if (c == QLatin1Char('+')) {
            ++m_pos;
            return parseFactor(out);
        }
        if (c == QLatin1Char('(')) {
            ++m_pos;
            if (!parseExpression(out)) return false;
            skipSpaces();
            if (!peek(c) || c != QLatin1Char(')')) return false;
            ++m_pos;
            return true;
        }

        const int start = m_pos;
        while (m_pos < m_text.size()
               && (m_text.at(m_pos).isDigit() || m_text.at(m_pos) == QLatin1Char('.'))) {
            ++m_pos;
        }
        if (m_pos == start) return false;

        bool ok = false;
        out = m_text.mid(start, m_pos - start).toDouble(&ok);
        return ok;
    }

    QString m_text;
    int m_pos = 0;
};

/// 按分隔符拆分坐标分量
QStringList splitComponents(const QString& text)
{
    return text.split(QLatin1Char(','), Qt::SkipEmptyParts);
}

} // namespace

std::optional<double> evaluateExpression(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return std::nullopt;

    ExpressionParser parser(trimmed);
    double value = 0.0;
    if (!parser.parse(value)) return std::nullopt;
    if (!std::isfinite(value)) return std::nullopt;
    return value;
}

ParsedCoordinate parseCoordinate(const QString& rawText)
{
    ParsedCoordinate result;
    const QString text = rawText.trimmed();

    if (text.isEmpty()) {
        result.error = QStringLiteral("输入为空。");
        return result;
    }

    bool relative = false;
    QString body = text;

    // 相对坐标前缀 '@'
    if (body.startsWith(QLatin1Char('@'))) {
        relative = true;
        body = body.mid(1).trimmed();
        if (body.isEmpty()) {
            result.error = QStringLiteral("相对坐标缺少数值。");
            return result;
        }
    }

    // 极坐标形式： 长度<角度
    const int anglePos = body.indexOf(QLatin1Char('<'));
    if (anglePos >= 0) {
        const QString lengthText = body.left(anglePos).trimmed();
        const QString angleText = body.mid(anglePos + 1).trimmed();

        const auto length = evaluateExpression(lengthText);
        const auto angle = evaluateExpression(angleText);
        if (!length || !angle) {
            result.error = QStringLiteral("极坐标格式应为「长度<角度」，例如 50<45。");
            return result;
        }

        result.kind = ParsedCoordinate::Kind::Polar;
        result.distance = *length;
        result.angleDegrees = *angle;
        result.point = Point2D(*length * std::cos(degToRad(*angle)),
                               *length * std::sin(degToRad(*angle)));
        return result;
    }

    // 直角坐标形式：x,y
    if (body.contains(QLatin1Char(','))) {
        const QStringList parts = splitComponents(body);
        if (parts.size() < 2) {
            result.error = QStringLiteral("直角坐标需要两个分量，例如 100,50。");
            return result;
        }

        const auto x = evaluateExpression(parts.at(0));
        const auto y = evaluateExpression(parts.at(1));
        if (!x || !y) {
            result.error = QStringLiteral("坐标分量不是有效数值。");
            return result;
        }

        result.kind = relative ? ParsedCoordinate::Kind::Relative
                               : ParsedCoordinate::Kind::Absolute;
        result.point = Point2D(*x, *y);
        return result;
    }

    // 裸标量
    const auto scalar = evaluateExpression(body);
    if (!scalar) {
        result.error = QStringLiteral("无法解析 \"%1\"。可输入 100,50 / @50,0 / 50<45 或一个数值。")
                           .arg(rawText);
        return result;
    }

    result.kind = ParsedCoordinate::Kind::Scalar;
    result.scalar = *scalar;
    return result;
}

std::optional<Point2D> ParsedCoordinate::resolve(const std::optional<Point2D>& lastPoint) const
{
    switch (kind) {
    case Kind::Absolute:
        return point;
    case Kind::Relative:
        if (!lastPoint) return std::nullopt;
        return Point2D(lastPoint->x + point.x, lastPoint->y + point.y);
    case Kind::Polar:
        if (!lastPoint) return std::nullopt;
        return Point2D(lastPoint->x + distance * std::cos(degToRad(angleDegrees)),
                       lastPoint->y + distance * std::sin(degToRad(angleDegrees)));
    case Kind::Scalar:
    case Kind::Invalid:
        return std::nullopt;
    }
    return std::nullopt;
}

QString formatCoordinate(const Point2D& point, int decimals)
{
    return QStringLiteral("%1, %2").arg(formatLength(point.x, decimals),
                                        formatLength(point.y, decimals));
}

} // namespace lycad
