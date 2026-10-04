// tests/tools/render_terminal_text.cpp
//
// 把一份**真实的**终端输出（纯文本）渲染成 PNG，并可对任意 PNG 做程序化检查。
//
// 为什么需要它：backup-server-admin 是终端程序，而这台验收机器上没有终端
// 模拟器（没有 X server / xterm / ImageMagick / Pillow）。用户在 SSH 里看到的
// 那一屏就是这些字节本身，所以这里用等宽字体把这批字节原样画进一张图：
//
//   * 输入文件逐字节读入；除了把 TAB 展开成 8 列对齐、去掉行尾的 CR 之外，
//     一个字符都不改（不换行、不省略、不加标题、不涂色、不排序）；
//   * 图里出现的每一行都来自输入文件，且顺序一致；
//   * 每张图的输入文本 sha256 与 PNG sha256 都打印出来，可以逐字节复核。
//
// 它**不是**窗口截图，也不冒充窗口截图：GUI 的截图走真实的
// QQuickWindow::grabWindow()（见 ui/modern/main.cpp 的 --remote-acceptance）。
//
// 用法:
//   render_terminal_text <输入文本> <输出PNG> [字号]
//   render_terminal_text --check <PNG>...            程序化检查（§32）
//
// --check 打印每个 PNG 的宽高、亮度范围、颜色数、不透明比例与 sha256，并在
// 任何一项不合格时以 1 退出。

#include <QByteArray>
#include <QColor>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QList>
#include <QPainter>
#include <QSet>
#include <QString>
#include <QStringList>
#include <cstdio>

namespace {

QFont TerminalFont(int pixel_size) {
  // 终端要等宽，还要能画中文：按可用性依次退让，不假设装了什么。
  QFont font;
  font.setFamilies(QStringList() << QStringLiteral("Noto Sans Mono CJK SC")
                                 << QStringLiteral("WenQuanYi Zen Hei Mono")
                                 << QStringLiteral("Noto Sans Mono")
                                 << QStringLiteral("DejaVu Sans Mono")
                                 << QStringLiteral("Noto Sans CJK SC")
                                 << QStringLiteral("monospace"));
  font.setStyleHint(QFont::Monospace);
  font.setPixelSize(pixel_size);
  font.setFixedPitch(true);
  return font;
}

QString Sha256Hex(const QByteArray& data) {
  return QString::fromLatin1(
      QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QString Sha256File(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return QStringLiteral("<unreadable>");
  }
  return Sha256Hex(file.readAll());
}

int CheckPngs(const QStringList& paths) {
  int failures = 0;
  for (const QString& path : paths) {
    QImage image;
    if (!image.load(path)) {
      std::printf("  FAIL %s -- decode 失败\n", qPrintable(path));
      ++failures;
      continue;
    }
    int minimum = 255;
    int maximum = 0;
    int opaque = 0;
    int sampled = 0;
    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); y += 3) {
      for (int x = 0; x < image.width(); x += 3) {
        const QColor color = image.pixelColor(x, y);
        const int luminance =
            (color.red() * 299 + color.green() * 587 + color.blue() * 114) / 1000;
        minimum = std::min(minimum, luminance);
        maximum = std::max(maximum, luminance);
        if (color.alpha() >= 255) {
          ++opaque;
        }
        ++sampled;
        colors.insert(color.rgb());
      }
    }
    const bool decoded = image.width() > 0 && image.height() > 0;
    const bool not_flat = maximum - minimum >= 32;
    const bool not_blank = colors.size() >= 8;
    const bool fully_opaque = sampled > 0 && opaque == sampled;
    const bool ok = decoded && not_flat && not_blank && fully_opaque;
    std::printf(
        "  %s %s %dx%d luminance=%d..%d colors=%d opaque=%d/%d sha256=%s\n",
        ok ? "PASS" : "FAIL", qPrintable(QFileInfo(path).fileName()),
        image.width(), image.height(), minimum, maximum,
        static_cast<int>(colors.size()), opaque, sampled,
        qPrintable(Sha256File(path)));
    if (!ok) {
      ++failures;
    }
  }
  std::printf("render-terminal-text: %s\n",
              failures == 0 ? "PNG 检查全部通过" : "有 PNG 不合格");
  return failures == 0 ? 0 : 1;
}

int Render(const QString& input_path, const QString& output_path,
           int pixel_size) {
  QFile file(input_path);
  if (!file.open(QIODevice::ReadOnly)) {
    std::fprintf(stderr, "无法读取 %s\n", qPrintable(input_path));
    return 1;
  }
  const QByteArray raw = file.readAll();
  QString text = QString::fromUtf8(raw);
  // 只做两件不改变内容的归一化：CRLF -> LF，TAB -> 8 列对齐空格。
  text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  QStringList lines = text.split(QLatin1Char('\n'));
  while (!lines.isEmpty() && lines.last().isEmpty()) {
    lines.removeLast();
  }
  const QFont font = TerminalFont(pixel_size);
  QFontMetricsF metrics(font);
  const double line_height = metrics.height() * 1.32;
  const double advance = metrics.horizontalAdvance(QLatin1Char('M'));

  QStringList expanded;
  int widest = 0;
  for (const QString& line : lines) {
    QString out;
    int column = 0;
    for (const QChar character : line) {
      if (character == QLatin1Char('\t')) {
        const int spaces = 8 - (column % 8);
        out += QString(spaces, QLatin1Char(' '));
        column += spaces;
        continue;
      }
      out += character;
      ++column;
    }
    widest = std::max(widest, column);
    expanded.append(out);
  }

  const int margin = 14;
  const int width = static_cast<int>(advance * (widest + 2)) + margin * 2;
  const int height = static_cast<int>(line_height * expanded.size()) + margin * 2;
  QImage image(width, height, QImage::Format_ARGB32);
  // 终端观感：深底浅字（与截图里的 Dark 主题同一族配色），整幅不透明。
  image.fill(QColor(0x1b, 0x1c, 0x20));
  QPainter painter(&image);
  painter.setFont(font);
  painter.setPen(QColor(0xe4, 0xe7, 0xeb));
  double y = margin + metrics.ascent();
  for (const QString& line : expanded) {
    double x = margin;
    for (const QChar character : line) {
      // 逐字符按真实字形宽度推进：ASCII 在等宽字体里等宽，中文按自己的宽度，
      // 因此 -- 这种 ASCII 表格的对齐与终端里看到的一致。
      painter.drawText(QPointF(x, y), QString(character));
      x += metrics.horizontalAdvance(character);
      if (x > width - margin) {
        break;
      }
    }
    y += line_height;
  }
  painter.end();
  if (!image.save(output_path)) {
    std::fprintf(stderr, "无法写入 %s\n", qPrintable(output_path));
    return 1;
  }
  std::printf("  输入 %s sha256=%s\n", qPrintable(QFileInfo(input_path).fileName()),
              qPrintable(Sha256Hex(raw)));
  std::printf("  输出 %s %dx%d sha256=%s\n", qPrintable(QFileInfo(output_path).fileName()),
              image.width(), image.height(), qPrintable(Sha256File(output_path)));
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);
  const QStringList arguments = app.arguments();
  if (arguments.size() >= 3 && arguments.at(1) == QStringLiteral("--check")) {
    QStringList paths;
    for (int index = 2; index < arguments.size(); ++index) {
      paths.append(arguments.at(index));
    }
    return CheckPngs(paths);
  }
  if (arguments.size() < 3) {
    std::fprintf(stderr,
                 "用法: %s <输入文本> <输出PNG> [字号]\n"
                 "      %s --check <PNG>...\n",
                 qPrintable(arguments.value(0)), qPrintable(arguments.value(0)));
    return 2;
  }
  const int pixel_size = arguments.size() >= 4 ? arguments.at(3).toInt() : 15;
  return Render(arguments.at(1), arguments.at(2), pixel_size <= 0 ? 15 : pixel_size);
}
