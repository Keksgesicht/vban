//
// Created by adrian on 03.06.21.
//

#ifndef VBAN_PIPEWIRE_BACKEND_H
#define VBAN_PIPEWIRE_BACKEND_H

#include "audio_backend.h"

#define PIPEWIRE_BACKEND_NAME   "pipewire"
int pipewire_backend_init(audio_backend_handle_t* handle);

#endif //VBAN_PIPEWIRE_BACKEND_H
