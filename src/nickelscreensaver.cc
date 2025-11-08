#include "nickelscreensaver.h"
#include <NickelHook.h>

#include <QApplication>
#include <QWidget>
#include <QDesktopWidget>
#include <QScreen>
#include <QDir>
#include <QTime>
#include <QPainter>
#include <QFile>
#include <QFileInfo>

typedef void N3PowerWorkflowManager;

void (*N3PowerWorkflowManager_handleSleep)(N3PowerWorkflowManager *_this);
void *(*MainWindowController_sharedInstance)();
QWidget *(*MainWindowController_currentView)(void *);

struct nh_info nickelscreensaver = {
    .name = "Nickel Screensaver",
    .desc = "Transparent screensaver support for Kobo",
    .uninstall_flag = NICKEL_SCREENSAVER_DELETE_FILE,
};

int ns_init() {
    // Only seed qsrand() once
    qsrand(QTime::currentTime().msec());

    return 0;
}

bool ns_uninstall() {
    return true;
}

struct nh_hook nickelscreensaverHook[] = {
    {
        .sym     = "_ZN22N3PowerWorkflowManager11handleSleepEv", 
        .sym_new = "ns_handle_sleep",
        .lib     = "libnickel.so.1.0.0",
        .out     = nh_symoutptr(N3PowerWorkflowManager_handleSleep),
        .desc    = "Handle sleep"
    },
    {0}
};

struct nh_dlsym nickelscreensaverDlsym[] = {
    {
		.name = "_ZN20MainWindowController14sharedInstanceEv",
		.out = nh_symoutptr(MainWindowController_sharedInstance)
	},
	{
		.name = "_ZNK20MainWindowController11currentViewEv",
		.out = nh_symoutptr(MainWindowController_currentView)
	},
	{0}
};

NickelHook(
    .init      = &ns_init,
    .info      = &nickelscreensaver,
    .hook      = nickelscreensaverHook,
    .dlsym     = nickelscreensaverDlsym,
    .uninstall = &ns_uninstall,
);

QString pick_random_file(QDir dir, QStringList files) {
    int idx = qrand() % files.size();
    return dir.filePath(files.at(idx));
}

extern "C" __attribute__((visibility("default"))) void ns_handle_sleep(N3PowerWorkflowManager *_this) {
    QString ns_path   = "/mnt/onboard/.adds/screensaver";
    QString kobo_path = "/mnt/onboard/.kobo/screensaver";
    QDir screensaver_dir(ns_path);
    QDir kobo_screensaver_dir(kobo_path);

    if (!kobo_screensaver_dir.exists()) {
        // Skip if Kobo's screensaver folder doesn't exist
        return N3PowerWorkflowManager_handleSleep(_this);
    }

    QString current_view_name = QString();

    void *mwc = MainWindowController_sharedInstance();
	if (!mwc) {
		nh_log("invalid MainWindowController");
		return N3PowerWorkflowManager_handleSleep(_this);
	}

    QWidget *current_view = MainWindowController_currentView(mwc);
	if (!current_view) {
		nh_log("invalid View");
		return N3PowerWorkflowManager_handleSleep(_this);
	}

    current_view_name = current_view->objectName();
    // Enable transparent mode when reading
    bool is_reading = current_view_name == QStringLiteral("ReadingView");
    bool transparent_mode = is_reading;

    // 1. Check NS's folder
    if (!screensaver_dir.exists()) {
        // Create empty screensaver folder
        screensaver_dir.mkpath(".");
    }

    // 2. Move old overlay_files from .kobo/screensaver to .adds/screensaver
    QStringList exclude = {
        "_config.ini",
        "nickel-screensaver.png",
        "nickel-screensaver.jpg",
    };
    for (const QFileInfo &file : kobo_screensaver_dir.entryInfoList(QDir::Files)) {
        // Don't move Nickel Screensaver's overlay_files
        if (exclude.contains(file.fileName())) {
            continue;
        }

        QString dest_path = ns_path + '/' + file.fileName();
        // Don't override file with the same name in .adds/screensaver
        if (!QFile::exists(dest_path)) {
            QFile::rename(file.filePath(), dest_path);
        }
    }

    // 3. Empty .kobo/screensaver folder
    kobo_screensaver_dir.removeRecursively();
    kobo_screensaver_dir.mkpath(".");

    // 4. Pick a random screensaver
    QStringList overlay_files;
    QPixmap wallpaper;

    if (is_reading) {
        // Check for PNG overlay_files first
        overlay_files = screensaver_dir.entryList(QStringList() << "*.png", QDir::Files);
        // If there is no PNG overlay_files -> check for JPG overlay_files, and switch to non-transparent mode
        if (overlay_files.isEmpty()) {
            overlay_files = screensaver_dir.entryList(QStringList() << "*.jpg", QDir::Files);
            transparent_mode = false;
        }
    } else {
        overlay_files = screensaver_dir.entryList(QStringList() << "*.png" << "*.jpg", QDir::Files);
    }

    if (overlay_files.isEmpty()) {
        // Skip if no overlay_files found
        return N3PowerWorkflowManager_handleSleep(_this);
    }

    if (!is_reading) {
        // Check wallpaper folder
        QDir wallpaper_dir("/mnt/onboard/.adds/screensaver/wallpaper");
        if (wallpaper_dir.exists()) {
            QStringList wallpaper_files = wallpaper_dir.entryList(QStringList() << "*.png" << "*.jpg", QDir::Files);
            if (wallpaper_files.isEmpty()) {
                transparent_mode = false;
            } else {
                transparent_mode = true;
                QString file = pick_random_file(wallpaper_dir, wallpaper_files);
                wallpaper.load(file);
            }
        }
    }

    QString overlay_file = pick_random_file(screensaver_dir, overlay_files);

    // If not transparent mode -> copy the file to .kobo/screensaver
    if (!transparent_mode) {
        QFileInfo info(overlay_file);
        QFile::copy(overlay_file, kobo_path + "/nickel-screensaver." + info.suffix());
        N3PowerWorkflowManager_handleSleep(_this);
        return;
    }

    // 5. Handle transparent mode
    QDesktopWidget* desktopWidget = QApplication::desktop();
    QScreen* screen = QGuiApplication::primaryScreen();
    QSize screen_size = screen->size();

    if (is_reading) {
        // Take screenshot of the current screen if reading
        QRect geometry = current_view->geometry();
        wallpaper = screen->grabWindow(
            desktopWidget->winId(),
            geometry.left(),
            geometry.top(),
            geometry.width(),
            geometry.height()
        );
    }

    // 6. Combine overlay & wallpaper
    QPixmap overlay(overlay_file);
    QImage result(screen_size, QImage::Format_RGB32);
    QPainter painter(&result);

    // Draw wallpaper
    if (wallpaper.size() != screen_size) {
        // Only scales if different sizes
        painter.drawPixmap(0, 0, wallpaper.scaled(screen_size, Qt::KeepAspectRatioByExpanding, Qt::FastTransformation));
    } else {
        painter.drawPixmap(0, 0, wallpaper);
    }

    // Draw overlay
    if (overlay.size() != screen_size) {
        // Only scales if different sizes
        painter.drawPixmap(0, 0, overlay.scaled(screen_size, Qt::KeepAspectRatioByExpanding, Qt::FastTransformation));
    } else {
        painter.drawPixmap(0, 0, overlay);
    }
    painter.end();

    // 7. Save screensaver
    result.save(kobo_path + "/nickel-screensaver.jpg", "JPEG", 100);

    // 8. Done
    N3PowerWorkflowManager_handleSleep(_this);
    // nh_log("Current view: %s", current_view_name.toStdString().c_str());
    // nh_dump_log();
}
