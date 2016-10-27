/****************************************************************************
 *
 *   (c) 2009-2016 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "MavlinkLogManager.h"
#include "QGCApplication.h"
#include <QQmlContext>
#include <QQmlProperty>
#include <QQmlEngine>
#include <QtQml>
#include <QSettings>
#include <QHttpPart>
#include <QNetworkReply>
#include <QFile>
#include <QFileInfo>

#define kTimeOutMilliseconds 1000

QGC_LOGGING_CATEGORY(MavlinkLogManagerLog, "MavlinkLogManagerLog")

static const char* kEmailAddressKey         = "MavlinkLogEmail";
static const char* kDescriptionsKey         = "MavlinkLogDescription";
static const char* kDefaultDescr            = "QGroundControl Session";
static const char* kPx4URLKey               = "MavlinkLogURL";
static const char* kDefaultPx4URL           = "http://logs.px4.io/upload";
static const char* kEnableAutoUploadKey     = "EnableAutoUploadKey";
static const char* kEnableAutoStartKey      = "EnableAutoStartKey";
static const char* kEnableDeletetKey        = "EnableDeleteKey";
static const char* kUlogExtension           = ".ulg";
static const char* kSidecarExtension        = ".uploaded";

//-----------------------------------------------------------------------------
MavlinkLogFiles::MavlinkLogFiles(MavlinkLogManager* manager, const QString& filePath, bool newFile)
    : _manager(manager)
    , _size(0)
    , _selected(false)
    , _uploading(false)
    , _progress(0)
    , _writing(false)
    , _uploaded(false)
{
    QFileInfo fi(filePath);
    _name = fi.baseName();
    if(!newFile) {
        _size = (quint32)fi.size();
        QString sideCar = filePath;
        sideCar.replace(kUlogExtension, kSidecarExtension);
        QFileInfo sc(sideCar);
        _uploaded = sc.exists();
    }
}

//-----------------------------------------------------------------------------
void
MavlinkLogFiles::setSize(quint32 size)
{
    _size = size;
    emit sizeChanged();
}

//-----------------------------------------------------------------------------
void
MavlinkLogFiles::setSelected(bool selected)
{
    _selected = selected;
    emit selectedChanged();
    emit _manager->selectedCountChanged();
}

//-----------------------------------------------------------------------------
void
MavlinkLogFiles::setUploading(bool uploading)
{
    _uploading = uploading;
    emit uploadingChanged();
}

//-----------------------------------------------------------------------------
void
MavlinkLogFiles::setProgress(qreal progress)
{
    _progress = progress;
    emit progressChanged();
}

//-----------------------------------------------------------------------------
void
MavlinkLogFiles::setWriting(bool writing)
{
    _writing = writing;
    emit writingChanged();
}

//-----------------------------------------------------------------------------
void
MavlinkLogFiles::setUploaded(bool uploaded)
{
    _uploaded = uploaded;
    emit uploadedChanged();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
MavlinkLogProcessor::MavlinkLogProcessor()
    : _fd(NULL)
    , _written(0)
    , _sequence(-1)
    , _numDrops(0)
    , _gotHeader(false)
    , _error(false)
    , _record(NULL)
{
}

//-----------------------------------------------------------------------------
MavlinkLogProcessor::~MavlinkLogProcessor()
{
    close();
}

//-----------------------------------------------------------------------------
void
MavlinkLogProcessor::close()
{
    if(_fd) {
        fclose(_fd);
        _fd = NULL;
    }
}

//-----------------------------------------------------------------------------
bool
MavlinkLogProcessor::valid()
{
    return (_fd != NULL) && (_record != NULL);
}

//-----------------------------------------------------------------------------
bool
MavlinkLogProcessor::create(MavlinkLogManager* manager, const QString path, uint8_t id)
{
    _fileName.sprintf("%s/%03d-%s%s",
        path.toLatin1().data(),
        id,
        QDateTime::currentDateTime().toString("yyyy-MM-dd-hh-mm-ss-zzz").toLatin1().data(),
        kUlogExtension);
    _fd = fopen(_fileName.toLatin1().data(), "wb");
    if(_fd) {
        _record = new MavlinkLogFiles(manager, _fileName, true);
        _record->setWriting(true);
        _sequence = -1;
        return true;
    }
    return false;
}

//-----------------------------------------------------------------------------
bool
MavlinkLogProcessor::_checkSequence(uint16_t seq, int& num_drops)
{
    num_drops = 0;
    //-- Check if a sequence is newer than the one previously received and if
    //   there were dropped messages between the last one and this.
    if(_sequence == -1) {
        _sequence = seq;
        return true;
    }
    if((uint16_t)_sequence == seq) {
        return false;
    }
    if(seq > (uint16_t)_sequence) {
        // Account for wrap-arounds, sequence is 2 bytes
        if((seq - _sequence) > (1 << 15)) { // Assume reordered
            return false;
        }
        num_drops = seq - _sequence - 1;
        _numDrops += num_drops;
        _sequence = seq;
        return true;
    } else {
        if((_sequence - seq) > (1 << 15)) {
            num_drops = (1 << 16) - _sequence - 1 + seq;
            _numDrops += num_drops;
            _sequence = seq;
            return true;
        }
        return false;
    }
}

//-----------------------------------------------------------------------------
void
MavlinkLogProcessor::_writeData(void* data, int len)
{
    if(!_error) {
        _error = fwrite(data, 1, len, _fd) != (size_t)len;
        if(!_error) {
            _written += len;
            if(_record) {
                _record->setSize(_written);
            }
        } else {
            qCDebug(MavlinkLogManagerLog) << "File IO error:" << len << "bytes into" << _fileName;
        }
    }
}

//-----------------------------------------------------------------------------
QByteArray
MavlinkLogProcessor::_writeUlogMessage(QByteArray& data)
{
    //-- Write ulog data w/o integrity checking, assuming data starts with a
    //   valid ulog message. returns the remaining data at the end.
    while(data.length() > 2) {
        uint8_t* ptr = (uint8_t*)data.data();
        int message_length = ptr[0] + (ptr[1] * 256) + 3; // 3 = ULog msg header
        if(message_length > data.length())
            break;
        _writeData(data.data(), message_length);
        data.remove(0, message_length);
        return data;
    }
    return data;
}

//-----------------------------------------------------------------------------
bool
MavlinkLogProcessor::processStreamData(uint16_t sequence, uint8_t first_message, QByteArray data)
{
    int num_drops = 0;
    _error = false;
    while(_checkSequence(sequence, num_drops)) {
        //-- The first 16 bytes need special treatment (this sounds awfully brittle)
        if(!_gotHeader) {
            if(data.size() < 16) {
                //-- Shouldn't happen but if it does, we might as well close shop.
                qCCritical(MavlinkLogManagerLog) << "Corrupt log header. Canceling log download.";
                return false;
            }
            //-- Write header
            _writeData(data.data(), 16);
            data.remove(0, 16);
            _gotHeader = true;
            // What about data start offset now that we removed 16 bytes off the start?
        }
        if(_gotHeader && num_drops > 0) {
            if(num_drops > 25) num_drops = 25;
            //-- Hocus Pocus
            //   Write a dropout message. We don't really know the actual duration,
            //   so just use the number of drops * 10 ms
            uint8_t bogus[] = {2, 0, 79, 0, 0};
            bogus[3] = num_drops * 10;
            _writeData(bogus, sizeof(bogus));
        }
        if(num_drops > 0) {
            _writeUlogMessage(_ulogMessage);
            _ulogMessage.clear();
            //-- If no usefull information in this message. Drop it.
            if(first_message == 255) {
                break;
            }
            if(first_message > 0) {
                data.remove(0, first_message);
                first_message = 0;
            }
        }
        if(first_message == 255 && _ulogMessage.length() > 0) {
            _ulogMessage.append(data);
            break;
        }
        if(_ulogMessage.length()) {
            _writeData(_ulogMessage.data(), _ulogMessage.length());
            if(first_message) {
                _writeData(data.left(first_message).data(), first_message);
            }
            _ulogMessage.clear();
        }
        if(first_message) {
            data.remove(0, first_message);
        }
        _ulogMessage = _writeUlogMessage(data);
        break;
    }
    return !_error;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
MavlinkLogManager::MavlinkLogManager(QGCApplication* app)
    : QGCTool(app)
    , _enableAutoUpload(true)
    , _enableAutoStart(true)
    , _nam(NULL)
    , _currentLogfile(NULL)
    , _vehicle(NULL)
    , _logRunning(false)
    , _loggingDisabled(false)
    , _logProcessor(NULL)
    , _deleteAfterUpload(false)
    , _loggingCmdTryCount(0)
{
    //-- Get saved settings
    QSettings settings;
    setEmailAddress(settings.value(kEmailAddressKey, QString()).toString());
    setDescription(settings.value(kDescriptionsKey, QString(kDefaultDescr)).toString());
    setUploadURL(settings.value(kPx4URLKey, QString(kDefaultPx4URL)).toString());
    setEnableAutoUpload(settings.value(kEnableAutoUploadKey, true).toBool());
    setEnableAutoStart(settings.value(kEnableAutoStartKey, true).toBool());
    setDeleteAfterUpload(settings.value(kEnableDeletetKey, false).toBool());
    //-- Logging location
    _logPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    _logPath += "/MavlinkLogs";
    if(!QDir(_logPath).exists()) {
        if(!QDir().mkpath(_logPath)) {
            qCCritical(MavlinkLogManagerLog) << "Could not create Mavlink log download path:" << _logPath;
            _loggingDisabled = true;
        }
    }
    if(!_loggingDisabled) {
        //-- Load current list of logs
        QString filter = "*";
        filter += kUlogExtension;
        QDirIterator it(_logPath, QStringList() << filter, QDir::Files);
        while(it.hasNext()) {
            _insertNewLog(new MavlinkLogFiles(this, it.next()));
        }
        qCDebug(MavlinkLogManagerLog) << "Mavlink logs directory:" << _logPath;
    }
}

//-----------------------------------------------------------------------------
MavlinkLogManager::~MavlinkLogManager()
{
    _logFiles.clear();
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::setToolbox(QGCToolbox* toolbox)
{
    QGCTool::setToolbox(toolbox);
    QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
    qmlRegisterUncreatableType<MavlinkLogManager>("QGroundControl.MavlinkLogManager", 1, 0, "MavlinkLogManager", "Reference only");
    if(!_loggingDisabled) {
        connect(toolbox->multiVehicleManager(), &MultiVehicleManager::activeVehicleChanged, this, &MavlinkLogManager::_activeVehicleChanged);
        connect(&_ackTimer, &QTimer::timeout, this, &MavlinkLogManager::_processCmdAck);
    }
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::setEmailAddress(QString email)
{
    _emailAddress = email;
    QSettings settings;
    settings.setValue(kEmailAddressKey, email);
    emit emailAddressChanged();
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::setDescription(QString description)
{
    _description = description;
    QSettings settings;
    settings.setValue(kDescriptionsKey, description);
    emit descriptionChanged();
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::setUploadURL(QString url)
{
    _uploadURL = url;
    if(_uploadURL.isEmpty()) {
        _uploadURL = kDefaultPx4URL;
    }
    QSettings settings;
    settings.setValue(kPx4URLKey, _uploadURL);
    emit uploadURLChanged();
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::setEnableAutoUpload(bool enable)
{
    _enableAutoUpload = enable;
    QSettings settings;
    settings.setValue(kEnableAutoUploadKey, enable);
    emit enableAutoUploadChanged();
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::setEnableAutoStart(bool enable)
{
    _enableAutoStart = enable;
    QSettings settings;
    settings.setValue(kEnableAutoStartKey, enable);
    emit enableAutoStartChanged();
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::setDeleteAfterUpload(bool enable)
{
    _deleteAfterUpload = enable;
    QSettings settings;
    settings.setValue(kEnableDeletetKey, enable);
    emit deleteAfterUploadChanged();
}

//-----------------------------------------------------------------------------
bool
MavlinkLogManager::uploading()
{
    return _currentLogfile != NULL;
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::uploadLog()
{
    if(_currentLogfile) {
        _currentLogfile->setUploading(false);
    }
    for(int i = 0; i < _logFiles.count(); i++) {
        _currentLogfile = qobject_cast<MavlinkLogFiles*>(_logFiles.get(i));
        Q_ASSERT(_currentLogfile);
        if(_currentLogfile->selected()) {
            _currentLogfile->setSelected(false);
            if(!_currentLogfile->uploaded() && !_emailAddress.isEmpty() && !_uploadURL.isEmpty()) {
                _currentLogfile->setUploading(true);
                _currentLogfile->setProgress(0.0);
                QString filePath = _makeFilename(_currentLogfile->name());
                _sendLog(filePath);
                emit uploadingChanged();
                return;
            }
        }
    }
    _currentLogfile = NULL;
    emit uploadingChanged();
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::_insertNewLog(MavlinkLogFiles* newLog)
{
    //-- Simpler than trying to sort this thing
    int count = _logFiles.count();
    if(!count) {
        _logFiles.append(newLog);
    } else {
        for(int i = 0; i < count; i++) {
            MavlinkLogFiles* f = qobject_cast<MavlinkLogFiles*>(_logFiles.get(i));
            if(newLog->name() < f->name()) {
                _logFiles.insert(i, newLog);
                return;
            }
        }
        _logFiles.append(newLog);
    }
}

//-----------------------------------------------------------------------------
int
MavlinkLogManager::_getFirstSelected()
{
    for(int i = 0; i < _logFiles.count(); i++) {
        MavlinkLogFiles* f = qobject_cast<MavlinkLogFiles*>(_logFiles.get(i));
        Q_ASSERT(f);
        if(f->selected()) {
            return i;
        }
    }
    return -1;
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::deleteLog()
{
    while (true) {
        int idx = _getFirstSelected();
        if(idx < 0) {
            break;
        }
        MavlinkLogFiles* log = qobject_cast<MavlinkLogFiles*>(_logFiles.get(idx));
        _deleteLog(log);
    }
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::_deleteLog(MavlinkLogFiles* log)
{
    QString filePath = _makeFilename(log->name());
    QFile gone(filePath);
    if(!gone.remove()) {
        qCWarning(MavlinkLogManagerLog) << "Could not delete Mavlink log file:" << _logPath;
    }
    //-- Remove sidecar file (if any)
    filePath.replace(kUlogExtension, kSidecarExtension);
    QFile sgone(filePath);
    if(sgone.exists()) {
        sgone.remove();
    }
    //-- Remove file from list and delete record
    _logFiles.removeOne(log);
    delete log;
    emit logFilesChanged();
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::cancelUpload()
{
    for(int i = 0; i < _logFiles.count(); i++) {
        MavlinkLogFiles* pLogFile = qobject_cast<MavlinkLogFiles*>(_logFiles.get(i));
        Q_ASSERT(pLogFile);
        if(pLogFile->selected() && pLogFile != _currentLogfile) {
            pLogFile->setSelected(false);
        }
    }
    if(_currentLogfile) {
        emit abortUpload();
    }
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::startLogging()
{
    if(_vehicle) {
        if(_createNewLog()) {
            _vehicle->startMavlinkLog();
            _logRunning = true;
            _loggingCmdTryCount = 0;
            _ackTimer.start(kTimeOutMilliseconds);
            emit logRunningChanged();
        }
    }
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::stopLogging()
{
    if(_vehicle) {
        //-- Tell vehicle to stop sending logs
        _vehicle->stopMavlinkLog();
    }
    if(_logProcessor) {
        _logProcessor->close();
        if(_logProcessor->record()) {
            _logProcessor->record()->setWriting(false);
            if(_enableAutoUpload) {
                //-- Queue log for auto upload (set selected flag)
                _logProcessor->record()->setSelected(true);
                if(!uploading()) {
                    uploadLog();
                }
            }
        }
        delete _logProcessor;
        _logProcessor = NULL;
        _logRunning = false;
        if(_vehicle) {
            //-- Setup a timer to make sure vehicle received the command
            _loggingCmdTryCount = 0;
            _ackTimer.start(kTimeOutMilliseconds);
        }
        emit logRunningChanged();
    }
}

//-----------------------------------------------------------------------------
QHttpPart
create_form_part(const QString& name, const QString& value)
{
    QHttpPart formPart;
    formPart.setHeader(QNetworkRequest::ContentDispositionHeader, QString("form-data; name=\"%1\"").arg(name));
    formPart.setBody(value.toUtf8());
    return formPart;
}

//-----------------------------------------------------------------------------
bool
MavlinkLogManager::_sendLog(const QString& logFile)
{
    QString defaultDescription = _description;
    if(_description.isEmpty()) {
        qCWarning(MavlinkLogManagerLog) << "Log description missing. Using defaults.";
        defaultDescription = kDefaultDescr;
    }
    if(_emailAddress.isEmpty()) {
        qCCritical(MavlinkLogManagerLog) << "User email missing.";
        return false;
    }
    if(_uploadURL.isEmpty()) {
        qCCritical(MavlinkLogManagerLog) << "Upload URL missing.";
        return false;
    }
    QFileInfo fi(logFile);
    if(!fi.exists()) {
        qCCritical(MavlinkLogManagerLog) << "Log file missing:" << logFile;
        return false;
    }
    QFile* file = new QFile(logFile);
    if(!file || !file->open(QIODevice::ReadOnly)) {
        if(file) {
            delete file;
        }
        qCCritical(MavlinkLogManagerLog) << "Could not open log file:" << logFile;
        return false;
    }
    if(!_nam) {
        _nam = new QNetworkAccessManager(this);
    }
    QNetworkProxy savedProxy = _nam->proxy();
    QNetworkProxy tempProxy;
    tempProxy.setType(QNetworkProxy::DefaultProxy);
    _nam->setProxy(tempProxy);
    //-- Build POST request
    QHttpMultiPart* multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart emailPart = create_form_part("email", _emailAddress);
    QHttpPart descriptionPart = create_form_part("description", _description);
    QHttpPart sourcePart = create_form_part("source", "QGroundControl");
    QHttpPart versionPart = create_form_part("version", _app->applicationVersion());
    QHttpPart logPart;
    logPart.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    logPart.setHeader(QNetworkRequest::ContentDispositionHeader, QString("form-data; name=\"filearg\"; filename=\"%1\"").arg(fi.fileName()));
    logPart.setBodyDevice(file);
    //-- Assemble request and POST it
    multiPart->append(emailPart);
    multiPart->append(descriptionPart);
    multiPart->append(sourcePart);
    multiPart->append(versionPart);
    multiPart->append(logPart);
    file->setParent(multiPart);
    QNetworkRequest request(_uploadURL);
#if QT_VERSION > 0x050600
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif
    QNetworkReply* reply = _nam->post(request, multiPart);
    connect(reply, &QNetworkReply::finished,  this, &MavlinkLogManager::_uploadFinished);
    connect(this, &MavlinkLogManager::abortUpload, reply, &QNetworkReply::abort);
    //connect(reply, &QNetworkReply::readyRead, this, &MavlinkLogManager::_dataAvailable);
    connect(reply, &QNetworkReply::uploadProgress, this, &MavlinkLogManager::_uploadProgress);
    multiPart->setParent(reply);
    qCDebug(MavlinkLogManagerLog) << "Log" << fi.baseName() << "Uploading." << fi.size() << "bytes.";
    _nam->setProxy(savedProxy);
    return true;
}

//-----------------------------------------------------------------------------
bool
MavlinkLogManager::_processUploadResponse(int http_code, QByteArray& data)
{
    qCDebug(MavlinkLogManagerLog) << "Uploaded response:" << QString::fromUtf8(data);
    emit readyRead(data);
    return http_code == 200;
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::_dataAvailable()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if(!reply) {
        return;
    }
    QByteArray data = reply->readAll();
    qCDebug(MavlinkLogManagerLog) << "Uploaded response data:" << QString::fromUtf8(data);
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::_uploadFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if(!reply) {
        return;
    }
    const int http_code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray data = reply->readAll();
    if(_processUploadResponse(http_code, data)) {
        qCDebug(MavlinkLogManagerLog) << "Log uploaded.";
        emit succeed();
        if(_deleteAfterUpload) {
            if(_currentLogfile) {
                _deleteLog(_currentLogfile);
                _currentLogfile = NULL;
            }
        } else {
            if(_currentLogfile) {
                _currentLogfile->setUploaded(true);
                //-- Write side-car file to flag it as uploaded
                QString sideCar = _makeFilename(_currentLogfile->name());
                sideCar.replace(kUlogExtension, kSidecarExtension);
                FILE* f = fopen(sideCar.toLatin1().data(), "wb");
                if(f) {
                    fclose(f);
                }
            }
        }
    } else {
        qCWarning(MavlinkLogManagerLog) << QString("Log Upload Error: %1 status: %2").arg(reply->errorString(), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toString());
        emit failed();
    }
    reply->deleteLater();
    //-- Next (if any)
    uploadLog();
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::_uploadProgress(qint64 bytesSent, qint64 bytesTotal)
{
    if(bytesTotal) {
        qreal progress = (qreal)bytesSent / (qreal)bytesTotal;
        if(_currentLogfile) {
            _currentLogfile->setProgress(progress);
        }
    }
    qCDebug(MavlinkLogManagerLog) << bytesSent << "of" << bytesTotal;
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::_activeVehicleChanged(Vehicle* vehicle)
{
    //-- TODO: This is not quite right. This is being used to detect when a vehicle
    //   connects/disconnects. In reality, if QGC is connected to multiple vehicles,
    //   this is called each time the user switches from one vehicle to another. So
    //   far, I'm working on the assumption that multiple vehicles is a rare exception.
    //   For now, we only handle one log download at a time.
    // Disconnect the previous one (if any)
    if(_vehicle) {
        disconnect(_vehicle, &Vehicle::armedChanged,   this, &MavlinkLogManager::_armedChanged);
        disconnect(_vehicle, &Vehicle::mavlinkLogData, this, &MavlinkLogManager::_mavlinkLogData);
        disconnect(_vehicle, &Vehicle::commandLongAck, this, &MavlinkLogManager::_commandLongAck);
        _vehicle = NULL;
        //-- Stop logging (if that's the case)
        stopLogging();
        emit canStartLogChanged();
    }
    // Connect new system
    if(vehicle) {
        _vehicle = vehicle;
        connect(_vehicle, &Vehicle::armedChanged,   this, &MavlinkLogManager::_armedChanged);
        connect(_vehicle, &Vehicle::mavlinkLogData, this, &MavlinkLogManager::_mavlinkLogData);
        connect(_vehicle, &Vehicle::commandLongAck, this, &MavlinkLogManager::_commandLongAck);
        emit canStartLogChanged();
    }
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::_processCmdAck()
{
    if(_loggingCmdTryCount++ > 3) {
        _ackTimer.stop();
        //-- Give up
        if(_logRunning) {
            qCWarning(MavlinkLogManagerLog) << "Start MAVLink log command had no response.";
            _discardLog();
        } else {
            qCWarning(MavlinkLogManagerLog) << "Stop MAVLink log command had no response.";
        }
    } else {
        if(_vehicle) {
            if(_logRunning) {
                _vehicle->startMavlinkLog();
                qCWarning(MavlinkLogManagerLog) << "Start MAVLink log command sent again.";
            } else {
                _vehicle->stopMavlinkLog();
                qCWarning(MavlinkLogManagerLog) << "Stop MAVLink log command sent again.";
            }
            _ackTimer.start(kTimeOutMilliseconds);
        } else {
            //-- Vehicle went away on us
            _ackTimer.stop();
        }
    }
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::_mavlinkLogData(Vehicle* /*vehicle*/, uint8_t /*target_system*/, uint8_t /*target_component*/, uint16_t sequence, uint8_t first_message, QByteArray data, bool /*acked*/)
{
    //-- Disable timer if we got a message before an ACK for the start command
    if(_logRunning) {
        _ackTimer.stop();
    }
    if(_logProcessor && _logProcessor->valid()) {
        if(!_logProcessor->processStreamData(sequence, first_message, data)) {
            qCCritical(MavlinkLogManagerLog) << "Error writing Mavlink log file:" << _logProcessor->fileName();
            delete _logProcessor;
            _logProcessor = NULL;
            _logRunning = false;
            _vehicle->stopMavlinkLog();
            emit logRunningChanged();
        }
    } else {
        qCWarning(MavlinkLogManagerLog) << "Mavlink log data received when not expected.";
    }
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::_commandLongAck(uint8_t /*compID*/, uint16_t command, uint8_t result)
{
    if(command == MAV_CMD_LOGGING_START || command == MAV_CMD_LOGGING_STOP) {
        _ackTimer.stop();
        //-- Did it fail?
        if(result) {
            if(command == MAV_CMD_LOGGING_STOP) {
                //-- Not that it could happen but...
                qCWarning(MavlinkLogManagerLog) << "Stop MAVLink log command failed.";
            } else {
                //-- Could not start logging for some reason.
                qCWarning(MavlinkLogManagerLog) << "Start MAVLink log command failed.";
                _discardLog();
            }
        }
    }
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::_discardLog()
{
    //-- Delete (empty) log file (and record)
    if(_logProcessor) {
        _logProcessor->close();
        if(_logProcessor->record()) {
            _deleteLog(_logProcessor->record());
        }
        delete _logProcessor;
        _logProcessor = NULL;
    }
    _logRunning = false;
    emit logRunningChanged();
}

//-----------------------------------------------------------------------------
bool
MavlinkLogManager::_createNewLog()
{
    if(_logProcessor) {
        delete _logProcessor;
        _logProcessor = NULL;
    }
    _logProcessor = new MavlinkLogProcessor;
    if(_logProcessor->create(this, _logPath, _vehicle->id())) {
        _insertNewLog(_logProcessor->record());
        emit logFilesChanged();
    } else {
        qCCritical(MavlinkLogManagerLog) << "Could not create Mavlink log file:" << _logProcessor->fileName();
        delete _logProcessor;
        _logProcessor = NULL;
    }
    return _logProcessor != NULL;
}

//-----------------------------------------------------------------------------
void
MavlinkLogManager::_armedChanged(bool armed)
{
    if(_vehicle) {
        if(armed) {
            if(_enableAutoStart) {
                startLogging();
            }
        } else {
            if(_logRunning && _enableAutoStart) {
                stopLogging();
            }
        }
    }
}

//-----------------------------------------------------------------------------
QString
MavlinkLogManager::_makeFilename(const QString& baseName)
{
    QString filePath = _logPath;
    filePath += "/";
    filePath += baseName;
    filePath += kUlogExtension;
    return filePath;
}