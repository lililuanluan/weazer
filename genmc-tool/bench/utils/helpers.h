#ifndef HELPERS_H
#define HELPERS_H

#include <stddef.h>
#include <string.h>
#include "random.h"

void* myMemset(void* ptr, int value, size_t num) {
    unsigned char* p = (unsigned char*)ptr;
    for (size_t i = 0; i < num; i++) {
        p[i] = (unsigned char)value;
    }
    return ptr;
}



#endif