/**
 * This file is part of TelepathyQt
 *
 * @copyright Copyright (C) 2012 Collabora Ltd. <http://www.collabora.co.uk/>
 * @copyright Copyright (C) 2012 Nokia Corporation
 * @license LGPL 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <TelepathyQt/BaseConnection>
#include "TelepathyQt/base-connection-internal.h"

#include "TelepathyQt/_gen/base-connection.moc.hpp"
#include "TelepathyQt/_gen/base-connection-internal.moc.hpp"

#include "TelepathyQt/debug-internal.h"

#include <TelepathyQt/BaseChannel>
#include <TelepathyQt/ChannelInterface>
#include <TelepathyQt/Constants>
#include <TelepathyQt/DBusObject>
#include <TelepathyQt/Utils>
#include <TelepathyQt/AbstractProtocolInterface>
#include <QString>
#include <QVariantMap>

namespace Tp
{

struct TP_QT_NO_EXPORT BaseConnection::Private {
    Private(BaseConnection *parent, const QDBusConnection &dbusConnection,
            const QString &cmName, const QString &protocolName,
            const QVariantMap &parameters)
        : parent(parent),
          cmName(cmName),
          protocolName(protocolName),
          parameters(parameters),
          status(Tp::ConnectionStatusDisconnected),
          selfHandle(0),
          adaptee(new BaseConnection::Adaptee(dbusConnection, parent)) {
    }

    BaseConnection *parent;
    QString cmName;
    QString protocolName;
    QVariantMap parameters;
    uint status;
    QHash<QString, AbstractConnectionInterfacePtr> interfaces;
    QSet<BaseChannelPtr> channels;
    CreateChannelCallback createChannelCB;
    RequestHandlesCallback requestHandlesCB;
    ConnectCallback connectCB;
    InspectHandlesCallback inspectHandlesCB;
    uint selfHandle;
    BaseConnection::Adaptee *adaptee;
};

BaseConnection::Adaptee::Adaptee(const QDBusConnection &dbusConnection,
                                 BaseConnection *connection)
    : QObject(connection),
      mConnection(connection)
{
    mAdaptor = new Service::ConnectionAdaptor(dbusConnection, this, connection->dbusObject());
}

BaseConnection::Adaptee::~Adaptee()
{
}

void BaseConnection::Adaptee::disconnect(const Tp::Service::ConnectionAdaptor::DisconnectContextPtr &context)
{
    debug() << "BaseConnection::Adaptee::disconnect";
    /* This will remove the connection from the connection manager
     * and destroy this object. */
    emit mConnection->disconnected();
    context->setFinished();
}

void BaseConnection::Adaptee::getSelfHandle(const Tp::Service::ConnectionAdaptor::GetSelfHandleContextPtr &context)
{
    context->setFinished(mConnection->mPriv->selfHandle);
}

uint BaseConnection::Adaptee::selfHandle() const
{
    return mConnection->mPriv->selfHandle;
}

void BaseConnection::Adaptee::getStatus(const Tp::Service::ConnectionAdaptor::GetStatusContextPtr &context)
{
    context->setFinished(mConnection->status());
}

void BaseConnection::Adaptee::connect(const Tp::Service::ConnectionAdaptor::ConnectContextPtr &context)
{
    if (!mConnection->mPriv->connectCB.isValid()) {
        context->setFinishedWithError(TP_QT_ERROR_NOT_IMPLEMENTED, QLatin1String("Not implemented"));
        return;
    }
    DBusError error;
    mConnection->mPriv->connectCB(&error);
    if (error.isValid()) {
        context->setFinishedWithError(error.name(), error.message());
        return;
    }
    context->setFinished();
}

void BaseConnection::Adaptee::inspectHandles(uint handleType,
                                             const Tp::UIntList &handles,
                                             const Tp::Service::ConnectionAdaptor::InspectHandlesContextPtr &context)
{
    if (!mConnection->mPriv->inspectHandlesCB.isValid()) {
        context->setFinishedWithError(TP_QT_ERROR_NOT_IMPLEMENTED, QLatin1String("Not implemented"));
        return;
    }
    DBusError error;
    QStringList ret = mConnection->mPriv->inspectHandlesCB(handleType, handles, &error);
    if (error.isValid()) {
        context->setFinishedWithError(error.name(), error.message());
        return;
    }
    context->setFinished(ret);
}
QStringList BaseConnection::Adaptee::interfaces() const
{
    QStringList ret;
    foreach(const AbstractConnectionInterfacePtr & iface, mConnection->interfaces()) {
        ret << iface->interfaceName();
    }
    return ret;
}

void BaseConnection::Adaptee::requestChannel(const QString &type, uint handleType, uint handle, bool suppressHandler,
        const Tp::Service::ConnectionAdaptor::RequestChannelContextPtr &context)
{
    debug() << "BaseConnection::Adaptee::requestChannel (deprecated)";
    DBusError error;
    bool yours;
    BaseChannelPtr channel = mConnection->ensureChannel(type,
                             handleType,
                             handle,
                             yours,
                             selfHandle(),
                             suppressHandler,
                             &error);
    if (error.isValid() || !channel) {
        context->setFinishedWithError(error.name(), error.message());
        return;
    }
    context->setFinished(QDBusObjectPath(channel->objectPath()));
}

void BaseConnection::Adaptee::requestHandles(uint handleType, const QStringList &identifiers,
        const Tp::Service::ConnectionAdaptor::RequestHandlesContextPtr &context)
{
    DBusError error;
    Tp::UIntList handles = mConnection->requestHandles(handleType, identifiers, &error);
    if (error.isValid()) {
        context->setFinishedWithError(error.name(), error.message());
        return;
    }
    context->setFinished(handles);
}

/**
 * \class BaseConnection
 * \ingroup serviceconn
 * \headerfile TelepathyQt/base-connection.h <TelepathyQt/BaseConnection>
 *
 * \brief Base class for Connection implementations.
 */

/**
 * Construct a BaseConnection.
 *
 * \param dbusConnection The D-Bus connection that will be used by this object.
 * \param cmName The name of the connection manager associated with this connection.
 * \param protocolName The name of the protocol associated with this connection.
 * \param parameters The parameters of this connection.
 */
BaseConnection::BaseConnection(const QDBusConnection &dbusConnection,
                               const QString &cmName, const QString &protocolName,
                               const QVariantMap &parameters)
    : DBusService(dbusConnection),
      mPriv(new Private(this, dbusConnection, cmName, protocolName, parameters))
{
}

/**
 * Class destructor.
 */
BaseConnection::~BaseConnection()
{
    delete mPriv;
}

/**
 * Return the name of the connection manager associated with this connection.
 *
 * \return The name of the connection manager associated with this connection.
 */
QString BaseConnection::cmName() const
{
    return mPriv->cmName;
}

/**
 * Return the name of the protocol associated with this connection.
 *
 * \return The name of the protocol associated with this connection.
 */
QString BaseConnection::protocolName() const
{
    return mPriv->protocolName;
}

/**
 * Return the parameters of this connection.
 *
 * \return The parameters of this connection.
 */
QVariantMap BaseConnection::parameters() const
{
    return mPriv->parameters;
}

/**
 * Return the immutable properties of this connection object.
 *
 * Immutable properties cannot change after the object has been registered
 * on the bus with registerObject().
 *
 * \return The immutable properties of this connection object.
 */
QVariantMap BaseConnection::immutableProperties() const
{
    // FIXME
    return QVariantMap();
}

/**
 * Return a unique name for this connection.
 *
 * \return A unique name for this connection.
 */
QString BaseConnection::uniqueName() const
{
    return QString(QLatin1String("_%1")).arg((quintptr) this, 0, 16);
}

uint BaseConnection::status() const
{
    debug() << "BaseConnection::status = " << mPriv->status << " " << this;
    return mPriv->status;
}

void BaseConnection::setStatus(uint newStatus, uint reason)
{
    debug() << "BaseConnection::setStatus " << newStatus << " " << reason << " " << this;
    bool changed = (newStatus != mPriv->status);
    mPriv->status = newStatus;
    if (changed)
        emit mPriv->adaptee->statusChanged(newStatus, reason);
}

void BaseConnection::setCreateChannelCallback(const CreateChannelCallback &cb)
{
    mPriv->createChannelCB = cb;
}

Tp::BaseChannelPtr BaseConnection::createChannel(const QString &channelType,
        uint targetHandleType,
        uint targetHandle,
        uint initiatorHandle,
        bool suppressHandler,
        DBusError *error)
{
    if (!mPriv->createChannelCB.isValid()) {
        error->set(TP_QT_ERROR_NOT_IMPLEMENTED, QLatin1String("Not implemented"));
        return BaseChannelPtr();
    }
    if (!mPriv->inspectHandlesCB.isValid()) {
        error->set(TP_QT_ERROR_NOT_IMPLEMENTED, QLatin1String("Not implemented"));
        return BaseChannelPtr();
    }

    BaseChannelPtr channel = mPriv->createChannelCB(channelType, targetHandleType, targetHandle, error);
    if (error->isValid())
        return BaseChannelPtr();

    QString targetID;
    if (targetHandle != 0) {
        QStringList list = mPriv->inspectHandlesCB(targetHandleType,  UIntList() << targetHandle, error);
        if (error->isValid()) {
            debug() << "BaseConnection::createChannel: could not resolve handle " << targetHandle;
            return BaseChannelPtr();
        } else {
            debug() << "BaseConnection::createChannel: found targetID " << *list.begin();
            targetID = *list.begin();
        }
    }
    QString initiatorID;
    if (initiatorHandle != 0) {
        QStringList list = mPriv->inspectHandlesCB(HandleTypeContact, UIntList() << initiatorHandle, error);
        if (error->isValid()) {
            debug() << "BaseConnection::createChannel: could not resolve handle " << initiatorHandle;
            return BaseChannelPtr();
        } else {
            debug() << "BaseConnection::createChannel: found initiatorID " << *list.begin();
            initiatorID = *list.begin();
        }
    }
    channel->setInitiatorHandle(initiatorHandle);
    channel->setInitiatorID(initiatorID);
    channel->setTargetID(targetID);
    channel->setRequested(initiatorHandle == mPriv->selfHandle);

    channel->registerObject(error);
    if (error->isValid())
        return BaseChannelPtr();

    mPriv->channels.insert(channel);

    BaseConnectionRequestsInterfacePtr reqIface =
        BaseConnectionRequestsInterfacePtr::dynamicCast(interface(TP_QT_IFACE_CONNECTION_INTERFACE_REQUESTS));

    if (!reqIface.isNull())
        //emit after return
        QMetaObject::invokeMethod(reqIface.data(), "newChannels",
                                  Qt::QueuedConnection,
                                  Q_ARG(Tp::ChannelDetailsList, ChannelDetailsList() << channel->details()));


    //emit after return
    QMetaObject::invokeMethod(mPriv->adaptee, "newChannel",
                              Qt::QueuedConnection,
                              Q_ARG(QDBusObjectPath, QDBusObjectPath(channel->objectPath())),
                              Q_ARG(QString, channel->channelType()),
                              Q_ARG(uint, channel->targetHandleType()),
                              Q_ARG(uint, channel->targetHandle()),
                              Q_ARG(bool, suppressHandler));

    QObject::connect(channel.data(),
                     SIGNAL(closed()),
                     SLOT(removeChannel()));

    return channel;
}

void BaseConnection::setRequestHandlesCallback(const RequestHandlesCallback &cb)
{
    mPriv->requestHandlesCB = cb;
}

UIntList BaseConnection::requestHandles(uint handleType, const QStringList &identifiers, DBusError* error)
{
    if (!mPriv->requestHandlesCB.isValid()) {
        error->set(TP_QT_ERROR_NOT_IMPLEMENTED, QLatin1String("Not implemented"));
        return UIntList();
    }
    return mPriv->requestHandlesCB(handleType, identifiers, error);
}

Tp::ChannelInfoList BaseConnection::channelsInfo()
{
    qDebug() << "BaseConnection::channelsInfo:";
    Tp::ChannelInfoList list;
    foreach(const BaseChannelPtr & c, mPriv->channels) {
        Tp::ChannelInfo info;
        info.channel = QDBusObjectPath(c->objectPath());
        info.channelType = c->channelType();
        info.handle = c->targetHandle();
        info.handleType = c->targetHandleType();
        qDebug() << "BaseConnection::channelsInfo " << info.channel.path();
        list << info;
    }
    return list;
}

Tp::ChannelDetailsList BaseConnection::channelsDetails()
{
    Tp::ChannelDetailsList list;
    foreach(const BaseChannelPtr & c, mPriv->channels)
    list << c->details();
    return list;
}

BaseChannelPtr BaseConnection::ensureChannel(const QString &channelType, uint targetHandleType,
        uint targetHandle, bool &yours, uint initiatorHandle,
        bool suppressHandler,
        DBusError* error)
{
    foreach(BaseChannelPtr channel, mPriv->channels) {
        if (channel->channelType() == channelType
                && channel->targetHandleType() == targetHandleType
                && channel->targetHandle() == targetHandle) {
            yours = false;
            return channel;
        }
    }
    yours = true;
    return createChannel(channelType, targetHandleType, targetHandle, initiatorHandle, suppressHandler, error);
}

void BaseConnection::removeChannel()
{
    BaseChannelPtr channel = BaseChannelPtr(
                                 qobject_cast<BaseChannel*>(sender()));
    Q_ASSERT(channel);
    Q_ASSERT(mPriv->channels.contains(channel));
    mPriv->channels.remove(channel);
}

/**
 * Return a list of interfaces that have been plugged into this Protocol
 * D-Bus object with plugInterface().
 *
 * This property is immutable and cannot change after this Protocol
 * object has been registered on the bus with registerObject().
 *
 * \return A list containing all the Protocol interface implementation objects.
 * \sa plugInterface(), interface()
 */
QList<AbstractConnectionInterfacePtr> BaseConnection::interfaces() const
{
    return mPriv->interfaces.values();
}

/**
 * Return a pointer to the interface with the given name.
 *
 * \param interfaceName The D-Bus name of the interface,
 * ex. TP_QT_IFACE_CONNECTION_INTERFACE_ADDRESSING.
 * \return A pointer to the AbstractConnectionInterface object that implements
 * the D-Bus interface with the given name, or a null pointer if such an interface
 * has not been plugged into this object.
 * \sa plugInterface(), interfaces()
 */
AbstractConnectionInterfacePtr BaseConnection::interface(const QString &interfaceName) const
{
    return mPriv->interfaces.value(interfaceName);
}

/**
 * Plug a new interface into this Connection D-Bus object.
 *
 * This property is immutable and cannot change after this Protocol
 * object has been registered on the bus with registerObject().
 *
 * \param interface An AbstractConnectionInterface instance that implements
 * the interface that is to be plugged.
 * \return \c true on success or \c false otherwise
 * \sa interfaces(), interface()
 */
bool BaseConnection::plugInterface(const AbstractConnectionInterfacePtr &interface)
{
    if (isRegistered()) {
        warning() << "Unable to plug protocol interface " << interface->interfaceName() <<
                  "- protocol already registered";
        return false;
    }

    if (interface->isRegistered()) {
        warning() << "Unable to plug protocol interface" << interface->interfaceName() <<
                  "- interface already registered";
        return false;
    }

    if (mPriv->interfaces.contains(interface->interfaceName())) {
        warning() << "Unable to plug protocol interface" << interface->interfaceName() <<
                  "- another interface with same name already plugged";
        return false;
    }

    debug() << "Interface" << interface->interfaceName() << "plugged";
    mPriv->interfaces.insert(interface->interfaceName(), interface);
    return true;
}

/**
 * Register this connection object on the bus.
 *
 * If \a error is passed, any D-Bus error that may occur will
 * be stored there.
 *
 * \param error A pointer to an empty DBusError where any
 * possible D-Bus error will be stored.
 * \return \c true on success and \c false if there was an error
 * or this connection object is already registered.
 * \sa isRegistered()
 */
bool BaseConnection::registerObject(DBusError *error)
{
    if (isRegistered()) {
        return true;
    }

    if (!checkValidProtocolName(mPriv->protocolName)) {
        if (error) {
            error->set(TP_QT_ERROR_INVALID_ARGUMENT,
                       mPriv->protocolName + QLatin1String("is not a valid protocol name"));
        }
        debug() << "Unable to register connection - invalid protocol name";
        return false;
    }

    QString escapedProtocolName = mPriv->protocolName;
    escapedProtocolName.replace(QLatin1Char('-'), QLatin1Char('_'));
    QString name = uniqueName();
    debug() << "cmName: " << mPriv->cmName << " escapedProtocolName: " << escapedProtocolName << " name:" << name;
    QString busName = QString(QLatin1String("%1%2.%3.%4"))
                      .arg(TP_QT_CONNECTION_BUS_NAME_BASE, mPriv->cmName, escapedProtocolName, name);
    QString objectPath = QString(QLatin1String("%1%2/%3/%4"))
                         .arg(TP_QT_CONNECTION_OBJECT_PATH_BASE, mPriv->cmName, escapedProtocolName, name);
    debug() << "busName: " << busName << " objectName: " << objectPath;
    DBusError _error;

    debug() << "Connection: registering interfaces  at " << dbusObject();
    foreach(const AbstractConnectionInterfacePtr & iface, mPriv->interfaces) {
        if (!iface->registerInterface(dbusObject())) {
            // lets not fail if an optional interface fails registering, lets warn only
            warning() << "Unable to register interface" << iface->interfaceName();
        }
    }

    bool ret = registerObject(busName, objectPath, &_error);
    if (!ret && error) {
        error->set(_error.name(), _error.message());
    }
    return ret;
}

/**
 * Reimplemented from DBusService.
 */
bool BaseConnection::registerObject(const QString &busName,
                                    const QString &objectPath, DBusError *error)
{
    return DBusService::registerObject(busName, objectPath, error);
}

void BaseConnection::setSelfHandle(uint selfHandle)
{
    mPriv->selfHandle = selfHandle;
}

uint BaseConnection::selfHandle() const
{
    return mPriv->selfHandle;
}

void BaseConnection::setConnectCallback(const ConnectCallback &cb)
{
    mPriv->connectCB = cb;
}

void BaseConnection::setInspectHandlesCallback(const InspectHandlesCallback &cb)
{
    mPriv->inspectHandlesCB = cb;
}

/**
 * \fn void BaseConnection::disconnected()
 *
 * Emitted when this connection has been disconnected.
 */

/**
 * \class AbstractConnectionInterface
 * \ingroup servicecm
 * \headerfile TelepathyQt/base-connection.h <TelepathyQt/BaseConnection>
 *
 * \brief Base class for all the Connection object interface implementations.
 */

AbstractConnectionInterface::AbstractConnectionInterface(const QString &interfaceName)
    : AbstractDBusServiceInterface(interfaceName)
{
}

AbstractConnectionInterface::~AbstractConnectionInterface()
{
}

}
