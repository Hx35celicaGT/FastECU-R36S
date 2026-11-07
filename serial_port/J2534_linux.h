#ifndef J2534_LINUX_H
#define J2534_LINUX_H

#include <QByteArray>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QTime>
#include <QElapsedTimer>   // R36S AUX Logging

#include <stdint.h>

#include "J2534_tactrix_linux.h"

class J2534 : public QWidget
{
    Q_OBJECT

signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

public:
    explicit J2534();
    ~J2534();

    const char *DLL_VERSION = "3.0.0";
    const char *API_VERSION = "04.04";

    bool serial_port_protocol_iso14230 = false;
    bool J2534_init_ok = false;

    bool is_serial_port_open();

    bool init() { return true; };
    void setDllName(const char* name) {}; // For Win/Linux compatibility only
    void getDllName(char* name) {}; // For Win/Linux compatibility only
    void debug(bool enable) { debugMode = enable; };

    long PassThruOpen(const void *pName, unsigned long *pDeviceID);
    long PassThruClose(unsigned long DeviceID);
    long PassThruConnect(unsigned long DeviceID, unsigned long ProtocolID, unsigned long Flags, unsigned long Baudrate, unsigned long *pChannelID);
    long PassThruDisconnect(unsigned long ChannelID);
    long PassThruReadMsgs(unsigned long ChannelID, PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs, unsigned long Timeout);
    long PassThruWriteMsgs(unsigned long ChannelID, const PASSTHRU_MSG *pMsg, unsigned long *pNumMsgs, unsigned long Timeout);
    long PassThruStartPeriodicMsg(unsigned long ChannelID, const PASSTHRU_MSG *pMsg, unsigned long *pMsgID, unsigned long TimeInterval);
    long PassThruStopPeriodicMsg(unsigned long ChannelID, unsigned long MsgID);
    long PassThruStartMsgFilter(unsigned long ChannelID, unsigned long FilterType, const PASSTHRU_MSG *pMaskMsg, const PASSTHRU_MSG *pPatternMsg, const PASSTHRU_MSG *pFlowControlMsg, unsigned long *pMsgID);
    long PassThruStopMsgFilter(unsigned long ChannelID, unsigned long MsgID);
    long PassThruSetProgrammingVoltage(unsigned long DeviceID, unsigned long Pin, unsigned long Voltage);
    long PassThruReadVersion(char *pApiVersion, char *pDllVersion, char *pFirmwareVersion, unsigned long DeviceID);
    long PassThruGetLastError(char *pErrorDescription);
    long PassThruIoctl(unsigned long ChannelID, unsigned long IoctlID, const void *pInput, void *pOutput);

    QString open_serial_port(QString serial_port);
    void close_serial_port();
    QByteArray read_serial_data(uint32_t datalen, uint16_t timeout);
    int write_serial_data(QByteArray output);
    QByteArray write_serial_iso14230_data(QByteArray output);
    QString parseMessageToHex(QByteArray received);
    uint32_t parse_ts(const char *data);
    bool get_is_tx_done();

    // R36S: Tactrix 2.5 mm jack (channel 9, receive-only). Its bytes arrive mixed into the
    // adapter stream and are collected by PassThruReadMsgs while the K-line is read.
    bool jack_open = false;
    QByteArray take_jack_data();
    void close_jack();            // R36S: Logging off

private:
    bool debugMode;

    QString opened_serial_port;
    QString serial_port_baudrate = "4800";

    uint16_t receive_timeout = 500;
    uint16_t serial_read_timeout = 2000;
    uint16_t serial_read_extra_short_timeout = 50;
    uint16_t serial_read_short_timeout = 200;
    uint16_t serial_read_medium_timeout = 500;
    uint16_t serial_read_long_timeout = 800;
    uint16_t serial_read_extra_long_timeout = 3000;

    QSerialPort *serial = new QSerialPort();
    unsigned long periodic_msg_id;

    bool msg_ack = false;
    bool is_tx_done = false;
    QByteArray jack_rx;           // R36S: jack bytes not yet taken
    bool r36s_read_line(QByteArray &buf, uint8_t end, int max, unsigned long timeout);    

    enum rx_msg_type {
        NORM_MSG,
        TX_DONE_MSG = 0x10,
        TX_LB_MSG = 0x20,
        RX_MSG_END_IND = 0x40,
        EXT_ADDR_MSG_END_IND = 0x44,
        LB_MSG_END_IND = 0x60,
        NORM_MSG_START_IND = 0x80,
        TX_LB_START_IND = 0xA0,
    };

    int is_valid_sconfig_param(SCONFIG s);
    void dump_sbyte_array(const SBYTE_ARRAY* s);
    void dump_sconfig_param(SCONFIG s);

    void delay(int n);

private slots:
    void handle_error(QSerialPort::SerialPortError error);

};

#endif // J2534_LINUX_H
