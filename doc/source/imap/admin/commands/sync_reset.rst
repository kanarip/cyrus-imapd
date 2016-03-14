.. _imap-admin-commands-sync_reset:

==============
**sync_reset**
==============

Account reset utility. DANGER.

Synopsis
========

.. parsed-literal::

    **sync_reset** [ **-C** *config-file* ] [ **-v** ] [ **-f** ]

Description
===========

**sync_reset** is a small utility program to destroy user accounts on a
system.  The only safeguard which is in place is the obligary force
option.

**sync_reset** |default-conf-text|

Options
=======

.. program:: sync_reset

.. option:: -C config-file

    |cli-dash-c-text|

.. option:: -v

  Verbose mode.

.. option:: -f

  Force operation. Without this flag **sync_reset** just bails out with
  an error.  Principally here to try and prevent accidents with command
  autorepeat.

Files
=====

/etc/imapd.conf

See Also
========

:manpage:`imapd.conf(5)`
