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

#ifndef __PAM_YUBICO_YUBI_LDAP_H_INCLUDED__
#define __PAM_YUBICO_YUBI_LDAP_H_INCLUDED__

#ifdef HAVE_LIBLDAP
#include <ldap.h>
#endif

typedef struct {
  int (*ldap_initialize)(LDAP **ldpm, const char *uri);
  LDAP *(*ldap_init)(const char *host, int port);
  char *(*ldap_err2string)( int err );
  int (*ldap_set_option)(LDAP *ld, int option, const void *invalue);
  int (*ldap_simple_bind_s)(LDAP *ld, const char *who, const char *passwd);
  int (*ldap_search_ext_s)(LDAP *ld, const char *base, int scope, const char *filter, char *attrs[], int attrsonly,
                        LDAPControl **serverctrls, LDAPControl **clientctrls, struct timeval *timeout,
                        int sizelimit, LDAPMessage **res);
  LDAPMessage *(*ldap_first_entry)(LDAP *ld, LDAPMessage *result);
  char *(*ldap_first_attribute)(LDAP *ld, LDAPMessage *entry, BerElement **berptr);
  char *(*ldap_next_attribute)(LDAP *ld, LDAPMessage *entry, BerElement *ber);
  struct berval **(*ldap_get_values_len)(LDAP *ld, LDAPMessage *entry, const char *attr); 
  int (*ldap_count_values_len)(struct berval **vals);
  void (*ldap_value_free_len)(struct berval **vals);
  void (*ldap_memfree)(void *p);
  int (*ldap_msgfree)(LDAPMessage *msg );
  int (*ldap_unbind_s)(LDAP *ld);
  void (*ber_free)(BerElement *ber, int freebuf);

} VirtLdap;

void v_ldap_inject(VirtLdap *target); 

int v_ldap_initialize(LDAP **ldpm, const char *uri);
LDAP *v_ldap_init(const char *host, int port);
char *v_ldap_err2string( int err );
int v_ldap_set_option(LDAP *ld, int option, const void *invalue);
int v_ldap_simple_bind_s(LDAP *ld, const char *who, const char *passwd);
int v_ldap_search_ext_s(LDAP *ld, char *base, int scope, char *filter, char *attrs[], int attrsonly,
                      LDAPControl **serverctrls, LDAPControl **clientctrls, struct timeval *timeout,
                      int sizelimit, LDAPMessage **res);
LDAPMessage *v_ldap_first_entry(LDAP *ld, LDAPMessage *result);
char *v_ldap_first_attribute(LDAP *ld, LDAPMessage *entry, BerElement **berptr);
char *v_ldap_next_attribute(LDAP *ld, LDAPMessage *entry, BerElement *ber);
struct berval **v_ldap_get_values_len(LDAP *ld, LDAPMessage *entry, const char *attr); 
void v_ldap_value_free_len(struct berval **vals);
int v_ldap_count_values_len(struct berval **vals);
void v_ldap_memfree(void *p);
int v_ldap_msgfree(LDAPMessage *msg );
int v_ldap_unbind_s(LDAP *ld);
void v_ber_free(BerElement *ber, int freebuf);

#endif
