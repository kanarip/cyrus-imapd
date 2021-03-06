.. _imap-installation-fedora-last-stable:

Installation of Cyrus IMAP |imap_last_stable_version| on Fedora
===============================================================

.. NOTE::

    Packages for Cyrus IMAP version |imap_last_stable_version| can be
    obtained from the :ref:`imap-installation-obs`.

#.  Clone the GIT repository:

    .. parsed-literal::

        $ :command:`git clone` |git_cyrus_imapd_url|

#.  Checkout the tag for version |imap_last_stable_version| of Cyrus
    IMAP:

    .. parsed-literal::

        $ :command:`git checkout` cyrus-imapd-|imap_last_stable_version|

#.  Install the build dependencies:

    .. parsed-literal::

        # :command:`yum -y install \\
            autoconf \\
            automake \\
            bison \\
            cyrus-sasl-devel \\
            flex \\
            groff \\
            krb5-devel \\
            mysql-devel \\
            openssl-devel \\
            "perl(ExtUtils::MakeMaker)" \\
            pkgconfig \\
            postgresql-devel \\
            net-snmp-devel \\
            transfig \\
            perl-devel \\
            db4-devel \\
            openldap-devel \\
            tcp_wrappers`

#.  Execute the following commands:

    .. parsed-literal::

        $ :command:`automake -a -f -c`
        $ :command:`aclocal -I cmulocal/`
        $ :command:`autoheader`
        $ :command:`autoconf -f`
        $ :command:`./configure` [options]

    For a full list of options, see ``./configure --help``.

    .. NOTE::

        We recommend at least specifying ``--prefix=/usr``,
        ``--with-cyrus-prefix=/usr/lib/cyrus-imapd`` and
        ``--with-service-path=/usr/lib/cyrus-imapd``.

#.  Build Cyrus IMAP:

    .. parsed-literal::

        $ :command:`make`

#.  Install Cyrus IMAP (with sufficient privileges):

    .. parsed-literal::

        # :command:`make install`
