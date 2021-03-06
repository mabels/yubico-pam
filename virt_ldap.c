/* 
 * Copyright (c) 2015 Yubico AB
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include "virt_ldap.h"

#ifndef LDAP_DEPRECATED
static int ldap_simple_bind_s(LDAP *ld, const char *who, const char *passwd) {
  struct berval creds;
  creds.bv_len = strlen(passwd);
  creds.bv_val = (char*)passwd;
  return ldap_sasl_bind_s(ld, who, NULL, &creds, NULL, NULL, NULL);
}
static int ldap_unbind_s(LDAP *ld) {
  return ldap_unbind_ext(ld, NULL, NULL);
}
static LDAP *ldap_init(const char *host, int port) {
  LDAP *ret;
  char uri[sizeof("ldap://") + strlen(host) + sizeof(":99999") + 10];
  sprintf(uri, "ldap://%s:%d", host, port);
  ldap_initialize(&ret, uri);
  return ret;
}
#endif


static VirtLdap lib_ldap = {
  &ldap_initialize,
  &ldap_init,
  &ldap_err2string,
  &ldap_set_option,
  &ldap_simple_bind_s,
  &ldap_search_ext_s,
  &ldap_first_entry,
  &ldap_first_attribute,
  &ldap_next_attribute,
  &ldap_get_values_len,
  &ldap_count_values_len,
  &ldap_value_free_len,
  &ldap_memfree,
  &ldap_msgfree,
  &ldap_unbind_s,
  &ber_free
};

static VirtLdap *running = &lib_ldap;

void v_ldap_inject(VirtLdap *target) {
  running = target;
}

int v_ldap_initialize(LDAP **ldpm, const char *uri) {
  return (running->ldap_initialize)(ldpm, uri);
}

LDAP *v_ldap_init(const char *host, int port) {
  return (running->ldap_init)(host, port);
}

char *v_ldap_err2string( int err ) {
  return (running->ldap_err2string)(err);
}

int v_ldap_set_option(LDAP *ld, int option, const void *invalue) {
  return (running->ldap_set_option)(ld, option, invalue);
}

int v_ldap_simple_bind_s(LDAP *ld, const char *who, const char *passwd) {
  return (running->ldap_simple_bind_s)(ld, who, passwd);
}

int v_ldap_search_ext_s(LDAP *ld, char *base, int scope, char *filter, char *attrs[], int attrsonly,
                      LDAPControl **serverctrls, LDAPControl **clientctrls, struct timeval *timeout,
                      int sizelimit, LDAPMessage **res) {
  return (running->ldap_search_ext_s)(ld, base, scope, filter, attrs, attrsonly, serverctrls, 
                                      clientctrls, timeout, sizelimit, res);
}

LDAPMessage *v_ldap_first_entry(LDAP *ld, LDAPMessage *result) {
  return (running->ldap_first_entry)(ld, result);
}

char *v_ldap_first_attribute(LDAP *ld, LDAPMessage *entry, BerElement **berptr) {
  return (running->ldap_first_attribute)(ld, entry, berptr);
}

char *v_ldap_next_attribute(LDAP *ld, LDAPMessage *entry, BerElement *ber) {
  return (running->ldap_next_attribute)(ld, entry, ber);
}

struct berval **v_ldap_get_values_len(LDAP *ld, LDAPMessage *entry, const char *attr) {
  return (running->ldap_get_values_len)(ld, entry, attr); 
}

int v_ldap_count_values_len(struct berval **vals) {
  return (running->ldap_count_values_len)(vals);
}

void v_ldap_value_free_len(struct berval **vals) {
  (running->ldap_value_free_len)(vals);
}

void v_ldap_memfree(void *p) {
  (running->ldap_memfree)(p);
}

int v_ldap_msgfree(LDAPMessage *msg ) {
  return (running->ldap_msgfree)(msg );
}

int v_ldap_unbind_s(LDAP *ld) {
  return (running->ldap_unbind_s)(ld);
}

void v_ber_free(BerElement *ber, int freebuf) {
  (running->ber_free)(ber, freebuf);
}

