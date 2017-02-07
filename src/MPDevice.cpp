/******************************************************************************
 **  Copyright (c) Raoul Hecky. All Rights Reserved.
 **
 **  Calaos is free software; you can redistribute it and/or modify
 **  it under the terms of the GNU General Public License as published by
 **  the Free Software Foundation; either version 3 of the License, or
 **  (at your option) any later version.
 **
 **  Calaos is distributed in the hope that it will be useful,
 **  but WITHOUT ANY WARRANTY; without even the implied warranty of
 **  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 **  GNU General Public License for more details.
 **
 **  You should have received a copy of the GNU General Public License
 **  along with Foobar; if not, write to the Free Software
 **  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 **
 ******************************************************************************/
#include "MPDevice.h"
#include <functional>

const char *mooltipassParamString[] = {
    "USER_PARAM_INIT_KEY_PARAM",
    "KEYBOARD_LAYOUT_PARAM",
    "USER_INTER_TIMEOUT_PARAM",
    "LOCK_TIMEOUT_ENABLE_PARAM",
    "LOCK_TIMEOUT_PARAM",
    "TOUCH_DI_PARAM",
    "TOUCH_WHEEL_OS_PARAM_OLD",
    "TOUCH_PROX_OS_PARAM",
    "OFFLINE_MODE_PARAM",
    "SCREENSAVER_PARAM",
    "TOUCH_CHARGE_TIME_PARAM",
    "TOUCH_WHEEL_OS_PARAM0",
    "TOUCH_WHEEL_OS_PARAM1",
    "TOUCH_WHEEL_OS_PARAM2"
    "FLASH_SCREEN_PARAM",
    "USER_REQ_CANCEL_PARAM",
    "TUTORIAL_BOOL_PARAM",
    "SCREEN_SAVER_SPEED_PARAM",
    "LUT_BOOT_POPULATING_PARAM",
    "KEY_AFTER_LOGIN_SEND_BOOL_PARAM",
    "KEY_AFTER_LOGIN_SEND_PARAM",
    "KEY_AFTER_PASS_SEND_BOOL_PARAM",
    "KEY_AFTER_PASS_SEND_PARAM",
    "DELAY_AFTER_KEY_ENTRY_BOOL_PARAM",
    "DELAY_AFTER_KEY_ENTRY_PARAM",
    "INVERTED_SCREEN_AT_BOOT_PARAM",
    "MINI_OLED_CONTRAST_CURRENT_PARAM",
    "MINI_LED_ANIM_MASK_PARAM",
    "MINI_KNOCK_DETECT_ENABLE_PARAM",
    "MINI_KNOCK_THRES_PARAM",
    "LOCK_UNLOCK_FEATURE_PARAM",
    "HASH_DISPLAY_FEATURE_PARAM",
    "RANDOM_INIT_PIN_PARAM",
};

const QRegularExpression regVersion("v([0-9]+)\\.([0-9]+)(.*)");

MPDevice::MPDevice(QObject *parent):
    QObject(parent)
{
    set_status(Common::UnknownStatus);
    set_memMgmtMode(false); //by default device is not in MMM

    statusTimer = new QTimer(this);
    statusTimer->start(500);
    connect(statusTimer, &QTimer::timeout, [=]()
    {
        sendData(MP_MOOLTIPASS_STATUS, [=](bool success, const QByteArray &data, bool &)
        {
            if (!success)
                return;
            if ((quint8)data.at(1) == MP_MOOLTIPASS_STATUS)
            {
                Common::MPStatus s = (Common::MPStatus)data.at(2);
                if (s != get_status())
                {
                    qDebug() << "received MP_MOOLTIPASS_STATUS: " << (int)data.at(2);

                    if (s == Common::Unlocked)
                    {
                        QTimer::singleShot(10, [=]()
                        {
                            loadParameters();
                            setCurrentDate();
                        });
                    }
                }
                set_status(s);
            }
            else if ((quint8)data.at(1) == MP_PLEASE_RETRY)
            {
                qDebug() << "Please retry received.";
            }
        });
    });

    connect(this, SIGNAL(platformDataRead(QByteArray)), this, SLOT(newDataRead(QByteArray)));
//    connect(this, SIGNAL(platformFailed()), this, SLOT(commandFailed()));
}

MPDevice::~MPDevice()
{
}

void MPDevice::sendData(unsigned char c, const QByteArray &data, MPCommandCb cb)
{
    MPCommand cmd;

    // Prepare MP packet
    cmd.data.append(data.size());
    cmd.data.append(c);
    cmd.data.append(data);
    cmd.cb = std::move(cb);

    commandQueue.enqueue(cmd);

    if (!commandQueue.head().running)
        sendDataDequeue();
}

void MPDevice::sendData(unsigned char cmd, MPCommandCb cb)
{
    sendData(cmd, QByteArray(), std::move(cb));
}

void MPDevice::sendDataDequeue()
{
    if (commandQueue.isEmpty())
        return;

    MPCommand &currentCmd = commandQueue.head();
    currentCmd.running = true;

    // send data with platform code
    //qDebug() << "Platform send command: " << QString("0x%1").arg((quint8)currentCmd.data[1], 2, 16, QChar('0'));
    platformWrite(currentCmd.data);
}

void MPDevice::runAndDequeueJobs()
{
    if (jobsQueue.isEmpty() || currentJobs)
        return;

    currentJobs = jobsQueue.dequeue();
    currentJobs->start();

    connect(currentJobs, &AsyncJobs::finished, [=](const QByteArray &)
    {
        currentJobs = nullptr;
        runAndDequeueJobs();
    });
    connect(currentJobs, &AsyncJobs::failed, [=](AsyncJob *)
    {
        currentJobs = nullptr;
        runAndDequeueJobs();
    });
}

bool MPDevice::isJobsQueueBusy()
{
    return currentJobs;
}

void MPDevice::loadParameters()
{
    AsyncJobs *jobs = new AsyncJobs(
                          "Loading device parameters",
                          this);

    jobs->append(new MPCommandJob(this,
                                  MP_VERSION,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_VERSION)
        {
            qWarning() << "Get version: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        qDebug() << "received MP version FLASH size: " << (quint8)data.at(2) << "Mb";
        QString hw = QString(data.mid(3, (quint8)data.at(0) - 2));
        qDebug() << "received MP version hw: " << hw;
        set_flashMbSize((quint8)data.at(2));
        set_hwVersion(hw);

        QRegularExpressionMatchIterator i = regVersion.globalMatch(hw);
        if (i.hasNext())
        {
            QRegularExpressionMatch match = i.next();
            int v = match.captured(1).toInt() * 10 +
                    match.captured(2).toInt();
            isFw12Flag = v >= 12;
            isMiniFlag = match.captured(3) == "_mini";
        }

        return true;
    }));

    jobs->append(new MPCommandJob(this,
                                  MP_GET_MOOLTIPASS_PARM,
                                  QByteArray(1, KEYBOARD_LAYOUT_PARAM),
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_MOOLTIPASS_PARM)
        {
            qWarning() << "Get parameter: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        qDebug() << "received language: " << (quint8)data.at(2);
        set_keyboardLayout((quint8)data.at(2));
        return true;
    }));

    jobs->append(new MPCommandJob(this,
                                  MP_GET_MOOLTIPASS_PARM,
                                  QByteArray(1, LOCK_TIMEOUT_ENABLE_PARAM),
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_MOOLTIPASS_PARM)
        {
            qWarning() << "Get parameter: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        qDebug() << "received lock timeout enable: " << (quint8)data.at(2);
        set_lockTimeoutEnabled(data.at(2) != 0);
        return true;
    }));

    jobs->append(new MPCommandJob(this,
                                  MP_GET_MOOLTIPASS_PARM,
                                  QByteArray(1, LOCK_TIMEOUT_PARAM),
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_MOOLTIPASS_PARM)
        {
            qWarning() << "Get parameter: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        qDebug() << "received lock timeout: " << (quint8)data.at(2);
        set_lockTimeout((quint8)data.at(2));
        return true;
    }));

    jobs->append(new MPCommandJob(this,
                                  MP_GET_MOOLTIPASS_PARM,
                                  QByteArray(1, SCREENSAVER_PARAM),
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_MOOLTIPASS_PARM)
        {
            qWarning() << "Get parameter: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        qDebug() << "received screensaver: " << (quint8)data.at(2);
        set_screensaver(data.at(2) != 0);
        return true;
    }));

    jobs->append(new MPCommandJob(this,
                                  MP_GET_MOOLTIPASS_PARM,
                                  QByteArray(1, USER_REQ_CANCEL_PARAM),
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_MOOLTIPASS_PARM)
        {
            qWarning() << "Get parameter: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        qDebug() << "received userRequestCancel: " << (quint8)data.at(2);
        set_userRequestCancel(data.at(2) != 0);
        return true;
    }));

    jobs->append(new MPCommandJob(this,
                                  MP_GET_MOOLTIPASS_PARM,
                                  QByteArray(1, USER_INTER_TIMEOUT_PARAM),
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_MOOLTIPASS_PARM)
        {
            qWarning() << "Get parameter: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        qDebug() << "received userInteractionTimeout: " << (quint8)data.at(2);
        set_userInteractionTimeout((quint8)data.at(2));
        return true;
    }));

    jobs->append(new MPCommandJob(this,
                                  MP_GET_MOOLTIPASS_PARM,
                                  QByteArray(1, FLASH_SCREEN_PARAM),
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_MOOLTIPASS_PARM)
        {
            qWarning() << "Get parameter: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        qDebug() << "received flashScreen: " << (quint8)data.at(2);
        set_flashScreen(data.at(2) != 0);
        return true;
    }));

    jobs->append(new MPCommandJob(this,
                                  MP_GET_MOOLTIPASS_PARM,
                                  QByteArray(1, OFFLINE_MODE_PARAM),
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_MOOLTIPASS_PARM)
        {
            qWarning() << "Get parameter: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        qDebug() << "received offlineMode: " << (quint8)data.at(2);
        set_offlineMode(data.at(2) != 0);
        return true;
    }));

    jobs->append(new MPCommandJob(this,
                                  MP_GET_MOOLTIPASS_PARM,
                                  QByteArray(1, TUTORIAL_BOOL_PARAM),
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_MOOLTIPASS_PARM)
        {
            qWarning() << "Get parameter: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        qDebug() << "received tutorialEnabled: " << (quint8)data.at(2);
        set_tutorialEnabled(data.at(2) != 0);
        return true;
    }));

    jobs->append(new MPCommandJob(this,
                                  MP_GET_MOOLTIPASS_PARM,
                                  QByteArray(1, MINI_OLED_CONTRAST_CURRENT_PARAM),
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_MOOLTIPASS_PARM)
        {
            qWarning() << "Get parameter: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        qDebug() << "received screenBrightness: " << (quint8)data.at(2);
        set_screenBrightness((quint8)data.at(2));
        return true;
    }));

    jobs->append(new MPCommandJob(this,
                                  MP_GET_MOOLTIPASS_PARM,
                                  QByteArray(1, MINI_KNOCK_DETECT_ENABLE_PARAM),
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_MOOLTIPASS_PARM)
        {
            qWarning() << "Get parameter: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        qDebug() << "received set_knockEnabled: " << (quint8)data.at(2);
        set_knockEnabled(data.at(2) != 0);
        return true;
    }));

    jobs->append(new MPCommandJob(this,
                                  MP_GET_MOOLTIPASS_PARM,
                                  QByteArray(1, MINI_KNOCK_THRES_PARAM),
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_MOOLTIPASS_PARM)
        {
            qWarning() << "Get parameter: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        qDebug() << "received knockSensitivity: " << (quint8)data.at(2);
        int v = 1;
        if (data.at(2) == 11) v = 0;
        else if (data.at(2) == 5) v = 2;
        set_knockSensitivity(v);
        return true;
    }));

    connect(jobs, &AsyncJobs::finished, [=](const QByteArray &data)
    {
        Q_UNUSED(data);
        //data is last result
        //all jobs finished success
        qInfo() << "Finished loading device options";

        if (isFw12() && isMini())
        {
            qInfo() << "Mini firmware above v1.2, requesting serial number";

            AsyncJobs* v12jobs = new AsyncJobs("Loading device serial number", this);

            /* Query serial number */
            v12jobs->append(new MPCommandJob(this,
                                          MP_GET_SERIAL,
                                          [=](const QByteArray &data, bool &) -> bool
            {
                if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_SERIAL)
                {
                    qWarning() << "Get serial: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
                    return false;
                }
                serialNumber = ((quint8)data[MP_PAYLOAD_FIELD_INDEX+3]) + ((quint32)((quint8)data[MP_PAYLOAD_FIELD_INDEX+2]) << 8) + ((quint32)((quint8)data[MP_PAYLOAD_FIELD_INDEX+1]) << 16) + ((quint32)((quint8)data[MP_PAYLOAD_FIELD_INDEX+0]) << 24);
                qDebug() << "Mooltipass Mini serial number:" << serialNumber;
                return true;
            }));

            connect(v12jobs, &AsyncJobs::finished, [=](const QByteArray &data)
            {
                Q_UNUSED(data);
                //data is last result
                //all jobs finished success
                qInfo() << "Finished loading Mini serial number";
            });

            connect(v12jobs, &AsyncJobs::failed, [=](AsyncJob *failedJob)
            {
                Q_UNUSED(failedJob);
                qCritical() << "Loading Mini serial number failed";
                loadParameters(); // memory: does it get "piled on?"
            });
            jobsQueue.enqueue(v12jobs);
            runAndDequeueJobs();
        }
    });

    connect(jobs, &AsyncJobs::failed, [=](AsyncJob *failedJob)
    {
        Q_UNUSED(failedJob);
        qCritical() << "Loading option failed";
        loadParameters(); // memory: does it get "piled on?"
    });

    jobsQueue.enqueue(jobs);
    runAndDequeueJobs();
}

void MPDevice::commandFailed()
{
    //TODO: fix this to work as it should on all platforms
    //this must be only called once when something went wrong
    //with the current command
//    MPCommand currentCmd = commandQueue.head();
//    currentCmd.cb(false, QByteArray());
//    commandQueue.dequeue();

//    QTimer::singleShot(150, this, SLOT(sendDataDequeue()));
}

void MPDevice::newDataRead(const QByteArray &data)
{
    //we assume that the QByteArray size is at least 64 bytes
    //this should be done by the platform code

    //qWarning() << "---> Packet data " << " size:" << (quint8)data[0] << " data:" << QString("0x%1").arg((quint8)data[1], 2, 16, QChar('0'));
    if ((quint8)data[1] == MP_DEBUG)
        qWarning() << data;

    if (commandQueue.isEmpty())
    {
        qWarning() << "Command queue is empty!";
        qWarning() << "Packet data " << " size:" << (quint8)data[0] << " data:" << data;
        return;
    }

    MPCommand currentCmd = commandQueue.head();

    bool done = true;
    currentCmd.cb(true, data, done);

    if (done)
    {
        commandQueue.dequeue();
        sendDataDequeue();
    }
}

void MPDevice::updateKeyboardLayout(int lang)
{
    updateParam(KEYBOARD_LAYOUT_PARAM, lang);
}

void MPDevice::updateParam(quint8 param, int val)
{
    QString logInf = QStringLiteral("Updating %1 param: %2").arg(mooltipassParamString[param]).arg(val);

    AsyncJobs *jobs = new AsyncJobs(logInf, this);

    QByteArray ba;
    ba.append((quint8)param);
    ba.append((quint8)val);

    jobs->append(new MPCommandJob(this, MP_SET_MOOLTIPASS_PARM, ba, MPCommandJob::defaultCheckRet));

    connect(jobs, &AsyncJobs::finished, [=](const QByteArray &)
    {
        qInfo() << mooltipassParamString[param] << " param updating with success";
    });
    connect(jobs, &AsyncJobs::failed, [=](AsyncJob *)
    {
        qWarning() << "Failed to change " << mooltipassParamString[param];
    });

    jobsQueue.enqueue(jobs);
    runAndDequeueJobs();
}

void MPDevice::updateParam(quint8 param, bool en)
{
    QString logInf = QStringLiteral("Updating lock timeout enabled: %1").arg(en);

    AsyncJobs *jobs = new AsyncJobs(logInf, this);

    QByteArray ba;
    ba.append((quint8)param);
    ba.append((quint8)en);

    jobs->append(new MPCommandJob(this, MP_SET_MOOLTIPASS_PARM, ba, MPCommandJob::defaultCheckRet));

    connect(jobs, &AsyncJobs::finished, [=](const QByteArray &)
    {
        qInfo() << "lock timeout param set success";
    });
    connect(jobs, &AsyncJobs::failed, [=](AsyncJob *)
    {
        qWarning() << "Failed to set lock timeout enabled";
    });

    jobsQueue.enqueue(jobs);
    runAndDequeueJobs();
}

void MPDevice::updateLockTimeoutEnabled(bool en)
{
    updateParam(LOCK_TIMEOUT_ENABLE_PARAM, en);
}

void MPDevice::updateLockTimeout(int timeout)
{
    if (timeout < 0) timeout = 0;
    if (timeout > 0xFF) timeout = 0xFF;
    updateParam(LOCK_TIMEOUT_PARAM, timeout);
}

void MPDevice::updateScreensaver(bool en)
{
    updateParam(SCREENSAVER_PARAM, en);
  }

void MPDevice::updateUserRequestCancel(bool en)
{   
    updateParam(USER_REQ_CANCEL_PARAM, en);
}

void MPDevice::updateUserInteractionTimeout(int timeout)
{   
    if (timeout < 0) timeout = 0;
    if (timeout > 0xFF) timeout = 0xFF;
    updateParam(USER_INTER_TIMEOUT_PARAM, timeout);
}

void MPDevice::updateFlashScreen(bool en)
{
    updateParam(FLASH_SCREEN_PARAM, en);
}

void MPDevice::updateOfflineMode(bool en)
{
    updateParam(OFFLINE_MODE_PARAM, en);
}

void MPDevice::updateTutorialEnabled(bool en)
{
    updateParam(TUTORIAL_BOOL_PARAM, en);
}

void MPDevice::updateScreenBrightness(int bval) //In percent
{
    updateParam(MINI_OLED_CONTRAST_CURRENT_PARAM, bval);
}

void MPDevice::updateKnockEnabled(bool en)
{
    updateParam(MINI_KNOCK_DETECT_ENABLE_PARAM, en);
}

void MPDevice::updateKnockSensitivity(int s) // 0-low, 1-medium, 2-high
{
    quint8 v = 8;
    if (s == 0) v = 11;
    else if (s == 2) v = 5;
    updateParam(MINI_KNOCK_THRES_PARAM, s);
}

void MPDevice::startMemMgmtMode()
{
    /* Start MMM here, and load all memory data from the device */

    /* If we're already in MMM, return */
    if (get_memMgmtMode())
    {
        return;
    }

    /* New job for starting MMM */
    AsyncJobs *jobs = new AsyncJobs("Starting MMM mode", this);

    /* Ask device to go into MMM first */
    jobs->append(new MPCommandJob(this, MP_START_MEMORYMGMT, MPCommandJob::defaultCheckRet));

    /* Get CTR value */
    jobs->append(new MPCommandJob(this, MP_GET_CTRVALUE,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_CTRVALUE)
        {
            /* Wrong packet received */
            qCritical() << "Get CTR value: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        else if (data[MP_LEN_FIELD_INDEX] == 1)
        {
            /* Received one byte as answer: command fail */
            qCritical() << "Get CTR value: couldn't get answer";
            return false;
        }
        else
        {
            ctrValue = data.mid(MP_PAYLOAD_FIELD_INDEX, data[MP_LEN_FIELD_INDEX]);
            ctrValueClone = data.mid(MP_PAYLOAD_FIELD_INDEX, data[MP_LEN_FIELD_INDEX]);
            qDebug() << "CTR value:" << ctrValue.toHex();
            return true;
        }
    }));

    /* Get CPZ and CTR values */
    jobs->append(new MPCommandJob(this, MP_GET_CARD_CPZ_CTR,
                                  [=](const QByteArray &data, bool &done) -> bool
    {
        /* The Mooltipass answers with CPZ CTR packets containing the CPZ_CTR values, and then a final MP_GET_CARD_CPZ_CTR packet */
        if ((quint8)data[1] == MP_CARD_CPZ_CTR_PACKET)
        {
            QByteArray cpz = data.mid(MP_PAYLOAD_FIELD_INDEX, data[MP_LEN_FIELD_INDEX]);
            if(cpzCtrValue.contains(cpz))
            {
                qDebug() << "Duplicate CPZ CTR value:" << cpz.toHex();
            }
            else
            {
                qDebug() << "CPZ CTR value:" << cpz.toHex();
                cpzCtrValue.append(cpz);
                cpzCtrValueClone.append(cpz);
            }
            done = false;
            return true;
        }
        else if((quint8)data[1] == MP_GET_CARD_CPZ_CTR)
        {
            /* Received packet indicating we received all CPZ CTR packets */
            qDebug() << "All CPZ CTR packets received";
            return true;
        }
        else
        {
            qCritical() << "Get CPZ CTR: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
    }));

    /* Get favorites */
    favoritesAddrs.clear();
    favoritesAddrsClone.clear();
    for (int i = 0; i<MOOLTIPASS_FAV_MAX; i++)
    {
        jobs->append(new MPCommandJob(this, MP_GET_FAVORITE,
                                      QByteArray(1, (quint8)i),
                                      [=](const QByteArray &, QByteArray &) -> bool
        {
            if (i == 0)
            {
                qInfo() << "Loading favorites...";
            }
            return true;
        },
                                      [=](const QByteArray &data, bool &) -> bool
        {
            if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_FAVORITE)
            {
                /* Wrong packet received */
                qCritical() << "Get favorite: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
                return false;
            }
            else if (data[MP_LEN_FIELD_INDEX] == 1)
            {
                /* Received one byte as answer: command fail */
                qCritical() << "Get favorite: couldn't get answer";
                return false;
            }
            else
            {
                /* Append favorite to list */
                qDebug() << "Favorite" << i << ": parent address:" << data.mid(MP_PAYLOAD_FIELD_INDEX, 2).toHex() << ", child address:" << data.mid(MP_PAYLOAD_FIELD_INDEX+2, 2).toHex();
                favoritesAddrs.append(data.mid(MP_PAYLOAD_FIELD_INDEX, MOOLTIPASS_ADDRESS_SIZE));
                favoritesAddrsClone.append(data.mid(MP_PAYLOAD_FIELD_INDEX, MOOLTIPASS_ADDRESS_SIZE));
                return true;
            }
        }));
    }

    /* Delete node list */
    qDeleteAll(loginNodes);
    loginNodes.clear();
    qDeleteAll(loginNodesClone);
    loginNodesClone.clear();

    /* Get parent node start address */
    jobs->append(new MPCommandJob(this, MP_GET_STARTING_PARENT,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_STARTING_PARENT)
        {
            /* Wrong packet received */
            qCritical() << "Get start node addr: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        else if (data[MP_LEN_FIELD_INDEX] == 1)
        {
            /* Received one byte as answer: command fail */
            qCritical() << "Get start node addr: couldn't get answer";
            return false;
        }
        else
        {
            startNode = data.mid(MP_PAYLOAD_FIELD_INDEX, data[MP_LEN_FIELD_INDEX]);
            startNodeClone = data.mid(MP_PAYLOAD_FIELD_INDEX, data[MP_LEN_FIELD_INDEX]);
            qDebug() << "Start node addr:" << startNode.toHex();

            //if parent address is not null, load nodes
            if (startNode != MPNode::EmptyAddress)
            {
                qInfo() << "Loading parent nodes...";
                loadLoginNode(jobs, startNode);
            }
            else
            {
                qInfo() << "No parent nodes to load.";
            }

            return true;
        }
    }));

    /* Delete data node list */
    qDeleteAll(dataNodes);
    dataNodes.clear();
    qDeleteAll(dataNodesClone);
    dataNodesClone.clear();

    //Get parent data node start address
    jobs->append(new MPCommandJob(this, MP_GET_DN_START_PARENT,
                                  [=](const QByteArray &data, bool &) -> bool
    {

        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_GET_DN_START_PARENT)
        {
            /* Wrong packet received */
            qCritical() << "Get data start node addr: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        else if (data[MP_LEN_FIELD_INDEX] == 1)
        {
            /* Received one byte as answer: command fail */
            qCritical() << "Get data start node addr: couldn't get answer";
            return false;
        }
        else
        {
            startDataNode = data.mid(MP_PAYLOAD_FIELD_INDEX, data[MP_LEN_FIELD_INDEX]);
            qDebug() << "Start data node addr:" << startDataNode.toHex();

            //if parent address is not null, load nodes
            if (startDataNode != MPNode::EmptyAddress)
            {
                qInfo() << "Loading data parent nodes...";
                loadDataNode(jobs, startDataNode);
            }
            else
            {
                qInfo() << "No parent data nodes to load.";
            }

            return true;
        }
    }));

    connect(jobs, &AsyncJobs::finished, [=](const QByteArray &data)
    {
        Q_UNUSED(data);
        //data is last result
        //all jobs finished success

        qInfo() << "Mem management mode enabled";
        force_memMgmtMode(true);
    });

    connect(jobs, &AsyncJobs::failed, [=](AsyncJob *failedJob)
    {
        Q_UNUSED(failedJob);
        qCritical() << "Setting device in MMM failed";


        /* Cleaning all temp values */
        ctrValue.clear();
        cpzCtrValue.clear();
        qDeleteAll(loginNodes);
        loginNodes.clear();
        qDeleteAll(dataNodes);
        dataNodes.clear();
        favoritesAddrs.clear();
        /* Cleaning the clones as well */
        ctrValueClone.clear();
        cpzCtrValueClone.clear();
        qDeleteAll(loginNodesClone);
        loginNodesClone.clear();
        qDeleteAll(dataNodesClone);
        dataNodesClone.clear();
        favoritesAddrsClone.clear();

        exitMemMgmtMode();
        force_memMgmtMode(false);
    });

    jobsQueue.enqueue(jobs);
    runAndDequeueJobs();
}

void MPDevice::loadLoginNode(AsyncJobs *jobs, const QByteArray &address)
{    
    qDebug() << "Loading cred parent node at address: " << address.toHex();

    /* Create new parent node, append to list */
    MPNode *pnode = new MPNode(this, address);
    loginNodes.append(pnode);
    MPNode *pnodeClone = new MPNode(this, address);
    loginNodesClone.append(pnodeClone);

    /* Send read node command, expecting 3 packets */
    jobs->append(new MPCommandJob(this, MP_READ_FLASH_NODE,
                                  address,
                                  [=](const QByteArray &data, bool &done) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_READ_FLASH_NODE)
        {
            /* Wrong packet received */
            qCritical() << "Get node: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        else if (data[MP_LEN_FIELD_INDEX] == 1)
        {
            /* Received one byte as answer: command fail */
            qCritical() << "Get node: couldn't get answer";
            return false;
        }
        else
        {
            /* Append received data to node data */
            pnode->appendData(data.mid(MP_PAYLOAD_FIELD_INDEX, data[MP_LEN_FIELD_INDEX]));
            pnodeClone->appendData(data.mid(MP_PAYLOAD_FIELD_INDEX, data[MP_LEN_FIELD_INDEX]));

            //Continue to read data until the node is fully received
            if (!pnode->isDataLengthValid())
            {
                done = false;
            }
            else
            {
                //Node is loaded
                qDebug() << address.toHex() << ": parent node loaded:" << pnode->getService();

                if (pnode->getStartChildAddress() != MPNode::EmptyAddress)
                {
                    qDebug() << pnode->getService() << ": loading child nodes...";
                    loadLoginChildNode(jobs, pnode, pnodeClone, pnode->getStartChildAddress());
                }
                else
                {
                    qDebug() << "Parent does not have childs.";
                }

                //Load next parent
                if (pnode->getNextParentAddress() != MPNode::EmptyAddress)
                {
                    loadLoginNode(jobs, pnode->getNextParentAddress());
                }
            }

            return true;
        }
    }));
}

void MPDevice::loadLoginChildNode(AsyncJobs *jobs, MPNode *parent, MPNode *parentClone, const QByteArray &address)
{
    qDebug() << "Loading cred child node at address:" << address.toHex();

    /* Create empty child node and add it to the list */
    MPNode *cnode = new MPNode(this, address);
    loginChildNodes.append(cnode);
    parent->appendChild(cnode);
    MPNode *cnodeClone = new MPNode(this, address);
    loginChildNodesClone.append(cnodeClone);
    parentClone->appendChild(cnodeClone);

    /* Query node */
    jobs->prepend(new MPCommandJob(this, MP_READ_FLASH_NODE,
                                  address,
                                  [=](const QByteArray &data, bool &done) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_READ_FLASH_NODE)
        {
            /* Wrong packet received */
            qCritical() << "Get child node: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        else if (data[MP_LEN_FIELD_INDEX] == 1)
        {
            /* Received one byte as answer: command fail */
            qCritical() << "Get child node: couldn't get answer";
            return false;
        }
        else
        {
            /* Append received data to node data */
            cnode->appendData(data.mid(MP_PAYLOAD_FIELD_INDEX, data[MP_LEN_FIELD_INDEX]));
            cnodeClone->appendData(data.mid(MP_PAYLOAD_FIELD_INDEX, data[MP_LEN_FIELD_INDEX]));

            //Continue to read data until the node is fully received
            if (!cnode->isDataLengthValid())
            {
                done = false;
            }
            else
            {
                //Node is loaded
                qDebug() << address.toHex() << ": child node loaded:" << cnode->getLogin();

                //Load next child
                if (cnode->getNextChildAddress() != MPNode::EmptyAddress)
                {
                    loadLoginChildNode(jobs, parent, parentClone, cnode->getNextChildAddress());
                }
            }

            return true;
        }
    }));
}

void MPDevice::loadDataNode(AsyncJobs *jobs, const QByteArray &address)
{
    MPNode *pnode = new MPNode(this, address);
    dataNodes.append(pnode);

    qDebug() << "Loading data parent node at address: " << address;

    jobs->append(new MPCommandJob(this, MP_READ_FLASH_NODE,
                                  address,
                                  [=](const QByteArray &data, bool &done) -> bool
    {
        if ((quint8)data[0] <= 1) return false;

        pnode->appendData(data.mid(2, data[0]));

        //Continue to read data until the node is fully received
        if (!pnode->isValid())
            done = false;
        else
        {
            //Node is loaded
            qDebug() << "Parent data node loaded: " << pnode->getService();

            //Load data child
            if (pnode->getStartChildAddress() != MPNode::EmptyAddress)
            {
                qDebug() << "Loading data child nodes...";
                loadDataChildNode(jobs, pnode, pnode->getStartChildAddress());
            }
            else
                qDebug() << "Parent data node does not have childs.";

            //Load next parent
            if (pnode->getNextParentAddress() != MPNode::EmptyAddress)
                loadDataNode(jobs, pnode->getNextParentAddress());
        }

        return true;
    }));
}

void MPDevice::loadDataChildNode(AsyncJobs *jobs, MPNode *parent, const QByteArray &address)
{
    MPNode *cnode = new MPNode(this, address);
    parent->appendChildData(cnode);

    qDebug() << "Loading data child node at address: " << address;

    jobs->prepend(new MPCommandJob(this, MP_READ_FLASH_NODE,
                                  address,
                                  [=](const QByteArray &data, bool &done) -> bool
    {
        if ((quint8)data[0] <= 1) return false;

        cnode->appendData(data.mid(2, data[0]));

        //Continue to read data until the node is fully received
        if (!cnode->isValid())
            done = false;
        else
        {
            //Node is loaded
            qDebug() << "Child data node loaded";

            //Load next child
            if (cnode->getNextChildDataAddress() != MPNode::EmptyAddress)
                loadDataChildNode(jobs, parent, cnode->getNextChildDataAddress());
        }

        return true;
    }));
}

/* Find a node inside a given list given his address */
MPNode *MPDevice::findNodeWithAddressInList(QList<MPNode *> list, const QByteArray &address)
{
    auto it = std::find_if(list.begin(), list.end(), [&address](const MPNode *const node)
    {
        return node->getAddress() == address;
    });

    return it == list.end()?nullptr:*it;
}

/* Follow the chain to tag pointed node (useful when doing integrity check when we are getting everything we can) */
void MPDevice::tagPointedNodes()
{
    QByteArray tempParentAddress;
    QByteArray tempChildAddress;
    MPNode* tempNextParentNodePt;
    MPNode* tempParentNodePt;
    MPNode* tempNextChildNodePt;
    MPNode* tempChildNodePt;

    /* start with start node (duh) */
    tempParentAddress = startNode;

    /* Loop through the parent nodes */
    while (tempParentAddress != MPNode::EmptyAddress)
    {
        /* Get pointer to next parent node */
        tempNextParentNodePt = findNodeWithAddressInList(loginNodes, tempParentAddress);

        /* Check that we could actually find it */
        if (tempNextParentNodePt == 0)
        {
            qCritical() << "tagPointedNodes: couldn't find parent node with address" << tempParentAddress.toHex() << "in our list";

            /* TODO: fix this node */
            if (tempParentAddress == startNode)
            {
                /* start node is incorrect */
            }
            else
            {
            }

            /* Stop looping */
            return;
        }
        else
        {
            /* check previous node address */
            if (tempParentAddress == startNode)
            {
                /* first parent node: previous address should be an empty one */
                if (tempNextParentNodePt->getPreviousParentAddress() != MPNode::EmptyAddress)
                {
                    qWarning() << "tagPointedNodes: parent node" << tempNextParentNodePt->getService() <<  "at address" << tempParentAddress.toHex() << "has incorrect previous address:" << tempNextParentNodePt->getPreviousParentAddress().toHex() << "instead of" << MPNode::EmptyAddress.toHex();
                }
            }
            else
            {
                /* normal linked chain */
                if (tempNextParentNodePt->getPreviousParentAddress() != tempParentNodePt->getAddress())
                {
                    qWarning() << "tagPointedNodes: parent node" << tempNextParentNodePt->getService() <<  "at address" << tempParentAddress.toHex() << "has incorrect previous address:" << tempNextParentNodePt->getPreviousParentAddress().toHex() << "instead of" << tempParentNodePt->getAddress().toHex();
                }
            }

            /* Set correct pointed node */
            tempParentNodePt = tempNextParentNodePt;

            /* tag parent */
            tempParentNodePt->setPointedToCheck();

            /* get first child */
            tempChildAddress = tempParentNodePt->getStartChildAddress();

            /* browse through all the children */
            while (tempChildAddress != MPNode::EmptyAddress)
            {
                /* Get pointer to the child node */
                tempNextChildNodePt = findNodeWithAddressInList(loginChildNodes, tempChildAddress);

                /* Check we could find child pointer */
                if (tempNextChildNodePt == 0)
                {
                    qWarning() << "tagPointedNodes: couldn't find child node with address" << tempChildAddress.toHex() << "in our list";

                    /* TODO: fix prev child / parent node */
                    if (tempChildAddress == tempParentNodePt->getStartChildAddress())
                    {
                        // first child
                    }
                    else
                    {
                        // fix prev child
                    }

                    /* Loop to next parent */
                    tempChildAddress = MPNode::EmptyAddress;
                }
                else
                {
                    /* check previous node address */
                    if (tempChildAddress == tempParentNodePt->getStartChildAddress())
                    {
                        /* first child node in given parent: previous address should be an empty one */
                        if (tempNextChildNodePt->getPreviousChildAddress() != MPNode::EmptyAddress)
                        {
                            qWarning() << "tagPointedNodes: child node" << tempNextChildNodePt->getLogin() <<  "at address" << tempChildAddress.toHex() << "has incorrect previous address:" << tempNextChildNodePt->getPreviousChildAddress().toHex() << "instead of" << MPNode::EmptyAddress.toHex();
                        }
                    }
                    else
                    {
                        /* normal linked chain */
                        if (tempNextChildNodePt->getPreviousChildAddress() != tempChildNodePt->getAddress())
                        {
                            qWarning() << "tagPointedNodes: child node" << tempNextChildNodePt->getLogin() <<  "at address" << tempChildAddress.toHex() << "has incorrect previous address:" << tempNextChildNodePt->getPreviousChildAddress().toHex() << "instead of" << tempChildNodePt->getAddress().toHex();
                        }
                    }

                    /* Set correct pointed node */
                    tempChildNodePt = tempNextChildNodePt;

                    /* Tag child */
                    tempChildNodePt->setPointedToCheck();

                    /* Loop to next possible child */
                    tempChildAddress = tempChildNodePt->getNextChildAddress();
                }
            }

            /* get next parent address */
            tempParentAddress = tempParentNodePt->getNextParentAddress();
        }
    }
}

bool MPDevice::checkLoadedNodes()
{
    QList<MPNode *>::iterator i;

    qInfo() << "Checking database...";

    /* Tag pointed nodes */
    tagPointedNodes();

    /* Scan for orphan nodes */
    quint32 nbOrphanParents = 0;
    quint32 nbOrphanChildren = 0;
    for (i = loginNodes.begin(); i != loginNodes.end(); i++)
    {
        if ((*i)->getPointedToCheck() == false)
        {
            qWarning() << "Orphan parent found:" << (*i)->getService() << "at address:" << (*i)->getAddress();
            nbOrphanParents++;
        }
    }
    for (i = loginChildNodes.begin(); i != loginChildNodes.end(); i++)
    {
        if ((*i)->getPointedToCheck() == false)
        {
            qWarning() << "Orphan child found:" << (*i)->getLogin() << "at address:" << (*i)->getAddress();
            nbOrphanParents++;
        }
    }
    qInfo() << "Number of parent orphans:" << nbOrphanParents;
    qInfo() << "Number of children orphans:" << nbOrphanChildren;

    qInfo() << "Database check OK";
    return true;
}

bool MPDevice::generateSavePackets()
{
    return true;
}

void MPDevice::exitMemMgmtMode()
{
    checkLoadedNodes();

    AsyncJobs *jobs = new AsyncJobs("Exiting MMM", this);

    jobs->append(new MPCommandJob(this, MP_END_MEMORYMGMT, MPCommandJob::defaultCheckRet));

    connect(jobs, &AsyncJobs::finished, [=](const QByteArray &)
    {
        //data is last result
        //all jobs finished success

        qInfo() << "MMM exit ok";

        /* Debug */
        /*qDebug() << ctrValue;
        qDebug() << ctrValueClone;
        qDebug() << startNode;
        qDebug() << startNodeClone;
        qDebug() << cpzCtrValue;
        qDebug() << cpzCtrValueClone;
        qDebug() << favoritesAddrs;
        qDebug() << favoritesAddrsClone;
        qDebug() << loginNodes;         //list of all parent nodes for credentials
        qDebug() << loginNodesClone;         //list of all parent nodes for credentials
        qDebug() << loginChildNodes;    //list of all parent nodes for credentials
        qDebug() << loginChildNodesClone;    //list of all parent nodes for credentials
        qDebug() << dataNodes;          //list of all parent nodes for data nodes
        qDebug() << dataNodesClone;          //list of all parent nodes for data nodes*/

        /* Cleaning all temp values */
        ctrValue.clear();
        cpzCtrValue.clear();
        qDeleteAll(loginNodes);
        loginNodes.clear();
        qDeleteAll(dataNodes);
        dataNodes.clear();
        favoritesAddrs.clear();
        /* Cleaning the clones as well */
        ctrValueClone.clear();
        cpzCtrValueClone.clear();
        qDeleteAll(loginNodesClone);
        loginNodesClone.clear();
        qDeleteAll(dataNodesClone);
        dataNodesClone.clear();
        favoritesAddrsClone.clear();

        force_memMgmtMode(false);
    });

    connect(jobs, &AsyncJobs::failed, [=](AsyncJob *)
    {
        qCritical() << "Failed to exit MMM";

        /* Cleaning all temp values */
        ctrValue.clear();
        cpzCtrValue.clear();
        qDeleteAll(loginNodes);
        loginNodes.clear();
        qDeleteAll(dataNodes);
        dataNodes.clear();
        favoritesAddrs.clear();
        /* Cleaning the clones as well */
        ctrValueClone.clear();
        cpzCtrValueClone.clear();
        qDeleteAll(loginNodesClone);
        loginNodesClone.clear();
        qDeleteAll(dataNodesClone);
        dataNodesClone.clear();
        favoritesAddrsClone.clear();

        force_memMgmtMode(false);
    });

    jobsQueue.enqueue(jobs);
    runAndDequeueJobs();
}

void MPDevice::setCurrentDate()
{
    AsyncJobs *jobs = new AsyncJobs("Send date to device", this);

    jobs->append(new MPCommandJob(this, MP_SET_DATE,
                                  [=](const QByteArray &, QByteArray &data_to_send) -> bool
    {
        data_to_send.clear();
        data_to_send.append(Common::dateToBytes(QDate::currentDate()));

        qDebug() << "Sending current date: " <<
                    QString("0x%1").arg((quint8)data_to_send[0], 2, 16, QChar('0')) <<
                    QString("0x%1").arg((quint8)data_to_send[1], 2, 16, QChar('0'));

        return true;
    },
                                [=](const QByteArray &data, bool &) -> bool
    {
        if ((quint8)data[MP_CMD_FIELD_INDEX] != MP_SET_DATE)
        {
            qWarning() << "Set date: wrong command received as answer:" << QString("0x%1").arg((quint8)data[MP_CMD_FIELD_INDEX], 0, 16);
            return false;
        }
        else
        {
            return true;
        }
    }));

    connect(jobs, &AsyncJobs::finished, [=](const QByteArray &)
    {
        qInfo() << "Date set success";

        /* If v1.2 firmware, query user change number */
        if (isFw12())
        {
            qInfo() << "Firmware above v1.2, requesting change numbers";
            getChangeNumbers();
        }
    });
    connect(jobs, &AsyncJobs::failed, [=](AsyncJob *)
    {
        qWarning() << "Failed to set date on device";
        setCurrentDate(); // memory: does it get piled on?
    });

    jobsQueue.enqueue(jobs);
    runAndDequeueJobs();
}

void MPDevice::getChangeNumbers()
{
    AsyncJobs* v12jobs = new AsyncJobs("Loading device db change numbers", this);

    /* Query change number */
    v12jobs->append(new MPCommandJob(this,
                                  MP_GET_USER_CHANGE_NB,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if (data[MP_PAYLOAD_FIELD_INDEX] == 0)
        {
            qWarning() << "Couldn't request change numbers";
        }
        else
        {
            credentialsDbChangeNumber = (quint8)data[MP_PAYLOAD_FIELD_INDEX+1];
            dataDbChangeNumber = (quint8)data[MP_PAYLOAD_FIELD_INDEX+2];
            qDebug() << "Credentials change number:" << credentialsDbChangeNumber;
            qDebug() << "Data change number:" << dataDbChangeNumber;
        }
        return true;
    }));

    connect(v12jobs, &AsyncJobs::finished, [=](const QByteArray &data)
    {
        Q_UNUSED(data);
        //data is last result
        //all jobs finished success
        qInfo() << "Finished loading change numbers";
    });

    connect(v12jobs, &AsyncJobs::failed, [=](AsyncJob *failedJob)
    {
        Q_UNUSED(failedJob);
        qCritical() << "Loading change numbers failed";
        getChangeNumbers(); // memory: does it get piled on?
    });

    jobsQueue.enqueue(v12jobs);
    runAndDequeueJobs();
}

void MPDevice::cancelUserRequest(const QString &reqid)
{
    // send data with platform code
    // This command is used to cancel a request.
    // As the request may block the sending queue, we directly send the command
    // and bypass the queue.

    if (!isFw12())
    {
        qDebug() << "cancelUserRequest not supported for fw < 1.2";
        return;
    }

    qInfo() << "cancel user request (reqid: " << reqid << ")";

    if (currentJobs->getJobsId() == reqid)
    {
        qInfo() << "request_id match current one. Cancel current request";

        QByteArray ba;
        ba.append((char)0);
        ba.append(MP_CANCEL_USER_REQUEST);

        qDebug() << "Platform send command: " << QString("0x%1").arg((quint8)ba[1], 2, 16, QChar('0'));
        platformWrite(ba);
        return;
    }

    //search for an existing jobid in the queue.
    for (AsyncJobs *j: qAsConst(jobsQueue))
    {
        if (j->getJobsId() == reqid)
        {
            qInfo() << "Removing request from queue";
            jobsQueue.removeAll(j);
            return;
        }
    }

    qWarning() << "No request found for reqid: " << reqid;
}

void MPDevice::getCredential(const QString &service, const QString &login, const QString &fallback_service, const QString &reqid,
                             std::function<void(bool success, QString errstr, const QString &_service, const QString &login, const QString &pass, const QString &desc)> cb)
{
    QString logInf = QStringLiteral("Ask for password for service: %1 login: %2 fallback_service: %3 reqid: %4")
                     .arg(service)
                     .arg(login)
                     .arg(fallback_service)
                     .arg(reqid);

    AsyncJobs *jobs;
    if (reqid.isEmpty())
        jobs = new AsyncJobs(logInf, this);
    else
        jobs = new AsyncJobs(logInf, reqid, this);

    QByteArray sdata = service.toUtf8();
    sdata.append((char)0);

    jobs->append(new MPCommandJob(this, MP_CONTEXT,
                                  sdata,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if (data[2] != 1)
        {
            if (!fallback_service.isEmpty())
            {
                QByteArray fsdata = fallback_service.toUtf8();
                fsdata.append((char)0);
                jobs->prepend(new MPCommandJob(this, MP_CONTEXT,
                                              fsdata,
                                              [=](const QByteArray &data, bool &) -> bool
                {
                    if (data[2] != 1)
                    {
                        qWarning() << "Error setting context: " << (quint8)data[2];
                        jobs->setCurrentJobError("failed to select context and fallback_context on device");
                        return false;
                    }

                    QVariantMap m = {{ "service", fallback_service }};
                    jobs->user_data = m;

                    return true;
                }));
                return true;
            }

            qWarning() << "Error setting context: " << (quint8)data[2];
            jobs->setCurrentJobError("failed to select context on device");
            return false;
        }

        QVariantMap m = {{ "service", service }};
        jobs->user_data = m;

        return true;
    }));

    jobs->append(new MPCommandJob(this, MP_GET_LOGIN,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if (data[2] == 0 && !login.isEmpty())
        {
            jobs->setCurrentJobError("credential access refused by user");
            return false;
        }

        QString l = data.mid(MP_PAYLOAD_FIELD_INDEX, data[MP_LEN_FIELD_INDEX]);
        if (!login.isEmpty() && l != login)
        {
            jobs->setCurrentJobError("login mismatch");
            return false;
        }

        QVariantMap m = jobs->user_data.toMap();
        m["login"] = l;
        jobs->user_data = m;

        return true;
    }));

    jobs->append(new MPCommandJob(this, MP_GET_DESCRIPTION,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if (data[2] == 0)
        {
            jobs->setCurrentJobError("failed to query description on device");
            qWarning() << "failed to query description on device";
            return true; //Do not fail if description is not available for this node
        }
        QVariantMap m = jobs->user_data.toMap();
        m["description"] = data.mid(MP_PAYLOAD_FIELD_INDEX, data[MP_LEN_FIELD_INDEX]);
        jobs->user_data = m;
        return true;
    }));

    jobs->append(new MPCommandJob(this, MP_GET_PASSWORD,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if (data[2] == 0)
        {
            jobs->setCurrentJobError("failed to query password on device");
            return false;
        }
        return true;
    }));

    connect(jobs, &AsyncJobs::finished, [=](const QByteArray &data)
    {
        //data is last result
        //all jobs finished success

        qInfo() << "Password retreived ok";
        QString pass = data.mid(MP_PAYLOAD_FIELD_INDEX, data[MP_LEN_FIELD_INDEX]);

        QVariantMap m = jobs->user_data.toMap();
        cb(true, QString(), m["service"].toString(), m["login"].toString(), pass, m["description"].toString());
    });

    connect(jobs, &AsyncJobs::failed, [=](AsyncJob *failedJob)
    {
        qCritical() << "Failed getting password: " << failedJob->getErrorStr();
        cb(false, failedJob->getErrorStr(), QString(), QString(), QString(), QString());
    });

    jobsQueue.enqueue(jobs);
    runAndDequeueJobs();
}

void MPDevice::getRandomNumber(std::function<void(bool success, QString errstr, const QByteArray &nums)> cb)
{
    AsyncJobs *jobs = new AsyncJobs("Get random numbers from device", this);

    jobs->append(new MPCommandJob(this, MP_GET_RANDOM_NUMBER, QByteArray()));

    connect(jobs, &AsyncJobs::finished, [=](const QByteArray &data)
    {
        //data is last result
        //all jobs finished success

        qInfo() << "Random numbers generated ok";
        cb(true, QString(), data);
    });

    connect(jobs, &AsyncJobs::failed, [=](AsyncJob *failedJob)
    {
        Q_UNUSED(failedJob);
        qCritical() << "Failed generating rng";
        cb(false, "failed to generate random numbers", QByteArray());
    });

    jobsQueue.enqueue(jobs);
    runAndDequeueJobs();
}

void MPDevice::createJobAddContext(const QString &service, AsyncJobs *jobs, bool isDataNode)
{
    QByteArray sdata = service.toUtf8();
    sdata.append((char)0);

    quint8 cmdAddCtx = isDataNode?MP_ADD_DATA_SERVICE:MP_ADD_CONTEXT;
    quint8 cmdSelectCtx = isDataNode?MP_SET_DATA_SERVICE:MP_CONTEXT;

    //Create context
    jobs->prepend(new MPCommandJob(this, cmdAddCtx,
                  sdata,
                  [=](const QByteArray &data, bool &) -> bool
    {
        if (data[2] != 1)
        {
            qWarning() << "Failed to add new context";
            jobs->setCurrentJobError("add_context failed on device");
            return false;
        }
        qDebug() << "context " << service << " added";
        return true;
    }));

    //choose context
    jobs->insertAfter(new MPCommandJob(this, cmdSelectCtx,
                                  sdata,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if (data[2] != 1)
        {
            qWarning() << "Failed to select new context";
            jobs->setCurrentJobError("unable to selected context on device");
            return false;
        }
        qDebug() << "set_context " << service;
        return true;
    }), 0);
}

void MPDevice::setCredential(const QString &service, const QString &login,
                             const QString &pass, const QString &description, bool setDesc,
                             std::function<void(bool success, QString errstr)> cb)
{
    if (service.isEmpty() ||
        pass.isEmpty())
    {
        qWarning() << "context or pass is empty.";
        cb(false, "context or password is empty");
        return;
    }

    QString logInf = QStringLiteral("Adding/Changing credential for service: %1 login: %2")
                     .arg(service)
                     .arg(login);

    AsyncJobs *jobs = new AsyncJobs(logInf, this);

    QByteArray sdata = service.toUtf8();
    sdata.append((char)0);

    //First query if context exist
    jobs->append(new MPCommandJob(this, MP_CONTEXT,
                                  sdata,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if (data[2] != 1)
        {
            qWarning() << "context " << service << " does not exist";
            //Context does not exists, create it
            createJobAddContext(service, jobs);
        }
        else
            qDebug() << "set_context " << service;
        return true;
    }));

    QByteArray ldata = login.toUtf8();
    ldata.append((char)0);

    jobs->append(new MPCommandJob(this, MP_SET_LOGIN,
                                  ldata,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if (data[2] == 0)
        {
            jobs->setCurrentJobError("set_login failed on device");
            qWarning() << "failed to set login to " << login;
            return false;
        }
        qDebug() << "set_login " << login;
        return true;
    }));

    if (isFw12() && setDesc)
    {
        QByteArray ddata = description.toUtf8();
        ddata.append((char)0);

        //Set description should be done right after set login
        jobs->append(new MPCommandJob(this, MP_SET_DESCRIPTION,
                                      ddata,
                                      [=](const QByteArray &data, bool &) -> bool
        {
            if (data[2] == 0)
            {
                if (description.size() > MOOLTIPASS_DESC_SIZE)
                {
                    qWarning() << "description text is more that " << MOOLTIPASS_DESC_SIZE << " chars";
                    jobs->setCurrentJobError(QString("set_description failed on device, max text length allowed is %1 characters").arg(MOOLTIPASS_DESC_SIZE));
                }
                else
                    jobs->setCurrentJobError("set_description failed on device");
                qWarning() << "Failed to set description to: " << description;
                return false;
            }
            qDebug() << "set_description " << description;
            return true;
        }));
    }

    QByteArray pdata = pass.toUtf8();
    pdata.append((char)0);

    jobs->append(new MPCommandJob(this, MP_CHECK_PASSWORD,
                                  pdata,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if (data[2] != 1)
        {
            //Password does not match, update it
            jobs->prepend(new MPCommandJob(this, MP_SET_PASSWORD,
                                          pdata,
                                          [=](const QByteArray &data, bool &) -> bool
            {
                if (data[2] == 0)
                {
                    jobs->setCurrentJobError("set_password failed on device");
                    qWarning() << "failed to set_password";
                    return false;
                }
                qDebug() << "set_password ok";
                return true;
            }));
        }
        else
            qDebug() << "password not changed";

        return true;
    }));

    connect(jobs, &AsyncJobs::finished, [=](const QByteArray &)
    {
        //all jobs finished success
        qInfo() << "set_credential success";
        cb(true, QString());
    });

    connect(jobs, &AsyncJobs::failed, [=](AsyncJob *failedJob)
    {
        qCritical() << "Failed adding new credential";
        cb(false, failedJob->getErrorStr());
    });

    jobsQueue.enqueue(jobs);
    runAndDequeueJobs();
}

bool MPDevice::getDataNodeCb(AsyncJobs *jobs,
                             std::function<void(int total, int current)> cbProgress,
                             const QByteArray &data, bool &)
{
    using namespace std::placeholders;

    //qDebug() << "getDataNodeCb data size: " << ((quint8)data[0]) << "  " << ((quint8)data[1]) << "  " << ((quint8)data[2]);

    if (data[0] == 1 && //data size is 1
        data[2] == 0)   //value is 0 means end of data
    {
        QVariantMap m = jobs->user_data.toMap();
        if (!m.contains("data"))
        {
            //if no data at all, report an error
            jobs->setCurrentJobError("reading data failed or no data");
            return false;
        }
        return true;
    }

    if (data[0] != 0)
    {
        QVariantMap m = jobs->user_data.toMap();
        QByteArray ba = m["data"].toByteArray();

        //first packet, we can read the file size
        if (ba.isEmpty())
        {
            ba.append(data.mid(2, (int)data.at(0)));
            quint32 sz = qFromBigEndian<quint32>((quint8 *)ba.data());
            m["progress_total"] = sz;
            cbProgress((int)sz, ba.size() - 4);
        }
        else
        {
            ba.append(data.mid(2, (int)data.at(0)));
            cbProgress(m["progress_total"].toInt(), ba.size() - 4);
        }

        m["data"] = ba;
        jobs->user_data = m;

        //ask for the next 32bytes packet
        //bind to a member function of MPDevice, to be able to loop over until with got all the data
        jobs->append(new MPCommandJob(this, MP_READ_32B_IN_DN,
                                      std::bind(&MPDevice::getDataNodeCb, this, jobs, std::move(cbProgress), _1, _2)));
    }
    return true;
}

void MPDevice::getDataNode(const QString &service, const QString &fallback_service, const QString &reqid,
                           std::function<void(bool success, QString errstr, QString serv, QByteArray rawData)> cb,
                           std::function<void(int total, int current)> cbProgress)
{
    if (service.isEmpty())
    {
        qWarning() << "context is empty.";
        cb(false, "context is empty", QString(), QByteArray());
        return;
    }

    QString logInf = QStringLiteral("Ask for data node for service: %1 fallback_service: %2 reqid: %3")
                     .arg(service)
                     .arg(fallback_service)
                     .arg(reqid);

    AsyncJobs *jobs;
    if (reqid.isEmpty())
        jobs = new AsyncJobs(logInf, this);
    else
        jobs = new AsyncJobs(logInf, reqid, this);

    QByteArray sdata = service.toUtf8();
    sdata.append((char)0);

    jobs->append(new MPCommandJob(this, MP_SET_DATA_SERVICE,
                                  sdata,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if (data[2] != 1)
        {
            if (!fallback_service.isEmpty())
            {
                QByteArray fsdata = fallback_service.toUtf8();
                fsdata.append((char)0);
                jobs->prepend(new MPCommandJob(this, MP_SET_DATA_SERVICE,
                                              fsdata,
                                              [=](const QByteArray &data, bool &) -> bool
                {
                    if (data[2] != 1)
                    {
                        qWarning() << "Error setting context: " << (quint8)data[2];
                        jobs->setCurrentJobError("failed to select context and fallback_context on device");
                        return false;
                    }

                    QVariantMap m = {{ "service", fallback_service }};
                    jobs->user_data = m;

                    return true;
                }));
                return true;
            }

            qWarning() << "Error setting context: " << (quint8)data[2];
            jobs->setCurrentJobError("failed to select context on device");
            return false;
        }

        QVariantMap m = {{ "service", service }};
        jobs->user_data = m;

        return true;
    }));

    using namespace std::placeholders;

    //ask for the first 32bytes packet
    //bind to a member function of MPDevice, to be able to loop over until with got all the data
    jobs->append(new MPCommandJob(this, MP_READ_32B_IN_DN,
                                  std::bind(&MPDevice::getDataNodeCb, this, jobs, std::move(cbProgress), _1, _2)));

    connect(jobs, &AsyncJobs::finished, [=](const QByteArray &)
    {
        //all jobs finished success
        qInfo() << "get_data_node success";
        QVariantMap m = jobs->user_data.toMap();
        QByteArray ndata = m["data"].toByteArray();

        //check data size
        quint32 sz = qFromBigEndian<quint32>((quint8 *)ndata.data());
        qDebug() << "Data size: " << sz;

        cb(true, QString(), m["service"].toString(), ndata.mid(4, sz));
    });

    connect(jobs, &AsyncJobs::failed, [=](AsyncJob *failedJob)
    {
        qCritical() << "Failed getting data node";
        cb(false, failedJob->getErrorStr(), QString(), QByteArray());
    });

    jobsQueue.enqueue(jobs);
    runAndDequeueJobs();
}

bool MPDevice::setDataNodeCb(AsyncJobs *jobs, int current,
                             std::function<void(int total, int current)> cbProgress,
                             const QByteArray &data, bool &)
{
    using namespace std::placeholders;

    qDebug() << "setDataNodeCb data current: " << current;

    if (data[2] == 0)
    {
        jobs->setCurrentJobError("writing data to device failed");
        return false;
    }

    //sending finished
    if (current >= currentDataNode.size())
        return true;

    //prepare next block of data
    char eod = (currentDataNode.size() - current <= MOOLTIPASS_BLOCK_SIZE)?1:0;

    QByteArray packet;
    packet.append(eod);
    packet.append(currentDataNode.mid(current, MOOLTIPASS_BLOCK_SIZE));
    packet.resize(MOOLTIPASS_BLOCK_SIZE + 1);

    cbProgress(currentDataNode.size() - 4, current + MOOLTIPASS_BLOCK_SIZE);

    //send 32bytes packet
    //bind to a member function of MPDevice, to be able to loop over until with got all the data
    jobs->append(new MPCommandJob(this, MP_WRITE_32B_IN_DN,
                                  packet,
                                  std::bind(&MPDevice::setDataNodeCb, this, jobs, current + MOOLTIPASS_BLOCK_SIZE,
                                            std::move(cbProgress), _1, _2)));

    return true;
}

void MPDevice::setDataNode(const QString &service, const QByteArray &nodeData, const QString &reqid,
                           std::function<void(bool success, QString errstr)> cb,
                           std::function<void(int total, int current)> cbProgress)
{
    if (service.isEmpty())
    {
        qWarning() << "context is empty.";
        cb(false, "context is empty");
        return;
    }

    QString logInf = QStringLiteral("Set data node for service: %1 reqid: %2")
                     .arg(service)
                     .arg(reqid);

    AsyncJobs *jobs;
    if (reqid.isEmpty())
        jobs = new AsyncJobs(logInf, this);
    else
        jobs = new AsyncJobs(logInf, reqid, this);

    QByteArray sdata = service.toUtf8();
    sdata.append((char)0);

    jobs->append(new MPCommandJob(this, MP_SET_DATA_SERVICE,
                                  sdata,
                                  [=](const QByteArray &data, bool &) -> bool
    {
        if (data[2] != 1)
        {
            qWarning() << "context " << service << " does not exist";
            //Context does not exists, create it
            createJobAddContext(service, jobs, true);
        }
        else
            qDebug() << "set_data_context " << service;
        return true;
    }));

    using namespace std::placeholders;

    //set size of data
    currentDataNode = QByteArray();
    currentDataNode.resize(4);
    qToBigEndian(nodeData.size(), (quint8 *)currentDataNode.data());
    currentDataNode.append(nodeData);

    //first packet
    QByteArray firstPacket;
    char eod = (nodeData.size() <= MOOLTIPASS_BLOCK_SIZE)?1:0;
    firstPacket.append(eod);
    firstPacket.append(currentDataNode.mid(0, MOOLTIPASS_BLOCK_SIZE));
    firstPacket.resize(MOOLTIPASS_BLOCK_SIZE + 1);

    cbProgress(currentDataNode.size() - 4, MOOLTIPASS_BLOCK_SIZE);

    //send the first 32bytes packet
    //bind to a member function of MPDevice, to be able to loop over until with got all the data
    jobs->append(new MPCommandJob(this, MP_WRITE_32B_IN_DN,
                                  firstPacket,
                                  std::bind(&MPDevice::setDataNodeCb, this, jobs, MOOLTIPASS_BLOCK_SIZE,
                                            std::move(cbProgress), _1, _2)));

    connect(jobs, &AsyncJobs::finished, [=](const QByteArray &)
    {
        //all jobs finished success
        qInfo() << "set_data_node success";
        cb(true, QString());
    });

    connect(jobs, &AsyncJobs::failed, [=](AsyncJob *failedJob)
    {
        qCritical() << "Failed writing data node";
        cb(false, failedJob->getErrorStr());
    });

    jobsQueue.enqueue(jobs);
    runAndDequeueJobs();
}
