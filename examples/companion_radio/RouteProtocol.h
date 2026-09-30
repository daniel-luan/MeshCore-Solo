#pragma once
#include "RouteStore.h"
namespace routes {
// Request: F0 SR 01 op request(u16) token(u32), optional body.
// Reply:   7F SR 01 op request(u16) token(u32) error state next(u32), optional body.
inline size_t handle(RouteStore &store, bool ble, const uint8_t *in, size_t len, uint8_t *out, uint32_t now) {
  if (len < 11 || in[1] != 'S' || in[2] != 'R' || in[3] != PROTOCOL) return 0;
  uint8_t op = in[4];
  uint16_t request = u16(in + 5);
  uint32_t token = u32(in + 7);
  Error error = OK;
  size_t extra = 0;
  out[0] = RESPONSE;
  out[1] = 'S';
  out[2] = 'R';
  out[3] = PROTOCOL;
  out[4] = op;
  put16(out + 5, request);
  if (ble)
    error = USB_ONLY;
  else
    switch (op) {
    case CAPABILITIES: {
      if (len != 11) {
        error = BAD_REQUEST;
        break;
      }
      if (store.state() == CHECKING) {
        error = BUSY;
        break;
      }
      Capacity c;
      if (!store.capacity(c)) {
        error = IO_ERROR;
        break;
      }
      put16(out + 17, CHUNK_SIZE);
      put32(out + 19, c.total);
      put32(out + 23, c.used);
      put32(out + 27, c.reserve);
      put32(out + 31, c.reclaimable);
      put32(out + 35, c.maximum);
      extra = 22;
      break;
    }
    case INFO: {
      if (len != 11) {
        error = BAD_REQUEST;
        break;
      }
      if (store.hasRoute()) {
        const Header &h = store.header();
        put32(out + 17, h.bytes);
        put32(out + 21, h.points);
        put32(out + 25, h.distance);
        put32(out + 29, h.checkpoints);
        put32(out + 33, h.segments);
        char name[64]{};
        if (!store.name(name, sizeof(name))) {
          error = IO_ERROR;
          break;
        }
        size_t n = strlen(name);
        out[37] = n;
        memcpy(out + 38, name, n);
        extra = 21 + n;
      }
      break;
    }
    case BEGIN:
      error = len == 19 ? store.startUpload(u32(in + 11), u32(in + 15), request, now) : BAD_REQUEST;
      break;
    case DATA:
      error = len > 15 ? store.data(token, u32(in + 11), in + 15, len - 15, now) : BAD_REQUEST;
      break;
    case STATUS:
      if (len != 11)
        error = BAD_REQUEST;
      else if (token && token != store.session())
        error = BAD_SESSION;
      put32(out + 17, store.verifyProgress());
      out[21] = store.error();
      extra = 5;
      break;
    case COMMIT:
      error = len == 11 ? store.commit(token, now) : BAD_REQUEST;
      break;
    case ABORT:
      error = len == 11 ? store.abort(token) : BAD_REQUEST;
      break;
    case DELETE_ROUTE:
      error = len == 15 && u32(in + 11) == 0x544c4544 ? store.erase() : BAD_REQUEST;
      break; // DELT
    default:
      error = UNSUPPORTED;
      break;
    }
  put32(out + 7, store.session());
  out[11] = error;
  out[12] = store.state();
  put32(out + 13, store.received());
  return 17 + extra;
}
} // namespace routes
