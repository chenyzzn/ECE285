#ifndef ENVMAP_H
#define ENVMAP_H

#include "Ri.h"

/* A latitude-longitude environment image loaded by RiMakeTexture. */
typedef struct {
    RtInt slot;
} EnvMapParams;

#ifdef __cplusplus
extern "C" {
#endif

void ENVIRONMENT(RtShaderContext *c);

#ifdef __cplusplus
}
#endif

#endif /* ENVMAP_H */
