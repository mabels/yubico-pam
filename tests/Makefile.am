# Copyright (c) 2014 Yubico AB
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#   * Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#
#   * Redistributions in binary form must reproduce the above
#      copyright notice, this list of conditions and the following
#      disclaimer in the documentation and/or other materials provided
#      with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# Self tests.

AM_LDFLAGS = -no-install
AM_CFLAGS=-I$(srcdir)/.. $(WARN_CFLAGS)
AM_CPPFLAGS = @YKPERS_CFLAGS@
test_LDADD = ../.libs/pam_yubico.o ../.libs/drop_privs.o ../libpam_util.la @LTLIBYUBIKEY@ @LTLIBYKCLIENT@ @LIBLDAP@ @LIBPAM@
util_test_LDADD = ../libpam_util.la
pam_test_LDADD = ../libpam_real.la ../libpam_util.la @LTLIBYUBIKEY@ @LTLIBYKCLIENT@ @LIBLDAP@

pam_test_CPPFLAGS = -DSRCDIR=\"$(srcdir)\"

check_PROGRAMS = test util_test pam_test
TESTS = $(check_PROGRAMS)

if ENABLE_COV
AM_LDFLAGS += --coverage
endif
