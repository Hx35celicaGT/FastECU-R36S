  // Modified for FastECU-R36S (R36S handheld running Arch R) by Victor, 2026-10.
  // See CHANGES-R36S.md for what changed. Original FastECU: https://github.com/miikasyvanen/fastecu-oem (GPLv3).


#include "calibration_treewidget.h"

CalibrationTreeWidget::CalibrationTreeWidget()
{

}

QTreeWidget *CalibrationTreeWidget::buildCalibrationFilesTree(int ecuCalDefIndex, QTreeWidget *filesTreeWidget, FileActions::EcuCalDefStructure *ecuCalDef)
{
    QString filename(ecuCalDef->FileName);

    /**************************
     * Create files tree
     *************************/
    QTreeWidget *calFilesTree = filesTreeWidget;
    calFilesTree->setAnimated(true);
    calFilesTree->setFocusPolicy(Qt::NoFocus);
    //calFilesTree->setStyleSheet("QListView::item:selected {background : transparent; border: solid 2px red;}");
    QFont filesItemFont;
    filesItemFont.setPixelSize(14);
    calFilesTree->setFont(filesItemFont);

    QTreeWidgetItem *topLevelFilesTreeItem = new QTreeWidgetItem();
    topLevelFilesTreeItem->setText(2, QString::number(ecuCalDefIndex));
    topLevelFilesTreeItem->setCheckState(0, Qt::Unchecked);
    topLevelFilesTreeItem->setFirstColumnSpanned(true);
    calFilesTree->addTopLevelItem(topLevelFilesTreeItem);

    topLevelFilesTreeItem->setText(0, filename);
    if (ecuCalDef->IdList.length())
        topLevelFilesTreeItem->setText(1, ecuCalDef->IdList.at(0));
    calFilesTree->expandItem(topLevelFilesTreeItem);

    for (int i = 0; i < calFilesTree->topLevelItemCount(); i++)
    {
        calFilesTree->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
        calFilesTree->topLevelItem(i)->setSelected(false);
    }
    topLevelFilesTreeItem->setSelected(true);
    topLevelFilesTreeItem->setCheckState(0, Qt::Checked);

    return calFilesTree;
}

QTreeWidget *CalibrationTreeWidget::buildCalibrationDataTree(QTreeWidget *dataTreeWidget, FileActions::EcuCalDefStructure *ecuCalDef)
{

    /**************************
     * Create data tree
     *************************/
    dataTreeWidget->clear();

    QTreeWidget *calDataTree = dataTreeWidget;

    calDataTree->setAnimated(true);
    calDataTree->setFocusPolicy(Qt::ClickFocus);
    calDataTree->setStyleSheet("QTreeWidget::item:selected { background: #3b78c8; color: white; }");   // R36S: clear highlight

    QFont dataItemFont;
    dataItemFont.setPixelSize(12);
    calDataTree->setFont(dataItemFont);

    // If OEM ecu file, add ROM info
    if (ecuCalDef->RomInfo.length())
    {
        QTreeWidgetItem * topLevelDataTreeItem = new QTreeWidgetItem();
        topLevelDataTreeItem->setText(0, "ROM Info");
        calDataTree->addTopLevelItem(topLevelDataTreeItem);
        if (ecuCalDef->RomInfoExpanded == "1")
            topLevelDataTreeItem->setExpanded(true);

        for (int i = 0; i < ecuCalDef->RomInfo.length(); i++)
        {
            emit LOG_D("Set " + ecuCalDef->RomInfoStrings.at(i) + ": " + ecuCalDef->RomInfo.at(i), true, true);
            QTreeWidgetItem * item = new QTreeWidgetItem();
            item->setText(0, ecuCalDef->RomInfoStrings.at(i) + ": " + ecuCalDef->RomInfo.at(i));
            calDataTree->topLevelItem(0)->addChild(item);
        }
    }

    // R36S: categories named "Group - Name" go into a folder "Group" when two or more of them share
    // that group (Vmod - Knock, Vmod - VE ... -> Vmod > Knock, VE). A group used by only one category,
    // and names without " - ", stay on the top level as before. Each category keeps its full name in
    // UserRole, so its open / closed state is still saved under the definition's category name.
    QHash<QString, int> groupCount;
    QSet<QString> seenCats;
    for (int j = 0; j < ecuCalDef->NameList.count() && j < ecuCalDef->CategoryList.count(); j++)
    {
        const QString cat = ecuCalDef->CategoryList.at(j);
        if (cat.trimmed().isEmpty() || ecuCalDef->NameList.at(j).trimmed().isEmpty() || seenCats.contains(cat))
            continue;
        seenCats.insert(cat);
        int dash = cat.indexOf(" - ");
        if (dash > 0)
            groupCount[cat.left(dash).trimmed()]++;
    }
    QHash<QString, QTreeWidgetItem*> catItems, folderItems;
 
    for (int j = 0; j < ecuCalDef->NameList.count() && j < ecuCalDef->CategoryList.count(); j++)
    {
        const QString cat = ecuCalDef->CategoryList.at(j);
        if (cat.trimmed().isEmpty() || ecuCalDef->NameList.at(j).trimmed().isEmpty())
            continue;
 
        QTreeWidgetItem *catItem = catItems.value(cat);
        if (!catItem)
        {
            int dash = cat.indexOf(" - ");
            QString group = dash > 0 ? cat.left(dash).trimmed() : QString();
            catItem = new QTreeWidgetItem();
            catItem->setData(0, Qt::UserRole, cat);
            if (!group.isEmpty() && groupCount.value(group) > 1)
            {
                QTreeWidgetItem *folder = folderItems.value(group);
                if (!folder)
                {
                    folder = new QTreeWidgetItem();
                    folder->setText(0, group);
                    folder->setData(0, Qt::UserRole, "folder:" + group);
                    calDataTree->addTopLevelItem(folder);
                    folderItems.insert(group, folder);
                    if (openFolders.contains(group))
                        folder->setExpanded(true);
                }
                catItem->setText(0, cat.mid(dash + 3).trimmed());
                folder->addChild(catItem);
            }
            else
            {
                catItem->setText(0, cat);
                calDataTree->addTopLevelItem(catItem);
            }
            catItems.insert(cat, catItem);
            if (j < ecuCalDef->CategoryExpandedList.count() && ecuCalDef->CategoryExpandedList.at(j) == "1")
                catItem->setExpanded(true);
        }
 
        QTreeWidgetItem * item = new QTreeWidgetItem();
        if (ecuCalDef->TypeList[j] == "1D" || ecuCalDef->TypeList[j] == "Selectable" || (ecuCalDef->YSizeList.at(j).toInt() == 1 && ecuCalDef->XSizeList.at(j).toInt() == 1))
            item->setIcon(0, QIcon(":/icons/1D-64.png"));
        else if (ecuCalDef->TypeList[j] == "2D")
            item->setIcon(0, QIcon(":/icons/2D-64.png"));
        else if (ecuCalDef->TypeList[j] == "3D")
            item->setIcon(0, QIcon(":/icons/3D-64.png"));
        if (ecuCalDef->VisibleList.at(j) == "1")
            item->setCheckState(0, Qt::Checked);
        else
            item->setCheckState(0, Qt::Unchecked);
        item->setText(0, ecuCalDef->NameList[j]);
        item->setText(1, QString::number(j));
        catItem->addChild(item);
        item->setToolTip(0, ecuCalDef->NameList[j] + ecuCalDef->DescriptionList[j]);
    }

    return calDataTree;
}

void *CalibrationTreeWidget::calibrationDataTreeWidgetItemExpanded(FileActions::EcuCalDefStructure *ecuCalDef, QString categoryName)
{
        if (categoryName.startsWith("folder:"))   // R36S: a folder such as Vmod
    {
        openFolders.insert(categoryName.mid(7));
        return NULL;
    }
    for (int i = 0; i < ecuCalDef->CategoryList.count(); i++)
    {
        if (ecuCalDef->CategoryList.at(i) == categoryName)
            ecuCalDef->CategoryExpandedList.replace(i, "1");
    }
    if (categoryName == "ROM Info")
        ecuCalDef->RomInfoExpanded = "1";

    return NULL;
}

void *CalibrationTreeWidget::calibrationDataTreeWidgetItemCollapsed(FileActions::EcuCalDefStructure *ecuCalDef, QString categoryName)
{
        if (categoryName.startsWith("folder:"))   // R36S: a folder such as Vmod
    {
        openFolders.remove(categoryName.mid(7));
        return NULL;
    }
    for (int i = 0; i < ecuCalDef->CategoryList.count(); i++)
    {
        if (ecuCalDef->CategoryList.at(i) == categoryName)
            ecuCalDef->CategoryExpandedList.replace(i, "0");
    }
    if (categoryName == "ROM Info")
        ecuCalDef->RomInfoExpanded = "0";

    return NULL;
}
