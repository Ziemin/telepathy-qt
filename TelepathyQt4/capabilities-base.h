/*
 * This file is part of TelepathyQt4
 *
 * Copyright (C) 2009-2010 Collabora Ltd. <http://www.collabora.co.uk/>
 * Copyright (C) 2009-2010 Nokia Corporation
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

#ifndef _TelepathyQt4_capabilities_base_h_HEADER_GUARD_
#define _TelepathyQt4_capabilities_base_h_HEADER_GUARD_

#ifndef IN_TELEPATHY_QT4_HEADER
#error IN_TELEPATHY_QT4_HEADER
#endif

#include <TelepathyQt4/RequestableChannelClassSpec>
#include <TelepathyQt4/Types>

namespace Tp
{

class TELEPATHY_QT4_EXPORT CapabilitiesBase
{
public:
    CapabilitiesBase();
    CapabilitiesBase(const CapabilitiesBase &other);
    virtual ~CapabilitiesBase();

    CapabilitiesBase &operator=(const CapabilitiesBase &other);

    RequestableChannelClassSpecList allClassSpecs() const;

    TELEPATHY_QT4_DEPRECATED RequestableChannelClassList requestableChannelClasses() const;

    bool isSpecificToContact() const;

    bool textChats() const;

    bool streamedMediaCalls() const;
    bool streamedMediaAudioCalls() const;
    bool streamedMediaVideoCalls() const;
    bool streamedMediaVideoCallsWithAudio() const;
    bool upgradingStreamedMediaCalls() const;

    TELEPATHY_QT4_DEPRECATED bool supportsTextChats() const;

    TELEPATHY_QT4_DEPRECATED bool supportsMediaCalls() const;
    TELEPATHY_QT4_DEPRECATED bool supportsAudioCalls() const;
    TELEPATHY_QT4_DEPRECATED bool supportsVideoCalls(bool withAudio = true) const;
    TELEPATHY_QT4_DEPRECATED bool supportsUpgradingCalls() const;

    // later: FIXME TODO why not now‽
    // bool fileTransfers() const;
    // QList<FileHashType> fileTransfersRequireHash() const;
    //
    // bool streamTubes() const;
    // bool dBusTubes() const;

protected:
    CapabilitiesBase(bool specificToContact);
    CapabilitiesBase(const RequestableChannelClassList &rccs,
            bool specificToContact);
    CapabilitiesBase(const RequestableChannelClassSpecList &rccSpecs,
            bool specificToContact);

    virtual void updateRequestableChannelClasses(
            const RequestableChannelClassList &rccs);

private:
    friend class Connection;
    friend class Contact;

    struct Private;
    friend struct Private;
    // FIXME (API/ABI break) Make mPriv a QSharedDataPointer
    Private *mPriv;
};

} // Tp

Q_DECLARE_METATYPE(Tp::CapabilitiesBase);

#endif
