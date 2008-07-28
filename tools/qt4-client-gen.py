#!/usr/bin/python
#
# Copyright (C) 2008 Collabora Limited <http://www.collabora.co.uk>
# Copyright (C) 2008 Nokia Corporation
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

from sys import argv
import xml.dom.minidom
import codecs
from getopt import gnu_getopt

from libtpcodegen import NS_TP, get_descendant_text, get_by_path
from libqt4codegen import binding_from_usage, extract_arg_or_member_info, format_docstring, gather_externals, gather_custom_lists, get_qt4_name, qt4_identifier_escape

class Generator(object):
    def __init__(self, opts):
        try:
            self.group = opts.get('--group', 'no-group-defined')
            self.headerfile = opts['--headerfile']
            self.implfile = opts['--implfile']
            self.namespace = opts['--namespace']
            self.typesnamespace = opts['--typesnamespace']
            self.realinclude = opts['--realinclude']
            self.prettyinclude = opts['--prettyinclude']
            self.typesinclude = opts['--typesinclude']
            self.mainiface = opts.get('--mainiface', None)
            ifacedom = xml.dom.minidom.parse(opts['--ifacexml'])
            specdom = xml.dom.minidom.parse(opts['--specxml'])
        except KeyError, k:
            assert False, 'Missing required parameter %s' % k.args[0]

        self.hs = []
        self.bs = []
        self.ifacenodes = ifacedom.getElementsByTagName('node')
        self.spec, = get_by_path(specdom, "spec")
        self.custom_lists = gather_custom_lists(self.spec, self.typesnamespace)
        self.externals = gather_externals(self.spec)
        self.mainifacename = self.mainiface and self.mainiface.replace('/', '').replace('_', '') + 'Interface'

    def __call__(self):
        # Output info header and includes
        self.h("""\
/*
 * This file contains D-Bus client proxy classes generated by qt4-client-gen.py.
 *
 * This file can be distributed under the same terms as the specification from
 * which it was generated.
 */

#include <QString>
#include <QObject>
#include <QVariant>

#include <QtGlobal>
#include <QtDBus>

#include <%s>

""" % self.typesinclude)

        self.b("""\
#include <%s>

""" % self.realinclude)

        # Begin namespace
        for ns in self.namespace.split('::'):
            self.hb("""\
namespace %s
{
""" % ns)

        # Output interface proxies
        def ifacenodecmp(x, y):
            xname, yname = x.getAttribute('name'), y.getAttribute('name')

            if xname == self.mainiface:
                return -1
            elif yname == self.mainiface:
                return 1
            else:
                return cmp(xname, yname)

        self.ifacenodes.sort(cmp=ifacenodecmp)
        for ifacenode in self.ifacenodes:
            self.do_ifacenode(ifacenode)

        # End namespace
        self.hb(''.join(['}\n' for ns in self.namespace.split('::')]))

        # Write output to files
        (codecs.getwriter('utf-8')(open(self.headerfile, 'w'))).write(''.join(self.hs))
        (codecs.getwriter('utf-8')(open(self.implfile, 'w'))).write(''.join(self.bs))

    def do_ifacenode(self, ifacenode):
        # Extract info
        name = ifacenode.getAttribute('name').replace('/', '').replace('_', '') + 'Interface'
        iface, = get_by_path(ifacenode, 'interface')
        dbusname = iface.getAttribute('name')

        # Begin class, constructors
        self.h("""
/**
 * \\class %(name)s
 * \\headerfile %(realinclude)s <%(prettyinclude)s>
 * \\ingroup %(group)s
 *
 * Proxy class providing a 1:1 mapping of the D-Bus interface "%(dbusname)s."
 */
class %(name)s : public QDBusAbstractInterface
{
    Q_OBJECT

public:
    /**
     * Returns the name of the interface "%(dbusname)s", which this class
     * represents.
     *
     * \\return The D-Bus interface name.
     */
    static inline const char *staticInterfaceName()
    {
        return "%(dbusname)s";
    }

    /**
     * Creates a %(name)s associated with the given object on the session bus.
     *
     * \\param serviceName Name of the service the object is on.
     * \\param objectPath Path to the object on the service.
     * \\param parent Passed to the parent class constructor.
     */
    explicit %(name)s(
        const QString& serviceName,
        const QString& objectPath,
        QObject* parent = 0
    );

    /**
     * Creates a %(name)s associated with the given object on the given bus.
     *
     * \\param connection The bus via which the object can be reached.
     * \\param serviceName Name of the service the object is on.
     * \\param objectPath Path to the object on the service.
     * \\param parent Passed to the parent class constructor.
     */
    explicit %(name)s(
        const QDBusConnection& connection,
        const QString& serviceName,
        const QString& objectPath,
        QObject* parent = 0
    );
""" % {'name' : name,
       'realinclude' : self.realinclude,
       'prettyinclude' : self.prettyinclude,
       'group' : self.group,
       'dbusname' : dbusname})

        self.b("""
%(name)s::%(name)s(const QString& serviceName, const QString& objectPath, QObject *parent)
    : QDBusAbstractInterface(serviceName, objectPath, staticInterfaceName(), QDBusConnection::sessionBus(), parent)
{
}

%(name)s::%(name)s(const QDBusConnection& connection, const QString& serviceName, const QString& objectPath, QObject *parent)
    : QDBusAbstractInterface(serviceName, objectPath, staticInterfaceName(), connection, parent)
{
}
""" % {'name' : name})

        # Main interface
        mainifacename = self.mainifacename or 'QDBusAbstractInterface'

        if mainifacename != name:
            self.h("""
    /**
     * Creates a %(name)s associated with the same object as the given proxy.
     * Additionally, the created proxy will have the same parent as the given
     * proxy.
     *
     * \\param mainInterface The proxy to use.
     */
    explicit %(name)s(const %(mainifacename)s& mainInterface);

    /**
     * Creates a %(name)s associated with the same object as the given proxy.
     * However, a different parent object can be specified.
     *
     * \\param mainInterface The proxy to use.
     * \\param parent Passed to the parent class constructor.
     */
    explicit %(name)s(const %(mainifacename)s& mainInterface, QObject* parent);
""" % {'name' : name,
       'mainifacename' : mainifacename})

            self.b("""
%(name)s::%(name)s(const %(mainifacename)s& mainInterface)
    : QDBusAbstractInterface(mainInterface.service(), mainInterface.path(), staticInterfaceName(), mainInterface.connection(), mainInterface.parent())
{
}

%(name)s::%(name)s(const %(mainifacename)s& mainInterface, QObject *parent)
    : QDBusAbstractInterface(mainInterface.service(), mainInterface.path(), staticInterfaceName(), mainInterface.connection(), parent)
{
}
""" % {'name' : name,
       'mainifacename' : mainifacename})

        # Properties
        for prop in get_by_path(iface, 'property'):
            # Skip tp:properties
            if not prop.namespaceURI:
                self.do_prop(prop)

        # Methods
        methods = get_by_path(iface, 'method')

        if methods:
            self.h("""
public Q_SLOTS:\
""")

            for method in methods:
                self.do_method(method)

        # Signals
        signals = get_by_path(iface, 'signal')

        if signals:
            self.h("""
Q_SIGNALS:\
""")

            for signal in signals:
                self.do_signal(signal)

        # Close class
        self.h("""\
};
""")

    def do_prop(self, prop):
        name = prop.getAttribute('name')
        access = prop.getAttribute('access')
        gettername = name
        settername = None

        sig = prop.getAttribute('type')
        tptype = prop.getAttributeNS(NS_TP, 'type')
        binding = binding_from_usage(sig, tptype, self.custom_lists, (sig, tptype) in self.externals, self.typesnamespace)

        if 'write' in access:
            settername = 'set' + name

        self.h("""
    /**
     * Represents property "%(name)s" on the remote object.
%(docstring)s\
     */
    Q_PROPERTY(%(val)s %(name)s READ %(gettername)s%(maybesettername)s)

    /**
     * Getter for the remote object property "%(name)s".
     *
     * \\return The value of the property, or a default-constructed value
     *          if the property is not readable.
     */
    inline %(val)s %(gettername)s() const
    {
        return %(getter-return)s;
    }
""" % {'name' : name,
       'docstring' : format_docstring(prop, '     * '),
       'val' : binding.val,
       'name' : name,
       'gettername' : gettername,
       'maybesettername' : settername and (' WRITE ' + settername) or '',
       'getter-return' : 'read' in access and ('qvariant_cast<%s>(internalPropGet("%s"))' % (binding.val, name)) or binding.val + '()'})

        if settername:
            self.h("""
    /**
     * Setter for the remote object property "%s".
     *
     * \\param newValue The value to set the property to.
     */
    inline void %s(%s newValue)
    {
        internalPropSet("%s", QVariant::fromValue(newValue));
    }
""" % (name, settername, binding.inarg, name))

    def do_method(self, method):
        name = method.getAttribute('name')
        args = get_by_path(method, 'arg')
        argnames, argdocstrings, argbindings = extract_arg_or_member_info(args, self.custom_lists, self.externals, self.typesnamespace, '     *     ')

        inargs = []
        outargs = []

        for i in xrange(len(args)):
            if args[i].getAttribute('direction') == 'out':
                outargs.append(i)
            else:
                inargs.append(i)

        rettypes = ', '.join([argbindings[i].val for i in outargs])
        params = ', '.join([argbindings[i].inarg + ' ' + argnames[i] for i in inargs])

        self.h("""
    /**
     * Begins a call to the D-Bus method "%s" on the remote object.
%s\
""" % (name, format_docstring(method, '     * ')))

        for i in inargs:
            if argdocstrings[i]:
                self.h("""\
     *
     * \\param %s
%s\
""" % (argnames[i], argdocstrings[i]))

        for i in outargs:
            if argdocstrings[i]:
                self.h("""\
     *
     * \\return
%s\
""" % argdocstrings[i])

        self.h("""\
     */
    inline QDBusPendingReply<%(rettypes)s> %(name)s(%(params)s)
    {\
""" % {'rettypes' : rettypes,
       'name' : name,
       'params' : params})

        if inargs:
            self.h("""
        QList<QVariant> argumentList;
        argumentList << %s;
        return asyncCallWithArgumentList(QLatin1String("%s"), argumentList);
    }
""" % (' << '.join(['QVariant::fromValue(%s)' % argnames[i] for i in inargs]), name))
        else:
            self.h("""
        return asyncCall(QLatin1String("%s"));
    }
""" % name)

    def do_signal(self, signal):
        name = signal.getAttribute('name')
        argnames, argdocstrings, argbindings = extract_arg_or_member_info(get_by_path(signal, 'arg'), self.custom_lists, self.externals, self.typesnamespace, '     *     ')

        self.h("""
    /**
     * Represents the signal "%s" on the remote object.
%s\
""" % (name, format_docstring(signal, '     * ')))

        for i in xrange(len(argnames)):
            if argdocstrings[i]:
                self.h("""\
     *
     * \\param %s
%s\
""" % (argnames[i], argdocstrings[i]))

        self.h("""\
     */
    void %s(%s);
""" % (name, ', '.join(['%s %s' % (binding.inarg, name) for binding, name in zip(argbindings, argnames)])))

    def h(self, str):
        self.hs.append(str)

    def b(self, str):
        self.bs.append(str)

    def hb(self, str):
        self.h(str)
        self.b(str)


if __name__ == '__main__':
    options, argv = gnu_getopt(argv[1:], '',
            ['group=',
             'namespace=',
             'typesnamespace=',
             'headerfile=',
             'implfile=',
             'ifacexml=',
             'specxml=',
             'realinclude=',
             'prettyinclude=',
             'typesinclude=',
             'mainiface='])

    Generator(dict(options))()
