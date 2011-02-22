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

#include <TelepathyQt4/Account>

#include "TelepathyQt4/_gen/account.moc.hpp"
#include "TelepathyQt4/_gen/cli-account.moc.hpp"
#include "TelepathyQt4/_gen/cli-account-body.hpp"

#include "TelepathyQt4/debug-internal.h"

#include "TelepathyQt4/connection-internal.h"

#include <TelepathyQt4/AccountManager>
#include <TelepathyQt4/Channel>
#include <TelepathyQt4/ChannelDispatcherInterface>
#include <TelepathyQt4/ConnectionCapabilities>
#include <TelepathyQt4/ConnectionLowlevel>
#include <TelepathyQt4/ConnectionManager>
#include <TelepathyQt4/PendingChannelRequest>
#include <TelepathyQt4/PendingFailure>
#include <TelepathyQt4/PendingReady>
#include <TelepathyQt4/PendingStringList>
#include <TelepathyQt4/PendingVariant>
#include <TelepathyQt4/PendingVoid>
#include <TelepathyQt4/Profile>
#include <TelepathyQt4/ReferencedHandles>
#include <TelepathyQt4/Constants>
#include <TelepathyQt4/Debug>

#include <QQueue>
#include <QRegExp>
#include <QSharedPointer>
#include <QTimer>
#include <QWeakPointer>

#include <string.h>

namespace
{

struct PresenceStatusInfo
{
    QString name;
    Tp::SimpleStatusSpec spec;
};

Tp::ConnectionPresenceType presenceTypeForStatus(const QString &status, bool &maySetOnSelf)
{
    static PresenceStatusInfo statuses[] = {
        { QLatin1String("available"), { Tp::ConnectionPresenceTypeAvailable, true, true } },
        { QLatin1String("chat"), { Tp::ConnectionPresenceTypeAvailable, true, true } },
        { QLatin1String("chatty"), { Tp::ConnectionPresenceTypeAvailable, true, true } },
        { QLatin1String("away"), { Tp::ConnectionPresenceTypeAway, true, true } },
        { QLatin1String("brb"), { Tp::ConnectionPresenceTypeAway, true, true } },
        { QLatin1String("out-to-lunch"), { Tp::ConnectionPresenceTypeAway, true, true } },
        { QLatin1String("xa"), { Tp::ConnectionPresenceTypeExtendedAway, true, true } },
        { QLatin1String("hidden"), { Tp::ConnectionPresenceTypeHidden, true, true } },
        { QLatin1String("invisible"), { Tp::ConnectionPresenceTypeHidden, true, true } },
        { QLatin1String("offline"), { Tp::ConnectionPresenceTypeOffline, true, false } },
        { QLatin1String("unknown"), { Tp::ConnectionPresenceTypeUnknown, false, false } },
        { QLatin1String("error"), { Tp::ConnectionPresenceTypeError, false, false } }
    };

    for (uint i = 0; i < sizeof(statuses) / sizeof(PresenceStatusInfo); ++i) {
        if (status == statuses[i].name) {
            maySetOnSelf = statuses[i].spec.maySetOnSelf;
            return (Tp::ConnectionPresenceType) statuses[i].spec.type;
        }
    }

    // fallback to type away if we don't know status
    maySetOnSelf = true;
    return Tp::ConnectionPresenceTypeAway;
}

Tp::PresenceSpec presenceSpecForStatus(const QString &status, bool canHaveStatusMessage)
{
    Tp::SimpleStatusSpec spec;
    spec.type = presenceTypeForStatus(status, spec.maySetOnSelf);
    spec.canHaveMessage = canHaveStatusMessage;
    return Tp::PresenceSpec(status, spec);
}

}

namespace Tp
{

struct TELEPATHY_QT4_NO_EXPORT Account::Private
{
    Private(Account *parent, const ConnectionFactoryConstPtr &connFactory,
            const ChannelFactoryConstPtr &chanFactory,
            const ContactFactoryConstPtr &contactFactory);
    ~Private();

    void init();

    static void introspectMain(Private *self);
    static void introspectAvatar(Private *self);
    static void introspectProtocolInfo(Private *self);
    static void introspectCapabilities(Private *self);

    void updateProperties(const QVariantMap &props);
    void retrieveAvatar();
    bool processConnQueue();

    bool checkCapabilitiesChanged(bool profileChanged);

    void addConferenceRequestCommonParameters(
            const char *channelType,
            HandleType targetHandleType,
            const QList<ChannelPtr> &channels,
            QVariantMap &request);
    void addConferenceRequestParameters(
            const char *channelType,
            HandleType targetHandleType,
            const QList<ChannelPtr> &channels,
            const QStringList &initialInviteeContactsIdentifiers,
            QVariantMap &request);
    void addConferenceRequestParameters(
            const char *channelType,
            HandleType targetHandleType,
            const QList<ChannelPtr> &channels,
            const QList<ContactPtr> &initialInviteeContacts,
            QVariantMap &request);

    QString connectionObjectPath() const;

    // Public object
    Account *parent;

    // Factories
    ConnectionFactoryConstPtr connFactory;
    ChannelFactoryConstPtr chanFactory;
    ContactFactoryConstPtr contactFactory;

    // Instance of generated interface class
    Client::AccountInterface *baseInterface;

    // Mandatory properties interface proxy
    Client::DBus::PropertiesInterface *properties;

    ReadinessHelper *readinessHelper;

    // Introspection
    QVariantMap parameters;
    bool valid;
    bool enabled;
    bool connectsAutomatically;
    bool hasBeenOnline;
    bool changingPresence;
    QString cmName;
    QString protocolName;
    QString serviceName;
    ProfilePtr profile;
    QString displayName;
    QString nickname;
    QString iconName;
    QQueue<QString> connObjPathQueue;
    ConnectionPtr connection;
    bool mayFinishCore, coreFinished;
    QString normalizedName;
    Avatar avatar;
    ConnectionManagerPtr cm;
    ConnectionStatus connectionStatus;
    ConnectionStatusReason connectionStatusReason;
    QString connectionError;
    Connection::ErrorDetails connectionErrorDetails;
    Presence automaticPresence;
    Presence currentPresence;
    Presence requestedPresence;
    bool usingConnectionCaps;
    ConnectionCapabilities customCaps;

    // The contexts should never be removed from the map, to guarantee O(1) CD introspections per bus
    struct DispatcherContext;
    static QMap<QString, QSharedPointer<DispatcherContext> > dispatcherContexts;
    QSharedPointer<DispatcherContext> dispatcherContext;
};

struct Account::Private::DispatcherContext
{
    DispatcherContext(const QDBusConnection &bus)
        : iface(new Client::ChannelDispatcherInterface(bus, TP_QT4_CHANNEL_DISPATCHER_BUS_NAME, TP_QT4_CHANNEL_DISPATCHER_OBJECT_PATH)),
          introspected(false), supportsHints(false)
    {
    }

    ~DispatcherContext()
    {
        delete iface;
    }

    Client::ChannelDispatcherInterface *iface;

    bool introspected, supportsHints;
    QWeakPointer<PendingVariant> introspectOp;

private:
    DispatcherContext(const DispatcherContext &);
    void operator=(const DispatcherContext &);
};

Account::Private::Private(Account *parent, const ConnectionFactoryConstPtr &connFactory,
        const ChannelFactoryConstPtr &chanFactory, const ContactFactoryConstPtr &contactFactory)
    : parent(parent),
      connFactory(connFactory),
      chanFactory(chanFactory),
      contactFactory(contactFactory),
      baseInterface(new Client::AccountInterface(parent)),
      properties(parent->interface<Client::DBus::PropertiesInterface>()),
      readinessHelper(parent->readinessHelper()),
      valid(false),
      enabled(false),
      connectsAutomatically(false),
      hasBeenOnline(false),
      changingPresence(false),
      mayFinishCore(false),
      coreFinished(false),
      connectionStatus(ConnectionStatusDisconnected),
      connectionStatusReason(ConnectionStatusReasonNoneSpecified),
      usingConnectionCaps(false),
      dispatcherContext(dispatcherContexts.value(parent->dbusConnection().name()))
{
    // FIXME: QRegExp probably isn't the most efficient possible way to parse
    //        this :-)
    QRegExp rx(QLatin1String("^" TELEPATHY_ACCOUNT_OBJECT_PATH_BASE
                "/([_A-Za-z][_A-Za-z0-9]*)"  // cap(1) is the CM
                "/([_A-Za-z][_A-Za-z0-9]*)"  // cap(2) is the protocol
                "/([_A-Za-z][_A-Za-z0-9]*)"  // account-specific part
                ));

    if (rx.exactMatch(parent->objectPath())) {
        cmName = rx.cap(1);
        protocolName = rx.cap(2);
    } else {
        warning() << "Account object path is not spec-compliant, "
            "trying again with a different account-specific part check";

        rx = QRegExp(QLatin1String("^" TELEPATHY_ACCOUNT_OBJECT_PATH_BASE
                    "/([_A-Za-z][_A-Za-z0-9]*)"  // cap(1) is the CM
                    "/([_A-Za-z][_A-Za-z0-9]*)"  // cap(2) is the protocol
                    "/([_A-Za-z0-9]*)"  // account-specific part
                    ));
        if (rx.exactMatch(parent->objectPath())) {
            cmName = rx.cap(1);
            protocolName = rx.cap(2);
        } else {
            warning() << "Not a valid Account object path:" <<
                parent->objectPath();
        }
    }

    ReadinessHelper::Introspectables introspectables;

    // As Account does not have predefined statuses let's simulate one (0)
    ReadinessHelper::Introspectable introspectableCore(
        QSet<uint>() << 0,                                                      // makesSenseForStatuses
        Features(),                                                             // dependsOnFeatures
        QStringList(),                                                          // dependsOnInterfaces
        (ReadinessHelper::IntrospectFunc) &Private::introspectMain,
        this);
    introspectables[FeatureCore] = introspectableCore;

    ReadinessHelper::Introspectable introspectableAvatar(
        QSet<uint>() << 0,                                                            // makesSenseForStatuses
        Features() << FeatureCore,                                                    // dependsOnFeatures (core)
        QStringList() << QLatin1String(TELEPATHY_INTERFACE_ACCOUNT_INTERFACE_AVATAR), // dependsOnInterfaces
        (ReadinessHelper::IntrospectFunc) &Private::introspectAvatar,
        this);
    introspectables[FeatureAvatar] = introspectableAvatar;

    ReadinessHelper::Introspectable introspectableProtocolInfo(
        QSet<uint>() << 0,                                                      // makesSenseForStatuses
        Features() << FeatureCore,                                              // dependsOnFeatures (core)
        QStringList(),                                                          // dependsOnInterfaces
        (ReadinessHelper::IntrospectFunc) &Private::introspectProtocolInfo,
        this);
    introspectables[FeatureProtocolInfo] = introspectableProtocolInfo;

    ReadinessHelper::Introspectable introspectableCapabilities(
        QSet<uint>() << 0,                                                      // makesSenseForStatuses
        Features() << FeatureCore << FeatureProtocolInfo << FeatureProfile,     // dependsOnFeatures
        QStringList(),                                                          // dependsOnInterfaces
        (ReadinessHelper::IntrospectFunc) &Private::introspectCapabilities,
        this);
    introspectables[FeatureCapabilities] = introspectableCapabilities;

    readinessHelper->addIntrospectables(introspectables);

    if (connFactory->dbusConnection().name() != parent->dbusConnection().name()) {
        warning() << "  The D-Bus connection in the conn factory is not the proxy connection for"
            << parent->objectPath();
    }

    if (chanFactory->dbusConnection().name() != parent->dbusConnection().name()) {
        warning() << "  The D-Bus connection in the channel factory is not the proxy connection for"
            << parent->objectPath();
    }

    if (!dispatcherContext) {
        dispatcherContext = QSharedPointer<DispatcherContext>(new DispatcherContext(parent->dbusConnection()));
        dispatcherContexts.insert(parent->dbusConnection().name(), dispatcherContext);
    }

    init();
}

Account::Private::~Private()
{
}

bool Account::Private::checkCapabilitiesChanged(bool profileChanged)
{
    /* when the capabilities changed:
     *
     * - We were using the connection caps and now we don't have connection or
     *   the connection we have is not connected (changed to CM caps)
     * - We were using the CM caps and now we have a connected connection
     *   (changed to new connection caps)
     */
    bool changed = false;

    if (usingConnectionCaps &&
        (parent->connection().isNull() ||
         connection->status() != ConnectionStatusConnected)) {
        usingConnectionCaps = false;
        changed = true;
    } else if (!usingConnectionCaps &&
        !parent->connection().isNull() &&
        connection->status() == ConnectionStatusConnected) {
        usingConnectionCaps = true;
        changed = true;
    } else if (!usingConnectionCaps && profileChanged) {
        changed = true;
    }

    if (changed && parent->isReady(FeatureCapabilities)) {
        emit parent->capabilitiesChanged(parent->capabilities());
    }

    return changed;
}

void Account::Private::addConferenceRequestCommonParameters(
        const char *channelType,
        HandleType targetHandleType,
        const QList<ChannelPtr> &channels,
        QVariantMap &request)
{
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(channelType));
    if (targetHandleType != HandleTypeNone) {
        request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType"),
                       (uint) targetHandleType);
    }

    ObjectPathList objectPaths;
    foreach (const ChannelPtr &channel, channels) {
        objectPaths << QDBusObjectPath(channel->objectPath());
    }

    request.insert(TP_QT4_IFACE_CHANNEL_INTERFACE_CONFERENCE + QLatin1String(".InitialChannels"),
            qVariantFromValue(objectPaths));
}

void Account::Private::addConferenceRequestParameters(
        const char *channelType,
        HandleType targetHandleType,
        const QList<ChannelPtr> &channels,
        const QStringList &initialInviteeContactsIdentifiers,
        QVariantMap &request)
{
    addConferenceRequestCommonParameters(channelType, targetHandleType,
            channels, request);

    if (!initialInviteeContactsIdentifiers.isEmpty()) {
        request.insert(TP_QT4_IFACE_CHANNEL_INTERFACE_CONFERENCE + QLatin1String(".InitialInviteeIDs"),
                initialInviteeContactsIdentifiers);
    }
}

void Account::Private::addConferenceRequestParameters(
        const char *channelType,
        HandleType targetHandleType,
        const QList<ChannelPtr> &channels,
        const QList<ContactPtr> &initialInviteeContacts,
        QVariantMap &request)
{
    addConferenceRequestCommonParameters(channelType, targetHandleType,
            channels, request);

    if (!initialInviteeContacts.isEmpty()) {
        UIntList handles;
        foreach (const ContactPtr &contact, initialInviteeContacts) {
            if (!contact) {
                continue;
            }
            handles << contact->handle()[0];
        }
        if (!handles.isEmpty()) {
            request.insert(TP_QT4_IFACE_CHANNEL_INTERFACE_CONFERENCE +
                        QLatin1String(".InitialInviteeHandles"),
                    qVariantFromValue(handles));
        }
    }
}

QString Account::Private::connectionObjectPath() const
{
    return !connection.isNull() ? connection->objectPath() : QString();
}

QMap<QString, QSharedPointer<Account::Private::DispatcherContext> > Account::Private::dispatcherContexts;

/**
 * \class Account
 * \ingroup clientaccount
 * \headerfile TelepathyQt4/account.h <TelepathyQt4/Account>
 *
 * \brief The Account class provides an object representing a Telepathy account.
 *
 * Account adds the following features compared to using
 * Client::AccountManagerInterface directly:
 * <ul>
 *  <li>Status tracking</li>
 *  <li>Getting the list of supported interfaces automatically</li>
 * </ul>
 *
 * The remote object accessor functions on this object (isValidAccount(),
 * isEnabled(), and so on) don't make any D-Bus calls; instead, they return/use
 * values cached from a previous introspection run. The introspection process
 * populates their values in the most efficient way possible based on what the
 * service implements. Their return value is mostly undefined until the
 * introspection process is completed, i.e. isReady() returns true. See the
 * individual accessor descriptions for more details.
 *
 * Signals are emitted to indicate that properties have changed, for example
 * displayNameChanged(), iconNameChanged(), etc.
 *
 * Convenience methods to create channels using the channel dispatcher such as
 * ensureTextChat(), createFileTransfer() are provided.
 *
 * To avoid unnecessary D-Bus traffic, some methods only return valid
 * information after a specific feature has been enabled by calling
 * becomeReady() with the desired set of features as an argument, and waiting
 * for the resulting PendingOperation to finish. For instance, to retrieve the
 * account protocol information, it is necessary to call becomeReady() with
 * Account::FeatureProtocolInfo included in the argument.
 * The required features are documented by each method.
 *
 * If the account is deleted from the AccountManager, this object
 * will not be deleted automatically; however, it will emit invalidated()
 * with error code #TELEPATHY_QT4_ERROR_OBJECT_REMOVED and will cease to
 * be useful.
 *
 * \section account_usage_sec Usage
 *
 * \subsection account_create_sec Creating an account object
 *
 * The easiest way to create account objects is through AccountManager. One can
 * just use the AccountManager convenience methods such as
 * AccountManager::validAccounts() to get a list of account objects representing
 * valid accounts.
 *
 * If you already know the object path, you can just call create().
 * For example:
 *
 * \code AccountPtr acc = Account::create(busName, objectPath); \endcode
 *
 * An AccountPtr object is returned, which will automatically keep
 * track of object lifetime.
 *
 * You can also provide a D-Bus connection as a QDBusConnection:
 *
 * \code
 *
 * AccountPtr acc = Account::create(QDBusConnection::sessionBus(),
 *         busName, objectPath);
 *
 * \endcode
 *
 * \subsection account_ready_sec Making account ready to use
 *
 * An Account object needs to become ready before usage, meaning that the
 * introspection process finished and the object accessors can be used.
 *
 * To make the object ready, use becomeReady() and wait for the
 * PendingOperation::finished() signal to be emitted.
 *
 * \code
 *
 * class MyClass : public QObject
 * {
 *     QOBJECT
 *
 * public:
 *     MyClass(QObject *parent = 0);
 *     ~MyClass() { }
 *
 * private Q_SLOTS:
 *     void onAccountReady(Tp::PendingOperation*);
 *
 * private:
 *     AccountPtr acc;
 * };
 *
 * MyClass::MyClass(const QString &busName, const QString &objectPath,
 *         QObject *parent)
 *     : QObject(parent)
 *       acc(Account::create(busName, objectPath))
 * {
 *     connect(acc->becomeReady(),
 *             SIGNAL(finished(Tp::PendingOperation*)),
 *             SLOT(onAccountReady(Tp::PendingOperation*)));
 * }
 *
 * void MyClass::onAccountReady(Tp::PendingOperation *op)
 * {
 *     if (op->isError()) {
 *         qWarning() << "Account cannot become ready:" <<
 *             op->errorName() << "-" << op->errorMessage();
 *         return;
 *     }
 *
 *     // Account is now ready
 *     qDebug() << "Display name:" << acc->displayName();
 * }
 *
 * \endcode
 *
 * See \ref async_model, \ref shared_ptr
 */

/**
 * Feature representing the core that needs to become ready to make the Account
 * object usable.
 *
 * Note that this feature must be enabled in order to use most Account methods.
 * See specific methods documentation for more details.
 *
 * When calling isReady(), becomeReady(), this feature is implicitly added
 * to the requested features.
 */
const Feature Account::FeatureCore = Feature(QLatin1String(Account::staticMetaObject.className()), 0, true);

/**
 * Feature used in order to access account avatar info.
 *
 * See avatar specific methods' documentation for more details.
 */
const Feature Account::FeatureAvatar = Feature(QLatin1String(Account::staticMetaObject.className()), 1);

/**
 * Feature used in order to access account protocol info.
 *
 * See protocol info specific methods' documentation for more details.
 */
const Feature Account::FeatureProtocolInfo = Feature(QLatin1String(Account::staticMetaObject.className()), 2);

/**
 * Feature used in order to access account capabilities.
 *
 * This feature will enable FeatureProtocolInfo and FeatureProfile.
 *
 * See capabilities specific methods' documentation for more details.
 */
const Feature Account::FeatureCapabilities = Feature(QLatin1String(Account::staticMetaObject.className()), 3);

/**
 * Feature used in order to access account profile info.
 *
 * See profile specific methods' documentation for more details.
 */
const Feature Account::FeatureProfile = FeatureProtocolInfo;
// FeatureProfile is the same as FeatureProtocolInfo for now, as it only needs
// the protocol info, cm name and protocol name to build a fake profile. Make it
// a full-featured feature if needed later.

/**
 * Create a new Account object using QDBusConnection::sessionBus() and the given factories.
 *
 * A warning is printed if the factories are not for QDBusConnection::sessionBus().
 *
 * \param busName The account well-known bus name (sometimes called a "service
 *                name"). This is usually the same as the account manager
 *                bus name #TELEPATHY_ACCOUNT_MANAGER_BUS_NAME.
 * \param objectPath The account object path.
 * \param connectionFactory The connection factory to use.
 * \param channelFactory The channel factory to use.
 * \param contactFactory The contact factory to use.
 * \return An AccountPtr object pointing to the newly created Account object.
 */
AccountPtr Account::create(const QString &busName, const QString &objectPath,
        const ConnectionFactoryConstPtr &connectionFactory,
        const ChannelFactoryConstPtr &channelFactory,
        const ContactFactoryConstPtr &contactFactory)
{
    return AccountPtr(new Account(QDBusConnection::sessionBus(), busName, objectPath,
                connectionFactory, channelFactory, contactFactory, Account::FeatureCore));
}

/**
 * Create a new Account object using the given \a bus and the given factories.
 *
 * A warning is printed if the factories are not for \a bus.
 *
 * \param bus QDBusConnection to use.
 * \param busName The account well-known bus name (sometimes called a "service
 *                name"). This is usually the same as the account manager
 *                bus name #TELEPATHY_ACCOUNT_MANAGER_BUS_NAME.
 * \param objectPath The account object path.
 * \param connectionFactory The connection factory to use.
 * \param channelFactory The channel factory to use.
 * \param contactFactory The contact factory to use.
 * \return An AccountPtr object pointing to the newly created Account object.
 */
AccountPtr Account::create(const QDBusConnection &bus,
        const QString &busName, const QString &objectPath,
        const ConnectionFactoryConstPtr &connectionFactory,
        const ChannelFactoryConstPtr &channelFactory,
        const ContactFactoryConstPtr &contactFactory)
{
    return AccountPtr(new Account(bus, busName, objectPath, connectionFactory, channelFactory,
                contactFactory, Account::FeatureCore));
}

/**
 * Construct a new Account object using the given \a bus and the given factories.
 *
 * A warning is printed if the factories are not for \a bus.
 *
 * \param bus QDBusConnection to use.
 * \param busName The account well-known bus name (sometimes called a "service
 *                name"). This is usually the same as the account manager
 *                bus name #TELEPATHY_ACCOUNT_MANAGER_BUS_NAME.
 * \param objectPath The account object path.
 * \param connectionFactory The connection factory to use.
 * \param channelFactory The channel factory to use.
 * \param contactFactory The contact factory to use.
 * \param coreFeature The core feature of the Account subclass. The corresponding introspectable
 * should depend on Account::FeatureCore.
 */
Account::Account(const QDBusConnection &bus,
        const QString &busName, const QString &objectPath,
        const ConnectionFactoryConstPtr &connectionFactory,
        const ChannelFactoryConstPtr &channelFactory,
        const ContactFactoryConstPtr &contactFactory,
        const Feature &coreFeature)
    : StatelessDBusProxy(bus, busName, objectPath, coreFeature),
      OptionalInterfaceFactory<Account>(this),
      mPriv(new Private(this, connectionFactory, channelFactory, contactFactory))
{
}

/**
 * Class destructor.
 */
Account::~Account()
{
    delete mPriv;
}

/**
 * Get the connection factory used by this account.
 *
 * Only read access is provided. This allows constructing object instances and examining the object
 * construction settings, but not changing settings. Allowing changes would lead to tricky
 * situations where objects constructed at different times by the account would have unpredictably
 * different construction settings (eg. subclass).
 *
 * \return Read-only pointer to the factory.
 */
ConnectionFactoryConstPtr Account::connectionFactory() const
{
    return mPriv->connFactory;
}

/**
 * Get the channel factory used by this account.
 *
 * Only read access is provided. This allows constructing object instances and examining the object
 * construction settings, but not changing settings. Allowing changes would lead to tricky
 * situations where objects constructed at different times by the account would have unpredictably
 * different construction settings (eg. subclass).
 *
 * \return Read-only pointer to the factory.
 */
ChannelFactoryConstPtr Account::channelFactory() const
{
    return mPriv->chanFactory;
}

/**
 * Get the contact factory used by this account.
 *
 * Only read access is provided. This allows constructing object instances and examining the object
 * construction settings, but not changing settings. Allowing changes would lead to tricky
 * situations where objects constructed at different times by the account would have unpredictably
 * different construction settings (eg. subclass).
 *
 * \return Read-only pointer to the factory.
 */
ContactFactoryConstPtr Account::contactFactory() const
{
    return mPriv->contactFactory;
}

/**
 * Return whether this is a valid account.
 *
 * If true, this account is considered by the account manager to be complete
 * and usable. If false, user action is required to make it usable, and it will
 * never attempt to connect (for instance, this might be caused by the absence
 * of a required parameter).
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return \c true if the account is valid, \c false otherwise.
 * \sa validityChanged()
 */
bool Account::isValidAccount() const
{
    return mPriv->valid;
}

/**
 * Return whether this account is enabled.
 *
 * Gives the users the possibility to prevent an account from
 * being used. This flag does not change the validity of the account.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return \c true if the account is enabled, \c false otherwise.
 * \sa stateChanged()
 */
bool Account::isEnabled() const
{
    return mPriv->enabled;
}

/**
 * Set whether this account should be enabled or disabled.
 *
 * \param value Whether this account should be enabled or disabled.
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when the call has finished.
 * \sa stateChanged()
 */
PendingOperation *Account::setEnabled(bool value)
{
    return new PendingVoid(
            mPriv->properties->Set(
                QLatin1String(TELEPATHY_INTERFACE_ACCOUNT),
                QLatin1String("Enabled"),
                QDBusVariant(value)),
            AccountPtr(this));
}

/**
 * Return the connection manager name of this account.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return The connection manager name of this account.
 */
QString Account::cmName() const
{
    return mPriv->cmName;
}

/**
 * Return the protocol name of this account.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return The protocol name of this account.
 */
QString Account::protocolName() const
{
    return mPriv->protocolName;
}

/**
 * Return the service name of this account.
 *
 * Note that this method will fallback to protocolName() if service name
 * is not known.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return The service name of this account.
 * \sa serviceNameChanged(), protocolName()
 */
QString Account::serviceName() const
{
    if (mPriv->serviceName.isEmpty()) {
        return mPriv->protocolName;
    }
    return mPriv->serviceName;
}

/**
 * Set the service name of this account.
 *
 * \param value The service name of this account.
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when the call has finished.
 * \sa serviceNameChanged()
 */
PendingOperation *Account::setServiceName(const QString &value)
{
    return new PendingVoid(
            mPriv->properties->Set(
                QLatin1String(TELEPATHY_INTERFACE_ACCOUNT),
                QLatin1String("Service"),
                QDBusVariant(value)),
            AccountPtr(this));
}

/**
 * Return the profile used for this account.
 *
 * Note that if a profile for serviceName() is not available, a fake profile
 * (Profile::isFake() will return \c true) will be returned in case protocolInfo() is valid.
 *
 * The fake profile will contain the following info:
 *  - Profile::type() will return "IM"
 *  - Profile::provider() will return an empty string
 *  - Profile::serviceName() will return cmName()-serviceName()
 *  - Profile::name() and Profile::protocolName() will return protocolName()
 *  - Profile::iconName() will return "im-protocolName()"
 *  - Profile::cmName() will return cmName()
 *  - Profile::parameters() will return a list matching CM default parameters for protocol with name
 *    protocolName()
 *  - Profile::presences() will return an empty list and
 *    Profile::allowOtherPresences() will return \c true, meaning that CM
 *    presences should be used
 *  - Profile::unsupportedChannelClassSpecs() will return an empty list
 *
 * This method requires Account::FeatureProfile to be enabled.
 *
 * \return The profile for this account.
 * \sa profileChanged()
 */
ProfilePtr Account::profile() const
{
    if (!isReady(FeatureProfile)) {
        return ProfilePtr();
    }

    if (!mPriv->profile) {
        mPriv->profile = Profile::createForServiceName(serviceName());
        if (!mPriv->profile->isValid()) {
            if (protocolInfo().isValid()) {
                mPriv->profile = ProfilePtr(new Profile(
                            QString(QLatin1String("%1-%2")).arg(mPriv->cmName).arg(serviceName()),
                            mPriv->cmName,
                            mPriv->protocolName,
                            protocolInfo()));
            } else {
                warning() << "Cannot create profile as neither a .profile is installed for service" <<
                    serviceName() << "nor protocol info can be retrieved";
            }
        }
    }
    return mPriv->profile;
}

/**
 * Return the display name of this account.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return The display name of this account.
 * \sa displayNameChanged()
 */
QString Account::displayName() const
{
    return mPriv->displayName;
}

/**
 * Set the display name of this account.
 *
 * \param value The display name of this account.
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when the call has finished.
 * \sa displayNameChanged()
 */
PendingOperation *Account::setDisplayName(const QString &value)
{
    return new PendingVoid(
            mPriv->properties->Set(
                QLatin1String(TELEPATHY_INTERFACE_ACCOUNT),
                QLatin1String("DisplayName"),
                QDBusVariant(value)),
            AccountPtr(this));
}

/**
 * Return the icon name of this account.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * If the account has no icon, and Account::FeatureProfile is enabled, the icon from the result of
 * profile() will be used.
 *
 * If neither the account nor the profile has an icon, and Account::FeatureProtocolInfo is
 * enabled, the icon from protocolInfo() will be used if set.
 *
 * As a last resort, "im-" + protocolName() will be returned.
 *
 * This matches the fallbacks recommended by the Telepathy specification.
 *
 * \return The icon name of this account.
 * \sa iconNameChanged()
 */
QString Account::iconName() const
{
    if (mPriv->iconName.isEmpty()) {
        if (isReady(FeatureProfile) && !profile().isNull()) {
            QString iconName = profile()->iconName();
            if (!iconName.isEmpty()) {
                return iconName;
            }
        }

        if (isReady(FeatureProtocolInfo) && protocolInfo().isValid()) {
            return protocolInfo().iconName();
        }

        return QString(QLatin1String("im-%1")).arg(protocolName());
    }

    return mPriv->iconName;
}

/**
 * Set the icon name of this account.
 *
 * \param value The icon name of this account.
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when the call has finished.
 * \sa iconNameChanged()
 */
PendingOperation *Account::setIconName(const QString &value)
{
    return new PendingVoid(
            mPriv->properties->Set(
                QLatin1String(TELEPATHY_INTERFACE_ACCOUNT),
                QLatin1String("Icon"),
                QDBusVariant(value)),
            AccountPtr(this));
}

/**
 * Return the nickname of this account.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return The nickname of this account.
 * \sa nicknameChanged()
 */
QString Account::nickname() const
{
    return mPriv->nickname;
}

/**
 * Set the nickname of this account.
 *
 * \param value The nickname of this account.
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when the call has finished.
 * \sa nicknameChanged()
 */
PendingOperation *Account::setNickname(const QString &value)
{
    return new PendingVoid(
            mPriv->properties->Set(
                QLatin1String(TELEPATHY_INTERFACE_ACCOUNT),
                QLatin1String("Nickname"),
                QDBusVariant(value)),
            AccountPtr(this));
}

/**
 * Return a AvatarSpec representing the avatar requirements (size limits, supported MIME types, etc)
 * for avatars passed to setAvatar().
 *
 * For now this method will only return the avatar requirements found in protocolInfo() if
 * FeatureProtocolInfo is ready.
 *
 * \return The avatar requirements for avatars passed to setAvatar().
 */
AvatarSpec Account::avatarRequirements() const
{
    // TODO Once connection has support for avatar requirements use it if the connection is usable
    ProtocolInfo pi = protocolInfo();
    if (pi.isValid()) {
        return pi.avatarRequirements();
    }
    return AvatarSpec();
}

/**
 * Return the avatar of this account.
 *
 * This method requires Account::FeatureAvatar to be enabled.
 *
 * \return The avatar of this account.
 * \sa avatarChanged()
 */
const Avatar &Account::avatar() const
{
    if (!isReady(Features() << FeatureAvatar)) {
        warning() << "Trying to retrieve avatar from account, but "
                     "avatar is not supported or was not requested. "
                     "Use becomeReady(FeatureAvatar)";
    }

    return mPriv->avatar;
}

/**
 * Set avatar of this account.
 *
 * \param avatar The avatar of this account.
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when the call has finished.
 * \sa avatarChanged()
 */
PendingOperation *Account::setAvatar(const Avatar &avatar)
{
    if (!interfaces().contains(QLatin1String(TELEPATHY_INTERFACE_ACCOUNT_INTERFACE_AVATAR))) {
        return new PendingFailure(
                QLatin1String(TELEPATHY_ERROR_NOT_IMPLEMENTED),
                QLatin1String("Account does not support Avatar"),
                AccountPtr(this));
    }

    return new PendingVoid(
            mPriv->properties->Set(
                QLatin1String(TELEPATHY_INTERFACE_ACCOUNT_INTERFACE_AVATAR),
                QLatin1String("Avatar"),
                QDBusVariant(QVariant::fromValue(avatar))),
            AccountPtr(this));
}

/**
 * Return the parameters of this account.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return The parameters of this account.
 * \sa parametersChanged()
 */
QVariantMap Account::parameters() const
{
    return mPriv->parameters;
}

/**
 * Update this account parameters.
 *
 * On success, the pending operation returned by this method will produce a
 * list of strings, which are the names of parameters whose changes will not
 * take effect until the account is disconnected and reconnected (for instance
 * by calling reconnect()).
 *
 * \param set Parameters to set.
 * \param unset Parameters to unset.
 * \return A PendingStringList which will emit PendingStringList::finished
 *         when the call has finished
 * \sa parametersChanged(), reconnect()
 */
PendingStringList *Account::updateParameters(const QVariantMap &set,
        const QStringList &unset)
{
    return new PendingStringList(
            baseInterface()->UpdateParameters(set, unset),
            AccountPtr(this));
}

/**
 * Return the protocol info of this account protocol.
 *
 * This method requires Account::FeatureProtocolInfo to be enabled.
 *
 * \return The protocol info of this account protocol.
 */
ProtocolInfo Account::protocolInfo() const
{
    if (!isReady(Features() << FeatureProtocolInfo)) {
        warning() << "Trying to retrieve protocol info from account, but "
                     "protocol info is not supported or was not requested. "
                     "Use becomeReady(FeatureProtocolInfo)";
        return ProtocolInfo();
    }

    return mPriv->cm->protocol(mPriv->protocolName);
}

/**
 * Return the capabilities for this account.
 *
 * This method requires Account::FeatureCapabilities to be enabled.
 *
 * Note that this method will return the connection() capabilities if the
 * account is online and ready. If the account is disconnected, it will fallback
 * to return the subtraction of the protocolInfo() capabilities and the profile unsupported
 * capabilities.
 *
 * \return The capabilities for this account.
 */
ConnectionCapabilities Account::capabilities() const
{
    if (!isReady(FeatureCapabilities)) {
        warning() << "Trying to retrieve capabilities from account, but "
                     "FeatureCapabilities was not requested. "
                     "Use becomeReady(FeatureCapabilities)";
        return ConnectionCapabilities();
    }

    // if the connection is online and ready use its caps
    if (mPriv->connection &&
        mPriv->connection->status() == ConnectionStatusConnected) {
        return mPriv->connection->capabilities();
    }

    // if we are here it means FeatureProtocolInfo and FeatureProfile are ready, as
    // FeatureCapabilities depend on them, so let's use the subtraction of protocol info caps rccs
    // and profile unsupported rccs.
    //
    // However, if we failed to introspect the CM (eg. this is a test), then let's not try to use
    // the protocolInfo because it'll be NULL! Profile may also be NULL in case a .profile for
    // serviceName() is not present and protocolInfo is NULL.
    ProtocolInfo pi = protocolInfo();
    if (!pi.isValid()) {
        return ConnectionCapabilities();
    }
    ProfilePtr pr = profile();
    if (!pr) {
        return pi.capabilities();
    }

    RequestableChannelClassSpecList piClassSpecs = pi.capabilities().allClassSpecs();
    RequestableChannelClassSpecList prUnsupportedClassSpecs = pr->unsupportedChannelClassSpecs();
    RequestableChannelClassSpecList classSpecs;
    bool unsupported;
    foreach (const RequestableChannelClassSpec &piClassSpec, piClassSpecs) {
        unsupported = false;
        foreach (const RequestableChannelClassSpec &prUnsuportedClassSpec, prUnsupportedClassSpecs) {
            // Here we check the following:
            // - If the unsupported spec has no allowed property it means it does not support any
            // class whose fixed properties match.
            //   E.g: Doesn't support any media calls, be it audio or video.
            // - If the unsupported spec has allowed properties it means it does not support a
            // specific class whose fixed properties and allowed properties should match.
            //   E.g: Doesn't support video calls but does support audio calls.
            if (prUnsuportedClassSpec.allowedProperties().isEmpty()) {
                if (piClassSpec.fixedProperties() == prUnsuportedClassSpec.fixedProperties()) {
                    unsupported = true;
                    break;
                }
            } else {
                if (piClassSpec == prUnsuportedClassSpec) {
                    unsupported = true;
                    break;
                }
            }
        }
        if (!unsupported) {
            classSpecs.append(piClassSpec);
        }
    }
    mPriv->customCaps = ConnectionCapabilities(classSpecs);
    return mPriv->customCaps;
}

/**
 * Return whether this account should be put online automatically whenever
 * possible.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return \c true if it should try to connect automatically, \c false
 *         otherwise.
 * \sa connectsAutomaticallyPropertyChanged()
 */
bool Account::connectsAutomatically() const
{
    return mPriv->connectsAutomatically;
}

/**
 * Set whether this account should be put online automatically whenever
 * possible.
 *
 * \param value Value indicating if this account should be put online whenever
 *              possible.
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when the call has finished.
 * \sa connectsAutomaticallyPropertyChanged()
 */
PendingOperation *Account::setConnectsAutomatically(bool value)
{
    return new PendingVoid(
            mPriv->properties->Set(
                QLatin1String(TELEPATHY_INTERFACE_ACCOUNT),
                QLatin1String("ConnectAutomatically"),
                QDBusVariant(value)),
            AccountPtr(this));
}

/**
 * Return whether this account has ever been put online successfully.
 *
 * This property cannot change from true to false, only from false to true.
 * When the account successfully goes online for the first time, or when it
 * is detected that this has already happened, the firstOnline() signal is
 * emitted.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return Whether the account has ever been online.
 */
bool Account::hasBeenOnline() const
{
    return mPriv->hasBeenOnline;
}

/**
 * Return the status of this account connection.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return The status of this account connection.
 * \sa connectionStatusChanged()
 */
ConnectionStatus Account::connectionStatus() const
{
    return mPriv->connectionStatus;
}

/**
 * Return the status reason of this account connection.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return The status reason of this account connection.
 * \sa connectionStatusChanged()
 */
ConnectionStatusReason Account::connectionStatusReason() const
{
    return mPriv->connectionStatusReason;
}

/**
 * Return the D-Bus error name for the last disconnection or connection failure,
 * (in particular, #TELEPATHY_ERROR_CANCELLED if it was disconnected by user
 * request), or an empty string if the account is connected.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return The D-Bus error name for the last disconnection or connection failure.
 * \sa connectionErrorDetails(), connectionStatus(), connectionStatusReason(),
 *     connectionStatusChanged()
 */
QString Account::connectionError() const
{
    return mPriv->connectionError;
}

/**
 * Return a map containing extensible error details related to
 * connectionError().
 *
 * The keys for this map are defined by
 * <a href="http://telepathy.freedesktop.org/spec/">the Telepathy D-Bus
 * Interface Specification</a>. They will typically include
 * <literal>debug-message</literal>, which is a debugging message in the C
 * locale.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return A map containing extensible error details related to
 *         connectionError().
 * \sa connectionError(), connectionStatus(), connectionStatusReason(), connectionStatusChanged(),
 *     Connection::ErrorDetails.
 */
Connection::ErrorDetails Account::connectionErrorDetails() const
{
    return mPriv->connectionErrorDetails;
}

/**
 * Return the ConnectionPtr object of this account.
 *
 * Note that the returned ConnectionPtr object will not be cached by the Account
 * instance; applications should do it themselves.
 *
 * Remember to call Connection::becomeReady on the new connection to
 * make sure it is ready before using it.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return A ConnectionPtr object pointing to the Connection object of this
 *         account, or a null ConnectionPtr if this account does not currently
 *         have a connection or if an error occurred.
 * \sa connectionChanged()
 */
ConnectionPtr Account::connection() const
{
    return mPriv->connection;
}

/**
 * Return whether this account's connection is changing presence.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return Whether this account's connection is changing presence.
 * \sa changingPresence(), currentPresenceChanged(), setRequestedPresence()
 */
bool Account::isChangingPresence() const
{
    return mPriv->changingPresence;
}

/**
 * Return a list of presences allowed by a connection to this account.
 *
 * Full functionality requires FeatureProtocolInfo and FeatureProfile to be ready as well as
 * Connection with Connection::FeatureSimplePresence enabled. If the connection is online and
 * Connection::FeatureSimplePresence is enabled, it will return the connection allowed statuses,
 * otherwise it will return a list os statuses based on profile() and protocolInfo() information
 * if the corresponding features are enabled.
 *
 * \param includeAllStatuses Whether the returned list will include all statuses or just the ones
 *                           that can are settable using setRequestedPresence().
 * \return A list of presences allowed by a connection to this account.
 */
PresenceSpecList Account::allowedPresenceStatuses(bool includeAllStatuses) const
{
    QMap<QString, PresenceSpec> specMap;

    // if the connection is online and ready use it
    if (mPriv->connection &&
        mPriv->connection->status() == ConnectionStatusConnected &&
        mPriv->connection->actualFeatures().contains(Connection::FeatureSimplePresence)) {
        SimpleStatusSpecMap connectionAllowedPresences =
            mPriv->connection->lowlevel()->allowedPresenceStatuses();
        SimpleStatusSpecMap::const_iterator i = connectionAllowedPresences.constBegin();
        SimpleStatusSpecMap::const_iterator end = connectionAllowedPresences.constEnd();
        for (; i != end; ++i) {
            PresenceSpec presence = PresenceSpec(i.key(), i.value());
            specMap.insert(i.key(), presence);
        }
    } else {
        ProtocolInfo pi = protocolInfo();
        if (pi.isValid()) {
            // add all ProtocolInfo presences to the returned map
            foreach (const PresenceSpec &piPresence, pi.allowedPresenceStatuses()) {
                QString piStatus = piPresence.presence().status();
                specMap.insert(piStatus, piPresence);
            }
        }

        ProfilePtr pr = profile();
        if (pr) {
            // add all Profile presences to the returned map
            foreach (const Profile::Presence &prPresence, pr->presences()) {
                QString prStatus = prPresence.id();
                if (specMap.contains(prStatus)) {
                    // we already got the presence from ProtocolInfo, just update
                    // canHaveStatusMessage if needed
                    PresenceSpec presence = specMap.value(prStatus);
                    if (presence.canHaveStatusMessage() != prPresence.canHaveStatusMessage()) {
                        SimpleStatusSpec spec;
                        spec.type = presence.presence().type();
                        spec.maySetOnSelf = presence.maySetOnSelf();
                        spec.canHaveMessage = prPresence.canHaveStatusMessage();
                        specMap.insert(prStatus, PresenceSpec(prStatus, spec));
                    }
                } else {
                    // presence not found in ProtocolInfo, adding it
                    specMap.insert(prStatus, presenceSpecForStatus(prStatus,
                                prPresence.canHaveStatusMessage()));
                }
            }

            // now remove all presences that are not in the Profile, if it does
            // not allow other presences, and the ones that are disabled
            QMap<QString, PresenceSpec>::iterator i = specMap.begin();
            QMap<QString, PresenceSpec>::iterator end = specMap.end();
            while (i != end) {
                PresenceSpec presence = i.value();
                QString status = presence.presence().status();
                bool hasPresence = pr->hasPresence(status);
                Profile::Presence prPresence = pr->presence(status);
                if ((!hasPresence && !pr->allowOtherPresences()) || (hasPresence && prPresence.isDisabled())) {
                     i = specMap.erase(i);
                } else {
                     ++i;
                }
            }
        }
    }

    // filter out presences that may not be set on self if includeAllStatuses is false
    if (!includeAllStatuses) {
        QMap<QString, PresenceSpec>::iterator i = specMap.begin();
        QMap<QString, PresenceSpec>::iterator end = specMap.end();
        while (i != end) {
            PresenceSpec presence = i.value();
            if (!presence.maySetOnSelf()) {
                i = specMap.erase(i);
            } else {
                ++i;
            }
        }
    }

    return specMap.values();
}

/**
 * Return the presence status that this account will have set on it by the
 * account manager if it brings it online automatically.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return The presence that will be set by the account manager if this
 *         account is brought online automatically by it.
 * \sa automaticPresenceChanged()
 */
Presence Account::automaticPresence() const
{
    return mPriv->automaticPresence;
}

/**
 * Set the presence status that this account should have if it is brought
 * online automatically by the account manager.
 *
 * \param presence The presence to set when this account is brought
 *                 online automatically by the account manager.
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when the call has finished.
 * \sa automaticPresenceChanged(), setRequestedPresence()
 */
PendingOperation *Account::setAutomaticPresence(const Presence &presence)
{
    return new PendingVoid(
            mPriv->properties->Set(
                QLatin1String(TELEPATHY_INTERFACE_ACCOUNT),
                QLatin1String("AutomaticPresence"),
                QDBusVariant(QVariant::fromValue(presence.barePresence()))),
            AccountPtr(this));
}

/**
 * Return the actual presence of this account.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return The actual presence of this account.
 * \sa currentPresenceChanged(), setRequestedPresence(), requestedPresence(), automaticPresence()
 */
Presence Account::currentPresence() const
{
    return mPriv->currentPresence;
}

/**
 * Return the requested presence of this account.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return The requested presence of this account.
 * \sa requestedPresenceChanged(), setRequestedPresence(), currentPresence(), automaticPresence()
 */
Presence Account::requestedPresence() const
{
    return mPriv->requestedPresence;
}

/**
 * Set the requested presence.
 *
 * When requested presence is changed, the account manager should attempt to
 * manipulate the connection to make currentPresence() match requestedPresence()
 * as closely as possible.
 *
 * \param presence The requested presence.
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when the call has finished.
 * \sa requestedPresenceChanged(), currentPresence(), automaticPresence(), setAutomaticPresence()
 */
PendingOperation *Account::setRequestedPresence(const Presence &presence)
{
    return new PendingVoid(
            mPriv->properties->Set(
                QLatin1String(TELEPATHY_INTERFACE_ACCOUNT),
                QLatin1String("RequestedPresence"),
                QDBusVariant(QVariant::fromValue(presence.barePresence()))),
            AccountPtr(this));
}

/**
 * Return whether this account is online.
 *
 * \return \c true if this account is online, otherwise \c false.
 */
bool Account::isOnline() const
{
    return mPriv->currentPresence.type() != ConnectionPresenceTypeOffline;
}

/**
 * Return the unique identifier of this account.
 *
 * This identifier should be unique per AccountManager implementation,
 * i.e. at least per QDBusConnection.
 *
 * \return The unique identifier of this account.
 */
QString Account::uniqueIdentifier() const
{
    QString path = objectPath();
    return path.right(path.length() -
            strlen("/org/freedesktop/Telepathy/Account/"));
}

/**
 * Return the normalized user ID of the local user of this account.
 *
 * It is unspecified whether this user ID is globally unique.
 *
 * As currently implemented, IRC user IDs are only unique within the same
 * IRCnet. On some saner protocols, the user ID includes a DNS name which
 * provides global uniqueness.
 *
 * If this value is not known yet (which will always be the case for accounts
 * that have never been online), it will be an empty string.
 *
 * It is possible that this value will change if the connection manager's
 * normalization algorithm changes.
 *
 * This method requires Account::FeatureCore to be enabled.
 *
 * \return Account normalized user ID of the local user.
 * \sa normalizedNameChanged()
 */
QString Account::normalizedName() const
{
    return mPriv->normalizedName;
}

/**
 * If this account is currently connected, disconnect and reconnect it. If it
 * is currently trying to connect, cancel the attempt to connect and start
 * another. If it is currently disconnected, do nothing.
 *
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when the call has finished.
 */
PendingOperation *Account::reconnect()
{
    return new PendingVoid(baseInterface()->Reconnect(), AccountPtr(this));
}

/**
 * Delete this account.
 *
 * \return A PendingOperation which will emit PendingOperation::finished
 *         when the call has finished.
 */
PendingOperation *Account::remove()
{
    return new PendingVoid(baseInterface()->Remove(), AccountPtr(this));
}

/**
 * Return whether passing hints on channel requests on this account is known to be supported.
 *
 * The return value is undefined unless Account::FeatureCore is ready on this account proxy.
 *
 * \return \c true if supported, \c false if not.
 */
bool Account::supportsRequestHints() const
{
    return mPriv->dispatcherContext->supportsHints;
}

/**
 * Return whether the ChannelRequest::succeeded(const Tp::ChannelPtr &) signal is expected to be
 * emitted with a non-NULL \a channel parameter for requests made using this account.
 *
 * This can be used as a run-time check for the Channel Dispatcher implementation being new enough.
 * In particular, similarly old Channel Dispatchers don't support request hints either, so the return
 * value for this function and Account::supportsRequestHints() will bet he same.
 *
 * The return value is undefined unless Account::FeatureCore is ready on this account proxy.
 *
 * \return \c true if supported, \c false if not.
 */
bool Account::requestsSucceedWithChannel() const
{
    return supportsRequestHints();
}

/**
 * Same as \c ensureTextChat(contactIdentifier, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::ensureTextChat(
        const QString &contactIdentifier,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return ensureTextChat(contactIdentifier, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to ensure that a text channel with the given
 * contact \a contactIdentifier exists, creating it if necessary.
 *
 * See ensureChannel() for more details.
 *
 * \param contactIdentifier The identifier of the contact to chat with.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::ensureTextChat(
        const QString &contactIdentifier,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_TEXT));
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType"),
                   (uint) Tp::HandleTypeContact);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetID"),
                   contactIdentifier);
    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, false, hints);
}

/**
 * Same as \c ensureTextChat(contact, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::ensureTextChat(
        const ContactPtr &contact,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return ensureTextChat(contact, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to ensure that a text channel with the given
 * contact \a contact exists, creating it if necessary.
 *
 * See ensureChannel() for more details.
 *
 * \param contact The contact to chat with.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::ensureTextChat(
        const ContactPtr &contact,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_TEXT));
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType"),
                   (uint) Tp::HandleTypeContact);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandle"),
                   contact ? contact->handle().at(0) : (uint) 0);
    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, false, hints);
}

/**
 * Same as \c ensureTextChatroom(roomName, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::ensureTextChatroom(
        const QString &roomName,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return ensureTextChatroom(roomName, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to ensure that a text chat room with the given
 * room name \a roomName exists, creating it if necessary.
 *
 * See ensureChannel() for more details.
 *
 * \param roomName The name of the chat room.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::ensureTextChatroom(
        const QString &roomName,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_TEXT));
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType"),
                   (uint) Tp::HandleTypeRoom);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetID"),
                   roomName);
    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, false, hints);
}

/**
 * Same as \c ensureStreamedMediaCall(contactIdentifier, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::ensureStreamedMediaCall(
        const QString &contactIdentifier,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return ensureStreamedMediaCall(contactIdentifier, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to ensure that a media channel with the given
 * contact \a contactIdentifier exists, creating it if necessary.
 *
 * See ensureChannel() for more details.
 *
 * \param contactIdentifier The identifier of the contact to call.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::ensureStreamedMediaCall(
        const QString &contactIdentifier,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_STREAMED_MEDIA));
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType"),
                   (uint) Tp::HandleTypeContact);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetID"),
                   contactIdentifier);
    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, false, hints);
}

/**
 * Same as \c ensureStreamedMediaCall(contact, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::ensureStreamedMediaCall(
        const ContactPtr &contact,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return ensureStreamedMediaCall(contact, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to ensure that a media channel with the given
 * contact \a contact exists, creating it if necessary.
 *
 * See ensureChannel() for more details.
 *
 * \param contact The contact to call.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::ensureStreamedMediaCall(
        const ContactPtr &contact,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_STREAMED_MEDIA));
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType"),
                   (uint) Tp::HandleTypeContact);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandle"),
                   contact ? contact->handle().at(0) : (uint) 0);
    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, false, hints);
}

/**
 * Same as \c ensureStreamedMediaAudioCall(contactIdentifier, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::ensureStreamedMediaAudioCall(
        const QString &contactIdentifier,
        QDateTime userActionTime,
        const QString &preferredHandler)
{
    return ensureStreamedMediaAudioCall(contactIdentifier, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to ensure that an audio call with the given
 * contact \a contactIdentifier exists, creating it if necessary.
 *
 * See ensureChannel() for more details.
 *
 * This will only work on relatively modern connection managers,
 * like telepathy-gabble 0.9.0 or later.
 *
 * \param contactIdentifier The identifier of the contact to call.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::ensureStreamedMediaAudioCall(
        const QString &contactIdentifier,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_STREAMED_MEDIA));
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType"),
                   (uint) Tp::HandleTypeContact);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".InitialAudio"),
                   true);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetID"),
                   contactIdentifier);
    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, false, hints);
}

/**
 * Same as \c ensureStreamedMediaAudioCall(contact, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::ensureStreamedMediaAudioCall(
        const ContactPtr &contact,
        QDateTime userActionTime,
        const QString &preferredHandler)
{
    return ensureStreamedMediaAudioCall(contact, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to ensure that an audio call with the given
 * contact \a contact exists, creating it if necessary.
 *
 * See ensureChannel() for more details.
 *
 * This will only work on relatively modern connection managers,
 * like telepathy-gabble 0.9.0 or later.
 *
 * \param contact The contact to call.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::ensureStreamedMediaAudioCall(
        const ContactPtr &contact,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_STREAMED_MEDIA));
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType"),
                   (uint) Tp::HandleTypeContact);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".InitialAudio"),
                   true);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandle"),
                   contact ? contact->handle().at(0) : (uint) 0);
    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, false, hints);
}

/**
 * Same as \c ensureStreamedMediaVideoCall(contactIdentifier, withAudio, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::ensureStreamedMediaVideoCall(
        const QString &contactIdentifier,
        bool withAudio,
        QDateTime userActionTime,
        const QString &preferredHandler)
{
    return ensureStreamedMediaVideoCall(contactIdentifier, withAudio, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to ensure that a video call with the given
 * contact \a contactIdentifier exists, creating it if necessary.
 *
 * See ensureChannel() for more details.
 *
 * This will only work on relatively modern connection managers,
 * like telepathy-gabble 0.9.0 or later.
 *
 * \param contactIdentifier The identifier of the contact to call.
 * \param withAudio true if both audio and video are required, false for a
 *                  video-only call.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::ensureStreamedMediaVideoCall(
        const QString &contactIdentifier,
        bool withAudio,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_STREAMED_MEDIA));
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType"),
                   (uint) Tp::HandleTypeContact);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".InitialVideo"),
                   true);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetID"),
                   contactIdentifier);

    if (withAudio) {
        request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".InitialAudio"),
                       true);
    }

    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, false, hints);
}

/**
 * Same as \c ensureStreamedMediaVideoCall(contact, withAudio, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::ensureStreamedMediaVideoCall(
        const ContactPtr &contact,
        bool withAudio,
        QDateTime userActionTime,
        const QString &preferredHandler)
{
    return ensureStreamedMediaVideoCall(contact, withAudio, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to ensure that a video call with the given
 * contact \a contact exists, creating it if necessary.
 *
 * See ensureChannel() for more details.
 *
 * This will only work on relatively modern connection managers,
 * like telepathy-gabble 0.9.0 or later.
 *
 * \param contact The contact to call.
 * \param withAudio true if both audio and video are required, false for a
 *                  video-only call.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::ensureStreamedMediaVideoCall(
        const ContactPtr &contact,
        bool withAudio,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_STREAMED_MEDIA));
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType"),
                   (uint) Tp::HandleTypeContact);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".InitialVideo"),
                   true);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandle"),
                   contact ? contact->handle().at(0) : (uint) 0);

    if (withAudio) {
        request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".InitialAudio"),
                       true);
    }

    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, false, hints);
}

/**
 * Same as \c createFileTransfer(contactIdentifier, properties, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::createFileTransfer(
        const QString &contactIdentifier,
        const FileTransferChannelCreationProperties &properties,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return createFileTransfer(contactIdentifier, properties, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to create a file transfer channel with the given
 * contact \a contact.
 *
 * \param contactIdentifier The identifier of the contact to send a file.
 * \param fileName The suggested filename for the receiver.
 * \param properties The desired properties.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::createFileTransfer(
        const QString &contactIdentifier,
        const FileTransferChannelCreationProperties &properties,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER));
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType"),
                   (uint) Tp::HandleTypeContact);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetID"),
                   contactIdentifier);

    QFileInfo fileInfo(properties.suggestedFileName());
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".Filename"),
                   fileInfo.fileName());
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".ContentType"),
                   properties.contentType());
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".Size"),
                   properties.size());

    if (properties.hasContentHash()) {
        request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".ContentHashType"),
                       (uint) properties.contentHashType());
        request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".ContentHash"),
                       properties.contentHash());
    }

    if (properties.hasDescription()) {
        request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".Description"),
                       properties.description());
    }

    if (properties.hasLastModificationTime()) {
        request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".Date"),
                       (qulonglong) properties.lastModificationTime().toTime_t());
    }

    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, true, hints);
}

/**
 * Same as \c createFileTransfer(contact, properties, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::createFileTransfer(
        const ContactPtr &contact,
        const FileTransferChannelCreationProperties &properties,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return createFileTransfer(contact, properties, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to create a file transfer channel with the given
 * contact \a contact.
 *
 * \param contact The contact to send a file.
 * \param properties The desired properties.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::createFileTransfer(
        const ContactPtr &contact,
        const FileTransferChannelCreationProperties &properties,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER));
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandleType"),
                   (uint) Tp::HandleTypeContact);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetHandle"),
                   contact ? contact->handle().at(0) : (uint) 0);

    QFileInfo fileInfo(properties.suggestedFileName());
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".Filename"),
                   fileInfo.fileName());
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".ContentType"),
                   properties.contentType());
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".Size"),
                   properties.size());

    if (properties.hasContentHash()) {
        request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".ContentHashType"),
                       (uint) properties.contentHashType());
        request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".ContentHash"),
                       properties.contentHash());
    }

    if (properties.hasDescription()) {
        request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".Description"),
                       properties.description());
    }

    if (properties.hasLastModificationTime()) {
        request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_FILE_TRANSFER ".Date"),
                       (qulonglong) properties.lastModificationTime().toTime_t());
    }

    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, true, hints);
}

/**
 * Same as \c createConferenceStreamedMediaCall(channels, initialInviteeContactsIdentifiers, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::createConferenceStreamedMediaCall(
        const QList<ChannelPtr> &channels,
        const QStringList &initialInviteeContactsIdentifiers,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return createConferenceStreamedMediaCall(channels, initialInviteeContactsIdentifiers, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to create a conference media call with the given
 * channels \a channels.
 *
 * \param channels The conference channels.
 * \param initialInviteeContactsIdentifiers A list of additional contacts
 *                                          identifiers to be invited to this
 *                                          conference when it is created.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::createConferenceStreamedMediaCall(
        const QList<ChannelPtr> &channels,
        const QStringList &initialInviteeContactsIdentifiers,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    mPriv->addConferenceRequestParameters(
            TELEPATHY_INTERFACE_CHANNEL_TYPE_STREAMED_MEDIA,
            HandleTypeNone,
            channels, initialInviteeContactsIdentifiers, request);

    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, true, hints);
}

/**
 * Same as \c createConferenceStreamedMediaCall(channels, initialInviteeContacts, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::createConferenceStreamedMediaCall(
        const QList<ChannelPtr> &channels,
        const QList<ContactPtr> &initialInviteeContacts,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return createConferenceStreamedMediaCall(channels, initialInviteeContacts, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to create a conference media call with the given
 * channels \a channels.
 *
 * \param channels The conference channels.
 * \param initialInviteeContactsIdentifiers A list of additional contacts
 *                                          to be invited to this
 *                                          conference when it is created.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::createConferenceStreamedMediaCall(
        const QList<ChannelPtr> &channels,
        const QList<ContactPtr> &initialInviteeContacts,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    // TODO may we use Channel.Type.StreamedMedia here or Channel.Type.Call
    //      should be used?
    mPriv->addConferenceRequestParameters(
            TELEPATHY_INTERFACE_CHANNEL_TYPE_STREAMED_MEDIA,
            HandleTypeNone,
            channels, initialInviteeContacts, request);

    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, true, hints);
}

/**
 * Same as \c createConferenceTextChat(channels, initialInviteeContactsIdentifiers, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::createConferenceTextChat(
        const QList<ChannelPtr> &channels,
        const QStringList &initialInviteeContactsIdentifiers,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return createConferenceTextChat(channels, initialInviteeContactsIdentifiers, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to create a conference text chat with the given
 * channels \a channels.
 *
 * \param channels The conference channels.
 * \param initialInviteeContactsIdentifiers A list of additional contacts
 *                                          identifiers to be invited to this
 *                                          conference when it is created.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::createConferenceTextChat(
        const QList<ChannelPtr> &channels,
        const QStringList &initialInviteeContactsIdentifiers,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    mPriv->addConferenceRequestParameters(
            TELEPATHY_INTERFACE_CHANNEL_TYPE_TEXT,
            HandleTypeNone,
            channels, initialInviteeContactsIdentifiers, request);

    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, true, hints);
}

/**
 * Same as \c createConferenceTextChat(channels, initialInviteeContacts, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::createConferenceTextChat(
        const QList<ChannelPtr> &channels,
        const QList<ContactPtr> &initialInviteeContacts,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return createConferenceTextChat(channels, initialInviteeContacts, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to create a conference text chat with the given
 * channels \a channels.
 *
 * \param channels The conference channels.
 * \param initialInviteeContacts list of additional contacts
 *                                          to be invited to this
 *                                          conference when it is created.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::createConferenceTextChat(
        const QList<ChannelPtr> &channels,
        const QList<ContactPtr> &initialInviteeContacts,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    mPriv->addConferenceRequestParameters(
            TELEPATHY_INTERFACE_CHANNEL_TYPE_TEXT,
            HandleTypeNone,
            channels, initialInviteeContacts, request);

    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, true, hints);
}

/**
 * Same as \c createConferenceTextChatroom(roomName, channels, initialInviteeContactsIdentifiers, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::createConferenceTextChatRoom(
        const QString &roomName,
        const QList<ChannelPtr> &channels,
        const QStringList &initialInviteeContactsIdentifiers,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return createConferenceTextChatroom(roomName, channels, initialInviteeContactsIdentifiers, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to create a conference text chat room with the given
 * channels \a channels and room name \a roomName.
 *
 * \param roomName The room name.
 * \param channels The conference channels.
 * \param initialInviteeContactsIdentifiers A list of additional contacts
 *                                          identifiers to be invited to this
 *                                          conference when it is created.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::createConferenceTextChatroom(
        const QString &roomName,
        const QList<ChannelPtr> &channels,
        const QStringList &initialInviteeContactsIdentifiers,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetID"),
                   roomName);
    mPriv->addConferenceRequestParameters(
            TELEPATHY_INTERFACE_CHANNEL_TYPE_TEXT,
            HandleTypeRoom,
            channels, initialInviteeContactsIdentifiers, request);

    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, true, hints);
}

/**
 * Same as \c createConferenceTextChatroom(roomName, channels, initialInviteeContacts, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::createConferenceTextChatRoom(
        const QString &roomName,
        const QList<ChannelPtr> &channels,
        const QList<ContactPtr> &initialInviteeContacts,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return createConferenceTextChatroom(roomName, channels, initialInviteeContacts, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to create a conference text chat room with the given
 * channels \a channels and room name \a roomName.
 *
 * \param roomName The room name.
 * \param channels The conference channels.
 * \param initialInviteeContactsIdentifiers A list of additional contacts
 *                                          to be invited to this
 *                                          conference when it is created.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa ensureChannel(), createChannel()
 */
PendingChannelRequest *Account::createConferenceTextChatroom(
        const QString &roomName,
        const QList<ChannelPtr> &channels,
        const QList<ContactPtr> &initialInviteeContacts,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".TargetID"),
                   roomName);
    mPriv->addConferenceRequestParameters(
            TELEPATHY_INTERFACE_CHANNEL_TYPE_TEXT,
            HandleTypeRoom,
            channels, initialInviteeContacts, request);

    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, true, hints);
}

/**
 * Same as \c createContactSearch(server, limit, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::createContactSearch(
        const QString &server,
        uint limit,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return createContactSearch(server, limit, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to create a contact search channel with the given
 * server \a server and limit \a limit.
 *
 * \param server For protocols which support searching for contacts on multiple servers with
 *               different DNS names (like XMPP), the DNS name of the server to be searched,
 *               e.g. "characters.shakespeare.lit". Otherwise, an empty string.
 * \param limit The desired maximum number of results that should be returned by a doing a search.
 *              If the protocol does not support specifying a limit for the number of results
 *              returned at a time, this will be ignored.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \return A PendingChannelRequest which will emit PendingChannelRequest::finished
 *         when the call has finished.
 * \sa createChannel()
 */
PendingChannelRequest *Account::createContactSearch(
        const QString &server,
        uint limit,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    QVariantMap request;
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL ".ChannelType"),
                   QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_CONTACT_SEARCH));
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_CONTACT_SEARCH ".Server"),
                   server);
    request.insert(QLatin1String(TELEPATHY_INTERFACE_CHANNEL_TYPE_CONTACT_SEARCH ".Limit"), limit);

    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, true, hints);
}

/**
 * Same as \c createChannel(request, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::createChannel(
        const QVariantMap &request,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return createChannel(request, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to create a channel.
 * This initially just creates a PendingChannelRequest object,
 * which can be used to track the success or failure of the request,
 * or to cancel it.
 *
 * Helper methods for text chat, text chat room, media call and conference are
 * provided and should be used if appropriate.
 *
 * \param request A dictionary containing desirable properties.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \sa createChannel()
 */
PendingChannelRequest *Account::createChannel(
        const QVariantMap &request,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, true, hints);
}

/**
 * Same as \c ensureChannel(request, userActionTime, preferredHandler, ChannelRequestHints())
 */
PendingChannelRequest *Account::ensureChannel(
        const QVariantMap &request,
        const QDateTime &userActionTime,
        const QString &preferredHandler)
{
    return ensureChannel(request, userActionTime, preferredHandler, ChannelRequestHints());
}

/**
 * Start a request to ensure that a channel exists, creating it if necessary.
 * This initially just creates a PendingChannelRequest object,
 * which can be used to track the success or failure of the request,
 * or to cancel it.
 *
 * Helper methods for text chat, text chat room, media call and conference are
 * provided and should be used if appropriate.
 *
 * \param request A dictionary containing desirable properties.
 * \param userActionTime The time at which user action occurred, or QDateTime()
 *                       if this channel request is for some reason not
 *                       involving user action.
 * \param preferredHandler Either the well-known bus name (starting with
 *                         org.freedesktop.Telepathy.Client.) of the preferred
 *                         handler for this channel, or an empty string to
 *                         indicate that any handler would be acceptable.
 * \param hints Arbitrary metadata which will be relayed to the handler if supported,
 *              as indicated by supportsRequestHints().
 * \sa createChannel()
 */
PendingChannelRequest *Account::ensureChannel(
        const QVariantMap &request,
        const QDateTime &userActionTime,
        const QString &preferredHandler,
        const ChannelRequestHints &hints)
{
    return new PendingChannelRequest(AccountPtr(this), request, userActionTime,
            preferredHandler, false, hints);
}

/**
 * \fn void Account::serviceNameChanged(const QString &serviceName);
 *
 * This signal is emitted when the value of serviceName() of this account
 * changes.
 *
 * \param serviceName The new service name of this account.
 * \sa serviceName(), setServiceName()
 */

/**
 * \fn void Account::profileChanged(const Tp::ProfilePtr &profile);
 *
 * This signal is emitted when the value of profile() of this account
 * changes.
 *
 * \param profile The new profile of this account.
 * \sa profile()
 */

/**
 * \fn void Account::displayNameChanged(const QString &displayName);
 *
 * This signal is emitted when the value of displayName() of this account
 * changes.
 *
 * \param displayName The new display name of this account.
 * \sa displayName(), setDisplayName()
 */

/**
 * \fn void Account::iconNameChanged(const QString &iconName);
 *
 * This signal is emitted when the value of iconName() of this account changes.
 *
 * \param iconName The new icon name of this account.
 * \sa iconName(), setIconName()
 */

/**
 * \fn void Account::nicknameChanged(const QString &nickname);
 *
 * This signal is emitted when the value of nickname() of this account changes.
 *
 * \param nickname The new nickname of this account.
 * \sa nickname(), setNickname()
 */

/**
 * \fn void Account::normalizedNameChanged(const QString &normalizedName);
 *
 * This signal is emitted when the value of normalizedName() of this account
 * changes.
 *
 * \param normalizedName The new normalized name of this account.
 * \sa normalizedName()
 */

/**
 * \fn void Account::validityChanged(bool validity);
 *
 * This signal is emitted when the value of isValidAccount() of this account
 * changes.
 *
 * \param validity The new validity of this account.
 * \sa isValidAccount()
 */

/**
 * \fn void Account::stateChanged(bool state);
 *
 * This signal is emitted when the value of isEnabled() of this account
 * changes.
 *
 * \param state The new state of this account.
 * \sa isEnabled()
 */

/**
 * \fn void Account::connectsAutomaticallyPropertyChanged(bool connectsAutomatically);
 *
 * This signal is emitted when the value of connectsAutomatically() of this
 * account changes.
 *
 * \param connectsAutomatically The new value of connects automatically property
 *                              of this account.
 * \sa isEnabled()
 */

/**
 * \fn void Account::firstOnline();
 *
 * This signal is emitted when this account is first put online.
 *
 * \sa hasBeenOnline()
 */

/**
 * \fn void Account::parametersChanged(const QVariantMap &parameters);
 *
 * This signal is emitted when the value of parameters() of this
 * account changes.
 *
 * \param parameters The new parameters of this account.
 * \sa parameters()
 */

/**
 * \fn void Account::changingPresence(bool value);
 *
 * This signal is emitted when the value of isChangingPresence() of this
 * account changes.
 *
 * \param value Whether this account's connection is changing presence.
 * \sa isChangingPresence()
 */

/**
 * \fn void Account::automaticPresenceChanged(const Tp::Presence &automaticPresence) const;
 *
 * This signal is emitted when the value of automaticPresence() of this
 * account changes.
 *
 * \param automaticPresence The new value of automatic presence property of this
 *                          account.
 * \sa automaticPresence()
 */

/**
 * \fn void Account::currentPresenceChanged(const Tp::Presence &currentPresence) const;
 *
 * This signal is emitted when the value of currentPresence() of this
 * account changes.
 *
 * \param currentPresence The new value of current presence property of this
 *                        account.
 * \sa currentPresence()
 */

/**
 * \fn void Account::requestedPresenceChanged(const Tp::Presence &requestedPresence) const;
 *
 * This signal is emitted when the value of requestedPresence() of this
 * account changes.
 *
 * \param requestedPresence The new value of requested presence property of this
 *                          account.
 * \sa requestedPresence()
 */

/**
 * \fn void Account::onlinenessChanged(bool online) const;
 *
 * This signal is emitted when the value of isOnline() of this
 * account changes.
 *
 * \param online Whether this account is online.
 * \sa currentPresence()
 */

/**
 * \fn void Account::avatarChanged(const Tp::Avatar &avatar);
 *
 * This signal is emitted when the value of avatar() of this
 * account changes.
 *
 * \param avatar The new avatar of this account.
 * \sa avatar()
 */

/**
 * \fn void Account::connectionStatusChanged(Tp::ConnectionStatus status);
 *
 * This signal is emitted when the connection status of this account changes.
 *
 * \param status The new status of this account connection.
 * \param statusReason The new status reason of this account connection.
 * \param errorName The D-Bus error name for the last disconnection or
 *                  connection failure,
 * \param errorDetails The error details related to errorName.
 * \sa connectionStatus(), connectionStatusReason(), connectionError(), connectionErrorDetails(),
 *     Connection::ErrorDetails
 */

/**
 * \fn void Account::connectionChanged(const Tp::ConnectionPtr &connection);
 *
 * This signal is emitted when the value of connection() of this
 * account changes.
 *
 * \param connection A ConnectionPtr pointing to the new Connection object or a null ConnectionPtr
 *                   if there is no connection.
 * \sa connection()
 */

/**
 * Return the Client::AccountInterface interface proxy object for this account.
 * This method is protected since the convenience methods provided by this
 * class should generally be used instead of calling D-Bus methods
 * directly.
 *
 * \return A pointer to the existing Client::AccountInterface object for this
 *         Account object.
 */
Client::AccountInterface *Account::baseInterface() const
{
    return mPriv->baseInterface;
}

/**
 * Return the Client::ChannelDispatcherInterface interface proxy object to use for requesting
 * channels on this account.
 *
 * This method is protected since the convenience methods provided by this
 * class should generally be used instead of calling D-Bus methods
 * directly.
 *
 * \return A pointer to the existing Client::ChannelDispatcherInterface object for this
 *         Account object.
 */
Client::ChannelDispatcherInterface *Account::dispatcherInterface() const
{
    return mPriv->dispatcherContext->iface;
}

/**** Private ****/
void Account::Private::init()
{
    if (!parent->isValid()) {
        return;
    }

    parent->connect(baseInterface,
            SIGNAL(Removed()),
            SLOT(onRemoved()));
    parent->connect(baseInterface,
            SIGNAL(AccountPropertyChanged(QVariantMap)),
            SLOT(onPropertyChanged(QVariantMap)));
}

void Account::Private::introspectMain(Account::Private *self)
{
    if (self->dispatcherContext->introspected) {
        self->parent->onDispatcherIntrospected(0);
        return;
    }

    if (!self->dispatcherContext->introspectOp) {
        debug() << "Discovering if the Channel Dispatcher supports request hints";
        self->dispatcherContext->introspectOp =
            self->dispatcherContext->iface->requestPropertySupportsRequestHints();
    }

    connect(self->dispatcherContext->introspectOp.data(),
            SIGNAL(finished(Tp::PendingOperation*)),
            self->parent,
            SLOT(onDispatcherIntrospected(Tp::PendingOperation*)));
}

void Account::Private::introspectAvatar(Account::Private *self)
{
    debug() << "Calling GetAvatar(Account)";
    // we already checked if avatar interface exists, so bypass avatar interface
    // checking
    Client::AccountInterfaceAvatarInterface *iface =
        self->parent->interface<Client::AccountInterfaceAvatarInterface>();

    // If we are here it means the user cares about avatar, so
    // connect to avatar changed signal, so we update the avatar
    // when it changes.
    self->parent->connect(iface,
            SIGNAL(AvatarChanged()),
            SLOT(onAvatarChanged()));

    self->retrieveAvatar();
}

void Account::Private::introspectProtocolInfo(Account::Private *self)
{
    Q_ASSERT(!self->cm);

    self->cm = ConnectionManager::create(
            self->parent->dbusConnection(), self->cmName,
            self->connFactory, self->chanFactory, self->contactFactory);
    self->parent->connect(self->cm->becomeReady(),
            SIGNAL(finished(Tp::PendingOperation*)),
            SLOT(onConnectionManagerReady(Tp::PendingOperation*)));
}

void Account::Private::introspectCapabilities(Account::Private *self)
{
    if (!self->connection) {
        // there is no connection, just make capabilities ready
        self->readinessHelper->setIntrospectCompleted(FeatureCapabilities, true);
        return;
    }

    self->parent->connect(self->connection->becomeReady(),
            SIGNAL(finished(Tp::PendingOperation*)),
            SLOT(onConnectionReady(Tp::PendingOperation*)));
}

void Account::Private::updateProperties(const QVariantMap &props)
{
    debug() << "Account::updateProperties: changed:";

    if (props.contains(QLatin1String("Interfaces"))) {
        parent->setInterfaces(qdbus_cast<QStringList>(props[QLatin1String("Interfaces")]));
        debug() << " Interfaces:" << parent->interfaces();
    }

    QString oldIconName = parent->iconName();
    bool serviceNameChanged = false;
    bool profileChanged = false;
    if (props.contains(QLatin1String("Service")) &&
        serviceName != qdbus_cast<QString>(props[QLatin1String("Service")])) {
        serviceNameChanged = true;
        serviceName = qdbus_cast<QString>(props[QLatin1String("Service")]);
        debug() << " Service Name:" << parent->serviceName();
        /* use parent->serviceName() here as if the service name is empty we are going to use the
         * protocol name */
        emit parent->serviceNameChanged(parent->serviceName());
        parent->notify("serviceName");

        /* if we had a profile and the service changed, it means the profile also changed */
        if (parent->isReady(Account::FeatureProfile)) {
            /* service name changed, let's recreate profile */
            profileChanged = true;
            profile.reset();
            emit parent->profileChanged(parent->profile());
            parent->notify("profile");
        }
    }

    if (props.contains(QLatin1String("DisplayName")) &&
        displayName != qdbus_cast<QString>(props[QLatin1String("DisplayName")])) {
        displayName = qdbus_cast<QString>(props[QLatin1String("DisplayName")]);
        debug() << " Display Name:" << displayName;
        emit parent->displayNameChanged(displayName);
        parent->notify("displayName");
    }

    if ((props.contains(QLatin1String("Icon")) &&
         oldIconName != qdbus_cast<QString>(props[QLatin1String("Icon")])) ||
        serviceNameChanged) {

        if (props.contains(QLatin1String("Icon"))) {
            iconName = qdbus_cast<QString>(props[QLatin1String("Icon")]);
        }

        QString newIconName = parent->iconName();
        if (oldIconName != newIconName) {
            debug() << " Icon:" << newIconName;
            emit parent->iconNameChanged(newIconName);
            parent->notify("iconName");
        }
    }

    if (props.contains(QLatin1String("Nickname")) &&
        nickname != qdbus_cast<QString>(props[QLatin1String("Nickname")])) {
        nickname = qdbus_cast<QString>(props[QLatin1String("Nickname")]);
        debug() << " Nickname:" << nickname;
        emit parent->nicknameChanged(nickname);
        parent->notify("nickname");
    }

    if (props.contains(QLatin1String("NormalizedName")) &&
        normalizedName != qdbus_cast<QString>(props[QLatin1String("NormalizedName")])) {
        normalizedName = qdbus_cast<QString>(props[QLatin1String("NormalizedName")]);
        debug() << " Normalized Name:" << normalizedName;
        emit parent->normalizedNameChanged(normalizedName);
        parent->notify("normalizedName");
    }

    if (props.contains(QLatin1String("Valid")) &&
        valid != qdbus_cast<bool>(props[QLatin1String("Valid")])) {
        valid = qdbus_cast<bool>(props[QLatin1String("Valid")]);
        debug() << " Valid:" << (valid ? "true" : "false");
        emit parent->validityChanged(valid);
        parent->notify("valid");
    }

    if (props.contains(QLatin1String("Enabled")) &&
        enabled != qdbus_cast<bool>(props[QLatin1String("Enabled")])) {
        enabled = qdbus_cast<bool>(props[QLatin1String("Enabled")]);
        debug() << " Enabled:" << (enabled ? "true" : "false");
        emit parent->stateChanged(enabled);
        parent->notify("enabled");
    }

    if (props.contains(QLatin1String("ConnectAutomatically")) &&
        connectsAutomatically !=
                qdbus_cast<bool>(props[QLatin1String("ConnectAutomatically")])) {
        connectsAutomatically =
                qdbus_cast<bool>(props[QLatin1String("ConnectAutomatically")]);
        debug() << " Connects Automatically:" << (connectsAutomatically ? "true" : "false");
        emit parent->connectsAutomaticallyPropertyChanged(connectsAutomatically);
        parent->notify("connectsAutomatically");
    }

    if (props.contains(QLatin1String("HasBeenOnline")) &&
        !hasBeenOnline &&
        qdbus_cast<bool>(props[QLatin1String("HasBeenOnline")])) {
        hasBeenOnline = true;
        debug() << " HasBeenOnline changed to true";
        // don't emit firstOnline unless we're already ready, that would be
        // misleading - we'd emit it just before any already-used account
        // became ready
        if (parent->isReady()) {
            emit parent->firstOnline();
        }
        parent->notify("hasBeenOnline");
    }

    if (props.contains(QLatin1String("Parameters")) &&
        parameters != qdbus_cast<QVariantMap>(props[QLatin1String("Parameters")])) {
        parameters = qdbus_cast<QVariantMap>(props[QLatin1String("Parameters")]);
        debug() << " Parameters:" << parameters;
        emit parent->parametersChanged(parameters);
        parent->notify("parameters");
    }

    if (props.contains(QLatin1String("AutomaticPresence")) &&
        automaticPresence.barePresence() != qdbus_cast<SimplePresence>(
                props[QLatin1String("AutomaticPresence")])) {
        automaticPresence = Presence(qdbus_cast<SimplePresence>(
                props[QLatin1String("AutomaticPresence")]));
        debug() << " Automatic Presence:" << automaticPresence.type() <<
            "-" << automaticPresence.status();
        emit parent->automaticPresenceChanged(automaticPresence);
        parent->notify("automaticPresence");
    }

    if (props.contains(QLatin1String("CurrentPresence")) &&
        currentPresence.barePresence() != qdbus_cast<SimplePresence>(
                props[QLatin1String("CurrentPresence")])) {
        currentPresence = Presence(qdbus_cast<SimplePresence>(
                props[QLatin1String("CurrentPresence")]));
        debug() << " Current Presence:" << currentPresence.type() <<
            "-" << currentPresence.status();
        emit parent->currentPresenceChanged(currentPresence);
        parent->notify("currentPresence");
        emit parent->onlinenessChanged(parent->isOnline());
        parent->notify("online");
    }

    if (props.contains(QLatin1String("RequestedPresence")) &&
        requestedPresence.barePresence() != qdbus_cast<SimplePresence>(
                props[QLatin1String("RequestedPresence")])) {
        requestedPresence = Presence(qdbus_cast<SimplePresence>(
                props[QLatin1String("RequestedPresence")]));
        debug() << " Requested Presence:" << requestedPresence.type() <<
            "-" << requestedPresence.status();
        emit parent->requestedPresenceChanged(requestedPresence);
        parent->notify("requestedPresence");
    }

    if (props.contains(QLatin1String("ChangingPresence")) &&
        changingPresence != qdbus_cast<bool>(
                props[QLatin1String("ChangingPresence")])) {
        changingPresence = qdbus_cast<bool>(
                props[QLatin1String("ChangingPresence")]);
        debug() << " Changing Presence:" << changingPresence;
        emit parent->changingPresence(changingPresence);
        parent->notify("changingPresence");
    }

    if (props.contains(QLatin1String("Connection"))) {
        QString path = qdbus_cast<QDBusObjectPath>(props[QLatin1String("Connection")]).path();
        if (path.isEmpty()) {
            debug() << " The map contains \"Connection\" but it's empty as a QDBusObjectPath!";
            debug() << " Trying QString (known bug in some MC/dbus-glib versions)";
            path = qdbus_cast<QString>(props[QLatin1String("Connection")]);
        }

        debug() << " Connection Object Path:" << path;
        if (path == QLatin1String("/")) {
            path = QString();
        }

        connObjPathQueue.enqueue(path);

        if (connObjPathQueue.size() == 1) {
            processConnQueue();
        }

        // onConnectionBuilt for a previous path will make sure the path we enqueued is processed if
        // the queue wasn't empty (so is now size() > 1)
    }

    bool connectionStatusChanged = false;
    if (props.contains(QLatin1String("ConnectionStatus")) ||
        props.contains(QLatin1String("ConnectionStatusReason")) ||
        props.contains(QLatin1String("ConnectionError")) ||
        props.contains(QLatin1String("ConnectionErrorDetails"))) {
        ConnectionStatus oldConnectionStatus = connectionStatus;

        if (props.contains(QLatin1String("ConnectionStatus")) &&
            connectionStatus != ConnectionStatus(
                    qdbus_cast<uint>(props[QLatin1String("ConnectionStatus")]))) {
            connectionStatus = ConnectionStatus(
                    qdbus_cast<uint>(props[QLatin1String("ConnectionStatus")]));
            debug() << " Connection Status:" << connectionStatus;
            connectionStatusChanged = true;
        }

        if (props.contains(QLatin1String("ConnectionStatusReason")) &&
            connectionStatusReason != ConnectionStatusReason(
                    qdbus_cast<uint>(props[QLatin1String("ConnectionStatusReason")]))) {
            connectionStatusReason = ConnectionStatusReason(
                    qdbus_cast<uint>(props[QLatin1String("ConnectionStatusReason")]));
            debug() << " Connection StatusReason:" << connectionStatusReason;
            connectionStatusChanged = true;
        }

        if (connectionStatusChanged) {
            parent->notify("connectionStatus");
            parent->notify("connectionStatusReason");
        }

        if (props.contains(QLatin1String("ConnectionError")) &&
            connectionError != qdbus_cast<QString>(
                props[QLatin1String("ConnectionError")])) {
            connectionError = qdbus_cast<QString>(
                    props[QLatin1String("ConnectionError")]);
            debug() << " Connection Error:" << connectionError;
            connectionStatusChanged = true;
        }

        if (props.contains(QLatin1String("ConnectionErrorDetails")) &&
            connectionErrorDetails.allDetails() != qdbus_cast<QVariantMap>(
                props[QLatin1String("ConnectionErrorDetails")])) {
            connectionErrorDetails = Connection::ErrorDetails(qdbus_cast<QVariantMap>(
                    props[QLatin1String("ConnectionErrorDetails")]));
            debug() << " Connection Error Details:" << connectionErrorDetails.allDetails();
            connectionStatusChanged = true;
        }

        if (connectionStatusChanged) {
            /* Something other than status changed, let's not emit connectionStatusChanged
             * and keep the error/errorDetails, for the next interaction.
             * It may happen if ConnectionError changes and in another property
             * change the status changes to Disconnected, so we use the error
             * previously signalled. If the status changes to something other
             * than Disconnected later, the error is cleared. */
            if (oldConnectionStatus != connectionStatus) {
                /* We don't signal error for status other than Disconnected */
                if (connectionStatus != ConnectionStatusDisconnected) {
                    connectionError = QString();
                    connectionErrorDetails = Connection::ErrorDetails();
                } else if (connectionError.isEmpty()) {
                    connectionError = ConnectionHelper::statusReasonToErrorName(
                            connectionStatusReason, oldConnectionStatus);
                }

                checkCapabilitiesChanged(profileChanged);

                emit parent->connectionStatusChanged(connectionStatus);
                parent->notify("connectionError");
                parent->notify("connectionErrorDetails");
            } else {
                connectionStatusChanged = false;
            }
        }
    }

    if (!connectionStatusChanged && profileChanged) {
        checkCapabilitiesChanged(profileChanged);
    }
}

void Account::Private::retrieveAvatar()
{
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(
            parent->mPriv->properties->Get(
                QLatin1String(TELEPATHY_INTERFACE_ACCOUNT_INTERFACE_AVATAR),
                QLatin1String("Avatar")), parent);
    parent->connect(watcher,
            SIGNAL(finished(QDBusPendingCallWatcher*)),
            SLOT(gotAvatar(QDBusPendingCallWatcher*)));
}

bool Account::Private::processConnQueue()
{
    while (!connObjPathQueue.isEmpty()) {
        QString path = connObjPathQueue.head();
        if (path.isEmpty()) {
            if (!connection.isNull()) {
                debug() << "Dropping connection for account" << parent->objectPath();

                connection.reset();
                emit parent->connectionChanged(connection);
                parent->notify("connection");
                parent->notify("connectionObjectPath");
            }

            connObjPathQueue.dequeue();
        } else {
            debug() << "Building connection" << path << "for account" << parent->objectPath();

            if (connection && connection->objectPath() == path) {
                debug() << "  Connection already built";
                connObjPathQueue.dequeue();
                return true;
            }

            QString busName = path.mid(1).replace(QLatin1String("/"), QLatin1String("."));
            parent->connect(connFactory->proxy(busName, path, chanFactory, contactFactory),
                    SIGNAL(finished(Tp::PendingOperation*)),
                    SLOT(onConnectionBuilt(Tp::PendingOperation*)));

            // No dequeue here, but only in onConnectionBuilt, so we will queue future changes
            return false; // Only move on to the next paths when that build finishes
        }
    }

    return true;
}

void Account::onDispatcherIntrospected(Tp::PendingOperation *op)
{
    if (!mPriv->dispatcherContext->introspected) {
        Tp::PendingVariant *pv = static_cast<Tp::PendingVariant *>(op);
        Q_ASSERT(pv != NULL);

        // Only the first Account for a given dispatcher will enter this branch, and will
        // immediately make further created accounts skip the whole waiting for CD to get
        // introspected part entirely
        mPriv->dispatcherContext->introspected = true;

        if (pv->isValid()) {
            mPriv->dispatcherContext->supportsHints = qdbus_cast<bool>(pv->result());
            debug() << "Discovered channel dispatcher support for request hints: "
                << mPriv->dispatcherContext->supportsHints;
        } else {
            if (pv->errorName() == TP_QT4_ERROR_NOT_IMPLEMENTED) {
                debug() << "Channel Dispatcher does not implement support for request hints";
            } else {
                warning() << "(Too old?) Channel Dispatcher failed to tell us whether"
                    << "it supports request hints, assuming it doesn't:"
                    << pv->errorName() << ':' << pv->errorMessage();
            }
            mPriv->dispatcherContext->supportsHints = false;
        }
    }

    debug() << "Calling Properties::GetAll(Account) on " << objectPath();
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(
            mPriv->properties->GetAll(
                QLatin1String(TELEPATHY_INTERFACE_ACCOUNT)), this);
    connect(watcher,
            SIGNAL(finished(QDBusPendingCallWatcher*)),
            SLOT(gotMainProperties(QDBusPendingCallWatcher*)));
}

void Account::gotMainProperties(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<QVariantMap> reply = *watcher;

    if (!reply.isError()) {
        debug() << "Got reply to Properties.GetAll(Account) for" << objectPath();
        mPriv->updateProperties(reply.value());

        mPriv->readinessHelper->setInterfaces(interfaces());
        mPriv->mayFinishCore = true;

        if (mPriv->connObjPathQueue.isEmpty()) {
            debug() << "Account basic functionality is ready";
            mPriv->coreFinished = true;
            mPriv->readinessHelper->setIntrospectCompleted(FeatureCore, true);
        } else {
            debug() << "Deferring finishing Account::FeatureCore until the connection is built";
        }
    } else {
        mPriv->readinessHelper->setIntrospectCompleted(FeatureCore, false, reply.error());

        warning().nospace() <<
            "GetAll(Account) failed: " <<
            reply.error().name() << ": " << reply.error().message();
    }

    watcher->deleteLater();
}

void Account::gotAvatar(QDBusPendingCallWatcher *watcher)
{
    QDBusPendingReply<QVariant> reply = *watcher;

    if (!reply.isError()) {
        debug() << "Got reply to GetAvatar(Account)";
        mPriv->avatar = qdbus_cast<Avatar>(reply);

        // It could be in either of actual or missing from the first time in corner cases like the
        // object going away, so let's be prepared for both (only checking for actualFeatures here
        // actually used to trigger a rare bug)
        //
        // Anyway, the idea is to not do setIntrospectCompleted twice
        if (!mPriv->readinessHelper->actualFeatures().contains(FeatureAvatar) &&
                !mPriv->readinessHelper->missingFeatures().contains(FeatureAvatar)) {
            mPriv->readinessHelper->setIntrospectCompleted(FeatureAvatar, true);
        }

        emit avatarChanged(mPriv->avatar);
        notify("avatar");
    } else {
        // check if the feature is already there, and for some reason retrieveAvatar
        // failed when called the second time
        if (!mPriv->readinessHelper->actualFeatures().contains(FeatureAvatar) &&
                !mPriv->readinessHelper->missingFeatures().contains(FeatureAvatar)) {
            mPriv->readinessHelper->setIntrospectCompleted(FeatureAvatar, false, reply.error());
        }

        warning().nospace() <<
            "GetAvatar(Account) failed: " <<
            reply.error().name() << ": " << reply.error().message();
    }

    watcher->deleteLater();
}

void Account::onAvatarChanged()
{
    debug() << "Avatar changed, retrieving it";
    mPriv->retrieveAvatar();
}

void Account::onConnectionManagerReady(PendingOperation *operation)
{
    bool error = operation->isError();
    if (!error) {
        error = !mPriv->cm->hasProtocol(mPriv->protocolName);
    }

    if (!error) {
        mPriv->readinessHelper->setIntrospectCompleted(FeatureProtocolInfo, true);
    }
    else {
        warning() << "Failed to find the protocol in the CM protocols for account" << objectPath();
        mPriv->readinessHelper->setIntrospectCompleted(FeatureProtocolInfo, false,
                operation->errorName(), operation->errorMessage());
    }
}

void Account::onConnectionReady(PendingOperation *op)
{
    mPriv->checkCapabilitiesChanged(false);

    /* let's not fail if connection can't become ready, the caps will still
     * work, but return the CM caps instead. Also no need to call
     * setIntrospectCompleted if the feature was already set to complete once,
     * since this method will be called whenever the account connection
     * changes */
    if (!isReady(FeatureCapabilities)) {
        mPriv->readinessHelper->setIntrospectCompleted(FeatureCapabilities, true);
    }
}

void Account::onPropertyChanged(const QVariantMap &delta)
{
    mPriv->updateProperties(delta);
}

void Account::onRemoved()
{
    mPriv->valid = false;
    mPriv->enabled = false;
    invalidate(QLatin1String(TELEPATHY_QT4_ERROR_OBJECT_REMOVED),
            QLatin1String("Account removed from AccountManager"));
    emit removed();
}

void Account::onConnectionBuilt(PendingOperation *op)
{
    PendingReady *readyOp = qobject_cast<PendingReady *>(op);
    Q_ASSERT(readyOp != NULL);

    if (op->isError()) {
        warning() << "Building connection" << mPriv->connObjPathQueue.head() << "failed with" <<
            op->errorName() << "-" << op->errorMessage();

        if (!mPriv->connection.isNull()) {
            mPriv->connection.reset();
            emit connectionChanged(mPriv->connection);
            notify("connection");
            notify("connectionObjectPath");
        }
    } else {
        ConnectionPtr prevConn = mPriv->connection;
        QString prevConnPath = mPriv->connectionObjectPath();

        mPriv->connection = ConnectionPtr::qObjectCast(readyOp->proxy());
        Q_ASSERT(mPriv->connection);

        debug() << "Connection" << mPriv->connectionObjectPath() << "built for" << objectPath();

        if (prevConn != mPriv->connection) {
            notify("connection");
            emit connectionChanged(mPriv->connection);
        }

        if (prevConnPath != mPriv->connectionObjectPath()) {
            notify("connectionObjectPath");
        }
    }

    mPriv->connObjPathQueue.dequeue();

    if (mPriv->processConnQueue() && !mPriv->coreFinished && mPriv->mayFinishCore) {
        debug() << "Account" << objectPath() << "basic functionality is ready (connections built)";
        mPriv->coreFinished = true;
        mPriv->readinessHelper->setIntrospectCompleted(FeatureCore, true);
    }
}

} // Tp
