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
        int mpx = qBound(8, h * 14 / 100, 13);
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
        minmax->setStyleSheet("color: grey;");
        value->setTextFormat(Qt::RichText);
        for (QLabel *l : {title, value, minmax})
            l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
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
                       "px; color:grey;\">" + r36sUnit(lv, j).toHtmlEscaped() + "</span>");
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
            datalog_file_outstream << "Time,";
            for (int j = 0; j < 0 /* R36S: gauge columns not logged */ ; j++){
                datalog_file_outstream << logValues->log_value_name.value(logValues->log_value_id.indexOf(logValues->dashboard_log_value_id.at(j), 0)) << ",";
            }
            for (int j = 0; j < logValues->lower_panel_log_value_id.count() ; j++){
                datalog_file_outstream << logValues->log_value_name.value(logValues->log_value_id.indexOf(logValues->lower_panel_log_value_id.at(j), 0)) << ",";
            }
            for (int j = 0; j < logValues->lower_panel_switch_id.count() /* R36S: switches from log_setup.txt */ ; j++){
                datalog_file_outstream << logValues->log_switch_name.value(logValues->log_switch_id.indexOf(logValues->lower_panel_switch_id.at(j),0)) << ",";
            }
            datalog_file_outstream << "\n";
        }
        else{

            datalog_file_outstream << QString::number(log_file_timer->elapsed() / 1000.0f) << ",";
            for (int j = 0; j < 0 /* R36S: gauge columns not logged */ ; j++){
                datalog_file_outstream << logValues->log_value.value(logValues->log_value_id.indexOf(logValues->dashboard_log_value_id.at(j), 0)) << ",";
            }
            for (int j = 0; j < logValues->lower_panel_log_value_id.count() ; j++){
                datalog_file_outstream << logValues->log_value.value(logValues->log_value_id.indexOf(logValues->lower_panel_log_value_id.at(j), 0)) << ",";
            }
            for (int j = 0; j < logValues->lower_panel_switch_id.count() /* R36S: switches from log_setup.txt */ ; j++){
                datalog_file_outstream << logValues->log_switch_state.value(logValues->log_switch_id.indexOf(logValues->lower_panel_switch_id.at(j), 0)) << ",";
            }
            datalog_file_outstream << "\n";
        }
    }
}

