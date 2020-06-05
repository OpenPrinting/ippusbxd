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
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include <limits.h>

#include "http.h"
#include "logging.h"

#define BUFFER_STEP (1 << 15)

struct http_packet_t *packet_new()
{
  size_t const capacity = BUFFER_STEP;

  struct http_packet_t *pkt = calloc(1, sizeof(*pkt));
  if (pkt == NULL) {
    ERR("failed to alloc packet");
    return NULL;
  }

  uint8_t *buf = calloc(capacity, sizeof(*buf));
  if (buf == NULL) {
    ERR("failed to alloc space for packet's buffer or space for packet");
    free(pkt);
    return NULL;
  }

  /* Assemble packet */
  pkt->buffer = buf;
  pkt->buffer_capacity = capacity;
  pkt->filled_size = 0;

  return pkt;
}

void packet_free(struct http_packet_t *pkt)
{
  free(pkt->buffer);
  free(pkt);
}
