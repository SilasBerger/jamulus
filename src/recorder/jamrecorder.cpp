/******************************************************************************\
 *
 * Author(s):
 *  pljones
 *
 ******************************************************************************
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
\******************************************************************************/

#include "jamrecorder.h"

using namespace recorder;

/* ********************************************************************************************************
 * CJamClient
 * ********************************************************************************************************/

/**
 * @brief CJamClient::CJamClient
 * @param frame Start frame of the client within the session
 * @param numChannels 1 for mono, 2 for stereo
 * @param name The client's current name
 * @param address IP and Port
 * @param recordBaseDir Session recording directory
 *
 * Creates a file for the raw PCM data and sets up a QDataStream to which to write received frames.
 * The data is stored Little Endian.
 */
CJamClient::CJamClient(const qint64 frame, const int _numChannels, const QString name, const CHostAddress address, const QDir recordBaseDir) :
    startFrame (frame),
    numChannels (static_cast<uint16_t>(_numChannels)),
    name (name),
    address (address)
{
    // At this point we may not have much of a name
    QString fileName = ClientName() + "-" + QString::number(frame) + "-" + QString::number(_numChannels);
    QString affix = "";
    while (recordBaseDir.exists(fileName + affix + ".wav"))
    {
        affix = affix.length() == 0 ? "_1" : "_" + QString::number(affix.remove(0, 1).toInt() + 1);
    }
    fileName = fileName + affix + ".wav";

    wavFile = new QFile(recordBaseDir.absoluteFilePath(fileName));
    if (!wavFile->open(QFile::OpenMode(QIODevice::OpenModeFlag::ReadWrite))) // need to allow rewriting headers
    {
        throw new std::runtime_error( ("Could not write to WAV file "  + wavFile->fileName()).toStdString() );
    }
    out = new CWaveStream(wavFile, numChannels);

    filename = wavFile->fileName();
}

/**
 * @brief CJamClient::Frame Handle a frame of PCM data from a client connected to the server
 * @param _name The client's current name
 * @param pcm The PCM data
 */
void CJamClient::Frame(const QString _name, const CVector<int16_t>& pcm)
{
    name = _name;

    for(int i = 0; i < numChannels * SYSTEM_FRAME_SIZE_SAMPLES; i++)
    {
        *out << pcm[i];
    }

    frameCount++;
}

/**
 * @brief CJamClient::Disconnect Clean up after a disconnected client
 */
void CJamClient::Disconnect()
{
    static_cast<CWaveStream*>(out)->finalise();
    out = nullptr;

    wavFile->close();

    delete wavFile;
    wavFile = nullptr;
}

/* ********************************************************************************************************
 * CJamSession
 * ********************************************************************************************************/

/**
 * @brief CJamSession::CJamSession Construct a new jam recording session
 * @param recordBaseDir The recording base directory
 *
 * Each session is stored into its own subdirectory of the recording base directory.
 */
CJamSession::CJamSession(QDir recordBaseDir) :
    sessionDir (QDir(recordBaseDir.absoluteFilePath("Jam-" + QDateTime().currentDateTimeUtc().toString("yyyyMMdd-HHmmsszzz")))),
    currentFrame (0),
    vecptrJamClients (MAX_NUM_CHANNELS),
    jamClientConnections()
{
    const QFileInfo fi(sessionDir.absolutePath());

    if (!fi.exists() && !QDir().mkpath(sessionDir.absolutePath()))
    {
        throw std::runtime_error( (sessionDir.absolutePath() + " does not exist but could not be created").toStdString() );
    }
    if (!fi.isDir())
    {
        throw std::runtime_error( (sessionDir.absolutePath() + " exists but is not a directory").toStdString() );
    }
    if (!fi.isWritable())
    {
        throw std::runtime_error( (sessionDir.absolutePath() + " is a directory but cannot be written to").toStdString() );
    }

    // Explicitly set all the pointers to "empty"
    vecptrJamClients.fill(nullptr);

    currentFrame = 0;
}

/**
 * @brief CJamSession::DisconnectClient Capture details of the departing client's connection
 * @param iChID the channel id of the client that disconnected
 */
void CJamSession::DisconnectClient(int iChID)
{
    vecptrJamClients[iChID]->Disconnect();

    jamClientConnections.append(new CJamClientConnection(vecptrJamClients[iChID]->NumAudioChannels(),
                                                         vecptrJamClients[iChID]->StartFrame(),
                                                         vecptrJamClients[iChID]->FrameCount(),
                                                         vecptrJamClients[iChID]->ClientName(),
                                                         vecptrJamClients[iChID]->FileName()));

    delete vecptrJamClients[iChID];
    vecptrJamClients[iChID] = nullptr;
}

/**
 * @brief CJamSession::Frame Process a frame emited for a client by the server
 * @param iChID the client channel id
 * @param name the client name
 * @param address the client IP and port number
 * @param numAudioChannels the client number of audio channels
 * @param data the frame data
 *
 * Manages changes that affect how the recording is stored - i.e. if the number of audio channels changes, we need a new file.
 * Files are grouped by IP and port number, so if either of those change for a connection, we also start a new file.
 *
 * Also manages the overall current frame counter for the session.
 */
void CJamSession::Frame(const int iChID, const QString name, const CHostAddress address, const int numAudioChannels, const CVector<int16_t> data)
{
    if (vecptrJamClients[iChID] == nullptr)
    {
        // then we have not seen this client this session
        vecptrJamClients[iChID] = new CJamClient(currentFrame, numAudioChannels, name, address, sessionDir);
    }
    else if (numAudioChannels != vecptrJamClients[iChID]->NumAudioChannels()
             || address.InetAddr != vecptrJamClients[iChID]->ClientAddress().InetAddr
             || address.iPort != vecptrJamClients[iChID]->ClientAddress().iPort)
    {
        DisconnectClient(iChID);
        if (numAudioChannels == 0)
        {
            vecptrJamClients[iChID] = nullptr;
        }
        else
        {
            vecptrJamClients[iChID] = new CJamClient(currentFrame, numAudioChannels, name, address, sessionDir);
        }
    }

    if (vecptrJamClients[iChID] == nullptr)
    {
        // Frame allegedly from iChID but unable to establish client details
        return;
    }

    vecptrJamClients[iChID]->Frame(name, data);

    // If _any_ connected client frame steps past currentFrame, increase currentFrame
    if (vecptrJamClients[iChID]->StartFrame() + vecptrJamClients[iChID]->FrameCount() > currentFrame)
    {
        currentFrame++;
    }
}

/**
 * @brief CJamSession::End Clean up any "hanging" clients when the server thinks they all left
 */
void CJamSession::End()
{
    for (int iChID = 0; iChID < vecptrJamClients.size(); iChID++)
    {
        if (vecptrJamClients[iChID] != nullptr)
        {
            DisconnectClient(iChID);
            vecptrJamClients[iChID] = nullptr;
        }
    }
}

/**
 * @brief CJamSession::Tracks Retrieve a map of (latest) client name to connection items
 * @return a map of (latest) client name to connection items
 */
QMap<QString, QList<STrackItem>> CJamSession::Tracks()
{
    QMap<QString, QList<STrackItem>> tracks;

    for (int i = 0; i < jamClientConnections.count(); i++ )
    {
        STrackItem track (
            jamClientConnections[i]->Format(),
            jamClientConnections[i]->StartFrame(),
            jamClientConnections[i]->Length(),
            jamClientConnections[i]->FileName()
        );

        if (!tracks.contains(jamClientConnections[i]->Name()))
        {
            tracks.insert(jamClientConnections[i]->Name(), { });
        }

        tracks[jamClientConnections[i]->Name()].append(track);
    }

    return tracks;
}

/**
 * @brief CJamSession::TracksFromSessionDir Replica of CJamSession::Tracks but using the directory contents to construct the track item map
 * @param sessionDirName the directory name to scan
 * @return a map of (latest) client name to connection items
 */
QMap<QString, QList<STrackItem>> CJamSession::TracksFromSessionDir(const QString& sessionDirName)
{
    QMap<QString, QList<STrackItem>> tracks;

    const QDir sessionDir(sessionDirName);
    foreach(auto entry, sessionDir.entryList({ "*.pcm" }))
    {

        auto split = entry.split(".")[0].split("-");
        QString name = split[0];
        QString hostPort = split[1];
        QString frame = split[2];
        QString tail = split[3]; //numChannels may have _nnn
        QString numChannels = tail.count("_") > 0 ? tail.split("_")[0] : tail;

        QString trackName = name + "-" + hostPort;
        if (!tracks.contains(trackName))
        {
            tracks.insert(trackName, { });
        }

        QFileInfo fiEntry(sessionDir.absoluteFilePath(entry));
        qint64 length = fiEntry.size() / numChannels.toInt() / SYSTEM_FRAME_SIZE_SAMPLES;

        STrackItem track (
                    numChannels.toInt(),
                    frame.toLongLong(),
                    length,
                    sessionDir.absoluteFilePath(entry)
                    );

        tracks[trackName].append(track);
    }

    return tracks;
}

/* ********************************************************************************************************
 * CJamRecorder
 * ********************************************************************************************************/

/**
 * @brief CJamRecorder::Init Create recording directory, if necessary, and connect signal handlers
 * @param server Server object emiting signals
 */
void CJamRecorder::Init(const CServer* server)
{
    const QFileInfo fi(recordBaseDir.absolutePath());

    if (!fi.exists() && !QDir().mkpath(recordBaseDir.absolutePath()))
    {
        throw std::runtime_error( (recordBaseDir.absolutePath() + " does not exist but could not be created").toStdString() );
    }
    if (!fi.isDir())
    {
        throw std::runtime_error( (recordBaseDir.absolutePath() + " exists but is not a directory").toStdString() );
    }
    if (!fi.isWritable())
    {
        throw std::runtime_error( (recordBaseDir.absolutePath() + " is a directory but cannot be written to").toStdString() );
    }

    QObject::connect((const QObject *)server, SIGNAL ( Stopped() ),
                     this, SLOT( OnEnd() ),
                     Qt::ConnectionType::QueuedConnection);

    QObject::connect((const QObject *)server, SIGNAL ( ClientDisconnected(int) ),
                     this, SLOT( OnDisconnected(int) ),
                     Qt::ConnectionType::QueuedConnection);

    qRegisterMetaType<CVector<int16_t>>();
    QObject::connect((const QObject *)server, SIGNAL ( AudioFrame(const int, const QString, const CHostAddress, const int, const CVector<int16_t>) ),
                     this, SLOT(  OnFrame(const int, const QString, const CHostAddress, const int, const CVector<int16_t>) ),
                     Qt::ConnectionType::QueuedConnection);
}

/**
 * @brief CJamRecorder::OnStart Start up tasks when the first client connects
 */
void CJamRecorder::OnStart() {
    // Ensure any previous cleaning up has been done.
    OnEnd();

    currentSession = new CJamSession(recordBaseDir);
    isRecording = true;
}

/**
 * @brief CJamRecorder::OnEnd Finalise the recording and emit the Reaper RPP file
 */
void CJamRecorder::OnEnd()
{
    if (isRecording)
    {
        isRecording = false;
        currentSession->End();

        QString reaperProjectFileName = currentSession->SessionDir().filePath(currentSession->Name().append(".rpp"));
        const QFileInfo fi(reaperProjectFileName);

        if (fi.exists())
        {
            tsConsole << "CJamRecorder::OnEnd() - " << fi.absolutePath() << " exists and will not be overwritten." << endl;
        }
        else
        {
            QFile outf (reaperProjectFileName);
            outf.open(QFile::WriteOnly);
            QTextStream out(&outf);
            out << CReaperProject(currentSession->Tracks()).toString() << endl;
            tsConsole << "Session RPP: " << reaperProjectFileName << endl;
        }

        delete currentSession;
        currentSession = nullptr;
    }
}

/**
 * @brief CJamRecorder::SessionDirToReaper Replica of CJamRecorder::OnEnd() but using the directory contents to construct the CReaperProject object
 * @param strSessionDirName
 */
void CJamRecorder::SessionDirToReaper(QString& strSessionDirName)
{
    const QFileInfo fiSessionDir(QDir::cleanPath(strSessionDirName));
    if (!fiSessionDir.exists() || !fiSessionDir.isDir())
    {
        throw std::runtime_error( (fiSessionDir.absoluteFilePath() + " does not exist or is not a directory.  Aborting.").toStdString() );
    }

    const QDir dSessionDir(fiSessionDir.absoluteFilePath());
    const QString reaperProjectFileName = dSessionDir.absoluteFilePath(fiSessionDir.baseName().append(".rpp"));
    const QFileInfo fiRPP(reaperProjectFileName);
    if (fiRPP.exists())
    {
        throw std::runtime_error( (fiRPP.absoluteFilePath() + " exists and will not be overwritten.  Aborting.").toStdString() );
    }

    QFile outf (fiRPP.absoluteFilePath());
    if (!outf.open(QFile::WriteOnly)) {
        throw std::runtime_error( (fiRPP.absoluteFilePath() + " could not be written.  Aborting.").toStdString() );
    }
    QTextStream out(&outf);

    out << CReaperProject(CJamSession::TracksFromSessionDir(fiSessionDir.absoluteFilePath())).toString() << endl;

    (*(new ConsoleWriterFactory())->get()) << "Session RPP: " << reaperProjectFileName << endl;
}

/**
 * @brief CJamRecorder::OnDisconnected Handle disconnection of a client
 * @param iChID the client channel id
 */
void CJamRecorder::OnDisconnected(int iChID)
{
    if (!isRecording)
    {
        tsConsole << "CJamRecorder::OnDisconnected: channel " << iChID << " disconnected but not recording" << endl;
    }
    if (currentSession == nullptr)
    {
        tsConsole << "CJamRecorder::OnDisconnected: channel " << iChID << " disconnected but no currentSession" << endl;
        return;
    }
    currentSession->DisconnectClient(iChID);
}

/**
 * @brief CJamRecorder::OnFrame Handle a frame emited for a client by the server
 * @param iChID the client channel id
 * @param name the client name
 * @param address the client IP and port number
 * @param numAudioChannels the client number of audio channels
 * @param data the frame data
 *
 * Ensures recording has started.
 */
void CJamRecorder::OnFrame(const int iChID, const QString name, const CHostAddress address, const int numAudioChannels, const CVector<int16_t> data)
{
    // Make sure we are ready
    if (!isRecording)
    {
        OnStart();
    }

    currentSession->Frame(iChID, name, address, numAudioChannels, data);
}
