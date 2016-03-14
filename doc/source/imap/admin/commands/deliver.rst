.. _imap-admin-commands-deliver:

===========
**deliver**
===========

intro

Synopsis
========

.. parsed-literal::

    **deliver** [ **-C** *config-file* ] [ **-d** ] [ **-r** *address* ]
           [ **-f** *address* ] [ **-m** *mailbox* ] [ **-a** *auth-id* ]
           [ **-q** ] [ *userid* ]...
    **deliver** [ **-C** *config-file* ] **-l**

Description
===========

**deliver** reads a message from the standard input and delivers it to
one or more IMAP mailboxes.


**deliver** |default-conf-text|

Options
=======

.. program:: deliver

.. option:: -C config-file

    |cli-dash-c-text|


.. option:: -d

    Ignored for compatability with **/bin/mail**.

.. option:: -r  address

    Insert a **Return-Path:** header containing *address*.

.. option:: -f  address

    Insert a **Return-Path:** header containing *address*.

.. option:: -m  mailbox

    Deliver to **mailbox**.  If any *userid*\ s are specified, attempts
    to deliver to ``user.``\ *userid*\ ``.mailbox`` for each *userid*\ .
    If the ACL on any such mailbox does not grant the sender the "p"
    right or if **-m** is not specified, then delivers to the INBOX for
    the *userid*, regardless of the ACL on the INBOX.

    If no *userid*\ s are specified, attempts to deliver to *mailbox*\
    . If the ACL on *mailbox* does not grant the sender the "p" right,
    the delivery fails.

.. option:: -a  auth-id

    Specify the authorization id of the sender.  Defaults to "anonymous".

.. option:: -q  user-id

    Deliver message even when receiving mailbox is over quota.

.. option:: -l

    Accept messages using the LMTP protocol.

NOTES
=====

Depending on the setting of ``reject8bit`` in :manpage:`imapd.conf(5)`, deliver
either rejects/accepts messages with 8-bit-set characters in the headers.
If we accept messages with 8-bit-set characters in the headers, then depending
on the setting of ``munge8bit``, these characters are either left un-touched
or changed to "X".

This is because such characters can't be interpreted since the
character set is not known, although some communities not well-served by
US-ASCII assume that those characters can be used to represent characters not
present in US-ASCII.

A method for encoding 8-bit-set characters is provided by :rfc:`2047`.

Examples
========

[NB: Examples needed]

Files
=====

/etc/imapd.conf

See Also
========
:manpage:`lmtpd(8)`
