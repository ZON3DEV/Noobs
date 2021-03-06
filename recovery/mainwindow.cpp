#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "multiimagewritethread.h"
#include "initdrivethread.h"
#include "confeditdialog.h"
#include "progressslideshowdialog.h"
#include "config.h"
#include "languagedialog.h"
#include "json.h"
#include "util.h"
#include "twoiconsdelegate.h"
#include "wifisettingsdialog.h"
#include <QMessageBox>
#include <QProgressDialog>
#include <QMap>
#include <QProcess>
#include <QDir>
#include <QDebug>
#include <QTimer>
#include <QLabel>
#include <QPixmap>
#include <QPainter>
#include <QKeyEvent>
#include <QApplication>
#include <QScreen>
#include <QSplashScreen>
#include <QDesktopWidget>
#include <QSettings>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkDiskCache>
#include <QtNetwork/QNetworkInterface>
#include <QtNetwork/QNetworkConfigurationManager>
#include <QtDBus/QDBusConnection>
#include <QHostInfo>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <inttypes.h>
#include <sys/reboot.h>
#include <linux/reboot.h>
#include <sys/time.h>

#ifdef Q_WS_QWS
#include <QWSServer>
#endif

/* Main window
 *
 * Initial author: Floris Bos
 * Maintained by Raspberry Pi
 *
 * See LICENSE.txt for license details
 *
 */

/* To keep track of where the different OSes get 'installed' from */
#define SOURCE_SDCARD "sdcard"
#define SOURCE_NETWORK "network"
#define SOURCE_INSTALLED_OS "installed_os"

/* Flag to keep track wheter or not we already repartitioned. */
bool MainWindow::_partInited = false;

/* Flag to keep track of current display mode. */
int MainWindow::_currentMode = 0;

MainWindow::MainWindow(const QString &drive, const QString &defaultDisplay, QSplashScreen *splash, QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    _qpd(NULL), _kcpos(0), _defaultDisplay(defaultDisplay),
    _silent(false), _allowSilent(false), _showAll(false), _fixate(false), _splash(splash), _settings(NULL),
    _hasWifi(false), _numInstalledOS(0), _devlistcount(0), _netaccess(NULL), _displayModeBox(NULL), _drive(drive), _bootdrive(drive)
{
    ui->setupUi(this);
    setWindowFlags(Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
    setContextMenuPolicy(Qt::NoContextMenu);
    update_window_title();
    _kc << 0x01000013 << 0x01000013 << 0x01000015 << 0x01000015 << 0x01000012
        << 0x01000014 << 0x01000012 << 0x01000014 << 0x42 << 0x41;
    ui->list->setItemDelegate(new TwoIconsDelegate(this));
    ui->list->installEventFilter(this);
    ui->advToolBar->setVisible(false);

    QRect s = QApplication::desktop()->screenGeometry();
    if (s.height() < 500)
        resize(s.width()-10, s.height()-100);

    if (qApp->arguments().contains("-runinstaller") && !_partInited)
    {
        /* Repartition SD card first */
        _partInited = true;
        setEnabled(false);
        _qpd = new QProgressDialog( tr("Setting up SD card"), QString(), 0, 0, this);
        _qpd->setWindowModality(Qt::WindowModal);
        _qpd->setWindowFlags(Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint);

        InitDriveThread *idt = new InitDriveThread(_bootdrive, this);
        connect(idt, SIGNAL(statusUpdate(QString)), _qpd, SLOT(setLabelText(QString)));
        connect(idt, SIGNAL(completed()), _qpd, SLOT(deleteLater()));
        connect(idt, SIGNAL(error(QString)), this, SLOT(onError(QString)));
        connect(idt, SIGNAL(query(QString, QString, QMessageBox::StandardButton*)),
                this, SLOT(onQuery(QString, QString, QMessageBox::StandardButton*)),
                Qt::BlockingQueuedConnection);

        idt->start();
        _qpd->exec();
        setEnabled(true);
    }

    /* Make sure the SD card is ready, and partition table is read by Linux */
    QByteArray settingsPartition = partdev(_bootdrive, SETTINGS_PARTNR);
    if (!QFile::exists(settingsPartition))
    {
        _qpd = new QProgressDialog( tr("Waiting for SD card (settings partition)"), QString(), 0, 0, this);
        _qpd->setWindowModality(Qt::WindowModal);
        _qpd->setWindowFlags(Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
        _qpd->show();

        while (!QFile::exists(settingsPartition))
        {
            QApplication::processEvents(QEventLoop::WaitForMoreEvents, 250);
        }
        _qpd->hide();
        _qpd->deleteLater();
    }

    _qpd = new QProgressDialog( tr("Mounting settings partition"), QString(), 0, 0, this);
    _qpd->setWindowModality(Qt::WindowModal);
    _qpd->setWindowFlags(Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
    _qpd->show();
    QApplication::processEvents();

    if (QProcess::execute("mount -t ext4 "+settingsPartition+" /settings") != 0)
    {
        _qpd->hide();

        if (QMessageBox::question(this,
                                  tr("Error mounting settings partition"),
                                  tr("Persistent settings partition seems corrupt. Reformat?"),
                                  QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes)
        {
            QProcess::execute("umount /settings");
            if (QProcess::execute("/usr/sbin/mkfs.ext4 "+settingsPartition) != 0
                || QProcess::execute("mount "+settingsPartition+" /settings") != 0)
            {
                QMessageBox::critical(this, tr("Reformat failed"), tr("SD card might be damaged"), QMessageBox::Close);
            }

            rebuildInstalledList();
        }
    }
    _qpd->hide();
    _qpd->deleteLater();
    _qpd = NULL;
    QProcess::execute("mount -o ro -t vfat "+partdev(_bootdrive, 1)+" /mnt");

    _model = getFileContents("/proc/device-tree/model");
    QString cmdline = getFileContents("/proc/cmdline");

    if (QFile::exists("/mnt/os_list_v3.json"))
    {
        /* We have a local os_list_v3.json for testing purposes */
        _repo = "/mnt/os_list_v3.json";

        /* We need a somewhat accurate date for https to work. Normally we retrieve that from the repo server,
           but since we are in testing mode, just set date to last modification time of our local file */
        if (QDate::currentDate().year() < 2016)
        {
            QFileInfo fi(_repo);

            struct timeval tv;
            tv.tv_sec = fi.lastModified().toTime_t();
            tv.tv_usec = 0;
            settimeofday(&tv, NULL);
        }
    }
    else if (cmdline.contains("repo="))
    {
        QByteArray searchFor = "repo=";
        int searchForLen = searchFor.length();
        int pos = cmdline.indexOf(searchFor);
        int end;

        if (cmdline.length() > pos+searchForLen && cmdline.at(pos+searchForLen) == '"')
        {
            /* Value between quotes */
            searchForLen++;
            end = cmdline.indexOf('"', pos+searchForLen);
        }
        else
        {
            end = cmdline.indexOf(' ', pos+searchForLen);
        }
        if (end != -1)
            end = end-pos-searchForLen;
        _repo = cmdline.mid(pos+searchForLen, end);
    }
    else
    {
        _repo = DEFAULT_REPO_SERVER;
    }

    _usbimages = !cmdline.contains("disableusbimages");

    if (cmdline.contains("showall"))
    {
        _showAll = true;
    }
    if (cmdline.contains("fixate"))
    {
        _fixate = true;
    }

    copyWpa();

    if (cmdline.contains("silentinstall"))
    {
        /* If silentinstall is specified, auto-install single image in /os */
        _allowSilent = true;
    }
    else
    {
        startNetworking();
    }

    /* Disable online help buttons until network is functional */
    ui->actionBrowser->setEnabled(false);
    QTimer::singleShot(1, this, SLOT(populate()));

    ui->targetLabel->setHidden(true);
    ui->targetCombo->setHidden(true);
    connect(&_piDrivePollTimer, SIGNAL(timeout()), SLOT(pollForNewDisks()));
    _piDrivePollTimer.start(100);
}

MainWindow::~MainWindow()
{
    QProcess::execute("umount /mnt");
    delete ui;
}

/* Discover which images we have, and fill in the list */
void MainWindow::populate()
{
    /* Ask user to wait while list is populated */
    if (!_allowSilent)
    {
        _qpd = new QProgressDialog(tr("Please wait while NOOBS initialises"), QString(), 0, 0, this);
        _qpd->setWindowFlags(Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
        _qpd->show();

        int timeout = 10000;
        if (getFileContents("/settings/wpa_supplicant.conf").contains("ssid="))
        {
            /* Longer timeout if we have a wifi network configured */
            timeout = 15000;
        }
        QTimer::singleShot(timeout, this, SLOT(hideDialogIfNoNetwork()));
        _time.start();
    }

    _settings = new QSettings("/settings/noobs.conf", QSettings::IniFormat, this);

    /* Restore saved display mode */
    qDebug() << "Default display mode is " << _defaultDisplay;
    int mode = _settings->value("display_mode", _defaultDisplay).toInt();
    if (mode)
    {
        displayMode(mode, true);
    }
    _settings->setValue("display_mode", _defaultDisplay);
    _settings->sync();

    // Fill in list of images
    repopulate();
    _availableMB = (getFileContents(sysclassblock(_drive)+"/size").trimmed().toUInt()-getFileContents(sysclassblock(_drive, 5)+"/start").trimmed().toUInt()-getFileContents(sysclassblock(_drive, 5)+"/size").trimmed().toUInt())/2048;
    updateNeeded();

    if (ui->list->count() != 0)
    {
        QList<QListWidgetItem *> l = ui->list->findItems(RECOMMENDED_IMAGE, Qt::MatchExactly);

        if (!l.isEmpty())
        {
            ui->list->setCurrentItem(l.first());
        }
        else
        {
            ui->list->setCurrentRow(0);
        }

        if (_allowSilent && !_numInstalledOS && ui->list->count() == 1)
        {
            // No OS installed, perform silent installation
            qDebug() << "Performing silent installation";
            _silent = true;
            ui->list->item(0)->setCheckState(Qt::Checked);
            on_actionWrite_image_to_disk_triggered();
            _numInstalledOS = 1;
        }
    }

    ui->actionCancel->setEnabled(_numInstalledOS > 0);
}

void MainWindow::repopulate()
{
    QMap<QString,QVariantMap> images = listImages();
    ui->list->clear();
    bool haveicons = false;
    QSize currentsize = ui->list->iconSize();
    QIcon localIcon(":/icons/hdd.png");
    QIcon internetIcon(":/icons/download.png");
    _numInstalledOS = 0;

    foreach (QVariant v, images.values())
    {
        QVariantMap m = v.toMap();
        QString flavour = m.value("name").toString();
        QString description = m.value("description").toString();
        QString folder  = m.value("folder").toString();
        QString iconFilename = m.value("icon").toString();
        bool installed = m.value("installed").toBool();
        bool recommended = m.value("recommended").toBool();

        if (!iconFilename.isEmpty() && !iconFilename.contains('/'))
            iconFilename = folder+"/"+iconFilename;
        if (!QFile::exists(iconFilename))
        {
            iconFilename = folder+"/"+flavour+".png";
            iconFilename.replace(' ', '_');
        }

        QString friendlyname = flavour;
        if (recommended)
            friendlyname += " ["+tr("RECOMMENDED")+"]";
        if (installed)
        {
            friendlyname += " ["+tr("INSTALLED")+"]";
            _numInstalledOS++;
        }
        if (!description.isEmpty())
            friendlyname += "\n"+description;

        QIcon icon;
        if (QFile::exists(iconFilename))
        {
            icon = QIcon(iconFilename);
            QList<QSize> avs = icon.availableSizes();
            if (avs.isEmpty())
            {
                /* Icon file corrupt */
                icon = QIcon();
            }
            else
            {
                QSize iconsize = avs.first();
                haveicons = true;

                if (iconsize.width() > currentsize.width() || iconsize.height() > currentsize.height())
                {
                    /* Make all icons as large as the largest icon we have */
                    currentsize = QSize(qMax(iconsize.width(), currentsize.width()),qMax(iconsize.height(), currentsize.height()));
                    ui->list->setIconSize(currentsize);
                }
            }
        }
        QListWidgetItem *item = new QListWidgetItem(icon, friendlyname);
        item->setData(Qt::UserRole, m);
        if (installed)
        {
            item->setData(Qt::BackgroundColorRole, INSTALLED_OS_BACKGROUND_COLOR);
            item->setCheckState(Qt::Checked);
        }
        else
            item->setCheckState(Qt::Unchecked);

        if (m["source"] == SOURCE_INSTALLED_OS)
        {
            item->setData(SecondIconRole, QIcon());
        }
        else
        {
            if (folder.startsWith("/mnt"))
                item->setData(SecondIconRole, localIcon);
            else
                item->setData(SecondIconRole, internetIcon);
        }

        if (recommended)
            ui->list->insertItem(0, item);
        else
            ui->list->addItem(item);
    }

    if (haveicons)
    {
        /* Giving items without icon a dummy icon to make them have equal height and text alignment */
        QPixmap dummyicon = QPixmap(currentsize.width(), currentsize.height());
        dummyicon.fill();

        for (int i=0; i< ui->list->count(); i++)
        {
            if (ui->list->item(i)->icon().isNull())
            {
                ui->list->item(i)->setIcon(dummyicon);
            }
        }
    }

    if (_numInstalledOS)
    {
        ui->actionCancel->setEnabled(true);
        if (_fixate)
        {
            ui->list->setEnabled(false);
        }
    }

    filterList();
}

/* Whether this OS should be displayed in the list of installable OSes */
bool MainWindow::canInstallOs(const QString &name, const QVariantMap &values)
{
    /* Can't simply pull "name" from "values" because in some JSON files it's "os_name" and in others it's "name" */

    /* If it's not bootable, it isn't really an OS, so is always installable */
    if (!canBootOs(name, values))
    {
        return true;
    }

    /* RISC_OS needs a matching riscos_offset */
    if (nameMatchesRiscOS(name))
    {
        if (!values.contains(RISCOS_OFFSET_KEY) || (values.value(RISCOS_OFFSET_KEY).toInt() != RISCOS_OFFSET))
        {
            return false;
        }
    }

    /* Display OS in list if it is supported or "showall" is specified in recovery.cmdline */
    if (_showAll)
    {
        return true;
    }
    else
    {
        return isSupportedOs(name, values);
    }
}

/* Whether this OS is supported */
bool MainWindow::isSupportedOs(const QString &name, const QVariantMap &values)
{
    /* Can't simply pull "name" from "values" because in some JSON files it's "os_name" and in others it's "name" */

    /* If it's not bootable, it isn't really an OS, so is always supported */
    if (!canBootOs(name, values))
    {
        return true;
    }

    if (values.contains("supported_models"))
    {
        QStringList supportedModels = values.value("supported_models").toStringList();

        foreach (QString m, supportedModels)
        {
            /* Check if the full formal model name (e.g. "Raspberry Pi 2 Model B Rev 1.1")
             * contains the string we are told to look for (e.g. "Pi 2") */
            if (_model.contains(m, Qt::CaseInsensitive))
            {
                return true;
            }
        }

        return false;
    }

    return true;
}

QMap<QString, QVariantMap> MainWindow::listImages(const QString &folder, bool includeInstalled)
{
    QMap<QString,QVariantMap> images;

    /* Local image folders */
    QDir dir(folder, "", QDir::Name, QDir::Dirs | QDir::NoDotAndDotDot);
    QStringList list = dir.entryList();

    foreach (QString image,list)
    {
        QString imagefolder = folder+"/"+image;
        if (!QFile::exists(imagefolder+"/os.json"))
            continue;
        QVariantMap osv = Json::loadFromFile(imagefolder+"/os.json").toMap();
        osv["source"] = SOURCE_SDCARD;

        QString basename = osv.value("name").toString();
        if (canInstallOs(basename, osv))
        {
            if (QFile::exists(imagefolder+"/flavours.json"))
            {
                QVariantMap v = Json::loadFromFile(imagefolder+"/flavours.json").toMap();
                QVariantList fl = v.value("flavours").toList();

                foreach (QVariant f, fl)
                {
                    QVariantMap fm  = f.toMap();
                    if (fm.contains("name"))
                    {
                        QString name = fm.value("name").toString();
                        if (name == RECOMMENDED_IMAGE)
                            fm["recommended"] = true;
                        fm["folder"] = imagefolder;
                        fm["release_date"] = osv.value("release_date");
                        fm["source"] = osv.value("source");
                        images[name] = fm;
                    }
                }
            }
            else
            {
                QString name = basename;
                if (name == RECOMMENDED_IMAGE)
                    osv["recommended"] = true;
                osv["folder"] = imagefolder;
                images[name] = osv;
            }
        }
    }

    /* Also add information about files already installed */
    if (_settings && includeInstalled)
    {
        QVariantList i = Json::loadFromFile("/settings/installed_os.json").toList();
        foreach (QVariant v, i)
        {
            QVariantMap m = v.toMap();
            QString name = m.value("name").toString();
            if (images.contains(name))
            {
                images[name]["partitions"] = m["partitions"];
            }
            else
            {
                images[name] = m;
                if (name == RECOMMENDED_IMAGE)
                    images[name]["recommended"] = true;
                images[name]["source"] = SOURCE_INSTALLED_OS;
            }
            images[name]["installed"] = true;
        }
    }

    for (QMap<QString,QVariantMap>::iterator i = images.begin(); i != images.end(); i++)
    {
        if (!i.value().contains("nominal_size"))
        {
            /* Calculate nominal_size based on information inside partitions.json */
            int nominal_size = 0;
            QVariantMap pv = Json::loadFromFile(i.value().value("folder").toString()+"/partitions.json").toMap();
            QVariantList pvl = pv.value("partitions").toList();

            foreach (QVariant v, pvl)
            {
                QVariantMap pv = v.toMap();
                nominal_size += pv.value("partition_size_nominal").toInt();
                nominal_size += 1; /* Overhead per partition for EBR */
            }

            i.value().insert("nominal_size", nominal_size);
        }
    }

    return images;
}

void MainWindow::on_actionWrite_image_to_disk_triggered()
{
    QString warning = tr("Warning: this will install the selected Operating System(s). All existing data on the SD card will be overwritten, including any OSes that are already installed.");
    if (_drive != "mmcblk0")
        warning.replace(tr("SD card"), tr("drive"));

    if (_silent || QMessageBox::warning(this,
                                        tr("Confirm"),
                                        warning,
                                        QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes)
    {
        /* See if any of the OSes are unsupported */
        bool allSupported = true;
        QString unsupportedOses;
        QList<QListWidgetItem *> selected = selectedItems();
        foreach (QListWidgetItem *item, selected)
        {
            QVariantMap entry = item->data(Qt::UserRole).toMap();
            QString name = entry.value("name").toString();
            if (!isSupportedOs(name, entry))
            {
                allSupported = false;
                unsupportedOses += "\n" + name;
            }
        }
        if (_silent || allSupported || QMessageBox::warning(this,
                                        tr("Confirm"),
                                        tr("Warning: incompatible Operating System(s) detected. The following OSes aren't supported on this revision of Raspberry Pi and may fail to boot or function correctly:") + unsupportedOses,
                                        QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes)
        {
            setEnabled(false);
            _numMetaFilesToDownload = 0;
            if (_networkStatusPollTimer.isActive())
                _networkStatusPollTimer.stop();

            QList<QListWidgetItem *> selected = selectedItems();
            foreach (QListWidgetItem *item, selected)
            {
                QVariantMap entry = item->data(Qt::UserRole).toMap();

                if (!entry.contains("folder"))
                {
                    QDir d;
                    QString folder = "/settings/os/"+entry.value("name").toString();
                    folder.replace(' ', '_');
                    if (!d.exists(folder))
                        d.mkpath(folder);

                    downloadMetaFile(entry.value("os_info").toString(), folder+"/os.json");
                    downloadMetaFile(entry.value("partitions_info").toString(), folder+"/partitions.json");

                    if (entry.contains("marketing_info"))
                        downloadMetaFile(entry.value("marketing_info").toString(), folder+"/marketing.tar");

                    if (entry.contains("partition_setup"))
                        downloadMetaFile(entry.value("partition_setup").toString(), folder+"/partition_setup.sh");

                    if (entry.contains("icon"))
                        downloadMetaFile(entry.value("icon").toString(), folder+"/icon.png");
                }
            }

            if (_numMetaFilesToDownload == 0)
            {
                /* All OSes selected are local */
                startImageWrite();
            }
            else if (!_silent)
            {
                _qpd = new QProgressDialog(tr("The install process will begin shortly."), QString(), 0, 0, this);
                _qpd->setWindowFlags(Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
                _qpd->show();
            }
        }
    }
}

void MainWindow::on_actionCancel_triggered()
{
    close();
}

void MainWindow::onCompleted()
{
    _qpd->hide();
    QSettings settings("/settings/noobs.conf", QSettings::IniFormat, this);
    settings.setValue("default_partition_to_boot", "800");
    settings.sync();

    if (!_silent)
        QMessageBox::information(this,
                                 tr("OS(es) installed"),
                                 tr("OS(es) Installed Successfully"), QMessageBox::Ok);
    _qpd->deleteLater();
    _qpd = NULL;
    close();
}

void MainWindow::onError(const QString &msg)
{
    qDebug() << "Error:" << msg;
    if (_qpd)
        _qpd->hide();
    QMessageBox::critical(this, tr("Error"), msg, QMessageBox::Close);
    setEnabled(true);
}

void MainWindow::onQuery(const QString &msg, const QString &title, QMessageBox::StandardButton* answer)
{
    *answer = QMessageBox::question(this, title, msg, QMessageBox::Yes|QMessageBox::No);
}

void MainWindow::on_list_currentRowChanged()
{
    QListWidgetItem *item = ui->list->currentItem();
    ui->actionEdit_config->setEnabled(item && item->data(Qt::UserRole).toMap().contains("partitions"));
}

void MainWindow::update_window_title()
{
    setWindowTitle(QString(tr("NOOBS v%1 - Built: %2")).arg(VERSION_NUMBER, QString::fromLocal8Bit(__DATE__)));
}

void MainWindow::changeEvent(QEvent* event)
{
    if (event && event->type() == QEvent::LanguageChange)
    {
        ui->retranslateUi(this);
        update_window_title();
        updateNeeded();
        //repopulate();
    }

    QMainWindow::changeEvent(event);
}

void MainWindow::displayMode(int modenr, bool silent)
{
#ifdef Q_WS_QWS
    QString cmd, mode;

    if (!silent && _displayModeBox)
    {
        /* User pressed another mode selection key while the confirmation box is being displayed */
        silent = true;
        _displayModeBox->close();
    }

    switch (modenr)
    {
    case 0:
        cmd  = "-p";
        mode = tr("HDMI preferred mode");
        break;
    case 1:
        cmd  = "-e \'DMT 4 DVI\'";
        mode = tr("HDMI safe mode");
        break;
    case 2:
        cmd  = "-c \'PAL 4:3\'";
        mode = tr("composite PAL mode");
        break;
    case 3:
        cmd  = "-c \'NTSC 4:3\'";
        mode = tr("composite NTSC mode");
        break;

    default:
        // unknown mode
        return;
    }
    _currentMode = modenr;

    // Trigger framebuffer resize
    QProcess *presize = new QProcess(this);
    presize->start(QString("sh -c \"tvservice -o; tvservice %1;\"").arg(cmd));
    presize->waitForFinished(4000);

    // Update screen resolution with current value (even if we didn't
    // get what we thought we'd get)
    QProcess *update = new QProcess(this);
    update->start(QString("sh -c \"tvservice -s | cut -d , -f 2 | cut -d \' \' -f 2 | cut -d x -f 1;tvservice -s | cut -d , -f 2 | cut -d \' \' -f 2 | cut -d x -f 2\""));
    update->waitForFinished(4000);
    update->setProcessChannelMode(QProcess::MergedChannels);

    QTextStream stream(update);
    int xres = stream.readLine().toInt();
    int yres = stream.readLine().toInt();
    int oTop = 0, oBottom = 0, oLeft = 0, oRight = 0;
    getOverscan(oTop, oBottom, oLeft, oRight);
    qDebug() << "Current overscan" << "top" << oTop << "bottom" << oBottom << "left" << oLeft << "right" << oRight;
    QScreen::instance()->setMode(xres-oLeft-oRight, yres-oTop-oBottom, 16);

    // Resize this window depending on screen resolution
    QRect s = QApplication::desktop()->screenGeometry();
    if (s.height() < 500)
        resize(s.width()-10, s.height()-100);
    else
        resize(575, 450);

    // Update UI item locations
    _splash->setPixmap(QPixmap(":/wallpaper.png"));
    LanguageDialog *ld = LanguageDialog::instance("en", "gb");
    ld->setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignHCenter | Qt::AlignBottom, ld->size(), qApp->desktop()->availableGeometry()));
    this->setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, this->size(), qApp->desktop()->availableGeometry()));

    // Refresh screen
    qApp->processEvents();
    QWSServer::instance()->refresh();

    // In case they can't see the message box, inform that mode change
    // is occurring by turning on the LED during the change
    QProcess *led_blink = new QProcess(this);
    connect(led_blink, SIGNAL(finished(int)), led_blink, SLOT(deleteLater()));
    led_blink->start("sh -c \"echo 1 > /sys/class/leds/led0/brightness; sleep 3; echo 0 > /sys/class/leds/led0/brightness\"");

    // Inform user of resolution change with message box.
    if (!silent && _settings)
    {
        _displayModeBox = new QMessageBox(QMessageBox::Question,
                      tr("Display Mode Changed"),
                      tr("Display mode changed to %1\nWould you like to make this setting permanent?").arg(mode),
                      QMessageBox::Yes | QMessageBox::No);
        _displayModeBox->installEventFilter(this);
        _displayModeBox->exec();

        if (_displayModeBox->standardButton(_displayModeBox->clickedButton()) == QMessageBox::Yes)
        {
            _settings->setValue("display_mode", modenr);
            _settings->sync();
            ::sync();
        }
        _displayModeBox = NULL;
    }

    /*
    QMessageBox *mbox = new QMessageBox;
    mbox->setWindowTitle(tr("Display Mode Changed"));
    mbox->setText(QString(tr("Display mode changed to %1")).arg(mode));
    mbox->setStandardButtons(0);
    mbox->show();
    QTimer::singleShot(2000, mbox, SLOT(hide()));
    */

#else
    Q_UNUSED(modenr)
    Q_UNUSED(silent)
#endif
}

bool MainWindow::eventFilter(QObject *, QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);

        // Let user find the best display mode for their display
        // experimentally by using keys 1-4. NOOBS will default to using HDMI preferred mode.

        // HDMI preferred mode
        if (keyEvent->key() == Qt::Key_1 && _currentMode != 0)
        {
            displayMode(0);
        }
        // HDMI safe mode
        if (keyEvent->key() == Qt::Key_2 && _currentMode != 1)
        {
            displayMode(1);
        }
        // Composite PAL
        if (keyEvent->key() == Qt::Key_3 && _currentMode != 2)
        {
            displayMode(2);
        }
         // Composite NTSC
        if (keyEvent->key() == Qt::Key_4 && _currentMode != 3)
        {
            displayMode(3);
        }
        // Catch Return key to trigger OS boot
        if (keyEvent->key() == Qt::Key_Return)
        {
            on_list_doubleClicked(ui->list->currentIndex());
        }
        else if (_kc.at(_kcpos) == keyEvent->key())
        {
            _kcpos++;
            if (_kcpos == _kc.size())
            {
                inputSequence();
                _kcpos = 0;
            }
        }
        else
            _kcpos=0;
    }

    return false;
}

void MainWindow::inputSequence()
{
    QLabel* info = new QLabel(this);
    info->setPixmap(QPixmap("/usr/data"));
    info->setGeometry(0,0,640,480);
    info->show();
}

void MainWindow::on_actionAdvanced_triggered(bool checked)
{
    ui->advToolBar->setVisible(checked);
}

void MainWindow::on_actionEdit_config_triggered()
{
    QListWidgetItem *item = ui->list->currentItem();

    if (item && item->data(Qt::UserRole).toMap().contains("partitions"))
    {
        QVariantList l = item->data(Qt::UserRole).toMap().value("partitions").toList();
        if (!l.isEmpty())
        {
            QString partition = l.first().toString();
            ConfEditDialog d(partition);
            d.exec();
        }
    }
}

void MainWindow::on_actionBrowser_triggered()
{
    startBrowser();
}

bool MainWindow::requireNetwork()
{
    if (!isOnline())
    {
        QMessageBox::critical(this,
                              tr("No network access"),
                              tr("Wired network access is required for this feature. Please insert a network cable into the network port."),
                              QMessageBox::Close);
        return false;
    }

    return true;
}

void MainWindow::startBrowser()
{
    if (!requireNetwork())
        return;
    QProcess *proc = new QProcess(this);
    QString lang = LanguageDialog::instance("en", "gb")->currentLanguage();
    if (lang == "gb" || lang == "us" || lang == "ar" || lang == "")
        lang = "en";
    proc->start("arora -lang "+lang+" "+HOMEPAGE);
}

void MainWindow::on_list_doubleClicked(const QModelIndex &index)
{
    if (index.isValid())
    {
        QListWidgetItem *item = ui->list->currentItem();
        if (item->checkState() == Qt::Unchecked)
            item->setCheckState(Qt::Checked);
        else
            item->setCheckState(Qt::Unchecked);
    }
}


void MainWindow::copyWpa()
{
    //This file is the one used by dhcpcd
    QFile f("/settings/wpa_supplicant.conf");
    if ( f.exists() && f.size() == 0 )
    {
        /* Remove corrupt file */
        f.remove();
    }

    /* If user supplied a wpa_supplicant.conf on the FAT partition copy that one to settings regardless */
    if (QFile::exists("/mnt/wpa_supplicant.conf"))
    {
        qDebug() << "Copying user wpa_supplicant.conf to /settings/wpa_supplicant.conf";

        QProcess::execute("mount -o remount,rw /mnt");

        backupFile("/settings/wpa_supplicant.conf");
        QFile::copy("/mnt/wpa_supplicant.conf", "/settings/wpa_supplicant.conf");
        f.setPermissions( QFile::WriteUser | QFile::ReadGroup | QFile::ReadOther | QFile::ReadUser );

	/* rename the user file to indicate that it has been copied (and prevent it being re-copied next time, 
           which could potentially overwrite any SSIDs created in the NOOBS GUI) */
	backupFile("/mnt/wpa_supplicant.conf");

        QProcess::execute("sync");
        QProcess::execute("mount -o remount,ro /mnt");
    }
    else if ( !f.exists() )
    {
        /* There is no existing file, must be first installation */
        qDebug() << "Copying /etc/wpa_supplicant.conf to /settings/wpa_supplicant.conf";
        QFile::copy("/etc/wpa_supplicant.conf", "/settings/wpa_supplicant.conf");
    }
    QFile::remove("/etc/wpa_supplicant.conf");
}

void MainWindow::startNetworking()
{
    /* Enable dbus so that we can use it to talk to wpa_supplicant later */
    qDebug() << "Starting dbus";
    QProcess::execute("/etc/init.d/S30dbus start");

    /* Run dhcpcd in background */
    QProcess *proc = new QProcess(this);
    qDebug() << "Starting dhcpcd";
    proc->start("/sbin/dhcpcd --noarp -e wpa_supplicant_conf=/settings/wpa_supplicant.conf --denyinterfaces \"*_ap\"");

    if ( isOnline() )
    {
        onOnlineStateChanged(true);
    }
    else
    {
        /* We could ask Qt's Bearer management to notify us once we are online,
           but it tends to poll every 10 seconds.
           Users are not that patient, so lets poll ourselves every 0.1 second */
        //QNetworkConfigurationManager *_netconfig = new QNetworkConfigurationManager(this);
        //connect(_netconfig, SIGNAL(onlineStateChanged(bool)), this, SLOT(onOnlineStateChanged(bool)));
        connect(&_networkStatusPollTimer, SIGNAL(timeout()), SLOT(pollNetworkStatus()));
        _networkStatusPollTimer.start(100);
    }
}

bool MainWindow::isOnline()
{
    /* Check if we have an IP-address other than localhost */
    QList<QHostAddress> addresses = QNetworkInterface::allAddresses();

    foreach (QHostAddress a, addresses)
    {
        if (a != QHostAddress::LocalHost && a != QHostAddress::LocalHostIPv6)
            return true;
    }

    return false;
}

void MainWindow::pollNetworkStatus()
{
    if (!_hasWifi && QFile::exists("/sys/class/net/wlan0"))
    {
        _hasWifi = true;
        ui->actionWifi->setEnabled(true);
    }
    if (isOnline())
    {
        _networkStatusPollTimer.stop();
        onOnlineStateChanged(true);
    }
}

void MainWindow::onOnlineStateChanged(bool online)
{
    if (online)
    {
        qDebug() << "Network up in" << _time.elapsed()/1000.0 << "seconds";
        if (!_netaccess)
        {
            QDir dir;
            dir.mkdir("/settings/cache");
            _netaccess = new QNetworkAccessManager(this);
            QNetworkDiskCache *_cache = new QNetworkDiskCache(this);
            _cache->setCacheDirectory("/settings/cache");
            _cache->setMaximumCacheSize(8 * 1024 * 1024);
            _netaccess->setCache(_cache);
            QNetworkConfigurationManager manager;
            _netaccess->setConfiguration(manager.defaultConfiguration());

            downloadLists();
        }
        ui->actionBrowser->setEnabled(true);
        emit networkUp();
    }
}

void MainWindow::downloadList(const QString &urlstring)
{
    QNetworkReply *reply = _netaccess->get(QNetworkRequest(QUrl(urlstring)));
    connect(reply, SIGNAL(finished()), this, SLOT(downloadListRedirectCheck()));
}

void MainWindow::downloadLists()
{
    _numIconsToDownload = 0;
    QStringList urls = _repo.split(' ', QString::SkipEmptyParts);

    foreach (QString url, urls)
    {
        if (url.startsWith("/"))
            processJson( Json::parse(getFileContents(url)) );
        else
            downloadList(url);
    }
}

void MainWindow::rebuildInstalledList()
{
    /* Recovery procedure for damaged settings partitions
     * Scan partitions for operating systems installed and regenerate a minimal
     * installed_os.json so that boot menu can function.
     */
    QDir dir;
    dir.mkdir("/mnt2");
    QVariantList installedlist;

    for (int i=5; i<=MAXIMUM_PARTITIONS; i++)
    {
        QString part = partdev(_drive, i);

        if (QFile::exists(part) && QProcess::execute("mount -t vfat "+part+" /mnt2") == 0)
        {
            qDebug() << "Scanning" << part;
            if (QFile::exists("/mnt2/os_config.json"))
            {
                QVariantMap m = Json::loadFromFile("/mnt2/os_config.json").toMap();
                QString f = m.value("flavour").toString();
                if (!f.isEmpty())
                {
                    qDebug() << "OS found:" << f;
                    QVariantMap osinfo;
                    osinfo.insert("name", f);
                    osinfo.insert("release_date", m.value("release_date"));
                    osinfo.insert("partitions", m.value("partitions"));
                    osinfo.insert("folder", m.value("imagefolder"));
                    osinfo.insert("description", m.value("description"));
                    installedlist.append(osinfo);
                }
            }
            QProcess::execute("umount /mnt2");
        }
    }

    if (!installedlist.isEmpty())
    {
        Json::saveToFile("/settings/installed_os.json", installedlist);
    }
}

void MainWindow::downloadListComplete()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    int httpstatuscode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    /* Set our clock to server time if we currently have a date before 2015 */
    QByteArray dateStr = reply->rawHeader("Date");
    if (!dateStr.isEmpty() && QDate::currentDate().year() < 2015)
    {
        // Qt 4 does not have a standard function for parsing the Date header, but it does
        // have one for parsing a Last-Modified header that uses the same date/time format, so just use that
        QNetworkRequest dummyReq;
        dummyReq.setRawHeader("Last-Modified", dateStr);
        QDateTime parsedDate = dummyReq.header(dummyReq.LastModifiedHeader).toDateTime();

        struct timeval tv;
        tv.tv_sec = parsedDate.toTime_t();
        tv.tv_usec = 0;
        settimeofday(&tv, NULL);
    }

    if (reply->error() != reply->NoError || httpstatuscode < 200 || httpstatuscode > 399)
    {
        if (_qpd)
            _qpd->hide();
        QMessageBox::critical(this, tr("Download error"), tr("Error downloading distribution list from Internet"), QMessageBox::Close);
    }
    else
    {
        processJson(Json::parse( reply->readAll() ));
    }

    reply->deleteLater();
}

void MainWindow::processJson(QVariant json)
{
    if (json.isNull())
    {
        QMessageBox::critical(this, tr("Error"), tr("Error parsing list.json downloaded from server"), QMessageBox::Close);
        return;
    }

    QSet<QString> iconurls;
    QVariantList list = json.toMap().value("os_list").toList();

    foreach (QVariant osv, list)
    {
        QVariantMap  os = osv.toMap();

        QString basename = os.value("os_name").toString();
        if (canInstallOs(basename, os))
        {
            if (os.contains("flavours"))
            {
                QVariantList flavours = os.value("flavours").toList();

                foreach (QVariant flv, flavours)
                {
                    QVariantMap flavour = flv.toMap();
                    QVariantMap item = os;
                    QString name        = flavour.value("name").toString();
                    QString description = flavour.value("description").toString();
                    QString iconurl     = flavour.value("icon").toString();

                    item.insert("name", name);
                    item.insert("description", description);
                    item.insert("icon", iconurl);
                    item.insert("feature_level", flavour.value("feature_level"));
                    item.insert("source", SOURCE_NETWORK);

                    processJsonOs(name, item, iconurls);
                }
            }
            if (os.contains("description"))
            {
                QString name = basename;
                os["name"] = name;
                os["source"] = SOURCE_NETWORK;
                processJsonOs(name, os, iconurls);
            }
        }
    }

    /* Download icons */
    if (!iconurls.isEmpty())
    {
         _numIconsToDownload += iconurls.count();
        foreach (QString iconurl, iconurls)
        {
            downloadIcon(iconurl, iconurl);
        }
    }
    else
    {
        if (_qpd)
        {
            _qpd->deleteLater();
            _qpd = NULL;
        }
    }

    filterList();
}

void MainWindow::processJsonOs(const QString &name, QVariantMap &new_details, QSet<QString> &iconurls)
{
    QIcon internetIcon(":/icons/download.png");

    QListWidgetItem *witem = findItem(name);
    if (witem)
    {
        QVariantMap existing_details = witem->data(Qt::UserRole).toMap();

        if ((existing_details["release_date"].toString() < new_details["release_date"].toString()) || (existing_details["source"].toString() == SOURCE_INSTALLED_OS))
        {
            /* Local version is older (or unavailable). Replace info with newer Internet version */
            new_details.insert("installed", existing_details.value("installed", false));
            if (existing_details.contains("partitions"))
            {
                new_details["partitions"] = existing_details["partitions"];
            }
            witem->setData(Qt::UserRole, new_details);
            witem->setData(SecondIconRole, internetIcon);
            ui->list->update();
        }

    }
    else if (!_fixate || _numInstalledOS == 0)
    {
        /* It's a new OS, so add it to the list */
        QString iconurl = new_details.value("icon").toString();
        QString description = new_details.value("description").toString();

        if (!iconurl.isEmpty())
            iconurls.insert(iconurl);

        bool recommended = (name == RECOMMENDED_IMAGE);

        QString friendlyname = name;
        if (recommended)
            friendlyname += " ["+tr("RECOMMENDED")+"]";
        if (!description.isEmpty())
            friendlyname += "\n"+description;

        witem = new QListWidgetItem(friendlyname);
        witem->setCheckState(Qt::Unchecked);
        witem->setData(Qt::UserRole, new_details);
        witem->setData(SecondIconRole, internetIcon);

        if (recommended)
            ui->list->insertItem(0, witem);
        else
            ui->list->addItem(witem);
    }
}

void MainWindow::downloadIcon(const QString &urlstring, const QString &originalurl)
{
    QUrl url(urlstring);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::User, originalurl);
    QNetworkReply *reply = _netaccess->get(request);
    connect(reply, SIGNAL(finished()), this, SLOT(downloadIconRedirectCheck()));
}

QListWidgetItem *MainWindow::findItem(const QVariant &name)
{
    for (int i=0; i<ui->list->count(); i++)
    {
        QListWidgetItem *item = ui->list->item(i);
        QVariantMap m = item->data(Qt::UserRole).toMap();
        if (m.value("name").toString() == name.toString())
        {
            return item;
        }
    }
    return NULL;
}

void MainWindow::downloadIconComplete()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    QString url = reply->url().toString();
    QString originalurl = reply->request().attribute(QNetworkRequest::User).toString();
    int httpstatuscode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != reply->NoError || httpstatuscode < 200 || httpstatuscode > 399)
    {
        //QMessageBox::critical(this, tr("Download error"), tr("Error downloading icon '%1'").arg(reply->url().toString()), QMessageBox::Close);
        qDebug() << "Error downloading icon" << url;
    }
    else
    {
        QPixmap pix;
        pix.loadFromData(reply->readAll());
        QIcon icon(pix);

        for (int i=0; i<ui->list->count(); i++)
        {
            QVariantMap m = ui->list->item(i)->data(Qt::UserRole).toMap();
            ui->list->setIconSize(QSize(40,40));
            if (m.value("icon") == originalurl)
            {
                ui->list->item(i)->setIcon(icon);
            }
        }
    }
    if (--_numIconsToDownload == 0 && _qpd)
    {
        _qpd->hide();
        _qpd->deleteLater();
        _qpd = NULL;
    }

    reply->deleteLater();
}

QList<QListWidgetItem *> MainWindow::selectedItems()
{
    QList<QListWidgetItem *> selected;

    for (int i=0; i < ui->list->count(); i++)
    {
        QListWidgetItem *item = ui->list->item(i);
        if (item->checkState())
        {
            selected.append(item);
        }
    }

    return selected;
}

void MainWindow::updateNeeded()
{
    bool enableOk = false;
    QColor colorNeededLabel = Qt::black;
    bool bold = false;

    _neededMB = 0;
    QList<QListWidgetItem *> selected = selectedItems();

    foreach (QListWidgetItem *item, selected)
    {
        QVariantMap entry = item->data(Qt::UserRole).toMap();
        _neededMB += entry.value("nominal_size").toUInt();

        if (nameMatchesRiscOS(entry.value("name").toString()))
        {
            /* RiscOS needs to start at a predetermined sector, calculate the extra space needed for that */
            uint startSector = getFileContents(sysclassblock(_drive, 5)+"/start").trimmed().toUInt()+getFileContents(sysclassblock(_drive, 5)+"/size").trimmed().toUInt();
            if (RISCOS_SECTOR_OFFSET > startSector)
            {
                _neededMB += (RISCOS_SECTOR_OFFSET - startSector)/2048;
            }
        }
    }

    ui->neededLabel->setText(QString("%1: %2 MB").arg(tr("Needed"), QString::number(_neededMB)));
    ui->availableLabel->setText(QString("%1: %2 MB").arg(tr("Available"), QString::number(_availableMB)));

    if (_neededMB > _availableMB)
    {
        /* Selection exceeds available space, make label red to alert user */
        colorNeededLabel = Qt::red;
        bold = true;
    }
    else
    {
        if (_neededMB)
        {
            /* Enable OK button if a selection has been made that fits on the card */
            enableOk = true;
        }
    }

    ui->actionWrite_image_to_disk->setEnabled(enableOk);
    QPalette p = ui->neededLabel->palette();
    if (p.color(QPalette::WindowText) != colorNeededLabel)
    {
        p.setColor(QPalette::WindowText, colorNeededLabel);
        ui->neededLabel->setPalette(p);
    }
    QFont font = ui->neededLabel->font();
    font.setBold(bold);
    ui->neededLabel->setFont(font);
}

void MainWindow::on_list_itemChanged(QListWidgetItem *)
{
    updateNeeded();
}

void MainWindow::downloadMetaFile(const QString &urlstring, const QString &saveAs)
{
    qDebug() << "Downloading" << urlstring << "to" << saveAs;
    _numMetaFilesToDownload++;
    QUrl url(urlstring);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::User, saveAs);
    QNetworkReply *reply = _netaccess->get(request);
    connect(reply, SIGNAL(finished()), this, SLOT(downloadMetaRedirectCheck()));
}

void MainWindow::downloadListRedirectCheck()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    int httpstatuscode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString redirectionurl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toString();

    if (httpstatuscode > 300 && httpstatuscode < 400)
    {
        qDebug() << "Redirection - Re-trying download from" << redirectionurl;
        downloadList(redirectionurl);
    }
    else
        downloadListComplete();
}

void MainWindow::downloadIconRedirectCheck()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    int httpstatuscode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString redirectionurl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toString();
    QString originalurl = reply->request().attribute(QNetworkRequest::User).toString();;

    if (httpstatuscode > 300 && httpstatuscode < 400)
    {
        qDebug() << "Redirection - Re-trying download from" << redirectionurl;
        downloadIcon(redirectionurl, originalurl);
    }
    else
        downloadIconComplete();
}

void MainWindow::downloadMetaRedirectCheck()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    int httpstatuscode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString redirectionurl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toString();
    QString saveAs = reply->request().attribute(QNetworkRequest::User).toString();

    if (httpstatuscode > 300 && httpstatuscode < 400)
    {
        qDebug() << "Redirection - Re-trying download from" << redirectionurl;
        _numMetaFilesToDownload--;
        downloadMetaFile(redirectionurl, saveAs);
    }
    else
        downloadMetaComplete();
}

void MainWindow::downloadMetaComplete()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    int httpstatuscode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (reply->error() != reply->NoError || httpstatuscode < 200 || httpstatuscode > 399)
    {
        if (_qpd)
        {
            _qpd->hide();
            _qpd->deleteLater();
            _qpd = NULL;
        }
        QMessageBox::critical(this, tr("Download error"), tr("Error downloading meta file")+"\n"+reply->url().toString(), QMessageBox::Close);
        setEnabled(true);
    }
    else
    {
        QString saveAs = reply->request().attribute(QNetworkRequest::User).toString();
        QFile f(saveAs);
        f.open(f.WriteOnly);
        if (f.write(reply->readAll()) == -1)
        {
            QMessageBox::critical(this, tr("Download error"), tr("Error writing downloaded file to SD card. SD card or file system may be damaged."), QMessageBox::Close);
            setEnabled(true);
        }
        else
        {
            _numMetaFilesToDownload--;
        }
        f.close();
    }

    if (_numMetaFilesToDownload == 0)
    {
        if (_qpd)
        {
            _qpd->hide();
            _qpd->deleteLater();
            _qpd = NULL;
        }
        startImageWrite();
    }
}

void MainWindow::startImageWrite()
{
    /* All meta files downloaded, extract slides tarball, and launch image writer thread */
    MultiImageWriteThread *imageWriteThread = new MultiImageWriteThread(_bootdrive, _drive);
    QString folder, slidesFolder;
    QStringList slidesFolders;

    QList<QListWidgetItem *> selected = selectedItems();
    foreach (QListWidgetItem *item, selected)
    {
        QVariantMap entry = item->data(Qt::UserRole).toMap();

        if (entry.contains("folder"))
        {
            /* Local image */
            folder = entry.value("folder").toString();
        }
        else
        {
            folder = "/settings/os/"+entry.value("name").toString();
            folder.replace(' ', '_');

            QString marketingTar = folder+"/marketing.tar";
            if (QFile::exists(marketingTar))
            {
                /* Extract tarball with slides */
                QProcess::execute("tar xf "+marketingTar+" -C "+folder);
                QFile::remove(marketingTar);
            }

            /* Insert tarball download URL information into partition_info.json */
            QVariantMap json = Json::loadFromFile(folder+"/partitions.json").toMap();
            QVariantList partitions = json["partitions"].toList();
            int i=0;
            QStringList tarballs = entry.value("tarballs").toStringList();
            foreach (QString tarball, tarballs)
            {
                QVariantMap partition = partitions[i].toMap();
                partition.insert("tarball", tarball);
                partitions[i] = partition;
                i++;
            }
            json["partitions"] = partitions;
            Json::saveToFile(folder+"/partitions.json", json);
        }

        slidesFolder.clear();
        //QRect s = QApplication::desktop()->screenGeometry();
        //if (s.width() > 640 && QFile::exists(folder+"/slides"))
        //{
        //    slidesFolder = folder+"/slides";
        //}
        if (QFile::exists(folder+"/slides_vga"))
        {
            slidesFolder = folder+"/slides_vga";
        }
        imageWriteThread->addImage(folder, entry.value("name").toString());
        if (!slidesFolder.isEmpty())
            slidesFolders.append(slidesFolder);
    }

    if (slidesFolders.isEmpty())
        slidesFolder.append("/mnt/defaults/slides");

    _qpd = new ProgressSlideshowDialog(slidesFolders, "", 20, _drive, this);
    connect(imageWriteThread, SIGNAL(parsedImagesize(qint64)), _qpd, SLOT(setMaximum(qint64)));
    connect(imageWriteThread, SIGNAL(completed()), this, SLOT(onCompleted()));
    connect(imageWriteThread, SIGNAL(error(QString)), this, SLOT(onError(QString)));
    connect(imageWriteThread, SIGNAL(statusUpdate(QString)), _qpd, SLOT(setLabelText(QString)));
    connect(imageWriteThread, SIGNAL(runningMKFS()), _qpd, SLOT(pauseIOaccounting()), Qt::BlockingQueuedConnection);
    connect(imageWriteThread, SIGNAL(finishedMKFS()), _qpd , SLOT(resumeIOaccounting()), Qt::BlockingQueuedConnection);
    connect(imageWriteThread, SIGNAL(newDrive(const QString&)), _qpd , SLOT(changeDrive(const QString&)), Qt::BlockingQueuedConnection);
    imageWriteThread->start();
    hide();
    _qpd->exec();
}

void MainWindow::hideDialogIfNoNetwork()
{
    if (_qpd)
    {
        if (!isOnline())
        {
            /* No network cable inserted */
            _qpd->hide();
            _qpd->deleteLater();
            _qpd = NULL;

            if (ui->list->count() == 0)
            {
                /* No local images either */
                if (_hasWifi)
                {
                    QMessageBox::critical(this,
                                          tr("No network access"),
                                          tr("Network access is required to use NOOBS without local images. Please select your wifi network in the next screen."),
                                          QMessageBox::Close);
                    on_actionWifi_triggered();
                }
                else
                {
                    QMessageBox::critical(this,
                                          tr("No network access"),
                                          tr("Wired network access is required to use NOOBS without local images. Please insert a network cable into the network port."),
                                          QMessageBox::Close);
                }
            }
        }
    }
}

void MainWindow::on_actionWifi_triggered()
{
    bool wasAlreadyOnlineBefore = !_networkStatusPollTimer.isActive();

    WifiSettingsDialog wsd;
    if ( wsd.exec() == wsd.Accepted )
    {
        if (wasAlreadyOnlineBefore)
        {
            /* Try to redownload list. Could have failed through previous access point */
            downloadLists();
        }
    }
}

void MainWindow::pollForNewDisks()
{
    QString dirname = "/sys/class/block";
    QDir dir(dirname);
    QStringList list = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    if (list.count() != _devlistcount)
    {
        foreach (QString devname, list)
        {
            QString blocklink = QFile::symLinkTarget(dirname+"/"+devname);
            /* skip virtual things and partitions */
            if (blocklink.contains("/devices/virtual/") || QFile::exists(blocklink+"/partition") )
                continue;

            QByteArray inflight = getFileContents(sysclassblock(devname)+"/inflight").trimmed();
            if (inflight.count() && inflight.left(1) != "0")
            {
                /* There are currently outstanding IO request, which could mean it hasn't finished
                 * reading the partition table. Check again on next round */
                return;
            }

            /* does the drive perhaps have a FAT partition with extra images? */
            if ("/dev/"+devname != _bootdrive && QFile::exists(sysclassblock(devname, 1)))
            {
                QString p1 = partdev(devname, 1);

                if (!QFile::exists("/dev/"+p1))
                {
                    /* /dev node not created yet. Check again on next round */
                    return;
                }

                if (_usbimages && !QFile::exists("/tmp/media/"+p1))
                    addImagesFromUSB(p1);
            }

            /* is the drive writable? */
            if (getFileContents(blocklink+"/ro").trimmed() == "1")
                continue;

            QString model = getFileContents(dirname+"/"+devname+"/device/model").trimmed();
            if (model.isEmpty())
                model = getFileContents(dirname+"/"+devname+"/device/name").trimmed();
            QIcon icon;

            if (devname.startsWith("mmc"))
            {
                icon = QIcon(":/icons/hdd.png");
            }
            else
            {
                icon = QIcon(":/icons/hdd_usb_unmount.png");
            }

            if (ui->targetCombo->findData(devname) == -1)
            {
                ui->targetCombo->addItem(icon, devname+": "+model, devname);

                /* does the partition structure look like a preloaded Pi drive, then select it by default? */
                if (devname == "sda"
                        && QFile::exists(sysclassblock(devname, 1))
                        && QFile::exists(sysclassblock(devname, 5))
                        && getFileContents(sysclassblock(devname, 5)+"/size").trimmed().toInt() == SETTINGS_PARTITION_SIZE)
                    ui->targetCombo->setCurrentIndex(ui->targetCombo->count()-1);
            }
        }

        if (ui->targetCombo->count() > 1)
        {
            ui->targetLabel->setHidden(false);
            ui->targetCombo->setHidden(false);
        }

        _devlistcount = list.count();
    }
}

void MainWindow::on_targetCombo_currentIndexChanged(int index)
{
    if (index != -1)
    {
        QString devname = ui->targetCombo->itemData(index).toString();

        if (devname != "mmcblk0" && (
                   !QFile::exists(sysclassblock(devname, 1))
                || !QFile::exists(sysclassblock(devname, 5))
                || getFileContents(sysclassblock(devname, 5)+"/size").trimmed().toInt() != SETTINGS_PARTITION_SIZE))
        {
            if (QMessageBox::question(this,
                                      tr("Reformat drive?"),
                                      tr("Are you sure you want to reformat the drive '%1' for use with NOOBS? All existing data on the drive will be deleted!").arg(devname),
                                      QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes)
            {
                InitDriveThread idt("/dev/"+devname);
                idt.formatUsbDrive();
            }
            else
            {
                int idx = ui->targetCombo->findData("mmcblk0");
                if (idx != -1 && idx != index)
                    ui->targetCombo->setCurrentIndex(idx);
                return;
            }
        }

        qDebug() << "New drive selected:" << devname;
        _drive = "/dev/"+devname;
        _availableMB = (getFileContents(sysclassblock(_drive)+"/size").trimmed().toUInt()-getFileContents(sysclassblock(_drive, 5)+"/start").trimmed().toUInt()-getFileContents(sysclassblock(_drive, 5)+"/size").trimmed().toUInt())/2048;
        filterList();
        updateNeeded();
    }
}

void MainWindow::addImagesFromUSB(const QString &device)
{
    QDir dir;
    QString mntpath = "/tmp/media/"+device;

    dir.mkpath(mntpath);
    if (QProcess::execute("mount -o ro -t vfat /dev/"+device+" "+mntpath) != 0)
    {
        dir.rmdir(mntpath);
        return;
    }

    if (!QFile::exists(mntpath+"/os"))
    {
        QProcess::execute("umount "+mntpath);
        dir.rmdir(mntpath);
        return;
    }

    QIcon usbIcon(":/icons/hdd_usb_unmount.png");
    QMap<QString,QVariantMap> images = listImages(mntpath+"/os", false);

    foreach (QVariant v, images.values())
    {
        QVariantMap m = v.toMap();
        QString name = m.value("name").toString();
        QString folder  = m.value("folder").toString();

        QListWidgetItem *witem = findItem(name);
        if (witem)
        {
            QVariantMap existing_details = witem->data(Qt::UserRole).toMap();

            if ((existing_details["release_date"].toString() <= m["release_date"].toString()))
            {
                /* Existing item in list is same version or older. Prefer image on USB storage. */
                m.insert("installed", existing_details.value("installed", false));
                if (existing_details.contains("partitions"))
                {
                    m["partitions"] = existing_details["partitions"];
                }
                witem->setData(Qt::UserRole, m);
                witem->setData(SecondIconRole, usbIcon);
                ui->list->update();
            }
        }
        else
        {
            /* It's a new OS, so add it to the list */
            QString description = m.value("description").toString();
            QString iconFilename = m.value("icon").toString();
            bool recommended = m.value("recommended").toBool();

            if (!iconFilename.isEmpty() && !iconFilename.contains('/'))
                iconFilename = folder+"/"+iconFilename;
            if (!QFile::exists(iconFilename))
            {
                iconFilename = folder+"/"+name+".png";
                iconFilename.replace(' ', '_');
            }

            QString friendlyname = name;
            if (recommended)
                friendlyname += " ["+tr("RECOMMENDED")+"]";
            if (!description.isEmpty())
                friendlyname += "\n"+description;

            witem = new QListWidgetItem(friendlyname);
            witem->setCheckState(Qt::Unchecked);
            witem->setData(Qt::UserRole, m);
            witem->setData(SecondIconRole, usbIcon);
            if (QFile::exists(iconFilename))
                witem->setIcon(QIcon(iconFilename));

            if (recommended)
                ui->list->insertItem(0, witem);
            else
                ui->list->addItem(witem);
        }
    }

    filterList();
}

/* Dynamically hide items from list depending on target drive */
void MainWindow::filterList()
{
    for (int i=0; i < ui->list->count(); i++)
    {
        QListWidgetItem *witem = ui->list->item(i);

        if (_drive == "/dev/mmcblk0")
        {
            witem->setHidden(false);
        }
        else
        {
            QVariantMap m = witem->data(Qt::UserRole).toMap();
            bool supportsUsb;
            QString param;

            if (_bootdrive == "/dev/mmcblk0")
                param = "supports_usb_root";
            else
                param = "supports_usb_boot";


            /* If the repo explicity states wheter or not usb is supported use that info */
            if (m.contains(param))
            {
                supportsUsb = m.value(param).toBool();
            }
            else
            {
                /* Otherwise just assume Linux does, and RiscOS and Windows do not */
                QString name = m.value("name").toString();
                supportsUsb = (!nameMatchesRiscOS(name) && !name.contains("Windows", Qt::CaseInsensitive));
            }

            if (supportsUsb)
            {
                witem->setHidden(false);
            }
            else
            {
                witem->setCheckState(Qt::Unchecked);
                witem->setHidden(true);
            }
        }
    }
}
