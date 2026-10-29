// SPDX-License-Identifier: GPL-3.0-only
#include <QApplication>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QRegularExpression>
#include <QTextEdit>
#include <QTimer>
#include <QtConcurrent>
#include <QVBoxLayout>
#include <functional>

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
        setWindowTitle("MediaBridge — Chelonia"); resize(760, 580);
        auto *root = new QWidget;
        auto *v = new QVBoxLayout(root); v->setSpacing(12);
        auto *heading = new QLabel("MEDIABRIDGE / DESKTOP");
        heading->setObjectName("heading"); v->addWidget(heading);

        auto *connectBox = new QGroupBox("SERVER / DEVICE APPROVAL");
        auto *c = new QVBoxLayout(connectBox);
        auto *addressRow = new QHBoxLayout;
        address = new QLineEdit("http://media-ingest:8081");
        address->setPlaceholderText("http://100.x.y.z:8081");
        addressRow->addWidget(new QLabel("Tailnet address")); addressRow->addWidget(address, 1);
        c->addLayout(addressRow);
        auto *pairRow = new QHBoxLayout;
        approval = new QLabel("Not connected");
        requestButton = new QPushButton("REQUEST ACCESS");
        pairRow->addWidget(approval,1); pairRow->addWidget(requestButton);
        c->addLayout(pairRow);v->addWidget(connectBox);

        auto *uploadBox = new QGroupBox("DIRECTORY UPLOAD");
        auto *u = new QVBoxLayout(uploadBox);
        auto *folderRow = new QHBoxLayout;
        directory = new QLineEdit; directory->setReadOnly(true);
        chooseButton = new QPushButton("SELECT FOLDER");
        folderRow->addWidget(directory,1);folderRow->addWidget(chooseButton);
        u->addLayout(folderRow);
        summary = new QLabel("Choose a directory. Its name and subfolders will be preserved under /media.");
        summary->setWordWrap(true);u->addWidget(summary);
        current = new QLabel("Idle"); current->setWordWrap(true);u->addWidget(current);
        bar = new QProgressBar;bar->setRange(0,1000);bar->setValue(0);u->addWidget(bar);
        auto *buttons = new QHBoxLayout;
        startButton = new QPushButton("START UPLOAD");
        pauseButton = new QPushButton("PAUSE");
        buttons->addWidget(startButton);buttons->addWidget(pauseButton);buttons->addStretch();
        u->addLayout(buttons);v->addWidget(uploadBox);

        logView = new QTextEdit;logView->setReadOnly(true);logView->setPlaceholderText("Transfer log");v->addWidget(logView,1);
        setCentralWidget(root);
        setStyleSheet(R"CSS(
            * { border-radius:0px; font-family:Sans Serif; }
            QWidget { background:#f7f7f5; color:#171b1f; }
            QLabel#heading { font-size:19px; font-weight:700; padding:8px 0; }
            QGroupBox { border:1px solid #b4b7b8; margin-top:13px; padding:14px 8px 8px; font-weight:600; }
            QGroupBox::title { subcontrol-origin:margin; subcontrol-position:top left; padding:0 6px; }
            QPushButton { background:#17212b; color:white; border:1px solid #17212b; padding:9px 14px; font-weight:600; }
            QPushButton:disabled { background:#b7b7b7; border-color:#b7b7b7; }
            QPushButton:hover:!disabled { background:#39485a; }
            QLineEdit, QTextEdit { background:white; border:1px solid #b8bdc2; padding:8px; }
            QProgressBar { background:#e3e3e3; border:1px solid #b8bdc2; text-align:center; height:22px; }
            QProgressBar::chunk { background:#177e75; }
        )CSS");

        loadIdentity();
        connect(requestButton,&QPushButton::clicked,this,[this]{ requestPairing(); });
        connect(chooseButton,&QPushButton::clicked,this,[this]{ selectDirectory(); });
        connect(startButton,&QPushButton::clicked,this,[this]{ startUpload(); });
        connect(pauseButton,&QPushButton::clicked,this,[this]{togglePause();});
        auto *timer = new QTimer(this);timer->setInterval(5000);
        connect(timer,&QTimer::timeout,this,[this]{ if(!id.isEmpty())pollApproval(); });timer->start();
        refreshControls();
        if (!id.isEmpty()) QTimer::singleShot(300,this,[this]{pollApproval();});
    }

private:
    QNetworkAccessManager manager;
    QLineEdit *address = nullptr, *directory = nullptr;
    QLabel *approval = nullptr, *summary = nullptr, *current = nullptr;
    QPushButton *requestButton = nullptr, *chooseButton = nullptr, *startButton = nullptr, *pauseButton = nullptr;
    QProgressBar *bar = nullptr;
    QTextEdit *logView = nullptr;
    QString id, token, pairedServer;
    bool revoked = false;
    bool approved = false, busy = false, paused = false;
    QVector<FileEntry> queue;
    int index = 0;
    qint64 totalBytes = 0, doneBytes = 0, offset = 0;
    QString uploadId;
    const qint64 chunkSize = 4LL*1024*1024;

    QString configPath() const {
        return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)+"/identity.json";
    }
    void loadIdentity() {
        QFile f(configPath());if(!f.open(QIODevice::ReadOnly))return;
        QJsonObject o=QJsonDocument::fromJson(f.readAll()).object();
        id=o.value("id").toString(); token=o.value("token").toString();
        pairedServer=o.value("server").toString();
        if(!pairedServer.isEmpty())address->setText(pairedServer);
    }
    bool saveIdentity(){
        QString dir=QFileInfo(configPath()).absolutePath();
        if(!QDir().mkpath(dir))return false;
        QSaveFile f(configPath());
        if(!f.open(QIODevice::WriteOnly))return false;
        const QJsonObject o{{"id",id},{"token",token},{"server",pairedServer}};
        if(f.write(QJsonDocument(o).toJson())<0||!f.commit())return false;
        return QFile::setPermissions(configPath(),QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    }
    QString base()const{return address->text().trimmed().remove(QRegularExpression("/+$"));}
    using Done = std::function<void(int,QJsonObject,QString)>;
    void send(const QByteArray &method,const QString &path,const QByteArray &body,const Done &done, bool authorized=true){
        auto url=QUrl(base()+path);
        QNetworkRequest req(url);req.setTransferTimeout(60000);
        if(authorized){req.setRawHeader("X-MediaBridge-Device",id.toUtf8());req.setRawHeader("Authorization","Bearer "+token.toUtf8());}
        QNetworkReply *reply;
        if(method=="GET") reply=manager.get(req);
        else {
            req.setHeader(QNetworkRequest::ContentTypeHeader,method=="PUT"?"application/octet-stream":"application/json");
            reply=manager.sendCustomRequest(req,method,body);
        }
        connect(reply,&QNetworkReply::finished,this,[reply,done]{
            int code=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject result=QJsonDocument::fromJson(reply->readAll()).object();
            QString err=reply->error()==QNetworkReply::NoError ? QString() : reply->errorString();
            if(code>=400)err=result.value("error").toString(err);
            done(code,result,err);reply->deleteLater();
        });
    }
    void note(const QString &msg){logView->append(QDateTime::currentDateTime().toString("hh:mm:ss")+"  "+msg);}
    void refreshControls(){startButton->setEnabled(approved&&!busy&&!directory->text().isEmpty());pauseButton->setEnabled(busy);pauseButton->setText(paused?"RESUME":"PAUSE");requestButton->setEnabled(!busy);address->setReadOnly(busy);}
    void requestPairing(){
        const QUrl server(base());
        if(!server.isValid()||server.scheme()!="http"||server.host().isEmpty()){
            QMessageBox::warning(this,"Server address","Enter a Tailnet HTTP URL, e.g. http://media-ingest:8081");return;
        }
        if(pairedServer!=base()||id.isEmpty()||token.isEmpty()||revoked){
            id=QString::fromUtf8(randomHex(16));token=QString::fromUtf8(randomHex(32));pairedServer=base();approved=false;
            revoked=false;
            if(!saveIdentity()){note("Cannot save device identity securely");return;}
        }
        QJsonObject body{{"id",id},{"token",token},{"name",QSysInfo::machineHostName()}};
        send("POST","/api/v1/pair",QJsonDocument(body).toJson(),[this](int code,QJsonObject result,QString err){
            if(code<200||code>=300){note("Pairing request failed: "+err);return;}
            auto state=result.value("state").toString();approved=state=="approved";
            approval->setText(approved?"APPROVED": "PENDING WEBUI APPROVAL");
            note("Device request: "+state);refreshControls();
        },false);
    }
    void pollApproval(){
        if(base()!=pairedServer||id.isEmpty())return;
        send("GET","/api/v1/pair/status",{},[this](int code,QJsonObject result,QString){
            if(code!=200){approved=false;approval->setText("DISCONNECTED / UNKNOWN DEVICE");}
            else {
                auto state=result.value("state").toString();
                bool wasApproved=approved;approved=(state=="approved");revoked=(state=="revoked");
                approval->setText(state.toUpper());
                if(wasApproved&&!approved){paused=true;note("Device access revoked; upload paused");}
            }
            refreshControls();
        });
    }
    void selectDirectory(){
        const QString path=QFileDialog::getExistingDirectory(this,"Choose a directory to upload",QDir::homePath());
        if(path.isEmpty())return;
        directory->setText(path);refreshControls();
        summary->setText("Directory: "+QFileInfo(path).fileName()+" — subfolders will be preserved.");
    }
    bool scan(){
        queue.clear();totalBytes=0;doneBytes=0;
        const QString root=directory->text();
        const QString prefix=QFileInfo(root).fileName()+"/";
        QDirIterator it(root,QDir::Files|QDir::NoSymLinks|QDir::NoDotAndDotDot,QDirIterator::Subdirectories);
        while(it.hasNext()){
            const QString absolute=it.next();QFileInfo fi(absolute);
            QString rel=prefix+QDir(root).relativeFilePath(absolute).replace('\\','/');
            if(rel.split('/').contains("..")||rel.contains(".mediabridge-"))continue;
            queue.push_back({absolute,rel,fi.size(),fi.lastModified().toMSecsSinceEpoch()});
            totalBytes+=fi.size();
        }
        return !queue.isEmpty();
    }
    void startUpload(){
        if(!approved)return;
        if(!scan()){QMessageBox::information(this,"No files","The selected directory has no regular files.");return;}
        index=0;busy=true;paused=false;note(QString("Starting %1 files (%2 MB)").arg(queue.size()).arg(totalBytes/1e6,0,'f',1));
        refreshControls();startNext();
    }
    void togglePause(){paused=!paused;note(paused?"Pausing after current chunk":"Resuming");refreshControls();if(!paused)sendChunk();}
    void updateProgress(){
        bar->setValue(totalBytes>0?int(1000.0*double(doneBytes+offset)/double(totalBytes)):1000);
        if(index<queue.size())current->setText(QString("%1 / %2 — %3 (%4 MB / %5 MB)")
            .arg(index+1).arg(queue.size()).arg(queue[index].relative)
            .arg(double(doneBytes+offset)/1e6,0,'f',1).arg(double(totalBytes)/1e6,0,'f',1));
    }
    void startNext(){
        if(paused)return;
        if(index>=queue.size()){
            busy=false;current->setText("Completed");bar->setValue(1000);note("All files uploaded");refreshControls();return;
        }
        const auto &f=queue[index];
        QJsonObject body{{"path",f.relative},{"size",double(f.size)},{"mod_time",double(f.modified)}};
        send("POST","/api/v1/uploads",QJsonDocument(body).toJson(),[this](int code,QJsonObject result,QString err){
            if(code==409){note("Skipped existing destination: "+queue[index].relative);doneBytes+=queue[index].size;++index;startNext();return;}
            if(code<200||code>=300){stopped("Cannot begin upload: "+err);return;}
            uploadId=result.value("id").toString();offset=qint64(result.value("offset").toDouble());
            if(offset<0||offset>queue[index].size){stopped("Server returned invalid resume offset");return;}
            updateProgress();sendChunk();
        });
    }
    void sendChunk(){
        if(paused||!busy||index>=queue.size())return;
        const auto &entry=queue[index];
        if(offset==entry.size){finalizeHash();return;}
        QFile file(entry.absolute);
        if(!file.open(QIODevice::ReadOnly)||!file.seek(offset)){stopped("Cannot read local file: "+entry.absolute);return;}
        const QByteArray chunk=file.read(qMin(chunkSize,entry.size-offset));
        if(chunk.isEmpty()){stopped("Unexpected end of local file");return;}
        const QString path=QString("/api/v1/uploads/%1/chunk?offset=%2").arg(uploadId).arg(offset);
        send("PUT",path,chunk,[this](int code,QJsonObject result,QString err){
            if(code<200||code>=300){stopped("Chunk failed (retry by Start Upload): "+err);return;}
            offset=qint64(result.value("offset").toDouble());updateProgress();sendChunk();
        });
    }
    void finalizeHash(){
        if(index>=queue.size())return;
        const QString filePath=queue[index].absolute;
        note("Verifying SHA-256: "+queue[index].relative);
        auto *watcher=new QFutureWatcher<QByteArray>(this);
        connect(watcher,&QFutureWatcher<QByteArray>::finished,this,[this,watcher]{
            const QByteArray hash=watcher->result();watcher->deleteLater();
            if(hash.isEmpty()){stopped("Could not hash local file");return;}
            QJsonObject body{{"sha256",QString::fromLatin1(hash.toHex())}};
            send("POST",QString("/api/v1/uploads/%1/finalize").arg(uploadId),QJsonDocument(body).toJson(),[this](int code,QJsonObject,QString err){
                if(code<200||code>=300){stopped("Finalization failed: "+err);return;}
                note("Completed: "+queue[index].relative);doneBytes+=queue[index].size;offset=0;++index;
                updateProgress();startNext();
            });
        });
        watcher->setFuture(QtConcurrent::run([filePath] {
            QFile f(filePath);if(!f.open(QIODevice::ReadOnly))return QByteArray();
            QCryptographicHash hash(QCryptographicHash::Sha256);
            while(!f.atEnd()){
                auto b=f.read(1<<20);if(b.isEmpty()&&!f.atEnd())return QByteArray();
                hash.addData(b);
            }
            return hash.result();
        }));
    }
    void stopped(const QString &why){note(why);current->setText(why);busy=false;paused=false;refreshControls();}
};

int main(int argc,char **argv){
    QApplication app(argc,argv);
    app.setApplicationName("MediaBridge");app.setOrganizationName("Evalisk");
    MainWindow w;w.show();return app.exec();
}
