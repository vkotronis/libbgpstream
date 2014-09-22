/*
 * bgpwatcher
 *
 * Alistair King, CAIDA, UC San Diego
 * corsaro-info@caida.org
 *
 * Copyright (C) 2012 The Regents of the University of California.
 *
 * This file is part of bgpwatcher.
 *
 * bgpwatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * bgpwatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with bgpwatcher.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef __BGPWATCHER_CLIENT_BROKER_H
#define __BGPWATCHER_CLIENT_BROKER_H

#include <czmq.h>
#include <stdint.h>

/** @file
 *
 * @brief Header file that exposes the private interface of the bgpwatcher
 * client broker
 *
 * @author Alistair King
 *
 */

/**
 * @name Public Opaque Data Structures
 *
 * @{ */

/** @} */

/**
 * @name Public Data Structures
 *
 * @{ */

/** State for the zactor that transparently proxies requests between the client
    and the server while managing hearbeats, reconnects etc. */
typedef struct bgpwatcher_client_broker {

  /** Identity of this client. MUST be globally unique */
  char *identity;

  /** Error status */
  bgpwatcher_err_t err;

  /** 0MQ context pointer (for broker->server comms) */
  zctx_t *ctx;

  /** URI to connect to the server on */
  char *server_uri;

  /** Socket used to connect to the server */
  void *server_socket;

  /** Time (in ms) between heartbeats sent to the server */
  uint64_t heartbeat_interval;

  /** Time (in ms) to send the next heartbeat to server */
  uint64_t heartbeat_next;

  /** The number of heartbeats that can go by before the server is declared
      dead */
  int heartbeat_liveness;

  /** The number of beats before the server is declared dead */
  int heartbeat_liveness_remaining;

  /** The minimum time (in ms) after a server disconnect before we try to
      reconnect */
  uint64_t reconnect_interval_min;

  /** The maximum time (in ms) after a server disconnect before we try to
      reconnect (after exponential back-off) */
  uint64_t reconnect_interval_max;

  /** The time before we will next attempt to reconnect */
  uint64_t reconnect_interval_next;

  /** Indicates that the client has been signaled to shutdown */
  int shutdown;

  /* OWNED BY THE BROKER */

  /** Pointer to the poller instance used by the broker */
  zpoller_t *poller;

  /** Pointer to the pipe used to talk to the master */
  zsock_t *master_pipe;

} bgpwatcher_client_broker_t;

/** @} */

/**
 * @name Public Enums
 *
 * @{ */

/** @} */

/** Main event loop of the client broker. Conforms to the zactor_fn spec.
 *
 * @param pipe          pointer to a pipe used to communicate with the client
 * @param args          pointer to a pre-populated client broker state struct
 *
 * @note all communication with the broker must be through the pipe. NO shared
 * memory is to be used.
 */
void bgpwatcher_client_broker_run(zsock_t *pipe, void *args);

#endif
