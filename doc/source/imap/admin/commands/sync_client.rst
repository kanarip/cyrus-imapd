.. _imap-admin-commands-sync_client:

===============
**sync_client**
===============

Client side of the synchronization (replication) engine

Synopsis
========

.. parsed-literal::

    **sync_client** [ **-v** ] [ **-l** ] [ **-L** ] [ **-z** ] [ **-C** *config-file* ] [ **-S** *server-name* ]
        [ **-f** *input-file* ] [ **-F** *shutdown_file* ] [ **-w** *wait_interval* ]
        [ **-t** *timeout* ] [ **-d** *delay* ] [ **-r** ] [ **-n** *channel* ] [ **-u** ] [ **-m** ]
        [ **-A** ] [ **-s** ] *objects*...

Description
===========

**sync_client** is the client side of the replication system.  It runs
on the client (master) system and connects to the target (replica)
system and generates an appropriate sequence of transactions to
synchronize the replica system with the master system.

**sync_client** |default-conf-text|

Options
=======

.. program:: sync_client

.. option:: -C config-file

    |cli-dash-c-text|

.. option:: -v

    Verbose mode.  Use twice (**-v -v**) to log all protocol traffic to
    stderr.

.. option:: -l

    Verbose logging mode.

.. option:: -L

    Perform only local mailbox operations (do not do mupdate operations).
    |v3-new-feature|

.. option:: -o

    Only attempt to connect to the backend server once rather than
    waiting up to 1000 seconds before giving up.

.. option:: -z

    Require compression.
    The replication protocol will always try to enable deflate
    compression if both ends support it.  Set this flag when you want
    to abort if compression is not available.

.. option:: -S servername

    Tells **sync_client** with which server to communicate.  Overrides
    the ``sync_host`` configuration option.

.. option:: -f input-file

    In mailbox or user replication mode: provides list of users or
    mailboxes to replicate.  In rolling replication mode, specifies an
    alternate log file (**sync_client** will exit after processing the
    log file).

.. option:: -F shutdown-file

    Rolling replication checks for this file at the end of each
    replication cycle and shuts down if it is present. Used to request
    a nice clean shutdown at the first convenient point. The file is
    removed on shutdown. Overrides ``sync_shutdown_file`` option in
    :manpage:`imapd.conf(5)`.

.. option:: -w interval

    Wait this long before starting. This option is typically used so
    that we can attach a debugger to one end of the replication system
    or the other.

.. option:: -t timeout

    Timeout for single replication run in rolling replication.
    **sync_client** will negotiate a restart after this many seconds.
    Default: 600 seconds

.. option:: -d delay

    Minimum delay between replication runs in rolling replication mode.
    Larger values provide better efficiency as transactions can be
    merged. Smaller values mean that the replica system is more up to
    date and that you don't end up with large blocks of replication
    transactions as a single group. Default: 3 seconds.

.. option:: -r

    Rolling (repeat) replication mode. Pick up a list of actions
    recorded by the :manpage:`lmtpd(8)`, :manpage:`imapd(8)`,
    :manpage:`popd(8)` and :manpage:`nntpd(8)` daemons from the file
    specified in ``sync_log_file``. Repeat until ``sync_shutdown_file``
    appears.

.. option:: -n channel

    Use the named channel for rolling replication mode.  If multiple
    channels are specified in ``sync_log_channels`` then use one of them.
    This option is probably best combined with **-S** to connect to a
    different server with each channel.

.. option:: -u

    User mode.
    Remaining arguments are list of users who should be replicated.

.. option:: -A

    All users mode.
    Sync every user on the server to the replica (doesn't do non-user
    mailboxes at all... this could be considered a bug and maybe it
    should do those mailboxes independently)

.. option:: -m

    Mailbox mode.
    Remaining arguments are list of mailboxes which should be replicated.

.. option:: -s

    Sieve mode.
    Remaining arguments are list of users whose Sieve files should be
    replicated. Principally used for debugging purposes: not exposed to
    :manpage:`sync_client(8)`.

Examples
========

On a replication master, the following would be added to the START
section of :manpage:`cyrus.conf(5)`:

    ::

        syncclient		cmd="/usr/lib/cyrus/bin/sync_client -r"

[NB: More examples needed]

History
=======

The **-L** feature, local updates only, was added in version 3.0.

Files
=====

/etc/imapd.conf

See Also
========

:manpage:`sync_server(8)`, :manpage:`cyrus.conf(5)`,
:manpage:`imapd.conf(5)`, :manpage:`cyrus-master(8)`
