#ifndef __CRITICAL_OP_H__
#define __CRITICAL_OP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "util.h"

typedef void (*SbcCriticalOpCb)(bool reply_received);
void sbc_critical_op(bool critical, SbcCriticalOpCb cb);

#ifdef __cplusplus
}
#endif

#endif
