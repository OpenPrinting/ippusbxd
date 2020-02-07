/* Copyright (C) 2014 Daniel Dressler and contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License. */

#define _GNU_SOURCE

#include "ippusbxd.h"

#include <errno.h>
#include <getopt.h>
#include <libusb.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dnssd.h"
#include "http.h"
#include "logging.h"
#include "options.h"
#include "tcp.h"
#include "usb.h"

/* Global variables */
static pthread_mutex_t thread_register_mutex;
static struct service_thread_param **service_threads = NULL;
static uint32_t num_service_threads = 0;

static void sigterm_handler(int sig)
{
  /* Flag that we should stop and return... */
  g_options.terminate = 1;
  NOTE("Caught signal %d, shutting down ...", sig);
}

static void list_service_threads(
    uint32_t num_service_threads,
    struct service_thread_param **service_threads)
{
  uint32_t i;
  char *p;
  char buf[10240];
  size_t written = 0;

  /* Set all bytes in |buf| to the null terminator so that we don't have to
     worry about it later. */
  memset(buf, '\0', sizeof(buf));

  written += snprintf(buf, sizeof(buf), "Threads currently running: ");
  p = buf + written;

  if (num_service_threads == 0) {
    written += snprintf(p, sizeof(buf) - written, "None");
  } else {
    for (i = 0; i < num_service_threads; i++) {
      written += snprintf(p, sizeof(buf) - written, "#%u",
                          service_threads[i]->thread_num);
      p = buf + written;
      if (i + 1 < num_service_threads) {
        written += snprintf(p, sizeof(buf) - written, ", ");
        p = buf + written;
      }
    }
  }
  NOTE("%s", buf);
}

static int register_service_thread(
    uint32_t *num_service_threads,
    struct service_thread_param ***service_threads,
    struct service_thread_param *new_thread)
{
  NOTE("Registering thread #%u", new_thread->thread_num);
  (*num_service_threads)++;
  *service_threads =
      realloc(*service_threads, *num_service_threads * sizeof(void *));
  if (*service_threads == NULL) {
    ERR("Registering thread #%u: Failed to alloc space for thread registration "
        "list",
        new_thread->thread_num);
    return -1;
  }
  (*service_threads)[*num_service_threads - 1] = new_thread;
  return 0;
}

static int unregister_service_thread(
    uint32_t *num_service_threads,
    struct service_thread_param ***service_threads, uint32_t thread_num)
{
  uint32_t i;

  NOTE("Unregistering thread #%u", thread_num);
  /* Search |service_threads| for an element with a matching thread number. */
  for (i = 0; i < *num_service_threads; i++) {
    if ((*service_threads)[i]->thread_num == thread_num) break;
  }

  if (i >= *num_service_threads) {
    ERR("Unregistering thread #%u: Cannot unregister, not found", thread_num);
    return -1;
  }

  (*num_service_threads)--;
  struct service_thread_param *removed_thread = (*service_threads)[i];
  /* Shift the contents after |removed_thread| down. */
  for (; i < *num_service_threads; i++) {
    (*service_threads)[i] = (*service_threads)[i + 1];
  }
  free(removed_thread);

  *service_threads =
      realloc(*service_threads, *num_service_threads * sizeof(void *));

  if (*num_service_threads == 0) {
    *service_threads = NULL;
  } else if (*service_threads == NULL) {
    ERR("Unregistering thread #%u: Failed to alloc space for thread "
        "registration list",
        thread_num);
    return -1;
  }

  return 0;
}

static void
cleanup_handler(void *arg_void)
{
  uint32_t thread_num = *((int *)(arg_void));
  NOTE("Thread #%u: Called clean-up handler", thread_num);
  pthread_mutex_lock(&thread_register_mutex);
  unregister_service_thread(&num_service_threads, &service_threads, thread_num);
  list_service_threads(num_service_threads, service_threads);
  pthread_mutex_unlock(&thread_register_mutex);
}

static void read_transfer_callback(struct libusb_transfer *transfer)
{
  struct libusb_callback_data *user_data =
      (struct libusb_callback_data *)transfer->user_data;

  uint32_t thread_num = user_data->thread_num;
  pthread_mutex_t *read_inflight_mutex = user_data->read_inflight_mutex;
  pthread_cond_t *read_inflight_cond = user_data->read_inflight_cond;

  switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:
      user_data->pkt->filled_size = transfer->actual_length;

      if (transfer->actual_length) {
        NOTE("Thread #%u: Pkt from %s (buffer size: %zu)\n===\n%s===",
             thread_num, "usb", user_data->pkt->filled_size,
             hexdump(user_data->pkt->buffer, (int)user_data->pkt->filled_size));
        tcp_packet_send(user_data->tcp, user_data->pkt);
        /* Mark the tcp socket as active. */
        set_is_active(user_data->tcp, 1);
      } else {
        /* Set that we received an empty response from the printer. */
        *user_data->empty_response = 1;
      }

      break;
    case LIBUSB_TRANSFER_ERROR:
      ERR("Thread #%u: There was an error completing the transfer", thread_num);
      g_options.terminate = 1;
      break;
    case LIBUSB_TRANSFER_TIMED_OUT:
      NOTE(
          "Thread #%u: The transfer timed out before it could be completed: "
          "Received %u bytes",
          thread_num, transfer->actual_length);
      break;
    case LIBUSB_TRANSFER_CANCELLED:
      NOTE("Thread #%u: The transfer was cancelled", thread_num);
      break;
    case LIBUSB_TRANSFER_STALL:
      ERR("Thread #%u: The transfer has stalled", thread_num);
      g_options.terminate = 1;
      break;
    case LIBUSB_TRANSFER_NO_DEVICE:
      ERR("Thread #%u: The printer was disconnected during the transfer",
          thread_num);
      g_options.terminate = 1;
      break;
    case LIBUSB_TRANSFER_OVERFLOW:
      ERR("Thread #%u: The printer sent more data than was requested",
          thread_num);
      g_options.terminate = 1;
      break;
    default:
      ERR("Thread #%u: Something unexpected happened", thread_num);
      g_options.terminate = 1;
  }

  /* Free the packet used for the transfer. */
  packet_free(user_data->pkt);

  /* Mark the transfer as completed. */
  pthread_mutex_lock(read_inflight_mutex);
  *user_data->read_inflight = 0;
  pthread_cond_broadcast(read_inflight_cond);
  pthread_mutex_unlock(read_inflight_mutex);

  /* Cleanup the data used for the transfer */
  free(user_data);
  libusb_free_transfer(transfer);
}

void *service_connection(void *params_void)
{
  struct service_thread_param *params =
      (struct service_thread_param *)params_void;
  uint32_t thread_num = params->thread_num;

  /* Detach this thread so that the main thread does not need to join this
     thread after termination for clean-up. */
  pthread_detach(pthread_self());

  /* Register clean-up handler. */
  pthread_cleanup_push(cleanup_handler, &thread_num);

  /* Allow immediate cancelling of this thread. */
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

  /* Attempt to establish a connection with the printer. */
  if (setup_usb_connection(params->usb_sock, params))
    goto cleanup;

  /* Condition variable used to broadcast updates to the printer thread. */
  pthread_cond_t cond;
  if (pthread_cond_init(&cond, NULL))
    goto cleanup;
  params->cond = &cond;

  /* Copy the contents of |params| into |printer_params|. The only
     differences between the two are the |thread_num| and |thread_handle|. */
  struct service_thread_param *printer_params =
      calloc(1, sizeof(*printer_params));
  memcpy(printer_params, params, sizeof(*printer_params));
  printer_params->thread_num += 1;

  /* Attempt to start the printer's end of the communication. */
  if (setup_communication_thread(&service_printer_connection, printer_params))
    goto cleanup;

  pthread_t printer_params_thread_handle = printer_params->thread_handle;

  /* This function will run until the socket has been closed. When this function
     returns it means that the communication has been completed. */
  service_socket_connection(params);

  /* Notify the printer's end that the socket has closed so that it does not
     have to wait for any pending asynchronous transfers to complete. */
  pthread_cond_broadcast(params->cond);
  
  /* Wait for the printer thread to exit. */
  NOTE("Thread #%u: Waiting for thread #%u to complete", thread_num,
       thread_num + 1);
  if (pthread_join(printer_params_thread_handle, NULL))
    ERR("Thread #%u: Something went wrong trying to join the printer thread",
        thread_num);

cleanup:
  if (params->usb_conn != NULL) {
    NOTE("Thread #%u: interface #%u: releasing usb conn", thread_num,
         params->usb_conn->interface_index);
    usb_conn_release(params->usb_conn);
    params->usb_conn = NULL;
  }

  NOTE("Thread #%u: closing, %s", thread_num,
       g_options.terminate ? "shutdown requested"
                           : "communication thread terminated");
  tcp_conn_close(params->tcp);

  /* Execute clean-up handler. */
  pthread_cleanup_pop(1);
  pthread_exit(NULL);
}

void service_socket_connection(struct service_thread_param *params)
{
  uint32_t thread_num = params->thread_num;

  while (is_socket_open(params) && !g_options.terminate) {
    int result = poll_tcp_socket(params->tcp);
    if (result < 0 || !is_socket_open(params)) {
      NOTE("Thread #%u: Client closed connection", thread_num);
      return;
    } else if (result == 0) {
      continue;
    }

    struct http_packet_t *pkt = tcp_packet_get(params->tcp);
    if (pkt == NULL) {
      NOTE("Thread #%u: There was an error reading from the socket",
           thread_num);
      return;
    }

    if (!is_socket_open(params)) {
      NOTE("Thread #%u: Client closed connection", thread_num);
      return;
    }

    NOTE("Thread #%u: Pkt from tcp (buffer size: %zu)\n===\n%s===", thread_num,
         pkt->filled_size, hexdump(pkt->buffer, (int)pkt->filled_size));

    /* Send pkt to printer. */
    usb_conn_packet_send(params->usb_conn, pkt);

    packet_free(pkt);
  }
}

void *service_printer_connection(void *params_void)
{
  struct service_thread_param *params =
      (struct service_thread_param *)params_void;
  uint32_t thread_num = params->thread_num;

  /* Register clean-up handler. */
  pthread_cleanup_push(cleanup_handler, &thread_num);

  /* Amount of time to wait in milliseconds before sending another read request
     if we received a 0-byte response from the printer. */
  int backoff = initial_backoff;

  int read_inflight = 0;
  int empty_response = 0;

  pthread_mutex_t read_inflight_mutex;
  if (pthread_mutex_init(&read_inflight_mutex, NULL))
    goto cleanup;

  struct libusb_transfer *read_transfer = NULL;

  while (is_socket_open(params) && !g_options.terminate) {
    /* If there is already a read from the printer underway, block until it has
       completed. */
    pthread_mutex_lock(&read_inflight_mutex);
    while (is_socket_open(params) && read_inflight)
      pthread_cond_wait(params->cond, &read_inflight_mutex);
    pthread_mutex_unlock(&read_inflight_mutex);

    /* After waking up due to a completed transfer, verify that the socket is
       still open and that the termination flag has not been set before
       attempting to start another transfer. */
    if (!is_socket_open(params) || g_options.terminate)
      break;

    /* If we received an empty response from the printer then wait for |backoff|
       milliseconds and update the backoff period. */
    if (empty_response) {
      /* usleep accepts microseconds. */
      usleep(backoff * 1000);
      backoff = update_backoff(backoff);
      /* Reset the empty response indicator before sending the next read
         request. A mutex should not be needed here since the transfer callback
         won't be fired until after calling libusb_submit_transfer(). */
      empty_response = 0;
    } else {
      /* If we received a non-empty response from the printer then reset the
         backoff to its initial value. */
      backoff = initial_backoff;
    }

    NOTE("Thread #%u: No read in flight, starting a new one", thread_num);
    struct http_packet_t *pkt = packet_new();
    if (pkt == NULL) {
      ERR("Thread #%u: Failed to allocate packet", thread_num);
      break;
    }

    struct libusb_callback_data *user_data = setup_libusb_callback_data(
        pkt, &read_inflight, &empty_response, params, &read_inflight_mutex);

    if (user_data == NULL) {
      ERR("Thread #%u: Failed to allocate memory for libusb_callback_data",
          thread_num);
      break;
    }

    read_transfer = setup_async_read(
        params->usb_conn, pkt, read_transfer_callback, (void *)user_data, 5000);

    if (read_transfer == NULL) {
      ERR("Thread #%u: Failed to allocate memory for libusb transfer",
          thread_num);
      break;
    }

    /* Mark that there is a new read in flight. A mutex should not be needed
       here since the transfer callback won't be fired until after calling
       libusb_submit_transfer() */
    read_inflight = 1;

    if (libusb_submit_transfer(read_transfer)) {
      ERR("Thread #%u: Failed to submit asynchronous USB transfer", thread_num);
      set_read_inflight(0, &read_inflight_mutex, &read_inflight);
      break;
    }
  }

  /* If the socket used for communication has closed and there is still a
     transfer from the printer in flight then we attempt to cancel it. */
  if (get_read_inflight(&read_inflight, &read_inflight_mutex)) {
    NOTE(
        "Thread #%u: There was a read in flight when the connection was "
        "closed, cancelling transfer", thread_num);
    int cancel_status = libusb_cancel_transfer(read_transfer);
    if (!cancel_status) {
      /* Wait until the cancellation has completed. */
      NOTE("Thread #%u: Waiting until the transfer has been cancelled",
           thread_num);
      pthread_mutex_lock(&read_inflight_mutex);
      while (read_inflight)
        pthread_cond_wait(params->cond, &read_inflight_mutex);
      pthread_mutex_unlock(&read_inflight_mutex);
    } else if (cancel_status == LIBUSB_ERROR_NOT_FOUND) {
      NOTE("Thread #%u: The transfer has already completed", thread_num);
    } else {
      NOTE("Thread #%u: Failed to cancel transfer");
      g_options.terminate = 1;
    }
  }

  pthread_mutex_destroy(&read_inflight_mutex);

cleanup:
  /* Execute clean-up handler. */
  pthread_cleanup_pop(1);
  pthread_exit(NULL);
}

static uint16_t open_tcp_socket(void)
{
  uint16_t desired_port = g_options.desired_port;
  g_options.tcp_socket = NULL;
  g_options.tcp6_socket = NULL;

  for (;;) {
    g_options.tcp_socket = tcp_open(desired_port, g_options.interface);
    g_options.tcp6_socket = tcp6_open(desired_port, g_options.interface);
    if (g_options.tcp_socket || g_options.tcp6_socket ||
        g_options.only_desired_port)
      break;
    /* Search for a free port. */
    desired_port ++;
    /* We failed with 0 as port number or we reached the max port number. */
    if (desired_port == 1 || desired_port == 0)
      /* IANA recommendation of 49152 to 65535 for ephemeral ports. */
      desired_port = 49152;
    NOTE("Access to desired port failed, trying alternative port %d",
         desired_port);
  }

  return desired_port;
}

int allocate_socket_connection(struct service_thread_param *param)
{
  param->tcp = calloc(1, sizeof(*param->tcp));

  if (param->tcp == NULL) {
    ERR("Preparing thread #%u: Failed to allocate space for cups connection",
        param->thread_num);
    return -1;
  }

  return 0;
}

int setup_socket_connection(struct service_thread_param *param)
{
  param->tcp = tcp_conn_select(g_options.tcp_socket, g_options.tcp6_socket);
  if (g_options.terminate || param->tcp == NULL)
    return -1;
  return 0;
}

int setup_usb_connection(struct usb_sock_t *usb_sock,
                         struct service_thread_param *param)
{
  param->usb_conn = usb_conn_acquire(usb_sock);
  if (param->usb_conn == NULL) {
    ERR("Thread #%u: Failed to acquire usb interface", param->thread_num);
    return -1;
  }

  return 0;
}

int setup_communication_thread(void *(*routine)(void *),
                               struct service_thread_param *param)
{
  pthread_mutex_lock(&thread_register_mutex);
  register_service_thread(&num_service_threads, &service_threads, param);
  list_service_threads(num_service_threads, service_threads);
  pthread_mutex_unlock(&thread_register_mutex);

  int status =
      pthread_create(&param->thread_handle, NULL, routine, param);

  if (status) {
    ERR("Creating thread #%u: Failed to spawn thread, error %d",
        param->thread_num, status);
    pthread_mutex_lock(&thread_register_mutex);
    unregister_service_thread(&num_service_threads, &service_threads,
                              param->thread_num);
    list_service_threads(num_service_threads, service_threads);
    pthread_mutex_unlock(&thread_register_mutex);
    return -1;
  }

  return 0;
}

struct libusb_callback_data *setup_libusb_callback_data(
    struct http_packet_t *pkt, int *read_inflight, int *empty_response,
    struct service_thread_param *thread_param,
    pthread_mutex_t *read_inflight_mutex)
{
  struct libusb_callback_data *data = calloc(1, sizeof(*data));
  if (data == NULL)
    return NULL;

  data->pkt = pkt;
  data->read_inflight = read_inflight;
  data->empty_response = empty_response;
  data->thread_num = thread_param->thread_num;
  data->read_inflight_mutex = read_inflight_mutex;
  data->read_inflight_cond = thread_param->cond;
  data->tcp = thread_param->tcp;

  return data;
}

int get_read_inflight(const int *read_inflight, pthread_mutex_t *mtx)
{
  pthread_mutex_lock(mtx);
  int val = *read_inflight;
  pthread_mutex_unlock(mtx);

  return val;
}

void set_read_inflight(int val, pthread_mutex_t *mtx, int *read_inflight)
{
  pthread_mutex_lock(mtx);
  *read_inflight = val;
  pthread_mutex_unlock(mtx);
}

int is_socket_open(const struct service_thread_param *param) {
  return !param->tcp->is_closed;
}

int update_backoff(int backoff) {
  int updated = backoff * 2;
  /* Cap the maximum backoff time at 1 second. */
  if (updated > maximum_backoff) {
    updated = maximum_backoff;
  }
  return updated;
}

static void start_daemon()
{
  /* Capture USB device. */
  struct usb_sock_t *usb_sock;

  /* Termination flag */
  g_options.terminate = 0;

  usb_sock = usb_open();
  if (usb_sock == NULL) goto cleanup_usb;

  /* Capture a socket */
  uint16_t desired_port = open_tcp_socket();
  if (g_options.tcp_socket == NULL && g_options.tcp6_socket == NULL)
    goto cleanup_tcp;

  if (g_options.tcp_socket)
    g_options.real_port = tcp_port_number_get(g_options.tcp_socket);
  else
    g_options.real_port = tcp_port_number_get(g_options.tcp6_socket);
  if (desired_port != 0 && g_options.only_desired_port == 1 &&
      desired_port != g_options.real_port) {
    ERR("Received port number did not match requested port number."
	" The requested port number may be too high.");
    goto cleanup_tcp;
  }
  printf("%u|", g_options.real_port);
  fflush(stdout);

  NOTE("Port: %d, IPv4 %savailable, IPv6 %savailable", g_options.real_port,
       g_options.tcp_socket ? "" : "not ", g_options.tcp6_socket ? "" : "not ");

  /* Lose connection to caller */
  uint16_t pid;
  if (!g_options.nofork_mode && (pid = fork()) > 0) {
    printf("%u|", pid);
    exit(0);
  }

  /* Redirect SIGINT and SIGTERM so that we do a proper shutdown, unregistering
     the printer from DNS-SD */
#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
  sigset(SIGTERM, sigterm_handler);
  sigset(SIGINT, sigterm_handler);
  NOTE("Using signal handler SIGSET");
#elif defined(HAVE_SIGACTION)
  struct sigaction action; /* Actions for POSIX signals */
  memset(&action, 0, sizeof(action));
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGTERM);
  action.sa_handler = sigterm_handler;
  sigaction(SIGTERM, &action, NULL);
  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGINT);
  action.sa_handler = sigterm_handler;
  sigaction(SIGINT, &action, NULL);
  NOTE("Using signal handler SIGACTION");
#else
  signal(SIGTERM, sigterm_handler);
  signal(SIGINT, sigterm_handler);
  NOTE("Using signal handler SIGNAL");
#endif /* HAVE_SIGSET */

  /* Register for unplug event */
  if (usb_can_callback(usb_sock))
    usb_register_callback(usb_sock);

  /* DNS-SD-broadcast the printer on the local machine so
     that cups-browsed and ippfind will discover it */
  if (g_options.nobroadcast == 0) {
    if (dnssd_init() == -1)
      goto cleanup_tcp;
  }

  /* Main loop */
  uint32_t i = 1;
  pthread_mutex_init(&thread_register_mutex, NULL);
  while (!g_options.terminate) {
    struct service_thread_param *args = calloc(1, sizeof(*args));
    if (args == NULL) {
      ERR("Preparing thread #%u: Failed to alloc space for thread args", i);
      goto cleanup_thread;
    }

    args->thread_num = i;
    args->usb_sock = usb_sock;

    /* Allocate space for a tcp socket to be used for communication. */
    if (allocate_socket_connection(args))
      goto cleanup_thread;

    /* Attempt to establish a connection to the relevant socket. */
    if (setup_socket_connection(args))
      goto cleanup_thread;

    /* Attempt to start up a new thread to handle the socket's end of
       communication. */
    if (setup_communication_thread(&service_connection, args))
      goto cleanup_thread;

    i += 2;

    continue;

  cleanup_thread:
    if (args != NULL) {
      if (args->tcp != NULL)
	tcp_conn_close(args->tcp);
      free(args);
    }
    break;
  }

 cleanup_tcp:
  /* Stop DNS-SD advertising of the printer */
  if (g_options.dnssd_data != NULL)
    dnssd_shutdown();

  /* Cancel communication threads which did not terminate by themselves when
     stopping ippusbxd, so that no USB communication with the printer can
     happen after the final reset */
  while (num_service_threads) {
    NOTE("Thread #%u did not terminate, canceling it now ...",
	 service_threads[0]->thread_num);
    i = num_service_threads;
    pthread_cancel(service_threads[0]->thread_handle);
    while (i == num_service_threads)
      usleep(1000000);
  }

  /* Wait for USB unplug event observer thread to terminate */
  NOTE("Shutting down usb observer thread");
  pthread_join(g_options.usb_event_thread_handle, NULL);

  /* TCP clean-up */
  if (g_options.tcp_socket!= NULL)
    tcp_close(g_options.tcp_socket);
  if (g_options.tcp6_socket!= NULL)
    tcp_close(g_options.tcp6_socket);

 cleanup_usb:
  /* USB clean-up and final reset of the printer */
  if (usb_sock != NULL)
    usb_close(usb_sock);
  return;
}

static uint16_t strto16hex(const char *str)
{
  unsigned long val = strtoul(str, NULL, 16);
  if (val > UINT16_MAX)
    exit(1);
  return (uint16_t)val;
}

static uint16_t strto16dec(const char *str)
{
  unsigned long val = strtoul(str, NULL, 10);
  if (val > UINT16_MAX)
    exit(1);
  return (uint16_t)val;
}

int main(int argc, char *argv[])
{
  int c;
  int option_index = 0;
  static struct option long_options[] = {
    {"vid",          required_argument, 0,  'v' },
    {"pid",          required_argument, 0,  'm' },
    {"serial",       required_argument, 0,  's' },
    {"bus",          required_argument, 0,  'b' },
    {"device",       required_argument, 0,  'D' },
    {"bus-device",   required_argument, 0,  'X' },
    {"from-port",    required_argument, 0,  'P' },
    {"only-port",    required_argument, 0,  'p' },
    {"interface",    required_argument, 0,  'i' },
    {"logging",      no_argument,       0,  'l' },
    {"debug",        no_argument,       0,  'd' },
    {"verbose",      no_argument,       0,  'q' },
    {"no-fork",      no_argument,       0,  'n' },
    {"no-broadcast", no_argument,       0,  'B' },
    {"help",         no_argument,       0,  'h' },
    {NULL,           0,                 0,  0   }
  };
  g_options.log_destination = LOGGING_STDERR;
  g_options.only_desired_port = 0;
  g_options.desired_port = 60000;
  g_options.interface = "lo";
  g_options.serial_num = NULL;
  g_options.vendor_id = 0;
  g_options.product_id = 0;
  g_options.bus = 0;
  g_options.device = 0;

  while ((c = getopt_long(argc, argv, "qnhdp:P:i:s:lv:m:B",
			  long_options, &option_index)) != -1) {
    switch (c) {
    case '?':
    case 'h':
      g_options.help_mode = 1;
      break;
    case 'p':
    case 'P':
      {
	long long port = 0;
	/* Request specific port */
	port = atoi(optarg);
	if (port < 0) {
	  ERR("Port number must be non-negative");
	  return 1;
	}
	if (port > UINT16_MAX) {
	  ERR("Port number must be %u or less, "
	      "but not negative", UINT16_MAX);
	  return 2;
	}
	g_options.desired_port = (uint16_t)port;
	if (c == 'p')
	  g_options.only_desired_port = 1;
	else
	  g_options.only_desired_port = 0;
	break;
      }
    case 'i':
      /* Request a specific network interface */
      g_options.interface = strdup(optarg);
      break;
    case 'l':
      g_options.log_destination = LOGGING_SYSLOG;
      break;
    case 'd':
      g_options.nofork_mode = 1;
      g_options.verbose_mode = 1;
      break;
    case 'q':
      g_options.verbose_mode = 1;
      break;
    case 'n':
      g_options.nofork_mode = 1;
      break;
    case 'v':
      g_options.vendor_id = strto16hex(optarg);
      break;
    case 'm':
      g_options.product_id = strto16hex(optarg);
      break;
    case 'b':
      g_options.bus = strto16dec(optarg);
      break;
    case 'D':
      g_options.device = strto16dec(optarg);
      break;
    case 'X':
      {
	char *p = strchr(optarg, ':');
	if (p == NULL) {
	  ERR("Bus and device must be given in the format <bus>:<device>");
	  return 3;
	}
	p ++;
	g_options.bus = strto16dec(optarg);
	g_options.device = strto16dec(p);
	break;
      }
    case 's':
      g_options.serial_num = (unsigned char *)optarg;
      break;
    case 'B':
      g_options.nobroadcast = 1;
      break;
    }
  }

  if (g_options.help_mode) {
    printf("Usage: %s -v <vendorid> -m <productid> -s <serial> -P <port>\n"
	   "       %s --bus <bus> --device <device> -P <port>\n"
	   "       %s -h\n"
	   "Options:\n"
	   "  --help\n"
	   "  -h           Show this help message\n"
	   "  --vid <vid>\n"
	   "  -v <vid>     Vendor ID of desired printer (as hexadecimal number)\n"
	   "  --pid <pid>\n"
	   "  -m <pid>     Product ID of desired printer (as hexadecimal number)\n"
	   "  --serial <serial>\n"
	   "  -s <serial>  Serial number of desired printer\n"
	   "  --bus <bus>\n"
	   "  --device <device>\n"
	   "  --bus-device <bus>:<device>\n"
	   "               USB bus and device numbers where the device is currently\n"
	   "               connected (see output of lsusb). Note that these numbers change\n"
	   "               when the device is disconnected and reconnected. This method of\n"
	   "               calling ippusbxd is only for calling via UDEV. <bus> and\n"
	   "               <device> have to be given in decimal numbers.\n"
	   "  --only-port <portnum>\n"
	   "  -p <portnum> Port number to bind against, error out if port already taken\n"
	   "  --from-port <portnum>\n"
	   "  -P <portnum> Port number to bind against, use another port if port already\n"
	   "               taken\n"
	   "  --interface <interface>\n"
	   "  -i <interface> Network interface to use. Default is the loopback interface\n"
	   "               (lo, localhost).\n"
	   "  --logging\n"
	   "  -l           Redirect logging to syslog\n"
	   "  --verbose\n"
	   "  -q           Enable verbose tracing\n"
	   "  --debug\n"
	   "  -d           Debug mode for verbose output and no fork\n"
	   "  --no-fork\n"
	   "  -n           No-fork mode\n"
	   "  --no-broadcast\n"
	   "  -B           No-broadcast mode, do not DNS-SD-broadcast\n"
	   , argv[0], argv[0], argv[0]);
    return 0;
  }
  start_daemon();
  NOTE("ippusbxd completed successfully");
  return 0;
}
