.. _imap-admin-commands-cyr_deny:

============
**cyr_deny**
============

deny users access to Cyrus services

Synopsis
========

.. parsed-literal::

    **cyr_deny** [ **-C** *config-file* ] [ **-s** *services* ] [ **-m** *message* ] *user*
    **cyr_deny** [ **-C** *config-file* ] **-a** *user*
    **cyr_deny** [ **-C** *config-file* ] **-l**

Description
===========

**cyr_deny** is used to deny individual users access to Cyrus services.
The first synopsis denies user *user* access to Cyrus services, the
second synopsis allows access again.  **cyr_deny** works by adding an
entry to the Cyrus ``user_deny.db`` database; the third synopsis lists
the entries in the database.

**cyr_deny** |default-conf-text|

Options
=======

.. program:: cyr_deny

.. option:: -C config-file

    |cli-dash-c-text|

.. option:: -a user

    Allow access to all services for user *user* (remove any entry
    from the deny database).

.. option:: -s services

    Deny access only to the given *services*, which is a
    comma-separated list of wildcard patterns.  The default is "*"
    which denies access to all services.


.. option:: -m message

    Provide a message which is sent to the user to explain why access is
    being denied.  A default message is used if none is specified.

.. option:: -l

    List the entries in the deny database.

Examples
========

[NB: Examples needed]

History
=======

|v3-new-command|

Files
=====

/etc/imapd.conf, <configdirectory>/user_deny.db

See Also
========

:manpage:`imapd.conf(5)`
