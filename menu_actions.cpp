// Modified for FastECU-R36S (R36S handheld running Arch R) by Victor, 2026-09 to 2026-10.
// See CHANGES-R36S.md for what changed. Original FastECU: https://github.com/miikasyvanen/fastecu-oem (GPLv3).
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "serial_port_actions.h"

// R36S: defined in log_operations_ssm.cpp
bool r36sGuardAction(QMainWindow *win, QMenuBar *bar, const QString &action, bool loggingState, bool writeCsv);
void r36sShowCsvState(QMainWindow *win, bool armed, bool recording);
void r36sSetMenuChecked(QMenuBar *bar, const QString &text, bool checked);
QString r36sSetValueDialog(QWidget *parent, QTableWidget *table, bool *ok);

void MainWindow::menu_action_triggered(QString action)
{
        // R36S: ignore Logging/CSV/Connect/Disconnect during a ROM read/write, and Start while the CSV records
    if (r36sGuardAction(this, ui->menubar, action, logging_state, write_datalog_to_file))
        return;
    // FILE MENU
    if (action == "new"){
        qDebug() << action;}
    if (action == "open_calibration")
        open_calibration_file(NULL);
    if (action == "save_calibration")
        save_calibration_file();
    if (action == "save_calibration_as")
        save_calibration_file_as();
    if (action == "close_calibration")
        close_calibration();
    if (action == "quit")
        close_app();

    // EDIT MENU
    if (action == "undo")
        qDebug() << action;
    if (action == "redo")
        qDebug() << action;
    if (action == "copy")
        copy_value();
    if (action == "paste")
        paste_value();
    if (action == "winols_csv_to_romraider_xml")
        winols_csv_to_romraider_xml();
    if (action == "settings")
        show_preferences_window();

    // TUNE MENU
    if (action == "fine_inc" || action == "fine_dec" || action == "coarse_inc" || action == "coarse_dec")
        inc_dec_value(action);
    if (action == "set_value")
        set_value();
    if (action == "interpolate_horizontal" || action == "interpolate_vertical" || action == "interpolate_bidirectional")
        interpolate_value(action);
    if (action == "toggle_realtime")
        toggle_realtime();
    if (action == "log_to_file")
        toggle_log_to_file();

    // ECU MENU
    if (action == "connect_to_ecu")
        connect_to_ecu();
    if (action == "disconnect_from_ecu")
        disconnect_from_ecu();
    if (action == "read_rom_from_ecu")
        start_ecu_operations("read");
    if (action == "test_write_rom_to_ecu")
        start_ecu_operations("test_write");
    if (action == "write_rom_to_ecu")
        start_ecu_operations("write");

    // VIEW MENU
    if (action == "setlogviews")
        change_gauge_values();

    // TESTING MENU
    if (action == "dtc_window")
        show_dtc_window();
    if (action == "hex_editor")
        show_hex_editor();

    if (action == "haltech_ic7")
        toggle_haltech_ic7_display();
    if (action == "simulate_obd")
        toggle_simulate_obd();
    if (action == "can_listener")
        toggle_can_listener();
    if (action == "biu_communication")
        show_subaru_biu_window();
    if (action == "get_key")
        show_subaru_get_key_window();
    if (action == "terminal")
        show_terminal_window();

    // HELP MENU
    if (action == "about")
        QMessageBox::information(this, tr("FastECU"),       "FastECU is open source tuning software for Subaru ECUs,\n"
                                                            "TCUs and also modifying BIU and ECUs of other car makes.\n"
                                                            "\n"
                                                            "This is beta test version for read and write ROMs via\n"
                                                            "K-Line and CAN connection with Open Port 2.0 or generic\n"
                                                            "OBD2 cable. Software is tested in Win7/Win10 32/64bit\n"
                                                            "and Linux amd64 and aarch64 platforms.\n"
                                                            "\n"
                                                            "There WILL be bugs and things that don't work. Be patient\n"
                                                            "with new versions relesed.\n"
                                                            "\n"
                                                            "All liability lies with the user. We are not responsible any\n"
                                                            "harm, laws broken or bricked ECUs that can follow for using\n"
                                                            "this software.\n"
                                                            "\n"
                                                            "\n"
                                                            "Huge thanks to following:\n"
                                                            "\n"
                                                            "fenugrec - author of nisprog software\n"
                                                            "rimwall - modifier of nisprog kernels for Subaru use\n"
                                                            "SergArb - testing and software development\n"
                                                            "alesv - testing and software development\n"
                                                            "jimihimisimi - testing and software development\n"
                                                            "\n"
                                                            "...and to all of you who had support software development by\n"
                                                            "donating! All, even the smallest amount of donates are welcome!\n"
                                 );
}

void MainWindow::inc_dec_value(QString action)
{
    union map_data{
        int8_t sbyte_value[4];
        int16_t sword_value[2];
        int32_t sdword_value;
        uint8_t byte_value[4];
        uint16_t word_value[2];
        uint32_t dword_value;
        float float_value;
    } map_data_value;

    int rom_number = 0;
    int map_number = 0;
    QString map_name = "";
    QString map_type = "";
    bool bStatus;

    //qDebug() << "Inc/dec value:" << action;
    QMdiSubWindow* w = ui->mdiArea->activeSubWindow();
    if (w)
    {
        QStringList mapWindowString = w->objectName().split(",");
        rom_number = mapWindowString.at(0).toInt();
        map_number = mapWindowString.at(1).toInt();
        map_name = mapWindowString.at(2);
        map_type = mapWindowString.at(3);

        QTableWidget* mapTableWidget = w->findChild<QTableWidget*>(w->objectName());
        if (mapTableWidget)
        {
            emit LOG_D("Map " + ecuCalDef[rom_number]->NameList.at(map_number) + " scaling " + ecuCalDef[rom_number]->MapScalingNameList.at(map_number) + " min / max: " + ecuCalDef[rom_number]->MinValueList.at(map_number) + " / " + ecuCalDef[rom_number]->MaxValueList.at(map_number), true, true);
            float map_coarse_inc_value = ecuCalDef[rom_number]->CoarseIncList[map_number].toFloat();
            float map_fine_inc_value = ecuCalDef[rom_number]->FineIncList[map_number].toFloat();
            QStringList map_data_cell_text = ecuCalDef[rom_number]->MapData.at(map_number).split(",");
            QString map_format = ecuCalDef[rom_number]->FormatList[map_number];
            QString map_value_to_byte = ecuCalDef[rom_number]->ToByteList[map_number];
            QString map_value_from_byte = ecuCalDef[rom_number]->FromByteList[map_number];
            QString map_value_storagetype = ecuCalDef[rom_number]->StorageTypeList[map_number];
            QString map_value_endian = ecuCalDef[rom_number]->EndianList[map_number];
            QString map_min_value = ecuCalDef[rom_number]->MinValueList[map_number];
            QString map_max_value = ecuCalDef[rom_number]->MaxValueList[map_number];
            uint32_t map_data_address = ecuCalDef[rom_number]->AddressList[map_number].toUInt(&bStatus, 16);
            int mapXSize = ecuCalDef[rom_number]->XSizeList[map_number].toInt();
            int mapYSize = ecuCalDef[rom_number]->YSizeList[map_number].toInt();

            if (!mapTableWidget->selectedRanges().isEmpty())
            {
                QList<QTableWidgetSelectionRange> selected_range = mapTableWidget->selectedRanges();
                int first_col = selected_range.begin()->leftColumn() - 1;
                int first_row = selected_range.begin()->topRow() - 1;
                int last_col = selected_range.begin()->rightColumn() - 1;
                int last_row = selected_range.begin()->bottomRow() - 1;

                if (selected_range.begin()->leftColumn() == 0 && mapYSize > 1){
                    if (ecuCalDef[rom_number]->XScaleTypeList.at(map_number) == "Static Y Axis" || ecuCalDef[rom_number]->XScaleTypeList.at(map_number) == "Static X Axis")
                        return;
                    map_data_cell_text = ecuCalDef[rom_number]->YScaleData.at(map_number).split(",");
                    map_coarse_inc_value = ecuCalDef[rom_number]->YScaleCoarseIncList[map_number].toFloat();
                    map_fine_inc_value = ecuCalDef[rom_number]->YScaleFineIncList[map_number].toFloat();
                    map_format = ecuCalDef[rom_number]->YScaleFormatList[map_number];
                    map_value_to_byte = ecuCalDef[rom_number]->YScaleToByteList[map_number];
                    map_value_from_byte = ecuCalDef[rom_number]->YScaleFromByteList[map_number];
                    map_value_storagetype = ecuCalDef[rom_number]->YScaleStorageTypeList[map_number];
                    map_value_endian = ecuCalDef[rom_number]->YScaleEndianList[map_number];
                    map_min_value = ecuCalDef[rom_number]->YScaleMinValueList[map_number];
                    map_max_value = ecuCalDef[rom_number]->YScaleMaxValueList[map_number];
                    map_data_address = ecuCalDef[rom_number]->YScaleAddressList[map_number].toUInt(&bStatus, 16);
                    first_col++;
                    last_col++;
                    mapXSize = 1;
                }
                else if (selected_range.begin()->topRow() == 0 && mapXSize > 1){
                    if (ecuCalDef[rom_number]->XScaleTypeList.at(map_number) == "Static Y Axis" || ecuCalDef[rom_number]->XScaleTypeList.at(map_number) == "Static X Axis")
                        return;
                    map_data_cell_text = ecuCalDef[rom_number]->XScaleData.at(map_number).split(",");
                    map_coarse_inc_value = ecuCalDef[rom_number]->XScaleCoarseIncList[map_number].toFloat();
                    map_fine_inc_value = ecuCalDef[rom_number]->XScaleFineIncList[map_number].toFloat();
                    map_format = ecuCalDef[rom_number]->XScaleFormatList[map_number];
                    map_value_to_byte = ecuCalDef[rom_number]->XScaleToByteList[map_number];
                    map_value_from_byte = ecuCalDef[rom_number]->XScaleFromByteList[map_number];
                    map_value_storagetype = ecuCalDef[rom_number]->XScaleStorageTypeList[map_number];
                    map_value_endian = ecuCalDef[rom_number]->XScaleEndianList[map_number];
                    map_min_value = ecuCalDef[rom_number]->XScaleMinValueList[map_number];
                    map_max_value = ecuCalDef[rom_number]->XScaleMaxValueList[map_number];
                    map_data_address = ecuCalDef[rom_number]->XScaleAddressList[map_number].toUInt(&bStatus, 16);
                    first_row++;
                    last_row++;
                }
                else{
                    if (mapXSize == 1 && ecuCalDef[rom_number]->XScaleTypeList.at(map_number) != "Static Y Axis" && ecuCalDef[rom_number]->XScaleTypeList.at(map_number) != "Static X Axis") {
                        first_row++;
                        last_row++;
                    }
                    if (mapYSize == 1) {
                        first_col++;
                        last_col++;
                    }
                }

                for (int j = first_row; j <= last_row; j++)
                {
                    for (int i = first_col; i <= last_col; i++)
                    {
                        float map_item_value = map_data_cell_text.at(j * mapXSize + i).toFloat();

                        uint16_t map_value_index = j * mapXSize + i;
                        QString rom_data_value = get_rom_data_value(rom_number, map_data_address, map_value_index, map_value_storagetype, map_value_endian);
                        QString new_rom_data_value;

                        map_data_value.float_value = 0;

                        do {
                            if (map_coarse_inc_value == 0 || map_fine_inc_value == 0)
                                QMessageBox::warning(this, tr("Set value"), "Fine / Coarse inc value not set or set to zero in definition file!");

                            if (action == "coarse_inc")
                                map_item_value += map_coarse_inc_value;
                            if (action == "fine_inc")
                                map_item_value += map_fine_inc_value;
                            if (action == "coarse_dec")
                                map_item_value -= map_coarse_inc_value;
                            if (action == "fine_dec")
                                map_item_value -= map_fine_inc_value;

                            if (map_min_value != " " && map_item_value < map_min_value.toFloat()) {
                                map_item_value = map_min_value.toFloat();
                                new_rom_data_value = QString::number(fileActions->calculate_value_from_expression(fileActions->parse_stringlist_from_expression_string(map_value_to_byte, QString::number(map_item_value))));
                                map_data_value.dword_value = new_rom_data_value.toUInt();
                                if (map_value_storagetype == "float")
                                    map_data_value.dword_value = (uint32_t)(qRound(map_data_value.float_value));
                                else
                                    new_rom_data_value = QString::number(qRound(new_rom_data_value.toFloat()));
                                break;
                            }
                            if (map_max_value != " " && map_item_value > map_max_value.toFloat()) {
                                map_item_value = map_max_value.toFloat();
                                new_rom_data_value = QString::number(fileActions->calculate_value_from_expression(fileActions->parse_stringlist_from_expression_string(map_value_to_byte, QString::number(map_item_value))));
                                map_data_value.dword_value = new_rom_data_value.toUInt();
                                if (map_value_storagetype == "float")
                                    map_data_value.dword_value = (uint32_t)(qRound(map_data_value.float_value));
                                else
                                    new_rom_data_value = QString::number(qRound(new_rom_data_value.toFloat()));
                                break;
                            }

                            new_rom_data_value = QString::number(fileActions->calculate_value_from_expression(fileActions->parse_stringlist_from_expression_string(map_value_to_byte, QString::number(map_item_value))));
                            map_data_value.dword_value = new_rom_data_value.toUInt();
                            if (map_value_storagetype == "float")
                                map_data_value.dword_value = (uint32_t)(qRound(map_data_value.float_value));
                            else
                                new_rom_data_value = QString::number(qRound(new_rom_data_value.toFloat()));
                            if (map_value_storagetype.startsWith("uint")) {
                                if (map_value_storagetype == "uint8" && new_rom_data_value.toUInt() > 0xff) {
                                    new_rom_data_value = rom_data_value;
                                    break;
                                }
                                else if (map_value_storagetype == "uint16" && new_rom_data_value.toUInt() > 0xffff) {
                                    new_rom_data_value = rom_data_value;
                                    break;
                                }
                                else if (map_value_storagetype == "uint32" && new_rom_data_value.toUInt() > 0xffffffff) {
                                    new_rom_data_value = rom_data_value;
                                    break;
                                }
                                if (new_rom_data_value.toInt() < 0) {
                                    new_rom_data_value = rom_data_value;
                                    break;
                                }
                            }
                            if (map_value_storagetype.startsWith("int")) {
                                if (map_value_storagetype == "int8" && ((rom_data_value.toUInt() <= 0x7f && new_rom_data_value.toUInt() > 0x7f) || ((uint8_t)rom_data_value.toInt() >= 0x80 && (uint8_t)new_rom_data_value.toInt() < 0x80 && new_rom_data_value.toInt() != 0x00))) {
                                    new_rom_data_value = rom_data_value;
                                    break;
                                }
                                else if (map_value_storagetype == "int16" && ((rom_data_value.toUInt() <= 0x7fff && new_rom_data_value.toUInt() > 0x7fff) || ((uint16_t)rom_data_value.toInt() >= 0x8000 && (uint16_t)new_rom_data_value.toInt() < 0x8000 && new_rom_data_value.toInt() != 0x00))) {
                                    new_rom_data_value = rom_data_value;
                                    break;
                                }
                                else if ((map_value_storagetype == "int32" || map_value_storagetype == "float") && ((rom_data_value.toUInt() <= 0x7fffffff && new_rom_data_value.toUInt() > 0x7fffffff) || ((uint32_t)rom_data_value.toInt() >= 0x80000000 && (uint32_t)new_rom_data_value.toInt() < 0x80000000 && new_rom_data_value.toInt() != 0x00))) {
                                    new_rom_data_value = rom_data_value;
                                    break;
                                }
                            }
                        } while (rom_data_value == new_rom_data_value);

                        map_item_value = fileActions->calculate_value_from_expression(fileActions->parse_stringlist_from_expression_string(map_value_from_byte, new_rom_data_value));

                        map_data_cell_text.replace(j * mapXSize + i, QString::number(map_item_value));

                        if (map_data_value.float_value == 0)
                            map_data_value.float_value = 0.0f;

                        map_data_value.dword_value = new_rom_data_value.toInt();
                        set_rom_data_value(rom_number, map_data_address, map_value_index, map_value_storagetype, map_value_endian, map_data_value.float_value);
                    }
                }
                if (selected_range.begin()->leftColumn() == 0 && mapYSize > 1)
                    ecuCalDef[rom_number]->YScaleData.replace(map_number, map_data_cell_text.join(","));
                else if (selected_range.begin()->topRow() == 0 && mapXSize > 1)
                    ecuCalDef[rom_number]->XScaleData.replace(map_number, map_data_cell_text.join(","));
                else
                    ecuCalDef[rom_number]->MapData.replace(map_number, map_data_cell_text.join(","));

                set_maptablewidget_items();
            }
        }
    }
}

void MainWindow::set_value()
{
    union map_data{
        int8_t sbyte_value[4];
        int16_t sword_value[2];
        int32_t sdword_value;
        uint8_t byte_value[4];
        uint16_t word_value[2];
        uint32_t dword_value;
        float float_value;
    } map_data_value;

    bool bStatus;

    int rom_number = 0;
    int map_number = 0;
    QString map_name = "";

    QMdiSubWindow* w = ui->mdiArea->activeSubWindow();
    if (w)
    {
        QStringList mapWindowString = w->objectName().split(",");
        rom_number = mapWindowString.at(0).toInt();
        map_number = mapWindowString.at(1).toInt();
        map_name = mapWindowString.at(2);

        QTableWidget* mapTableWidget = w->findChild<QTableWidget*>(w->objectName());
        if (mapTableWidget)
        {
            QString text = r36sSetValueDialog(this, mapTableWidget, &bStatus);   // R36S: D-pad Set value window

            text.replace(",", ".");
            bool r36sSetTo = text.startsWith('=');   // R36S: "=" means set to this value (negatives allowed)
            if (r36sSetTo)
                text.remove(0, 1);

            if (bStatus && !text.isEmpty()){
                QStringList map_data_cell_text = ecuCalDef[rom_number]->MapData.at(map_number).split(",");
                QString map_format = ecuCalDef[rom_number]->FormatList[map_number];
                QString map_value_to_byte = ecuCalDef[rom_number]->ToByteList[map_number];
                QString map_value_from_byte = ecuCalDef[rom_number]->FromByteList[map_number];
                QString map_value_storagetype = ecuCalDef[rom_number]->StorageTypeList[map_number];
                QString map_value_endian = ecuCalDef[rom_number]->EndianList[map_number];
                QString map_min_value = ecuCalDef[rom_number]->MinValueList[map_number];
                QString map_max_value = ecuCalDef[rom_number]->MaxValueList[map_number];
                uint32_t map_data_address = ecuCalDef[rom_number]->AddressList[map_number].toUInt(&bStatus, 16);
                int map_x_size = ecuCalDef[rom_number]->XSizeList[map_number].toInt();
                int map_y_size = ecuCalDef[rom_number]->YSizeList[map_number].toInt();

                if (!mapTableWidget->selectedRanges().isEmpty()){
                    QList<QTableWidgetSelectionRange> selected_range = mapTableWidget->selectedRanges();
                    int firstCol = selected_range.begin()->leftColumn() - 1;
                    int firstRow = selected_range.begin()->topRow() - 1;
                    int lastCol = selected_range.begin()->rightColumn() - 1;
                    int lastRow = selected_range.begin()->bottomRow() - 1;

                    if (selected_range.begin()->leftColumn() == 0 && map_y_size > 1){
                        if (ecuCalDef[rom_number]->XScaleTypeList.at(map_number) == "Static Y Axis" || ecuCalDef[rom_number]->XScaleTypeList.at(map_number) == "Static X Axis")
                            return;
                        map_data_cell_text = ecuCalDef[rom_number]->YScaleData.at(map_number).split(",");
                        map_format = ecuCalDef[rom_number]->YScaleFormatList[map_number];
                        map_value_to_byte = ecuCalDef[rom_number]->YScaleToByteList[map_number];
                        map_value_from_byte = ecuCalDef[rom_number]->YScaleFromByteList[map_number];
                        map_value_storagetype = ecuCalDef[rom_number]->YScaleStorageTypeList[map_number];
                        map_value_endian = ecuCalDef[rom_number]->YScaleEndianList[map_number];
                        map_min_value = ecuCalDef[rom_number]->YScaleMinValueList[map_number];
                        map_max_value = ecuCalDef[rom_number]->YScaleMaxValueList[map_number];
                        map_data_address = ecuCalDef[rom_number]->YScaleAddressList[map_number].toUInt(&bStatus, 16);
                        firstCol += 1;
                        lastCol += 1;
                        map_x_size = 1;
                    }
                    else if (selected_range.begin()->topRow() == 0 && map_x_size > 1){
                        if (ecuCalDef[rom_number]->XScaleTypeList.at(map_number) == "Static Y Axis" || ecuCalDef[rom_number]->XScaleTypeList.at(map_number) == "Static X Axis")
                            return;
                        map_data_cell_text = ecuCalDef[rom_number]->XScaleData.at(map_number).split(",");
                        map_format = ecuCalDef[rom_number]->XScaleFormatList[map_number];
                        map_value_to_byte = ecuCalDef[rom_number]->XScaleToByteList[map_number];
                        map_value_from_byte = ecuCalDef[rom_number]->XScaleFromByteList[map_number];
                        map_value_storagetype = ecuCalDef[rom_number]->XScaleStorageTypeList[map_number];
                        map_value_endian = ecuCalDef[rom_number]->XScaleEndianList[map_number];
                        map_min_value = ecuCalDef[rom_number]->XScaleMinValueList[map_number];
                        map_max_value = ecuCalDef[rom_number]->XScaleMaxValueList[map_number];
                        map_data_address = ecuCalDef[rom_number]->XScaleAddressList[map_number].toUInt(&bStatus, 16);
                        firstRow += 1;
                        lastRow += 1;
                    }
                    else{
                        qDebug() << "x:" << map_x_size << "y:" << map_y_size;
                        if (map_x_size == 1 && ecuCalDef[rom_number]->XScaleTypeList.at(map_number) != "Static Y Axis" && ecuCalDef[rom_number]->XScaleTypeList.at(map_number) != "Static X Axis") {
                            firstRow++;
                            lastRow++;
                        }
                        if (map_y_size == 1) {
                            firstCol++;
                            lastCol++;
                        }
                    }

                    for (int j = firstRow; j <= lastRow; j++)
                    {
                        for (int i = firstCol; i <= lastCol; i++)
                        {
                            float map_item_value = map_data_cell_text.at(j * map_x_size + i).toFloat();

                            uint16_t map_value_index = j * map_x_size + i;
                            QString rom_data_value = get_rom_data_value(rom_number, map_data_address, map_value_index, map_value_storagetype, map_value_endian);

                            if (r36sSetTo)
                                map_item_value = text.toFloat();
                            else if (text.at(0) == '+'){
                                QStringList mapItemText = text.split("+");
                                map_item_value = map_item_value + mapItemText[1].toFloat();
                            }
                            else if (text.at(0) == '-'){
                                QStringList mapItemText = text.split("-");
                                map_item_value = map_item_value - mapItemText[1].toFloat();
                            }
                            else if (text.at(0) == '*'){
                                QStringList mapItemText = text.split("*");
                                map_item_value = map_item_value * mapItemText[1].toFloat();
                            }
                            else if (text.at(0) == '/'){
                                QStringList mapItemText = text.split("/");
                                if (mapItemText[1].toFloat() == 0)
                                    QMessageBox::warning(this, tr("Set value"), "Cannot divide by zero!");
                                else
                                    map_item_value = map_item_value / mapItemText[1].toFloat();
                            }
                            else
                                map_item_value = text.toFloat();

                            if (map_min_value != " " && map_item_value < map_min_value.toFloat())
                                map_item_value = map_min_value.toFloat();
                            if (map_max_value != " " && map_item_value > map_max_value.toFloat())
                                map_item_value = map_max_value.toFloat();

                            rom_data_value = QString::number(fileActions->calculate_value_from_expression(fileActions->parse_stringlist_from_expression_string(map_value_to_byte, QString::number(map_item_value))));
                            map_data_value.dword_value = rom_data_value.toUInt();
                            if (map_value_storagetype == "float")
                                map_data_value.dword_value = (uint32_t)(qRound(map_data_value.float_value));
                            else
                                rom_data_value = QString::number(qRound(rom_data_value.toFloat()));
                            map_item_value = fileActions->calculate_value_from_expression(fileActions->parse_stringlist_from_expression_string(map_value_from_byte, rom_data_value));
                            qDebug() << "map_item_value" << map_item_value;
                            map_data_cell_text.replace(j * map_x_size + i, QString::number(map_item_value));
                            qDebug() << j * map_x_size + i << QString::number(map_item_value);

                            map_data_value.dword_value = rom_data_value.toInt();
                            set_rom_data_value(rom_number, map_data_address, map_value_index, map_value_storagetype, map_value_endian, map_data_value.float_value);
                        }
                    }
                    if (selected_range.begin()->leftColumn() == 0 && map_y_size > 1)
                        ecuCalDef[rom_number]->YScaleData.replace(map_number, map_data_cell_text.join(","));
                    else if (selected_range.begin()->topRow() == 0 && map_x_size > 1)
                        ecuCalDef[rom_number]->XScaleData.replace(map_number, map_data_cell_text.join(","));
                    else
                        ecuCalDef[rom_number]->MapData.replace(map_number, map_data_cell_text.join(","));

                    set_maptablewidget_items();
                }
            }
        }
    }
}

void MainWindow::interpolate_value(QString action)
{
    union map_data{
        int8_t sbyte_value[4];
        int16_t sword_value[2];
        int32_t sdword_value;
        uint8_t byte_value[4];
        uint16_t word_value[2];
        uint32_t dword_value;
        float float_value;
    } map_data_value;

    bool bStatus = false;

    int rom_number = 0;
    int map_number = 0;
    QString map_name = "";

    QMdiSubWindow* w = ui->mdiArea->activeSubWindow();
    if (w)
    {
        QStringList mapWindowString = w->objectName().split(",");
        rom_number = mapWindowString.at(0).toInt();
        map_number = mapWindowString.at(1).toInt();
        map_name = mapWindowString.at(2);

        QTableWidget* mapTableWidget = w->findChild<QTableWidget*>(w->objectName());
        if (mapTableWidget)
        {
            QStringList map_data_cell_text = ecuCalDef[rom_number]->MapData.at(map_number).split(",");
            QString map_format = ecuCalDef[rom_number]->FormatList[map_number];
            QString map_value_to_byte = ecuCalDef[rom_number]->ToByteList[map_number];
            QString map_value_from_byte = ecuCalDef[rom_number]->FromByteList[map_number];
            QString map_value_storagetype = ecuCalDef[rom_number]->StorageTypeList[map_number];
            QString map_value_endian = ecuCalDef[rom_number]->EndianList[map_number];
            QString map_min_value = ecuCalDef[rom_number]->MinValueList[map_number];
            QString map_max_value = ecuCalDef[rom_number]->MaxValueList[map_number];
            uint32_t map_data_address = ecuCalDef[rom_number]->AddressList[map_number].toUInt(&bStatus, 16);
            int map_x_size = ecuCalDef[rom_number]->XSizeList[map_number].toInt();
            int map_y_size = ecuCalDef[rom_number]->YSizeList[map_number].toInt();

            if (!mapTableWidget->selectedRanges().isEmpty()){
                QList<QTableWidgetSelectionRange> selected_range = mapTableWidget->selectedRanges();
                int firstCol = selected_range.begin()->leftColumn() - 1;
                int firstRow = selected_range.begin()->topRow() - 1;
                int lastCol = selected_range.begin()->rightColumn() - 1;
                int lastRow = selected_range.begin()->bottomRow() - 1;

                if (selected_range.begin()->leftColumn() == 0 && map_y_size > 1){
                    if (ecuCalDef[rom_number]->XScaleTypeList.at(map_number) == "Static Y Axis" || ecuCalDef[rom_number]->XScaleTypeList.at(map_number) == "Static X Axis")
                        return;
                    map_data_cell_text = ecuCalDef[rom_number]->YScaleData.at(map_number).split(",");
                    map_format = ecuCalDef[rom_number]->YScaleFormatList[map_number];
                    map_value_to_byte = ecuCalDef[rom_number]->YScaleToByteList[map_number];
                    map_value_from_byte = ecuCalDef[rom_number]->YScaleFromByteList[map_number];
                    map_value_storagetype = ecuCalDef[rom_number]->YScaleStorageTypeList[map_number];
                    map_value_endian = ecuCalDef[rom_number]->YScaleEndianList[map_number];
                    map_min_value = ecuCalDef[rom_number]->YScaleMinValueList[map_number];
                    map_max_value = ecuCalDef[rom_number]->YScaleMaxValueList[map_number];
                    map_data_address = ecuCalDef[rom_number]->YScaleAddressList[map_number].toUInt(&bStatus, 16);
                    firstCol += 1;
                    lastCol += 1;
                    map_x_size = 1;
                }
                else if (selected_range.begin()->topRow() == 0 && map_x_size > 1){
                    if (ecuCalDef[rom_number]->XScaleTypeList.at(map_number) == "Static Y Axis" || ecuCalDef[rom_number]->XScaleTypeList.at(map_number) == "Static X Axis")
                        return;
                    map_data_cell_text = ecuCalDef[rom_number]->XScaleData.at(map_number).split(",");
                    map_format = ecuCalDef[rom_number]->XScaleFormatList[map_number];
                    map_value_to_byte = ecuCalDef[rom_number]->XScaleToByteList[map_number];
                    map_value_from_byte = ecuCalDef[rom_number]->XScaleFromByteList[map_number];
                    map_value_storagetype = ecuCalDef[rom_number]->XScaleStorageTypeList[map_number];
                    map_value_endian = ecuCalDef[rom_number]->XScaleEndianList[map_number];
                    map_min_value = ecuCalDef[rom_number]->XScaleMinValueList[map_number];
                    map_max_value = ecuCalDef[rom_number]->XScaleMaxValueList[map_number];
                    map_data_address = ecuCalDef[rom_number]->XScaleAddressList[map_number].toUInt(&bStatus, 16);
                    firstRow += 1;
                    lastRow += 1;
                }
                else{
                    if (map_x_size == 1 && ecuCalDef[rom_number]->XScaleTypeList.at(map_number) != "Static Y Axis" && ecuCalDef[rom_number]->XScaleTypeList.at(map_number) != "Static X Axis") {
                        firstRow++;
                        lastRow++;
                    }
                    if (map_y_size == 1) {
                        firstCol++;
                        lastCol++;
                    }
                }

                int interpolateColCount = lastCol - firstCol + 1;
                int interpolateRowCount = lastRow - firstRow + 1;
                float cellValue[128][128];
                //float cellValue[interpolateColCount][interpolateRowCount];

                float topLeftValue = map_data_cell_text.at(firstRow * map_x_size + firstCol).toFloat();
                float topRightValue = map_data_cell_text.at(firstRow * map_x_size + lastCol).toFloat();
                float bottomLeftValue = map_data_cell_text.at(lastRow * map_x_size + firstCol).toFloat();
                float bottomRightValue = map_data_cell_text.at(lastRow * map_x_size + lastCol).toFloat();

                float leftRowAdder = 0;
                float rightRowAdder = 0;
                float colAdder = 0;
                float rowAdder = 0;
                if (interpolateRowCount > 1){
                    leftRowAdder = (bottomLeftValue - topLeftValue) / (float)(interpolateRowCount - 1);
                    rightRowAdder = (bottomRightValue - topRightValue) / (float)(interpolateRowCount - 1);
                }
                for (int j = 0; j < interpolateRowCount; j++){
                    for (int i = 0; i < interpolateColCount; i++){
                        cellValue[i][j] = map_data_cell_text.at((firstRow + j) * map_x_size + firstCol + i).toFloat();
                    }
                }
                if (action == "interpolate_horizontal"){
                    for (int j = 0; j < interpolateRowCount; j++){
                        if (interpolateColCount > 1)
                            colAdder = (cellValue[interpolateColCount - 1][j] - cellValue[0][j]) / (float)(interpolateColCount - 1);
                        for (int i = 0; i < interpolateColCount; i++){
                            if (interpolateColCount > 1)
                                cellValue[i][j] = cellValue[0][j] + i * colAdder;

                        }
                    }
                }
                if (action == "interpolate_vertical"){
                    for (int i = 0; i < interpolateColCount; i++){
                        if (interpolateRowCount > 1)
                            rowAdder = (cellValue[i][interpolateRowCount - 1] - cellValue[i][0]) / (float)(interpolateRowCount - 1);
                        for (int j = 0; j < interpolateRowCount; j++){
                            if (interpolateRowCount > 1)
                                cellValue[i][j] = cellValue[i][0] + j * rowAdder;

                        }
                    }
                }
                if (action == "interpolate_bidirectional"){
                    for (int j = 0; j < interpolateRowCount; j++){
                        cellValue[0][j] = cellValue[0][0] + j * leftRowAdder;
                        if (interpolateColCount > 1){
                            cellValue[interpolateColCount - 1][j] = cellValue[interpolateColCount - 1][0] + j * rightRowAdder;
                        }
                    }
                    for (int j = 0; j < interpolateRowCount; j++){
                        if (interpolateColCount > 1)
                            colAdder = (cellValue[interpolateColCount - 1][j] - cellValue[0][j]) / (float)(interpolateColCount - 1);
                        for (int i = 0; i < interpolateColCount; i++){
                            if (interpolateColCount > 1)
                                cellValue[i][j] = cellValue[0][j] + i * colAdder;
                        }
                    }
                }

                for (int j = 0; j < interpolateRowCount; j++){
                    for (int i = 0; i < interpolateColCount; i++){
                        uint16_t map_value_index = (j + firstRow) * map_x_size + firstCol + i;
                        QString rom_data_value = QString::number(fileActions->calculate_value_from_expression(fileActions->parse_stringlist_from_expression_string(map_value_to_byte, QString::number(cellValue[i][j]))));
                        map_data_value.dword_value = rom_data_value.toUInt();
                        if (map_value_storagetype == "float")
                            map_data_value.dword_value = (uint32_t)(qRound(map_data_value.float_value));
                        else
                            rom_data_value = QString::number(qRound(rom_data_value.toFloat()));
                        float map_item_value = fileActions->calculate_value_from_expression(fileActions->parse_stringlist_from_expression_string(map_value_from_byte, rom_data_value));

                        map_data_cell_text.replace((firstRow + j) * map_x_size + firstCol + i, QString::number(map_item_value));

                        map_data_value.dword_value = rom_data_value.toInt();
                        set_rom_data_value(rom_number, map_data_address, map_value_index, map_value_storagetype, map_value_endian, map_data_value.float_value);
                    }
                }
                if (selected_range.begin()->leftColumn() == 0 && map_y_size > 1)
                    ecuCalDef[rom_number]->YScaleData.replace(map_number, map_data_cell_text.join(","));
                else if (selected_range.begin()->topRow() == 0 && map_x_size > 1)
                    ecuCalDef[rom_number]->XScaleData.replace(map_number, map_data_cell_text.join(","));
                else
                    ecuCalDef[rom_number]->MapData.replace(map_number, map_data_cell_text.join(","));

                set_maptablewidget_items();
            }
        }
    }
}

void MainWindow::copy_value()
{
    QMdiSubWindow* w = ui->mdiArea->activeSubWindow();
    if (w)
    {
        QStringList mapWindowString = w->objectName().split(",");

        QTableWidget* mapTableWidget = w->findChild<QTableWidget*>(w->objectName());
        if (mapTableWidget){
            QModelIndexList cells = mapTableWidget->selectionModel()->selectedIndexes();
            //qSort(cells); // Necessary, otherwise they are in column order

            QString text;
            int currentRow = 0;
            foreach (const QModelIndex& cell, cells) {
                if (text.length() == 0) {
                } else if (cell.row() != currentRow) {
                    text += '\n';
                } else {
                    text += '\t';
                }
                currentRow = cell.row();
                text += cell.data().toString();
            }

            QApplication::clipboard()->setText(text);
        }
    }
}

void MainWindow::paste_value()
{
    union map_data{
        int8_t sbyte_value[4];
        int16_t sword_value[2];
        int32_t sdword_value;
        uint8_t byte_value[4];
        uint16_t word_value[2];
        uint32_t dword_value;
        float float_value;
    } map_data_value;

    bool bStatus = false;

    int rom_number = 0;
    int map_number = 0;

    QMdiSubWindow* w = ui->mdiArea->activeSubWindow();
    if (w)
    {
        QStringList mapWindowString = w->objectName().split(",");
        rom_number = mapWindowString.at(0).toInt();
        map_number = mapWindowString.at(1).toInt();

        QTableWidget* mapTableWidget = w->findChild<QTableWidget*>(w->objectName());
        if (mapTableWidget)
        {
            if (!mapTableWidget->selectedRanges().isEmpty())
            {
                QStringList mapDataCellText = ecuCalDef[rom_number]->MapData.at(map_number).split(",");
                QString pasteString = QApplication::clipboard()->text();
                QStringList rows = pasteString.split('\n');
                //QString mapFormat = ecuCalDef[mapRomNumber]->FormatList[mapNumber];
                QString map_value_storagetype = ecuCalDef[rom_number]->StorageTypeList[map_number];
                QString map_value_to_byte = ecuCalDef[rom_number]->ToByteList[map_number];
                QString map_value_endian = ecuCalDef[rom_number]->EndianList[map_number];
                uint32_t map_data_address = ecuCalDef[rom_number]->AddressList[map_number].toUInt(&bStatus, 16);
                int map_x_size = ecuCalDef[rom_number]->XSizeList[map_number].toInt();
                int map_y_size = ecuCalDef[rom_number]->YSizeList[map_number].toInt();

                if (!mapTableWidget->selectedRanges().isEmpty()){
                    QList<QTableWidgetSelectionRange> selected_range = mapTableWidget->selectedRanges();
                    int firstCol = selected_range.begin()->leftColumn() - 1;
                    int firstRow = selected_range.begin()->topRow() - 1;

                    int numRows = rows.count();
                    int numColumns = rows.first().count('\t') + 1;

                    for (int j = 0; j < numRows; ++j) {
                        QStringList columns = rows[j].split('\t');
                        for (int i = 0; i < numColumns; ++i) {
                            if ((j + firstRow) < map_y_size && (i + firstCol) < map_x_size)
                            {
                                uint16_t map_value_index = (j + firstRow) * map_x_size + firstCol + i;
                                mapDataCellText.replace((j + firstRow) * map_x_size + (i + firstCol), columns[i]);
                                QString rom_data_value = QString::number(fileActions->calculate_value_from_expression(fileActions->parse_stringlist_from_expression_string(map_value_to_byte, mapDataCellText.at((j + firstRow) * map_x_size + (i + firstCol)))));
                                map_data_value.dword_value = rom_data_value.toUInt();
                                if (map_value_storagetype == "float")
                                    map_data_value.dword_value = (uint32_t)(qRound(map_data_value.float_value));
                                else
                                    rom_data_value = QString::number(qRound(rom_data_value.toFloat()));

                                map_data_value.dword_value = rom_data_value.toInt();
                                set_rom_data_value(rom_number, map_data_address, map_value_index, map_value_storagetype, map_value_endian, map_data_value.float_value);
                            }
                        }
                    }
                    ecuCalDef[rom_number]->MapData.replace(map_number, mapDataCellText.join(","));
                    set_maptablewidget_items();
                }
            }
        }
    }
}

int MainWindow::connect_to_ecu()
{
    ecuid.clear();
    ecu_init_complete = false;
    set_status_bar_label(false, false, "");
    serial->reset_connection();

    qDebug() << "Opening interface, please wait...";
    open_serial_port();
    if (serial->is_serial_port_open())
    {
        serial_port_list->setDisabled(true);
        refresh_serial_port_list->setDisabled(true);
        qDebug() << "Initialising ECU, please wait...";
        int loopcount = 0;
        while (!ecu_init_complete && loopcount < 5)
        {
            ecu_init();
            delay(500);
            loopcount++;
        }
        if (!ecu_init_complete)
            disconnect_from_ecu();
    }
    else
    {
        QMessageBox::warning(this, tr("Serial port"), "Could not open interface!");
        return STATUS_ERROR;
    }
    return STATUS_SUCCESS;
}

void MainWindow::disconnect_from_ecu()
{
    qDebug() << "Disconnecting...";
    ecuid.clear();
    ecu_init_complete = false;
    set_status_bar_label(false, false, "");
    serial->reset_connection();

    serial->set_serial_port_baudrate("4800");
    serial->set_serial_port_parity(QSerialPort::NoParity);

    serial_port_list->setEnabled(true);
    refresh_serial_port_list->setEnabled(true);
}

void MainWindow::ecu_definition_manager()
{
    QDialog *definitions_manager_dialog = new QDialog;
    definitions_manager_dialog->setObjectName("ecu_definition_manager_dialog");
    definitions_manager_dialog->setFixedWidth(640);
    definitions_manager_dialog->setFixedHeight(240);
    definitions_manager_dialog->setWindowModality(Qt::ApplicationModal);
    definitions_manager_dialog->resize(800, 600);
    definitions_manager_dialog->setWindowTitle("ECU definition manager");
    definitions_manager_dialog->setAttribute(Qt::WA_DeleteOnClose);

    QVBoxLayout *definitions_manager_layout = new QVBoxLayout;
    definitions_manager_dialog->setLayout(definitions_manager_layout);

    QListWidget *definition_files = new QListWidget;
    definition_files->setObjectName("ecu_definition_files_list");
    definition_files->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (int i = 0; i < configValues->romraider_definition_files.length(); i++)
    {
        new QListWidgetItem(configValues->romraider_definition_files.at(i), definition_files);
    }
    definitions_manager_layout->addWidget(definition_files);

    QWidget *definitions_manager_widget = new QWidget;
    QHBoxLayout *definitions_manager_buttons = new QHBoxLayout;
    definitions_manager_layout->addWidget(definitions_manager_widget);
    definitions_manager_widget->setLayout(definitions_manager_buttons);

    QPushButton *add_new_file = new QPushButton("Add new file");
    QPushButton *remove_file = new QPushButton("Remove file");
    QPushButton *close = new QPushButton("Close");
    QSpacerItem *spacer = new QSpacerItem(20, 20, QSizePolicy::MinimumExpanding, QSizePolicy::Minimum);

    definitions_manager_buttons->addWidget(add_new_file);
    definitions_manager_buttons->addWidget(remove_file);
    definitions_manager_buttons->addSpacerItem(spacer);
    definitions_manager_buttons->addWidget(close);

    connect(add_new_file, SIGNAL(clicked()), this, SLOT(add_new_ecu_definition_file()));
    connect(remove_file, SIGNAL(clicked()), this, SLOT(remove_ecu_definition_file()));
    connect(close, SIGNAL(clicked()), definitions_manager_dialog, SLOT(close()));
    definitions_manager_dialog->exec();

}

void MainWindow::logger_definition_manager()
{

}

void MainWindow::set_realtime_state(bool state)
{
    QAction *logger;
    QList<QMenu*> menus = ui->menubar->findChildren<QMenu*>();
    foreach (QMenu *menu, menus) {
        foreach (QAction *action, menu->actions()) {
            if (action->isSeparator()) {

            } else if (action->menu()) {

            } else {
                if (action->text() == "Logging")
                {
                    action->setChecked(state);
                }
            }
        }
    }
}

// R36S: defined in log_operations_ssm.cpp
void r36sApplyLogSetup(FileActions::LogValuesStructure *lv, const QString &protocol, const QString &ecuid);
void r36sResetMinMax();
void r36sShowLogPanel(QWidget *area, bool show);


void MainWindow::toggle_realtime()
{
    QAction *logger;
    QList<QMenu*> menus = ui->menubar->findChildren<QMenu*>();
    foreach (QMenu *menu, menus) {
        foreach (QAction *action, menu->actions()) {
            if (action->isSeparator()) {

            } else if (action->menu()) {

            } else {
                if (action->text() == "Logging")
                {
                    logger = action;
                    logging_state = logger->isChecked();
                }
            }
        }
    }

    if (logging_state)
    {
        qDebug() << "Start datalog";
        if (!ecu_init_complete)
        {
            if (connect_to_ecu())
            {
                QMessageBox::information(this, tr("ECU connection"), "Unable to connect to ECU");
                logger->setChecked(false);
                return;
            }
        }
        // R36S: re-read FastECU-Files/log_setup.txt so edits apply without reconnecting
        r36sApplyLogSetup(logValues, protocol, ecuid);
        r36sResetMinMax();
        update_logboxes(protocol);
        r36sShowLogPanel(ui->logBoxWidget, true);   // R36S: reveal the digital panel
        r36sShowCsvState(this, write_datalog_to_file, write_datalog_to_file);   // R36S: CSV box
        logging_counter = 0;
        logging_state = true;
        log_ssm_values();
        logparams_poll_timer->start();
    }
    else
    {
        qDebug() << "Stop datalog";
        if (datalog_file_open){
            datalog_file_open = false;
            datalog_file.close();
        }

        logging_state = false;
        log_params_request_started = false;
        log_ssm_values();
        delay(200);
        logparams_poll_timer->stop();
        r36sShowLogPanel(ui->logBoxWidget, false);  // R36S: hide the digital panel
        r36sSetMenuChecked(ui->menubar, "Log to file", false);   // R36S: CSV ends with Logging
        write_datalog_to_file = false;
        r36sShowCsvState(this, false, false);
        //disconnect_from_ecu();
    }
}

void MainWindow::toggle_log_to_file()
{
    QList<QMenu*> menus = ui->menubar->findChildren<QMenu*>();
    foreach (QMenu *menu, menus) {
        foreach (QAction *action, menu->actions()) {
            if (action->isSeparator()) {

            } else if (action->menu()) {

            } else {
                if (action->text() == "Log to file")
                    write_datalog_to_file = action->isChecked();
            }
        }
         r36sShowCsvState(this, write_datalog_to_file, write_datalog_to_file && logging_state);   // R36S: CSV box
    }

    if (!write_datalog_to_file){
        if (datalog_file_open){
            datalog_file_open = false;
            datalog_file.close();
        }
    }
}

void MainWindow::toggle_haltech_ic7_display()
{
    QList<QMenu*> menus = ui->menubar->findChildren<QMenu*>();
    foreach (QMenu *menu, menus) {
        foreach (QAction *action, menu->actions()) {
            if (action->isSeparator()) {

            } else if (action->menu()) {

            } else {
                if (action->text() == "Haltech IC-7")
                    haltech_ic7_display_on = action->isChecked();
            }
        }
    }
    if (haltech_ic7_display_on)
        test_haltech_ic7_display();
}

void MainWindow::toggle_simulate_obd()
{
    QList<QMenu*> menus = ui->menubar->findChildren<QMenu*>();
    foreach (QMenu *menu, menus) {
        foreach (QAction *action, menu->actions()) {
            if (action->isSeparator()) {

            } else if (action->menu()) {

            } else {
                if (action->text() == "Simulate OBD")
                    simulate_obd_on = action->isChecked();
            }
        }
    }
    if (simulate_obd_on)
        simulate_obd();
}

void MainWindow::toggle_can_listener()
{
    QList<QMenu*> menus = ui->menubar->findChildren<QMenu*>();
    foreach (QMenu *menu, menus) {
        foreach (QAction *action, menu->actions()) {
            if (action->isSeparator()) {

            } else if (action->menu()) {

            } else {
                if (action->text() == "CAN listener")
                    can_listener_on = action->isChecked();
            }
        }
    }
    if (can_listener_on)
        can_listener();
}

void MainWindow::show_dtc_window()
{
    serial->reset_connection();
    ecuid.clear();
    ecu_init_complete = false;

    QStringList spl;
    spl.append(serial_ports.at(serial_port_list->currentIndex()));
    serial->set_serial_port_list(spl);

    emit LOG_D("Starting DTC operations", true, true);

    DtcOperations dtcOperations(serial, this);
    QObject::connect(&dtcOperations, &DtcOperations::LOG_E, syslogger, &SystemLogger::log_messages);
    QObject::connect(&dtcOperations, &DtcOperations::LOG_W, syslogger, &SystemLogger::log_messages);
    QObject::connect(&dtcOperations, &DtcOperations::LOG_I, syslogger, &SystemLogger::log_messages);
    QObject::connect(&dtcOperations, &DtcOperations::LOG_D, syslogger, &SystemLogger::log_messages);

    dtcOperations.exec();
    //dtcOperations->run();

    emit LOG_D("DTC operations stopped", true, true);
}

void MainWindow::show_hex_editor()
{
    emit LOG_D("Show hex editor", true, true);

    int rom_number = 0;

    QTreeWidgetItem *selectedItem = NULL;
    int item_count = ui->calibrationFilesTreeWidget->selectedItems().count();
    if (item_count)
    {
        selectedItem = ui->calibrationFilesTreeWidget->selectedItems().at(0);
        rom_number = ui->calibrationFilesTreeWidget->indexOfTopLevelItem(selectedItem);

        //HexEdit *hexEdit = new HexEdit(ecuCalDef[rom_number], this);
        HexEdit *hexEdit = new HexEdit(ecuCalDef[rom_number], this);
    }

}

void MainWindow::show_preferences_window()
{
    Settings settings(configValues);
    settings.exec();
    //fileActions->save_config_file();

}

void MainWindow::show_subaru_biu_window()
{
    logging_poll_timer->stop();

    serial->reset_connection();
    ecuid.clear();
    ecu_init_complete = false;
    serial->set_add_iso14230_header(false);
    serial->set_is_iso14230_connection(true);
    open_serial_port();
    serial->change_port_speed("10400");
    //serial->change_port_speed("4800");

    BiuOperationsSubaru biuOperationsSubaru(serial, this);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_E, syslogger, &SystemLogger::log_messages);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_W, syslogger, &SystemLogger::log_messages);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_I, syslogger, &SystemLogger::log_messages);
    QObject::connect(&biuOperationsSubaru, &BiuOperationsSubaru::LOG_D, syslogger, &SystemLogger::log_messages);

    biuOperationsSubaru.exec();

    emit LOG_D("BIU stopped", true, true);

    serial->set_add_iso14230_header(false);
}

void MainWindow::show_terminal_window()
{
    QStringList serial_port;
    serial_port.append(serial_ports.at(serial_port_list->currentIndex()));
    serial->set_serial_port_list(serial_port);
    DataTerminal hexCommander(serial, this);
    QObject::connect(&hexCommander, &DataTerminal::LOG_E, syslogger, &SystemLogger::log_messages);
    QObject::connect(&hexCommander, &DataTerminal::LOG_W, syslogger, &SystemLogger::log_messages);
    QObject::connect(&hexCommander, &DataTerminal::LOG_I, syslogger, &SystemLogger::log_messages);
    QObject::connect(&hexCommander, &DataTerminal::LOG_D, syslogger, &SystemLogger::log_messages);

    hexCommander.exec();
}

void MainWindow::show_subaru_get_key_window()
{

    GetKeyOperationsSubaru getKeyOperationsSubaru (this);
    getKeyOperationsSubaru.exec();
}

void MainWindow::winols_csv_to_romraider_xml()
{
    DefinitionFileConvert definitionFileMaker;
    definitionFileMaker.exec();
}

void MainWindow::set_maptablewidget_items()
{
    int mapRomNumber = 0;
    int mapNumber = 0;
    QString mapName = "";

    QMdiSubWindow* w = ui->mdiArea->activeSubWindow();
    if (w)
    {
        QStringList mapWindowString = w->objectName().split(",");
        mapRomNumber = mapWindowString.at(0).toInt();
        mapNumber = mapWindowString.at(1).toInt();
        mapName = mapWindowString.at(2);

        QTableWidget* mapTableWidget = w->findChild<QTableWidget*>(w->objectName());
        if (mapTableWidget)
        {
            int xSize = ecuCalDef[mapRomNumber]->XSizeList.at(mapNumber).toInt();
            int ySize = ecuCalDef[mapRomNumber]->YSizeList.at(mapNumber).toInt();
            int mapSize = xSize * ySize;

            int xSizeOffset = 0;
            int ySizeOffset = 0;

            if (ecuCalDef[mapRomNumber]->YSizeList.at(mapNumber).toInt() > 1)
                xSizeOffset = 1;
            if (ecuCalDef[mapRomNumber]->XSizeList.at(mapNumber).toInt() > 1 || ecuCalDef[mapRomNumber]->XScaleTypeList.at(mapNumber) == "Static Y Axis" || ecuCalDef[mapRomNumber]->XScaleTypeList.at(mapNumber) == "Static X Axis")
                ySizeOffset = 1;

            QFont cellFont = mapTableWidget->font();
            cellFont.setPointSize(cellFontSize);
            cellFont.setFamily("Franklin Gothic");

            if (xSize > 1)
            {
                QStringList xScaleCellText = ecuCalDef[mapRomNumber]->XScaleData.at(mapNumber).split(",");
                for (int i = 0; i < xSize; i++)
                {
                    QTableWidgetItem *cellItem;
                    if (ySize > 1)
                        cellItem = mapTableWidget->item(0, i + 1);
                    else
                        cellItem = mapTableWidget->item(0, i);

                    cellItem->setTextAlignment(Qt::AlignCenter);
                    cellItem->setFont(cellFont);

                    QString xScaleCellDataText;

                    if (xScaleCellText.at(i) == " ") {
                        xScaleCellText.insert(i, QString::number(i));
                        xScaleCellDataText = xScaleCellText.at(i);
                    }
                    else if (ecuCalDef[mapRomNumber]->XScaleTypeList.at(mapNumber) == "Static Y Axis" || ecuCalDef[mapRomNumber]->XScaleTypeList.at(mapNumber) == "Static X Axis")
                        xScaleCellDataText = xScaleCellText.at(i);
                    else
                        xScaleCellDataText = QString::number(xScaleCellText.at(i).toFloat(), 'f', get_mapvalue_decimal_count(ecuCalDef[mapRomNumber]->XScaleFormatList.at(mapNumber)));

                    if (i < xScaleCellText.count())
                        cellItem->setText(xScaleCellDataText);
                }
            }
            if (ySize > 1)
            {
                QStringList yScaleCellText = ecuCalDef[mapRomNumber]->YScaleData.at(mapNumber).split(",");
                for (int i = 0; i < ySize; i++)
                {
                    QTableWidgetItem *cellItem;
                    cellItem = mapTableWidget->item(i + 1, 0);

                    cellItem->setTextAlignment(Qt::AlignCenter);
                    cellItem->setFont(cellFont);
                    if (i < yScaleCellText.count())
                        cellItem->setText(QString::number(yScaleCellText.at(i).toFloat(), 'f', get_mapvalue_decimal_count(ecuCalDef[mapRomNumber]->YScaleFormatList.at(mapNumber))));
                }
            }
            QStringList mapDataCellText = ecuCalDef[mapRomNumber]->MapData.at(mapNumber).split(",");
            for (int i = 0; i < mapSize; i++)
            {
                int yPos = 0;
                int xPos = 0;
                if (ecuCalDef[mapRomNumber]->XSizeList.at(mapNumber).toUInt() > 1)
                    yPos = i / xSize + ySizeOffset;
                else
                    yPos = i / xSize;
                if (ecuCalDef[mapRomNumber]->YSizeList.at(mapNumber).toUInt() > 1)
                    xPos = i - (yPos - ySizeOffset) * xSize + xSizeOffset;
                else
                    xPos = i - (yPos - ySizeOffset) * xSize;

                //qDebug() << "X pos:" << xPos << "Y pos:" << yPos;
                QTableWidgetItem *cellItem;// = new QTableWidgetItem;
                cellItem = mapTableWidget->item(yPos, xPos);

                cellItem->setTextAlignment(Qt::AlignCenter);
                cellItem->setFont(cellFont);
                int mapItemColor = get_map_cell_colors(ecuCalDef[mapRomNumber], mapDataCellText.at(i).toFloat(), mapNumber);
                int mapItemColorRed = (mapItemColor >> 16) & 0xff;
                int mapItemColorGreen = (mapItemColor >> 8) & 0xff;
                int mapItemColorBlue = mapItemColor & 0xff;
                cellItem->setBackground(QBrush(QColor(mapItemColorRed , mapItemColorGreen, mapItemColorBlue, 255)));
                //if (ecuCalDef[mapRomNumber]->TypeList.at(mapNumber) == "1D")
                    cellItem->setForeground(Qt::black);
                //else
                //    cellItem->setForeground(Qt::white);

                if (i < mapDataCellText.count())
                    cellItem->setText(QString::number(mapDataCellText.at(i).toFloat(), 'f', get_mapvalue_decimal_count(ecuCalDef[mapRomNumber]->FormatList.at(mapNumber))));
            }
        }

        //fileActions->checksum_correction(ecuCalDef[mapRomNumber]);

    }
}

QString MainWindow::get_rom_data_value(uint8_t rom_number, uint32_t data_address, uint16_t value_index, QString storagetype, QString endian)
{
    union map_data{
        int8_t sbyte_value[4];
        int16_t sword_value[2];
        int32_t sdword_value;
        uint8_t byte_value[4];
        uint16_t word_value[2];
        uint32_t dword_value;
        float float_value;
    } map_data_value;

    map_data_value.float_value = 0;

    QString value;
    uint8_t storagesize = 0;

    storagesize = 1;
    if (storagetype == "uint16" || storagetype == "int16")
        storagesize = 2;
    else if (storagetype == "uint24" || storagetype == "int24")
        storagesize = 3;
    else if (storagetype == "uint32" || storagetype == "int32" || storagetype == "float")
        storagesize = 4;

    uint32_t data_byte = 0x00;

    uint32_t byte_address = data_address + (value_index * storagesize);
    if (ecuCalDef[rom_number]->RomInfo.at(FlashMethod) == "wrx02" && ecuCalDef[rom_number]->FileSize.toUInt() < byte_address)
        byte_address -= 0x8000;

    for (int k = 0; k < storagesize; k++)
    {
        if (endian == "little" || storagetype == "float")
        {
            data_byte = (data_byte << 8) + (uint8_t)ecuCalDef[rom_number]->FullRomData.at(byte_address + storagesize - 1 - k);
            map_data_value.byte_value[k] = (uint8_t)ecuCalDef[rom_number]->FullRomData.at(byte_address + storagesize - 1 - k);
        }
        else
        {
            data_byte = (data_byte << 8) + (uint8_t)ecuCalDef[rom_number]->FullRomData.at(byte_address + k);
            map_data_value.byte_value[k] = (uint8_t)ecuCalDef[rom_number]->FullRomData.at(byte_address + k);
        }
    }
    if (storagetype.startsWith("uint"))
        map_data_value.dword_value = data_byte;

    if (storagetype.startsWith("float"))
        value = QString::number(map_data_value.float_value);
    else if (storagetype.startsWith("uint"))
        value = QString::number(map_data_value.dword_value);
    else if (storagetype.startsWith("int")) {
        if (storagesize == 1)
            value = QString::number(map_data_value.sbyte_value[0]);
        if (storagesize == 2)
            value = QString::number(map_data_value.sword_value[0]);
        if (storagesize == 4)
            value = QString::number(map_data_value.sdword_value);
    }

    return value;
}

void MainWindow::set_rom_data_value(uint8_t rom_number, uint32_t data_address, uint16_t value_index, QString storagetype, QString endian, float map_value)
{
    union map_data{
        int8_t sbyte_value[4];
        int16_t sword_value[2];
        int32_t sdword_value;
        uint8_t byte_value[4];
        uint16_t word_value[2];
        uint32_t dword_value;
        float float_value;
    } map_data_value;

    map_data_value.float_value = map_value;
    emit LOG_D("set_rom_data_value: 0x" + QString::number(map_data_value.dword_value, 16) + " | 0x" + QString::number(value_index, 16), true, true);

    uint8_t storagesize = 0;

    storagesize = 1;
    if (storagetype == "uint16" || storagetype == "int16")
        storagesize = 2;
    else if (storagetype == "uint24" || storagetype == "int24")
        storagesize = 3;
    else if (storagetype == "uint32" || storagetype == "int32" || storagetype == "float")
        storagesize = 4;

    uint32_t byte_address = data_address + (value_index * storagesize);
    if (ecuCalDef[rom_number]->RomInfo.at(FlashMethod) == "wrx02" && ecuCalDef[rom_number]->FileSize.toUInt() < (190 * 1024) && byte_address > 0x27FFF)
        byte_address -= 0x8000;

    for (int k = 0; k < storagesize; k++)
    {
        if (endian == "little" || storagetype == "float")
        {
            ecuCalDef[rom_number]->FullRomData[byte_address + k] = (uint8_t)(map_data_value.byte_value[storagesize - 1 - k]);
        }
        else
        {
            ecuCalDef[rom_number]->FullRomData[byte_address + k] = (uint8_t)(map_data_value.byte_value[k]);
        }
    }
}

int MainWindow::get_mapvalue_decimal_count(QString valueFormat)
{
    if (valueFormat.contains("."))
        return valueFormat.split(".").at(1).count(QLatin1Char('0'));
    else
        return 0;//valueFormat.count(QLatin1Char('0'));
}

int MainWindow::get_map_cell_colors(FileActions::EcuCalDefStructure *ecuCalDef, float mapDataValue, int mapIndex)
{
    int mapCellColors;
    float mapMinValue = 0;
    float mapMaxValue = 0;
    float scale_start = (210.0/360.0);

    mapMinValue = ecuCalDef->MapCellColorMin.at(mapIndex).toFloat();
    mapMaxValue = ecuCalDef->MapCellColorMax.at(mapIndex).toFloat();

    QColor color;
    float color_scale = (1 - (mapDataValue - mapMinValue) / (mapMaxValue - mapMinValue)) * scale_start;
    float color_value = scale_start - color_scale;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    double r = 0;
    double g = 0;
    double b = 0;
#else
    float r = 0;
    float g = 0;
    float b = 0;
#endif

    if (color_value < 0)
        color_value = 0;

    color.setHsvF(color_value, 0.85, 0.85);
    color.getRgbF(&r, &g, &b);
    mapCellColors = ((int)(r*255)<<16) + ((int)(g*255)<<8) + b*255;

    //qDebug() << "Map min:" << mapMinValue << "Map max:" << mapMaxValue << "color scale:" << color_scale << "scale start:" << scale_start;

    return mapCellColors;
}

bool MainWindow::check_rom_data_value(QString storagetype, QString rom_data_value, QString new_rom_data_value)
{
    uint8_t storagesize = 1;
    bool result = false;

    union map_data{
        int8_t sbyte_value[4];
        int16_t sword_value[2];
        int32_t sdword_value;
        uint8_t byte_value[4];
        uint16_t word_value[2];
        uint32_t dword_value;
        float float_value;
    } map_data_value;

    if (storagetype == "uint16" || storagetype == "int16")
        storagesize = 2;
    else if (storagetype == "uint24" || storagetype == "int24")
        storagesize = 3;
    else if (storagetype == "uint32" || storagetype == "int32" || storagetype == "float")
        storagesize = 4;

    if (storagetype.startsWith("uint"))
    {
        if (storagetype.startsWith("uint8"))
        {
//            if (rom_data_value.toUInt() >= 0 && new_rom_data_value.toUInt() )
        }

    }
    if (storagetype.startsWith("int"))
    {

    }
    if (storagetype.startsWith("float"))
    {

    }

    return result;
}

int MainWindow::test_haltech_ic7_display()
{
    QByteArray output;
    QByteArray received;

    uint16_t temp_base = 2300;

    uint16_t RPM = 0;
    uint16_t MAP = 0;
    uint16_t TPS = 0;

    uint16_t IDC = 0;
    uint16_t IGN = 0;

    uint16_t SPD = 0;
    uint16_t GEAR = 0;

    uint16_t BATT = 0;

    uint16_t CLT = 0;
    uint16_t IAT = 0;
    uint16_t FLT = 0;
    uint16_t OLT = 0;

    int i = 0;

    //serial_poll_timer->stop();
    //ssm_init_poll_timer->stop();
    logging_poll_timer->stop();

    serial->set_is_iso15765_connection(true);
    //serial->set_is_can_connection(true);
    serial->set_is_29_bit_id(false);
    serial->set_can_speed("1000000");

    serial->reset_connection();
    ecuid.clear();
    ecu_init_complete = false;
    open_serial_port();

    qDebug() << "Send data to Haltech IC-7 display";

    while (haltech_ic7_display_on)
    {
        i = 0;
        while (i < 101)
        {
            RPM = i * 60;
            MAP = i * 10;
            TPS = i * 10;

            IDC = i * 10;
            IGN = i * 5;

            SPD = i * 10;
            GEAR = i / 20 + 1;
            BATT = i / 2 + 100;

            CLT = temp_base + i * 10;
            IAT = temp_base + i * 10;
            FLT = temp_base + i * 10;
            OLT = temp_base + i * 10;

            qDebug() << "MAP:" << MAP << "TPS:" << TPS;

            output.clear();
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x03);
            output.append((uint8_t)0x60);
            //output.append((uint8_t)0x00 & 0xFF);
            //output.append((uint8_t)0x00 & 0xFF);
            output.append((uint8_t)(RPM >> 8) & 0xFF);
            output.append((uint8_t)RPM & 0xFF);
            output.append((uint8_t)(MAP >> 8) & 0xFF);
            output.append((uint8_t)MAP & 0xFF);
            output.append((uint8_t)(TPS >> 8) & 0xFF);
            output.append((uint8_t)TPS & 0xFF);

            received = serial->write_serial_data_echo_check(output);

            output.clear();
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x03);
            output.append((uint8_t)0xE0);
            output.append((uint8_t)(CLT >> 8) & 0xFF);
            output.append((uint8_t)CLT & 0xFF);
            output.append((uint8_t)(IAT >> 8) & 0xFF);
            output.append((uint8_t)IAT & 0xFF);
            output.append((uint8_t)(FLT >> 8) & 0xFF);
            output.append((uint8_t)FLT & 0xFF);
            output.append((uint8_t)(OLT >> 8) & 0xFF);
            output.append((uint8_t)OLT & 0xFF);

            received = serial->write_serial_data_echo_check(output);

            output.clear();
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x03);
            output.append((uint8_t)0x62);
            output.append((uint8_t)(IDC >> 8) & 0xFF);
            output.append((uint8_t)IDC & 0xFF);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)(IGN >> 8) & 0xFF);
            output.append((uint8_t)IGN & 0xFF);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);

            received = serial->write_serial_data_echo_check(output);

            output.clear();
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x03);
            output.append((uint8_t)0x70);
            output.append((uint8_t)(SPD >> 8) & 0xFF);
            output.append((uint8_t)SPD & 0xFF);
            output.append((uint8_t)(GEAR >> 8) & 0xFF);
            output.append((uint8_t)GEAR & 0xFF);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);

            received = serial->write_serial_data_echo_check(output);

            output.clear();
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x03);
            output.append((uint8_t)0x72);
            output.append((uint8_t)(BATT >> 8) & 0xFF);
            output.append((uint8_t)BATT & 0xFF);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);
            output.append((uint8_t)0x00);

            received = serial->write_serial_data_echo_check(output);

            i++;

            delay(20);
        }
    }

    /*
    output.append((uint8_t)0x03);
    output.append((uint8_t)0x60);
    output.append((uint8_t)0x03);
    output.append((uint8_t)0x61);

    output.append((uint8_t)0x03);
    output.append((uint8_t)0x68);
    output.append((uint8_t)0x03);
    output.append((uint8_t)0x69);

    output.append((uint8_t)0x03);
    output.append((uint8_t)0x73);
    output.append((uint8_t)0x03);
    output.append((uint8_t)0x74);
    output.append((uint8_t)0x03);
    output.append((uint8_t)0x75);
    output.append((uint8_t)0x03);
    output.append((uint8_t)0xE0);
    output.append((uint8_t)0x03);
    output.append((uint8_t)0xE2);
*/
    serial->set_can_speed("500000");
    //serial_poll_timer->start();
    //ssm_init_poll_timer->start();

    return 0;
}

int MainWindow::simulate_obd()
{
    QByteArray output;
    QByteArray received;

    QByteArray sid3e = { "\x81\xf1\x12\x7e" };

    uint8_t sid_4d_ff_b4[] = { 0x4c, 0x00, 0xb4 };

    uint8_t sid_81[] = { 0x83, 0xf1, 0x12, 0xc1, 0xef, 0x8f };
    uint8_t sid_82[] = { 0x81, 0xf1, 0x12, 0xc2 };
    uint8_t sid_83_00[] = { 0x80, 0xf1, 0x12, 0x07, 0xc3, 0x00, 0x00, 0xef, 0x00, 0x78, 0x00 };
    uint8_t sid_83_02[] = { 0x80, 0xf1, 0x12, 0x07, 0xc3, 0x02, 0x00, 0x28, 0x00, 0x14, 0x00 };
    uint8_t sid_83_03[] = { 0x82, 0xf1, 0x12, 0xc3, 0x03 };

    uint8_t sid_27_01[] = { 0x80, 0xf1, 0x12, 0x05, 0x67, 0x01, 0xb3, 0x59, 0x2c };
    uint8_t sid_27_02[] = { 0x82, 0xf1, 0x12, 0x03, 0x67, 0x01, 0x34 };
    uint8_t sid_27_10[] = { 0x80, 0xf1, 0x12, 0x06, 0x67, 0x10, 0x10, 0x10, 0x10, 0x10 };
    uint8_t sid_27_11[] = { 0x82, 0xf1, 0x12, 0x03, 0x67, 0x11, 0x34 };

    uint8_t sid_1a_90[] = { 0x13, 0x5a, 0x90, 0x57, 0x44, 0x42, 0x32, 0x31, 0x31, 0x32, 0x32, 0x36, 0x31, 0x41, 0x32, 0x39, 0x32, 0x38, 0x36, 0x39 };
    uint8_t sid_21_09[] = { 0x80, 0xf1, 0x12, 0x66, 0x61, 0x09, 0x43, 0x52, 0x33, 0x30, 0x2d, 0x36, 0x34, 0x38, 0x2d, 0x44,
                            0x32, 0x4d, 0x31, 0x2d, 0x53, 0x32, 0x31, 0x31, 0x2d, 0x4d, 0x45, 0x30, 0x34, 0x30, 0x33, 0x2d,
                            0x30, 0x30, 0x31, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
                            0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
                            0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
                            0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
                            0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00
                          };

    qDebug() << "Simulating OBD communications";

    //serial_poll_timer->stop();
    //ssm_init_poll_timer->stop();
    logging_poll_timer->stop();

    serial->reset_connection();
    ecuid.clear();
    ecu_init_complete = false;

    serial->set_add_iso14230_header(false);
    //serial->set_add_iso14230_header(true);
    open_serial_port();
    //serial->change_port_speed("10400");
    serial->change_port_speed("9600");

    while (simulate_obd_on)
    {
        received.clear();
        output.clear();
        received = serial->read_serial_data(100);
        if (received != "")
        {
            qDebug() << "Received:" << parse_message_to_hex(received);
            // sid_4d_ff_b4
            if ((uint8_t)received.at(0) == 0x4d || (uint8_t)received.at(1) == 0xff || (uint8_t)received.at(2) == 0xb4)
                for (uint8_t i = 0; i < sizeof(sid_4d_ff_b4); i++) output.append((uint8_t)sid_4d_ff_b4[i]);
/*
            if ((uint8_t)received.at(3) == 0x3e || (uint8_t)received.at(4) == 0x3e)
                output = sid3e;

            if ((uint8_t)received.at(3) == 0x81 || (uint8_t)received.at(4) == 0x81)
                for (uint8_t i = 0; i < sizeof(sid_81); i++) output.append((uint8_t)sid_81[i]);
            if ((uint8_t)received.at(3) == 0x82 || (uint8_t)received.at(4) == 0x82)
                for (uint8_t i = 0; i < sizeof(sid_82); i++) output.append((uint8_t)sid_82[i]);
            if (((uint8_t)received.at(3) == 0x83 && (uint8_t)received.at(4) == 0x00) || ((uint8_t)received.at(4) == 0x83 && (uint8_t)received.at(5) == 0x00))
                for (uint8_t i = 0; i < sizeof(sid_83_00); i++) output.append((uint8_t)sid_83_00[i]);
            if (((uint8_t)received.at(3) == 0x83 && (uint8_t)received.at(4) == 0x02) || ((uint8_t)received.at(4) == 0x83 && (uint8_t)received.at(5) == 0x02))
                for (uint8_t i = 0; i < sizeof(sid_83_02); i++) output.append((uint8_t)sid_83_02[i]);
            if (((uint8_t)received.at(3) == 0x83 && (uint8_t)received.at(4) == 0x03) || ((uint8_t)received.at(4) == 0x83 && (uint8_t)received.at(5) == 0x03))
                for (uint8_t i = 0; i < sizeof(sid_83_03); i++) output.append((uint8_t)sid_83_03[i]);

            if (((uint8_t)received.at(3) == 0x27 && (uint8_t)received.at(4) == 0x01) || ((uint8_t)received.at(4) == 0x27 && (uint8_t)received.at(5) == 0x01))
                for (uint8_t i = 0; i < sizeof(sid_27_01); i++) output.append((uint8_t)sid_27_01[i]);
            if (((uint8_t)received.at(3) == 0x27 && (uint8_t)received.at(4) == 0x02) || ((uint8_t)received.at(4) == 0x27 && (uint8_t)received.at(5) == 0x02))
                for (uint8_t i = 0; i < sizeof(sid_27_02); i++) output.append((uint8_t)sid_27_02[i]);
            if (((uint8_t)received.at(3) == 0x27 && (uint8_t)received.at(4) == 0x10) || ((uint8_t)received.at(4) == 0x27 && (uint8_t)received.at(5) == 0x10))
                for (uint8_t i = 0; i < sizeof(sid_27_10); i++) output.append((uint8_t)sid_27_10[i]);
            if (((uint8_t)received.at(3) == 0x27 && (uint8_t)received.at(4) == 0x11) || ((uint8_t)received.at(4) == 0x27 && (uint8_t)received.at(5) == 0x11))
                for (uint8_t i = 0; i < sizeof(sid_27_11); i++) output.append((uint8_t)sid_27_11[i]);

            if ((uint8_t)received.at(1) == 0x1a && (uint8_t)received.at(2) == 0x90)
                for (uint8_t i = 0; i < sizeof(sid_1a_90); i++) output.append((uint8_t)sid_1a_90[i]);

            if ((uint8_t)received.at(3) == 0x21 && (uint8_t)received.at(4) == 0x09)
                for (uint8_t i = 0; i < sizeof(sid_21_09); i++) output.append((uint8_t)sid_21_09[i]);
*/
            if (output != "")
            {
                qDebug() << "Send msg:" << parse_message_to_hex(output);
                received = serial->write_serial_data_echo_check(output);
            }
        }
    }

    // 81 f1 12 7e
    // 81 f1 12 c2
    // 83 f1 12 c1 ef 8f
    // 80 f1 12 07 c3 02 00 28 00 14 00
    // 80 f1 12 07 c3 00 00 ef 00 78 00






    return 0;
}


// ===== Map window title-bar menu: Copy Table / Paste Table / Send To Graph =====
// Clicking the icon at the top left of a map window opens its menu (Restore, Move, Size,
// Minimize, Maximize, Stay on Top, Close). These items are added above them for 2D and 3D maps.

// R36S: defined in log_operations_ssm.cpp
void r36sGraphShow(int mode, const QString &title, const QString &file,
                   const QString &xName, const QString &yName, const QString &zName,
                   const QVector<double> &x, const QVector<double> &y,
                   const QStringList &xText, const QStringList &yText,
                   const QVector<QVector<double> > &z);

namespace {
// Where the parts of a 2D / 3D map sit in its table widget (same rules as CalibrationMaps)
struct MapTableLayout
{
    int xs = 0, ys = 0;          // map data size: X columns, Y rows
    int xOff = 0, yOff = 0;      // data starts at table column xOff, row yOff
    bool staticAxis = false;     // X header holds fixed text labels
    bool xAxis = false;          // X header holds values stored in the ROM (can be pasted)
    bool yAxis = false;          // Y header holds values stored in the ROM (can be pasted)
};

MapTableLayout mapTableLayout(FileActions::EcuCalDefStructure *def, int map)
{
    MapTableLayout l;
    l.xs = def->XSizeList.at(map).toInt();
    l.ys = def->YSizeList.at(map).toInt();
    QString xType = def->XScaleTypeList.at(map);
    l.staticAxis = xType == "Static Y Axis" || xType == "Static X Axis";
    l.xOff = l.ys > 1 ? 1 : 0;
    l.yOff = (l.xs > 1 || l.staticAxis) ? 1 : 0;
    l.xAxis = l.xs > 1 && !l.staticAxis && def->XScaleAddressList.at(map).trimmed() != "";
    l.yAxis = l.ys > 1 && !l.staticAxis && def->YScaleAddressList.at(map).trimmed() != "";
    return l;
}

// Clipboard text -> rows of cells. Tabs (Excel / FastECU) first, then commas, then spaces.
QList<QStringList> parseTableText(QString text)
{
    text.replace("\r\n", "\n").replace('\r', '\n');
    QStringList lines = text.split('\n');
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty())
        lines.removeLast();
    QList<QStringList> rows;
    bool tabs = text.contains('\t');
    bool commas = !tabs && text.contains(',');
    for (const QString &line : lines)
    {
        QStringList cells;
        if (tabs)
            cells = line.split('\t');
        else if (commas)
            cells = line.split(',');
        else
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
            cells = line.simplified().split(' ', Qt::SkipEmptyParts);
#else
            cells = line.simplified().split(' ', QString::SkipEmptyParts);
#endif
        for (QString &c : cells)
            c = c.trimmed();
        rows.append(cells);
    }
    return rows;
}
}

void MainWindow::add_map_window_menu(QMdiSubWindow *subWindow)
{
    QStringList name = subWindow->objectName().split(",");
    if (name.count() < 2)
        return;
    int rom_number = name.at(0).toInt();
    int map_number = name.at(1).toInt();
    QString type = ecuCalDef[rom_number]->TypeList.at(map_number);
    if (type != "2D" && type != "3D")
        return;                                 // switches, selectables and 1D values keep the plain menu

    QMenu *menu = subWindow->systemMenu();
    if (!menu)
        return;
    QAction *first = menu->actions().isEmpty() ? nullptr : menu->actions().first();
    QAction *copyAct = new QAction("Copy Table", menu);
    QAction *pasteAct = new QAction("Paste Table", menu);
    QAction *graphAct = new QAction("Send To 2D/3D Graph", menu);
    QAction *xAct = new QAction("Send X axis To 2D Graph", menu);
    QAction *yAct = new QAction("Send Y axis To 2D Graph", menu);
    menu->insertAction(first, copyAct);
    menu->insertAction(first, pasteAct);
    menu->insertSeparator(first);
    menu->insertAction(first, graphAct);
    menu->insertAction(first, xAct);
    menu->insertAction(first, yAct);
    menu->insertSeparator(first);

    MapTableLayout l = mapTableLayout(ecuCalDef[rom_number], map_number);
    xAct->setEnabled(l.xAxis);                  // only when the header holds numbers (RPM, load, MAF volts ...)
    yAct->setEnabled(l.yAxis);
    connect(menu, &QMenu::aboutToShow, pasteAct, [pasteAct]() {
        pasteAct->setEnabled(!QApplication::clipboard()->text().trimmed().isEmpty());
    });

    connect(copyAct, &QAction::triggered, this, [this, subWindow]() { copy_table(subWindow); });
    connect(pasteAct, &QAction::triggered, this, [this, subWindow]() { paste_table(subWindow); });
    connect(graphAct, &QAction::triggered, this, [this, subWindow]() { send_table_to_graph(subWindow, 0); });
    connect(xAct, &QAction::triggered, this, [this, subWindow]() { send_table_to_graph(subWindow, 1); });
    connect(yAct, &QAction::triggered, this, [this, subWindow]() { send_table_to_graph(subWindow, 2); });
}

// Whole table, X header row and Y header column included, as tab separated text
// (pastes straight into Excel / LibreOffice and back into another FastECU map of the same size)
void MainWindow::copy_table(QMdiSubWindow *subWindow)
{
    QTableWidget *table = subWindow->findChild<QTableWidget*>(subWindow->objectName());
    if (!table)
        return;
    QStringList lines;
    for (int row = 0; row < table->rowCount(); row++)
    {
        QStringList cells;
        for (int col = 0; col < table->columnCount(); col++)
        {
            QTableWidgetItem *item = table->item(row, col);
            cells << (item ? item->text() : QString());
        }
        lines << cells.join('\t');
    }
    QApplication::clipboard()->setText(lines.join("\r\n") + "\r\n");
    emit LOG_I("Copied table " + subWindow->objectName().split(",").value(2) + " ("
               + QString::number(table->columnCount()) + " x " + QString::number(table->rowCount()) + ")", true, true);
}

// One value into the ROM, converted and limited the same way as Set value.
// part: 0 = map data, 1 = X axis, 2 = Y axis. cell_text is the matching data list, updated here.
bool MainWindow::write_table_value(int rom_number, int map_number, int part, int index, QString text, QStringList &cell_text)
{
    union map_data{
        int8_t sbyte_value[4];
        int16_t sword_value[2];
        int32_t sdword_value;
        uint8_t byte_value[4];
        uint16_t word_value[2];
        uint32_t dword_value;
        float float_value;
    } map_data_value;

    FileActions::EcuCalDefStructure *def = ecuCalDef[rom_number];
    bool ok = false;
    text = text.trimmed();
    text.replace(",", ".");
    float map_item_value = text.toFloat(&ok);
    if (!ok || index < 0 || index >= cell_text.count())
        return false;

    QString to_byte = def->ToByteList[map_number];
    QString from_byte = def->FromByteList[map_number];
    QString storagetype = def->StorageTypeList[map_number];
    QString endian = def->EndianList[map_number];
    QString min_value = def->MinValueList[map_number];
    QString max_value = def->MaxValueList[map_number];
    QString address = def->AddressList[map_number];
    if (part == 1)
    {
        to_byte = def->XScaleToByteList[map_number];
        from_byte = def->XScaleFromByteList[map_number];
        storagetype = def->XScaleStorageTypeList[map_number];
        endian = def->XScaleEndianList[map_number];
        min_value = def->XScaleMinValueList[map_number];
        max_value = def->XScaleMaxValueList[map_number];
        address = def->XScaleAddressList[map_number];
    }
    else if (part == 2)
    {
        to_byte = def->YScaleToByteList[map_number];
        from_byte = def->YScaleFromByteList[map_number];
        storagetype = def->YScaleStorageTypeList[map_number];
        endian = def->YScaleEndianList[map_number];
        min_value = def->YScaleMinValueList[map_number];
        max_value = def->YScaleMaxValueList[map_number];
        address = def->YScaleAddressList[map_number];
    }
    bool bStatus = false;
    uint32_t data_address = address.toUInt(&bStatus, 16);
    if (!bStatus)
        return false;

    if (min_value != " " && map_item_value < min_value.toFloat())
        map_item_value = min_value.toFloat();
    if (max_value != " " && map_item_value > max_value.toFloat())
        map_item_value = max_value.toFloat();

    QString rom_data_value = QString::number(fileActions->calculate_value_from_expression(fileActions->parse_stringlist_from_expression_string(to_byte, QString::number(map_item_value))));
    if (storagetype == "float")
    {
        map_data_value.float_value = rom_data_value.toFloat();
    }
    else
    {
        // keep the raw value inside what the storage type can hold
        double raw = qRound64(rom_data_value.toDouble());
        double raw_min = 0, raw_max = 255;
        if (storagetype == "uint16") raw_max = 65535;
        else if (storagetype == "uint24") raw_max = 16777215;
        else if (storagetype == "uint32") raw_max = 4294967295.0;
        else if (storagetype == "int8")  { raw_min = -128; raw_max = 127; }
        else if (storagetype == "int16") { raw_min = -32768; raw_max = 32767; }
        else if (storagetype == "int24") { raw_min = -8388608; raw_max = 8388607; }
        else if (storagetype == "int32") { raw_min = -2147483648.0; raw_max = 2147483647.0; }
        raw = qBound(raw_min, raw, raw_max);
        rom_data_value = QString::number((qint64)raw);
        map_data_value.dword_value = storagetype.startsWith("uint") ? (uint32_t)(quint64)raw : (uint32_t)(int32_t)(qint64)raw;
    }
    map_item_value = fileActions->calculate_value_from_expression(fileActions->parse_stringlist_from_expression_string(from_byte, rom_data_value));
    cell_text.replace(index, QString::number(map_item_value));
    set_rom_data_value(rom_number, data_address, index, storagetype, endian, map_data_value.float_value);
    return true;
}

// Paste a table copied with Copy Table (or from a spreadsheet) into this map.
// Accepted: the whole table with its headers (same size as this one), or the values only.
// Headers are written too when this map's axis is stored in the ROM; text labels are left alone.
void MainWindow::paste_table(QMdiSubWindow *subWindow)
{
    QStringList name = subWindow->objectName().split(",");
    int rom_number = name.at(0).toInt();
    int map_number = name.at(1).toInt();
    FileActions::EcuCalDefStructure *def = ecuCalDef[rom_number];
    QTableWidget *table = subWindow->findChild<QTableWidget*>(subWindow->objectName());
    if (!table)
        return;

    MapTableLayout l = mapTableLayout(def, map_number);
    QList<QStringList> rows = parseTableText(QApplication::clipboard()->text());
    int pasteRows = rows.count();
    int pasteCols = 0;
    for (const QStringList &r : rows)
        pasteCols = qMax(pasteCols, r.count());

    bool withHeaders = pasteRows == l.ys + l.yOff && pasteCols == l.xs + l.xOff;
    bool valuesOnly = !withHeaders && pasteRows == l.ys && pasteCols == l.xs;
    if (!withHeaders && !valuesOnly)
    {
        QMessageBox::warning(this, tr("Paste Table"),
                             QString("The copied table does not match this map.\n\n"
                                     "Copied: %1 columns x %2 rows\n"
                                     "This map: %3 x %4 with headers, or %5 x %6 values only")
                             .arg(pasteCols).arg(pasteRows)
                             .arg(l.xs + l.xOff).arg(l.ys + l.yOff).arg(l.xs).arg(l.ys));
        return;
    }
    int r0 = withHeaders ? l.yOff : 0;
    int c0 = withHeaders ? l.xOff : 0;
    auto cell = [&](int r, int c) { return c < rows.at(r).count() ? rows.at(r).at(c) : QString(); };

    // check every value before anything is written
    for (int j = 0; j < l.ys; j++)
        for (int i = 0; i < l.xs; i++)
        {
            bool ok = false;
            QString t = cell(r0 + j, c0 + i);
            t.replace(",", ".").toFloat(&ok);
            if (!ok)
            {
                QMessageBox::warning(this, tr("Paste Table"),
                                     QString("Value at column %1, row %2 is not a number: \"%3\"\n\nNothing was pasted.")
                                     .arg(c0 + i + 1).arg(r0 + j + 1).arg(cell(r0 + j, c0 + i)));
                return;
            }
        }
    bool pasteX = withHeaders && l.xAxis && l.yOff == 1;
    bool pasteY = withHeaders && l.yAxis && l.xOff == 1;

    QString what = QString("%1 x %2 values").arg(l.xs).arg(l.ys);
    if (pasteX && pasteY) what += " and both axes";
    else if (pasteX) what += " and the X axis";
    else if (pasteY) what += " and the Y axis";
    if (QMessageBox::question(this, tr("Paste Table"),
                              "Paste " + what + " into\n" + def->NameList.at(map_number) + " ?",
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    QStringList data = def->MapData.at(map_number).split(",");
    for (int j = 0; j < l.ys; j++)
        for (int i = 0; i < l.xs; i++)
            write_table_value(rom_number, map_number, 0, j * l.xs + i, cell(r0 + j, c0 + i), data);
    def->MapData.replace(map_number, data.join(","));

    if (pasteX)
    {
        QStringList xData = def->XScaleData.at(map_number).split(",");
        for (int i = 0; i < l.xs; i++)
            write_table_value(rom_number, map_number, 1, i, cell(0, l.xOff + i), xData);   // not a number: left as it is
        def->XScaleData.replace(map_number, xData.join(","));
    }
    if (pasteY)
    {
        QStringList yData = def->YScaleData.at(map_number).split(",");
        for (int j = 0; j < l.ys; j++)
            write_table_value(rom_number, map_number, 2, j, cell(l.yOff + j, 0), yData);
        def->YScaleData.replace(map_number, yData.join(","));
    }

    // new colour range for the new values, then redraw this map
    float lo = data.value(0).toFloat(), hi = lo;
    for (int k = 0; k < l.xs * l.ys && k < data.count(); k++)
    {
        lo = qMin(lo, data.at(k).toFloat());
        hi = qMax(hi, data.at(k).toFloat());
    }
    def->MapCellColorMin[map_number] = QString::number(lo);
    def->MapCellColorMax[map_number] = QString::number(hi);
    ui->mdiArea->setActiveSubWindow(subWindow);
    set_maptablewidget_items();
    emit LOG_I("Pasted table into " + def->NameList.at(map_number), true, true);
}

// Send this map to the 2D / 3D desktop. mode 0 = 2D/3D, 1 = X axis, 2 = Y axis
void MainWindow::send_table_to_graph(QMdiSubWindow *subWindow, int mode)
{
    QStringList name = subWindow->objectName().split(",");
    int rom_number = name.at(0).toInt();
    int map_number = name.at(1).toInt();
    FileActions::EcuCalDefStructure *def = ecuCalDef[rom_number];
    QTableWidget *table = subWindow->findChild<QTableWidget*>(subWindow->objectName());
    if (!table)
        return;
    MapTableLayout l = mapTableLayout(def, map_number);
    if (l.xs < 1 || l.ys < 1)
        return;

    QStringList data = def->MapData.at(map_number).split(",");
    QVector<QVector<double> > z(l.ys, QVector<double>(l.xs, 0.0));
    for (int j = 0; j < l.ys; j++)
        for (int i = 0; i < l.xs; i++)
            z[j][i] = data.value(j * l.xs + i).toDouble();

    // axis positions: the stored values when they are numbers, otherwise step 0, 1, 2 ...
    // axis labels: the header text exactly as the table shows it
    QVector<double> x, y;
    QStringList xText, yText;
    QStringList xData = def->XScaleData.at(map_number).split(",");
    QStringList yData = def->YScaleData.at(map_number).split(",");
    bool xNum = l.xAxis, yNum = l.yAxis;
    for (int i = 0; i < l.xs && xNum; i++) { bool ok; xData.value(i).toDouble(&ok); xNum = ok; }
    for (int j = 0; j < l.ys && yNum; j++) { bool ok; yData.value(j).toDouble(&ok); yNum = ok; }
    if (l.xs > 1)
        for (int i = 0; i < l.xs; i++)
        {
            QTableWidgetItem *item = table->item(0, l.xOff + i);
            x << (xNum ? xData.at(i).toDouble() : double(i));
            xText << (item ? item->text() : QString::number(i));
        }
    if (l.ys > 1)
        for (int j = 0; j < l.ys; j++)
        {
            QTableWidgetItem *item = table->item(l.yOff + j, 0);
            y << (yNum ? yData.at(j).toDouble() : double(j));
            yText << (item ? item->text() : QString::number(j));
        }

    auto axisTitle = [](const QString &n, const QString &u) {
        QString a = n.trimmed(), b = u.trimmed();
        return b.isEmpty() ? a : (a.isEmpty() ? b : a + " (" + b + ")");
    };
    QString xName = axisTitle(def->XScaleNameList.at(map_number), def->XScaleUnitsList.at(map_number));
    QString yName = axisTitle(def->YScaleNameList.at(map_number), def->YScaleUnitsList.at(map_number));
    QString zName = map_number < def->UnitsList.count() ? def->UnitsList.at(map_number).trimmed() : QString();

    if (mode == 1 || mode == 2)
    {
        // Send X / Y axis: only that header's values (RPM, load, MAF volts ...) against step 1, 2, 3 ...
        bool numeric = mode == 1 ? xNum : yNum;
        const QVector<double> &head = mode == 1 ? x : y;
        if (!numeric || head.isEmpty())
        {
            QMessageBox::information(this, tr("Send To Graph"), "This header has no values stored in the ROM to graph.");
            return;
        }
        QVector<double> steps;
        QStringList stepText;
        QVector<QVector<double> > hz;
        for (int k = 0; k < head.size(); k++)
        {
            steps << k + 1;
            stepText << QString::number(k + 1);
        }
        if (mode == 1)
            hz << head;                                     // one row: the X header
        else
            for (double v : head)
                hz << QVector<double>(1, v);                // one column: the Y header
        QString headName = mode == 1 ? xName : yName;
        if (mode == 1)
            r36sGraphShow(1, def->NameList.at(map_number), def->FileName, "Step", "", headName,
                          steps, QVector<double>(), stepText, QStringList(), hz);
        else
            r36sGraphShow(2, def->NameList.at(map_number), def->FileName, "", "Step", headName,
                          QVector<double>(), steps, QStringList(), stepText, hz);
        return;
    }

    r36sGraphShow(0, def->NameList.at(map_number), def->FileName, xName, yName, zName, x, y, xText, yText, z);
}
// ===== end map window title-bar menu =====