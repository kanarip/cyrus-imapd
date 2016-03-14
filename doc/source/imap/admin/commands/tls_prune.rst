.. _imap-admin-commands-tls_prune:

=============
**tls_prune**
=============

Prune expired sessions from the TLS sessions database

Synopsis
========

.. parsed-literal::

    tls_prune [OPTIONS]

Description
===========

    **CMD** [ **-C** *config-file* ]

Description
===========

**CMD** is used to prune expired sessions from the TLS sessions
database.  The lifetime of a TLS session is determined by the
``tls_session_timeout`` configuration option.


**CMD** |default-conf-text|

Options
=======

.. program:: CMD

.. option:: -C config-file

    |cli-dash-c-text|

Files
=====

/etc/imapd.conf

See Also
========

:manpage:`imapd.conf(5)`,
:manpage:`master(8)`
