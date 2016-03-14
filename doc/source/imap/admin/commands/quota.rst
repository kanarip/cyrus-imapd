.. _imap-admin-commands-quota:

=========
**quota**
=========

Report and optionally fix storage and message quota usage.

Synopsis
========

.. parsed-literal::

    **quota** [ **-C** *config-file* ] [ **-d** *domain* ] [ **-f** ] [ *mailbox-prefix*... ]

Description
===========

**quota** generates a report listing quota roots, giving their limits and
usage.

If the **-f** option is specified, **quota** first fixes any
inconsistencies in the quota subsystem, such as mailboxes with the wrong
quota root or quota roots with the wrong quota usage reported.

If an optional *domain* is specified with the **-d** option, the quota
listing (and any inconsistency fixing) is performed only in that domain
rather than all mailboxes.

If one or more *mailbox-prefix* arguments are specified, the quota
listing (and inconsistency fixing) is limited to quota roots with names
that start with one of the given prefixes.

.. WARNING::

    Running **quota** with both the **-f** option and *mailbox-prefix*
    arguments is not recommended.

**quota** |default-conf-text|

Options
=======

.. program:: quota

.. option:: -C config-file

    |cli-dash-c-text|

.. option:: -d domain

    List and/or fix quota only in *domain*.

.. option:: -f

    Fix any inconsistencies in the quota subsystem before generating a
    report.

.. option:: -q

    Operate quietly. If **-f** is specified, then don't print the quota
    values, only print messages when things are changed.

.. option:: mailbox-prefix

    Only report and/or fix quota in mailboxes starting with the
    specified *mailbox-prefix*.

Examples
========

.. parsed-literal::

    **quota**

..

        List quotas for all users and mailboxes.

.. only:: html

    ::

        Quota     % Used     Used              Resource Root
        1048576       21   228429              STORAGE example.org!user.jane
                             9459              MESSAGE example.org!user.jane
                                1 X-ANNOTATION-STORAGE example.org!user.jane
                               26        X-NUM-FOLDERS example.org!user.jane
                           169791              STORAGE example.org!user.jane.Archive
                             4137              MESSAGE example.org!user.jane.Archive
                                0 X-ANNOTATION-STORAGE example.org!user.jane.Archive
                                1        X-NUM-FOLDERS example.org!user.jane.Archive
        1048576       42   448944              STORAGE example.org!user.john
                             9088              MESSAGE example.org!user.john
                                2 X-ANNOTATION-STORAGE example.org!user.john
                               35        X-NUM-FOLDERS example.org!user.john

Files
=====

/etc/imapd.conf

See Also
========

:manpage:`imapd.conf(5)`
