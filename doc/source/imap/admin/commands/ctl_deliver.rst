.. _imap-admin-commands-ctl_deliver:

===============
**ctl_deliver**
===============

Perform operations on the duplicate delivery database

Synopsis
========

.. parsed-literal::

    **ctl_deliver** [ **-C** *config-file* ] **-d** [ **-f** *filename* ]

Description
===========

**ctl_deliver** is used to perform various administrative operations on
the duplicate delivery database.

**ctl_deliver** |default-conf-text|
|def-confdir-text| delivery database.

Options
=======

.. program:: ctl_deliver

.. option:: -C config-file

    |cli-dash-c-text|

.. option:: -d

    Dump the contents of the database to standard output in a portable
    flat-text format.

.. option:: -f filename

    Use the database specified by *filename* instead of the default
    (*configdirectory*/**deliver.db**).

Examples
========

.. parsed-literal::

    **ctl_deliver -d**

..

        Dump *configdirectory*/**deliver.db** to STDOUT

.. only:: html

    ::

        id: <FA6558DA-5287-41D7-BCE8-C38962096A47@gmail.com>	to: .+tech.support@.sieve.	at: 1433518227	uid: 0
        id: <FA6558DA-5287-41D7-BCE8-C38962096A47@gmail.com>	to: tech.support        	at: 1433518227	uid: 47489
        id: <cmu-lmtpd-10330-1433633682-0@imap.example.com>	to: .+tech.support@.sieve.	at: 1433633682	uid: 0
        id: <cmu-lmtpd-10330-1433633682-0@imap.example.com>	to: tech.support        	at: 1433633682	uid: 47513
        id: <wikidb.5571a7bc1bc865.04482782@smtp.example.org>	to: .+tech.support@.sieve.	at: 1433511915	uid: 0
        id: <wikidb.5571a7bc1bc865.04482782@smtp.example.org>	to: tech.support        	at: 1433511915	uid: 47481

..


.. parsed-literal::

    **ctl_deliver -d -f** */tmp/deliverdb.txt*

..

        Dump *configdirectory*/**deliver.db** to */tmp/deliverdb.txt*.

See Also
========
:manpage:`imapd.conf(5)`, :manpage:`master(8)`

Files
=====
/etc/imapd.conf,
<configdirectory>/deliver.db
