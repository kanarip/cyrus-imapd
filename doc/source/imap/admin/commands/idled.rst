.. _imap-admin-commands-idled:

=========
**idled**
=========

Provide near real-time updates for IMAP IDLE

Synopsis
========

.. parsed-literal::

    **idled** [ **-C** *config-file* ]

Description
===========

**idled** is a long lived datagram daemon which receives notifications of
mailbox changes and signals the appropriate **imapd** to report the
changes to the client.

**Idled** is usually started from :manpage:`master(8)`.

**idled** |default-conf-text|

Options
=======

.. program:: idled

.. option:: -C config-file

    |cli-dash-c-text|

Examples
========

**idled** is commonly included in the START section of
:manpage:`cyrus.conf(5)` like so::

    START {
        recover			cmd="/usr/lib/cyrus/bin/ctl_cyrusdb -r"
        idled			cmd="/usr/lib/cyrus/bin/idled"
        tlsprune		cmd="/usr/lib/cyrus/bin/tls_prune"
    }

Files
=====

/etc/imapd.conf

See Also
========

:manpage:`imapd.conf(5)`,
:manpage:`cyrus.conf(5)`,
:manpage:`master(8)`
