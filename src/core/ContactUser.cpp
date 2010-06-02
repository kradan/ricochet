/* TorIM - http://gitorious.org/torim
 * Copyright (C) 2010, John Brooks <special@dereferenced.net>
 *
 * TorIM is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with TorIM. If not, see http://www.gnu.org/licenses/
 */

#include "main.h"
#include "ContactUser.h"
#include "ContactsManager.h"
#include "ui/ChatWidget.h"
#include "utils/DateUtil.h"
#include "utils/SecureRNG.h"
#include "protocol/GetSecretCommand.h"
#include "core/ContactIDValidator.h"
#include "core/OutgoingContactRequest.h"
#include <QPixmapCache>
#include <QtDebug>
#include <QBuffer>
#include <QDateTime>

ContactUser::ContactUser(int id, QObject *parent)
    : QObject(parent), uniqueID(id)
{
    Q_ASSERT(uniqueID >= 0);

    loadSettings();

    /* Connection */
    QString host = readSetting("hostname").toString();
    quint16 port = (quint16)readSetting("port", 80).toUInt();
    pConn = new ProtocolManager(this, host, port);

    QByteArray remoteSecret = readSetting("remoteSecret").toByteArray();
    if (!remoteSecret.isNull())
        pConn->setSecret(remoteSecret);

    connect(pConn, SIGNAL(primaryConnected()), this, SLOT(onConnected()));
    connect(pConn, SIGNAL(primaryDisconnected()), this, SLOT(onDisconnected()));

    /* Outgoing request */
    if (isContactRequest())
    {
        OutgoingContactRequest *request = OutgoingContactRequest::requestForUser(this);
        Q_ASSERT(request);
        connect(request, SIGNAL(statusChanged(int,int)), this, SLOT(updateStatusLine()));
    }
}

void ContactUser::loadSettings()
{
    config->beginGroup(QLatin1String("contacts/") + QString::number(uniqueID));

    pNickname = config->value("nickname", uniqueID).toString();

    config->endGroup();
}

QVariant ContactUser::readSetting(const QString &key, const QVariant &defaultValue) const
{
    return config->value(QString::fromLatin1("contacts/%1/%2").arg(uniqueID).arg(key), defaultValue);
}

void ContactUser::writeSetting(const QString &key, const QVariant &value)
{
    config->setValue(QString::fromLatin1("contacts/%1/%2").arg(uniqueID).arg(key), value);
}

void ContactUser::removeSetting(const QString &key)
{
    config->remove(QString::fromLatin1("contacts/%1/%2").arg(uniqueID).arg(key));
}

ContactUser *ContactUser::addNewContact(int id)
{
    ContactUser *user = new ContactUser(id);
    user->writeSetting("whenCreated", QDateTime::currentDateTime());

    /* Generate the local secret and set it */
    user->writeSetting("localSecret", SecureRNG::random(16));

    return user;
}

QString ContactUser::statusLine() const
{
    if (isConnected())
    {
        ChatWidget *chat = ChatWidget::widgetForUser(const_cast<ContactUser*>(this), false);
        if (chat && chat->unreadMessages())
            return tr("%n new message(s)", 0, chat->unreadMessages());
        return tr("Online");
    }
    else if (isContactRequest())
    {
        OutgoingContactRequest *request = OutgoingContactRequest::requestForUser(const_cast<ContactUser*>(this));
        switch (request->status())
        {
        case OutgoingContactRequest::Pending:
        case OutgoingContactRequest::Acknowledged:
        case OutgoingContactRequest::Accepted:
            return tr("Contact request pending");
        case OutgoingContactRequest::Error:
            return tr("Contact request error");
        case OutgoingContactRequest::Rejected:
            return tr("Contact request rejected");
        }
    }
    else
    {
        QDateTime lastConnected = readSetting("lastConnected").toDateTime();
        if (lastConnected.isNull())
            return tr("Never connected");
        return timeDifferenceString(lastConnected, QDateTime::currentDateTime());
    }

    return QString();
}

void ContactUser::updateStatusLine()
{
    emit statusLineChanged();
}

void ContactUser::onConnected()
{
    emit connected();

    writeSetting("lastConnected", QDateTime::currentDateTime());

    if (isContactRequest())
    {
        qDebug() << "Implicitly accepting outgoing contact request for" << uniqueID << "from primary connection";

        OutgoingContactRequest *request = OutgoingContactRequest::requestForUser(this);
        Q_ASSERT(request);
        request->accept();
        Q_ASSERT(!isContactRequest());
    }

    if (readSetting("remoteSecret").isNull())
    {
        qDebug() << "Requesting remote secret from user" << uniqueID;
        GetSecretCommand *command = new GetSecretCommand(this);
        command->send(conn());
    }
}

void ContactUser::onDisconnected()
{
    emit disconnected();

    writeSetting("lastConnected", QDateTime::currentDateTime());
}

void ContactUser::setNickname(const QString &nickname)
{
    if (pNickname == nickname)
        return;

    /* non-critical, just a safety net for UI checks */
    Q_ASSERT(!contactsManager->lookupNickname(nickname));

    pNickname = nickname;

    writeSetting("nickname", nickname);
    emit statusLineChanged();
}

QString ContactUser::hostname() const
{
    return readSetting("hostname").toString();
}

QString ContactUser::contactID() const
{
    return ContactIDValidator::idFromHostname(hostname());
}

void ContactUser::setHostname(const QString &hostname)
{
    QString fh = hostname;

    if (!hostname.endsWith(QLatin1String(".onion")))
        fh.append(QLatin1String(".onion"));

    writeSetting(QLatin1String("hostname"), fh);
    conn()->setHost(fh);
}

QPixmap ContactUser::avatar(AvatarSize size)
{
    QPixmap re;
    if (QPixmapCache::find(cachedAvatar[size], &re))
        return re;

    QString settingsKey = QString::fromLatin1("contacts/%1/avatar").arg(uniqueID);
    if (size == TinyAvatar)
        settingsKey.append(QLatin1String("-tiny"));

    re.loadFromData(config->value(settingsKey).toByteArray());

    cachedAvatar[size] = QPixmapCache::insert(re);
    return re;
}

void ContactUser::setAvatar(QImage image)
{
    if (image.width() > 160 || image.height() > 160)
        image = image.scaled(QSize(160, 160), Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QString key = QString::fromLatin1("contacts/%1/avatar").arg(uniqueID);

    if (!image.isNull())
    {
        QBuffer buffer;
        buffer.open(QBuffer::ReadWrite);
        if (image.save(&buffer, "jpeg", 100))
        {
            config->setValue(key, buffer.buffer());

            QImage tiny = image.scaled(QSize(35, 35), Qt::KeepAspectRatio, Qt::SmoothTransformation);
            buffer.close();
            buffer.open(QBuffer::ReadWrite);
            if (tiny.save(&buffer, "jpeg", 100))
                config->setValue(key + QLatin1String("-tiny"), buffer.buffer());
            else
                image = QImage();
        }
        else
            image = QImage();
    }

    if (image.isNull())
    {
        config->remove(key);
        config->remove(key + QLatin1String("-tiny"));
    }

    for (int i = 0; i < 2; ++i)
        QPixmapCache::remove(cachedAvatar[i]);
}

QString ContactUser::notesText() const
{
    return readSetting("notes").toString();
}

void ContactUser::setNotesText(const QString &text)
{
    QString key = QString::fromLatin1("contacts/%1/notes").arg(uniqueID);

    if (text.isEmpty())
        config->remove(key);
    else
        config->setValue(key, text);
}
