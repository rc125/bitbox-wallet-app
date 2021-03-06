#include <QApplication>
#include <QWebEngineView>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QWebChannel>
#include <QWebEngineUrlRequestInterceptor>
#include <QContextMenuEvent>
#include <QMenu>
#include <QThread>
#include <QMutex>
#include <QResource>
#include <QByteArray>
#include <QSettings>
#include <iostream>
#include <string>
#include <set>

#include "libserver.h"
#include "webclass.h"

static QWebEngineView* view;
static bool pageLoaded = false;
static WebClass* webClass;
static QMutex webClassMutex;

class RequestInterceptor : public QWebEngineUrlRequestInterceptor {
public:
    explicit RequestInterceptor() : QWebEngineUrlRequestInterceptor() { }
    void interceptRequest(QWebEngineUrlRequestInfo& info) override {
        if (info.requestUrl().scheme() != "qrc" && info.requestUrl().scheme() != "blob") {
            info.block(true);
        }
    };
};

class WebEngineView : public QWebEngineView {
public:
    void closeEvent(QCloseEvent*) override {
        QSettings settings;
        settings.setValue("mainWindowGeometry", saveGeometry());
    }

    QSize sizeHint() const override {
        // Default initial window size.
        return QSize(1257, 675);
    }

    void contextMenuEvent(QContextMenuEvent *event) override {
        std::set<QAction*> whitelist = {
            page()->action(QWebEnginePage::Cut),
            page()->action(QWebEnginePage::Copy),
            page()->action(QWebEnginePage::Paste),
            page()->action(QWebEnginePage::Undo),
            page()->action(QWebEnginePage::Redo),
            page()->action(QWebEnginePage::SelectAll),
            page()->action(QWebEnginePage::CopyLinkToClipboard),
            page()->action(QWebEnginePage::Unselect),
        };
        QMenu *menu = page()->createStandardContextMenu();
        for (const auto action : menu->actions()) {
            if (whitelist.find(action) == whitelist.cend()) {
                menu->removeAction(action);
            }
        }
        if (!menu->isEmpty()) {
            menu->popup(event->globalPos());
        }
    }
};

int main(int argc, char *argv[])
{
    // note: doesn't work as expected. Users with hidpi enabled should set the environment flag themselves
    // turn on the DPI support**
// #if QT_VERSION >= QT_VERSION_CHECK(5,6,0)
//     QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
// #else
//     qputenv("QT_AUTO_SCREEN_SCALE_FACTOR", QByteArray("1"));
// #endif // QT_VERSION

// QT configuration parameters which change the attack surface for memory
// corruption vulnerabilities
#if QT_VERSION >= QT_VERSION_CHECK(5,8,0)
    qputenv("QT_ENABLE_REGEXP_JIT", "0");
    qputenv("QV4_FORCE_INTERPRETER", "1");
    qputenv("DRAW_USE_LLVM", "0");
#endif
#if QT_VERSION >= QT_VERSION_CHECK(5,11,0)
    qputenv("QMLSCENE_DEVICE", "softwarecontext");
    qputenv("QT_QUICK_BACKEND", "software");
#endif


    QApplication a(argc, argv);
    a.setApplicationName(QString("BitBox Wallet"));
    a.setOrganizationDomain("shiftcrypto.ch");
    a.setOrganizationName("Shift Cryptosecurity");
    view = new WebEngineView();
    view->setGeometry(0, 0, a.devicePixelRatio() * view->width(), a.devicePixelRatio() * view->height());
    view->setMinimumSize(650, 375);

    QSettings settings;
    if (settings.contains("mainWindowGeometry")) {
        // std::cout << settings.fileName().toStdString() << std::endl;
        view->restoreGeometry(settings.value("mainWindowGeometry").toByteArray());
    } else {
        view->adjustSize();
    }

    pageLoaded = false;
    QObject::connect(view, &QWebEngineView::loadFinished, [](bool ok){ pageLoaded = ok; });

    QResource::registerResource(QCoreApplication::applicationDirPath() + "/assets.rcc");

    QThread workerThread;
    webClass = new WebClass();
    // Run client queries in a separate to not block the UI.
    webClass->moveToThread(&workerThread);
    workerThread.start();

    serve([](const char* msg) {
            if (!pageLoaded) return;
            webClassMutex.lock();
            if (webClass != nullptr) {
                webClass->pushNotify(QString(msg));
            }
            webClassMutex.unlock();
        },
        [](int queryID, const char* msg) {
            if (!pageLoaded) return;
            webClassMutex.lock();
            if (webClass != nullptr) {
                webClass->gotResponse(queryID, QString(msg));
            }
            webClassMutex.unlock();
        }
        );

    RequestInterceptor interceptor;
    view->page()->profile()->setRequestInterceptor(&interceptor);
    QWebChannel channel;
    channel.registerObject("backend", webClass);
    view->page()->setWebChannel(&channel);
    view->show();
    view->load(QUrl("qrc:/index.html"));

    QObject::connect(&a, &QApplication::aboutToQuit, [&]() {
            webClassMutex.lock();
            channel.deregisterObject(webClass);
            delete webClass;
            webClass = nullptr;
            webClassMutex.unlock();
            workerThread.quit();
            workerThread.wait();
        });

    return a.exec();
}
