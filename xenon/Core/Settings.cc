// __BEGIN_LICENSE__
// Copyright (C) 2006-2010 United States Government as represented by
// the Administrator of the National Aeronautics and Space Administration.
// All Rights Reserved.
// __END_LICENSE__


#include <fstream>

#include <xenon/config.h>
#include <xenon/Core/Thread.h>
#include <xenon/Core/Cache.h>
#include <xenon/Core/Settings.h>
#include <xenon/Core/ConfigParser.h>

// Boost headers
#include <boost/bind.hpp>
#include <boost/thread/xtime.hpp>

// C Standard Library headers ( for stat(2) and getpwuid() )
#include <sys/types.h>
#include <sys/stat.h>
#include <ctime>

#include <xenon/config.h>

#ifdef XENON_HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef XENON_HAVE_PWD_H
#include <pwd.h>
#endif

#ifdef WIN32
#define stat _stat
typedef struct _stat struct_stat;
#else
typedef struct stat struct_stat;
#endif

namespace xenon {

// ---------------------------------------------------
// Settings Methods
// ---------------------------------------------------

// Every m_rc_poll_period seconds, this method polls the
// m_rc_filename to see if it exists and to see if it has been
// recently modified.  If so, we reload the log ruleset from the file.
void Settings::reload_config() {

#if XENON_ENABLE_CONFIG_FILE

  // We CANNOT use the xenon log infrastructure here, because it will
  // call reload_config and deadlock!

  boost::xtime xt;
  boost::xtime_get(&xt, boost::TIME_UTC);
  bool needs_reloading = false;

  // Every five seconds, we attempt to open the log config file to see
  // if there have been any changes.  The mutex locking for querying
  // the time is handled separately from reading the file so that only
  // one thread takes the performance hit of reading the rc file
  // during any given reload.
  {
    RecursiveMutex::Lock time_lock(m_rc_time_mutex);
    if (xt.sec - m_rc_last_polltime > m_rc_poll_period) {
      m_rc_last_polltime = xt.sec;
      needs_reloading = true;
    }
  }

  if (needs_reloading) {
    RecursiveMutex::Lock lock(m_rc_file_mutex);

    // Check to see if the file has changed.  If so, re-read the settings.
    struct_stat statbuf;
    if (stat(m_rc_filename.c_str(), &statbuf) != 0)
      return;
#ifdef __APPLE__
    time_t mtime = statbuf.st_mtimespec.tv_sec;
#else // Linux / Windows
    time_t mtime = statbuf.st_mtime;
#endif
    if (mtime > m_rc_last_modification) {
      m_rc_last_modification = mtime;

      // if it throws, let it bubble up.
      parse_config_file(m_rc_filename.c_str(), *this);
    }
  }
#endif
}

void Settings::set_rc_filename(std::string filename, bool parse_now) {

  // limit the scope of the lock
  {
    // we grab both locks in the same order that reload_config does.
    RecursiveMutex::Lock time_lock(m_rc_time_mutex);
    RecursiveMutex::Lock file_lock(m_rc_file_mutex);

    if (filename.empty()) {
      // Set the last poll time and the last modification time to the death
      // of the universe.  This is a little bit of a hack, but it lets us
      // disable the config file without making the locking in
      // reload_config() even more complex.
      m_rc_last_polltime     = std::numeric_limits<long>::max();
      m_rc_last_modification = std::numeric_limits<long>::max();
    } else if (filename != m_rc_filename) {
      m_rc_last_polltime = 0;
      m_rc_last_modification = 0;
    }
    m_rc_filename = filename;
  }

  // Okay, we might have changed the filename. Call reload_config() in order to
  // re-read it. It will grab time_lock and file_lock, so we need to make sure
  // we've released them before we re-read it (or we deadlock).
  if (parse_now)
    reload_config();
}

void Settings::set_rc_poll_period(float period) {

  // limit the scope of the lock
  {
    // we only need the time lock here
    RecursiveMutex::Lock time_lock(m_rc_time_mutex);
    m_rc_poll_period = period;
    m_rc_last_polltime = 0;
  }
  reload_config();
}

namespace {
  std::string default_tmp_dir() {
    const char *dir;
    if ((dir = ::getenv("TMPDIR"))) // unix standard
      return dir;
    else if ((dir = ::getenv("TEMP"))) // windows standard
      return dir;
    else
      return "/tmp";
  }

  std::string default_xenonrc() {
    const char *dir;
    if ((dir = ::getenv("HOME"))) // This is hopefully set on windows and linux
      return std::string(dir) + "/.xenonrc";

#ifdef XENON_HAVE_GETPWUID
    // Not set? Try a last ditch effort...
    struct passwd *pw;
    pw = getpwuid( getuid() );
    if (pw && pw->pw_dir && strlen(pw->pw_dir))
      return std::string(pw->pw_dir) + "/.xenonrc";
#endif
    // Out of options, need to disable config.
    return std::string();
  }
}

#define _XENON_SET1(Name, Default)\
  m_ ## Name(Default), m_ ## Name ## _override(false)

Settings::Settings()
  : _XENON_SET1(default_num_threads, XENON_NUM_THREADS),
    _XENON_SET1(system_cache_size, 768 * 1024 * 1024),
    _XENON_SET1(write_pool_size, 21), // 21 threads is about 252MB of back data for RGB f32 1024x1024 blocks
    _XENON_SET1(default_tile_size, 256),
    _XENON_SET1(tmp_directory, default_tmp_dir()),
    m_rc_poll_period(5.0f)
{
  set_rc_filename(default_xenonrc(), false);
}

#define GETSET(Name, Type, Callback)           \
  Type Settings::Name() {                      \
    if (!m_ ## Name ## _override)              \
      reload_config();                         \
    RecursiveMutex::Lock lock(m_settings_mutex);        \
    return m_ ## Name;                         \
  }                                            \
  void Settings::set_ ## Name(const Type& x) { \
    RecursiveMutex::Lock lock(m_settings_mutex);        \
    m_ ## Name ## _override = true;            \
    m_ ## Name = x;                            \
    Callback                                   \
  }

GETSET(default_num_threads, uint32, ;);
GETSET(system_cache_size, size_t, xenon_system_cache().resize(x););
GETSET(write_pool_size, uint32, ;);
GETSET(default_tile_size, uint32, ;);
GETSET(tmp_directory, std::string, ;);

} // namespace xenon
