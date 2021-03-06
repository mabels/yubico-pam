/* Written by Simon Josefsson <simon@yubico.com>.
 * Copyright (c) 2006-2014 Yubico AB
 * Copyright (c) 2011 Tollef Fog Heen <tfheen@err.no>
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
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <syslog.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pwd.h>

#include "util.h"
#include "drop_privs.h"


#if HAVE_CR
/* for yubikey_hex_encode */
#include <yubikey.h>
/* for yubikey pbkdf2*/
#include <ykpbkdf2.h>
#endif /* HAVE_CR */


#include "virt_pam.h"
#include "virt_ldap.h"
#include "virt_ykclient.h"

#ifndef PAM_EXTERN
#ifdef PAM_STATIC
#define PAM_EXTERN static
#else
#define PAM_EXTERN extern
#endif
#endif

#define TOKEN_OTP_LEN 32
#define MAX_TOKEN_ID_LEN 16
#define DEFAULT_TOKEN_ID_LEN 12

enum key_mode {
  CHRESP,
  CLIENT
};

struct cfg
{
  int client_id;
  const char *client_key;
  int debug;
  int alwaysok;
  int verbose_otp;
  const char *password_prompt;
  const char *yubi_prompt;
  int try_first_pass;
  int use_first_pass;
  const char *auth_file;
  const char *capath;
  const char *url;
  const char *urllist;
  const char *ldapserver;
  const char *ldap_uri;
  int ldap_bind_no_anonymous;
  const char *ldap_bind_user_filter;
  const char *ldap_bind_user;
  const char *ldap_bind_password;
  const char *ldap_filter;
  const char *ldap_cacertfile;
  const char *ldapdn;
  const char *user_attr;
  const char *yubi_attr;
  const char *yubi_attr_prefix;
  int token_id_length;
  enum key_mode mode;
  const char *chalresp_path;
};

struct YubiPassword {
  char *password;
  char *otp;
  char *otp_id;
};


#ifdef DBG
#undef DBG
#endif
#define DBG(x) if (cfg->debug) { D(x); }
#define CFGDUMP(x) if (cfg->debug&0x2) { D(x); }

static void str_free(char *a)  {
  memset(a, 0, strlen(a));
  free(a);
}


/*
 * Authorize authenticated OTP_ID for login as USERNAME using
 * AUTHFILE.  Return -2 if the user is unknown, -1 if the OTP_ID does not match,  0 on internal failures, otherwise success.
 */
static int
authorize_user_token (YubiMem *ym, struct cfg *cfg,
		      const char *username,
		      const char *otp_id,
		      pam_handle_t *pamh)
{
  int retval;

  if (cfg->auth_file)
    {
      /* Administrator had configured the file and specified is name
         as an argument for this module.
       */
      DBG (("Using system-wide auth_file %s", cfg->auth_file));
      retval = check_user_token (cfg->auth_file, username, otp_id, cfg->debug);
    }
  else
    {
      char *userfile = NULL;
      struct passwd *p;
      PAM_MODUTIL_DEF_PRIVS(privs);

      p = getpwnam (username);
      if (p == NULL) {
	DBG (("getpwnam: %s", strerror(errno)));
	return 0;
      }

      /* Getting file from user home directory
         ..... i.e. ~/.yubico/authorized_yubikeys
       */
      if (! get_user_cfgfile_path (ym, NULL, "authorized_yubikeys", username, &userfile)) {
	D (("Failed figuring out per-user cfgfile"));
	return 0;
      }

      DBG (("Dropping privileges"));
      if(v_pam_modutil_drop_priv(pamh, &privs, p)) {
        DBG (("could not drop privileges"));
	retval = 0;
	goto free_out;
      }

      retval = check_user_token (userfile, username, otp_id, cfg->debug);

      if(v_pam_modutil_regain_priv(pamh, &privs)) {
        DBG (("could not restore privileges"));
        retval = 0;
        goto free_out;
      }
free_out:
      DBG(("jump"));
    }

  return retval;
}

/*
 * This function will look in ldap id the token correspond to the
 * requested user. It will returns 0 for failure and 1 for success.
 *
 * ldaps is only supported for ldap_uri based connections.
 * ldap_cacertfile usually needs to be set for this to work.
 *
 * ldap serve can be on a remote host.
 *
 * You need the following parameters in you pam config:
 * ldapserver=  OR ldap_uri=
 * ldapdn=
 * user_attr=
 * yubi_attr=
 *
 * If using ldap_uri, you can specify multiple failover hosts
 * eg.
 * ldap_uri=ldaps://host1.fqdn.example.com,ldaps://host2.fqdn.example.com
 */
static int
authorize_user_token_ldap (YubiMem *ym, struct cfg *cfg,
			   const char *user, struct YubiPassword *yubipw)
{
  int retval = 0;
  int protocol;
#ifdef HAVE_LIBLDAP
  int yubi_attr_prefix_len = 0;
  LDAP *ld = NULL;
  LDAPMessage *result = NULL, *e;
  BerElement *ber;
  char *a;
  char *attrs[2] = {NULL, NULL};

  struct berval **vals;
  int i, rc;

  char *filter = NULL;
  char *find = NULL;
  int scope = LDAP_SCOPE_BASE;
#endif
  DBG(("called"));
#ifdef HAVE_LIBLDAP
  if (cfg->yubi_attr == NULL) {
    DBG (("Trying to look up user to YubiKey mapping in LDAP, but yubi_attr not set!"));
    return 0;
  }
  if (cfg->ldapdn == NULL) {
    DBG (("Trying to look up user to YubiKey mapping in LDAP, but ldapdn not set!"));
    return 0;
  }

  /* Get a handle to an LDAP connection. */
  if (cfg->ldap_uri)
    {
      rc = v_ldap_initialize (&ld, cfg->ldap_uri);
      if (rc != LDAP_SUCCESS)
	{
	  DBG (("ldap_initialize: %s", v_ldap_err2string (rc)));
	  retval = 0;
	  goto done;
	}
    }
  else
    {
      if ((ld = v_ldap_init (cfg->ldapserver, LDAP_PORT)) == NULL)
	{
	  DBG (("ldap_init"));
	  retval = 0;
	  goto done;
	}
    }

  /* LDAPv2 is historical -- RFC3494. */
  v_ldap_set_option(ld, LDAP_OPT_REFERRALS, LDAP_OPT_OFF);
  protocol = LDAP_VERSION3;
  v_ldap_set_option (ld, LDAP_OPT_PROTOCOL_VERSION, &protocol);

  if (cfg->ldap_uri && cfg->ldap_cacertfile) {
    /* Set CA CERTFILE.  This makes ldaps work when using ldap_uri */
    v_ldap_set_option (0, LDAP_OPT_X_TLS_CACERTFILE, cfg->ldap_cacertfile);
  }
  /* Bind anonymously to the LDAP server. */
  if (cfg->ldap_bind_user && cfg->ldap_bind_password) {
    DBG (("try bind with: %s:[XXXX]", cfg->ldap_bind_user));
    rc = v_ldap_simple_bind_s (ld, cfg->ldap_bind_user, cfg->ldap_bind_password);
  } else if (cfg->ldap_bind_no_anonymous) {
    char *tmp_user;
    if (cfg->ldap_bind_user_filter) {
	tmp_user = filter_printf(ym, cfg->ldap_bind_user_filter, user);
    } else {
	tmp_user = y_strdup(ym, user);
    }
    DBG (("try bind with: %s:[%s]", tmp_user, yubipw->password));
    rc = v_ldap_simple_bind_s (ld, tmp_user, yubipw->password);
  } else {
    DBG (("try bind anonymous"));
    rc = v_ldap_simple_bind_s (ld, NULL, NULL);
  }
  if (rc != LDAP_SUCCESS)
    {
      DBG (("ldap_simple_bind_s: %s", v_ldap_err2string (rc)));
      retval = 0;
      goto done;
    }

  /* Allocation of memory for search strings depending on input size */
  if (cfg->user_attr && cfg->yubi_attr) {
    i = (strlen(cfg->user_attr) + strlen(cfg->ldapdn) + strlen(user) + 3) * sizeof(char);
    if ((find = y_alloc(ym, i)) == NULL) {
      DBG (("Failed allocating %i bytes", i));
      retval = 0;
      goto done;
    }
    sprintf (find, "%s=%s,%s", cfg->user_attr, user, cfg->ldapdn);
    filter = NULL;
  } else {
    find = y_strdup(ym, cfg->ldapdn); // allow free later-:)
  }
  if (cfg->ldap_filter) {
    filter = filter_printf(ym, cfg->ldap_filter, user);
    scope = LDAP_SCOPE_SUBTREE;
  }
  attrs[0] = (char *) cfg->yubi_attr;

  DBG(("LDAP : look up object base='%s' filter='%s', ask for attribute '%s'", find,
      filter ? filter:"(null)", cfg->yubi_attr));

  /* Search for the entry. */
  if ((rc = v_ldap_search_ext_s (ld, find, scope,
			       filter, attrs, 0, NULL, NULL, LDAP_NO_LIMIT,
			       LDAP_NO_LIMIT, &result)) != LDAP_SUCCESS)
    {
      DBG (("ldap_search_ext_s: %s", v_ldap_err2string (rc)));

      retval = 0;
      goto done;
    }

  e = v_ldap_first_entry (ld, result);
  if (e == NULL)
    {
      DBG (("No result from LDAP search"));
      retval = -2;
    }
  else
    {
      retval = -1;
      /* Iterate through each returned attribute. */
      for (a = v_ldap_first_attribute (ld, e, &ber);
	   a != NULL; a = v_ldap_next_attribute (ld, e, ber))
	{
	  if ((vals = v_ldap_get_values_len (ld, e, a)) != NULL)
	    {
	      yubi_attr_prefix_len = cfg->yubi_attr_prefix ? strlen(cfg->yubi_attr_prefix) : 0;

	      /* Compare each value for the attribute against the token id. */
	      for (i = 0; vals[i] != NULL; i++)
		{
	          DBG(("LDAP : Found %i values - checking if any of them match '%s:%s:%s'",
		       v_ldap_count_values_len(vals),
		       vals[i]->bv_val,
		       cfg->yubi_attr_prefix ? cfg->yubi_attr_prefix : "", yubipw->otp_id));

		  /* Only values containing this prefix are considered. */
		  if ((!cfg->yubi_attr_prefix || !strncmp (cfg->yubi_attr_prefix, vals[i]->bv_val, yubi_attr_prefix_len)))
		    {
		      if(!strncmp (yubipw->otp_id, vals[i]->bv_val + yubi_attr_prefix_len, strlen (yubipw->otp_id)))
		        {
		          DBG (("Token Found :: %s", vals[i]->bv_val));
		          retval = 1;
		        }
		    }
		}
	      v_ldap_value_free_len (vals);
	    }
	  v_ldap_memfree (a);
	}
      if (ber != NULL)
	  v_ber_free (ber, 0);
    }

 done:
  if (result != NULL)
    v_ldap_msgfree (result);
  if (ld != NULL)
    v_ldap_unbind_s (ld);

#else
  DBG (("Trying to use LDAP, but this function is not compiled in pam_yubico!!"));
  DBG (("Install libldap-dev and then recompile pam_yubico."));
#endif
  return retval;
}

#if HAVE_CR
static int
display_error(pam_handle_t *pamh, const char *message) {
  struct pam_conv *conv;
  const struct pam_message *pmsg[1];
  struct pam_message msg[1];
  struct pam_response *resp = NULL;
  int retval;

  retval = v_pam_get_item (pamh, PAM_CONV, (const void **) &conv);
  if (retval != PAM_SUCCESS) {
    D(("get conv returned error: %s", v_pam_strerror (pamh, retval)));
    return retval;
  }

  pmsg[0] = &msg[0];
  msg[0].msg = (char *)message;
  msg[0].msg_style = PAM_ERROR_MSG;
  retval = conv->conv(1, pmsg, &resp, conv->appdata_ptr);

  if (retval != PAM_SUCCESS) {
    D(("conv returned error: %s", v_pam_strerror (pamh, retval)));
    return retval;
  }

  D(("conv returned: '%s'", resp->resp));
  if (resp)
    {
      if (resp->resp)
        free (resp->resp);
      free (resp);
    }
  return retval;
}
#endif /* HAVE_CR */

#if HAVE_CR
static int
do_challenge_response(YubiMem *ym, pam_handle_t *pamh, struct cfg *cfg, const char *username)
{
  char *userfile = NULL, *tmpfile = NULL;
  FILE *f = NULL;
  char buf[CR_RESPONSE_SIZE + 16], response_hex[CR_RESPONSE_SIZE * 2 + 1];
  int ret, fd;

  unsigned int response_len = 0;
  YK_KEY *yk = NULL;
  CR_STATE state;

  const char *errstr = NULL;

  struct passwd *p;
  struct stat st;

  /* we must declare two sepparate privs structures as they can't be reused */
  PAM_MODUTIL_DEF_PRIVS(privs);
  PAM_MODUTIL_DEF_PRIVS(privs2);

  ret = PAM_AUTH_ERR;

  if (! init_yubikey(&yk)) {
    DBG(("Failed initializing YubiKey"));
    goto out;
  }

  if (! check_firmware_version(yk, false, true)) {
    DBG(("YubiKey does not support Challenge-Response (version 2.2 required)"));
    goto out;
  }


  if (! get_user_challenge_file (ym, yk, cfg->chalresp_path, username, &userfile)) {
    DBG(("Failed getting user challenge file for user %s", username));
    goto out;
  }

  DBG(("Loading challenge from file %s", userfile));

  p = getpwnam (username);
  if (p == NULL) {
      DBG (("getpwnam: %s", strerror(errno)));
      goto out;
  }

  /* Drop privileges before opening user file. */
  if (v_pam_modutil_drop_priv(pamh, &privs, p)) {
      DBG (("could not drop privileges"));
      goto out;
  }

  fd = open(userfile, O_RDONLY, 0);
  if (fd < 0) {
      DBG (("Cannot open file: %s (%s)", userfile, strerror(errno)));
      goto restpriv_out;
  }

  if (fstat(fd, &st) < 0) {
      DBG (("Cannot stat file: %s (%s)", userfile, strerror(errno)));
      close(fd);
      goto restpriv_out;
  }

  if (!S_ISREG(st.st_mode)) {
      DBG (("%s is not a regular file", userfile));
      close(fd);
      goto restpriv_out;
  }

  f = fdopen(fd, "r");
  if (f == NULL) {
      DBG (("fdopen: %s", strerror(errno)));
      close(fd);
      goto restpriv_out;
  }

  if (! load_chalresp_state(f, &state, cfg->debug))
    goto restpriv_out;

  if (fclose(f) < 0) {
    f = NULL;
    goto restpriv_out;
  }
  f = NULL;

  if (v_pam_modutil_regain_priv(pamh, &privs)) {
      DBG (("could not restore privileges"));
      goto out;
  }

  if (! challenge_response(yk, state.slot, state.challenge, state.challenge_len,
			   true, true, false,
			   buf, sizeof(buf), &response_len)) {
    DBG(("Challenge-response FAILED"));
    goto out;
  }

  /*
   * Check YubiKey response against the expected response
   */

  yubikey_hex_encode(response_hex, buf, response_len);
  if(state.salt_len > 0) { // the expected response has gone through pbkdf2
    YK_PRF_METHOD prf_method = {20, yk_hmac_sha1};
    yk_pbkdf2(response_hex, (unsigned char*)state.salt, state.salt_len, state.iterations,
        (unsigned char*)buf, response_len, &prf_method);
  }

  if (memcmp(buf, state.response, state.response_len) == 0) {
    ret = PAM_SUCCESS;
  } else {
    DBG(("Unexpected C/R response : %s", response_hex));
    goto out;
  }

  DBG(("Got the expected response, generating new challenge (%i bytes).", CR_CHALLENGE_SIZE));

  errstr = "Error generating new challenge, please check syslog or contact your system administrator";
  if (generate_random(state.challenge, sizeof(state.challenge))) {
    DBG(("Failed generating new challenge!"));
    goto out;
  }

  errstr = "Error communicating with Yubikey, please check syslog or contact your system administrator";
  if (! challenge_response(yk, state.slot, state.challenge, CR_CHALLENGE_SIZE,
			   true, true, false,
			   buf, sizeof(buf), &response_len)) {
    DBG(("Second challenge-response FAILED"));
    goto out;
  }

  /* There is a bug that makes the YubiKey 2.2 send the same response for all challenges
     unless HMAC_LT64 is set, check for that here */
  if (memcmp(buf, state.response, state.response_len) == 0) {
    errstr = "Same response for second challenge, YubiKey should be reconfigured with the option HMAC_LT64";
    goto out;
  }

  /* the yk_* functions leave 'junk' in errno */
  errno = 0;

  /*
   * Write the challenge and response we will expect the next time to the state file.
   */
  if (response_len > sizeof(state.response)) {
    DBG(("Got too long response ??? (%u/%lu)", response_len, (unsigned long) sizeof(state.response)));
    goto out;
  }
  memcpy (state.response, buf, response_len);
  state.response_len = response_len;

  /* point to the fresh privs structure.. */
  privs = privs2;
  /* Drop privileges before creating new challenge file. */
  if (v_pam_modutil_drop_priv(pamh, &privs, p)) {
      DBG (("could not drop privileges"));
      goto out;
  }

  /* Write out the new file */
  tmpfile = y_alloc(ym, strlen(userfile) + 1 + 4);
  if (! tmpfile)
    goto restpriv_out;
  strcpy(tmpfile, userfile);
  strcat(tmpfile, ".tmp");

  fd = open(tmpfile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
  if (fd < 0) {
      DBG (("Cannot open file: %s (%s)", tmpfile, strerror(errno)));
      goto restpriv_out;
  }

  f = fdopen(fd, "w");
  if (! f) {
    close(fd);
    goto restpriv_out;
  }

  errstr = "Error updating Yubikey challenge, please check syslog or contact your system administrator";
  if (! write_chalresp_state (f, &state))
    goto out;
  if (fclose(f) < 0) {
    f = NULL;
    goto restpriv_out;
  }
  f = NULL;
  if (rename(tmpfile, userfile) < 0) {
    goto restpriv_out;
  }

  if (v_pam_modutil_regain_priv(pamh, &privs)) {
      DBG (("could not restore privileges"));
      goto out;
  }

  DBG(("Challenge-response success!"));
  errstr = NULL;
  errno = 0;
  goto out;

restpriv_out:
  if (v_pam_modutil_regain_priv(pamh, &privs)) {
      DBG (("could not restore privileges"));
  }

 out:
  if (yk_errno) {
    if (yk_errno == YK_EUSBERR) {
      syslog(LOG_ERR, "USB error: %s", yk_usb_strerror());
      DBG(("USB error: %s", yk_usb_strerror()));
    } else {
      syslog(LOG_ERR, "Yubikey core error: %s", yk_strerror(yk_errno));
      DBG(("Yubikey core error: %s", yk_strerror(yk_errno)));
    }
  }

  if (errstr)
    display_error(pamh, errstr);

  if (errno) {
    syslog(LOG_ERR, "Challenge response failed: %s", strerror(errno));
    DBG(("Challenge response failed: %s", strerror(errno)));
  }

  if (yk)
    yk_close_key(yk);
  yk_release();

  if (f)
    fclose(f);

  free(userfile);
  free(tmpfile);
  return ret;
}
#endif /* HAVE_CR */

static void
parse_cfg (int flags, int argc, const char **argv, struct cfg *cfg)
{
  int i;

  memset (cfg, 0, sizeof(struct cfg));
  cfg->client_id = -1;
  cfg->token_id_length = DEFAULT_TOKEN_ID_LEN;
  cfg->mode = CLIENT;
  cfg->password_prompt = "password: ";
  cfg->yubi_prompt = "yubikey: ";

  for (i = 0; i < argc; i++)
    {
      if (strncmp (argv[i], "id=", 3) == 0)
	sscanf (argv[i], "id=%d", &cfg->client_id);
      if (strncmp (argv[i], "key=", 4) == 0)
	cfg->client_key = argv[i] + 4;
      if (strcmp (argv[i], "debug") == 0)
	cfg->debug = 1;
      if (strcmp (argv[i], "alwaysok") == 0)
	cfg->alwaysok = 1;
      if (strcmp (argv[i], "verbose_otp") == 0)
	cfg->verbose_otp = 1;
      if (strncmp (argv[i], "password_prompt=", sizeof("password_prompt=")-1) == 0)
	cfg->password_prompt = argv[i] + sizeof("password_prompt=")-1;
      if (strncmp (argv[i], "yubikey_prompt=", sizeof("yubikey_prompt=")-1) == 0)
	cfg->yubi_prompt = argv[i] + sizeof("yubikey_prompt=")-1;
      if (strncmp (argv[i], "ldap_uri=", 9) == 0)
	cfg->ldap_uri = argv[i] + 9;
      if (strcmp (argv[i], "try_first_pass") == 0)
	cfg->try_first_pass = 1;
      if (strcmp (argv[i], "use_first_pass") == 0)
	cfg->use_first_pass = 1;
      if (strncmp (argv[i], "authfile=", 9) == 0)
	cfg->auth_file = argv[i] + 9;
      if (strncmp (argv[i], "capath=", 7) == 0)
	cfg->capath = argv[i] + 7;
      if (strncmp (argv[i], "url=", 4) == 0)
	cfg->url = argv[i] + 4;
      if (strncmp (argv[i], "urllist=", 8) == 0)
	cfg->urllist = argv[i] + 8;
      if (strncmp (argv[i], "ldapserver=", 11) == 0)
	cfg->ldapserver = argv[i] + 11;
      if (strncmp (argv[i], "ldap_uri=", 9) == 0)
	cfg->ldap_uri = argv[i] + 9;
      if (strncmp (argv[i], "ldap_bind_no_anonymous", sizeof("ldap_bind_no_anonymous")-1) == 0)
	cfg->ldap_bind_no_anonymous = 1;
      if (strncmp (argv[i], "ldap_bind_user=", sizeof("ldap_bind_user=")-1) == 0)
	cfg->ldap_bind_user = argv[i] + sizeof("ldap_bind_user=")-1;
      if (strncmp (argv[i], "ldap_bind_user_filter=", sizeof("ldap_bind_user_filter=")-1) == 0)
	cfg->ldap_bind_user_filter = argv[i] + sizeof("ldap_bind_user_filter=")-1;
      if (strncmp (argv[i], "ldap_bind_password=", sizeof("ldap_bind_password=")-1) == 0)
	cfg->ldap_bind_password = argv[i] + sizeof("ldap_bind_password=")-1;
      if (strncmp (argv[i], "ldap_filter=", sizeof("ldap_filter=")-1) == 0)
	cfg->ldap_filter = argv[i] + sizeof("ldap_filter=")-1;
      if (strncmp (argv[i], "ldap_cacertfile=", sizeof("ldap_cacertfile=")-1) == 0)
      cfg->ldap_cacertfile = (char *) argv[i] + sizeof("ldap_cacertfile=")-1;
      if (strncmp (argv[i], "ldapdn=", 7) == 0)
	cfg->ldapdn = argv[i] + 7;
      if (strncmp (argv[i], "user_attr=", 10) == 0)
	cfg->user_attr = argv[i] + 10;
      if (strncmp (argv[i], "yubi_attr=", 10) == 0)
	cfg->yubi_attr = argv[i] + 10;
      if (strncmp (argv[i], "yubi_attr_prefix=", 17) == 0)
	cfg->yubi_attr_prefix = argv[i] + 17;
      if (strncmp (argv[i], "token_id_length=", 16) == 0)
	sscanf (argv[i], "token_id_length=%d", &cfg->token_id_length);
      if (strcmp (argv[i], "mode=challenge-response") == 0)
	cfg->mode = CHRESP;
      if (strcmp (argv[i], "mode=client") == 0)
	cfg->mode = CLIENT;
      if (strncmp (argv[i], "chalresp_path=", 14) == 0)
	cfg->chalresp_path = argv[i] + 14;
    }

  if (cfg->debug)
    {
      CFGDUMP (("called."));
      CFGDUMP (("flags %d argc %d", flags, argc));
      for (i = 0; i < argc; i++)
	CFGDUMP (("argv[%d]=%s", i, argv[i]));
      CFGDUMP (("id=%d", cfg->client_id));
      CFGDUMP (("key=%s", cfg->client_key ? cfg->client_key : "(null)"));
      CFGDUMP (("debug=%d", cfg->debug));
      CFGDUMP (("alwaysok=%d", cfg->alwaysok));
      CFGDUMP (("verbose_otp=%d", cfg->verbose_otp));
      CFGDUMP (("password_prompt=%s", cfg->password_prompt));
      CFGDUMP (("yubi_prompt=%s", cfg->yubi_prompt));
      CFGDUMP (("try_first_pass=%d", cfg->try_first_pass));
      CFGDUMP (("use_first_pass=%d", cfg->use_first_pass));
      CFGDUMP (("authfile=%s", cfg->auth_file ? cfg->auth_file : "(null)"));
      CFGDUMP (("ldapserver=%s", cfg->ldapserver ? cfg->ldapserver : "(null)"));
      CFGDUMP (("ldap_uri=%s", cfg->ldap_uri ? cfg->ldap_uri : "(null)"));
      CFGDUMP (("ldap_bind_no_anonymous=%d", cfg->ldap_bind_no_anonymous));
      CFGDUMP (("ldap_bind_user=%s", cfg->ldap_bind_user ? cfg->ldap_bind_user : "(null)"));
      CFGDUMP (("ldap_bind_password=%s", cfg->ldap_bind_password ? cfg->ldap_bind_password : "(null)"));
      CFGDUMP (("ldap_filter=%s", cfg->ldap_filter ? cfg->ldap_filter : "(null)"));
      CFGDUMP (("ldap_cacertfile=%s", cfg->ldap_cacertfile ? cfg->ldap_cacertfile : "(null)"));
      CFGDUMP (("ldapdn=%s", cfg->ldapdn ? cfg->ldapdn : "(null)"));
      CFGDUMP (("user_attr=%s", cfg->user_attr ? cfg->user_attr : "(null)"));
      CFGDUMP (("yubi_attr=%s", cfg->yubi_attr ? cfg->yubi_attr : "(null)"));
      CFGDUMP (("yubi_attr_prefix=%s", cfg->yubi_attr_prefix ? cfg->yubi_attr_prefix : "(null)"));
      CFGDUMP (("url=%s", cfg->url ? cfg->url : "(null)"));
      CFGDUMP (("urllist=%s", cfg->urllist ? cfg->urllist : "(null)"));
      CFGDUMP (("capath=%s", cfg->capath ? cfg->capath : "(null)"));
      CFGDUMP (("token_id_length=%d", cfg->token_id_length));
      CFGDUMP (("mode=%s", cfg->mode == CLIENT ? "client" : "chresp" ));
      CFGDUMP (("chalresp_path=%s", cfg->chalresp_path ? cfg->chalresp_path : "(null)"));
    }
}

static int ykclient_setup(YubiMem *ym, struct cfg *cfg, ykclient_t **ykc, size_t *templates, char **urls) {
  int rc;
  if(v_ykclient_global_init() != YKCLIENT_OK)
      {
        DBG (("Failed initializing ykclient library"));
        return  PAM_AUTHINFO_UNAVAIL;
      }
  rc = v_ykclient_init (ykc);
  if (rc != YKCLIENT_OK)
    {
      DBG (("ykclient_init() failed (%d): %s", rc, v_ykclient_strerror (rc)));
      return PAM_AUTHINFO_UNAVAIL;
    }

  rc = v_ykclient_set_client_b64 (*ykc, cfg->client_id, cfg->client_key);
  if (rc != YKCLIENT_OK)
    {
      DBG (("ykclient_set_client_b64() failed (%d): %s",
	    rc, v_ykclient_strerror (rc)));
      return PAM_AUTHINFO_UNAVAIL;
    }

  if (cfg->client_key)
    v_ykclient_set_verify_signature (*ykc, 1);

  if (cfg->capath)
    v_ykclient_set_ca_path (*ykc, cfg->capath);

  if (cfg->url)
    {
      rc = v_ykclient_set_url_template (*ykc, cfg->url);
      if (rc != YKCLIENT_OK)
	{
	  DBG (("v_ykclient_set_url_template() failed (%d): %s",
		rc, v_ykclient_strerror (rc)));
	  return PAM_AUTHINFO_UNAVAIL;
	}
    }

  if (cfg->urllist)
    {
      char *saveptr = NULL;
      char *part = NULL;
      char *tmpurl = y_strdup(ym, cfg->urllist);

      while ((part = strtok_r(*templates == 0 ? tmpurl : NULL, ";", &saveptr)))
	{
	  if(*templates == 10)
	    {
	      DBG (("maximum 10 urls supported in list."));
	      return PAM_AUTHINFO_UNAVAIL;
	    }
	  urls[*templates] = y_strdup(ym, part);
	  (*templates)++;
	}
      rc = v_ykclient_set_url_bases (*ykc, *templates, (const char **)urls);
      if (rc != YKCLIENT_OK)
	{
	  DBG (("ykclient_set_url_bases() failed (%d): %s",
		rc, v_ykclient_strerror (rc)));
	  return PAM_AUTHINFO_UNAVAIL;
	}
    }
  return PAM_SUCCESS;
}

static int ask_user_for_input(YubiMem *ym, struct cfg *cfg, pam_handle_t * pamh, const char *user, const char *template, char **password) {
  struct pam_conv *conv;
  const struct pam_message *pmsg[1];
  struct pam_message msg[1];
  struct pam_response *resp = NULL;

  int retval = v_pam_get_item(pamh, PAM_CONV, (const void **) &conv);
  if (retval != PAM_SUCCESS)
    {
      DBG (("get conv returned error: %s", v_pam_strerror (pamh, retval)));
      return retval;
    }
  pmsg[0] = &msg[0];
  msg[0].msg = filter_printf(ym, template, user);
  if (!msg[0].msg)
    {
      DBG (("filter_printf return null for %s:%s", template, user));
      return PAM_BUF_ERR;
    }
  msg[0].msg_style = cfg->verbose_otp ? PAM_PROMPT_ECHO_ON : PAM_PROMPT_ECHO_OFF;
  retval = conv->conv (sizeof(msg)/sizeof(msg[0]), pmsg, &resp, conv->appdata_ptr);
  if (retval != PAM_SUCCESS)
    {
      DBG (("conv returned error: %s", v_pam_strerror (pamh, retval)));
      return retval;
    }
  *password = y_strdup(ym, resp->resp);
  str_free(resp->resp);
  free(resp);
  return PAM_SUCCESS;
}

static int ask_password_and_otp(YubiMem *ym, struct cfg *cfg, pam_handle_t *pamh, const char *user, char **password) {
  DBG(("ask_password_and_otp:%s:%s", user, *password));
  if (*password == NULL || **password == *"") {
    int rc = ask_user_for_input(ym, cfg, pamh, user, cfg->password_prompt, password);
    if (rc != PAM_SUCCESS) {
      return rc;
    }
  }
  if (*password != NULL && strlen(*password) == (cfg->token_id_length + TOKEN_OTP_LEN)) {
    // if the password is equal the length of a token this will fail-:(
    char *pwd;
    int rc = ask_user_for_input(ym, cfg, pamh, user, cfg->password_prompt, &pwd);
    if (rc != PAM_SUCCESS) {
        return rc;
    }
    char *passwd_plus_otp = y_alloc(ym, strlen(pwd) + strlen(*password) + 1);
    strcpy(passwd_plus_otp, pwd);
    strcat(passwd_plus_otp, *password);
    *password = passwd_plus_otp;
  } else if (*password != NULL && strlen(*password) < (cfg->token_id_length + TOKEN_OTP_LEN)) {
    char *yubikey = NULL;
    int rc = ask_user_for_input(ym, cfg, pamh, user, cfg->yubi_prompt, &yubikey);
    if (rc != PAM_SUCCESS) {
      return rc;
    }
    char *passwd_plus_otp = y_alloc(ym, strlen(*password) + strlen(yubikey) + 1);
    strcpy(passwd_plus_otp, *password);
    strcat(passwd_plus_otp, yubikey);
    *password = passwd_plus_otp;
  }
  return PAM_SUCCESS;
}

static int split_password(YubiMem *ym, struct cfg *cfg, pam_handle_t *pamh, const char *password, struct YubiPassword *yubipw) {
  if (password == NULL)
    {
      DBG (("no password, giving up"));
      return PAM_AUTH_ERR;
    }

  const int password_len = strlen (password);
  if (password_len < (cfg->token_id_length + TOKEN_OTP_LEN))
    {
      DBG (("OTP too short to be considered : %i < %i", password_len, (cfg->token_id_length + TOKEN_OTP_LEN)));
      return PAM_AUTH_ERR;
    }

  /* In case the input was systempassword+YubiKeyOTP, we want to skip over
     "systempassword" when copying the token_id and OTP to separate buffers */
  int skip_bytes = password_len - (cfg->token_id_length + TOKEN_OTP_LEN);

  DBG (("Skipping first %i bytes. Length is %i, token_id set to %i and token OTP always %i.",
	skip_bytes, password_len, cfg->token_id_length, TOKEN_OTP_LEN));

  /* Copy full YubiKey output (public ID + OTP) into otp */
  yubipw->otp = y_strdup(ym, password + skip_bytes);
  yubipw->otp_id = y_alloc(ym, cfg->token_id_length+1);
  memcpy(yubipw->otp_id, yubipw->otp, cfg->token_id_length);
  yubipw->otp_id[cfg->token_id_length] = 0;
  yubipw->password = y_alloc(ym, skip_bytes+1);
  memcpy(yubipw->password, password, skip_bytes);
  yubipw->password[skip_bytes] = 0;

  DBG (("PWD:[%s]:OTP:[%s]:ID:[%s]", yubipw->password, yubipw->otp, yubipw->otp_id));
  int retval = v_pam_set_item (pamh, PAM_AUTHTOK, yubipw->password);
  if (retval != PAM_SUCCESS)
    {
      DBG (("set_item returned error: %s", v_pam_strerror(pamh, retval)));
      return retval; 
    }
  return PAM_SUCCESS;
}

PAM_EXTERN int
pam_sm_authenticate (pam_handle_t * pamh,
		     int flags, int argc, const char **argv)
{
  int retval, rc;
  const char *user = NULL;
  char *password = NULL;
  int skip_bytes = 0;
  int valid_token = 0;
  struct pam_conv *conv;
  ykclient_t *ykc = NULL;
  struct cfg cfg_st;
  struct cfg *cfg = &cfg_st; /* for DBG macro */
  char *urls[10];
  size_t templates = 0;
  struct YubiPassword yubipw;
  YubiMem *ym = y_construct();

  parse_cfg (flags, argc, argv, cfg);

  DBG (("pam_yubico version: %s", VERSION));

  if (cfg->token_id_length > MAX_TOKEN_ID_LEN)
  {
    DBG (("configuration error: token_id_length too long. Maximum acceptable value : %d", MAX_TOKEN_ID_LEN));
    retval = PAM_AUTHINFO_UNAVAIL;
    goto done;
  }

  retval = v_pam_get_user(pamh, &user, NULL);
  if (retval != PAM_SUCCESS)
    {
      DBG (("get user returned error: %s", v_pam_strerror (pamh, retval)));
      goto done;
    }
  DBG (("get user returned: %s", user));

  if (cfg->mode == CHRESP) {
#if HAVE_CR
    return do_challenge_response(ym, pamh, cfg, user);
#else
    DBG (("no support for challenge/response"));
    retval = PAM_AUTH_ERR;
    goto done;
#endif
  }

  if (cfg->try_first_pass || cfg->use_first_pass)
    {
      char *tmp;
      retval = v_pam_get_item (pamh, PAM_AUTHTOK, (const void **) &tmp);
      if (retval != PAM_SUCCESS)
	{
	  DBG (("get password returned error: %s setting ",
	      v_pam_strerror (pamh, retval)));
          if (cfg->use_first_pass) {
            goto done;
          }
          DBG (("setting empty password"));
          password = y_strdup(ym, "");
	} else {
          if (tmp == NULL) {
            password = y_strdup(ym, "");
          } else {
            password = y_strdup(ym, tmp);
          }
        }
      DBG (("get password returned: %s", password));
    }

  if (ykclient_setup(ym, cfg, &ykc, &templates, urls) != PAM_SUCCESS) {
    goto done;
  }

  if (!cfg->use_first_pass) {
    retval = ask_password_and_otp(ym, cfg, pamh, user, &password);
    if (retval != PAM_SUCCESS) {
      DBG (("ask_password_and_otp failed"));
      goto done;
    }
  }

  retval = split_password(ym, cfg, pamh, password, &yubipw);
  if (PAM_SUCCESS != retval) {
      DBG (("split_password failed"));
      goto done;
  }
  rc = v_ykclient_request (ykc, yubipw.otp);
  switch (rc)
    {
    case YKCLIENT_OK:
      break;

    case YKCLIENT_BAD_OTP:
    case YKCLIENT_REPLAYED_OTP:
      DBG (("ykclient return value (%d): %s", rc, v_ykclient_strerror (rc)));
      retval = PAM_AUTH_ERR;
      goto done;

    default:
      DBG (("ykclient return value (%d): %s", rc, v_ykclient_strerror (rc)));
      retval = PAM_AUTHINFO_UNAVAIL;
      goto done;
    }

  /* authorize the user with supplied token id */
  if (cfg->ldapserver != NULL || cfg->ldap_uri != NULL)
    valid_token = authorize_user_token_ldap (ym, cfg, user, &yubipw);
  else
    valid_token = authorize_user_token (ym, cfg, user, yubipw.otp_id, pamh);

  switch(valid_token)
    {
    case 1:
      retval = PAM_SUCCESS;
      break;
    case 0:
      DBG (("Internal error while validating user"));
      retval = PAM_AUTHINFO_UNAVAIL;
      break;
    case -1:
      DBG (("Unauthorized token for this user"));
      retval = PAM_AUTH_ERR;
      break;
    case -2:
      DBG (("Unknown user"));
      retval = PAM_USER_UNKNOWN;
      break;
    default:
      DBG (("Unhandled value for token-user validation"))
      retval = PAM_AUTHINFO_UNAVAIL;
    }

done:
  if (ykc) {
    v_ykclient_done (&ykc);
    v_ykclient_global_done();
  }
  if (cfg->alwaysok && retval != PAM_SUCCESS)
    {
      DBG (("alwaysok needed (otherwise return with %d)", retval));
      retval = PAM_SUCCESS;
    }
  DBG (("done. [%s]", v_pam_strerror (pamh, retval)));
  v_pam_set_data (pamh, "yubico_setcred_return", (void*)(intptr_t)retval, NULL);
  v_pam_set_data (pamh, "yubico_used_ldap", (void*)(intptr_t)cfg->ldap_bind_no_anonymous, NULL);
  y_release(ym);
  return retval;
}

PAM_EXTERN int
pam_sm_setcred (pam_handle_t * pamh, int flags, int argc, const char **argv)
{
  return PAM_SUCCESS;
}

PAM_EXTERN int
pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
  int use_ldap = -1;
  int rc = v_pam_get_data(pamh, "yubico_used_ldap", (const void**)&use_ldap);
  if (rc == PAM_SUCCESS && use_ldap) {
	  int retval;
	  rc = v_pam_get_data(pamh, "yubico_setcred_return", (const void**)&retval);
	  if (rc == PAM_SUCCESS && retval == PAM_SUCCESS) {
	      D (("pam_sm_acct_mgmt returing PAM_SUCCESS"));
	      return PAM_SUCCESS;
	  }
  }
  D (("pam_sm_acct_mgmt returing PAM_AUTH_ERR:%d", use_ldap));
  return PAM_AUTH_ERR;
}

PAM_EXTERN int
pam_sm_open_session(pam_handle_t *pamh, int flags,
	int argc, const char *argv[])
{

  D(("pam_sm_open_session"));
  return (PAM_SUCCESS);
}

PAM_EXTERN int
pam_sm_close_session(pam_handle_t *pamh, int flags,
	int argc, const char *argv[])
{
  D(("pam_sm_close_session"));
  return (PAM_SUCCESS);
}

PAM_EXTERN int
pam_sm_chauthtok(pam_handle_t *pamh, int flags,
	int argc, const char *argv[])
{
  D(("pam_sm_chauthtok"));
  return (PAM_SERVICE_ERR);
}


#ifdef PAM_STATIC

struct pam_module _pam_yubico_modstruct = {
  "pam_yubico",
  pam_sm_authenticate,
  pam_sm_setcred,
  pam_sm_acct_mgmt,
  pam_sm_open_session,
  pam_sm_close_session,
  pam_sm_chauthtok
};

#endif
