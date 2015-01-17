
#ifdef HAVE_MEMMEM
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <libgen.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#ifndef WIN32
#include <pwd.h>
#endif


#include "util.h"
#include "jv.h"

#ifndef HAVE_MKSTEMP
int mkstemp(char *template) {
  size_t len = strlen(template);
  int tries=5;
  int fd;

  // mktemp() truncates template when it fails
  char *s = alloca(len + 1);
  assert(s != NULL);
  strcpy(s, template);

  do {
    // Restore template
    strcpy(template, s);
    (void) mktemp(template);
    fd = open(template, O_CREAT | O_EXCL | O_RDWR, 0600);
  } while (fd == -1 && tries-- > 0);
  return fd;
}
#endif

jv expand_path(jv path) {
  assert(jv_get_kind(path) == JV_KIND_STRING);
  const char *pstr = jv_string_value(path);
  jv ret = path;
  if (jv_string_length_bytes(jv_copy(path)) > 1 && pstr[0] == '~' && pstr[1] == '/') {
    jv home = get_home();
    if (jv_is_valid(home)) {
      ret = jv_string_fmt("%s/%s",jv_string_value(home),pstr+2);
      jv_free(home);
    } else {
      jv emsg = jv_invalid_get_msg(home);
      ret = jv_invalid_with_msg(jv_string_fmt("Could not expand %s. (%s)", pstr, jv_string_value(emsg)));
      jv_free(emsg);
    }
    jv_free(path);
  }
  return ret;
}

jv get_home() {
  jv ret;
  char *home = getenv("HOME");
  if (!home) {
#ifndef WIN32
    struct passwd* pwd = getpwuid(getuid());
    if (pwd) 
      ret = jv_string(pwd->pw_dir);
    else
      ret = jv_invalid_with_msg(jv_string("Could not find home directory."));
#else
    home = getenv("USERPROFILE");
    if (!home) {
      char *hd = getenv("HOMEDRIVE");
      if (!hd) hd = "";
      home = getenv("HOMEPATH");
      if (!home) {
        ret = jv_invalid_with_msg(jv_string("Could not find home directory."));
      } else {
        ret = jv_string_fmt("%s%s",hd,home);
      }
    } else {
      ret = jv_string(home);
    }
#endif
  } else {
    ret = jv_string(home);
  }
  return ret;
}

jv jq_find_package_root(jv start_path) {
  // TODO: keep the string ".jq" elsewhere.
  jv jq_metadata_subfolder = jv_string(".jq");
  char *current_path = strdup(jv_string_value(start_path));
  struct stat st;
  int stat_ret;
  int dirname_calls = 0;
  int done = 0;
  jv ret;

  stat_ret = stat(current_path, &st);

  if (stat_ret < 0 && errno == ENOENT) {
    ret = jv_invalid_with_msg(jv_string_fmt("Could not find package root from non-existent start path '%s' ('%s')", jv_string_value(start_path), jv_string_value(jq_realpath(start_path))));
    free(current_path);
  } else {
    if (S_ISDIR(st.st_mode) == 0) {
      // Get parent folder path. (These following lines have been duplicated elsewhere in this function.)
      char *tmp_path = dirname(current_path);
      dirname_calls++;
      if (dirname_calls == 1) {
        // Both dirname() and basename() return pointers to null-terminated strings. (Do not pass these pointers to free(3).)
        // This means free(current_path) should only be called before being replaced for the first time, and never on tmp_path.
        free(current_path);
      }
      current_path = tmp_path;
    }

    do {
      // TODO: detect windows disk/file system roots such as "C:\".
      if (strcmp(current_path,"/") == 0){
        break;
      }

      // Check if ${current or parent dir}/.jq/ directory exists.
      jv jq_metadata_subfolder_path = jv_string_fmt("%s%s%s",
                                                 current_path,
                                                 // TODO: detect windows disk/file system roots such as "C:\".
                                                 strcmp(current_path,"/") == 0 ? "" : "/",
                                                 jv_string_value(jq_metadata_subfolder));
      stat_ret = stat(jv_string_value(jq_metadata_subfolder_path), &st);
      jv_free(jq_metadata_subfolder_path);
      if (stat_ret == 0 && S_ISDIR(st.st_mode) != 0) {
        // Package root found.
        done = 1;
        ret = jv_string(current_path);
      } else {
        // Get parent folder path. (These following lines have been duplicated elsewhere in this function.)
        char *tmp_path = dirname(current_path);
        dirname_calls++;
        if (dirname_calls == 1) {
          // Both dirname() and basename() return pointers to null-terminated strings. (Do not pass these pointers to free(3).)
          // This means free(current_path) should only be called before being replaced for the first time, and never on tmp_path.
          free(current_path);
        }
        current_path = tmp_path;
      }
    } while(done != 1);
  }

  if (jv_get_kind(ret) != JV_KIND_STRING) {
    ret = jv_invalid_with_msg(jv_string_fmt("Could not find package root from start path '%s' ('%s')", jv_string_value(start_path), jv_string_value(jq_realpath(start_path))));
  }

  jv_free(jq_metadata_subfolder);
  return ret;
}

jv jq_realpath(jv path) {
  int path_max;
  char *buf = NULL;
#ifdef _PC_PATH_MAX
  path_max = pathconf(jv_string_value(path),_PC_PATH_MAX);
#else
  path_max = PATH_MAX;
#endif
  if (path_max > 0) {
     buf = malloc(sizeof(char) * path_max);
  }
#ifdef WIN32
  char *tmp = _fullpath(buf, jv_string_value(path), path_max);
#else
  char *tmp = realpath(jv_string_value(path), buf);
#endif
  if (tmp == NULL) {
    free(buf);
    return path;
  }
  jv_free(path);
  path = jv_string(tmp);
  free(tmp);
  return path;
}

const void *_jq_memmem(const void *haystack, size_t haystacklen,
                       const void *needle, size_t needlelen) {
#ifdef HAVE_MEMMEM
  return (const void*)memmem(haystack, haystacklen, needle, needlelen);
#else
  const char *h = haystack;
  const char *n = needle;
  size_t hi, hi2, ni;

  if (haystacklen < needlelen || haystacklen == 0)
    return NULL;
  for (hi = 0; hi < (haystacklen - needlelen + 1); hi++) {
    for (ni = 0, hi2 = hi; ni < needlelen; ni++, hi2++) {
      if (h[hi2] != n[ni])
        goto not_this;
    }

    return &h[hi];

not_this:
    continue;
  }
  return NULL;
#endif /* !HAVE_MEMMEM */
}

