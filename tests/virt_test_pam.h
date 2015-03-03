
static struct TestPam {
  const char *user;
  const char *auth_ok_password;
  const struct pam_conv *conv;
} test_pam_instance;

const char *test_pam_strerror(PAM_STRERROR_CONST pam_handle_t *_pamh, int _error_number) {
  switch (_error_number) {
    case 0:
      return "PAM is fine";
    default:
      return "really a error";
  }
}

int test_pam_get_data(const pam_handle_t *_pamh, const char *_module_data_name, const void **_data) {
  return 0;
}

int test_pam_set_data(pam_handle_t *_pamh, const char *_module_data_name, void *_data, void (*_cleanup)(pam_handle_t *_pamh,
                  void *_data, int _pam_end_status)) {
  return 0;
}

int test_pam_get_user(pam_handle_t *_pamh, const char **_user, const char *_prompt) {
  *_user = ((struct TestPam *)_pamh)->user;
  return 0;
}

int test_pam_get_item(const pam_handle_t *_pamh, int _item_type, const void **_item) {
  if (_item_type == PAM_AUTHTOK) {
    *_item = ((struct TestPam *)_pamh)->auth_ok_password;
    return 0;
  }
  if (_item_type == PAM_CONV) {
    *_item = test_pam_instance.conv;
    return 0;
  }
  return 1;
}

int test_pam_set_item(pam_handle_t *_pamh, int _item_type, const void *_item) {
  if (_item_type == PAM_AUTHTOK) {
    ((struct TestPam *)_pamh)->auth_ok_password = _item;
    return 0;
  }
  return 1;
}

int test_pam_start(const char *_service, const char *_user, const struct pam_conv *_pam_conv, pam_handle_t **_pamh) {
  test_pam_instance.user = _user;
  *_pamh = (pam_handle_t *)&test_pam_instance;
  test_pam_instance.conv = _pam_conv;
  return 0;
}

int test_pam_modutil_drop_priv(pam_handle_t *pamh, PamModutilPrivs *p, const struct passwd *pw) {
  return 0;
}

int test_pam_modutil_regain_priv(pam_handle_t *pamh, PamModutilPrivs *p) {
  return 0;
}


static VirtPam test_pam = {
  &test_pam_strerror,
  &test_pam_get_data,
  &test_pam_set_data,
  &test_pam_get_user,
  &test_pam_get_item,
  &test_pam_set_item,
  &test_pam_start,
  &test_pam_modutil_drop_priv,
  &test_pam_modutil_regain_priv
};

