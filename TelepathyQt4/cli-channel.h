/*
 * This file is part of TelepathyQt4
 *
 * Copyright (C) 2008 Collabora Ltd. <http://www.collabora.co.uk/>
 * Copyright (C) 2008 Nokia Corporation
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

#ifndef _TelepathyQt4_Client_Channel_HEADER_GUARD_
#define _TelepathyQt4_Client_Channel_HEADER_GUARD_

/**
 * \addtogroup clientsideproxies Client-side proxies
 *
 * Proxy objects representing remote service objects accessed via D-Bus.
 *
 * In addition to providing direct access to methods, signals and properties
 * exported by the remote objects, some of these proxies offer features like
 * automatic inspection of remote object capabilities, property tracking,
 * backwards compatibility helpers for older services and other utilities.
 */

/**
 * \defgroup clientchannel Channel proxies
 * \ingroup clientsideproxies
 *
 * Proxy objects representing remote Telepathy Channels and their optional
 * interfaces.
 */

#include <TelepathyQt4/_gen/cli-channel.h>

namespace Telepathy
{
namespace Client
{

/**
 * \class Channel
 * \ingroup clientchannel
 *
 * High-level proxy object for accessing remote %Telepathy %Channel objects.
 */
class Channel : public ChannelInterface
{
    Q_OBJECT
    Q_ENUMS(Readiness)

public:
    /**
     * Describes readiness of the Channel for usage. The readiness depends on
     * the state of the remote object. In suitable states, an asynchronous
     * introspection process is started, and the Channel becomes more ready when
     * that process is completed.
     */
    enum Readiness {
        /**
         * The object has just been created and introspection is still in
         * progress. No functionality is available.
         *
         * The readiness can change to any other state depending on the result
         * of the initial state query to the remote object.
         */
        ReadinessJustCreated,

        /**
         * The remote object is alive and all introspection has been completed.
         * Most functionality is available.
         *
         * The readiness can change to ReadinessDead.
         */
        ReadinessFull,

        /**
         * The remote object has gone into a state where it can no longer be
         * used. No functionality is available.
         *
         * No further readiness changes are possible.
         */
        ReadinessDead
    };

    /**
     * Creates a Channel associated with the given object on the session bus.
     *
     * \param serviceName Name of the service the object is on.
     * \param objectPath  Path to the object on the service.
     * \param parent      Passed to the parent class constructor.
     */
    Channel(const QString& serviceName,
            const QString& objectPath,
            QObject* parent = 0);

    /**
     * Creates a Channel associated with the given object on the given bus.
     *
     * \param connection  The bus via which the object can be reached.
     * \param serviceName Name of the service the object is on.
     * \param objectPath  Path to the object on the service.
     * \param parent      Passed to the parent class constructor.
     */
    Channel(const QDBusConnection& connection,
            const QString &serviceName,
            const QString &objectPath,
            QObject* parent = 0);

    /**
     * Class destructor.
     */
    ~Channel();

    /**
     * Returns the current readiness of the Channel.
     *
     * \return The readiness, as defined in #Readiness.
     */
    Readiness readiness() const;

    /**
     * Returns a list of optional interfaces implemented by the remote object.
     * The contents of the list are undefined unless the Channel has readiness
     * #ReadinessFull. The returned value stays constant for the entire lifetime
     * of the Channel after reaching full readiness.
     *
     * \return D-Bus names of the supported interfaces.
     */
    QStringList interfaces() const;

Q_SIGNALS:
    /**
     * Emitted whenever the readiness of the Channel changes.
     *
     * \param newReadiness The new readiness, as defined in #Readiness.
     */
    void readinessChanged(Telepathy::Client::Channel::Readiness newReadiness);

private Q_SLOTS:
    void gotMainProperties(QDBusPendingCallWatcher* watcher);
    void gotChannelType(QDBusPendingCallWatcher* watcher);
    void gotHandle(QDBusPendingCallWatcher* watcher);
    void gotInterfaces(QDBusPendingCallWatcher* watcher);

private:
    struct Private;
    friend struct Private;
    Private *mPriv;
};

}
}

#endif
