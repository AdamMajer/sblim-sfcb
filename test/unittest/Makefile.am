# (C) Copyright IBM Corp 2004
# All rights reserved.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  This
# file and program are licensed under a BSD style license.  See
# the Copying file included with the OpenHPI distribution for
# full licensing terms.

MAINTAINERCLEANFILES = Makefile.in

AM_LDFLAGS=-L"$(top_builddir)/.libs"

TESTS_ENVIRONMENT = SFCB_TRACE_FILE="/tmp/sfcbtracetest"

TESTS = xmlUnescape newCMPIInstance EmbeddedTests genericlist newDateTime

check_PROGRAMS = xmlUnescape newCMPIInstance EmbeddedTests genericlist newDateTime

xmlUnescape_SOURCES = xmlUnescape.c
xmlUnescape_LDADD = -lsfcCimXmlCodec -lsfcHttpAdapter

newCMPIInstance_SOURCES = newCMPIInstance.c
newCMPIInstance_LDADD = -lsfcCimXmlCodec -lsfcHttpAdapter

EmbeddedTests_SOURCES = EmbeddedTests.c 
EmbeddedTests_LDADD = -lsfcCimXmlCodec -lsfcHttpAdapter

genericlist_SOURCES = genericlist.c
genericlist_LDADD = -lsfcBrokerCore

newDateTime_SOURCES = newDateTime.c
newDateTime_LDADD = -lsfcBrokerCore
