#include "mainwindow.h"
#include "optionswindow.h"

#include <QtTest>
#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

// Real HTTP responses make the tests cover QNetworkAccessManager and event-loop
// ordering, rather than assuming the refresh/validation callbacks were invoked.
class HttpFixture : public QObject {
public:
    struct Response {
        QByteArray body;
        int status;
        bool held;
        Response(QByteArray body = QByteArray(), int status = 200, bool held = false)
            : body(body), status(status), held(held) {}
    };

    explicit HttpFixture(QObject *parent = nullptr) : QObject(parent) {
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                QTcpSocket *socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("answered").toBool())
                        return;
                    QByteArray request = socket->property("request").toByteArray();
                    request += socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n"))
                        return;
                    socket->setProperty("answered", true);
                    const QByteArray path = request.split(' ').value(1);
                    const int index = counts.value(path, 0);
                    counts[path] = index + 1;
                    const QList<Response> available = responses.value(path);
                    const Response response = available.isEmpty()
                        ? Response("not found", 404)
                        : available.at(qMin(index, available.size() - 1));
                    if (response.held) {
                        pending.append(Pending{path, socket, response});
                    } else {
                        send(socket, response);
                    }
                });
            }
        });
    }

    bool listen() { return server.listen(QHostAddress::LocalHost); }
    QString url(const QByteArray &path) const {
        return QString("http://127.0.0.1:%1%2")
            .arg(server.serverPort()).arg(QString::fromLatin1(path));
    }
    void set(const QByteArray &path, const QList<Response> &sequence) { responses[path] = sequence; }
    int count(const QByteArray &path) const { return counts.value(path, 0); }
    void release(const QByteArray &path) {
        for (int i = pending.size() - 1; i >= 0; --i) {
            if (pending.at(i).path == path) {
                const Pending response = pending.takeAt(i);
                if (response.socket)
                    send(response.socket, response.response);
            }
        }
    }

private:
    struct Pending {
        QByteArray path;
        QPointer<QTcpSocket> socket;
        Response response;
    };
    static void send(QTcpSocket *socket, const Response &response) {
        QByteArray header = "HTTP/1.1 " + QByteArray::number(response.status)
            + (response.status == 200 ? " OK\r\n" : " Error\r\n");
        header += "Content-Type: application/xml\r\nContent-Length: "
            + QByteArray::number(response.body.size()) + "\r\nConnection: close\r\n\r\n";
        socket->write(header + response.body);
        socket->disconnectFromHost();
    }
    QTcpServer server;
    QHash<QByteArray, QList<Response> > responses;
    QHash<QByteArray, int> counts;
    QList<Pending> pending;
};

class LaunchRefreshTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
#ifdef Q_OS_WIN
        QSKIP("The integration launch marker uses a POSIX shell script; run on Linux/macOS.");
#endif
        previousDirectory = QDir::currentPath();
        shownErrors.clear();
        temporary = new QTemporaryDir;
        QVERIFY(temporary->isValid());
        QVERIFY(QDir::setCurrent(temporary->path()));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary->path());
        QCoreApplication::setOrganizationName("SweetTeaTests");
        QCoreApplication::setApplicationName("LaunchRefresh");
        http = new HttpFixture(this);
        QVERIFY(http->listen());
        const QByteArray script("#!/bin/sh\nprintf '%s\\n' \"$*\" >> launches.txt\n");
        QVERIFY(writeFile("launch-client.sh", script));
        QVERIFY(QFile::setPermissions("launch-client.sh", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(writeFile("payload.dat", "old payload"));
        QVERIFY(writeFile("log.txt", QByteArray()));
        // Production errors may be modal. Dismiss them so a failure can still be
        // asserted without hanging the test runner inside a nested event loop.
        connect(&dismissErrors, &QTimer::timeout, this, [this] {
            const auto widgets = QApplication::topLevelWidgets();
            for (QWidget *widget : widgets) {
                QMessageBox *message = qobject_cast<QMessageBox *>(widget);
                if (message && message->isVisible()) {
                    shownErrors.append(message->text());
                    message->accept();
                }
            }
        });
        dismissErrors.start(20);
    }

    void cleanup() {
        dismissErrors.stop();
        disconnect(&dismissErrors, nullptr, this, nullptr);
        delete window;
        window = nullptr;
        delete http;
        http = nullptr;
        QDir::setCurrent(previousDirectory);
        delete temporary;
        temporary = nullptr;
    }

    void unchangedLaunchFetchesFreshManifest() {
        const QByteArray original = manifest("old payload", "original");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(original)});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(http->count("/manifest.xml"), 2);
        QTRY_COMPARE(launchLines().size(), 1);
        QTest::qWait(100);
        QCOMPARE(launchLines().size(), 1);
        QVERIFY(launchLines().first().contains("original"));
    }

    void changedManifestWaitsForValidationAndUsesFreshArguments() {
        const QByteArray original = manifest("old payload", "original");
        const QByteArray updatedPayload("new payload with a different checksum");
        const QByteArray updated = manifest(updatedPayload, "fresh-argument");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(updated)});
        http->set("/payload", {HttpFixture::Response(updatedPayload, 200, true)});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(http->count("/payload"), 1);
        QCOMPARE(http->count("/manifest.xml"), 2);
        QVERIFY(launchLines().isEmpty());
        QVERIFY(!launch()->isEnabled());
        QVERIFY(!validate()->isEnabled());
        QVERIFY(!options()->isEnabled());
        http->release("/payload");
        QTRY_COMPARE(launchLines().size(), 1);
        QVERIFY(launchLines().first().contains("fresh-argument"));
        QVERIFY(!launchLines().first().contains("original"));
        QCOMPARE(readFile("payload.dat"), updatedPayload);
        QSettings settings;
        QCOMPARE(settings.value("manifestChecksum").toByteArray(),
                 QCryptographicHash::hash(updated, QCryptographicHash::Md5));
    }

    void refreshErrorsFailClosed_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("http-error") << 503 << QByteArray("temporarily unavailable");
        QTest::newRow("malformed-xml") << 200 << QByteArray("<manifest><launch");
        QTest::newRow("empty-response") << 200 << QByteArray();
        QTest::newRow("missing-profile") << 200 << QByteArray("<manifest/>");
    }

    void refreshErrorsFailClosed() {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        const QByteArray original = manifest("old payload", "original");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(body, status)});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(http->count("/manifest.xml"), 2);
        QTRY_VERIFY(options()->isEnabled());
        QTest::qWait(100);
        QVERIFY(launchLines().isEmpty());
        QCOMPARE(readFile("payload.dat"), QByteArray("old payload"));
    }

    void repeatedClicksWhileRefreshingLaunchOnlyOnce() {
        const QByteArray original = manifest("old payload", "original");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(original, 200, true)});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(http->count("/manifest.xml"), 2);
        QVERIFY(!launch()->isEnabled());
        QVERIFY(!validate()->isEnabled());
        QVERIFY(!profiles()->isEnabled());
        QVERIFY(!options()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTest::mouseClick(options(), Qt::LeftButton);
        QTest::qWait(50);
        QCOMPARE(http->count("/manifest.xml"), 2);
        QVERIFY(launchLines().isEmpty());
        QVERIFY(!window->findChild<OptionsWindow *>());
        http->release("/manifest.xml");
        QTRY_COMPARE(launchLines().size(), 1);
        QTest::qWait(100);
        QCOMPARE(launchLines().size(), 1);
    }

    void payloadFailureTerminatesWithoutLaunching_data() {
        QTest::addColumn<int>("status");
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("http-error") << 503 << QByteArray("unavailable");
        QTest::newRow("bad-checksum") << 200 << QByteArray("corrupt payload");
    }

    void payloadFailureTerminatesWithoutLaunching() {
        QFETCH(int, status);
        QFETCH(QByteArray, body);
        const QByteArray original = manifest("old payload", "original");
        const QByteArray updated = manifest("new payload", "fresh-argument");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(updated)});
        http->set("/payload", {HttpFixture::Response(body, status)});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_VERIFY(http->count("/payload") >= 1);
        QTRY_VERIFY(options()->isEnabled());
        QTest::qWait(100);
        QVERIFY(launchLines().isEmpty());
        QCOMPARE(http->count("/payload"), 1);
        QSettings settings;
        QVERIFY(settings.value("manifestChecksum").toByteArray()
                != QCryptographicHash::hash(updated, QCryptographicHash::Md5));
    }

    void optionsBlockLaunchAndReloadTheSelectedSource() {
        const QByteArray original = manifest("old payload", "original");
        const QByteArray replacement = manifest("old payload", "replacement", "Replacement Server");
        http->set("/manifest.xml", {HttpFixture::Response(original)});
        http->set("/replacement.xml", {HttpFixture::Response(replacement, 200, true)});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(options(), Qt::LeftButton);
        OptionsWindow *dialog = window->findChild<OptionsWindow *>();
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->isVisible());
        QVERIFY(!launch()->isEnabled());
        QVERIFY(!validate()->isEnabled());
        QListWidget *sources = dialog->findChild<QListWidget *>("ManifestList");
        QVERIFY(sources);
        sources->clear();
        sources->addItem(http->url("/replacement.xml"));
        QTest::mouseClick(dialog->findChild<QPushButton *>("ApplyButton"), Qt::LeftButton);
        QTRY_COMPARE(http->count("/replacement.xml"), 1);
        QVERIFY(!launch()->isEnabled());
        QVERIFY(launchLines().isEmpty());
        http->release("/replacement.xml");
        QTRY_COMPARE(profiles()->count(), 1);
        QCOMPARE(profiles()->item(0)->text(), QString("Replacement Server"));
        QVERIFY(launchLines().isEmpty());
    }

    void staleManifestResponseCannotRepopulateReloadedList() {
        const QByteArray original = manifest("old payload", "original");
        const QByteArray stale = manifest("old payload", "stale", "Stale Server");
        const QByteArray replacement = manifest("old payload", "replacement", "Replacement Server");
        http->set("/manifest.xml", {HttpFixture::Response(original)});
        http->set("/stale.xml", {HttpFixture::Response(stale, 200, true)});
        http->set("/replacement.xml", {HttpFixture::Response(replacement)});
        start(original, http->url("/manifest.xml") + " " + http->url("/stale.xml"));
        QTRY_COMPARE(profiles()->count(), 1);
        QTRY_COMPARE(http->count("/stale.xml"), 1);
        QVERIFY(options()->isEnabled());
        QTest::mouseClick(options(), Qt::LeftButton);
        OptionsWindow *dialog = window->findChild<OptionsWindow *>();
        QVERIFY(dialog);
        QListWidget *sources = dialog->findChild<QListWidget *>("ManifestList");
        sources->clear();
        sources->addItem(http->url("/replacement.xml"));
        QTest::mouseClick(dialog->findChild<QPushButton *>("ApplyButton"), Qt::LeftButton);
        QTRY_COMPARE(http->count("/replacement.xml"), 1);
        QTRY_COMPARE(profiles()->count(), 1);
        QCOMPARE(profiles()->item(0)->text(), QString("Replacement Server"));
        http->release("/stale.xml");
        QTest::qWait(150);
        QCOMPARE(profiles()->count(), 1);
        QCOMPARE(profiles()->item(0)->text(), QString("Replacement Server"));
        QVERIFY(launchLines().isEmpty());
    }

    void staleMetadataResponsesCannotTouchReloadedRows() {
        QByteArray original = manifest("old payload", "original");
        original.replace("<launch exec=", "<launch motd=\"" + http->url("/old-motd").toUtf8()
                         + "\" icon=\"" + http->url("/old-icon").toUtf8() + "\" exec=");
        const QByteArray replacement = manifest("old payload", "replacement", "Replacement Server");
        http->set("/manifest.xml", {HttpFixture::Response(original)});
        http->set("/replacement.xml", {HttpFixture::Response(replacement)});
        http->set("/old-motd", {HttpFixture::Response("Obsolete MOTD", 200, true)});
        const QByteArray png = QByteArray::fromBase64(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l9sAAAAASUVORK5CYII=");
        http->set("/old-icon", {HttpFixture::Response(png, 200, true)});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        QTRY_COMPARE(http->count("/old-motd"), 1);
        QTRY_COMPARE(http->count("/old-icon"), 1);
        QTest::mouseClick(options(), Qt::LeftButton);
        OptionsWindow *dialog = window->findChild<OptionsWindow *>();
        QVERIFY(dialog);
        QListWidget *sources = dialog->findChild<QListWidget *>("ManifestList");
        sources->clear();
        sources->addItem(http->url("/replacement.xml"));
        QTest::mouseClick(dialog->findChild<QPushButton *>("ApplyButton"), Qt::LeftButton);
        QTRY_COMPARE(http->count("/replacement.xml"), 1);
        QTRY_COMPARE(profiles()->count(), 1);
        QCOMPARE(profiles()->item(0)->text(), QString("Replacement Server"));
        http->release("/old-motd");
        http->release("/old-icon");
        QTest::qWait(150);
        QCOMPARE(profiles()->count(), 1);
        QCOMPARE(profiles()->item(0)->text(), QString("Replacement Server"));
        QVERIFY(profiles()->item(0)->data(Qt::UserRole).toString().isEmpty());
        QVERIFY(profiles()->item(0)->icon().isNull());
        QVERIFY(launchLines().isEmpty());
    }

    void localManifestIsReadAgainBeforeLaunching() {
        const QByteArray original = manifest("old payload", "original");
        const QByteArray updated = manifest("old payload", "local-fresh-argument");
        const QString source = temporary->filePath("manifest.xml");
        QVERIFY(writeFile(source, original));
        start(original, source);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        // Explicit validation supports both historical normalized-XML and raw
        // byte checksum conventions for local manifests.
        QTest::mouseClick(validate(), Qt::LeftButton);
        QTRY_VERIFY(launch()->isEnabled());
        QVERIFY(writeFile(source, updated));
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(launchLines().size(), 1);
        QVERIFY(launchLines().first().contains("local-fresh-argument"));
        QVERIFY(!launchLines().first().contains("original"));
    }

    void refreshedProfileOrderDoesNotChangeTheSelectedServer() {
        const QByteArray original = manifest("old payload", "original");
        QByteArray updated = manifest("old payload", "fresh-target");
        updated.replace("<launch exec=", "<launch exec=\"./launch-client.sh\" params=\"wrong-server\">Other Server</launch><launch exec=");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(updated)});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(launchLines().size(), 1);
        QVERIFY(launchLines().first().contains("fresh-target"));
        QVERIFY(!launchLines().first().contains("wrong-server"));
    }

    void keyboardSelectionRefreshesTheCorrectSource() {
        const QByteArray original = manifest("old payload", "original");
        http->set("/manifest.xml", {HttpFixture::Response(original)});
        http->set("/second.xml", {HttpFixture::Response(original, 200, true), HttpFixture::Response(original)});
        start(original, http->url("/manifest.xml") + " " + http->url("/second.xml"));
        QTRY_COMPARE(profiles()->count(), 1);
        QTRY_COMPARE(http->count("/second.xml"), 1);
        http->release("/second.xml");
        QTRY_COMPARE(profiles()->count(), 2);
        selectFirst();
        profiles()->setFocus();
        QTest::keyClick(profiles(), Qt::Key_Down);
        QCOMPARE(profiles()->currentRow(), 1);
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(http->count("/second.xml"), 2);
        QCOMPARE(http->count("/manifest.xml"), 1);
        QTRY_COMPARE(launchLines().size(), 1);
    }

    void missingOrAmbiguousSelectedProfileBlocksLaunch_data() {
        QTest::addColumn<QString>("mode");
        QTest::newRow("removed-but-other-profile-exists") << QString("removed");
        QTest::newRow("duplicate-selected-name") << QString("duplicate");
        QTest::newRow("empty-selected-executable") << QString("empty");
    }

    void missingOrAmbiguousSelectedProfileBlocksLaunch() {
        QFETCH(QString, mode);
        const QByteArray original = manifest("old payload", "original");
        QByteArray updated = manifest("old payload", "fresh-argument");
        if (mode == "removed") {
            updated.replace("Test Server", "Other Server");
        } else if (mode == "duplicate") {
            updated.replace("</manifest>", "<launch exec=\"./launch-client.sh\" params=\"other\">Test Server</launch></manifest>");
        } else {
            updated.replace("exec=\"./launch-client.sh\"", "exec=\"\"");
        }
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(updated)});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(http->count("/manifest.xml"), 2);
        QTRY_VERIFY(options()->isEnabled());
        QTest::qWait(100);
        QVERIFY(launchLines().isEmpty());
    }

    void destructionWithDelayedResponseCannotLaunch_data() {
        QTest::addColumn<bool>("holdPayload");
        QTest::newRow("during-refresh") << false;
        QTest::newRow("during-payload-download") << true;
    }

    void destructionWithDelayedResponseCannotLaunch() {
        QFETCH(bool, holdPayload);
        const QByteArray original = manifest("old payload", "original");
        const QByteArray updated = manifest("new payload", "fresh-argument");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(updated, 200, !holdPayload)});
        http->set("/payload", {HttpFixture::Response("new payload", 200, true)});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        if (holdPayload)
            QTRY_COMPARE(http->count("/payload"), 1);
        else
            QTRY_COMPARE(http->count("/manifest.xml"), 2);
        QVERIFY(launchLines().isEmpty());
        delete window;
        window = nullptr;
        http->release(holdPayload ? "/payload" : "/manifest.xml");
        QTest::qWait(150);
        QVERIFY(launchLines().isEmpty());
    }

    void changedManifestWithNoFilesCompletes() {
        const QByteArray original = manifest("old payload", "original");
        const QByteArray updated("<manifest><launch exec=\"./launch-client.sh\" params=\"empty-manifest\">Test Server</launch></manifest>");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(updated)});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(launchLines().size(), 1);
        QVERIFY(launchLines().first().contains("empty-manifest"));
        QVERIFY(options()->isEnabled());
    }

    void failedMirrorFallsBackAndLaunchesOnce() {
        const QByteArray original = manifest("old payload", "original");
        QByteArray updated = manifest("new payload", "fresh-argument");
        // Tequila manifests list the preferred mirror last.
        updated.replace("<url>", "<url>" + http->url("/fallback").toUtf8() + "</url><url>");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(updated)});
        http->set("/payload", {HttpFixture::Response("unavailable", 503)});
        http->set("/fallback", {HttpFixture::Response("new payload")});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(launchLines().size(), 1);
        QCOMPARE(http->count("/payload"), 1);
        QCOMPARE(http->count("/fallback"), 1);
        QCOMPARE(readFile("payload.dat"), QByteArray("new payload"));
    }

    void failedPayloadCanBeValidatedAndLaunchedOnRetry() {
        const QByteArray original = manifest("old payload", "original");
        const QByteArray updated = manifest("new payload", "fresh-argument");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(updated)});
        http->set("/payload", {HttpFixture::Response("unavailable", 503), HttpFixture::Response("new payload")});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(http->count("/payload"), 1);
        QTRY_VERIFY(validate()->isEnabled());
        QVERIFY(launchLines().isEmpty());
        QVERIFY(!launch()->isEnabled());
        // Close the nonmodal error viewer before interacting with the main UI.
        for (QWidget *widget : QApplication::topLevelWidgets())
            if (widget != window && widget->parentWidget() == window)
                widget->close();
        QTest::mouseClick(validate(), Qt::LeftButton);
        QTRY_COMPARE(http->count("/payload"), 2);
        QTRY_VERIFY(launch()->isEnabled());
        QVERIFY(launchLines().isEmpty());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(http->count("/manifest.xml"), 3);
        QTRY_COMPARE(launchLines().size(), 1);
        QVERIFY(launchLines().first().contains("fresh-argument"));
    }

    void heldManifestTimesOutWithoutLaunching() {
        const QByteArray original = manifest("old payload", "original");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(original, 200, true)});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QElapsedTimer elapsed;
        elapsed.start();
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(http->count("/manifest.xml"), 2);
        while (!options()->isEnabled() && elapsed.elapsed() < 35000)
            QTest::qWait(50);
        QVERIFY2(options()->isEnabled(), "Manifest refresh did not terminate after its 30-second timeout");
        QVERIFY(elapsed.elapsed() >= 25000);
        QVERIFY(launchLines().isEmpty());
        http->release("/manifest.xml");
        QTest::qWait(100);
        QVERIFY(launchLines().isEmpty());
    }

    void unrelatedManifestMetadataSurvivesSelectedManifestRefresh() {
        const QByteArray original = manifest("old payload", "original");
        const QByteArray updated = manifest("old payload", "fresh-argument");
        QByteArray other = manifest("old payload", "other", "Other Server");
        other.replace("<launch exec=", "<launch motd=\"" + http->url("/other-motd").toUtf8() + "\" exec=");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(updated)});
        http->set("/other.xml", {HttpFixture::Response(other)});
        http->set("/other-motd", {HttpFixture::Response("Other server is online", 200, true)});
        start(original, http->url("/manifest.xml") + " " + http->url("/other.xml"));
        QTRY_COMPARE(profiles()->count(), 2);
        QTRY_COMPARE(http->count("/other-motd"), 1);
        QListWidgetItem *selected = profileNamed("Test Server");
        QVERIFY(selected);
        QTest::mouseClick(profiles()->viewport(), Qt::LeftButton, Qt::NoModifier,
                         profiles()->visualItemRect(selected).center());
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(launchLines().size(), 1);
        QCOMPARE(profiles()->count(), 2);
        QVERIFY(profileNamed("Other Server"));
        http->release("/other-motd");
        QTRY_COMPARE(profileNamed("Other Server")->data(Qt::UserRole).toString(),
                     QString("Other server is online"));
    }

    void destinationWriteFailureBlocksLaunch() {
        const QByteArray original = manifest("old payload", "original");
        const QByteArray updated = manifest("new payload", "fresh-argument");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(updated)});
        http->set("/payload", {HttpFixture::Response("new payload")});
        QVERIFY(QFile::remove("payload.dat"));
        QVERIFY(QDir().mkpath("payload.dat"));
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(http->count("/manifest.xml"), 2);
        QTRY_VERIFY(options()->isEnabled());
        QTest::qWait(100);
        QVERIFY(launchLines().isEmpty());
        QVERIFY(!launch()->isEnabled());
        QVERIFY(QFileInfo("payload.dat").isDir());
        QCOMPARE(http->count("/payload"), 0);
        QSettings settings;
        QVERIFY(settings.value("manifestChecksum").toByteArray()
                != QCryptographicHash::hash(updated, QCryptographicHash::Md5));
    }

    void processStartFailureIsReported() {
        const QByteArray original = manifest("old payload", "original");
        QByteArray updated = manifest("old payload", "fresh-argument");
        updated.replace("./launch-client.sh", "./missing-client.sh");
        http->set("/manifest.xml", {HttpFixture::Response(original), HttpFixture::Response(updated)});
        start(original);
        QTRY_COMPARE(profiles()->count(), 1);
        selectFirst();
        QTRY_VERIFY(launch()->isEnabled());
        QTest::mouseClick(launch(), Qt::LeftButton);
        QTRY_COMPARE(http->count("/manifest.xml"), 2);
        QTRY_VERIFY(!shownErrors.isEmpty());
        QVERIFY(shownErrors.join(" ").contains("missing-client.sh"));
        QVERIFY(launchLines().isEmpty());
        QVERIFY(options()->isEnabled());
    }

private:
    static bool writeFile(const QString &name, const QByteArray &data) {
        QFile file(name);
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
    }
    static QByteArray readFile(const QString &name) {
        QFile file(name);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        return file.readAll();
    }
    QByteArray manifest(const QByteArray &payload, const QByteArray &arguments,
                        const QByteArray &name = "Test Server") const {
        return "<manifest><file name=\"payload.dat\" size=\""
            + QByteArray::number(payload.size()) + "\" md5=\""
            + QCryptographicHash::hash(payload, QCryptographicHash::Md5).toHex()
            + "\"><url>" + http->url("/payload").toUtf8()
            + "</url></file><launch exec=\"./launch-client.sh\" params=\""
            + arguments + "\">" + name + "</launch></manifest>";
    }
    void start(const QByteArray &validated, const QString &source = QString()) {
        QSettings settings;
        settings.setValue("checkUpdates", false);
        settings.setValue("manifests", source.isEmpty() ? http->url("/manifest.xml") : source);
        settings.setValue("datadir", temporary->path());
        settings.setValue("manifestChecksum", QCryptographicHash::hash(validated, QCryptographicHash::Md5));
        settings.setValue("oldDir", temporary->path());
        settings.sync();
        window = new MainWindow;
        window->show();
    }
    QListWidget *profiles() const { return window->findChild<QListWidget *>("listWidget"); }
    QPushButton *launch() const { return window->findChild<QPushButton *>("LaunchButton"); }
    QPushButton *validate() const { return window->findChild<QPushButton *>("ValidateButton"); }
    QPushButton *options() const { return window->findChild<QPushButton *>("OptionsButton"); }
    QListWidgetItem *profileNamed(const QString &name) const {
        const QList<QListWidgetItem *> matches = profiles()->findItems(name, Qt::MatchExactly);
        return matches.isEmpty() ? nullptr : matches.first();
    }
    void selectFirst() {
        QListWidget *list = profiles();
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                         list->visualItemRect(list->item(0)).center());
    }
    QList<QByteArray> launchLines() const {
        QFile file(temporary->filePath("launches.txt"));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        QList<QByteArray> lines = file.readAll().split('\n');
        lines.removeAll(QByteArray());
        return lines;
    }

    QString previousDirectory;
    QTemporaryDir *temporary = nullptr;
    HttpFixture *http = nullptr;
    MainWindow *window = nullptr;
    QTimer dismissErrors;
    QStringList shownErrors;
};

QTEST_MAIN(LaunchRefreshTest)
#include "tst_launchrefresh.moc"
