/*
   Copyright (c) 2003, 2025, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef MGMAPI_SERVICE_HPP
#define MGMAPI_SERVICE_HPP

#include "NdbSleep.h"
#include "Parser.hpp"
#include "util/InputStream.hpp"
#include "util/NdbSocket.h"
#include "util/OutputStream.hpp"
#include "util/SocketServer.hpp"

#include "MgmtSrvr.hpp"

class MgmApiSession : public SocketServer::Session {
  static void list_session(SocketServer::Session *_s, void *data);
  static void get_session(SocketServer::Session *_s, void *data);
  static void show_cert(SocketServer::Session *_s, void *data);

 private:
  typedef Parser<MgmApiSession> Parser_t;

  NdbSocket m_secure_socket;
  class MgmtSrvr &m_mgmsrv;
  InputStream *m_input;
  OutputStream *m_output;
  Parser_t *m_parser;
  char m_err_str[1024];
  int m_stopSelf;  // -1 is restart, 0 do nothing, 1 stop
  NdbMutex *m_mutex;

  unsigned int m_sessionAuthLevel{0};

  // for listing sessions and other fun:
  Parser_t::Context *m_ctx;
  Uint64 m_session_id;
  struct x509_st *m_cert{nullptr};

  int m_errorInsert;

  BaseString m_name;
  const char *name() { return m_name.c_str(); }

  const char *get_error_text(int err_no) {
    return m_mgmsrv.getErrorText(err_no, m_err_str, sizeof(m_err_str));
  }

  /* Client version info, m_vMajor != 0 if known */
  unsigned int m_vMajor;
  unsigned int m_vMinor;
  unsigned int m_vBuild;

 public:
  MgmApiSession(class MgmtSrvr &mgm, NdbSocket &&sock, Uint64 session_id);
  ~MgmApiSession() override;
  void runSession() override;

  static const unsigned SOCKET_TIMEOUT = 30000;

  int checkAuth(struct CmdAuth *) const;
  void reportAuthFailure(int code);
  int on_verify(int, struct x509_store_ctx_st *);

  void getConfig(Parser_t::Context &ctx, const class Properties &args, bool v2);
  void getConfig_v1(Parser_t::Context &ctx, const class Properties &args);
  void getConfig_v2(Parser_t::Context &ctx, const class Properties &args);
  void setConfig_v1(Parser_t::Context &ctx, const class Properties &args);
  void setConfig_v2(Parser_t::Context &ctx, const class Properties &args);
  void setConfig(Parser_t::Context &ctx, const class Properties &args, bool v2);
  void showConfig(Parser_t::Context &ctx, const class Properties &args);
  void reloadConfig(Parser_t::Context &ctx, const class Properties &args);

  void get_nodeid(Parser_t::Context &ctx, const class Properties &args);
  void getVersion(Parser_t::Context &ctx, const class Properties &args);
  void setClientVersion(Parser_t::Context &ctx, const class Properties &args);
  void getStatus(Parser_t::Context &ctx, const class Properties &args);
  void getInfoClusterLog(Parser_t::Context &ctx, const class Properties &args);
  void restart(const class Properties &args, int version);
  void restart_v1(Parser_t::Context &ctx, const class Properties &args);
  void restart_v2(Parser_t::Context &ctx, const class Properties &args);
  void restartAll(Parser_t::Context &ctx, const class Properties &args);
  void insertError(Parser_t::Context &ctx, const class Properties &args);
  void setTrace(Parser_t::Context &ctx, const class Properties &args);
  void logSignals(Parser_t::Context &ctx, const class Properties &args);
  void startSignalLog(Parser_t::Context &ctx, const class Properties &args);
  void stopSignalLog(Parser_t::Context &ctx, const class Properties &args);
  void dumpState(Parser_t::Context &ctx, const class Properties &args);
  void startBackup(Parser_t::Context &ctx, const class Properties &args);
  void abortBackup(Parser_t::Context &ctx, const class Properties &args);
  void enterSingleUser(Parser_t::Context &ctx, const class Properties &args);
  void exitSingleUser(Parser_t::Context &ctx, const class Properties &args);
  void stop_v1(Parser_t::Context &ctx, const class Properties &args);
  void stop_v2(Parser_t::Context &ctx, const class Properties &args);
  void stop(const class Properties &args, int version);
  void stopAll(Parser_t::Context &ctx, const class Properties &args);
  void start(Parser_t::Context &ctx, const class Properties &args);
  void startAll(Parser_t::Context &ctx, const class Properties &args);
  void startTls(Parser_t::Context &ctx, const class Properties &args);
  void bye(Parser_t::Context &ctx, const class Properties &args);
  void endSession(Parser_t::Context &ctx, const class Properties &args);
  void setLogLevel(Parser_t::Context &ctx, const class Properties &args);
  void getClusterLogLevel(Parser_t::Context &ctx, const class Properties &args);
  void setClusterLogLevel(Parser_t::Context &ctx, const class Properties &args);
  void setLogFilter(Parser_t::Context &ctx, const class Properties &args);

  void setParameter(Parser_t::Context &ctx, const class Properties &args);
  void setConnectionParameter(Parser_t::Context &ctx,
                              const class Properties &args);
  void getConnectionParameter(Parser_t::Context &ctx, Properties const &args);

  void listen_event(Parser_t::Context &ctx, const class Properties &args);

  void purge_stale_sessions(Parser_t::Context &ctx,
                            const class Properties &args);
  void check_connection(Parser_t::Context &ctx, const class Properties &args);

  void transporter_connect(Parser_t::Context &ctx, Properties const &args);

  void get_mgmd_nodeid(Parser_t::Context &ctx, Properties const &args);

  static const unsigned MAX_EVENT_LENGTH = 25;

  void report_event(Parser_t::Context &ctx, Properties const &args);

  void listSessions(Parser_t::Context &ctx, Properties const &args);
  void listCerts(Parser_t::Context &ctx, Properties const &args);
  void getTlsStats(Parser_t::Context &ctx, Properties const &args);

  void getSessionId(Parser_t::Context &ctx, Properties const &args);
  void getSession(Parser_t::Context &ctx, Properties const &args);

  void create_nodegroup(Parser_t::Context &ctx, Properties const &args);
  void drop_nodegroup(Parser_t::Context &ctx, Properties const &args);

  void show_variables(Parser_t::Context &ctx, Properties const &args);

  void dump_events(Parser_t::Context &ctx, Properties const &args);

  void set_ports(Parser_t::Context &, Properties const &args);
};

class MgmApiService : public SocketServer::Service {
  MgmtSrvr &m_mgmsrv;
  Uint64 m_next_session_id;  // Protected by m_sessions mutex it SocketServer
 public:
  MgmApiService(MgmtSrvr &mgm) : m_mgmsrv(mgm), m_next_session_id(1) {}

  SocketServer::Session *newSession(NdbSocket &&socket) override {
    return new MgmApiSession(m_mgmsrv, std::move(socket), m_next_session_id++);
  }
};

static const char *str_null(const char *str) { return (str ? str : "(null)"); }

static const char *yes_no(bool value) { return (value ? "yes" : "no"); }

#endif
