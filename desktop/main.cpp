// SPDX-License-Identifier: GPL-3.0-only
#include <QApplication>
#include <QBitArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>
#include <QVBoxLayout>
#include <functional>
#include <cmath>

struct FileEntry {
    QString absolute;
    QString relative;
    qint64 size = 0;
    qint64 modified = 0;
};

static QByteArray randomHex(int bytes) {
    QByteArray raw(bytes, '\0');
    for (int i = 0; i < bytes; i += 4) {
        quint32 n = QRandomGenerator::system()->generate();
        for (int j = 0; j < 4 && i + j < bytes; ++j)
            raw[i+j] = char((n >> (j*8)) & 0xff);
    }
    return raw.toHex();
}

class MainWindow final : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("MediaBridge — Chelonia");
        resize(790, 540);
        auto *root = new QWidget;
        auto *v = new QVBoxLayout(root);

        auto *heading = new QLabel("MediaBridge / Desktop");
        QFont titleFont = heading->font();
        titleFont.setBold(true);
        titleFont.setPointSize(titleFont.pointSize() + 3);
        heading->setFont(titleFont);
        v->addWidget(heading);

        auto *serverBox = new QGroupBox("Server / Device approval");
        auto *serverLayout = new QVBoxLayout(serverBox);
        auto *serverRow = new QHBoxLayout;
        address = new QLineEdit("http://media-ingest:8081");
        address->setPlaceholderText("http://100.x.y.z:8081");
        serverRow->addWidget(new QLabel("Tailnet address"));
        serverRow->addWidget(address, 1);
        serverLayout->addLayout(serverRow);
        auto *pairRow = new QHBoxLayout;
        approval = new QLabel("Not connected");
        requestButton = new QPushButton("Request access");
        pairRow->addWidget(approval, 1);
        pairRow->addWidget(requestButton);
        serverLayout->addLayout(pairRow);
        v->addWidget(serverBox);

        auto *uploadBox = new QGroupBox("Directory upload");
        auto *uploadLayout = new QVBoxLayout(uploadBox);
        auto *folderRow = new QHBoxLayout;
        directory = new QLineEdit;
        directory->setReadOnly(true);
        chooseButton = new QPushButton("Select folder");
        folderRow->addWidget(directory, 1);
        folderRow->addWidget(chooseButton);
        uploadLayout->addLayout(folderRow);
        auto *optionsRow = new QHBoxLayout;
        optionsRow->addWidget(new QLabel("Parallel chunks"));
        parallelism = new QSpinBox;
        parallelism->setRange(1, 8);
        parallelism->setToolTip("Concurrent 4 MiB chunk requests for a single file.");
        optionsRow->addWidget(parallelism);
        optionsRow->addStretch();
        uploadLayout->addLayout(optionsRow);
        summary = new QLabel("Choose a folder to upload it, including its subdirectories.");
        summary->setWordWrap(true);
        uploadLayout->addWidget(summary);
        current = new QLabel("Idle");
        current->setWordWrap(true);
        uploadLayout->addWidget(current);
        bar = new QProgressBar;
        bar->setRange(0, 1000);
        bar->setValue(0);
        uploadLayout->addWidget(bar);
        auto *metrics = new QGridLayout;
        metrics->addWidget(new QLabel("Speed"), 0, 0);
        metrics->addWidget(new QLabel("Elapsed"), 0, 1);
        metrics->addWidget(new QLabel("ETA"), 0, 2);
        speedLabel = new QLabel("—");
        elapsedLabel = new QLabel("00:00");
        etaLabel = new QLabel("—");
        QFont metricFont = speedLabel->font();
        metricFont.setBold(true);
        speedLabel->setFont(metricFont);
        elapsedLabel->setFont(metricFont);
        etaLabel->setFont(metricFont);
        metrics->addWidget(speedLabel, 1, 0);
        metrics->addWidget(elapsedLabel, 1, 1);
        metrics->addWidget(etaLabel, 1, 2);
        for (int column = 0; column < 3; ++column)
            metrics->setColumnStretch(column, 1);
        uploadLayout->addLayout(metrics);
        auto *buttonRow = new QHBoxLayout;
        startButton = new QPushButton("Start upload");
        pauseButton = new QPushButton("Pause");
        buttonRow->addWidget(startButton);
        buttonRow->addWidget(pauseButton);
        buttonRow->addStretch();
        uploadLayout->addLayout(buttonRow);
        v->addWidget(uploadBox);

        logView = new QTextEdit;
        logView->setReadOnly(true);
        logView->setPlaceholderText("Transfer log");
        v->addWidget(logView, 1);
        setCentralWidget(root);

        // Deliberately do not set an application-wide stylesheet or style.
        // Use the platform-selected KDE/Fusion QStyle, palette and fonts.
        manager.setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        controlTimer = new QTimer(this);
        controlTimer->setSingleShot(true);
        connect(controlTimer, &QTimer::timeout, this, [this] {
            if (!busy || paused || controlPending) return;
            if (phase == Phase::Starting) startNext();
            else if (phase == Phase::Finalizing) finalize();
        });
        metricsTimer = new QTimer(this);
        metricsTimer->setInterval(1000);
        connect(metricsTimer, &QTimer::timeout, this, [this]{ updateMetrics(); });
        loadIdentity();
        connect(requestButton, &QPushButton::clicked, this, [this]{ requestPairing(); });
        connect(chooseButton, &QPushButton::clicked, this, [this]{ selectDirectory(); });
        connect(startButton, &QPushButton::clicked, this, [this]{ startUpload(); });
        connect(pauseButton, &QPushButton::clicked, this, [this]{ togglePause(); });
        connect(parallelism, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this](int){ if (!id.isEmpty()) saveIdentity(); });
        auto *timer = new QTimer(this);
        timer->setInterval(5000);
        connect(timer, &QTimer::timeout, this, [this]{ if (!id.isEmpty()) pollApproval(); });
        timer->start();
        refreshControls();
        if (!id.isEmpty()) QTimer::singleShot(300, this, [this]{ pollApproval(); });
    }

private:
    enum class Phase { Idle, Starting, Transferring, Hashing, Finalizing };
    using Done = std::function<void(int, QJsonObject, QString)>;
    static constexpr qint64 defaultChunkSize = 4LL * 1024 * 1024;

    QNetworkAccessManager manager;
    QLineEdit *address = nullptr, *directory = nullptr;
    QLabel *approval = nullptr, *summary = nullptr, *current = nullptr;
    QLabel *speedLabel = nullptr, *elapsedLabel = nullptr, *etaLabel = nullptr;
    QPushButton *requestButton = nullptr, *chooseButton = nullptr, *startButton = nullptr, *pauseButton = nullptr;
    QSpinBox *parallelism = nullptr;
    QProgressBar *bar = nullptr;
    QTextEdit *logView = nullptr;
    QTimer *controlTimer = nullptr;
    QTimer *metricsTimer = nullptr;
    QElapsedTimer sessionClock;
    qint64 pausedMillis = 0, pauseStartedMillis = -1;
    qint64 sessionTransferredBytes = 0;
    QList<QPair<qint64, qint64>> speedSamples;
    QString id, token, pairedServer;
    bool revoked = false, approved = false, busy = false, paused = false;
    Phase phase = Phase::Idle;
    QVector<FileEntry> queue;
    int fileIndex = 0, controlRetry = 0;
    bool controlPending = false;
    qint64 totalBytes = 0, doneBytes = 0, confirmedBytes = 0, chunkSize = defaultChunkSize;
    QString uploadId;
    QBitArray doneChunks;
    QSet<int> activeChunks, retryWaiting;
    QHash<int, int> attempts;

    QString configPath() const {
        return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/identity.json";
    }
    void loadIdentity() {
        parallelism->setValue(4);
        QFile file(configPath());
        if (!file.open(QIODevice::ReadOnly)) return;
        const auto config = QJsonDocument::fromJson(file.readAll()).object();
        id = config.value("id").toString();
        token = config.value("token").toString();
        pairedServer = config.value("server").toString();
        parallelism->setValue(qBound(1, config.value("parallelism").toInt(4), 8));
        if (!pairedServer.isEmpty()) address->setText(pairedServer);
    }
    bool saveIdentity() {
        if (!QDir().mkpath(QFileInfo(configPath()).absolutePath())) return false;
        QSaveFile file(configPath());
        if (!file.open(QIODevice::WriteOnly)) return false;
        QJsonObject config{{"id", id}, {"token", token},
                           {"server", pairedServer}, {"parallelism", parallelism->value()}};
        if (file.write(QJsonDocument(config).toJson()) < 0 || !file.commit()) return false;
        return QFile::setPermissions(configPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    QString base() const {
        return address->text().trimmed().remove(QRegularExpression("/+$"));
    }
    void send(const QByteArray &method, const QString &path, const QByteArray &body,
              const Done &done, bool authorized = true) {
        QNetworkRequest req(QUrl(base() + path));
        req.setTransferTimeout(90000);
        if (authorized) {
            req.setRawHeader("X-MediaBridge-Device", id.toUtf8());
            req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        }
        QNetworkReply *reply = nullptr;
        if (method == "GET") {
            reply = manager.get(req);
        } else {
            req.setHeader(QNetworkRequest::ContentTypeHeader,
                          method == "PUT" ? "application/octet-stream" : "application/json");
            reply = manager.sendCustomRequest(req, method, body);
        }
        connect(reply, &QNetworkReply::finished, this, [reply, done] {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
            QString error = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
            if (code >= 400) error = response.value("error").toString(error);
            done(code, response, error);
            reply->deleteLater();
        });
    }
    void note(const QString &msg) {
        logView->append(QDateTime::currentDateTime().toString("hh:mm:ss") + "  " + msg.toHtmlEscaped());
    }
    void refreshControls() {
        startButton->setEnabled(approved && !busy && !directory->text().isEmpty());
        pauseButton->setEnabled(busy);
        pauseButton->setText(paused ? "Resume" : "Pause");
        requestButton->setEnabled(!busy);
        address->setReadOnly(busy);
        chooseButton->setEnabled(!busy);
        parallelism->setEnabled(!busy);
    }
    void requestPairing() {
        const QUrl server(base());
        if (!server.isValid() || server.scheme() != "http" || server.host().isEmpty()) {
            QMessageBox::warning(this, "Server address", "Enter a Tailnet URL such as http://media-ingest:8081.");
            return;
        }
        if (pairedServer != base() || id.isEmpty() || token.isEmpty() || revoked) {
            id = QString::fromUtf8(randomHex(16));
            token = QString::fromUtf8(randomHex(32));
            pairedServer = base();
            approved = revoked = false;
            if (!saveIdentity()) { note("Cannot save device identity"); return; }
        }
        QJsonObject body{{"id", id}, {"token", token}, {"name", QSysInfo::machineHostName()}};
        send("POST", "/api/v1/pair", QJsonDocument(body).toJson(),
             [this](int code, QJsonObject result, QString err) {
            if (code < 200 || code >= 300) { note("Pairing request failed: " + err); return; }
            const QString state = result.value("state").toString();
            approved = state == "approved";
            approval->setText(state.toUpper());
            note("Device request: " + state);
            refreshControls();
        }, false);
    }
    void pollApproval() {
        if (base() != pairedServer || id.isEmpty()) return;
        send("GET", "/api/v1/pair/status", {}, [this](int code, QJsonObject result, QString) {
            if (code != 200) {
                // A transient network outage does not revoke persistent approval.
                approval->setText("Offline / checking...");
                return;
            }
            const QString state = result.value("state").toString();
            const bool wasApproved = approved;
            approved = state == "approved";
            revoked = state == "revoked";
            approval->setText(state.toUpper());
            if (wasApproved && !approved) {
                if (busy && !paused) {
                    paused = true;
                    pauseStartedMillis = sessionClock.elapsed();
                    updateMetrics();
                }
                note("Access revoked; uploads paused");
            }
            refreshControls();
        });
    }
    void selectDirectory() {
        const QString path = QFileDialog::getExistingDirectory(this, "Choose directory to upload", QDir::homePath());
        if (path.isEmpty()) return;
        directory->setText(path);
        summary->setText("Directory: " + QFileInfo(path).fileName() + " — subfolders will be preserved.");
        refreshControls();
    }
    bool scan() {
        queue.clear();
        totalBytes = doneBytes = 0;
        const QString root = directory->text();
        const QString prefix = QFileInfo(root).fileName() + "/";
        QDirIterator it(root, QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString absolute = it.next();
            QFileInfo info(absolute);
            QString relative = prefix + QDir(root).relativeFilePath(absolute).replace('\\', '/');
            if (relative.split('/').contains("..") || relative.contains(".mediabridge-")) continue;
            queue.push_back({absolute, relative, info.size(), info.lastModified().toMSecsSinceEpoch()});
            totalBytes += info.size();
        }
        return !queue.isEmpty();
    }
    void startUpload() {
        if (!approved) return;
        if (!scan()) {
            QMessageBox::information(this, "No files", "The selected directory has no regular files.");
            return;
        }
        fileIndex = 0;
        busy = true;
        paused = false;
        controlRetry = 0;
        pausedMillis = 0;
        pauseStartedMillis = -1;
        sessionTransferredBytes = 0;
        speedSamples.clear();
        sessionClock.start();
        speedSamples.append({0, 0});
        metricsTimer->start();
        updateMetrics();
        note(QString("Starting %1 files (%2 MB) with %3 parallel chunks")
             .arg(queue.size()).arg(double(totalBytes)/1e6, 0, 'f', 1).arg(parallelism->value()));
        refreshControls();
        startNext();
    }
    void togglePause() {
        paused = !paused;
        if (paused) {
            pauseStartedMillis = sessionClock.elapsed();
        } else if (pauseStartedMillis >= 0) {
            pausedMillis += sessionClock.elapsed() - pauseStartedMillis;
            pauseStartedMillis = -1;
            // A paused interval should not dilute the resumed transfer speed.
            speedSamples.clear();
            speedSamples.append({activeElapsedMillis(), sessionTransferredBytes});
        }
        updateMetrics();
        note(paused ? "Paused (current requests may finish)" : "Resumed");
        refreshControls();
        if (paused) return;
        if (phase == Phase::Starting) {controlTimer->stop();startNext();}
        else if (phase == Phase::Finalizing) {controlTimer->stop();finalize();}
        else if (phase == Phase::Transferring) {
            const auto pending = retryWaiting.values();
            retryWaiting.clear();
            for (int i : pending) sendChunk(i);
            scheduleChunks();
        }
    }
    void updateProgress() {
        const qint64 progressed = doneBytes + confirmedBytes;
        bar->setValue(totalBytes > 0 ? int(1000.0 * double(progressed) / double(totalBytes)) : 1000);
        if (fileIndex < queue.size()) {
            current->setText(QString("%1 / %2 — %3  (%4 MB / %5 MB)")
                .arg(fileIndex + 1).arg(queue.size()).arg(queue[fileIndex].relative)
                .arg(double(progressed) / 1e6, 0, 'f', 1).arg(double(totalBytes) / 1e6, 0, 'f', 1));
        }
        updateMetrics();
    }
    qint64 activeElapsedMillis() const {
        if (!sessionClock.isValid()) return 0;
        const qint64 end = pauseStartedMillis >= 0 ? pauseStartedMillis : sessionClock.elapsed();
        return qMax(qint64(0), end - pausedMillis);
    }
    static QString formatDuration(qint64 seconds) {
        seconds = qMax(qint64(0), seconds);
        const qint64 hours = seconds / 3600;
        const qint64 minutes = (seconds / 60) % 60;
        const qint64 remainder = seconds % 60;
        if (hours > 0) {
            return QString("%1:%2:%3").arg(hours)
                .arg(minutes, 2, 10, QChar('0'))
                .arg(remainder, 2, 10, QChar('0'));
        }
        return QString("%1:%2").arg(seconds / 60)
            .arg(remainder, 2, 10, QChar('0'));
    }
    void updateMetrics() {
        if (!sessionClock.isValid()) return;
        const qint64 now = activeElapsedMillis();
        elapsedLabel->setText(formatDuration(now / 1000));
        if (!paused && busy) {
            speedSamples.append({now, sessionTransferredBytes});
            // Calculate recent acknowledged payload throughput over ~10 seconds.
            while (speedSamples.size() > 2 && speedSamples.at(1).first <= now - 10000)
                speedSamples.removeFirst();
        }
        double bytesPerSecond = 0.0;
        if (!paused && busy && speedSamples.size() >= 2) {
            const auto &first = speedSamples.first();
            const qint64 duration = now - first.first;
            if (duration >= 1000 && sessionTransferredBytes >= first.second)
                bytesPerSecond = double(sessionTransferredBytes - first.second) * 1000.0 / double(duration);
        }
        speedLabel->setText(QString("%1 MiB/s").arg(bytesPerSecond / 1048576.0, 0, 'f', 2));
        if (!busy) {
            etaLabel->setText(phase == Phase::Idle && doneBytes == totalBytes && totalBytes > 0
                                  ? "Complete" : "—");
        } else if (paused) {
            etaLabel->setText("Paused");
        } else if (phase == Phase::Hashing) {
            etaLabel->setText("Verifying...");
        } else if (phase == Phase::Finalizing) {
            etaLabel->setText("Finalizing...");
        } else {
            const qint64 remaining = qMax(qint64(0), totalBytes - doneBytes - confirmedBytes);
            if (remaining == 0 || bytesPerSecond < 1.0) {
                etaLabel->setText(remaining == 0 ? "Finishing..." : "Calculating...");
            } else {
                const double seconds = std::ceil(double(remaining) / bytesPerSecond);
                etaLabel->setText(seconds < double(1000000000LL)
                                      ? formatDuration(qint64(seconds)) : "—");
            }
        }
    }
    static bool transient(int status) {
        return status == 0 || status == 408 || status == 409 || status == 429 ||
               status == 500 || status == 502 || status == 503 || status == 504;
    }
    static int retryDelay(int attempt) {
        return qMin(30000, 1000 * (1 << qMin(attempt, 5)));
    }
    void retryControl(const QString &operation, int status, const QString &error) {
        if (!transient(status)) { stopped(operation + ": " + error); return; }
        const int delay = retryDelay(controlRetry++);
        if (controlRetry == 1 || controlRetry % 5 == 0)
            note(QString("%1: HTTP %2 (%3). Retrying automatically...")
                 .arg(operation).arg(status).arg(error));
        controlTimer->start(delay);
    }
    void startNext() {
        if (!busy || paused || controlPending) return;
        if (fileIndex >= queue.size()) {
            busy = false; phase = Phase::Idle;
            current->setText("Completed");
            bar->setValue(1000);
            metricsTimer->stop();
            updateMetrics();
            note("All files uploaded");
            refreshControls();
            return;
        }
        phase = Phase::Starting;
        controlPending = true;
        const FileEntry entry = queue[fileIndex];
        QJsonObject payload{{"path",entry.relative},{"size",double(entry.size)},{"mod_time",double(entry.modified)}};
        send("POST", "/api/v2/uploads", QJsonDocument(payload).toJson(),
             [this](int code, QJsonObject result, QString error) {
            controlPending = false;
            if (!busy || phase != Phase::Starting) return;
            if (code == 409 && error.startsWith("destination exists")) {
                note("Skipped existing destination: " + queue[fileIndex].relative);
                doneBytes += queue[fileIndex].size;
                ++fileIndex; controlRetry = 0;
                updateProgress(); startNext();
                return;
            }
            if (code < 200 || code >= 300) { retryControl("Start upload",code,error); return; }
            controlRetry = 0;
            uploadId = result.value("id").toString();
            chunkSize = qint64(result.value("chunk_size").toDouble());
            if (uploadId.isEmpty() || chunkSize != defaultChunkSize) {
                stopped("Server returned unsupported parallel-upload parameters"); return;
            }
            const int count = int((queue[fileIndex].size + chunkSize - 1) / chunkSize);
            const QByteArray bitmap = QByteArray::fromBase64(result.value("bitmap").toString().toLatin1());
            if (bitmap.size() != (count + 7) / 8) { stopped("Invalid server resume bitmap"); return; }
            doneChunks = QBitArray(count);
            confirmedBytes = 0;
            for (int i = 0; i < count; ++i) {
                if ((quint8(bitmap.at(i/8)) & (1 << (i%8))) != 0) {
                    doneChunks.setBit(i);
                    confirmedBytes += qMin(chunkSize,queue[fileIndex].size - qint64(i)*chunkSize);
                }
            }
            activeChunks.clear(); retryWaiting.clear(); attempts.clear();
            phase = Phase::Transferring;
            note(QString("Resuming %1: %2 / %3 MiB already present")
                 .arg(queue[fileIndex].relative).arg(double(confirmedBytes)/1048576.0,0,'f',1)
                 .arg(double(queue[fileIndex].size)/1048576.0,0,'f',1));
            updateProgress();
            scheduleChunks();
        });
    }
    void scheduleChunks() {
        if (!busy || paused || phase != Phase::Transferring) return;
        while (activeChunks.size() < parallelism->value()) {
            int next = -1;
            for (int i = 0; i < doneChunks.size(); ++i) {
                if (!doneChunks.testBit(i) && !activeChunks.contains(i)) { next = i; break; }
            }
            if (next < 0) break;
            activeChunks.insert(next);
            sendChunk(next);
        }
        if (activeChunks.isEmpty() && confirmedBytes == queue[fileIndex].size) finalizeHash();
    }
    void sendChunk(int index) {
        if (!busy || paused || phase != Phase::Transferring) {
            retryWaiting.insert(index);
            return;
        }
        retryWaiting.remove(index);
        const auto &entry = queue[fileIndex];
        const qint64 offset = qint64(index)*chunkSize;
        QFile file(entry.absolute);
        if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) {
            stopped("Cannot read local file: " + entry.absolute); return;
        }
        QByteArray bytes = file.read(qMin(chunkSize,entry.size-offset));
        if (bytes.size() != qMin(chunkSize,entry.size-offset)) {
            stopped("Local file changed during upload: " + entry.absolute); return;
        }
        const QString path = QString("/api/v2/uploads/%1/chunks/%2").arg(uploadId).arg(index);
        send("PUT", path, bytes, [this,index](int code, QJsonObject, QString error) {
            if (!busy || phase != Phase::Transferring) return;
            if (code >= 200 && code < 300) {
                if (!doneChunks.testBit(index)) {
                    doneChunks.setBit(index);
                    const qint64 accepted = qMin(chunkSize, queue[fileIndex].size - qint64(index)*chunkSize);
                    confirmedBytes += accepted;
                    // Only chunks confirmed in this session contribute to
                    // throughput. Restored progress must not inflate speed.
                    sessionTransferredBytes += accepted;
                }
                activeChunks.remove(index);
                retryWaiting.remove(index);
                attempts.remove(index);
                updateProgress();
                scheduleChunks();
                return;
            }
            if (!transient(code)) { stopped(QString("Chunk %1: %2").arg(index).arg(error)); return; }
            const int attempt = ++attempts[index];
            if (attempt == 1 || attempt % 5 == 0)
                note(QString("Chunk %1: HTTP %2 (%3); retrying without losing progress")
                     .arg(index).arg(code).arg(error));
            QTimer::singleShot(retryDelay(attempt-1), this, [this,index] {
                if (!busy || phase != Phase::Transferring || !activeChunks.contains(index)) return;
                sendChunk(index);
            });
        });
    }
    void finalizeHash() {
        if (!busy || phase != Phase::Transferring || fileIndex >= queue.size()) return;
        phase = Phase::Hashing;
        const QString filePath = queue[fileIndex].absolute;
        note("Verifying SHA-256: " + queue[fileIndex].relative);
        auto *watcher = new QFutureWatcher<QByteArray>(this);
        connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this,watcher] {
            QByteArray digest = watcher->result();
            watcher->deleteLater();
            if (!busy || phase != Phase::Hashing) return;
            if (digest.isEmpty()) { stopped("Cannot compute local SHA-256"); return; }
            fileHash = digest.toHex();
            phase = Phase::Finalizing;
            controlRetry = 0;
            finalize();
        });
        watcher->setFuture(QtConcurrent::run([filePath] {
            QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly)) return QByteArray();
            QCryptographicHash sha(QCryptographicHash::Sha256);
            while (!file.atEnd()) {
                const QByteArray block = file.read(1<<20);
                if (block.isEmpty() && !file.atEnd()) return QByteArray();
                sha.addData(block);
            }
            return sha.result();
        }));
    }
    QByteArray fileHash;
    void finalize() {
        if (!busy || paused || phase != Phase::Finalizing || controlPending) return;
        controlPending = true;
        const QJsonObject payload{{"sha256",QString::fromLatin1(fileHash)}};
        send("POST", QString("/api/v2/uploads/%1/finalize").arg(uploadId),
             QJsonDocument(payload).toJson(), [this](int code, QJsonObject, QString error) {
            controlPending = false;
            if (!busy || phase != Phase::Finalizing) return;
            if (code < 200 || code >= 300) { retryControl("Finalize",code,error); return; }
            note("Completed: " + queue[fileIndex].relative);
            doneBytes += queue[fileIndex].size;
            confirmedBytes = 0;
            ++fileIndex; controlRetry = 0;
            uploadId.clear();
            updateProgress();
            startNext();
        });
    }
    void stopped(const QString &why) {
        controlTimer->stop();
        note(why);
        current->setText(why);
        busy = paused = false;
        phase = Phase::Idle;
        pauseStartedMillis = -1;
        metricsTimer->stop();
        updateMetrics();
        refreshControls();
    }
};

int main(int argc, char **argv) {
    QApplication app(argc,argv);
    app.setApplicationName("MediaBridge");
    app.setOrganizationName("Evalisk");
    MainWindow window;
    window.show();
    return app.exec();
}
