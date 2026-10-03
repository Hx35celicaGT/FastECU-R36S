#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "serial_port_actions.h"
#include <cmath>
#include <cstring>

// ===== R36S: correct math for logged values =====
// FastECU's expression evaluator groups * and / from the right, so an
// expression like "x/128*14.7" (A/F Sensor) was computed as x/(128*14.7)
// and showed ~0.07 instead of ~14.7. This evaluator uses normal math rules
// (left to right, * and / before + and -, parentheses, negative numbers).
namespace {
struct R36SExpr
{
    const QString &s;
    int p;
    double x;
    R36SExpr(const QString &str, double xv) : s(str), p(0), x(xv) {}
    bool at(QChar c) { ws(); return p < s.size() && s.at(p) == c; }
    void ws() { while (p < s.size() && s.at(p).isSpace()) p++; }
    double primary()
    {
        ws();
        if (p >= s.size()) return 0;
        QChar c = s.at(p);
        if (c == '-' || c == '+') { p++; double v = primary(); return c == '-' ? -v : v; }
        if (c == '(') { p++; double v = sum(); if (at(')')) p++; return v; }
        if (c == 'x' || c == 'X') { p++; return x; }
        int start = p;
        while (p < s.size() && (s.at(p).isDigit() || s.at(p) == '.')) p++;
        if (start == p) { p++; return 0; }          // skip anything unexpected
        return s.mid(start, p - start).toDouble();
    }
    double product()
    {
        double v = primary();
        for (;;) {
            if (at('*')) { p++; v *= primary(); }
            else if (at('/')) { p++; double d = primary(); v = (d != 0) ? v / d : 0; }
            else return v;
        }
    }
    double sum()
    {
        double v = product();
        for (;;) {
            if (at('+')) { p++; v += product(); }
            else if (at('-')) { p++; v -= product(); }
            else return v;
        }
    }
};
}

static double r36sEvalExpr(const QString &expr, double x)
{
    R36SExpr e(expr, x);
    double v = e.sum();
    return std::isnan(v) || std::isinf(v) ? 0 : v;
}
// ===== end R36S math =====

#include <QDomDocument>
#include <QFile>
#include <QHash>
#include <QPair>
#include <QDir>
#include <QTextStream>
#include <QFrame>
#include <QLabel>
#include <QGridLayout>
#include <QSplitter>
#include <QTimer>
#include <QFontMetrics>
#include <QElapsedTimer>

// ===== R36S: logging fixes =====
// Value length/address is now read correctly from <address length=".."> in
// FileActions::read_logger_definition_file(), so the old re-read is gone.

// Request every byte of a value (address, address+1, ...)
static void r36sAppendAddr(QByteArray &out, const QString &addrText, const QString &lenText)
{
    bool ok = false;
    uint32_t a = addrText.toUInt(&ok, 16);
    int n = qBound(1, lenText.toInt(), 4);
    for (int k = 0; k < n; k++) {
        uint32_t ak = a + k;
        out.append((char)(uint8_t)(ak >> 16));
        out.append((char)(uint8_t)(ak >> 8));
        out.append((char)(uint8_t)ak);
    }
}

// ===== R36S: log setup file, 6-box panel, min/max, switch logging =====
// Logging is set up from a text file in FastECU-Files (editable over USB):
//   /root/FastECU/Files/log_setup.txt   <- you edit this
//   /root/FastECU/Files/available_parameters.txt   <- written on connect, lists names/IDs
// "display" entries go in the 6 boxes, "log" entries are only recorded to CSV,
// "switch" entries are switches recorded to CSV.

static const QString R36S_FILES_DIR = "/root/FastECU/Files/";
static const int R36S_MAX_DISPLAY = 6;
static const int R36S_MAX_ADDRESSES = 80;   // one SSM A8 request: length byte limits it to ~84 addresses

static QStringList r36sDisplayIds;                    // ids shown in the boxes, in order
static QHash<int, QPair<double, double> > r36sMinMax; // value index -> (min, max) since logging started

void r36sResetMinMax()
{
    r36sMinMax.clear();
}

// Unique switch byte addresses (and which switches use them) for one A8 request
static QVector<uint32_t> r36sSwitchAddresses(FileActions::LogValuesStructure *lv, const QString &protocol)
{
    QVector<uint32_t> addrs;
    for (int i = 0; i < lv->lower_panel_switch_id.length(); i++)
    {
        for (int j = 0; j < lv->log_switch_id.length(); j++)
        {
            if (lv->lower_panel_switch_id.at(i) == lv->log_switch_id.at(j) &&
                lv->log_switch_protocol.at(j) == protocol &&
                lv->log_switch_enabled.at(j) == "1")
            {
                bool ok = false;
                uint32_t a = lv->log_switch_address.at(j).toUInt(&ok, 16) & 0xFFFFFF;
                if (ok && !addrs.contains(a))
                    addrs.append(a);
                break;
            }
        }
    }
    return addrs;
}

// Indexes (into logValues lists) of values requested in one SSM A8 read, in order.
// Only enabled values with an address are included, and only as many as fit in one request
// (space for the switch bytes is kept free).
static QVector<int> r36sLogValueIndexes(FileActions::LogValuesStructure *lv, const QString &protocol)
{
    QVector<int> indexes;
    int budget = R36S_MAX_ADDRESSES - r36sSwitchAddresses(lv, protocol).length();
    int used = 0;
    for (int i = 0; i < lv->lower_panel_log_value_id.length(); i++)
    {
        for (int j = 0; j < lv->log_value_id.length(); j++)
        {
            if (lv->lower_panel_log_value_id.at(i) == lv->log_value_id.at(j) &&
                lv->log_value_protocol.at(j) == protocol &&
                lv->log_value_enabled.at(j) == "1" &&
                !lv->log_value_address.at(j).isEmpty() &&
                lv->log_value_ecu_byte_index.at(j) != "ard")   // R36S: Arduino values are not asked from the ECU
            {
                int len = qBound(1, lv->log_value_length.at(j).toInt(), 4);
                if (used + len <= budget && !indexes.contains(j))
                {
                    indexes.append(j);
                    used += len;
                }
                break;
            }
        }
    }
    return indexes;
}

static int r36sFindValue(FileActions::LogValuesStructure *lv, const QString &protocol, const QString &key)
{
    QString k = key.trimmed().toLower();
    for (int j = 0; j < lv->log_value_id.length(); j++)
    {
        // only values the ECU can be asked for (calculated values have no address)
        if (lv->log_value_protocol.at(j) != protocol || lv->log_value_enabled.at(j) != "1" || lv->log_value_address.at(j).isEmpty())
            continue;
        if (lv->log_value_id.at(j).toLower() == k || lv->log_value_name.at(j).trimmed().toLower() == k)
            return j;
    }
    return -1;
}

static int r36sFindSwitch(FileActions::LogValuesStructure *lv, const QString &protocol, const QString &key)
{
    QString k = key.trimmed().toLower();
    for (int j = 0; j < lv->log_switch_id.length(); j++)
    {
        if (lv->log_switch_protocol.at(j) != protocol || lv->log_switch_enabled.at(j) != "1")
            continue;
        if (lv->log_switch_id.at(j).toLower() == k || lv->log_switch_name.at(j).trimmed().toLower() == k)
            return j;
    }
    return -1;
}

static QString r36sUnit(FileActions::LogValuesStructure *lv, int j)
{
    QStringList c = lv->log_value_units.at(j).split(",");
    return c.length() > 1 ? c.at(1) : QString();
}

// Shown when log_setup.txt has no usable "display" lines (first 6 this ECU supports)
static const QStringList R36S_DEFAULT_DISPLAY = {
    "Engine Speed", "Coolant Temperature", "Manifold Relative Pressure", "IAM", "A/F Sensor #1",
    "Ignition Total Timing", "Intake Air Temperature", "Mass Airflow", "Battery Voltage", "Engine Load (Relative)" };

// Also load the extended values listed for other ECU IDs (log_setup.txt: extended_ecu_id = ...).
// For a tune that reports its own ECU ID but uses the RAM layout of a known ROM.
static int r36sAddExtendedFor(FileActions::LogValuesStructure *lv, const QStringList &ids)
{
    int added = 0;
    if (lv->std_log_value_count < 0 || ids.isEmpty())
        return 0;
    for (int i = 0; i < lv->ext_id.length(); i++)
    {
        QStringList ecus = lv->ext_ecu_ids.at(i).split(',');
        bool match = false;
        for (const QString &id : ids)
            if (ecus.contains(id)) { match = true; break; }
        if (!match)
            continue;
        bool exists = false;
        for (int j = lv->std_log_value_count; j < lv->log_value_id.length(); j++)
            if (lv->log_value_id.at(j) == lv->ext_id.at(i) && lv->log_value_protocol.at(j) == lv->ext_protocol.at(i)) { exists = true; break; }
        if (exists)
            continue;
        lv->log_value_protocol.append(lv->ext_protocol.at(i));
        lv->log_value_id.append(lv->ext_id.at(i));
        lv->log_value_name.append(lv->ext_name.at(i));
        lv->log_value_description.append(lv->ext_description.at(i));
        lv->log_value_ecu_byte_index.append("ext");
        lv->log_value_ecu_bit.append("0");
        lv->log_value_target.append(lv->ext_target.at(i));
        lv->log_value_enabled.append("1");
        lv->log_value.append("0.00");
        lv->log_value_address.append(lv->ext_address.at(i));
        lv->log_value_length.append(lv->ext_length.at(i));
        lv->log_value_units.append(lv->ext_units.at(i));
        lv->log_value_storagetype.append(lv->ext_storagetype.at(i));
        lv->log_value_endian.append(lv->ext_endian.at(i));
        added++;
    }
    return added;
}

// Up to 6 display ids: what FastECU already had, else the defaults this ECU supports
static QStringList r36sFallbackDisplay(FileActions::LogValuesStructure *lv, const QString &protocol)
{
    QStringList ids;
    for (const QString &id : lv->lower_panel_log_value_id)
    {
        int j = r36sFindValue(lv, protocol, id);
        if (j >= 0 && !ids.contains(lv->log_value_id.at(j)) && ids.length() < R36S_MAX_DISPLAY)
            ids.append(lv->log_value_id.at(j));
    }
    for (const QString &name : R36S_DEFAULT_DISPLAY)
    {
        int j = r36sFindValue(lv, protocol, name);
        if (j >= 0 && !ids.contains(lv->log_value_id.at(j)) && ids.length() < R36S_MAX_DISPLAY)
            ids.append(lv->log_value_id.at(j));
    }
    return ids;
}

static void r36sWriteTemplate(const QString &path, FileActions::LogValuesStructure *lv, const QString &protocol)
{
    QFile f(path);
    if (!f.open(QFile::WriteOnly | QFile::Text))
        return;
    QTextStream out(&f);
    out << "# FastECU logging setup - edit over USB.\n"
           "# After saving, turn Logging off and on again in FastECU to reload.\n"
           "#\n"
           "# Use a parameter NAME or its ID, exactly as listed in available_parameters.txt\n"
           "#   display = shown in the 6 boxes (2 across, 3 down), in this order (max 6)\n"
           "#   log     = recorded to the CSV log file only (not shown)\n"
           "#   switch  = switch recorded to the CSV log file\n"
           "# Everything under display and log is recorded when \"Log to file\" is on.\n"
           "#\n"
           "# extended_ecu_id = also load the extended values listed for this ECU ID\n"
           "#   (only for a tune built on that ROM - its RAM addresses must match)\n"
           "# Lines starting with # are ignored.\n\n";
    for (const QString &id : r36sFallbackDisplay(lv, protocol))
    {
        int j = lv->log_value_id.indexOf(id);
        if (j >= 0)
            out << "display = " << lv->log_value_name.at(j) << "\n";
    }
    out << "\n# log = Mass Airflow\n# switch = Clutch Switch\n# extended_ecu_id = 134271823F\n";
}

static void r36sWriteAvailable(const QString &path, FileActions::LogValuesStructure *lv, const QString &protocol,
                               const QString &ecuid, const QStringList &notFound, const QStringList &notes)
{
    QFile f(path);
    if (!f.open(QFile::WriteOnly | QFile::Text))
        return;
    QTextStream out(&f);
    out << "# Parameters available for ECU ID " << ecuid << " (written by FastECU when it connects)\n"
        << "# Copy a name, or the ID in [brackets], into log_setup.txt\n\n";
    for (const QString &n : notes)
        out << "# " << n << "\n";
    if (!notes.isEmpty())
        out << "\n";
    if (!notFound.isEmpty())
    {
        out << "== NOT FOUND - check spelling in log_setup.txt ==\n";
        for (const QString &n : notFound)
            out << n << "\n";
        out << "\n";
    }
    QStringList std_lines, ext_lines, sw_lines;
    for (int j = 0; j < lv->log_value_id.length(); j++)
    {
        if (lv->log_value_protocol.at(j) != protocol || lv->log_value_enabled.at(j) != "1" || lv->log_value_address.at(j).isEmpty())
            continue;
        QString line = lv->log_value_name.at(j) + "  [" + lv->log_value_id.at(j) + "]  (" + r36sUnit(lv, j) + ")";
        if (lv->log_value_ecu_byte_index.at(j) == "ext")
            ext_lines.append(line);
        else
            std_lines.append(line);
    }
    for (int j = 0; j < lv->log_switch_id.length(); j++)
    {
        if (lv->log_switch_protocol.at(j) == protocol && lv->log_switch_enabled.at(j) == "1")
            sw_lines.append(lv->log_switch_name.at(j) + "  [" + lv->log_switch_id.at(j) + "]");
    }
    std_lines.sort(Qt::CaseInsensitive);
    ext_lines.sort(Qt::CaseInsensitive);
    sw_lines.sort(Qt::CaseInsensitive);
    out << "== Standard values (" << std_lines.length() << ") ==\n" << std_lines.join("\n") << "\n\n";
    out << "== Extended values for this ECU ID (" << ext_lines.length() << ") ==\n" << ext_lines.join("\n") << "\n\n";
    out << "== Switches (" << sw_lines.length() << ") ==\n" << sw_lines.join("\n") << "\n";
}

// ===== R36S: Arduino (Nano) as an extra log device =====
// The Nano prints a header, then one CSV line every 100 ms:
//   time_ms,ethanol_pct,fuel_temp,fuel_psi,oil_psi,afr,knock1,knock2,spare
// Each column becomes a log value named "... (Arduino)" that can be shown in the boxes
// and logged to CSV next to the ECU values. Turn it on in log_setup.txt:
//   arduino_port = auto            (or a port, e.g. /dev/ttyUSB1)
//   arduino_baud = 115200          (optional, default 115200)
//   arduino_units = fuel_temp:C    (optional unit overrides, column:unit, comma separated)
// If the Nano has no serial port (the R36S kernel has no FTDI driver), FastECU talks
// to its FTDI FT232R chip directly over USB instead (see "FTDI direct" below).
#include <QSerialPort>
#include <QSerialPortInfo>
#include <thread>
#include <mutex>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

struct R36SArduinoColumn { const char *col; const char *name; const char *unit; bool minusOneIsMissing; };
static const R36SArduinoColumn R36S_ARDUINO_COLUMNS[] = {
    { "time_ms",     "Arduino Time (Arduino)",       "ms",    false },
    { "ethanol_pct", "Ethanol Content (Arduino)",    "%",     true  },
    { "fuel_temp",   "Fuel Temperature (Arduino)",   "F",     true  },
    { "fuel_psi",    "Fuel Pressure (Arduino)",      "psi",   true  },
    { "oil_psi",     "Oil Pressure (Arduino)",       "psi",   true  },
    { "afr",         "Wideband AFR (Arduino)",       "AFR",   true  },
    { "knock1",      "Knock 1 (Arduino)",            "level", false },
    { "knock2",      "Knock 2 (Arduino)",            "level", false },
    { "spare",       "Spare (Arduino)",              "",      false },
};

static QSerialPort *r36sArduinoPort = nullptr;
static FileActions::LogValuesStructure *r36sArduinoLv = nullptr;
static QString r36sArduinoProtocol;
static QString r36sArduinoEcuPort;          // the OBD cable's port, never used for the Arduino
static QStringList r36sArduinoColumns;       // column names from the Nano's header line
static QByteArray r36sArduinoBuffer;
static QElapsedTimer r36sArduinoLastLine;
static QHash<QString, QString> r36sArduinoUnits;

void r36sArduinoSetEcuPort(const QString &port)
{
    r36sArduinoEcuPort = port.split(" - ").at(0).trimmed();
}

static int r36sArduinoIndex(FileActions::LogValuesStructure *lv, const QString &col)
{
    QString id = "ARD_" + col;
    for (int j = lv->log_value_id.length() - 1; j >= 0; j--)
        if (lv->log_value_id.at(j) == id && lv->log_value_protocol.at(j) == r36sArduinoProtocol)
            return j;
    return -1;
}

// Add one log value for an Arduino column (once)
static void r36sArduinoAddColumn(FileActions::LogValuesStructure *lv, const QString &col)
{
    if (col == "" || r36sArduinoIndex(lv, col) >= 0)
        return;
    QString name = col + " (Arduino)", unit;
    for (const R36SArduinoColumn &c : R36S_ARDUINO_COLUMNS)
        if (col == c.col) { name = c.name; unit = c.unit; }
    if (r36sArduinoUnits.contains(col))
        unit = r36sArduinoUnits.value(col);
    lv->log_value_protocol.append(r36sArduinoProtocol);
    lv->log_value_id.append("ARD_" + col);
    lv->log_value_name.append(name);
    lv->log_value_description.append("Arduino column " + col);
    lv->log_value_ecu_byte_index.append("ard");   // not requested from the ECU
    lv->log_value_ecu_bit.append("0");
    lv->log_value_target.append("1");
    lv->log_value_enabled.append("1");
    lv->log_value.append("--");
    lv->log_value_address.append("ard");
    lv->log_value_length.append("1");
    lv->log_value_units.append("conversion 0," + unit + ",x,0.00,0,0,0");
    lv->log_value_storagetype.append("");
    lv->log_value_endian.append("big");
}

static bool r36sArduinoMinusOneIsMissing(const QString &col)
{
    for (const R36SArduinoColumn &c : R36S_ARDUINO_COLUMNS)
        if (col == c.col) return c.minusOneIsMissing;
    return false;
}

static void r36sArduinoLine(const QByteArray &raw)
{
    FileActions::LogValuesStructure *lv = r36sArduinoLv;
    QString line = QString::fromLatin1(raw).trimmed();
    if (!lv || line.isEmpty())
        return;
    if (line.startsWith("time_ms,"))
    {
        // Header: remember the column order, add any columns we did not know about
        r36sArduinoColumns = line.split(',');
        for (QString &c : r36sArduinoColumns) c = c.trimmed();
        for (const QString &c : r36sArduinoColumns) r36sArduinoAddColumn(lv, c);
        return;
    }
    if (r36sArduinoColumns.isEmpty())
    {
        // Header missed (Nano was already running): accept lines in the sketch's known column order
        QStringList parts = line.split(',');
        const int known = sizeof(R36S_ARDUINO_COLUMNS) / sizeof(R36S_ARDUINO_COLUMNS[0]);
        bool numeric = (parts.length() == known);
        for (const QString &p : parts) { bool ok = false; p.trimmed().toDouble(&ok); numeric = numeric && ok; }
        if (!numeric)
            return;                              // I2C scan lines etc. before the header
        for (const R36SArduinoColumn &c : R36S_ARDUINO_COLUMNS) r36sArduinoColumns << c.col;
    }
    QStringList parts = line.split(',');
    if (parts.length() != r36sArduinoColumns.length())
        return;                                  // partial or garbled line
    r36sArduinoLastLine.start();
    for (int i = 0; i < parts.length(); i++)
    {
        const QString &col = r36sArduinoColumns.at(i);
        int j = r36sArduinoIndex(lv, col);
        if (j < 0) continue;
        bool ok = false;
        double v = parts.at(i).trimmed().toDouble(&ok);
        if (!ok || (r36sArduinoMinusOneIsMissing(col) && v == -1))
        {
            lv->log_value.replace(j, "--");      // no reading: shown as -- and left out of min/max
            continue;
        }
        lv->log_value.replace(j, parts.at(i).trimmed());
        QHash<int, QPair<double, double> >::iterator mm = r36sMinMax.find(j);
        if (mm == r36sMinMax.end())
            r36sMinMax.insert(j, qMakePair(v, v));
        else
        {
            mm.value().first = qMin(mm.value().first, v);
            mm.value().second = qMax(mm.value().second, v);
        }
    }
}

// ---- FTDI direct: talk to the Nano's FT232R chip over USB (no kernel driver needed) ----
// Uses Linux usbfs (/dev/bus/usb/BBB/DDD). A reader thread collects the bytes; the
// FastECU timer hands them to r36sArduinoLine on the main thread.
static int r36sFtdiFd = -1;
static std::thread r36sFtdiThread;
static std::atomic<bool> r36sFtdiRun(false);
static std::mutex r36sFtdiMutex;
static QByteArray r36sFtdiIn;
static QString r36sFtdiName;

static bool r36sFtdiControl(int fd, int request, int value, int index)
{
    struct usbdevfs_ctrltransfer c;
    memset(&c, 0, sizeof(c));
    c.bRequestType = 0x40;                       // vendor request, host to device
    c.bRequest = request;
    c.wValue = value;
    c.wIndex = index;
    c.wLength = 0;
    c.timeout = 1000;
    c.data = nullptr;
    return ioctl(fd, USBDEVFS_CONTROL, &c) >= 0;
}

// FT232R baud rate divisor (3 MHz base clock, 1/8 steps), as the FTDI driver computes it
static void r36sFtdiBaudValue(int baud, int &value, int &index)
{
    static const int fracCode[8] = { 0, 3, 2, 4, 1, 5, 6, 7 };
    int div8 = (3000000 * 8 + baud / 2) / baud;
    uint32_t enc = (div8 >> 3) | (fracCode[div8 & 7] << 14);
    if (enc == 1) enc = 0;                       // 3 Mbaud
    else if (enc == 0x4001) enc = 1;             // 2 Mbaud
    value = enc & 0xFFFF;
    index = (enc >> 16) & 0xFFFF;
}

// Find an FT232R (0403:6001) in /sys and open it through usbfs
static int r36sFtdiOpenDevice(QString &name)
{
    QDir sys("/sys/bus/usb/devices");
    for (const QString &d : sys.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        auto readAttr = [&](const char *a) {
            QFile f(sys.filePath(d) + "/" + a);
            return f.open(QFile::ReadOnly) ? QString(f.readAll()).trimmed() : QString();
        };
        if (readAttr("idVendor") != "0403" || readAttr("idProduct") != "6001")
            continue;
        QString path = QString("/dev/bus/usb/%1/%2").arg(readAttr("busnum").toInt(), 3, 10, QChar('0'))
                                                    .arg(readAttr("devnum").toInt(), 3, 10, QChar('0'));
        int fd = ::open(path.toLatin1().constData(), O_RDWR);
        if (fd < 0)
            continue;
        unsigned int iface = 0;
        if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0)
        {
            ::close(fd);                         // a driver owns it (then it has a serial port anyway)
            continue;
        }
        name = "FTDI " + readAttr("product") + " at " + path;
        return fd;
    }
    return -1;
}

static void r36sFtdiStop()
{
    r36sFtdiRun = false;
    if (r36sFtdiThread.joinable())
        r36sFtdiThread.join();
    if (r36sFtdiFd >= 0)
    {
        unsigned int iface = 0;
        ioctl(r36sFtdiFd, USBDEVFS_RELEASEINTERFACE, &iface);
        ::close(r36sFtdiFd);
        r36sFtdiFd = -1;
    }
}

static bool r36sFtdiStart(int baud, QString &status)
{
    r36sFtdiStop();
    QString name;
    int fd = r36sFtdiOpenDevice(name);
    if (fd < 0)
        return false;
    int value = 0, index = 0;
    r36sFtdiBaudValue(baud, value, index);
    bool ok = r36sFtdiControl(fd, 0, 0, 1)                 // reset
           && r36sFtdiControl(fd, 3, value, index)         // baud rate
           && r36sFtdiControl(fd, 4, 8, 1)                 // 8 data bits, no parity, 1 stop bit
           && r36sFtdiControl(fd, 2, 0, 1)                 // no flow control
           && r36sFtdiControl(fd, 9, 16, 1);               // latency timer 16 ms
    if (!ok)
    {
        unsigned int iface = 0;
        ioctl(fd, USBDEVFS_RELEASEINTERFACE, &iface);
        ::close(fd);
        status = "Arduino: found " + name + " but could not set it up (" + QString(strerror(errno)) + ")";
        return true;
    }
    // Pulse DTR to restart the Nano, so its header line is sent again
    r36sFtdiControl(fd, 1, 0x0101, 1);
    QThread::msleep(50);
    r36sFtdiControl(fd, 1, 0x0100, 1);

    r36sFtdiFd = fd;
    r36sFtdiName = name;
    r36sFtdiRun = true;
    r36sFtdiThread = std::thread([fd]() {
        unsigned char buf[512];
        while (r36sFtdiRun)
        {
            struct usbdevfs_bulktransfer b;
            b.ep = 0x81;
            b.len = sizeof(buf);
            b.timeout = 200;
            b.data = buf;
            int n = ioctl(fd, USBDEVFS_BULK, &b);
            if (n < 0)
            {
                if (errno == ETIMEDOUT) continue;
                break;                               // unplugged; reopened on next Logging on
            }
            // Every 64-byte USB packet starts with 2 FTDI status bytes
            QByteArray data;
            for (int p = 0; p < n; p += 64)
            {
                int end = qMin(p + 64, n);
                if (end - p > 2)
                    data.append(reinterpret_cast<const char*>(buf + p + 2), end - p - 2);
            }
            if (!data.isEmpty())
            {
                std::lock_guard<std::mutex> lock(r36sFtdiMutex);
                r36sFtdiIn.append(data);
                if (r36sFtdiIn.size() > 8192) r36sFtdiIn.clear();
            }
        }
        r36sFtdiRun = false;
    });

    // Main-thread timer: hand received lines to the parser
    static QTimer *pump = nullptr;
    if (!pump)
    {
        pump = new QTimer(qApp);
        QObject::connect(pump, &QTimer::timeout, []() {
            QByteArray data;
            {
                std::lock_guard<std::mutex> lock(r36sFtdiMutex);
                data.swap(r36sFtdiIn);
            }
            if (data.isEmpty()) return;
            r36sArduinoBuffer.append(data);
            int nl;
            while ((nl = r36sArduinoBuffer.indexOf('\n')) >= 0)
            {
                r36sArduinoLine(r36sArduinoBuffer.left(nl));
                r36sArduinoBuffer.remove(0, nl + 1);
            }
            if (r36sArduinoBuffer.size() > 4096) r36sArduinoBuffer.clear();
        });
        pump->start(30);
    }
    status = "Arduino: reading " + name + " directly (no serial driver) at " + QString::number(baud);
    return true;
}

// Pick the Arduino's port: the one set in log_setup.txt, or with "auto" the first
// USB serial port that is not the OBD cable (Arduino / CH340 / FTDI chips first).
static QString r36sArduinoFindPort(const QString &setting)
{
    if (setting.toLower() != "auto")
        return setting;
    QString fallback;
    for (const QSerialPortInfo &p : QSerialPortInfo::availablePorts())
    {
        QString path = p.systemLocation();
        if (path == r36sArduinoEcuPort || !(path.contains("ttyUSB") || path.contains("ttyACM")))
            continue;
        quint16 vid = p.hasVendorIdentifier() ? p.vendorIdentifier() : 0;
        if (vid == 0x2341 || vid == 0x2A03 || vid == 0x1A86 || vid == 0x0403)
            return path;
        if (fallback.isEmpty())
            fallback = path;
    }
    return fallback;
}

// Called from r36sApplyLogSetup (connect and Logging on): read the arduino_ settings,
// make sure the Arduino values exist, open the port if needed. Returns a status line.
QStringList r36sArduinoUpdate(FileActions::LogValuesStructure *lv, const QString &protocol)
{
    QString portSetting, baud = "115200";
    r36sArduinoUnits.clear();
    QFile f(QDir(R36S_FILES_DIR).filePath("log_setup.txt"));
    if (f.open(QFile::ReadOnly | QFile::Text))
    {
        QTextStream in(&f);
        while (!in.atEnd())
        {
            QString line = in.readLine().trimmed();
            if (line.startsWith('#')) continue;
            int eq = line.indexOf('=');
            if (eq < 0) continue;
            QString kind = line.left(eq).trimmed().toLower();
            QString val = line.mid(eq + 1).trimmed();
            if (kind == "arduino_port") portSetting = val;
            else if (kind == "arduino_baud") baud = val;
            else if (kind == "arduino_units")
                for (const QString &pair : val.split(',', Qt::SkipEmptyParts))
                {
                    int c = pair.indexOf(':');
                    if (c > 0) r36sArduinoUnits.insert(pair.left(c).trimmed(), pair.mid(c + 1).trimmed());
                }
        }
    }
    if (portSetting.isEmpty())
    {
        if (r36sArduinoPort) { r36sArduinoPort->close(); }
        r36sFtdiStop();
        return QStringList() << "Arduino: off (add  arduino_port = auto  to log_setup.txt to use it)";
    }

    r36sArduinoLv = lv;
    r36sArduinoProtocol = protocol;
    for (const R36SArduinoColumn &c : R36S_ARDUINO_COLUMNS) r36sArduinoAddColumn(lv, c.col);
    for (const QString &c : r36sArduinoColumns) r36sArduinoAddColumn(lv, c);

    if (!r36sArduinoPort)
    {
        r36sArduinoPort = new QSerialPort(qApp);
        QObject::connect(r36sArduinoPort, &QSerialPort::readyRead, []() {
            r36sArduinoBuffer.append(r36sArduinoPort->readAll());
            int nl;
            while ((nl = r36sArduinoBuffer.indexOf('\n')) >= 0)
            {
                r36sArduinoLine(r36sArduinoBuffer.left(nl));
                r36sArduinoBuffer.remove(0, nl + 1);
            }
            if (r36sArduinoBuffer.size() > 4096) r36sArduinoBuffer.clear();
        });
        QObject::connect(r36sArduinoPort, &QSerialPort::errorOccurred, [](QSerialPort::SerialPortError e) {
            if (e == QSerialPort::ResourceError) r36sArduinoPort->close();   // unplugged; reopened on next Logging on
        });
        // Values go to -- if the Nano stops sending
        QTimer *stale = new QTimer(qApp);
        QObject::connect(stale, &QTimer::timeout, []() {
            if (!r36sArduinoLv || !r36sArduinoLastLine.isValid() || r36sArduinoLastLine.elapsed() < 1500) return;
            for (const QString &c : r36sArduinoColumns)
            {
                int j = r36sArduinoIndex(r36sArduinoLv, c);
                if (j >= 0) r36sArduinoLv->log_value.replace(j, "--");
            }
        });
        stale->start(500);
    }
    if (r36sArduinoPort->isOpen())
        return QStringList() << "Arduino: reading " + r36sArduinoPort->portName();
    if (r36sFtdiRun)
        return QStringList() << "Arduino: reading " + r36sFtdiName + " directly";

    QString port = r36sArduinoFindPort(portSetting);
    if (port.isEmpty())
    {
        // No serial port for the Nano (e.g. no FTDI driver on the R36S): talk to the chip directly
        QString status;
        r36sArduinoColumns.clear();
        r36sArduinoBuffer.clear();
        if (r36sFtdiStart(baud.toInt() > 0 ? baud.toInt() : 115200, status))
            return QStringList() << status;
        return QStringList() << "Arduino: no USB serial port or FTDI chip found besides the OBD cable (" + r36sArduinoEcuPort + ")";
    }
    r36sArduinoPort->setPortName(port);
    r36sArduinoPort->setBaudRate(baud.toInt() > 0 ? baud.toInt() : 115200);
    r36sArduinoColumns.clear();                  // wait for the header after the Nano restarts
    r36sArduinoBuffer.clear();
    if (!r36sArduinoPort->open(QIODevice::ReadOnly))
        return QStringList() << "Arduino: could not open " + port + " (" + r36sArduinoPort->errorString() + ")";
    return QStringList() << "Arduino: reading " + port + " at " + baud;
}
// ===== end R36S Arduino =====




// Read log_setup.txt and set what is requested, shown and logged. Called on connect and when logging starts.
void r36sApplyLogSetup(FileActions::LogValuesStructure *lv, const QString &protocol, const QString &ecuid)
{
    if (!lv)
        return;
    QDir dir(R36S_FILES_DIR);
    if (!dir.exists())
    {
        // Not started through the R36S launcher: keep FastECU's own selection
        r36sDisplayIds = lv->lower_panel_log_value_id.mid(0, R36S_MAX_DISPLAY);
        return;
    }
    QString setupPath = dir.filePath("log_setup.txt");
    if (!QFile::exists(setupPath))
        r36sWriteTemplate(setupPath, lv, protocol);

    QStringList display, logged, switches, notFound, notes;
    QList<QPair<QString, QString> > entries;   // (kind, key) in file order
    QStringList extraIds;
    QFile f(setupPath);
    if (f.open(QFile::ReadOnly | QFile::Text))
    {
        QTextStream in(&f);
        while (!in.atEnd())
        {
            QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#'))
                continue;
            int eq = line.indexOf('=');
            if (eq < 0)
                continue;
            QString kind = line.left(eq).trimmed().toLower();
            QString key = line.mid(eq + 1).trimmed();
            if (key.isEmpty())
                continue;
            if (kind == "extended_ecu_id")
            {
                for (const QString &id : key.toUpper().remove(' ').split(',', Qt::SkipEmptyParts))
                    if (!extraIds.contains(id)) extraIds.append(id);
                continue;
            }
            entries.append(qMakePair(kind, key));
        }
    }

    // Extended values for extra ECU IDs must exist before names are matched
    r36sAddExtendedFor(lv, extraIds);

        notes << r36sArduinoUpdate(lv, protocol);   // R36S: Arduino values + port

    for (const QPair<QString, QString> &e : entries)
    {
        {
            QString kind = e.first;
            QString key = e.second;
            if (kind == "display" || kind == "log")
            {
                int j = r36sFindValue(lv, protocol, key);
                if (j < 0) { notFound.append(kind + " = " + key); continue; }
                QString id = lv->log_value_id.at(j);
                if (kind == "display" && display.length() < R36S_MAX_DISPLAY && !display.contains(id))
                    display.append(id);
                else if (!logged.contains(id) && !display.contains(id))
                    logged.append(id);
            }
            else if (kind == "switch")
            {
                int j = r36sFindSwitch(lv, protocol, key);
                if (j < 0) { notFound.append(kind + " = " + key); continue; }
                if (!switches.contains(lv->log_switch_id.at(j)))
                    switches.append(lv->log_switch_id.at(j));
            }
        }
    }

    if (display.isEmpty())
    {
        // Never leave the boxes empty
        display = r36sFallbackDisplay(lv, protocol);
        for (const QString &id : display)
            logged.removeAll(id);
        notes << "log_setup.txt has no usable display lines - showing defaults. Add display = ... lines to choose.";
    }

    r36sDisplayIds = display;
    lv->lower_panel_log_value_id = display + logged;   // request + CSV order: display first, then log
    lv->lower_panel_switch_id = switches;

    int extCount = 0;
    for (int j = 0; j < lv->log_value_id.length(); j++)
        if (lv->log_value_protocol.at(j) == protocol && lv->log_value_ecu_byte_index.at(j) == "ext")
            extCount++;
    notes.prepend("Extended values loaded: " + QString::number(extCount) +
                  (extraIds.isEmpty() ? QString(" (for ECU ID ") + ecuid + ")"
                                      : QString(" (ECU ID ") + ecuid + " + extended_ecu_id " + extraIds.join(",") + ")"));

    if (!ecuid.isEmpty())
        r36sWriteAvailable(dir.filePath("available_parameters.txt"), lv, protocol, ecuid, notFound, notes);
}

// ---- 6-box panel (2 across, 3 down) ----
static void r36sPanelFitFonts(QWidget *panel)
{
    for (int i = 0; i < R36S_MAX_DISPLAY; i++)
    {
        QFrame *box = panel->findChild<QFrame*>("r36sBox" + QString::number(i));
        if (!box) continue;
        int h = box->height(), w = box->width();
        QLabel *title = box->findChild<QLabel*>("r36sTitle");
        QLabel *value = box->findChild<QLabel*>("r36sValue");
        QLabel *minmax = box->findChild<QLabel*>("r36sMinMax");
        int tpx = qBound(9, h * 17 / 100, 16);
        int mpx = tpx;   // R36S: min/max same size as the title
        int vpx = qBound(12, h * 40 / 100, 48);
        QFont vf("Arial");
        while (vpx > 12)
        {
            vf.setPixelSize(vpx);
            if (QFontMetrics(vf).horizontalAdvance("-8888.88") <= w * 80 / 100) break;
            vpx--;
        }
        QFont tf("Arial"); tf.setPixelSize(tpx); tf.setBold(true);
        QFont mf("Arial"); mf.setPixelSize(mpx);
        vf.setPixelSize(vpx);
        if (title) title->setFont(tf);
        if (minmax) minmax->setFont(mf);
        if (value) { value->setFont(vf); value->setProperty("r36s_unit_px", qMax(8, vpx * 45 / 100)); }
    }
}

class R36SPanelScaler : public QObject
{
public:
    explicit R36SPanelScaler(QObject *parent) : QObject(parent) {}
protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() == QEvent::Resize)
        {
            QWidget *panel = qobject_cast<QWidget*>(obj);
            QTimer::singleShot(0, panel, [panel]() { r36sPanelFitFonts(panel); });
        }
        return false;
    }
};

static QWidget *r36sPanel(QWidget *area)
{
    QWidget *panel = area->findChild<QWidget*>("r36sPanel");
    if (panel)
        return panel;

    // Hide the old log box grid and the switch row; the panel replaces them
    if (QWidget *old = area->findChild<QWidget*>("r36sGridHost"))
        old->hide();
    QWidget *top = area->window();
    if (QWidget *sw = top->findChild<QWidget*>("switchBoxWidget"))
        sw->hide();

    panel = new QWidget(area);
    panel->setObjectName("r36sPanel");
    QGridLayout *grid = new QGridLayout(panel);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setSpacing(3);
    for (int i = 0; i < R36S_MAX_DISPLAY; i++)
    {
        QFrame *box = new QFrame(panel);
        box->setObjectName("r36sBox" + QString::number(i));
        box->setStyleSheet("QFrame#r36sBox" + QString::number(i) + "{border:1px solid black;border-radius:5px;}");
        QVBoxLayout *v = new QVBoxLayout(box);
        v->setContentsMargins(5, 2, 5, 2);
        v->setSpacing(0);
        QLabel *title = new QLabel(box);   title->setObjectName("r36sTitle");
        QLabel *value = new QLabel(box);   value->setObjectName("r36sValue");
        QLabel *minmax = new QLabel(box);  minmax->setObjectName("r36sMinMax");
        title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        minmax->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        minmax->setStyleSheet("color: black;");
        value->setTextFormat(Qt::RichText);
        for (QLabel *l : {title, value, minmax})
            l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);   // R36S: only the big number shrinks
        v->addWidget(title);
        v->addWidget(value, 1);
        v->addWidget(minmax);
        grid->addWidget(box, i / 2, i % 2);   // 2 across, 3 down
    }
    for (int r = 0; r < 3; r++) grid->setRowStretch(r, 1);
    for (int c = 0; c < 2; c++) grid->setColumnStretch(c, 1);

    if (QBoxLayout *outer = qobject_cast<QBoxLayout*>(area->layout()))
        outer->addWidget(panel, 1);
    panel->installEventFilter(new R36SPanelScaler(panel));

    area->hide();   // R36S: hidden until Logging is turned on (see r36sShowLogPanel)
    return panel;
}

// Titles/units for the 6 boxes (called when the selection changes)
void r36sPanelSetup(QWidget *area, FileActions::LogValuesStructure *lv, const QString &protocol)
{
    if (!area || !lv) return;
    QWidget *panel = r36sPanel(area);
    QStringList ids = r36sDisplayIds;
    if (ids.isEmpty())
        ids = lv->lower_panel_log_value_id.mid(0, R36S_MAX_DISPLAY);
    for (int i = 0; i < R36S_MAX_DISPLAY; i++)
    {
        QFrame *box = panel->findChild<QFrame*>("r36sBox" + QString::number(i));
        if (!box) continue;
        int index = -1;
        if (i < ids.length())
        {
            for (int j = 0; j < lv->log_value_id.length(); j++)
                if (lv->log_value_id.at(j) == ids.at(i) && lv->log_value_protocol.at(j) == protocol) { index = j; break; }
        }
        box->setProperty("r36s_index", index);
        box->findChild<QLabel*>("r36sTitle")->setText(index >= 0 ? lv->log_value_name.at(index) : QString());
        box->findChild<QLabel*>("r36sValue")->setText(index >= 0 ? "--" : QString());
        box->findChild<QLabel*>("r36sMinMax")->setText(QString());
    }
    r36sPanelFitFonts(panel);
}

// Live values + min/max for the 6 boxes
void r36sPanelUpdate(QWidget *area, FileActions::LogValuesStructure *lv)
{
    if (!area || !lv) return;
    QWidget *panel = area->findChild<QWidget*>("r36sPanel");
    if (!panel) return;
    for (int i = 0; i < R36S_MAX_DISPLAY; i++)
    {
        QFrame *box = panel->findChild<QFrame*>("r36sBox" + QString::number(i));
        if (!box) continue;
        int j = box->property("r36s_index").toInt();
        if (j < 0 || j >= lv->log_value.length()) continue;
        QLabel *value = box->findChild<QLabel*>("r36sValue");
        int upx = value->property("r36s_unit_px").toInt();
        if (upx <= 0) upx = 10;
        value->setText(lv->log_value.at(j) + " <span style=\"font-size:" + QString::number(upx) +
                       "px; color:black;\">" + r36sUnit(lv, j).toHtmlEscaped() + "</span>");
        QHash<int, QPair<double, double> >::const_iterator it = r36sMinMax.constFind(j);
        if (it != r36sMinMax.constEnd())
        {
            QString cur = lv->log_value.at(j);
            int dec = cur.contains('.') ? cur.length() - cur.indexOf('.') - 1 : 0;
            box->findChild<QLabel*>("r36sMinMax")->setText("min " + QString::number(it.value().first, 'f', dec) +
                                                           "   max " + QString::number(it.value().second, 'f', dec));
        }
    }
}
// ===== end R36S log setup / panel =====

// R36S: show the panel filling the screen when Logging starts, hide it when Logging stops.
// The divider stays at the top edge, so it can still be dragged down to shrink the panel.
#include <QMainWindow>
#include <QStatusBar>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QScreen>

// If the window has grown past the screen (status bar pushed off the bottom), pull it back
static void r36sKeepWindowOnScreen(QWidget *win)
{
    if (!win || win->isMaximized() || win->isFullScreen() || !win->screen())
        return;
    QSize avail = win->screen()->availableGeometry().size();
    QSize now = win->size();
    if (now.width() > avail.width() || now.height() > avail.height())
        win->resize(qMin(now.width(), avail.width()), qMin(now.height(), avail.height()));
}

void r36sShowLogPanel(QWidget *area, bool show)
{
    if (!area)
        return;
    QWidget *win = area->window();
    if (!show)
    {
        area->hide();
        QTimer::singleShot(0, win, [win]() { r36sKeepWindowOnScreen(win); });
        return;
    }
    area->show();
    if (QSplitter *split = qobject_cast<QSplitter*>(area->parentWidget()))
    {
        QTimer::singleShot(0, split, [split, area, win]() {
            QList<int> sizes;
            for (int k = 0; k < split->count(); k++)
                sizes << (split->widget(k) == area ? split->height() : 0);
            split->setSizes(sizes);
            r36sKeepWindowOnScreen(win);
        });
    }
}

// R36S: tick/untick a menu item (e.g. "Logging", "Log to file") without running its action
void r36sSetMenuChecked(QMenuBar *bar, const QString &text, bool checked)
{
    if (!bar)
        return;
    for (QMenu *menu : bar->findChildren<QMenu*>())
        for (QAction *action : menu->actions())
            if (!action->isSeparator() && !action->menu() && action->text() == text)
                action->setChecked(checked);
}

static bool r36sMenuChecked(QMenuBar *bar, const QString &text)
{
    if (!bar)
        return false;
    for (QMenu *menu : bar->findChildren<QMenu*>())
        for (QAction *action : menu->actions())
            if (!action->isSeparator() && !action->menu() && action->text() == text)
                return action->isChecked();
    return false;
}

// R36S: CSV box in the status bar, right after the yellow/green/red connection box.
//   recording -> green "Logging CSV"   armed (Log to file on, Logging off) -> grey "CSV ready"   else hidden
void r36sShowCsvState(QMainWindow *win, bool armed, bool recording)
{
    if (!win)
        return;
    QStatusBar *bar = win->statusBar();
    QLabel *label = bar->findChild<QLabel*>("r36sCsvLabel");
    if (!label)
    {
        label = new QLabel(bar);
        label->setObjectName("r36sCsvLabel");
        label->setMargin(5);
        bar->insertWidget(1, label);   // 0 = connection box
    }
    label->setProperty("r36s_state", recording ? 2 : (armed ? 1 : 0));
    if (recording)
    {
        label->setText("Logging CSV");
        label->setStyleSheet("QLabel { background-color : green; color : white; }");
        label->show();
    }
    else if (armed)
    {
        label->setText("CSV ready");
        label->setStyleSheet("QLabel { background-color : grey; color : white; }");
        label->show();
    }
    else
        label->hide();
}

// R36S: true while a ROM read/write runs (set in MainWindow::start_ecu_operations)
bool r36sEcuBusy = false;

// R36S: called first in menu_action_triggered. Returns true when the action must be ignored:
//  - Logging, Log to file, Connect and Disconnect do nothing while a ROM read/write runs
//  - Start (Logging off) does nothing while the CSV is recording: stop the CSV first (FN+Start)
bool r36sGuardAction(QMainWindow *win, QMenuBar *bar, const QString &action, bool loggingState, bool writeCsv)
{
    if (r36sEcuBusy && (action == "toggle_realtime" || action == "log_to_file" ||
                        action == "connect_to_ecu" || action == "disconnect_from_ecu"))
    {
        // undo the tick the key/click just changed
        if (action == "toggle_realtime") r36sSetMenuChecked(bar, "Logging", loggingState);
        if (action == "log_to_file")     r36sSetMenuChecked(bar, "Log to file", writeCsv);
        return true;
    }
    if (action == "toggle_realtime" && loggingState && writeCsv && !r36sMenuChecked(bar, "Logging"))
    {
        r36sSetMenuChecked(bar, "Logging", true);
        QLabel *label = win ? win->statusBar()->findChild<QLabel*>("r36sCsvLabel") : nullptr;
        if (label)
        {
            label->setText("Stop CSV first (FN+Start)");
            QTimer::singleShot(2000, label, [label]() {
                if (label->property("r36s_state").toInt() == 2)
                    label->setText("Logging CSV");
            });
        }
        return true;
    }
    return false;
}


// Raw ECU bytes -> number, using RomRaider storagetype/endian (extended params are often float)
static double r36sRawValue(const QByteArray &data, int offset, int length, const QString &storagetype, const QString &endian)
{
    uint32_t raw = 0;
    bool little = (endian == "little");
    for (int k = 0; k < length && k < 4; k++)
    {
        uint8_t byte = (uint8_t)data.at(offset + (little ? (length - 1 - k) : k));
        raw = (raw << 8) | byte;
    }
    if (storagetype == "float" && length == 4)
    {
        float f;
        memcpy(&f, &raw, sizeof(f));
        return f;
    }
    if (storagetype == "int8")  return (int8_t)raw;
    if (storagetype == "int16") return (int16_t)raw;
    if (storagetype == "int32") return (int32_t)raw;
    return raw;
}
// ===== end R36S logging fixes =====


// ===== R36S: choose display / log / switch values on the device =====
#include <QDialog>
#include <QTabWidget>
#include <QComboBox>
#include <QListWidget>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QMessageBox>

// Rewrite the display/log/switch lines of log_setup.txt; comments and other lines stay
static void r36sSaveLogSetup(const QStringList &display, const QStringList &logged, const QStringList &switches)
{
    QString path = QDir(R36S_FILES_DIR).filePath("log_setup.txt");
    QStringList kept;
    QFile in(path);
    if (in.open(QFile::ReadOnly | QFile::Text))
    {
        QTextStream ts(&in);
        while (!ts.atEnd())
        {
            QString line = ts.readLine();
            QString t = line.trimmed();
            int eq = t.indexOf('=');
            QString kind = eq > 0 ? t.left(eq).trimmed().toLower() : QString();
            if (!t.startsWith('#') && (kind == "display" || kind == "log" || kind == "switch"))
                continue;
            kept.append(line);
        }
        in.close();
    }
    while (!kept.isEmpty() && kept.last().trimmed().isEmpty())
        kept.removeLast();
    QFile out(path);
    if (!out.open(QFile::WriteOnly | QFile::Text))
        return;
    QTextStream os(&out);
    for (const QString &l : kept) os << l << "\n";
    os << "\n";
    for (const QString &n : display)  os << "display = " << n << "\n";
    for (const QString &n : logged)   os << "log = " << n << "\n";
    for (const QString &n : switches) os << "switch = " << n << "\n";
}

// Window to pick the 6 boxes, extra CSV values and switches. tab: 0 Display, 1 Log, 2 Switches.
// Returns true when OK was pressed (log_setup.txt saved and applied).
bool r36sLogSetupDialog(QWidget *parent, FileActions::LogValuesStructure *lv, const QString &protocol, const QString &ecuid, int tab)
{
    if (!lv)
        return false;
    if (!QDir(R36S_FILES_DIR).exists())
    {
        QMessageBox::information(parent, "Log values", "FastECU-Files is not available.\nStart FastECU with the R36S launcher or run-fastecu-pc.sh.");
        return false;
    }

    // What this ECU supports (known after Connect to ECU)
    QStringList names, switchNames;
    for (int j = 0; j < lv->log_value_id.length(); j++)
        if (lv->log_value_protocol.at(j) == protocol && lv->log_value_enabled.at(j) == "1" &&
            !lv->log_value_address.at(j).isEmpty() && !names.contains(lv->log_value_name.at(j)))
            names.append(lv->log_value_name.at(j));
    for (int j = 0; j < lv->log_switch_id.length(); j++)
        if (lv->log_switch_protocol.at(j) == protocol && lv->log_switch_enabled.at(j) == "1" &&
            !switchNames.contains(lv->log_switch_name.at(j)))
            switchNames.append(lv->log_switch_name.at(j));
    if (names.isEmpty())
    {
        QMessageBox::information(parent, "Log values", "Connect to the ECU first, so FastECU knows which values it supports.");
        return false;
    }
    names.sort(Qt::CaseInsensitive);
    switchNames.sort(Qt::CaseInsensitive);

    // Current selection, as names
    auto valueName = [lv, protocol](const QString &id) {
        for (int j = 0; j < lv->log_value_id.length(); j++)
            if (lv->log_value_id.at(j) == id && lv->log_value_protocol.at(j) == protocol) return lv->log_value_name.at(j);
        return QString();
    };
    auto switchName = [lv, protocol](const QString &id) {
        for (int j = 0; j < lv->log_switch_id.length(); j++)
            if (lv->log_switch_id.at(j) == id && lv->log_switch_protocol.at(j) == protocol) return lv->log_switch_name.at(j);
        return QString();
    };
    QStringList curDisplay, curLogged, curSwitches;
    for (const QString &id : r36sDisplayIds) curDisplay << valueName(id);
    for (const QString &id : lv->lower_panel_log_value_id)
        if (!r36sDisplayIds.contains(id)) curLogged << valueName(id);
    for (const QString &id : lv->lower_panel_switch_id) curSwitches << switchName(id);

    QDialog dlg(parent);
    dlg.setWindowTitle("Log values");
    dlg.resize(600, 420);   // fits the R36S 640 x 480 screen
    QVBoxLayout *mainLayout = new QVBoxLayout(&dlg);
    QTabWidget *tabs = new QTabWidget(&dlg);
    mainLayout->addWidget(tabs);

    // Display: one dropdown per box
    QWidget *displayTab = new QWidget;
    QFormLayout *form = new QFormLayout(displayTab);
    QList<QComboBox*> boxes;
    for (int i = 0; i < R36S_MAX_DISPLAY; i++)
    {
        QComboBox *cb = new QComboBox;
        cb->addItem("(empty)");
        cb->addItems(names);
        cb->setMaxVisibleItems(12);
        int k = i < curDisplay.length() ? cb->findText(curDisplay.at(i)) : -1;
        cb->setCurrentIndex(k > 0 ? k : 0);
        form->addRow(QString("Box %1 (%2, row %3)").arg(i + 1).arg(i % 2 == 0 ? "left" : "right").arg(i / 2 + 1), cb);
        boxes << cb;
    }
    tabs->addTab(displayTab, "Display");

    // Log / Switches: tap a row to select or unselect it (highlighted = on)
    auto makeList = [](const QStringList &items, const QStringList &selected) {
        QListWidget *list = new QListWidget;
        list->setSelectionMode(QAbstractItemView::MultiSelection);
        list->addItems(items);
        for (int r = 0; r < list->count(); r++)
            list->item(r)->setSelected(selected.contains(list->item(r)->text()));
        return list;
    };
    QListWidget *logList = makeList(names, curLogged);
    QListWidget *switchList = makeList(switchNames, curSwitches);
    tabs->addTab(logList, "Log to CSV");
    tabs->addTab(switchList, "Switches");

    // R36S: values shown in the boxes are always logged, so on the Log to CSV tab they are
    // highlighted and locked (can't be unselected). Changing a box updates the list right away.
    logList->setStyleSheet("QListWidget::item:selected:!enabled { background-color: #8fb4d9; color: #303030; }");
    auto syncLog = [boxes, logList]() {
        QStringList shown;
        for (QComboBox *cb : boxes)
            if (cb->currentIndex() > 0)
                shown << cb->currentText();
        for (int r = 0; r < logList->count(); r++)
        {
            QListWidgetItem *item = logList->item(r);
            bool isShown = shown.contains(item->text());
            if (isShown)
            {
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
                item->setSelected(true);
                item->setToolTip("Shown in a box - always logged");
            }
            else if (!(item->flags() & Qt::ItemIsEnabled))
            {
                item->setFlags(item->flags() | Qt::ItemIsEnabled);   // left a box: stays logged until unselected
                item->setToolTip(QString());
            }
        }
    };
    for (QComboBox *cb : boxes)
        QObject::connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged), logList, [syncLog](int) { syncLog(); });
    syncLog();

    tabs->setCurrentIndex(qBound(0, tab, 2));

    QLabel *note = new QLabel("Highlighted rows are logged. Light rows are shown in a box and always logged. Fewer values = faster updates.");
    note->setWordWrap(true);
    mainLayout->addWidget(note);
    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    mainLayout->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    QStringList display, logged, switches;
    for (QComboBox *cb : boxes)
        if (cb->currentIndex() > 0 && !display.contains(cb->currentText()))
            display << cb->currentText();
    for (int r = 0; r < logList->count(); r++)
        if (logList->item(r)->isSelected() && !display.contains(logList->item(r)->text()))
            logged << logList->item(r)->text();
    for (int r = 0; r < switchList->count(); r++)
        if (switchList->item(r)->isSelected())
            switches << switchList->item(r)->text();

    r36sSaveLogSetup(display, logged, switches);
    r36sApplyLogSetup(lv, protocol, ecuid);
    r36sResetMinMax();
    return true;
}
// ===== end R36S choose values =====



void MainWindow::kline_listener()
{
    QByteArray received;

    //serial->change_port_speed("10400");
    serial->change_port_speed("125000");

    while (haltech_ic7_display_on)
    {
        received = serial->read_serial_data(receive_timeout);
        //emit LOG_D(parse_message_to_hex(received), true, true);
        //delay(5);
    }
}

void MainWindow::canbus_listener()
{
    QByteArray received;

    while (haltech_ic7_display_on)
    {
        received = serial->read_serial_data(receive_timeout);
        //emit LOG_D(parse_message_to_hex(received), true, true);
        //delay(5);
    }
}

bool MainWindow::ecu_init()
{
    if (serial->is_serial_port_open())
    {
        if (!ecu_init_complete)
        {
            if (configValues->flash_protocol_selected_make == "Subaru")
            {
                if (configValues->flash_protocol_selected_log_transport == "CAN" || configValues->flash_protocol_selected_log_transport == "iso15765")
                    ssm_can_init();
                else if (configValues->flash_protocol_selected_log_transport == "K-Line")
                    //serial->fast_init(output);
                    ssm_kline_init();
                else if (configValues->flash_protocol_selected_log_transport == "SSM")
                    ssm_init();
            }
        }
    }
    else
    {
        //emit LOG_D("Connection is not ready!", true, true);
        ecu_init_complete = false;
        ecuid.clear();
    }
    //emit LOG_D("ECU ID check complete", true, true);

    return ecu_init_complete;
}

void MainWindow::ssm_init()
{
    QByteArray output;
    QByteArray received;

    serial->reset_connection();
    serial->set_serial_port_baudrate("1953");
    serial->set_serial_port_parity(QSerialPort::EvenParity);
    serial->open_serial_port();

    emit LOG_D("Using SSM1 protocol", true, true);

    emit LOG_D("Issue read cmd...", true, true);
    received.clear();
    output.clear();
    output.append((uint8_t)0x78);
    output.append((uint8_t)0x12);
    output.append((uint8_t)0x34);
    output.append((uint8_t)0x00);
    serial->write_serial_data_echo_check(output);
    //delay(1000);
    for (int i = 0; i < 10; i++)
    {
        received.append(serial->read_serial_data(500));
    }
    emit LOG_D("Received: " + parse_message_to_hex(received), true, true);
    //if (received.length() > 0)
    //    emit LOG_D("Something received " + parse_message_to_hex(received), true, true);
    //else
    //    emit LOG_D("No response...", true, true);


    emit LOG_D("Issue init cmd...", true, true);
    received.clear();
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x46);
    output.append((uint8_t)0x48);
    output.append((uint8_t)0x49);
    serial->write_serial_data(output);

    emit LOG_D("Init sent, delaying 2s...", true, true);
    //delay(2000);
    for (int i = 0; i < 2; i++)
    {
        received.append(serial->read_serial_data(500));
    }
    emit LOG_D("Received: " + parse_message_to_hex(received), true, true);
    //emit LOG_D("Checking response...";
    //received = serial->read_serial_data(receive_timeout);
    received.clear();
    output.clear();
    output.append((uint8_t)0x12);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    serial->write_serial_data(output);
    received.append(serial->read_serial_data(500));

    if (received.length() > 0)
    {
        emit LOG_D("Something received " + parse_message_to_hex(received), true, true);

        if (received.length() == (uint8_t)received.at(3) + 5)
        {
            ecu_init_complete = true;
            ecuid = parse_ecuid(received);
            emit LOG_D("ECU ID: " + ecuid, true, true);
            parse_log_value_list(received, "SSM");
            if (ecuid == "")
                set_status_bar_label(true, false, "");
            else
                set_status_bar_label(true, true, ecuid);

            received = serial->read_serial_data(100);
            while(received.length() > 0)
            {
                ecu_init_complete = true;
                ecuid = parse_ecuid(received);
                parse_log_value_list(received, "SSM");

                received = serial->read_serial_data(100);
                while(received.length() > 0)
                {
                    received = serial->read_serial_data(100);
                }
            }
        }
    }
    else
        emit LOG_D("No response...", true, true);

}

void MainWindow::ssm_kline_init()
{
    QByteArray output;
    QByteArray received;
    int loopcount = 0;

    //emit LOG_D("ECU K-Line INIT";
    if (!ecu_init_started)
    {
        ecu_init_started = true;

        emit LOG_D("K-Line SSM init with BF", true, true);
        output.clear();
        output.append((uint8_t)0xBF);
        serial->write_serial_data_echo_check(add_ssm_header(output, false));
        emit LOG_D("K-Line SSM init sent", true, true);
        delay(200);
        received = serial->read_serial_data(serial_read_short_timeout);
        emit LOG_D("K-Line SSM init response: " + parse_message_to_hex(received), true, true);
        while (received.length() < 4 && loopcount < 10)
        {
            received.append(serial->read_serial_data(50));
            loopcount++;
        }
        if (received.length() < 4)
            return;

        emit LOG_D("SSM Init response header received", true, true);
        loopcount = 0;
        while (received.length() < (uint8_t)received.at(3) + 5 && loopcount < 10)
        {
            received.append(serial->read_serial_data(50));
            loopcount++;
        }
        if (received.length() < (uint8_t)received.at(3) + 5)
            return;

        emit LOG_D("ECU INIT length: " + QString::number((uint8_t)received.at(3)) + " " + QString::number(received.length()), true, true);
        if (received.length() >= (uint8_t)received.at(3) + 5)
        {
            ecu_init_complete = true;
            //set_status_bar_label(true, true, ecuid);
            ecuid = parse_ecuid(received);
            emit LOG_D("ECU ID: " + ecuid, true, true);
            parse_log_value_list(received, "SSM");
            //emit LOG_D("ECU ID: " + ecuid, true, true);
            if (ecuid == "")
                set_status_bar_label(true, false, "");
            else
                set_status_bar_label(true, true, ecuid);
            return;
        }
        /*
        received.append(serial->read_serial_data(100));
        while(received.length() > 0)
        {
            //emit LOG_D("ECU ID: " + parse_ecuid(received), true, true);
            ecu_init_complete = true;
            //set_status_bar_label(true, true, ecuid);
            ecuid = parse_ecuid(received);
            parse_log_value_list(received, "SSM");

            received = serial->read_serial_data(100);
            while(received.length() > 0)
            {
                received = serial->read_serial_data(100);
            }
        }
*/
    }
    ecu_init_started = false;
}

void MainWindow::ssm_can_init()
{
    QByteArray output;
    QByteArray received;
    uint32_t control_unit_addr = 0;

    if (ecu_radio_button->isChecked())
        control_unit_addr = 0x7E0;
    else if (tcu_radio_button->isChecked())
        control_unit_addr = 0x7E1;

    const uint8_t cu_addr_b3 = (uint8_t)((control_unit_addr >> 24) & 0xFF);
    const uint8_t cu_addr_b2 = (uint8_t)((control_unit_addr >> 16) & 0xFF);
    const uint8_t cu_addr_b1 = (uint8_t)((control_unit_addr >> 8) & 0xFF);
    const uint8_t cu_addr_b0 = (uint8_t)(control_unit_addr & 0xFF);

    emit LOG_D(QString(Q_FUNC_INFO) + " " + configValues->flash_protocol_selected_log_protocol, true, true);

    if (configValues->flash_protocol_selected_log_protocol == "CAN")
        received = serial->read_serial_data(receive_timeout);
    else if (configValues->flash_protocol_selected_log_transport == "iso15765")
    {
        // Set serial port
        serial->reset_connection();
        serial->set_is_iso14230_connection(false);
        serial->set_add_iso14230_header(false);
        serial->set_is_can_connection(false);
        serial->set_is_iso15765_connection(true);
        serial->set_is_29_bit_id(false);
        serial->set_can_speed("500000");
        serial->set_iso15765_source_address(control_unit_addr);
        serial->set_iso15765_destination_address(0x7E8);
        // Open serial port
        serial->open_serial_port();

        output.clear();
        output.append(cu_addr_b3);
        output.append(cu_addr_b2);
        output.append(cu_addr_b1);
        output.append(cu_addr_b0);
        output.append((uint8_t)0x22);
        output.append((uint8_t)0xF1);
        output.append((uint8_t)0x82);
        serial->write_serial_data_echo_check(output);
        received = serial->read_serial_data(100);
        if (received.length() > 7 &&
            (uint8_t)received.at(4) == 0x62 &&
            (uint8_t)received.at(5) == 0xF1 &&
            (uint8_t)received.at(6) == 0x82)
        {
            ecu_init_complete = true;
            received = received.remove(0, 7);
            QString msg;
            for (int i = 0; i < received.length(); i++)
            {
                msg.append(QString("%1").arg((uint8_t)received.at(i),2,16,QLatin1Char('0')).toUpper());
            }
            emit LOG_D("ECU ID " + msg, true, true);
            set_status_bar_label(true, true, msg);
        }
    }
}

void MainWindow::log_ssm_values()
{
    QByteArray output;
    QByteArray received;
    uint16_t value_count = 0;

    if (ecu_init_complete)// && !log_params_request_started)
    {
        if (!logging_state)
        {
            output.append((uint8_t)0xA8);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x07);

        }
        else
        {
            //emit LOG_D("Send log request";
            output.append((uint8_t)0xA8);
            output.append((uint8_t)0x01);
            // R36S: same value list is used to build request and parse reply
            QVector<int> log_indexes = r36sLogValueIndexes(logValues, protocol);
            for (int n = 0; n < log_indexes.length(); n++)
            {
                int j = log_indexes.at(n);
                value_count++;
                r36sAppendAddr(output, logValues->log_value_address.at(j), logValues->log_value_length.at(j));   // R36S
            }
            // R36S: switch bytes after the values (each byte holds up to 8 switches)
            QVector<uint32_t> switch_addrs = r36sSwitchAddresses(logValues, protocol);
            for (int n = 0; n < switch_addrs.length(); n++)
            {
                output.append((char)(uint8_t)(switch_addrs.at(n) >> 16));
                output.append((char)(uint8_t)(switch_addrs.at(n) >> 8));
                output.append((char)(uint8_t)switch_addrs.at(n));
            }
/*
            for (int i = 0; i < logValues->dashboard_log_value_id.length(); i++)
            {
                for (int j = 0; j < logValues->log_value_id.length(); j++)
                {
                    if (logValues->dashboard_log_value_id.at(i) == logValues->log_value_id.at(j) && logValues->log_value_protocol.at(j) == protocol)
                    {
                        value_count++;
                        //emit LOG_D(logValues->log_value_id.at(i) << "at" << logValues->log_value_address.at(i) << "enabled, length" << logValues->log_value_length.at(i), true, true);
                        r36sAppendAddr(output, logValues->log_value_address.at(j), logValues->log_value_length.at(j));   // R36S
                    }
                }
            }*/
        }
        serial->write_serial_data_echo_check(add_ssm_header(output, false));
        delay(10);
    }
}

void MainWindow::read_log_serial_data()
{
    QByteArray received;

    if (!logparams_read_active)
    {
        logparams_read_active = true;
        //emit LOG_D("Read logger data from ECU", true, true);
        if (serial->get_use_openport2_adapter())
            received = serial->read_serial_data(100);
        else
        {
            int loop_count = 0;
            while (received.length() < 3 && loop_count < 100)
            {
                received.append(serial->read_serial_data(10));
                loop_count++;
            }
            //emit LOG_D("Header data" << parse_message_to_hex(received), true, true);
            // R36S: look for the reply header without reading past an empty buffer,
            // and give up after ~1.5 s so a missing reply can't hang the screen
            QElapsedTimer sync_timer;
            sync_timer.start();
            while (sync_timer.elapsed() < 1500)
            {
                if (received.length() >= 3 && (uint8_t)received.at(0) == 0x80 && (uint8_t)received.at(1) == 0xf0 && (uint8_t)received.at(2) == 0x10)
                    break;
                if (received.length() >= 3)
                    received.remove(0, 1);
                received.append(serial->read_serial_data(50));
            }
            received.append(serial->read_serial_data(50));
            received.append(serial->read_serial_data(100));
        }

        if (received.length() > 6 && (uint8_t)received.at(4) == 0xe8)
        {
            received.remove(0, 5);
            received.remove(received.length() - 1, 1);
            parse_log_params(received, protocol);
            //emit LOG_D("Log params data:" << parse_message_to_hex(received), true, true);
        }
        else
        {
            //emit LOG_D("Log params get failed:" << parse_message_to_hex(received), true, true);
        }

        logparams_read_active = false;
    }
}

QString MainWindow::parse_log_params(QByteArray received, QString protocol)
{
    QString params;

    uint16_t log_value_index = 0;
    uint16_t value_index = 0;

    double timer_elapsed = log_speed_timer->elapsed();
    if (timer_elapsed < 1)
        timer_elapsed = 1;
    double data_received = received.length();
    double log_speed = 1000.0f / timer_elapsed * data_received;

    //emit LOG_D((uint16_t)log_speed << "values read per second:" << data_received << "bytes at" << timer_elapsed << "milliseconds", true, true);

    if (!logging_request_active)
    {
        logging_request_active = true;
        //emit LOG_D("Log read count:" << logging_counter++, true, true);
        // R36S: walk the same list used for the request, byte offset advances by value length
        QVector<int> log_indexes = r36sLogValueIndexes(logValues, protocol);
        for (int n = 0; n < log_indexes.length(); n++)
        {
            int j = log_indexes.at(n);
            int log_value_length = qBound(1, logValues->log_value_length.at(j).toInt(), 4);
            if (log_value_index + log_value_length > received.length())
                break;

            QStringList conversion = logValues->log_value_units.at(j).split(",");
            if (conversion.length() < 4)
            {
                log_value_index += log_value_length;
                continue;
            }
            QString from_byte = conversion.at(2);
            QStringList format_str_lst = conversion.at(3).split(".");
            uint8_t format = 0;
            if (format_str_lst.length() > 1)
                format = format_str_lst.at(1).count(QRegularExpression("0"));

            double raw = r36sRawValue(received, log_value_index, log_value_length,
                                      logValues->log_value_storagetype.at(j),
                                      logValues->log_value_endian.at(j));
            float calc_float_value = r36sEvalExpr(from_byte, raw);
            QString calc_value = QString::number(calc_float_value, 'f', format);
            logValues->log_value.replace(j, calc_value);

            // R36S: min/max since logging started
            QHash<int, QPair<double, double> >::iterator mm = r36sMinMax.find(j);
            if (mm == r36sMinMax.end())
                r36sMinMax.insert(j, qMakePair((double)calc_float_value, (double)calc_float_value));
            else
            {
                mm.value().first = qMin(mm.value().first, (double)calc_float_value);
                mm.value().second = qMax(mm.value().second, (double)calc_float_value);
            }

            log_value_index += log_value_length;
        }

        // R36S: switch bytes follow the values, in the same order they were requested
        QVector<uint32_t> switch_addrs = r36sSwitchAddresses(logValues, protocol);
        QHash<uint32_t, uint8_t> switch_bytes;
        for (int n = 0; n < switch_addrs.length() && log_value_index < received.length(); n++)
            switch_bytes.insert(switch_addrs.at(n), (uint8_t)received.at(log_value_index++));
        for (int i = 0; i < logValues->lower_panel_switch_id.length(); i++)
        {
            for (int j = 0; j < logValues->log_switch_id.length(); j++)
            {
                if (logValues->log_switch_id.at(j) == logValues->lower_panel_switch_id.at(i) && logValues->log_switch_protocol.at(j) == protocol)
                {
                    bool ok = false;
                    uint32_t a = logValues->log_switch_address.at(j).toUInt(&ok, 16) & 0xFFFFFF;
                    if (ok && switch_bytes.contains(a))
                    {
                        int bit = logValues->log_switch_ecu_bit.at(j).toInt();
                        logValues->log_switch_state.replace(j, ((switch_bytes.value(a) >> bit) & 1) ? "1" : "0");
                    }
                    break;
                }
            }
        }
/*
        //emit LOG_D("indexOf " + logValues->log_value_id.indexOf(QString(logValues->dashboard_log_value_id.at(2)))+1, true, true);
        for (int i = 0; i < logValues->dashboard_log_value_id.length(); i++)
        {
            for (int j = 0; j < logValues->log_value_id.length(); j++)
            {
                //value_index = logValues->log_value_id.indexOf(QString(logValues->log_values_by_protocol.at(i)))+1;
                if (logValues->dashboard_log_value_id.at(i) == logValues->log_value_id.at(j) && logValues->log_value_protocol.at(j) == protocol)
                {
                    //emit LOG_D(logValues->log_value_name.at(j) << logValues->log_value_enabled.at(j), true, true);
                    if (logValues->log_value_enabled.at(j) == "1" && log_value_index < received.length())
                    {
                        QStringList conversion = logValues->log_value_units.at(j).split(",");
                        QString value_name = logValues->log_value_name.at(j);
                        QString unit = conversion.at(1);
                        QString from_byte = conversion.at(2);
                        QStringList format_str_lst = conversion.at(3).split(".");
                        uint8_t format = 0;
                        if (format_str_lst.length() > 1)
                            format = format_str_lst.at(1).count(QRegularExpression("0"));

                        //QString gauge_min = conversion.at(4);
                        //QString gauge_max = conversion.at(5);
                        //QString gauge_step = conversion.at(6);
                        QString value;
                        uint8_t log_value_length = logValues->log_value_length.at(j).toUInt();
                        for (uint8_t k = 0; k < log_value_length; k++)
                        {
                            value.append(QString::number((uint8_t)received.at(log_value_index + k)));
                        }
                        float calc_float_value = r36sEvalExpr(from_byte, value.toDouble());
                        QString calc_value = QString::number(calc_float_value, 'f', format);
                        logValues->log_value.replace(j, calc_value);

                        //emit LOG_D(QString::number(log_value_index) + ". " + value_name + ": " + calc_value + " " + unit + " from_byte: " + value + " via expr: " + from_byte, true, true);
                        //params.append(calc_value);
                        //params.append(", ");

                        log_value_index += log_value_length;
                    }
                }
            }
        }
*/
        //emit LOG_D(parse_message_to_hex(received), true, true);
        //emit LOG_D(" ", true, true);
        logging_request_active = false;
        log_to_file();
    }

    log_speed_timer->start();
    update_logbox_values(protocol);

    return params;
}

void MainWindow::parse_log_value_list(QByteArray received, QString protocol)
{
    uint16_t enabled_log_value_count = 0;
    uint16_t enabled_log_switch_count = 0;
    uint16_t log_value_count = logValues->log_value_id.length();
    uint16_t log_switch_count = logValues->log_switch_id.length();

    received.remove(0, 5);

    //emit LOG_D(parse_message_to_hex(received), true, true);

    logValues->log_values_by_protocol.clear();

    //emit LOG_D("Parsing log values list", true, true), true, true);
    for (int i = 0; i < log_value_count; i++)
    {
        if (logValues->log_value_protocol.at(i) == protocol)
        {
            logValues->log_values_by_protocol.append(logValues->log_value_name.at(i));

            // R36S: extended values are enabled by ECU ID match, not init capability bits
            if (logValues->log_value_ecu_byte_index.at(i) == "ext")
                continue;

            uint16_t ecu_byte_index = logValues->log_value_ecu_byte_index.at(i).toUInt();
            if (ecu_byte_index < received.length() && logValues->log_value_ecu_byte_index.at(i) != "No byte index")
            {
                //emit LOG_D("Value: " + logValues->log_value_id.at(i) + " " + logValues->log_value_name.at(i), true, true);
                uint8_t ecu_bit = logValues->log_value_ecu_bit.at(i).toUInt();
                uint16_t value = (uint8_t)received.at(ecu_byte_index);
                if (((value) & (1 << (ecu_bit))))
                {
                    logValues->log_value_enabled.replace(i, "1");
                    emit LOG_D("Byte: 0x" + QString::number(value, 16) + " - ECU byte index " + QString::number(ecu_byte_index) + " and bit " + QString::number(ecu_bit) +  " is enabled: " + logValues->log_value_id.at(i) + " " + logValues->log_value_name.at(i) + " " + logValues->log_value_enabled.at(i), true, true);
                    enabled_log_value_count++;
                }
                else
                {
                    logValues->log_value_enabled.replace(i, "0");
                    //emit LOG_D("Disabled: " + logValues->log_value_id.at(i) + " " + logValues->log_value_name.at(i) + " " + logValues->log_value_enabled.at(i), true, true);
                }
            }
            else
                logValues->log_value_enabled[i] = "0";

        }
    }
    //emit LOG_D("Log values list ready", true, true);

    //emit LOG_D("Parsing log switches list", true, true);
    for (int i = 0; i < log_switch_count; i++)
    {
        if (logValues->log_switch_protocol.at(i) == protocol)
        {
            // 'switch_byte_index' is byte index in SSM init response
            uint16_t switch_byte_index = logValues->log_switch_ecu_byte_index.at(i).toUInt();
            if (switch_byte_index < received.length())
            {
                uint8_t switch_bit = logValues->log_switch_ecu_bit.at(i).toUInt();
                //emit LOG_D("1 " + switch_byte_index, true, true);
                uint8_t value = (uint8_t)received.at(switch_byte_index);
                //emit LOG_D("2", true, true);
                if (((value) & (1 << (switch_bit))))
                {
                    logValues->log_switch_enabled.replace(i, "1");
                    //emit LOG_D("Switch: " + logValues->log_switch_id.at(i) + " " + logValues->log_switch_name.at(i) + " " + logValues->log_switch_enabled.at(i), true, true);
                    enabled_log_switch_count++;
                }
                else
                    logValues->log_switch_enabled.replace(i, "0");
            }
        }
    }
    // R36S: add RomRaider extended params (ecuparam) that match the connected ECU ID
    fileActions->add_extended_log_values(logValues, ecuid);

    fileActions->read_logger_conf(logValues, ecuid, false);
    //emit LOG_D("Log switches list ready", true, true);

    // R36S: what to show/log comes from FastECU-Files/log_setup.txt
    r36sArduinoSetEcuPort(serial_port);   // R36S: so the Arduino search skips the OBD cable
    r36sApplyLogSetup(logValues, protocol, ecuid);
    r36sResetMinMax();

    

    update_logboxes(protocol);
}

QByteArray MainWindow::add_ssm_header(QByteArray output, bool dec_0x100)
{
    uint8_t length = output.length();

    output.insert(0, (uint8_t)0x80);
    if (ecu_radio_button->isChecked())
    {
        emit LOG_D("Set target ID to ECU ", true, true);
        output.insert(1, (uint8_t)0x10);
    }
    else
    {
        emit LOG_D("Set target ID to TCU", true, true);
        output.insert(1, (uint8_t)0x18);
    }
    output.insert(2, (uint8_t)0xF0);
    output.insert(3, length);
    output.append(calculate_checksum(output, dec_0x100));

    emit LOG_D("Generated SSM message: " + parse_message_to_hex(output), true, true);

    return output;
}

uint8_t MainWindow::calculate_checksum(QByteArray output, bool dec_0x100)
{
    uint8_t checksum = 0;

    for (uint16_t i = 0; i < output.length(); i++)
    {
        checksum += (uint8_t)output.at(i);
    }
    if (dec_0x100)
        checksum = (uint8_t) (0x100 - checksum);

    return checksum;
}

// R36S: the CSV viewer (desktop 2) gets each row as it is written (defined further down)
void r36sCsvLiveStart(const QString &path, const QStringList &header);
void r36sCsvLiveRow(const QStringList &row);

void MainWindow::log_to_file(){
    if (write_datalog_to_file){
        if (!datalog_file_open){
            QDateTime dateTime = dateTime.currentDateTime();
            QString dateTimeString = dateTime.toString("yyyy-MM-dd_hh'h'mm'm'ss's'");

            QString log_file_name = configValues->datalog_files_directory;
            if (configValues->datalog_files_directory.at(configValues->datalog_files_directory.length() - 1) != '/')
                log_file_name.append("/");
            log_file_name.append("fastecu_" + dateTimeString + ".csv");

            datalog_file.setFileName(log_file_name);
            if (!datalog_file.open(QIODevice::WriteOnly)) {
                QMessageBox::information(this, tr("Unable to open file"),
                datalog_file.errorString());
                return;
            }
            else
            {
                datalog_file_open = true;
                log_file_timer->start();
            }

            datalog_file_outstream.setDevice(&datalog_file);
            QStringList r36sHeader;   // R36S: the same header goes to the CSV viewer
            r36sHeader << "Time";
            datalog_file_outstream << "Time,";
            for (int j = 0; j < logValues->lower_panel_log_value_id.count() ; j++){
                QString name = logValues->log_value_name.value(logValues->log_value_id.indexOf(logValues->lower_panel_log_value_id.at(j), 0));
                datalog_file_outstream << name << ",";
                r36sHeader << name;
            }
            for (int j = 0; j < logValues->lower_panel_switch_id.count() /* R36S: switches from log_setup.txt */ ; j++){
                QString name = logValues->log_switch_name.value(logValues->log_switch_id.indexOf(logValues->lower_panel_switch_id.at(j),0));
                datalog_file_outstream << name << ",";
                r36sHeader << name;
            }
            datalog_file_outstream << "\n";
            r36sCsvLiveStart(log_file_name, r36sHeader);   // R36S
        }
        else{
            QStringList r36sRow;      // R36S: the same row goes to the CSV viewer
            QString time = QString::number(log_file_timer->elapsed() / 1000.0f);
            datalog_file_outstream << time << ",";
            r36sRow << time;
            for (int j = 0; j < logValues->lower_panel_log_value_id.count() ; j++){
                QString value = logValues->log_value.value(logValues->log_value_id.indexOf(logValues->lower_panel_log_value_id.at(j), 0));
                datalog_file_outstream << value << ",";
                r36sRow << value;
            }
            for (int j = 0; j < logValues->lower_panel_switch_id.count() /* R36S: switches from log_setup.txt */ ; j++){
                QString value = logValues->log_switch_state.value(logValues->log_switch_id.indexOf(logValues->lower_panel_switch_id.at(j), 0));
                datalog_file_outstream << value << ",";
                r36sRow << value;
            }
            datalog_file_outstream << "\n";
            // R36S: write to the card about once a second, so a crash or power cut loses at most a second
            static QElapsedTimer r36sFlushClock;
            if (!r36sFlushClock.isValid() || r36sFlushClock.elapsed() >= 1000) {
                datalog_file_outstream.flush();
                r36sFlushClock.restart();
            }
            r36sCsvLiveRow(r36sRow);   // R36S
        }
    }
}


// ===== R36S: Set value window (D-pad friendly) =====
// Opened by FN+A (key S) from MainWindow::set_value(). One scrolling row:
//   [operation] [ ] [ ] [ ] [ ] [ ] [ ]      (6 number spots, empty to start)
//   Left/Right   pick a spot (far left = operation)
//   Up/Down      operation:  =  +  -  x  /
//                number spot: (empty) 0 1 2 3 4 5 6 7 8 9 .
//                first spot also has '-' when the operation is '=' (set a negative value)
//   Empty spots are skipped, so  x .98   = 1000   x 1.27  are all fine.
//   There is only one '.': putting it somewhere else moves it.
//   Y / Enter = apply, B / Esc = cancel. On a keyboard you can also type 0-9 . = + - * /
// Returns the same text the old typing box did ("=1000", "+20", "-5", "*0.98", "/2"),
// so FastECU's own code does the math on every selected cell.
#include <QKeyEvent>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QTableWidget>

class R36SSetValueDialog : public QDialog
{
public:
    explicit R36SSetValueDialog(QWidget *parent) : QDialog(parent)
    {
        setWindowTitle("Set value");
        resize(520, 280);   // fits the R36S 640 x 480 screen

        QVBoxLayout *main = new QVBoxLayout(this);
        main->addStretch(1);
        QGridLayout *grid = new QGridLayout;
        grid->setHorizontalSpacing(6);
        grid->setVerticalSpacing(0);
        for (int k = 0; k < POS; k++)
        {
            for (int r = 0; r < 3; r++)
            {
                QLabel *l = new QLabel(this);
                QFont f("Monospace");
                f.setStyleHint(QFont::TypeWriter);
                f.setBold(r == 1);
                f.setPixelSize(r == 1 ? 48 : 24);
                l->setFont(f);
                l->setAlignment(Qt::AlignCenter);
                l->setMinimumWidth(40);
                grid->addWidget(l, r, k + 1);
                cell[r][k] = l;
            }
        }
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(POS + 1, 1);
        main->addLayout(grid);
        main->addStretch(1);

        message = new QLabel(this);
        message->setAlignment(Qt::AlignCenter);
        message->setStyleSheet("color: #b00000;");
        main->addWidget(message);
        QLabel *help = new QLabel("Left/Right: move   Up/Down: change   Y: apply   B: cancel", this);
        help->setAlignment(Qt::AlignCenter);
        main->addWidget(help);
        refresh();
    }

    // The text FastECU's set_value() understands, empty if the number is not valid
    QString result() const
    {
        bool ok = false;
        double v = numberText().toDouble(&ok);
        if (!ok) return QString();
        static const char *ops[OPS] = { "=", "+", "-", "*", "/" };
        return QString(ops[pick[0]]) + QString::number(v, 'g', 12);
    }

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        int k = e->key();
        message->clear();
        if (k == Qt::Key_Escape) { reject(); return; }
        if (k == Qt::Key_Return || k == Qt::Key_Enter)
        {
            if (numberText().isEmpty()) { message->setText("Enter a number first"); return; }
            if (result().isEmpty())     { message->setText("Not a number"); return; }
            if (pick[0] == 4 && numberText().toDouble() == 0.0) { message->setText("Cannot divide by zero"); return; }
            accept();
            return;
        }
        if (k == Qt::Key_Left && pos > 0) pos--;
        else if (k == Qt::Key_Right && pos < POS - 1) pos++;
        else if (k == Qt::Key_Up || k == Qt::Key_Down)
        {
            int step = (k == Qt::Key_Up) ? 1 : -1;
            if (pos == 0) pick[0] = (pick[0] - step + OPS) % OPS;      // Up goes up the list  = + - x /
            else { int n = choices(pos); setSpot(pos, (pick[pos] + step + n) % n); }   // Up: empty, 0..9, . (, -)
        }
        else if (k >= Qt::Key_0 && k <= Qt::Key_9)
        {
            if (pos == 0) pos = 1;
            setSpot(pos, k - Qt::Key_0 + 1);
            if (pos < POS - 1) pos++;
        }
        else if (k == Qt::Key_Period || k == Qt::Key_Comma)
        {
            if (pos == 0) pos = 1;
            setSpot(pos, DOT);
            if (pos < POS - 1) pos++;
        }
        else if (k == Qt::Key_Minus && pos == 1 && pick[0] == 0) { setSpot(1, MINUS); pos = 2; }
        else if (k == Qt::Key_Equal)    { pick[0] = 0; }
        else if (k == Qt::Key_Plus)     { pick[0] = 1; }
        else if (k == Qt::Key_Minus)    { pick[0] = 2; }
        else if (k == Qt::Key_Asterisk || k == Qt::Key_X) { pick[0] = 3; }
        else if (k == Qt::Key_Slash)    { pick[0] = 4; }
        else if (k == Qt::Key_Backspace || k == Qt::Key_Delete)
        {
            if (pos > 0) setSpot(pos, EMPTY);
        }
        else
        {
            QDialog::keyPressEvent(e);
            return;
        }
        refresh();
    }

private:
    static const int POS = 7;       // 0 = operation, 1..6 = number spots
    static const int OPS = 5;
    // number spot choices: 0 = empty, 1..10 = digits 0..9, 11 = '.', 12 = '-'
    static const int EMPTY = 0;
    static const int DOT = 11;
    static const int MINUS = 12;
    static const int CHOICES = 12;  // without '-'
    int pick[POS] = {1};            // operation starts at '+', number spots empty
    int pos = 0;
    QLabel *cell[3][POS];           // row 0 = what Up gives, 1 = current, 2 = what Down gives
    QLabel *message = nullptr;

    static QString opChar(int i) { static const char *c[OPS] = { "=", "+", "-", "x", "/" }; return c[(i + OPS) % OPS]; }
    static QString spotChar(int i, int n)
    {
        i = (i + n) % n;
        if (i == EMPTY) return QString();
        if (i == DOT)   return ".";
        if (i == MINUS) return "-";
        return QString::number(i - 1);
    }

    // '-' is only offered on the first spot while the operation is '='
    int choices(int p) const { return (p == 1 && pick[0] == 0) ? CHOICES + 1 : CHOICES; }

    // Only one '.': putting it somewhere empties the old one
    void setSpot(int p, int v)
    {
        pick[p] = v;
        if (v == DOT)
            for (int k = 1; k < POS; k++)
                if (k != p && pick[k] == DOT) pick[k] = EMPTY;
        if (v == MINUS && p != 1)
            pick[p] = EMPTY;
    }

    QString numberText() const   // empty spots are skipped
    {
        QString s;
        for (int k = 1; k < POS; k++) s += spotChar(pick[k], CHOICES + 1);
        return s;
    }

    void refresh()
    {
        if (pick[0] != 0 && pick[1] == MINUS)
            pick[1] = EMPTY;        // '-' only exists with '='
        for (int k = 0; k < POS; k++)
        {
            bool sel = (k == pos);
            QString up, now, down;
            if (k == 0)
            {
                up = opChar(pick[0] - 1); now = opChar(pick[0]); down = opChar(pick[0] + 1);
            }
            else
            {
                int n = choices(k);
                up = spotChar(pick[k] + 1, n); now = spotChar(pick[k], n); down = spotChar(pick[k] - 1, n);
                if (up.isEmpty())   up = "_";
                if (down.isEmpty()) down = "_";
            }
            bool empty = now.isEmpty();
            cell[0][k]->setText(sel ? up : QString());
            cell[1][k]->setText(empty ? "_" : now);
            cell[2][k]->setText(sel ? down : QString());
            cell[0][k]->setStyleSheet("QLabel { color: #808080; }");
            cell[2][k]->setStyleSheet("QLabel { color: #808080; }");
            QString style;
            if (sel)         style = "background-color: #2f7fd0; color: white; border-radius: 4px;";
            else if (k == 0) style = "color: #2f7fd0;";
            else if (empty)  style = "color: #b0b0b0;";
            else             style = "color: black;";
            cell[1][k]->setStyleSheet("QLabel { " + style + " }");
        }
    }
};

// Called from MainWindow::set_value() instead of the old typing box
QString r36sSetValueDialog(QWidget *parent, QTableWidget *table, bool *ok)
{
    Q_UNUSED(table)
    R36SSetValueDialog dlg(parent);
    bool accepted = dlg.exec() == QDialog::Accepted;
    if (ok) *ok = accepted;
    return accepted ? dlg.result() : QString();
}

// ===== R36S CSV viewer (desktop 2) =====
// Newest log opens by itself. While "Log to file" records, rows arrive straight from
// log_to_file (no waiting for the card), the newest row sits on the centre marker and the
// graphs scroll; only the rows in view are kept. When the recording stops, the finished log
// is read back from the card in full. Each graph side (A left, B right) is scaled to the whole log's min/max,
// so it never changes while you move or zoom; live, it only widens when a new extreme arrives.
// Keys: up/down list, left/right move (hold = faster), Enter (Y) or a click (A) = pick graph
// for the highlighted value, Space (Select) = older logs, +/- (R2/L2) = zoom, Esc (B) = close.
#include <QPainter>
#include <QMouseEvent>
#include <QFileInfo>
#include <QMap>
#include <QPointer>
#include <cfloat>
#include <functional>

struct R36SCsvData
{
    QString path;
    QStringList names;                 // columns after Time
    QVector<double> time;
    QVector<QVector<float> > cols;
    QVector<float> lo, hi;             // whole-log min/max per column
    QVector<int> dec;                  // decimals seen per column
    double t0 = 0;                     // time of the first row (kept when old rows are dropped)

    void clear() { path.clear(); names.clear(); time.clear(); cols.clear(); lo.clear(); hi.clear(); dec.clear(); t0 = 0; }

    // Live: drop the oldest rows, keeping the min/max already seen
    int trimTo(int keep)
    {
        int drop = rows() - keep;
        if (drop <= 0)
            return 0;
        time.remove(0, drop);
        for (QVector<float> &c : cols)
            c.remove(0, drop);
        return drop;
    }
    int rows() const { return time.size(); }

    void setNames(const QStringList &header)   // header starts with Time
    {
        names.clear();
        for (int i = 1; i < header.size(); i++)
        {
            QString n = header.at(i).trimmed();
            if (!n.isEmpty())
                names << n;
        }
        time.clear();
        cols = QVector<QVector<float> >(names.size());
        lo = QVector<float>(names.size(), FLT_MAX);
        hi = QVector<float>(names.size(), -FLT_MAX);
        dec = QVector<int>(names.size(), 0);
    }

    void addRow(const QStringList &f)          // f starts with Time
    {
        if (f.isEmpty() || names.isEmpty())
            return;
        bool ok = false;
        double t = f.at(0).trimmed().toDouble(&ok);
        if (!ok)
            return;
        if (time.isEmpty() && t0 == 0)
            t0 = t;
        time.append(t);
        for (int k = 0; k < names.size(); k++)
        {
            QString s = k + 1 < f.size() ? f.at(k + 1).trimmed() : QString();
            float v = s.toFloat(&ok);
            if (!ok)
                v = cols[k].isEmpty() ? 0.0f : cols[k].last();   // "--" or missing: repeat last
            else
            {
                int dot = s.indexOf('.');
                if (dot >= 0)
                    dec[k] = qMax(dec[k], qMin(3, s.length() - dot - 1));
                lo[k] = qMin(lo[k], v);
                hi[k] = qMax(hi[k], v);
            }
            cols[k].append(v);
        }
    }
};

static bool r36sCsvLoad(const QString &path, R36SCsvData &d)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    d.clear();
    d.path = path;
    d.setNames(QString::fromLatin1(f.readLine()).trimmed().split(','));
    while (!f.atEnd())
    {
        QString line = QString::fromLatin1(f.readLine()).trimmed();
        if (!line.isEmpty())
            d.addRow(line.split(','));
    }
    return true;
}

static R36SCsvData r36sCsvLive;                 // the recording in progress (or the last one)
static QElapsedTimer r36sCsvLiveClock;          // time since the last live row
static int r36sCsvLiveKeep = 600;               // rows kept while live (the viewer's window)
struct R36SCsvPick { int graph; char side; int color; };
static QHash<QString, R36SCsvPick> r36sCsvPicks;   // graph/side/colour picked by hand, by column name

static bool r36sCsvLiveActive()
{
    return r36sCsvLiveClock.isValid() && r36sCsvLiveClock.elapsed() < 1500;   // no row for 1.5 s = stopped
}

// Every fastecu_*.csv, newest first (the date is in the name)
static QStringList r36sCsvLogList()
{
    QStringList dirs;
    dirs << R36S_FILES_DIR + "datalogs";
    if (!r36sCsvLive.path.isEmpty())
        dirs << QFileInfo(r36sCsvLive.path).absolutePath();
    QMap<QString, QString> byName;
    for (const QString &dir : dirs)
    {
        QDir d(dir);
        for (const QString &f : d.entryList(QStringList() << "fastecu_*.csv", QDir::Files))
            if (!byName.contains(f))
                byName.insert(f, d.filePath(f));
    }
    QStringList out;
    for (auto it = byName.constEnd(); it != byName.constBegin();)
    {
        --it;
        out << it.value();
    }
    return out;
}

// Names on the display = ... lines of log_setup.txt, in order
static QStringList r36sCsvDisplayNames()
{
    QStringList out;
    QFile f(R36S_FILES_DIR + "log_setup.txt");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;
    while (!f.atEnd())
    {
        QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith('#') || !line.contains('='))
            continue;
        if (line.section('=', 0, 0).trimmed().toLower() == "display")
            out << line.section('=', 1).trimmed();
    }
    return out;
}

// The 6 line colours offered in the picker (all readable on FastECU's light background)
static const int R36S_CSV_COLORS = 6;
static QColor r36sCsvColor(int c)
{
    static const char *col[R36S_CSV_COLORS] = { "#1f6fd1", "#d1495b", "#2a9d4b", "#e07b00", "#7b4fc9", "#0f8b8d" };
    return QColor(col[((c % R36S_CSV_COLORS) + R36S_CSV_COLORS) % R36S_CSV_COLORS]);
}

// A plain widget that hands its painting and clicks to a callback
class R36SPaintBox : public QWidget
{
public:
    std::function<void(QPainter &, const QRect &)> onPaint;
    std::function<void(const QPoint &)> onClick;
    explicit R36SPaintBox(QWidget *parent) : QWidget(parent) { setFocusPolicy(Qt::NoFocus); }
protected:
    void paintEvent(QPaintEvent *) override { QPainter p(this); if (onPaint) onPaint(p, rect()); }
    void mousePressEvent(QMouseEvent *e) override { if (e->button() == Qt::LeftButton && onClick) onClick(e->pos()); }
};

class R36SCsvViewer : public QWidget
{
public:
    explicit R36SCsvViewer(QWidget *parent) : QWidget(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        top = new QLabel(this);
        top->setTextFormat(Qt::RichText);
        top->setContentsMargins(6, 3, 6, 3);
        layout->addWidget(top);
        split = new QSplitter(Qt::Horizontal, this);
        split->setHandleWidth(10);              // wide enough to grab with the stick cursor
        split->setChildrenCollapsible(false);
        list = new R36SPaintBox(split);
        graphs = new R36SPaintBox(split);
        list->setMinimumWidth(90);
        graphs->setMinimumWidth(160);
        split->addWidget(list);
        split->addWidget(graphs);
        split->setStretchFactor(1, 1);
        split->setSizes(QList<int>() << 200 << 440);
        layout->addWidget(split, 1);
        overlay = new R36SPaintBox(this);
        overlay->hide();

        list->onPaint = [this](QPainter &p, const QRect &r) { paintList(p, r); };
        graphs->onPaint = [this](QPainter &p, const QRect &r) { paintGraphs(p, r); };
        overlay->onPaint = [this](QPainter &p, const QRect &r) { paintOverlay(p, r); };
        // A (left click) acts on what the D-pad highlighted, wherever the stick cursor happens to be
        list->onClick = [this](const QPoint &) { openPicker(); };
        graphs->onClick = [this](const QPoint &) { openPicker(); };
        overlay->onClick = [this](const QPoint &) { activateOverlay(); };

        statusTimer = new QTimer(this);
        statusTimer->setInterval(1000);
        connect(statusTimer, &QTimer::timeout, this, [this]() { if (usingLive) finishLive(); });
    }

    // A new recording started (called from r36sCsvLiveStart)
    void liveStarted()
    {
        lastNewest = r36sCsvLive.path;
        if (pinned)
            return;
        usingLive = true;
        follow = true;
        win = 600;      // about 30 s at 20 rows a second
        pos = 0;
        pickMode = 0;
        applyAssignments();
        updateAll();
    }

    // A live row arrived (called from r36sCsvLiveRow)
    void liveRowAdded(int dropped)
    {
        if (!usingLive)
        {
            // the same recording carried on after a pause: go back to following it
            if (!pinned && QFileInfo(file.path).fileName() == QFileInfo(r36sCsvLive.path).fileName())
            {
                usingLive = true;
                follow = true;
                win = 600;
                applyAssignments();
            }
            else
                return;
        }
        pos -= dropped;
        clampPos();
        if (follow)
            pos = data().rows() - 1;
        if (!repaintPending && isVisible())
        {
            repaintPending = true;
            QTimer::singleShot(100, this, [this]() { repaintPending = false; updateAll(); });
        }
    }

protected:
    // Recording stopped: read the finished log from the card, whole drive on screen
    void finishLive()
    {
        if (!usingLive || r36sCsvLiveActive())
        {
            updateAll();
            return;
        }
        QString path = r36sCsvLive.path;
        lastNewest = path;
        pinned = false;
        openLog(path);
        updateAll();
    }

    void showEvent(QShowEvent *e) override
    {
        QWidget::showEvent(e);
        refresh();
        statusTimer->start();
    }

    void hideEvent(QHideEvent *e) override
    {
        statusTimer->stop();
        QWidget::hideEvent(e);
    }

    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        overlay->setGeometry(rect());
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        const int key = e->key();
        const bool enter = key == Qt::Key_Return || key == Qt::Key_Enter;
        const int n = data().names.size();
        if (pickMode == 1)   // graph / side picker
        {
            // up/down: Graph, Side, Colour, Accept/Cancel rows; left/right: change the choice on that row
            if (key == Qt::Key_Up)          pickRow = qMax(0, pickRow - 1);
            else if (key == Qt::Key_Down)   pickRow = qMin(3, pickRow + 1);
            else if (key == Qt::Key_Left || key == Qt::Key_Right)
            {
                int d = key == Qt::Key_Left ? -1 : 1;
                if (pickRow == 0)      pickGraph = qBound(0, pickGraph + d, 4);
                else if (pickRow == 1) pickSide = d < 0 ? 'A' : 'B';
                else if (pickRow == 2) pickColor = qBound(0, pickColor + d, R36S_CSV_COLORS - 1);
                else                   pickCancel = d > 0;
            }
            else if (enter)                 activateOverlay();
            else if (key == Qt::Key_Escape) pickMode = 0;
            else return QWidget::keyPressEvent(e);
            updateAll();
            return;
        }
        if (pickMode == 2)   // log list
        {
            if (key == Qt::Key_Up)        logSel = qMax(0, logSel - 1);
            else if (key == Qt::Key_Down) logSel = qMin(logs.size() - 1, logSel + 1);
            else if (enter)               openFromList(logSel);
            else if (key == Qt::Key_Escape || key == Qt::Key_Space) pickMode = 0;
            else return QWidget::keyPressEvent(e);
            updateAll();
            return;
        }
        if (key == Qt::Key_Up)                    sel = qMax(0, sel - 1);
        else if (key == Qt::Key_Down)             sel = qMin(n - 1, sel + 1);
        else if (key == Qt::Key_Left || key == Qt::Key_Right)
        {
            repeats = e->isAutoRepeat() ? repeats + 1 : 0;
            int step = qMax(1, win / (repeats > 8 ? 20 : 100));
            pos += key == Qt::Key_Left ? -step : step;
            clampPos();
            follow = usingLive && pos >= data().rows() - 1;
        }
        else if (enter)                           openPicker();
        else if (key == Qt::Key_Space)            openLogList();
        else if (key == Qt::Key_Plus || key == Qt::Key_Equal || key == Qt::Key_PageUp)
            zoom(1.0 / 1.5);
        else if (key == Qt::Key_Minus || key == Qt::Key_PageDown)
            zoom(1.5);
        else
            return QWidget::keyPressEvent(e);
        updateAll();
    }

private:
    R36SCsvData file;            // a log read from the card
    bool usingLive = false;      // showing r36sCsvLive instead of file
    bool pinned = false;         // an older log was picked by hand: don't jump to the newest
    bool follow = false;         // live: keep the newest row on the marker
    bool repaintPending = false;
    QString lastNewest;
    int sel = 0, pos = 0, win = 600, listTop = 0, repeats = 0;
    QVector<int> graph;          // per column: 0 = not shown, 1-4 = graph
    QVector<char> side;          // per column: 'A' or 'B'
    QVector<int> color;          // per column: 0-5, see r36sCsvColor
    int pickMode = 0;            // 0 none, 1 graph picker, 2 log list
    int pickGraph = 1;
    char pickSide = 'A';
    int pickColor = 0;
    int pickRow = 0;             // picker row with the focus: 0 graph, 1 side, 2 colour, 3 accept/cancel
    bool pickCancel = false;     // on row 3: Cancel highlighted instead of Accept
    QStringList logs;
    int logSel = 0, logTop = 0;
    QRect panel;
    QList<QPair<QRect, int> > hits;
    QLabel *top;
    QSplitter *split;
    R36SPaintBox *list, *graphs, *overlay;
    QTimer *statusTimer;

    const R36SCsvData &data() const { return usingLive ? r36sCsvLive : file; }
    int rowHeight() const { return fontMetrics().height() + 6; }
    void clampPos() { pos = qBound(0, pos, qMax(0, data().rows() - 1)); }

    void zoom(double f)
    {
        int rows = data().rows();
        win = qBound(20, int(win * f + 0.5), usingLive ? 6000 : qMax(40, rows * 2));
        if (usingLive)
            r36sCsvLiveKeep = win;
        if (follow)
            pos = rows - 1;
    }

    void refresh()
    {
        if (r36sCsvLiveActive() && !pinned)
        {
            if (!usingLive)
            {
                usingLive = true;
                win = 600;
                applyAssignments();
            }
            follow = true;
            pos = data().rows() - 1;
            clampPos();
            updateAll();
            return;
        }
        if (usingLive)
        {
            finishLive();
            return;
        }
        QStringList all = r36sCsvLogList();
        QString newest = all.value(0);
        if (!pinned && !newest.isEmpty() && newest != lastNewest)
        {
            lastNewest = newest;
            openLog(newest);
        }
        updateAll();
    }

    void openLog(const QString &path)
    {
        r36sCsvLoad(path, file);
        usingLive = false;
        follow = false;
        int rows = file.rows();
        win = qMax(20, rows);        // whole log on screen first
        pos = rows / 2;
        applyAssignments();
    }

    void openFromList(int i)
    {
        QString path = logs.value(i);
        pickMode = 0;
        if (path.isEmpty())
            return;
        bool isLive = r36sCsvLiveActive() &&
                      QFileInfo(r36sCsvLive.path).fileName() == QFileInfo(path).fileName();
        pinned = i != 0 && !isLive;
        if (isLive)
        {
            usingLive = true;
            follow = true;
            win = 600;
            applyAssignments();
            pos = data().rows() - 1;
        }
        else
            openLog(path);
        lastNewest = logs.value(0);
    }

    // Display values first (2 per graph, A then B), then anything picked by hand this session
    void applyAssignments()
    {
        const R36SCsvData &d = data();
        int n = d.names.size();
        graph = QVector<int>(n, 0);
        side = QVector<char>(n, 'A');
        color = QVector<int>(n, 0);
        for (int k = 0; k < n; k++)
            color[k] = k % R36S_CSV_COLORS;
        QList<int> show;
        for (const QString &want : r36sCsvDisplayNames())
            for (int k = 0; k < n; k++)
                if (!show.contains(k) && d.names.at(k).compare(want, Qt::CaseInsensitive) == 0)
                    show << k;
        if (show.isEmpty())   // names not matched (IDs in log_setup.txt): display values are written first
        {
            int count = r36sDisplayIds.isEmpty() ? 6 : r36sDisplayIds.size();
            for (int k = 0; k < n && k < count; k++)
                show << k;
        }
        for (int i = 0; i < show.size() && i < 6; i++)
        {
            graph[show.at(i)] = i / 2 + 1;
            side[show.at(i)] = (i % 2) ? 'B' : 'A';
            color[show.at(i)] = i;   // the 6 display values start with 6 different colours
        }
        for (int k = 0; k < n; k++)
        {
            auto it = r36sCsvPicks.constFind(d.names.at(k));
            if (it != r36sCsvPicks.constEnd())
            {
                graph[k] = it.value().graph;
                side[k] = it.value().side;
                color[k] = it.value().color;
            }
        }
        sel = qBound(0, sel, qMax(0, n - 1));
    }

    void openPicker()
    {
        if (data().names.isEmpty())
            return;
        pickGraph = graph.value(sel) ? graph.value(sel) : 1;
        pickSide = side.value(sel, 'A');
        pickColor = color.value(sel, 0);
        pickRow = 0;
        pickCancel = false;
        pickMode = 1;
        updateAll();
    }

    void applyPicker()
    {
        if (sel >= 0 && sel < graph.size())
        {
            graph[sel] = pickGraph;
            side[sel] = pickSide;
            color[sel] = pickColor;
            r36sCsvPicks.insert(data().names.at(sel), R36SCsvPick{ pickGraph, pickSide, pickColor });
        }
        pickMode = 0;
    }

    void openLogList()
    {
        logs = r36sCsvLogList();
        if (logs.isEmpty())
            return;
        logSel = 0;
        for (int i = 0; i < logs.size(); i++)
            if (QFileInfo(logs.at(i)).fileName() == QFileInfo(data().path).fileName())
                logSel = i;
        pickMode = 2;
        updateAll();
    }

    QString valueText(int k) const
    {
        const R36SCsvData &d = data();
        if (d.rows() == 0 || k < 0 || k >= d.cols.size())
            return "--";
        return QString::number(d.cols.at(k).at(qBound(0, pos, d.rows() - 1)), 'f', d.dec.at(k));
    }

    void updateAll()
    {
        const R36SCsvData &d = data();
        QString name = d.path.isEmpty() ? QString("No logs in %1datalogs").arg(R36S_FILES_DIR)
                                        : QFileInfo(d.path).fileName();
        QString text = name.toHtmlEscaped();
        if (d.rows() > 1)
        {
            double t0 = d.t0, span = d.time.last() - t0;
            double secsPerRow = (d.time.last() - d.time.first()) / (d.rows() - 1);
            text += QString("&nbsp;&nbsp;&middot;&nbsp;&nbsp;%1 s of %2 s&nbsp;&nbsp;&middot;&nbsp;&nbsp;window %3 s")
                        .arg(d.time.at(qBound(0, pos, d.rows() - 1)) - t0, 0, 'f', 1)
                        .arg(span, 0, 'f', 1)
                        .arg(win * secsPerRow, 0, 'f', 0);
        }
        if (usingLive)
            text += r36sCsvLiveActive() ? "&nbsp;&nbsp;<b style=\"color:#1a8f3c\">LIVE</b>"
                                        : "&nbsp;&nbsp;(recording stopped)";
        top->setText(text);
        overlay->setVisible(pickMode != 0);
        overlay->raise();
        list->update();
        graphs->update();
        overlay->update();
    }

    void paintList(QPainter &p, const QRect &r)
    {
        const R36SCsvData &d = data();
        p.fillRect(r, palette().base());
        QFontMetrics fm(font());
        int rh = rowHeight();
        int visible = qMax(1, r.height() / rh);
        if (sel < listTop) listTop = sel;
        if (sel >= listTop + visible) listTop = sel - visible + 1;
        listTop = qMax(0, listTop);
        int tagW = fm.horizontalAdvance("4B") + 8;
        for (int i = 0; i < visible && listTop + i < d.names.size(); i++)
        {
            int k = listTop + i;
            QRect row(r.x(), r.y() + i * rh, r.width(), rh);
            bool on = k == sel;
            if (on)
                p.fillRect(row, palette().highlight());
            QColor ink = on ? palette().highlightedText().color() : palette().text().color();
            QRect box(row.x() + 5, row.y() + (rh - 10) / 2, 10, 10);
            if (graph.value(k))
                p.fillRect(box, r36sCsvColor(color.value(k)));
            else
            {
                p.setPen(ink);
                p.drawRect(box.adjusted(0, 0, -1, -1));
            }
            QRect tag(row.right() - tagW, row.y(), tagW - 4, rh);
            if (graph.value(k))
            {
                p.setPen(ink);
                p.drawText(tag, Qt::AlignVCenter | Qt::AlignRight, QString("%1%2").arg(graph.value(k)).arg(side.value(k)));
            }
            QString val = valueText(k);
            int valW = fm.horizontalAdvance(val) + 8;
            QRect valR(tag.left() - valW, row.y(), valW, rh);
            p.setPen(ink);
            p.drawText(valR, Qt::AlignVCenter | Qt::AlignRight, val);
            QRect nameR(box.right() + 6, row.y(), valR.left() - box.right() - 8, rh);
            p.drawText(nameR, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(d.names.at(k), Qt::ElideRight, nameR.width()));
        }
    }

    void paintGraphs(QPainter &p, const QRect &r)
    {
        const R36SCsvData &d = data();
        p.fillRect(r, palette().base());
        QFont small = font();
        small.setPointSizeF(qMax(6.0, small.pointSizeF() - 1));
        p.setFont(small);
        QFontMetrics sfm(small);
        if (d.rows() == 0)
        {
            p.setPen(palette().text().color());
            p.drawText(r, Qt::AlignCenter, d.names.isEmpty() ? "No log loaded" : "Waiting for data");
            return;
        }
        int G = 1;
        for (int g : graph)
            G = qMax(G, g);
        const int L = sfm.horizontalAdvance("-00000") + 6, R = L, hh = sfm.height() + 4;
        const int rows = d.rows(), a = pos - win / 2;
        const int gh = r.height() / G;
        for (int g = 1; g <= G; g++)
        {
            int y0 = r.y() + (g - 1) * gh;
            QRect box(r.x(), y0, r.width(), g == G ? r.bottom() - y0 + 1 : gh);
            QRect head(box.x(), box.y(), box.width(), hh);
            QRect plot(box.x() + L, box.y() + hh + 2, box.width() - L - R, box.height() - hh - 6);
            if (g > 1)
            {
                p.setPen(palette().mid().color());
                p.drawLine(box.left(), box.top(), box.right(), box.top());
            }
            if (plot.width() < 10 || plot.height() < 10)
                continue;
            p.setPen(palette().mid().color());
            p.drawRect(plot.adjusted(0, 0, -1, -1));
            p.drawLine(plot.left(), plot.center().y(), plot.right(), plot.center().y());

            QList<int> ks;
            for (int pass = 0; pass < 2; pass++)
                for (int k = 0; k < graph.size(); k++)
                    if (graph.at(k) == g && side.at(k) == (pass ? 'B' : 'A'))
                        ks << k;
            if (ks.isEmpty())
            {
                p.setPen(palette().mid().color());
                p.drawText(head.adjusted(L, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, QString("Graph %1: empty").arg(g));
                continue;
            }
            // header: side, name and the value at the marker, in the line's colour
            int itemW = (head.width() - 8) / ks.size();
            for (int i = 0; i < ks.size(); i++)
            {
                int k = ks.at(i);
                bool isB = side.at(k) == 'B';
                QString pre = QString("%1: ").arg(side.at(k)), val = " " + valueText(k);
                int nameW = itemW - sfm.horizontalAdvance(pre + val) - 8;
                QString txt = pre + sfm.elidedText(d.names.at(k), Qt::ElideRight, qMax(10, nameW)) + val;
                p.setPen(r36sCsvColor(color.value(k)));
                // A values from the left (their scale is on the left), B values from the right
                QRect cell(head.x() + 4 + i * itemW, head.y(), itemW, head.height());
                p.drawText(cell, Qt::AlignVCenter | (isB ? Qt::AlignRight : Qt::AlignLeft), txt);
            }
            for (char s : { 'A', 'B' })
            {
                float lo = FLT_MAX, hi = -FLT_MAX;
                int first = -1, decs = 0;
                for (int k : ks)
                    if (side.at(k) == s)
                    {
                        if (first < 0) first = k;
                        lo = qMin(lo, d.lo.at(k));
                        hi = qMax(hi, d.hi.at(k));
                        decs = qMax(decs, d.dec.at(k));
                    }
                if (first < 0)
                    continue;
                if (lo > hi) { lo = 0; hi = 1; }
                if (hi - lo < 1e-6f) { hi += 1; lo -= 1; }
                float pad = (hi - lo) * 0.05f;
                lo -= pad;
                hi += pad;
                // scale labels: top, middle, bottom
                p.setPen(r36sCsvColor(color.value(first)));
                int ld = qMin(decs, 1);
                QRect lab = s == 'A' ? QRect(box.x(), plot.y(), L - 4, plot.height())
                                     : QRect(plot.right() + 4, plot.y(), R - 4, plot.height());
                Qt::Alignment h = s == 'A' ? Qt::AlignRight : Qt::AlignLeft;
                p.drawText(lab, h | Qt::AlignTop, QString::number(hi, 'f', ld));
                p.drawText(lab, h | Qt::AlignVCenter, QString::number((hi + lo) / 2, 'f', ld));
                p.drawText(lab, h | Qt::AlignBottom, QString::number(lo, 'f', ld));
                // lines
                p.setClipRect(plot.adjusted(1, 1, -1, -1));
                const int i0 = qMax(0, a), i1 = qMin(rows - 1, a + win);
                const double xs = double(plot.width() - 1) / win, ys = double(plot.height() - 1) / (hi - lo);
                for (int k : ks)
                {
                    if (side.at(k) != s || i1 < i0)
                        continue;
                    const QVector<float> &c = d.cols.at(k);
                    QPolygonF line;
                    if (i1 - i0 <= plot.width() * 2)
                        for (int i = i0; i <= i1; i++)
                            line << QPointF(plot.x() + (i - a) * xs, plot.bottom() - (c.at(i) - lo) * ys);
                    else   // more rows than pixels: min and max per pixel column
                    {
                        int px = -1;
                        float mn = 0, mx = 0;
                        for (int i = i0; i <= i1 + 1; i++)
                        {
                            int x = i <= i1 ? int(plot.x() + (i - a) * xs) : -2;
                            if (x != px)
                            {
                                if (px >= 0)
                                    line << QPointF(px, plot.bottom() - (mn - lo) * ys)
                                         << QPointF(px, plot.bottom() - (mx - lo) * ys);
                                if (i > i1)
                                    break;
                                px = x;
                                mn = mx = c.at(i);
                            }
                            else
                            {
                                mn = qMin(mn, c.at(i));
                                mx = qMax(mx, c.at(i));
                            }
                        }
                    }
                    p.setPen(QPen(r36sCsvColor(color.value(k)), 1.5));
                    p.drawPolyline(line);
                }
                p.setClipping(false);
            }
        }
        // centre marker: the values in the list and the headers are read here
        int mx = r.x() + L + (r.width() - L - R) / 2;
        QPen dash(palette().text().color(), 1, Qt::DashLine);
        p.setPen(dash);
        p.drawLine(mx, r.top(), mx, r.bottom());
    }

    void drawOption(QPainter &p, const QRect &b, const QString &text, bool on, int id)
    {
        if (on)
        {
            p.fillRect(b, palette().highlight());
            p.setPen(palette().highlightedText().color());
        }
        else
        {
            p.setPen(palette().mid().color());
            p.drawRect(b.adjusted(0, 0, -1, -1));
            p.setPen(palette().text().color());
        }
        p.drawText(b, Qt::AlignCenter, text);
        hits << qMakePair(b, id);
    }

    void paintOverlay(QPainter &p, const QRect &r)
    {
        hits.clear();
        p.fillRect(r, QColor(0, 0, 0, 70));
        QFontMetrics fm(font());
        int lh = fm.height() + 10;
        if (pickMode == 1)
        {
            int w = qMin(r.width() - 20, 440), h = lh * 6 + 12;
            panel = QRect(r.center().x() - w / 2, r.center().y() - h / 2, w, h);
            p.fillRect(panel, palette().window());
            p.setPen(palette().dark().color());
            p.drawRect(panel.adjusted(0, 0, -1, -1));
            QRect line(panel.x() + 10, panel.y() + 6, w - 20, lh);
            p.setPen(palette().text().color());
            p.drawText(line, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(data().names.value(sel), Qt::ElideRight, line.width()));
            int bx = line.x() + fm.horizontalAdvance("Colour") + 14;
            // the row with the focus gets a light band and a marker, so up/down is easy to follow
            QColor band = palette().highlight().color();
            band.setAlpha(50);
            auto rowLabel = [&](int row, const QString &text) {
                line.translate(0, lh);
                if (pickRow == row)
                {
                    p.fillRect(QRect(panel.x() + 2, line.y(), panel.width() - 4, lh), band);
                    p.fillRect(QRect(panel.x() + 2, line.y() + 3, 3, lh - 6), palette().highlight());
                }
                p.setPen(palette().text().color());
                p.drawText(line, Qt::AlignVCenter | Qt::AlignLeft, text);
            };
            rowLabel(0, "Graph");
            int bw = (line.right() - bx) / 5;
            for (int g = 0; g <= 4; g++)
                drawOption(p, QRect(bx + g * bw, line.y() + 3, bw - 4, lh - 6), g ? QString::number(g) : "Off", pickGraph == g, g);
            rowLabel(1, "Side");
            bw = (line.right() - bx) / 2;
            drawOption(p, QRect(bx, line.y() + 3, bw - 4, lh - 6), "A  left", pickSide == 'A', 10);
            drawOption(p, QRect(bx + bw, line.y() + 3, bw - 4, lh - 6), "B  right", pickSide == 'B', 11);
            rowLabel(2, "Colour");
            bw = (line.right() - bx) / R36S_CSV_COLORS;
            for (int c = 0; c < R36S_CSV_COLORS; c++)
            {
                QRect sw(bx + c * bw, line.y() + 3, bw - 4, lh - 6);
                p.fillRect(sw, r36sCsvColor(c));
                if (pickColor == c)   // chosen: thick outline in the text colour
                {
                    p.setPen(QPen(palette().text().color(), 3));
                    p.drawRect(sw.adjusted(1, 1, -2, -2));
                }
            }
            line.translate(0, lh);
            if (pickRow == 3)
                p.fillRect(QRect(panel.x() + 2, line.y(), panel.width() - 4, lh), band);
            int half = (line.width() - 4) / 2;
            drawOption(p, QRect(line.x(), line.y() + 3, half, lh - 6), "Accept", pickRow == 3 && !pickCancel, 20);
            drawOption(p, QRect(line.x() + half + 4, line.y() + 3, half, lh - 6), "Cancel", pickRow == 3 && pickCancel, 21);
            line.translate(0, lh);
            QFont hint = font();
            hint.setPointSizeF(qMax(6.0, hint.pointSizeF() - 2));
            p.setFont(hint);
            p.setPen(palette().dark().color());
            p.drawText(line, Qt::AlignVCenter | Qt::AlignLeft, "Up/down: row   Left/right: choose   Y: accept   B: cancel");
            p.setFont(font());
        }
        else if (pickMode == 2)
        {
            panel = r.adjusted(20, 20, -20, -20);
            p.fillRect(panel, palette().window());
            p.setPen(palette().dark().color());
            p.drawRect(panel.adjusted(0, 0, -1, -1));
            QRect line(panel.x() + 10, panel.y() + 4, panel.width() - 20, lh);
            p.setPen(palette().text().color());
            p.drawText(line, Qt::AlignVCenter | Qt::AlignLeft, "Logs, newest first");
            int visible = qMax(1, (panel.height() - 2 * lh - 8) / lh);
            if (logSel < logTop) logTop = logSel;
            if (logSel >= logTop + visible) logTop = logSel - visible + 1;
            for (int i = 0; i < visible && logTop + i < logs.size(); i++)
            {
                int j = logTop + i;
                QRect row(line.x(), line.y() + (i + 1) * lh, line.width(), lh);
                QFileInfo fi(logs.at(j));
                bool live = r36sCsvLiveActive() && fi.fileName() == QFileInfo(r36sCsvLive.path).fileName();
                QString text = fi.fileName() + (live ? "   (recording)" : QString("   %1 KB").arg((fi.size() + 1023) / 1024));
                if (j == logSel)
                {
                    p.fillRect(row, palette().highlight());
                    p.setPen(palette().highlightedText().color());
                }
                else
                    p.setPen(palette().text().color());
                p.drawText(row.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
                hits << qMakePair(row, 100 + j);
            }
            p.setPen(palette().dark().color());
            p.drawText(QRect(line.x(), panel.bottom() - lh, line.width(), lh), Qt::AlignVCenter | Qt::AlignLeft,
                       "Up/down: choose   Y: open   B: close");
        }
    }

    // Y (Enter) or A (click, anywhere): accept the picker, or Cancel if it is highlighted;
    // in the log list, open the highlighted log
    void activateOverlay()
    {
        if (pickMode == 1)
        {
            if (pickRow == 3 && pickCancel)
                pickMode = 0;
            else
                applyPicker();
        }
        else if (pickMode == 2)
            openFromList(logSel);
        updateAll();
    }
};

static QPointer<R36SCsvViewer> r36sCsvViewer;

static QWidget *r36sCsvViewerCreate(QWidget *parent)
{
    r36sCsvViewer = new R36SCsvViewer(parent);
    return r36sCsvViewer;
}

// Called from log_to_file when a new CSV is opened (header starts with Time)
void r36sCsvLiveStart(const QString &path, const QStringList &header)
{
    r36sCsvLive.clear();
    r36sCsvLive.path = path;
    r36sCsvLive.setNames(header);
    r36sCsvLiveKeep = 600;
    r36sCsvLiveClock.start();
    if (r36sCsvViewer)
        r36sCsvViewer->liveStarted();
}

// Called from log_to_file for every row written to the CSV
void r36sCsvLiveRow(const QStringList &row)
{
    if (r36sCsvLive.names.isEmpty())
        return;
    r36sCsvLive.addRow(row);
    int dropped = r36sCsvLive.rows() > r36sCsvLiveKeep + 200 ? r36sCsvLive.trimTo(r36sCsvLiveKeep) : 0;
    r36sCsvLiveClock.restart();
    if (r36sCsvViewer)
        r36sCsvViewer->liveRowAdded(dropped);
}
// ===== end R36S CSV viewer =====


// ===== R36S pointer: hides when not in use =====
// The stick cursor hides as soon as the D-pad (or Y) is used, or after 3 s without stick
// movement. While it is hidden, A, X and L3 clicks are ignored, so a click can never land
// on whatever the hidden pointer is resting on. Moving the stick brings it back where it was.
// The right stick (scroll wheel) works either way. Y always acts on the selection.
static const int R36S_POINTER_IDLE_MS = 3000;

class R36SPointerHider : public QObject
{
public:
    explicit R36SPointerHider(QObject *parent) : QObject(parent)
    {
        idle = new QTimer(this);
        idle->setSingleShot(true);
        idle->setInterval(R36S_POINTER_IDLE_MS);
        connect(idle, &QTimer::timeout, this, [this]() { hide(); });
        last = QCursor::pos();
        idle->start();
    }
protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        Q_UNUSED(obj)
        switch (event->type())
        {
        case QEvent::MouseMove:
        {
            QPoint now = QCursor::pos();
            if (now != last)             // real stick movement, not a synthetic move
            {
                last = now;
                show();
                idle->start();
            }
            break;
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
            if (hidden)
            {
                swallowed = true;
                return true;             // pointer hidden: the click is ignored
            }
            idle->start();
            break;
        case QEvent::MouseButtonRelease:
            if (swallowed)
            {
                if (QGuiApplication::mouseButtons() == Qt::NoButton)
                    swallowed = false;
                return true;             // the matching release of an ignored click
            }
            idle->start();
            break;
        case QEvent::KeyPress:
        {
            int key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Left || key == Qt::Key_Right ||
                key == Qt::Key_Return || key == Qt::Key_Enter)
                hide();                  // D-pad or Y: keys are in charge now
            break;
        }
        default:
            break;
        }
        return false;
    }
private:
    QTimer *idle;
    QPoint last;
    bool hidden = false, swallowed = false;

    void hide()
    {
        if (hidden || QGuiApplication::mouseButtons() != Qt::NoButton)   // never in the middle of a drag
            return;
        hidden = true;
        QGuiApplication::setOverrideCursor(Qt::BlankCursor);
    }
    void show()
    {
        if (!hidden)
            return;
        hidden = false;
        QGuiApplication::restoreOverrideCursor();
    }
};
// ===== end R36S pointer =====



// ===== R36S desktops (L1 / R1) =====
// L1 and R1 send Shift+Tab and Tab. On the FastECU screen and on the other desktops they
// switch desktops; in dialogs (Log values, Set value, Settings ...) they stay Tab / Shift+Tab.
// Desktops 2-4 are pages in one separate full-screen window with no FastECU menu, toolbar or
// status bar, so FastECU's shortcuts (Start, FN+Start, Connect ...) do not reach them.
// Switching never opens or closes a window: pages flip inside that window, and going to or
// from FastECU only raises one window above the other. Logging and CSV keep running.
#include <QApplication>
#include <QPointer>
#include <QStackedWidget>

static const char *R36S_DESKTOP_NAMES[] = { "FastECU", "CSV viewer", "2D / 3D", "Arduino control" };
static const int R36S_DESKTOP_COUNT = 4;

static QPointer<QMainWindow> r36sDeskMain;       // desktop 1 = FastECU main window
static QPointer<QWidget> r36sDeskOverlay;        // one window holding desktops 2-4
static QPointer<QStackedWidget> r36sDeskStack;   // its pages: index 0 = desktop 2

// Fill one desktop. Each later step replaces the placeholder for its own desktop here.
static void r36sDesktopBuild(int index, QWidget *page)
{
    QVBoxLayout *layout = new QVBoxLayout(page);
    QLabel *title = new QLabel(R36S_DESKTOP_NAMES[index], page);
    QFont font = title->font();
    font.setPointSize(24);
    font.setBold(true);
    title->setFont(font);
    title->setAlignment(Qt::AlignCenter);
    QLabel *info = new QLabel(QString("Desktop %1 of %2\n\nComing soon\n\nL1 / R1: switch desktops")
                                  .arg(index + 1).arg(R36S_DESKTOP_COUNT), page);
    info->setAlignment(Qt::AlignCenter);
    layout->addStretch();
    layout->addWidget(title);
    layout->addWidget(info);
    layout->addStretch();
}

static QWidget *r36sDesktopOverlay()
{
    if (!r36sDeskOverlay)
    {
        // No parent, so it can go below FastECU; it does not keep FastECU open when FastECU closes
        QWidget *w = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint);
        w->setObjectName("r36sDesktops");
        w->setWindowTitle("FastECU desktops");
        w->setAttribute(Qt::WA_QuitOnClose, false);
        w->setFocusPolicy(Qt::StrongFocus);
        w->setAutoFillBackground(true);   // FastECU's own colours
        QVBoxLayout *layout = new QVBoxLayout(w);
        layout->setContentsMargins(0, 0, 0, 0);
        QStackedWidget *stack = new QStackedWidget(w);
        for (int i = 1; i < R36S_DESKTOP_COUNT; i++)
        {
            QWidget *page = i == 1 ? r36sCsvViewerCreate(stack) : new QWidget(stack);   // desktop 2 = CSV viewer
            if (i != 1)
                r36sDesktopBuild(i, page);
            stack->addWidget(page);
        }
        layout->addWidget(stack);
        r36sDeskOverlay = w;
        r36sDeskStack = stack;
    }
    return r36sDeskOverlay;
}

// Which desktop a window is: 0 = FastECU, 1-3 = the others, -1 = a dialog or anything else
static int r36sDesktopIndexOf(QWidget *w)
{
    if (!w)
        return -1;
    if (w == r36sDeskMain)
        return 0;
    if (r36sDeskOverlay && w == r36sDeskOverlay && r36sDeskStack)
        return r36sDeskStack->currentIndex() + 1;
    return -1;
}

static void r36sDesktopShow(int current, int index)
{
    QMainWindow *win = r36sDeskMain;
    if (!win)
        return;
    index = ((index % R36S_DESKTOP_COUNT) + R36S_DESKTOP_COUNT) % R36S_DESKTOP_COUNT;
    if (index == current)
        return;
    if (index == 0)
    {
        win->raise();
        win->activateWindow();
        if (r36sDeskOverlay)
            r36sDeskOverlay->lower();   // stays open behind FastECU
        return;
    }
    QWidget *overlay = r36sDesktopOverlay();
    r36sDeskStack->setCurrentIndex(index - 1);   // flip the page in place
    r36sDeskStack->currentWidget()->setFocus();   // keys go to that desktop
    if (current == 0)
    {
        overlay->setGeometry(win->geometry());   // same place and size as FastECU (full screen on the R36S)
        if (!overlay->isVisible())
            overlay->show();                     // only the first time
        overlay->raise();
        overlay->activateWindow();
        r36sDeskStack->currentWidget()->setFocus();
    }
}

class R36SDesktopSwitcher : public QObject
{
public:
    explicit R36SDesktopSwitcher(QObject *parent) : QObject(parent) {}
protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        Q_UNUSED(obj)
        if (event->type() != QEvent::KeyPress)
            return false;
        QKeyEvent *ke = static_cast<QKeyEvent*>(event);
        if (ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier))
            return false;
        bool prev = ke->key() == Qt::Key_Backtab ||
                    (ke->key() == Qt::Key_Tab && (ke->modifiers() & Qt::ShiftModifier));
        bool next = ke->key() == Qt::Key_Tab && !prev;
        if (!next && !prev)
            return false;
        if (QApplication::activeModalWidget() || QApplication::activePopupWidget())
            return false;                       // dialog or open list: normal Tab
        int current = r36sDesktopIndexOf(QApplication::activeWindow());
        if (current < 0)
            return false;                       // non-desktop window: normal Tab
        if (!ke->isAutoRepeat() && !r36sEcuBusy)
            r36sDesktopShow(current, current + (next ? 1 : -1));
        return true;                            // on a desktop Tab never moves focus
    }
};

// Called once from the MainWindow constructor
void r36sDesktopsInstall(QMainWindow *win)
{
    r36sDeskMain = win;
    QObject::connect(win, &QObject::destroyed, []() { delete r36sDeskOverlay.data(); });
    qApp->installEventFilter(new R36SDesktopSwitcher(win));
    qApp->installEventFilter(new R36SPointerHider(win));   // R36S: pointer hides when not in use
}
// ===== end R36S desktops =====


// ===== end R36S Set value window =====

