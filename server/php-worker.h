// Compiler for PHP (aka KPHP)
// Copyright (c) 2020 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#pragma once

#include "server/php-query-data.h"
#include "server/php-runner.h"

enum php_worker_mode_t {
  http_worker,
  rpc_worker,
  once_worker
};

enum php_worker_state_t {
  phpq_try_start,
  phpq_init_script,
  phpq_run,
  phpq_free_script,
  phpq_finish
};

struct php_worker {
  struct connection *conn;

  php_query_data *data;

  bool paused;
  bool terminate_flag;
  script_error_t terminate_reason;
  const char *error_message;

  //for wait query
  int waiting;
  int wakeup_flag;
  double wakeup_time;

  double init_time;
  double start_time;
  double finish_time;

  php_worker_state_t state;
  php_worker_mode_t mode;

  long long req_id;
  int target_fd;
};

