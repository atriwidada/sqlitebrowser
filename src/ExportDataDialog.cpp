#include "ExportDataDialog.h"
#include "ui_ExportDataDialog.h"
#include "sqlitedb.h"
#include "Settings.h"
#include "sqlite.h"
#include "FileDialog.h"
#include "IconCache.h"
#include "Data.h"

#include <QSaveFile>
#include <QTextStream>
#include <QMessageBox>
#include <QTextCodec>
#include <json.hpp>

using json = nlohmann::json;

ExportDataDialog::ExportDataDialog(DBBrowserDB& db, ExportFormats format, QWidget* parent, const std::string& query, const sqlb::ObjectIdentifier& selection)
    : QDialog(parent),
      ui(new Ui::ExportDataDialog),
      pdb(db),
      m_format(format),
      m_sQuery(query)
{
    // Create UI
    ui->setupUi(this);

    // Show different option widgets depending on the export format
    ui->stackFormat->setCurrentIndex(format);
    if(format == ExportFormatJson) {
        setWindowTitle(tr("Export data as JSON"));
    }

    // Retrieve the saved dialog preferences
    ui->checkHeader->setChecked(Settings::getValue("exportcsv", "firstrowheader").toBool());
    setSeparatorChar(QChar(Settings::getValue("exportcsv", "separator").toInt()));
    setQuoteChar(QChar(Settings::getValue("exportcsv", "quotecharacter").toInt()));
    setNewLineString(Settings::getValue("exportcsv", "newlinecharacters").toString());
    ui->checkPrettyPrint->setChecked(Settings::getValue("exportjson", "prettyprint").toBool());

    // Update the visible/hidden status of the "Other" line edit fields
    showCustomCharEdits();

    // If a SQL query was specified hide the table combo box. If not fill it with tables to export
    if(query.empty())
    {
        // Get list of tables to export
        for(const auto& it : pdb.schemata)
        {
            for(const auto& jt : it.second.tables)
            {
                sqlb::ObjectIdentifier obj(it.first, jt.first);
                QListWidgetItem* item = new QListWidgetItem(IconCache::get(jt.second->isView() ? "view" : "table"), QString::fromStdString(obj.toDisplayString()));
                item->setData(Qt::UserRole, QString::fromStdString(obj.toSerialised()));
                ui->listTables->addItem(item);
            }
        }

        // Sort list of tables and select the table specified in the selection parameter or alternatively the first one
        ui->listTables->model()->sort(0);
        if(selection.isEmpty())
        {
            ui->listTables->setCurrentItem(ui->listTables->item(0));
        } else {
            for(int i=0;i<ui->listTables->count();i++)
            {
                if(sqlb::ObjectIdentifier(ui->listTables->item(i)->data(Qt::UserRole).toString().toStdString()) == selection)
                {
                    ui->listTables->setCurrentRow(i);
                    break;
                }
            }
        }
    } else {
        // Hide table combo box
        ui->labelTable->setVisible(false);
        ui->listTables->setVisible(false);
        resize(minimumSize());
    }
}

ExportDataDialog::~ExportDataDialog()
{
    delete ui;
}

bool ExportDataDialog::exportQuery(const std::string& sQuery, const QString& sFilename)
{
    switch(m_format)
    {
    case ExportFormatCsv:
        return exportQueryCsv(sQuery, sFilename);
    case ExportFormatJson:
        return exportQueryJson(sQuery, sFilename);
    }

    return false;
}

bool ExportDataDialog::exportQueryCsv(const std::string& sQuery, const QString& sFilename)
{
    // Prepare the quote and separating characters
    QChar quoteChar = currentQuoteChar();
    QString quotequoteChar = QString(quoteChar) + quoteChar;
    QChar sepChar = currentSeparatorChar();
    QString newlineStr = currentNewLineString();

    // Chars that require escaping
    std::string special_chars = newlineStr.toStdString() + sepChar.toLatin1() + quoteChar.toLatin1();
    bool writeError = false;
    // Open file
    QSaveFile file(sFilename);
    if(file.open(QIODevice::WriteOnly))
    {
        // Open text stream to the file
        QTextStream stream(&file);

        auto pDb = pdb.get(tr("exporting CSV"));

        sqlite3_stmt* stmt;
        int status = sqlite3_prepare_v2(pDb.get(), sQuery.c_str(), static_cast<int>(sQuery.size()), &stmt, nullptr);
        if(SQLITE_OK == status)
        {
            if(ui->checkHeader->isChecked())
            {
                int columns = sqlite3_column_count(stmt);
                for (int i = 0; i < columns; ++i)
                {
                    QString content = QString::fromUtf8(sqlite3_column_name(stmt, i));
                    if(content.toStdString().find_first_of(special_chars) != std::string::npos)
                        stream << quoteChar << content.replace(quoteChar, quotequoteChar) << quoteChar;
                    else
                        stream << content;
                    if(i != columns - 1)
                        // Only output the separator value if sepChar isn't 0,
                        // as that's used to indicate no separator character
                        // should be used
                        if(!sepChar.isNull())
                            stream << sepChar;
                }
                stream << newlineStr;
            }

            QApplication::setOverrideCursor(Qt::WaitCursor);
            int columns = sqlite3_column_count(stmt);
            size_t counter = 0;
            while(!writeError && sqlite3_step(stmt) == SQLITE_ROW)
            {
                for (int i = 0; i < columns; ++i)
                {
                    QByteArray blob (reinterpret_cast<const char*>(sqlite3_column_blob(stmt, i)),
                                     sqlite3_column_bytes(stmt, i));
                    QString content = QString::fromUtf8(blob);

                    // Convert to base64 if the data is binary. This case doesn't need quotes.
                    if(!isTextOnly(blob))
                        stream << blob.toBase64();
                    // If no quote char is set but the content contains a line break, we enforce some quote characters. This probably isn't entirely correct
                    // but still better than having the line breaks unquoted and effectively outputting a garbage file.
                    else if(quoteChar.isNull() && content.contains(newlineStr))
                        stream << '"' << content.replace('"', "\"\"") << '"';
                    // If the content needs to be quoted, quote it. But only if a quote char has been specified
                    else if(!quoteChar.isNull() && content.toStdString().find_first_of(special_chars) != std::string::npos)
                        stream << quoteChar << content.replace(quoteChar, quotequoteChar) << quoteChar;
                    // If it doesn't need to be quoted, don't quote it
                    else
                        stream << content;

                    if(i != columns - 1)
                        // Only output the separator value if sepChar isn't 0,
                        // as that's used to indicate no separator character
                        // should be used
                        if(!sepChar.isNull())
                            stream << sepChar;
                }
                stream << newlineStr;
                if(counter % 1000 == 0)
                    qApp->processEvents();
                counter++;
                writeError = stream.status() != QTextStream::Ok;
            }
        }
        sqlite3_finalize(stmt);

        QApplication::restoreOverrideCursor();
        qApp->processEvents();

        // Done writing the file
        if(!file.commit()) {
            writeError = true;
        }

        if(writeError || file.error() != QFileDevice::NoError) {
            QMessageBox::warning(this, QApplication::applicationName(),
                                 tr("Error while writing the file '%1': %2")
                                 .arg(sFilename, file.errorString()));
            return false;
        }

    } else {
        QMessageBox::warning(this, QApplication::applicationName(),
                             tr("Could not open output file: %1").arg(sFilename));
        return false;
    }

    return true;
}

bool ExportDataDialog::exportQueryJson(const std::string& sQuery, const QString& sFilename)
{
    // Open file
    QSaveFile file(sFilename);
    if(file.open(QIODevice::WriteOnly))
    {
        auto pDb = pdb.get(tr("exporting JSON"));

        sqlite3_stmt* stmt;
        int status = sqlite3_prepare_v2(pDb.get(), sQuery.c_str(), static_cast<int>(sQuery.size()), &stmt, nullptr);

        json json_table;

        if(SQLITE_OK == status)
        {
            QApplication::setOverrideCursor(Qt::WaitCursor);
            int columns = sqlite3_column_count(stmt);
            size_t counter = 0;
            std::vector<std::string> column_names;
            while(sqlite3_step(stmt) == SQLITE_ROW)
            {
                // Get column names if we didn't do so before
                if(!column_names.size())
                {
                    for(int i=0;i<columns;++i)
                        column_names.push_back(sqlite3_column_name(stmt, i));
                }

                json json_row;
                for(int i=0;i<columns;++i)
                {
                    int type = sqlite3_column_type(stmt, i);
                    std::string column_name = column_names[static_cast<size_t>(i)];

                    switch (type) {
                    case SQLITE_INTEGER: {
                        qint64 content = sqlite3_column_int64(stmt, i);
                        json_row[column_name] = content;
                        break;
                    }
                    case SQLITE_FLOAT: {
                        double content = sqlite3_column_double(stmt, i);
                        json_row[column_name] = content;
                        break;
                    }
                    case SQLITE_NULL: {
                        json_row[column_name] = nullptr;
                        break;
                    }
                    case SQLITE_TEXT: {
                        QString content = QString::fromUtf8(
                            reinterpret_cast<const char*>(sqlite3_column_text(stmt, i)),
                            sqlite3_column_bytes(stmt, i));
                        json_row[column_name] = content.toStdString();
                        break;
                    }
                    case SQLITE_BLOB: {
                        QByteArray content(reinterpret_cast<const char*>(sqlite3_column_blob(stmt, i)),
                                           sqlite3_column_bytes(stmt, i));
                        QTextCodec *codec = QTextCodec::codecForName("UTF-8");
                        QString string = codec->toUnicode(content.toBase64(QByteArray::Base64Encoding));
                        json_row[column_name] = string.toStdString();
                        break;
                    }
                    }
                }

                json_table.push_back(json_row);

                if(counter % 1000 == 0)
                    qApp->processEvents();
                counter++;
            }
        }

        sqlite3_finalize(stmt);

        // Create JSON document
        file.write(json_table.dump(ui->checkPrettyPrint->isChecked() ? 4 : -1).c_str());
        bool writeError = file.error() != QFileDevice::NoError;

        QApplication::restoreOverrideCursor();
        qApp->processEvents();

        // Done writing the file
        if(!file.commit()) {
            writeError = true;
        }

        if(writeError || file.error() != QFileDevice::NoError) {
          QMessageBox::warning(this, QApplication::applicationName(),
                               tr("Error while writing the file '%1': %2")
                               .arg(sFilename, file.errorString()));
          return false;
        }

    } else {
        QMessageBox::warning(this, QApplication::applicationName(),
                             tr("Could not open output file: %1").arg(sFilename));
        return false;
    }

    return true;
}

void ExportDataDialog::accept()
{
    QStringList file_dialog_filter;
    QString default_file_extension;
    switch(m_format)
    {
    case ExportFormatCsv:
        file_dialog_filter << FILE_FILTER_CSV
                           << FILE_FILTER_TSV
                           << FILE_FILTER_DSV
                           << FILE_FILTER_TXT
                           << FILE_FILTER_ALL;
        default_file_extension = FILE_EXT_CSV_DEFAULT;
        break;
    case ExportFormatJson:
        file_dialog_filter << FILE_FILTER_JSON
                           << FILE_FILTER_TXT
                           << FILE_FILTER_ALL;
        default_file_extension = FILE_EXT_JSON_DEFAULT;
        break;
    }

    bool success = true;
    
    if(!m_sQuery.empty())
    {
        // called from sqlexecute query tab
        QString sFilename = FileDialog::getSaveFileName(
                CreateDataFile,
                this,
                tr("Choose a filename to export data"),
                file_dialog_filter.join(";;"));
        if(sFilename.isEmpty())
        {
            close();
            return;
        }

        success = exportQuery(m_sQuery, sFilename);
    } else {
        // called from the File export menu
        const QList<QListWidgetItem*> selectedItems = ui->listTables->selectedItems();

        if(selectedItems.isEmpty())
        {
            QMessageBox::warning(this, QApplication::applicationName(),
                                 tr("Please select at least 1 table."));
            return;
        }

        // Get filename
        QStringList filenames;
        if(selectedItems.size() == 1)
        {
            QString fileName = FileDialog::getSaveFileName(
                    CreateDataFile,
                    this,
                    tr("Choose a filename to export data"),
                    file_dialog_filter.join(";;"),
                    selectedItems.at(0)->text() + default_file_extension);
            if(fileName.isEmpty())
            {
                close();
                return;
            }

            filenames << fileName;
        } else {
            // ask for folder
            QString exportfolder = FileDialog::getExistingDirectory(
                        CreateDataFile,
                        this,
                        tr("Choose a directory"),
                        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

            if(exportfolder.isEmpty())
            {
                close();
                return;
            }

            for(const QListWidgetItem* item : selectedItems)
                filenames << QDir(exportfolder).filePath(item->text() + default_file_extension);
        }

        // Only if the user hasn't clicked the cancel button
        for(int i = 0; i < selectedItems.size(); ++i)
        {
            // if we are called from execute sql tab, query is already set
            // and we only export 1 select
            std::string sQuery = "SELECT * FROM " + sqlb::ObjectIdentifier(selectedItems.at(i)->data(Qt::UserRole).toString().toStdString()).toString() + ";";
            success = exportQuery(sQuery, filenames.at(i)) && success;
        }
    }

    // Save the dialog preferences for future use
    Settings::setValue("exportcsv", "firstrowheader", ui->checkHeader->isChecked());
    Settings::setValue("exportjson", "prettyprint", ui->checkPrettyPrint->isChecked());
    Settings::setValue("exportcsv", "separator", currentSeparatorChar());
    Settings::setValue("exportcsv", "quotecharacter", currentQuoteChar());
    Settings::setValue("exportcsv", "newlinecharacters", currentNewLineString());

    // Notify the user the export has completed
    if(success) {
        QMessageBox::information(this, QApplication::applicationName(), tr("Export completed."));
    } else {
        QMessageBox::warning(this, QApplication::applicationName(), tr("Export finished with errors."));
    }
    QDialog::accept();
}

void ExportDataDialog::showCustomCharEdits()
{
    // Retrieve selection info for the quote, separator, and newline widgets
    int quoteIndex = ui->comboQuoteCharacter->currentIndex();
    int quoteCount = ui->comboQuoteCharacter->count();
    int sepIndex = ui->comboFieldSeparator->currentIndex();
    int sepCount = ui->comboFieldSeparator->count();
    int newLineIndex = ui->comboNewLineString->currentIndex();
    int newLineCount = ui->comboNewLineString->count();

    // Determine which will have their 'Other' line edit widget visible
    bool quoteVisible = quoteIndex == (quoteCount - 1);
    bool sepVisible = sepIndex == (sepCount - 1);
    bool newLineVisible = newLineIndex == (newLineCount - 1);

    // Update the visibility of the 'Other' line edit widgets
    ui->editCustomQuote->setVisible(quoteVisible);
    ui->editCustomSeparator->setVisible(sepVisible);
    ui->editCustomNewLine->setVisible(newLineVisible);
}

void ExportDataDialog::setQuoteChar(const QChar& c)
{
    QComboBox* combo = ui->comboQuoteCharacter;

    // Set the combo and/or Other box to the correct selection
    switch (c.toLatin1()) {
    case '"':
        combo->setCurrentIndex(0);  // First option is a quote character
        break;

    case '\'':
        combo->setCurrentIndex(1);  // Second option is a single quote character
        break;

    case 0:
        combo->setCurrentIndex(2);  // Third option is blank (no character)
        break;

    default:
        // For everything else, set the combo box to option 3 ('Other') and
        // place the desired string into the matching edit line box
        combo->setCurrentIndex(3);
        if(!c.isNull())
        {
            // Don't set it if/when it's the 0 flag value
            ui->editCustomQuote->setText(c);
        }
        break;
    }
}

char ExportDataDialog::currentQuoteChar() const
{
    QComboBox* combo = ui->comboQuoteCharacter;

    switch (combo->currentIndex()) {
    case 0:
        return '"';  // First option is a quote character

    case 1:
        return '\'';  // Second option is a single quote character

    case 2:
        return 0;  // Third option is a blank (no character)

    default:
        // The 'Other' option was selected, so check if the matching edit
        // line widget contains something
        int customQuoteLength = ui->editCustomQuote->text().length();
        if (customQuoteLength > 0) {
            // Yes it does.  Return its first character
            char customQuoteChar = ui->editCustomQuote->text().at(0).toLatin1();
            return customQuoteChar;
        } else {
            // No it doesn't, so return 0 to indicate it was empty
            return 0;
        }
    }
}

void ExportDataDialog::setSeparatorChar(const QChar& c)
{
    QComboBox* combo = ui->comboFieldSeparator;

    // Set the combo and/or Other box to the correct selection
    switch (c.toLatin1()) {
    case ',':
        combo->setCurrentIndex(0);  // First option is a comma character
        break;

    case ';':
        combo->setCurrentIndex(1);  // Second option is a semi-colon character
        break;

    case '\t':
        combo->setCurrentIndex(2);  // Third option is a tab character
        break;

    case '|':
        combo->setCurrentIndex(3);  // Fourth option is a pipe symbol
        break;

    default:
        // For everything else, set the combo box to option 3 ('Other') and
        // place the desired string into the matching edit line box
        combo->setCurrentIndex(4);

        // Only put the separator character in the matching line edit box if
        // it's not the flag value of 0, which is for indicating its empty
        if(!c.isNull())
            ui->editCustomSeparator->setText(c);
        break;
    }
}

char ExportDataDialog::currentSeparatorChar() const
{
    QComboBox* combo = ui->comboFieldSeparator;

    switch (combo->currentIndex()) {
    case 0:
        return ',';  // First option is a comma character

    case 1:
        return ';';  // Second option is a semi-colon character

    case 2:
        return '\t';  // Third option is a tab character

    case 3:
        return '|';  // Fourth option is a pipe character

    default:
        // The 'Other' option was selected, so check if the matching edit
        // line widget contains something
        int customSeparatorLength = ui->editCustomSeparator->text().length();
        if (customSeparatorLength > 0) {
            // Yes it does.  Return its first character
            char customSeparatorChar = ui->editCustomSeparator->text().at(0).toLatin1();
            return customSeparatorChar;
        } else {
            // No it doesn't, so return 0 to indicate it was empty
            return 0;
        }
    }
}

void ExportDataDialog::setNewLineString(const QString& s)
{
    QComboBox* combo = ui->comboNewLineString;

    // Set the combo and/or Other box to the correct selection
    if (s == "\r\n") {
        // For Windows style newlines, set the combo box to option 0
        combo->setCurrentIndex(0);
    } else if (s == "\n") {
        // For Unix style newlines, set the combo box to option 1
        combo->setCurrentIndex(1);
    } else {
        // For everything else, set the combo box to option 2 ('Other') and
        // place the desired string into the matching edit line box
        combo->setCurrentIndex(2);
        ui->editCustomNewLine->setText(s);
    }
}

QString ExportDataDialog::currentNewLineString() const
{
    QComboBox* combo = ui->comboNewLineString;

    switch (combo->currentIndex()) {
    case 0:
        // Windows style newlines
        return QString("\r\n");

    case 1:
        // Unix style newlines
        return QString("\n");

    default:
        // Return the text from the 'Other' box
        return QString(ui->editCustomNewLine->text().toLatin1());
    }
}
